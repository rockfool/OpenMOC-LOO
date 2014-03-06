/*
 * Cmfd.cpp
 *
 *  Created on: September 13, 2012
 *      Author: Sam Shaner
 *		MIT, Course 22
 *              shaner@mit.edu
 */

#include "Cmfd.h"
#include <cmath>

int Cmfd::_surf_index[] = {1,2,2,3,3,0,0,1,2,1,3,2,0,3,1,0};

/**
 * Acceleration constructor
 * @param geom pointer to the geometry
 * @param track_generator pointer to the trackgenerator
 */
Cmfd::Cmfd(Geometry* geom, Plotter* plotter, Mesh* mesh, 
           TrackGenerator *track_generator, Options *opts) 
{
    _geom = geom;
    _plotter = plotter;
    _mesh = mesh;
    _quad = new Quadrature(TABUCHI);

    _num_azim = track_generator->getNumAzim();
    _spacing = track_generator->getSpacing();
    _num_tracks = track_generator->getNumTracks();

    _l2_norm = 1.0;
    _l2_norm_conv_thresh = opts->getL2NormConvThresh();
    _use_diffusion_correction = opts->getDiffusionCorrection();

    _ng = NUM_ENERGY_GROUPS;
    if (opts->getGroupStructure() == false)
        _ng = 1;
    _cw = _mesh->getCellWidth();
    _ch = _mesh->getCellHeight();

    _damp_factor = opts->getDampFactor();

    _run_cmfd = false;
    if (opts->getCmfd())
        _run_cmfd = true;

    /* since we need to run diffusion at 1st iteration, AMPhi needs to be 
     * created for every acceleration. */
    PetscInt size1, size2;
    size1 = _cw * _ch * _ng;
    size2 = 4 + _ng;
    //createAMPhi(size1, size2, _ch * _cw * _ng);
    /* FIXME: Valgrind shows bad read (uninitialized, conditional
     * jump) from _A, _M, _phi_new, _source_old */
    MatCreateSeqAIJ(PETSC_COMM_WORLD, size1, size1, size2, 
                                PETSC_NULL, 
                                &_A);
    MatCreateSeqAIJ(PETSC_COMM_WORLD, size1, size1, _ng, 
                                PETSC_NULL, 
                                &_M);
    VecCreateSeq(PETSC_COMM_WORLD, size1, &_phi_new);
    VecDuplicate(_phi_new, &_source_old);



    _run_loo = false;
    _run_loo_psi = false;
    _run_loo_phi = false;
    if (opts->getLoo())
        _run_loo = true;
    if (opts->getLoo1())
    {
        _run_loo = true;
        _run_loo_psi = true;
        _run_loo_phi = false;
    }
    else if (opts->getLoo2())
    {
        _run_loo = true;
        _run_loo_psi = false;
        _run_loo_phi = true;
    }

    _num_loop = _cw;
    _num_track = 4 * _cw; 

    _i_array = new int[_num_loop * _num_track];
    _t_array = new int[_num_loop * _num_track];
    _t_arrayb = new int[_num_loop * _num_track];

    if (_run_loo || opts->getAccAfterMOCConverge())
    {
        generateTrack(_i_array, _t_array, _t_arrayb);
        //checkTrack();
    }
    _num_iter_to_conv = 0;
    _plot_prolongation = opts->plotProlongation();
    _update_boundary = opts->getUpdateBoundary();
    _reflect_outgoing = opts->getReflectOutgoing();
    _first_diffusion = opts->getFirstDiffusion();

    if (_reflect_outgoing)
        _nq = 2;
    else
        _nq = 4;

    for (int s = 0; s < 4; s++)
    {
        _bc[s] = _mesh->getBoundary(s);

        switch (_bc[s])
        {
        case REFLECTIVE:
            log_printf(DEBUG, "mesh boundary %d reflective", s);
            break;
        case VACUUM:
            log_printf(DEBUG, "mesh boundary %d vacuum", s);
            break;
        case BOUNDARY_NONE:
            log_printf(DEBUG, "mesh boundary %d unknown", s);
        break;
        }
    }

    if ((_bc[0] == REFLECTIVE) || (_bc[1] == REFLECTIVE) || 
        (_bc[2] == REFLECTIVE) || (_bc[3] == REFLECTIVE) )
        _any_reflective = true;
    else
        _any_reflective = false;

    _converged = false;

    _cell_source = new double[_cw * _ch];
    for (int i = 0; i < _cw * _ch; i++)
        _cell_source[i] = 1.0;
}

/**
 * cmfd Destructor clears all memory
 */
Cmfd::~Cmfd() {
}

void Cmfd::runCmfd() {
    _run_cmfd = true;
    _run_loo = false;
    _run_loo_psi = false;
    _run_loo_phi = false;
}

void Cmfd::runLoo1() {
    _run_cmfd = false;
    _run_loo = true;
    _run_loo_psi = true;
    _run_loo_phi = false;
}

void Cmfd::runLoo2() {
    _run_cmfd = false;
    _run_loo = true;
    _run_loo_psi = false;
    _run_loo_phi = true;
}


/**
 * Create the loss matrix (A), fission matrix (A), and flux vector (phi)
 * @param number of columns needed in A matrix
 * @param number of rows needed in A matrix and M matrix
 * @param number of rows needed in phi vector
 */ 
int Cmfd::createAMPhi(PetscInt size1, PetscInt size2, int cells){

    int petsc_err = 0;
    //MatCreate(PETSC_COMM_WORLD,&_A);
    //MatSetType(_A, MATSEQAIJ);
    petsc_err = MatCreateSeqAIJ(PETSC_COMM_WORLD, size1, size1, size2, 
                                PETSC_NULL, 
                                &_A);
    size2 = size2 - 4;
    petsc_err = MatCreateSeqAIJ(PETSC_COMM_WORLD, size1, size1, size2, 
                                PETSC_NULL, 
                                &_M);
    petsc_err = VecCreateSeq(PETSC_COMM_WORLD, cells, &_phi_new);
    //petsc_err = VecCreateSeq(PETSC_COMM_WORLD, cells, &_source_old);
    VecDuplicate(_phi_new, &_source_old);
    CHKERRQ(petsc_err);

    return petsc_err;
}

double Cmfd::computeCellSourceNorm()
{
    /* initialize variables */
    double *old_cell_source; 
    old_cell_source = new double[_cw * _ch];
    double l2_norm;

    for (int i = 0; i < _cw * _ch; i++)
        old_cell_source[i] = _cell_source[i];
 
    computeCellSourceFromFSR();

    l2_norm = computeCellSourceNormGivenTwoSources(old_cell_source, 
                                                   _cell_source);

    return l2_norm;
}

double Cmfd::computeCellSourceNormGivenTwoSources(double *old_cell_source, 
                                                  double *new_cell_source)
{
    double l2_norm;
    double *source_residual;
    source_residual = new double[_cw * _ch];
    int counter = 0;

    for (int i = 0; i < _cw * _ch; i++)
    {
        if (new_cell_source[i] > 1e-10)
        {
            source_residual[i] = pow(new_cell_source[i] / old_cell_source[i] 
                                     - 1.0, 2);
            counter += 1;
        }
        else
            source_residual[i] = 0.0;
    }

    l2_norm = pairwise_sum<double>(source_residual, _ch * _cw);
    l2_norm /= (double) (counter);
    l2_norm = sqrt(l2_norm);

    return l2_norm;
}

void Cmfd::computeCellSourceFromFSR()
{
    double volume, flux, nu_fis;
    MeshCell* meshCell;
    FlatSourceRegion* fsr;
    std::vector<int>::iterator iter;

    for (int i = 0; i < _cw * _ch; i++)
    {
        meshCell = _mesh->getCells(i);
        _cell_source[i] = 0;
        for (int e = 0; e < NUM_ENERGY_GROUPS; e++) 
        {
            for (iter = meshCell->getFSRs()->begin(); 
                 iter != meshCell->getFSRs()->end(); ++iter)
            {
                fsr = &_flat_source_regions[*iter];
                volume = fsr->getVolume();
                flux = fsr->getFlux()[e];
                nu_fis = fsr->getMaterial()->getNuSigmaF()[e];
                _cell_source[i] += nu_fis * flux * volume;
            }
        }
        log_printf(ACTIVE, "cell %d nu_fis %f flux %f vol %f source %f", i, 
                   nu_fis, flux, volume, _cell_source[i]);
    }
    return;
}


/** Computes the cross section for all MeshCells in the Mesh 
 * Create cross sections and fluxes for each cmfd cell by
 * energy condensing and volume averaging cross sections from
 * the MOC sweep.
 * @param pointer to an array of fsrs 
 *
 */

void Cmfd::computeXS_old()
{
    /* split corner currents to side surfaces */
    if (_run_cmfd)
    {
        _mesh->splitCornerCurrents();
    }
    if (_run_loo)
    {
        _mesh->splitCornerQuadCurrents();
        _mesh->splitCornerQuadWeights();
    }

    /* initialize variables */
    double volume, flux, abs, tot, nu_fis, chi;
    double* scat;
    double abs_tally_group, nu_fis_tally_group, dif_tally_group, rxn_tally_group, vol_tally_group, tot_tally_group;
    double nu_fis_tally = 0, dif_tally = 0, rxn_tally = 0, abs_tally = 0, tot_tally = 0;
    double scat_tally_group[NUM_ENERGY_GROUPS];
    std::vector<int>::iterator iter;
    int i, e, g;

    /* create pointers to objects */
    MeshCell* meshCell;
    FlatSourceRegion* fsr;
    Material* material;

#if USE_OPENMP
#pragma omp parallel for private(i, e, g, volume, flux, abs, tot, nu_fis, \
                                 chi, scat, abs_tally_group, nu_fis_tally_group, \
                                 dif_tally_group, rxn_tally_group, vol_tally_group, tot_tally_group, \
                                 nu_fis_tally, dif_tally, rxn_tally, abs_tally, tot_tally, \
                                 scat_tally_group, iter, meshCell, fsr, material)
#endif
    /* loop over mesh cells */
    for (i = 0; i < _cw * _ch; i++){
        meshCell = _mesh->getCells(i);

        /* if single group, zero tallies */
        if (_mesh->getMultigroup() == false){
            abs_tally = 0.0;
            nu_fis_tally = 0.0;
            dif_tally = 0.0;
            tot_tally = 0.0;
            rxn_tally = 0.0;
        }

        /* loop over energy groups */
        for (e = 0; e < NUM_ENERGY_GROUPS; e++) {

            /* zero tallies for this group */
            abs_tally_group = 0;
            nu_fis_tally_group = 0;
            dif_tally_group = 0;
            rxn_tally_group = 0;
            vol_tally_group = 0;
            tot_tally_group = 0;

            /* zero each group to group scattering tally */
            for (g = 0; g < NUM_ENERGY_GROUPS; g++){
                scat_tally_group[g] = 0;
            }

            /* loop over FSRs in mesh cell */
            for (iter = meshCell->getFSRs()->begin(); iter != meshCell->getFSRs()->end(); ++iter){
                fsr = &_flat_source_regions[*iter];

                /* Gets FSR specific data. */
                material = fsr->getMaterial();
                chi = material->getChi()[e];
                volume = fsr->getVolume();
                flux = fsr->getFlux()[e];
                abs = material->getSigmaA()[e];
                tot = material->getSigmaT()[e];
                nu_fis = material->getNuSigmaF()[e];
                scat = material->getSigmaS();

                /* increment tallies for this group */
                abs_tally_group += abs * flux * volume;
                tot_tally_group += tot * flux * volume;
                nu_fis_tally_group += nu_fis * flux * volume;
                rxn_tally_group += flux * volume;
                vol_tally_group += volume;
                dif_tally_group += flux  * volume / (3.0 * tot);

                /* increment group to group scattering tallies */
                for (g = 0; g < NUM_ENERGY_GROUPS; g++){
                    scat_tally_group[g] += scat[g*NUM_ENERGY_GROUPS + e] * flux * volume;
                    log_printf(DEBUG, "scattering from group %i to %i: %f", e, g, scat[g*NUM_ENERGY_GROUPS + e]);
                }

                /* choose a chi for this group */
                if (chi > meshCell->getChi()[e])
                    meshCell->setChi(chi,e);
            }

            /* if multigroup, set the multigroup parameters */
            if (_mesh->getMultigroup() == true){
                meshCell->setATVolume(vol_tally_group);
                meshCell->setSigmaA(abs_tally_group / rxn_tally_group, e);
                meshCell->setSigmaT(tot_tally_group / rxn_tally_group, e);
                meshCell->setNuSigmaF(nu_fis_tally_group / rxn_tally_group, e);
                meshCell->setDiffusion(dif_tally_group / rxn_tally_group, e);
                meshCell->setOldFlux(rxn_tally_group / vol_tally_group, e);

                for (int g = 0; g < NUM_ENERGY_GROUPS; g++){
                    meshCell->setSigmaS(scat_tally_group[g] / rxn_tally_group,e,g);
                }
            }
            /* if single group, add group-wise tallies to group independent tallies */
            else{
                abs_tally += abs_tally_group;
                tot_tally += tot_tally_group;
                nu_fis_tally += nu_fis_tally_group;
                dif_tally += dif_tally_group;
                rxn_tally += rxn_tally_group;
            }
        }

        /* if single group, set single group parameters */
        if (_mesh->getMultigroup() == false){
            meshCell->setATVolume(vol_tally_group);
            meshCell->setSigmaT(tot_tally / rxn_tally, 0);
            meshCell->setSigmaA(abs_tally / rxn_tally, 0);
            meshCell->setNuSigmaF(nu_fis_tally / rxn_tally, 0);
            meshCell->setDiffusion(dif_tally / rxn_tally, 0);
            meshCell->setOldFlux(rxn_tally / vol_tally_group, 0);
            meshCell->setChi(1,0);
        }
    }
}

void Cmfd::computeXS()
{
    /* split corner currents to side surfaces */
    if (_run_cmfd)
        _mesh->splitCornerCurrents();

    if (_run_loo)
    {
        _mesh->splitCornerCurrents();
        _mesh->splitCornerQuadCurrents();
    }

    /* initialize variables */
    double abs_tally_group, nu_fis_tally_group, dif_tally_group, 
        rxn_tally_group, vol_tally_group, tot_tally_group;
    double nu_fis_tally = 0, dif_tally = 0, rxn_tally = 0, abs_tally = 0, 
        tot_tally = 0, scat_tally = 0;
    double scat_tally_group[NUM_ENERGY_GROUPS];

    double src_tally = 0, src_tally_group = 0;

    std::vector<int>::iterator iter;
    int i,e,g;

    /* create pointers to objects */
    MeshCell* meshCell;
    FlatSourceRegion* fsr;
    Material* material;

    /* loop over mesh cells */
#if USE_OPENMP
#pragma omp parallel for private(i, e, g, volume, flux, abs, tot, nu_fis, \
                                 chi, scat, abs_tally_group, nu_fis_tally_group, \
                                 dif_tally_group, rxn_tally_group, vol_tally_group, tot_tally_group, \
                                 nu_fis_tally, dif_tally, rxn_tally, abs_tally, tot_tally, \
                                 scat_tally_group, iter, meshCell, fsr, material)
#endif 

    for (i = 0; i < _cw * _ch; i++){
        meshCell = _mesh->getCells(i);

        /* if single group, zero tallies */
        if (_mesh->getMultigroup() == false)
        {
            abs_tally = 0.0;
            nu_fis_tally = 0.0;
            dif_tally = 0.0;
            tot_tally = 0.0;
            rxn_tally = 0.0;
            scat_tally = 0.0;
            src_tally = 0.0;
        }

        /* loop over energy groups */
        for (e = 0; e < NUM_ENERGY_GROUPS; e++) 
        {

            /* zero tallies for this group */
            abs_tally_group = 0;
            nu_fis_tally_group = 0;
            dif_tally_group = 0;
            rxn_tally_group = 0;
            vol_tally_group = 0;
            tot_tally_group = 0;
            src_tally_group = 0;

            /* zero each group to group scattering tally */
            for (g = 0; g < NUM_ENERGY_GROUPS; g++)
                scat_tally_group[g] = 0;

            /* loop over FSRs in mesh cell */
            for (iter = meshCell->getFSRs()->begin(); 
                 iter != meshCell->getFSRs()->end(); ++iter)
            {
                fsr = &_flat_source_regions[*iter];

                /* Gets FSR specific data. */
                double volume, flux, chi, abs, tot, nu_fis;
                double *scat;
                volume = fsr->getVolume();
                flux = fsr->getFlux()[e];
                material = fsr->getMaterial();
                chi = material->getChi()[e];
                abs = material->getSigmaA()[e];
                tot = material->getSigmaT()[e];
                nu_fis = material->getNuSigmaF()[e];
                scat = material->getSigmaS();

                /* increment tallies for this group */
                abs_tally_group += abs * flux * volume;
                tot_tally_group += tot * flux * volume;
                nu_fis_tally_group += nu_fis * flux * volume;
                rxn_tally_group += flux * volume;
                vol_tally_group += volume;
                dif_tally_group += flux * volume / (3.0 * tot);
                src_tally_group += fsr->getSource()[e] * volume;

                /* increment group to group scattering tallies */
                for (g = 0; g < NUM_ENERGY_GROUPS; g++)
                {
                    scat_tally_group[g] += scat[g * NUM_ENERGY_GROUPS + e] 
                        * flux * volume;
                }
                if (chi > meshCell->getChi()[e])
                    meshCell->setChi(chi,e);
            }

            /* if multigroup, set the multigroup parameters */
            if (_mesh->getMultigroup())
            {
                meshCell->setATVolume(vol_tally_group);
                meshCell->setSigmaA(abs_tally_group / rxn_tally_group, e);
                meshCell->setSigmaT(tot_tally_group / rxn_tally_group, e);
                meshCell->setNuSigmaF(nu_fis_tally_group / rxn_tally_group, e);
                meshCell->setDiffusion(dif_tally_group / rxn_tally_group, e);
                meshCell->setOldFlux(rxn_tally_group / vol_tally_group, e);
                meshCell->setSrc(src_tally_group / vol_tally_group, e);

                for (g = 0; g < NUM_ENERGY_GROUPS; g++)
                {
                    meshCell->setSigmaS(scat_tally_group[g] / rxn_tally_group,
                                        e, g);
                }
            }
            /* if single group, add group-wise tallies up */
            else
            {
                abs_tally += abs_tally_group;
                tot_tally += tot_tally_group;
                nu_fis_tally += nu_fis_tally_group;
                dif_tally += dif_tally_group;
                rxn_tally += rxn_tally_group;
                for (g = 0; g < NUM_ENERGY_GROUPS; g++)
                    scat_tally += scat_tally_group[g];
                src_tally += src_tally_group;
            }
        }

        /* if single group, set single group parameters */
        if (_mesh->getMultigroup() == false)
        {
            meshCell->setATVolume(vol_tally_group);
            meshCell->setSigmaT(tot_tally / rxn_tally, 0);
            meshCell->setSigmaA(abs_tally / rxn_tally, 0);
            meshCell->setNuSigmaF(nu_fis_tally / rxn_tally, 0);
            meshCell->setDiffusion(dif_tally / rxn_tally, 0);
            meshCell->setOldFlux(rxn_tally / vol_tally_group, 0);
            log_printf(INFO, "mesh = %d, rxn tally = %.10f, vol = %.10f,"
                       " mesh's old flux = %.10f", 
                       i, rxn_tally, vol_tally_group, 
                       meshCell->getOldFlux()[0]);
            meshCell->setChi(1, 0);
            /* SG needs to add up all in-group scattering */
            meshCell->setSigmaS(scat_tally / rxn_tally, 0, 0);
            meshCell->setSrc(src_tally / vol_tally_group, 0);
        }
    }

    return;
}

void Cmfd::computeDsxDirection(double x, double y, int e, MeshCell *meshCell, 
                               double d, double f, double flux, 
                               double dt_weight)
{
    /* initialize variables */
    double d_next = 0, d_hat = 0, d_tilde = 0, current = 0, flux_next = 0, 
        f_next = 0;
    MeshSurface *surf;
    MeshCell* meshCellNext;

    /* if cell on left side, set d_hat and d_tilde to 0 */
    if (x == 0)
    {
        if (_mesh->getBoundary(0) == REFLECTIVE)
        {
            d_hat = 0; 
            d_tilde = 0;
        }
        else if (_mesh->getBoundary(0) == VACUUM)
        {
            surf = meshCell->getMeshSurfaces(0);
            current = - surf->getCurrent(e);
            d_hat = 2 * d * f / meshCell->getWidth() 
                / (1 + 4 * d*f / meshCell->getWidth());
            d_tilde = - (d_hat * flux + current 
                         / meshCell->getHeight()) / flux;
        }
    }
    /* if cell has a left neighbor, computes D's regularly */
    else
    {
        /* get mesh cell to left */
        meshCellNext = _mesh->getCells(y*_cw + x - 1);
        d_next = meshCellNext->getDiffusion()[e];
        flux_next = meshCellNext->getOldFlux()[e];

        /* get diffusion correction term for meshCellNext */
        f_next = computeDiffCorrect(d_next, meshCellNext->getWidth());

        /* compute d_hat */
        d_hat = 2.0 * d*f * d_next * f_next / 
            (meshCell->getWidth() * d * f 
             + meshCellNext->getWidth() * d_next * f_next);

        /* Computes current: increment by outwards current on 
         * next cell's RHS, decrement by outward current on LHS */
        current = 0.0;
        current += meshCellNext->getMeshSurfaces(2)->getCurrent(e);
        current -= meshCell->getMeshSurfaces(0)->getCurrent(e);

        /* compute d_tilde */
        d_tilde = -(d_hat * (flux - flux_next) + current  
                    / meshCell->getHeight()) / (flux_next + flux);
    }

#if 0
    /* if abs(d_tilde) > abs(d_hat), make them equal in magnitude */
    if (fabs(d_tilde) > fabs(d_hat))
    {
        log_printf(DEBUG, "correcting Ds: LEFT group: %i, x: %f,"
                   " y: %f, dh: %f, dt: %f, c:%f", 
                   e, x, y, d_hat, d_tilde, current);

        /* d_tilde is positive */
        if (1 - fabs(d_tilde)/d_tilde < 1e-8)
        {
            d_hat   = - current/(2*flux*meshCell->getHeight());
            d_tilde = - current/(2*flux*meshCell->getHeight());
        }
        else
        {
            d_hat   = current/(2*flux_next*meshCell->getHeight());
            d_tilde = - current/(2*flux_next*meshCell->getHeight());
        }
    }
#endif


    log_printf(DEBUG, "cell: %f, group: %i, side: LEFT,"
               " current: %.10f, dhat: %f, dtilde: %f", 
               y*_cw + x, e, current, d_hat, d_tilde);

    /* set d_hat and d_tilde */
    d_tilde = meshCell->getMeshSurfaces(0)->getDTilde()[e] 
        * (1.0 - dt_weight) + dt_weight * d_tilde;
    meshCell->getMeshSurfaces(0)->setDHat(d_hat, e);
    meshCell->getMeshSurfaces(0)->setDTilde(d_tilde, e);
}

/* compute the xs for all MeshCells in the Mesh */
void Cmfd::computeDs(int moc_iter)
{
    /* initialize variables */
    double d = 0, d_next = 0, d_hat = 0, d_tilde = 0, 
        current = 0, flux = 0, flux_next = 0, f = 1, f_next = 1;
    MeshCell* meshCell;
    MeshCell* meshCellNext;
    int _ng = NUM_ENERGY_GROUPS;
    int x, y, e;
    double dt_weight = _damp_factor;

    if (_mesh->getMultigroup() == false)
        _mesh->computeTotCurrents();

    /* loop over all mesh cells */
    for (y = 0; y < _ch; y++)
    {
        for (x = 0; x < _cw; x++)
        {
            meshCell = _mesh->getCells(y*_cw + x);

            for (e = 0; e < _ng; e++)
            {

                /* get diffusivity and flux for mesh cell */
                d = meshCell->getDiffusion()[e];
                flux = meshCell->getOldFlux()[e];

                /* get diffusion correction term for meshCell */
                f = computeDiffCorrect(d, meshCell->getWidth());

                /* LEFT */
                computeDsxDirection(x, y, e, meshCell, d, f, flux, dt_weight);

                /* RIGHT */
                /* if cell on right side, set d_hat and d_tilde to 0 */
                if (x == _cw - 1)
                {
                    if (_mesh->getBoundary(2) == REFLECTIVE)
                    {
                        d_hat = 0.0;
                        d_tilde = 0.0;
                        current = 0.0;
                    }
                    else if (_mesh->getBoundary(2) == VACUUM)
                    {
                        current = meshCell->getMeshSurfaces(2)->getCurrent(e);
                        d_hat = 2 * d * f / meshCell->getWidth() / 
                            (1 + 4 * d * f / meshCell->getWidth());
                        d_tilde = (d_hat * flux - current / 
                                   meshCell->getHeight()) / flux;
                    }
                }
                else
                {
                    /* get mesh cell to the right */
                    meshCellNext = _mesh->getCells(y*_cw + x + 1);
                    d_next = meshCellNext->getDiffusion()[e];
                    flux_next = meshCellNext->getOldFlux()[e];

                    /* get diffusion correction term for meshCellNext */
                    f_next = computeDiffCorrect(d_next, 
                                                meshCellNext->getWidth());

                    /* compute d_hat */
                    d_hat = 2.0 * d * f * d_next * f_next / 
                        (meshCell->getWidth() * d * f 
                         + meshCellNext->getWidth() * d_next * f_next);

                    /* get net outward current across surface */
                    current = 0.0;
                    current += meshCell->getMeshSurfaces(2)->getCurrent(e);
                    current -= meshCellNext->getMeshSurfaces(0)->getCurrent(e);

                    /* compute d_tilde */
                    d_tilde = - (d_hat * (flux_next - flux) + current / 
                                 meshCell->getHeight()) / (flux_next + flux);
                }

                log_printf(DEBUG, "cell: %i, group: %i, side:  RIGHT,"
                           " current: %f, dhat: %f, dtilde: %f", 
                           y*_cw + x, e, current, d_hat, d_tilde);

#if 0
                /* if abs(d_tilde) > abs(d_hat) make them equal to each other */
                if (fabs(d_tilde) > fabs(d_hat))
                {
                    log_printf(INFO, "correcting Ds: RIGHT group: %i,"
                               " x: %i, y: %i, dh: %f, dt: %f, c: %f", 
                               e, x, y, d_hat, d_tilde, current);

                    /* d_tilde is positive */
                    if (1 - fabs(d_tilde)/d_tilde < 1e-8)
                    {
                        d_hat   = - current/(2*flux_next*meshCell->getHeight());
                        d_tilde = - current/(2*flux_next*meshCell->getHeight());
                    }
                    else
                    {
                        d_hat   = current/(2*flux*meshCell->getHeight());
                        d_tilde = - current/(2*flux*meshCell->getHeight());
                    }
                }
#endif
                /* set d_hat and d_tilde */
                d_tilde = meshCell->getMeshSurfaces(2)->getDTilde()[e] 
                    * (1.0 - dt_weight) + dt_weight * d_tilde;
                meshCell->getMeshSurfaces(2)->setDHat(d_hat, e);
                meshCell->getMeshSurfaces(2)->setDTilde(d_tilde, e);


                /* BOTTOM */
                /* get diffusion correction term for meshCell */
                f = computeDiffCorrect(d, meshCell->getHeight());

                if (y == _ch - 1)
                {
                    if (_mesh->getBoundary(1) == REFLECTIVE)
                    {
                        d_hat = 0.0;
                        d_tilde = 0.0;
                        current = 0.0;
                    }
                    else if (_mesh->getBoundary(1) == VACUUM)
                    {
                        current = meshCell->getMeshSurfaces(1)->getCurrent(e);
                        d_hat = 2 * d*f / meshCell->getHeight() / 
                            (1 + 4 * d*f / meshCell->getHeight());
                        d_tilde = (d_hat * flux - current / 
                                   meshCell->getWidth()) / flux;
                    }
                }
                else
                {
                    /* get mesh cell below */
                    meshCellNext = _mesh->getCells((y+1)*_cw + x);
                    d_next = meshCellNext->getDiffusion()[e];
                    /* FIXME: double check this is the right flux */
                    flux_next = meshCellNext->getOldFlux()[e];

                    /* get diffusion correction term for meshCellNext */
                    f_next = computeDiffCorrect(d_next, 
                                                meshCellNext->getHeight());

                    /* compute d_hat */
                    d_hat = 2.0 * d * f * d_next * f_next / 
                        (meshCell->getHeight() * d * f 
                         + meshCellNext->getHeight() * d_next * f_next);

                    /* get net outward current across surface */
                    current = 0.0;
                    current += meshCell->getMeshSurfaces(1)->getCurrent(e);
                    current -= meshCellNext->getMeshSurfaces(3)->getCurrent(e);

                    /* compute d_tilde */
                    d_tilde = -(d_hat * (flux_next - flux) + current / 
                                meshCell->getWidth()) / (flux_next + flux);
                }

                if ((x == 0) && (y == 0))
                {
                    log_printf(ACTIVE, "cell (%d %d), group: %i, side: BOTTOM, "
                               "current: %f, flux %f, %f, dhat: %f, dtilde: %f",
                               x, y, e, current, flux, flux_next, 
                               d_hat, d_tilde);
                }


#if 0
                /* if abs(d_tilde) > abs(d_hat), equates them */
                if (fabs(d_tilde) > fabs(d_hat))
                {
                    log_printf(INFO, "correcting Ds: BOTTOM group: %i, x: %i, "
                               "y: %i, dh: %f, dt: %f, c:%f", 
                               e, x, y, d_hat, d_tilde, current);

                    /* d_tilde is positive */
                    if (1 - fabs(d_tilde)/d_tilde < 1e-8)
                    {
                        d_hat   = - current/(2*flux_next*meshCell->getWidth());
                        d_tilde = - current/(2*flux_next*meshCell->getWidth());
                    }
                    else
                    {
                        d_hat   = current/(2*flux*meshCell->getWidth());
                        d_tilde = - current/(2*flux*meshCell->getWidth());
                    }
                }
#endif

                /* set d_hat and d_tilde */
                d_tilde = meshCell->getMeshSurfaces(1)->getDTilde()[e] 
                    * (1.0 - dt_weight) + dt_weight * d_tilde;
                meshCell->getMeshSurfaces(1)->setDHat(d_hat, e);
                meshCell->getMeshSurfaces(1)->setDTilde(d_tilde, e);

///////////////////////////////////////////////////////////////////////////////

                /* TOP */
                /* get diffusion correction term for meshCell */
                f = computeDiffCorrect(d, meshCell->getHeight());

                /* if cell on top side, set d_hat and d_tilde to 0 */
                if (y == 0)
                {
                    if (_mesh->getBoundary(3) == REFLECTIVE)
                    {
                        d_hat = 0.0;
                        d_tilde = 0.0;
                        current = 0.0;
                    }
                    else if (_mesh->getBoundary(3) == VACUUM)
                    {
                        current = - meshCell->getMeshSurfaces(3)->getCurrent(e);
                        d_hat = 2 * d*f / meshCell->getHeight() / 
                            (1 + 4 * d*f / meshCell->getHeight());

                        d_tilde = - (d_hat * flux + 
                                     current / meshCell->getWidth()) / flux;
                    }
                }
                else
                {
                    /* get mesh cell above */
                    meshCellNext = _mesh->getCells((y-1)*_cw + x);
                    d_next = meshCellNext->getDiffusion()[e];
                    flux_next = meshCellNext->getOldFlux()[e];

                    /* get diffusion correction term for meshCellNext */
                    f_next = computeDiffCorrect(d_next, 
                                                meshCellNext->getHeight());

                    /* compute d_hat */
                    d_hat = 2.0 * d * f * d_next * f_next / 
                        (meshCell->getHeight() * d * f 
                         + meshCellNext->getHeight() * d_next * f_next);

                    /* get net outward current across surface */
                    current = 0.0;
                    current += meshCellNext->getMeshSurfaces(1)->getCurrent(e);
                    current -= meshCell->getMeshSurfaces(3)->getCurrent(e);

                    /* compute d_tilde */
                    d_tilde = - (d_hat * (flux - flux_next) + 
                                 current / meshCell->getWidth()) / 
                        (flux_next + flux);
                }

                log_printf(DEBUG, "cell: %i, group: %i, side: TOP, current: %f,"
                           " dhat: %f, dtilde: %f", 
                           y*_cw + x, e, current, d_hat, d_tilde);

#if 0
                /* if abs(d_tilde) > abs(d_hat) make them equal to each other */
                if (fabs(d_tilde) > fabs(d_hat)){
                    log_printf(INFO, "correcting Ds: TOP group: %i, x: %i, "
                               "y: %i, dh: %f, dt: %f, c:%f", 
                               e, x, y, d_hat, d_tilde, current);

                    /* d_tilde is positive */
                    if (1 - fabs(d_tilde)/d_tilde < 1e-8)
                    {
                        d_hat   = - current/(2*flux*meshCell->getWidth());
                        d_tilde = - current/(2*flux*meshCell->getWidth());
                    }
                    else
                    {
                        d_hat   = current/(2*flux_next*meshCell->getWidth());
                        d_tilde = - current/(2*flux_next*meshCell->getWidth());
                    }
                }
#endif
                /* set d_hat and d_tilde */
                d_tilde = meshCell->getMeshSurfaces(3)->getDTilde()[e] 
                    * (1.0 - dt_weight) + dt_weight * d_tilde;
                meshCell->getMeshSurfaces(3)->setDHat(d_hat, e);
                meshCell->getMeshSurfaces(3)->setDTilde(d_tilde, e);
            }
        }
    }

    /* if a regular diffusion is requested, set dtilde to zero. */
    if ((moc_iter == 0) && (_first_diffusion))
    {
        for (y = 0; y < _ch; y++)
        {
            for (x = 0; x < _cw; x++)
            {
                meshCell = _mesh->getCells(y * _cw + x);
                
                for (e = 0; e < _ng; e++)
                {
                    for (int s = 0; s < 4; s++)
                        meshCell->getMeshSurfaces(s)->setDTilde(0.0, e);
                }
            }
        }
    }

}

/* Computes _quad_flux based on _quad_current */
void Cmfd::computeQuadFlux()
{
    MeshSurface *s[4];
    MeshCell* meshCell;
    double flux = 0, tmp = 0, wt = 0;

    if (_mesh->getMultigroup() == false)
        _mesh->computeTotQuadCurrents();

    for (int y = 0; y < _ch; y++)
    {
        for (int x = 0; x < _cw; x++)
        {
            meshCell = _mesh->getCells(y * _cw + x);

            /* get four surfaces */
            for (int i = 0; i < 4; i++) 
            {
                s[i] = meshCell->getMeshSurfaces(i);    
                for (int e = 0; e < _ng; e++)
                {
                    tmp = 0.0;
                    wt = 0;

                    /* FIXME: somehow _nq = 4 breaks the code for _ro case */
                    for (int j = 0; j < 4; j++)
                    {
#if NEW
                        if (j < 2)
                            wt = s[i]->getTotalWt(j);
                        else /* quadrature 2 & 3 do not have their own
                              * weights, instead just use 0's and 1's
                              * weights.  */
                            wt = s[i]->getTotalWt(j - 2);
#else
                        wt = _mesh->getCells(0)->getWidth();
#endif

                        if (wt > 1e-10)
                        {
                            flux = s[i]->getQuadCurrent(e, j) / SIN_THETA_45 
                                / wt / P0;
                            s[i]->setQuadFlux(flux, e, j);
                            tmp += s[i]->getQuadCurrent(e, j);
                        }
                        else
                            s[i]->setQuadFlux(0, e, j);
                    }	

#if NEW	
                    s[i]->setCurrent(tmp / s[i]->getTotalWt(2), e);
#else
                    s[i]->setCurrent(tmp, e);
#endif
                }
            }
        }
    }
}

void Cmfd::computeCurrent()
{
    /* Initializations */
    MeshSurface *s[4];
    MeshCell* meshCell;

    /* loop over all mesh cells */
    for (int y = 0; y < _ch; y++)
    {
        for (int x = 0; x < _cw; x++)
        {
            meshCell = _mesh->getCells(y * _cw + x);

            for (int i = 0; i < 4; i++) 
            {
                s[i] = meshCell->getMeshSurfaces(i);    
                for (int e = 0; e < _ng; e++)
                {
                    /* FIXME: need to figure out what to do here */
                    double wt = s[i]->getTotalWt(2); 
                    
                    s[i]->setCurrent(s[i]->getCurrent(e) / wt, e);
                }
            }
        }
    }

    return;
}

/* Store new_quad_flux into old_quad_flux for book-keeping */
void Cmfd::updateOldQuadFlux()
{
    /* Initializations */
    MeshSurface *s[4];
    MeshCell* meshCell;

    /* loop over all mesh cells */
    for (int y = 0; y < _ch; y++)
    {
        for (int x = 0; x < _cw; x++)
        {
            meshCell = _mesh->getCells(y * _cw + x);
            for (int i = 0; i < 4; i++) 
            {
                s[i] = meshCell->getMeshSurfaces(i);
                for (int e = 0; e < _ng; e++)
                {
                    // FIXME: _nq
                    for (int j = 0; j < 4; j++)
                    {
                        s[i]->setOldQuadFlux(s[i]->getQuadFlux(e, j), e, j);
                    } 
                }
            }
        }
    }

    return;
}

/* Computes _quad_src based on (m+1/2) results */
void Cmfd::computeQuadSrc()
{
    MeshSurface *s[4];
    MeshCell *meshCell, *nextCell;
    double **out = new double*[_ng];
    double **in = new double*[_ng];
    for (int i = 0; i < _ng; i++)
    {
        out[i] = new double[8];
        in[i] = new double[8];
    }

    /* loop over all mesh cells */
    for (int y = 0; y < _ch; y++)
    {
        for (int x = 0; x < _cw; x++)
        {
            meshCell = _mesh->getCells(y * _cw + x);

            /* get four surfaces */
            for (int i = 0; i < 4; i++) 
                s[i] = meshCell->getMeshSurfaces(i);

            /* initialize the 8 outgoing quad fluxes */
            for (int e = 0; e < _ng; e++)
            {
                out[e][0] = s[2]->getQuadFlux(e,0);
                out[e][1] = s[1]->getQuadFlux(e,0);
                out[e][2] = s[3]->getQuadFlux(e,1);
                out[e][3] = s[2]->getQuadFlux(e,1);
                out[e][4] = s[0]->getQuadFlux(e,0);
                out[e][5] = s[3]->getQuadFlux(e,0);
                out[e][6] = s[1]->getQuadFlux(e,1);
                out[e][7] = s[0]->getQuadFlux(e,1);

                if (x == 0)
                {
                    if (_bc[0] == REFLECTIVE)
                    {
                        if (_reflect_outgoing)
                        {
                            in[e][5] = out[e][7];
                            //in[e][5] = s[0]->getQuadFlux(e,2);
                            in[e][6] = out[e][4];
                        }
                        else
                        {
                            in[e][5] = s[0]->getQuadFlux(e,2);
                            in[e][6] = s[0]->getQuadFlux(e,3);
                        }

                        // FIXME? 
                        s[0]->setQuadFlux(in[e][5], e, 1);
                        s[0]->setQuadFlux(in[e][6], e, 0);
                        /*
                        s[0]->setOldQuadFlux(in[e][5], e, 1);
                        s[0]->setOldQuadFlux(in[e][6], e, 0); 
                        */
                    }
                    else if (_bc[0] == VACUUM)
                    {
                        in[e][5] = 0;
                        in[e][6] = 0;
                    }
                }
                else
                {
                    nextCell = _mesh->getCells(y * _cw + x - 1);
                    in[e][5] = nextCell->getMeshSurfaces(2)->getQuadFlux(e,0);
                    in[e][6] = nextCell->getMeshSurfaces(2)->getQuadFlux(e,1);
                }			

                if (x == _cw - 1)
                {
                    if (_bc[2] == REFLECTIVE)
                    {
                        if (_reflect_outgoing)
                        {
                            in[e][1] = out[e][3];
                            //in[e][1] = s[2]->getQuadFlux(e,2);
                            in[e][2] = out[e][0];
                        }
                        else
                        {
                            in[e][1] = s[2]->getQuadFlux(e,2);
                            in[e][2] = s[2]->getQuadFlux(e,3);
                        }

                        s[2]->setQuadFlux(in[e][1], e, 1);
                        s[2]->setQuadFlux(in[e][2], e, 0);
                        /*
                        s[2]->setOldQuadFlux(in[e][1], e, 1);
                        s[2]->setOldQuadFlux(in[e][2], e, 0);
                        */
                    }
                    else if (_bc[2] == VACUUM)
                    {
                        in[e][1] = 0;
                        in[e][2] = 0;
                    }
                }
                else
                {
                    nextCell = _mesh->getCells(y * _cw + x + 1);
                    in[e][1] = nextCell->getMeshSurfaces(0)->getQuadFlux(e,0);
                    in[e][2] = nextCell->getMeshSurfaces(0)->getQuadFlux(e,1);
                }			
			
                if (y == 0)
                {
                    if (_bc[3] == REFLECTIVE)
                    {
                        if (_reflect_outgoing)
                        {
                            in[e][4] = out[e][2];
                            //in[e][4] = s[3]->getQuadFlux(e,2);
                            in[e][3] = out[e][5];
                        }
                        else
                        {
                            in[e][4] = s[3]->getQuadFlux(e,2);
                            in[e][3] = s[3]->getQuadFlux(e,3);
                        }

                        s[3]->setQuadFlux(in[e][4], e, 1);
                        s[3]->setQuadFlux(in[e][3], e, 0); 
                        /*
                        s[3]->setOldQuadFlux(in[e][4], e, 1);
                        s[3]->setOldQuadFlux(in[e][3], e, 0);
                        */
                    }
                    else if (_bc[3] == VACUUM)
                    {
                        in[e][3] = 0;
                        in[e][4] = 0;
                    }
                }
                else
                {
                    nextCell = _mesh->getCells((y - 1) * _cw + x);
                    in[e][3] = nextCell->getMeshSurfaces(1)->getQuadFlux(e,1);
                    in[e][4] = nextCell->getMeshSurfaces(1)->getQuadFlux(e,0);
                }

                if (y == _ch - 1)
                {
                    if (_bc[1] == REFLECTIVE)
                    {
                        if (_reflect_outgoing)
                        {
                            in[e][0] = out[e][6];
                            //in[e][0] = s[1]->getQuadFlux(e,2);
                            in[e][7] = out[e][1];
                        }
                        else
                        {
                            in[e][0] = s[1]->getQuadFlux(e,2);
                            in[e][7] = s[1]->getQuadFlux(e,3);
                        }

                        s[1]->setQuadFlux(in[e][0], e, 1);
                        s[1]->setQuadFlux(in[e][7], e, 0);
                        /*
                        s[1]->setOldQuadFlux(in[e][0], e, 1);
                        s[1]->setOldQuadFlux(in[e][7], e, 0); 
                        */
                    }
                    else if (_bc[1] == VACUUM)
                    {
                        in[e][7] = 0;
                        in[e][0] = 0;
                    }
                }
                else
                {
                    nextCell = _mesh->getCells( (y + 1) * _cw + x);
                    in[e][7] = nextCell->getMeshSurfaces(3)->getQuadFlux(e,1);
                    in[e][0] = nextCell->getMeshSurfaces(3)->getQuadFlux(e,0);
                }
            }

            /* Now that we have all the in's and out's, computes src */
            /* has to use get l to get the polar angle right */
            double l = meshCell->getATL();
            double xs = 0, ex = 0, sum_quad_flux = 0, tmp_src = 0, src = 0;
            for (int e = 0; e < _ng; e++)
            {
                xs = meshCell->getSigmaT()[e];
                ex = exp(-xs * l);
                sum_quad_flux = 0;
                tmp_src = 0;
                for (int t = 0; t < 8; t++)
                {
                    src = xs * (out[e][t] - ex * in[e][t]) / (1.0 - ex);

                    if (src < 0)
                    {
                        log_printf(NORMAL, "(%d %d) e %d t %d"
                                   " quad src = %f * (%f - %f * %f) / %f = %f",
                                   x, y, e, t, xs, out[e][t], ex, in[e][t], 
                                   1 - ex, 
                                   (out[e][t] - ex * in[e][t]) / (1-ex));
                    }

                    meshCell->setQuadSrc(src, e, t);
                    tmp_src += src;
                    sum_quad_flux += src/xs + (in[e][t] - out[e][t])/(xs * l);
                }
                meshCell->setSumQuadFlux(sum_quad_flux, e);

                log_printf(DEBUG, " cell %d e %d"
                           " bar q / sum q = %.10f, phi / sum psi = %.10f", 
                           y * _cw + x, e,
                           FOUR_PI * meshCell->getSrc()[e] / tmp_src,
                           meshCell->getOldFlux()[e] 
                           / meshCell->getSumQuadFlux()[e]);
            }
        }
    }
    
    for (int i = 0; i < _ng; i++)
    {
        delete[] in[i];
        delete[] out[i];
    }
    delete[] in;
    delete[] out;

    return;
}	 

/*
 * CMFD solver that solves the diffusion problem
 * @param solve methed - either diffusion or cmfd (acceleration)
 * @param iteration number of in MOC solver - used for plotting
 * @return k-effective
 */
double Cmfd::computeCMFDFluxPower(solveType solveMethod, int moc_iter)
{
    if (solveMethod == DIFFUSION)
        log_printf(NORMAL, "Running coarse-mesh diffusion solver...");
    else
        log_printf(DEBUG, "Running CMFD Acceleration");

    MeshCell* meshCell;
    int max_outer, iter = 0, petsc_err;
    Vec phi_old, sold, snew, res;
    PetscInt size, its;
    PetscScalar sumold, sumnew, scale_val, eps = 0;
    PetscScalar rtol = 1e-10, atol = rtol;
    std::string string;
    PetscScalar *old_phi, *new_phi;

    /* create petsc array to store old flux */
    size = _ch * _cw * _ng;
    petsc_err = VecCreateSeq(PETSC_COMM_WORLD, size, &phi_old);
    CHKERRQ(petsc_err);
	
    /* zero out and construct memory efficient versions of
     * A matrix, M matrix, and flux vector */
    petsc_err = MatZeroEntries(_A);
    petsc_err = MatZeroEntries(_M);
    petsc_err = constructAMPhi(_A, _M, phi_old, solveMethod);
    CHKERRQ(petsc_err);

    /*
    petsc_err = VecGetArray(phi_old, &old_phi);
    log_printf(ACTIVE, "CMFD initial flux from MOC:");
    for (int i = 0; i < _cw*_ch; i++)
    {
        log_printf(ACTIVE, " cell %d energy 0 flux %f", 
                   i, (double)old_phi[i*_ng + 0]);
    }
    */

    /* if solve method is DIFFUSION, set max_outer to large number to allow 
     * the solver to fully converge; for CMFD, 50 is probably sufficient */
    if (solveMethod == DIFFUSION)
        max_outer = 1000;
    else
        max_outer = 500;

    int min_outer = 5;

    /* create old source and residual vectors */
    petsc_err = VecCreateSeq(PETSC_COMM_WORLD, _ch * _cw * _ng, &sold);
    petsc_err = VecCreateSeq(PETSC_COMM_WORLD, _ch * _cw * _ng, &snew);
    petsc_err = VecCreateSeq(PETSC_COMM_WORLD, _ch * _cw * _ng, &res);
    CHKERRQ(petsc_err);

    /* assembly vectors and matrices*/
    petsc_err = VecAssemblyBegin(phi_old);
    petsc_err = VecAssemblyEnd(phi_old);
    petsc_err = VecAssemblyBegin(_phi_new);
    petsc_err = VecAssemblyEnd(_phi_new);
    petsc_err = VecAssemblyBegin(_source_old);
    petsc_err = VecAssemblyEnd(_source_old);
    petsc_err = VecAssemblyBegin(sold);
    petsc_err = VecAssemblyEnd(sold);
    petsc_err = VecAssemblyBegin(snew);
    petsc_err = VecAssemblyEnd(snew);
    petsc_err = VecAssemblyBegin(res);
    petsc_err = VecAssemblyEnd(res);
    petsc_err = MatAssemblyBegin(_A, MAT_FINAL_ASSEMBLY);
    petsc_err = MatAssemblyEnd(_A, MAT_FINAL_ASSEMBLY);
    petsc_err = MatAssemblyBegin(_M, MAT_FINAL_ASSEMBLY);
    petsc_err = MatAssemblyEnd(_M, MAT_FINAL_ASSEMBLY);
    CHKERRQ(petsc_err);

    /*
    if ((solveMethod == CMFD) && (moc_iter == 4))
    {
        PetscViewerSetFormat(PETSC_VIEWER_STDOUT_SELF, 
                             PETSC_VIEWER_ASCII_MATLAB);
        MatView(_A, PETSC_VIEWER_STDOUT_SELF);
        MatView(_M, PETSC_VIEWER_STDOUT_SELF);
    }
    */

    /* initialize KSP solver */
    KSP ksp;
    petsc_err = KSPCreate(PETSC_COMM_WORLD, &ksp);
    petsc_err = KSPSetTolerances(ksp, rtol, atol, PETSC_DEFAULT, PETSC_DEFAULT);
    petsc_err = KSPSetType(ksp, KSPGMRES);
    //petsc_err = PetscObjectSetPrecision((_p_PetscObject*) ksp, 
    //					  PETSC_PRECISION_DOUBLE);
    petsc_err = KSPSetInitialGuessNonzero(ksp, PETSC_TRUE);
    petsc_err = KSPSetOperators(ksp, _A, _A, SAME_NONZERO_PATTERN);

    /* from options must be called before KSPSetUp() */
    petsc_err = KSPSetFromOptions(ksp);
    petsc_err = KSPSetUp(ksp);
    CHKERRQ(petsc_err);

    /* get initial source and find initial k_eff */
    petsc_err = MatMult(_M, phi_old, snew);
    petsc_err = VecSum(snew, &sumnew);
    petsc_err = MatMult(_A, phi_old, sold);
    petsc_err = VecSum(sold, &sumold);
    _keff = (double) sumnew / (double) sumold;

    if (moc_iter == 10000)
    {
        log_printf(NORMAL, " prior to CMFD, xs collapsed from MOC"
                   " produce keff of %.10f = %.10f / %.10f", 
                   _keff, (double) sumnew, (double) sumold);
    }

    petsc_err = VecCopy(snew, _source_old);


    /*
    VecCopy(snew, sold);
    VecScale(sold, 1 / _keff);
    sumold = sumnew / _keff;
    */
    petsc_err = MatMult(_M, phi_old, sold);
    petsc_err = VecSum(sold, &sumold);
    scale_val = 1/_keff;
    petsc_err = VecScale(sold, scale_val);
    sumold *= scale_val;

    //VecView(_phi_new, PETSC_VIEWER_STDOUT_SELF);

    /* diffusion solver */
    for (iter = 0; iter < max_outer; iter++)
    {
        /* Solve x = A_inverse * b problem and compute new source */
        petsc_err = KSPSolve(ksp, sold, _phi_new);
        petsc_err = KSPGetIterationNumber(ksp,&its);
        CHKERRQ(petsc_err);

        //VecView(sold, PETSC_VIEWER_STDOUT_SELF);
        //VecView(_phi_new, PETSC_VIEWER_STDOUT_SELF);
		
        /* compute and set keff */
        petsc_err = MatMult(_M, _phi_new, snew);
        petsc_err = VecSum(snew, &sumnew);
        _keff =  sumnew / sumold;

        /* divide new RHS by keff for the next iteration */
        scale_val = 1/_keff;
        petsc_err = VecScale(snew, scale_val);
        sumnew *= scale_val;
        CHKERRQ(petsc_err);

        /* compute the L2 norm of source error */
        PetscScalar *old_source, *new_source;
        petsc_err = VecGetArray(sold, &old_source);
        petsc_err = VecGetArray(snew, &new_source);     
        
        double *old_source_energy_integrated, *new_source_energy_integrated;
        old_source_energy_integrated = new double[_cw * _ch];
        new_source_energy_integrated = new double[_cw * _ch];
        
        for (int ii = 0; ii < _cw * _ch; ii++)
        {
            old_source_energy_integrated[ii] = 0;
            new_source_energy_integrated[ii] = 0;
            
            for (int e = 0; e < _ng; e++)
            { 
                old_source_energy_integrated[ii] += 
                    (double) old_source[ii * _ng + e];
                new_source_energy_integrated[ii] += 
                    (double) new_source[ii * _ng + e];
            }
        }

        eps = computeCellSourceNormGivenTwoSources(old_source_energy_integrated,
                                                   new_source_energy_integrated
            );

        petsc_err = VecRestoreArray(sold, &old_source);
        petsc_err = VecRestoreArray(snew, &new_source);


        /* prints keff and error */
        if (moc_iter == 10000)
        {
            log_printf(NORMAL, " %d-th CMFD iter k = %.10f = %.10f / %.10f,"
                       " eps = %e, taking %d KSP iterations", 
                       iter, _keff, sumold * _keff, sumold, eps, (int)its);

            petsc_err = VecGetArray(phi_old, &old_phi);
            petsc_err = VecGetArray(_phi_new, &new_phi);
			
            for (int i = 0; i < _cw * _ch; i++)
            {
                meshCell = _mesh->getCells(i);
                for (int e = 0; e < _ng; e++)
                {
                    log_printf(ACTIVE, " Update from (m+1/2) %e =  %f / %f - 1",
                               (double)(old_phi[i*_ng + e]) 
                               / (double)(new_phi[i*_ng + e]) - 1.0,
                               (double)(old_phi[i*_ng + e]), 
                               (double)(new_phi[i*_ng + e])
                        );
                }
            }			
            petsc_err = VecRestoreArray(phi_old, &old_phi);
            petsc_err = VecRestoreArray(_phi_new, &new_phi);
        }
        else
        {
            log_printf(DEBUG, " %d-th CMFD iter k = %.10f, eps = %e"
                       " GMRES # = %d" , 
                       iter, _keff, eps, (int)its);
        }

        /* book-keeping: set old source to new source */
        petsc_err = VecCopy(snew, sold);
        VecSum(sold, &sumold);
        CHKERRQ(petsc_err);

        /* check for convergence for the CMFD iterative solver using 
         * _l2_norm_conv_thresh as criteria */
        if ((iter > min_outer) && (eps < _l2_norm_conv_thresh))
            break;
    }
    _num_iter_to_conv = iter;

    /* scale new flux such that its sum equals that of the old flux */
    petsc_err = MatMult(_M, _phi_new, snew);
    petsc_err = VecSum(snew, &sumnew);
    petsc_err = MatMult(_M, phi_old, sold);
    petsc_err = VecSum(sold, &sumold);
    scale_val = sumold / sumnew;
    petsc_err = VecScale(_phi_new, scale_val);
    CHKERRQ(petsc_err);

    petsc_err = VecGetArray(phi_old, &old_phi);
    petsc_err = VecGetArray(_phi_new, &new_phi);
    CHKERRQ(petsc_err);

    log_printf(ACTIVE, "CMFD converged flux:");
    for (int i = 0; i < _cw*_ch; i++)
    {
        meshCell = _mesh->getCells(i);
        for (int e = 0; e < _ng; e++)
        {
            meshCell->setOldFlux(double(old_phi[i*_ng + e]), e);
            meshCell->setNewFlux(double(new_phi[i*_ng + e]), e);
        }

        log_printf(ACTIVE, " cell %d energy 0 new flux %f, old flux %f", 
                   i, new_phi[i * _ng + 0], old_phi[i * _ng + 0]);
    }

    petsc_err = VecRestoreArray(phi_old, &old_phi);
    petsc_err = VecRestoreArray(_phi_new, &new_phi);
    CHKERRQ(petsc_err);

    /* Computes L2 norm between the source that enters the CMFD acceleration 
     * step and the one coming out of converged CMFD step to decided whether 
     * the outter MOC iteration / source iteration should quit */
    if (moc_iter > 0)
        petsc_err = computeCmfdL2Norm(snew, moc_iter);

    /* Copies source new to source old */
    petsc_err = VecCopy(snew, _source_old);
    CHKERRQ(petsc_err);

    /* compute the residual */
    MatMult(_A,_phi_new, sold);
    sumold = 1/_keff;
    MatScale(_M,sumold);
    MatMult(_M,_phi_new,snew);
    sumold = -1;
    VecWAXPY(res, sumold, snew, sold);
    VecSum(res,&sumold);
    log_printf(DEBUG, "CMFD/DIFFUSION residual: %f", double(sumold));

    /* destroy matrices and vectors */
    petsc_err = VecDestroy(&phi_old);
    petsc_err = VecDestroy(&snew);
    petsc_err = VecDestroy(&sold);
    petsc_err = VecDestroy(&res);
    petsc_err = KSPDestroy(&ksp);
    CHKERRQ(petsc_err);

    /* plot flux, current, and k_eff */
    if (solveMethod == DIFFUSION)
    {
        if (_plotter->plotDiffusion() == true)
        {
            string = "diff";
            _plotter->plotCMFDflux(_mesh, string, moc_iter);
        }
    }

    if (_plotter->plotKeff())
        _plotter->plotCMFDKeff(_mesh, moc_iter);

    if (solveMethod == CMFD)
        _mesh->setKeffCMFD(_keff, moc_iter);

    return _keff;
}

/* Computes the flux in each mesh cell using LOO */
double Cmfd::computeLooFluxPower(int moc_iter, double k_MOC)
{
    if (_run_loo_phi)
        log_printf(DEBUG, "Running LOO (phi) ...");
    else if (_run_loo_psi)
        log_printf(DEBUG, "Running LOO (psi) ...");
    else
        log_printf(ERROR, "Neither LOO psi nor phi is requested.");

    int loo_iter, max_outer = 500; 

    /* we set min_outer to make sure the low order system's
     * convergence criteria is sufficiently tight */ 
    int min_outer = 5;

    if (moc_iter == 10000)
    {
        max_outer = 30;
        log_printf(NORMAL, "DEBUG mode on, max outer = %d", max_outer);
    }

    _keff = k_MOC;
    _converged = false;
    MeshCell* meshCell;

    /* Allocate memories for terms internal to the LOO iterative solver */
    double eps = 0.0;
    double *old_power = NULL, *new_power = NULL;
    double **sum_quad_flux, **quad_xs, **expo, **tau, **new_src;
    double **new_quad_src, **net_current;

    try
    {
        old_power = new double[_cw * _ch];
        new_power = new double[_cw * _ch];

        sum_quad_flux = new double*[_cw*_ch];
        quad_xs = new double*[_cw*_ch];
        expo = new double*[_cw*_ch];
        tau = new double*[_cw*_ch];
        new_src = new double*[_cw*_ch];
        new_quad_src = new double*[_cw*_ch];
        net_current = new double*[_cw*_ch];

        for (int i = 0; i < _cw * _ch; i++)
        {
            new_src[i] = new double[_ng];
            sum_quad_flux[i] = new double[_ng];
            quad_xs[i] = new double[_ng];
            tau[i] = new double[_ng];
            expo[i] = new double[_ng];
            net_current[i] = new double[_ng];
            new_quad_src[i] = new double[8 * _ng];
        }
    }
    catch (std::exception &e)
    {
        log_error("Unable to allocate memory for LOO."
                  " Backtrace: \n%s", e.what());
    }

    for (int i = 0; i < _cw * _ch; i++)
    {
        meshCell = _mesh->getCells(i);
        old_power[i] = 0;
        for (int e = 0; e < _ng; e++)
        {
            new_src[i][e] = 0.0;
            sum_quad_flux[i][e] = 0;
            quad_xs[i][e] = meshCell->getSigmaT()[e];
            tau[i][e] = quad_xs[i][e] * meshCell->getATL();
            expo[i][e] = exp(-tau[i][e]);
            old_power[i] +=  meshCell->getNuSigmaF()[e] 
                * meshCell->getOldFlux()[e];
            meshCell->setNewFlux(meshCell->getOldFlux()[e], e);
        }
        for (int t = 0; t < 8 * _ng; t++)
            new_quad_src[i][t] = 0.0;
    }

    /* Starts LOO acceleration iteration, we do not update src, quad_src, 
     * quad_flux, old_flux, as they are computed from the MOC step (i.e., 
     * order m+1/2) and should not be updated during acceleration step. */
    for (loo_iter = 0; loo_iter < max_outer; loo_iter++)
    {
        log_printf(DEBUG, "k passed into LOO = %.10f", _keff);
		
        /* Resets terms to zeros for each LOO iteration */
        for (int i = 0; i < _cw * _ch; i++)
        {
            for (int e = 0; e < _ng; e++)
            {
                net_current[i][e] = 0.0;
                sum_quad_flux[i][e] = 0.0;
            }
        }

        /* Computes new cell averaged source, looping over energy groups */
        double src_ratio;
        for (int i = 0; i < _cw * _ch; i++)
        {
            meshCell = _mesh->getCells(i);

            for (int e = 0; e < _ng; e++)
            {
                new_src[i][e] = 0.0;
                for (int g = 0; g < _ng; g++)
                {
                    new_src[i][e] += meshCell->getSigmaS()[g * _ng + e] 
                        * meshCell->getNewFlux()[g] * ONE_OVER_FOUR_PI;
                    new_src[i][e] += meshCell->getChi()[e] *
                        meshCell->getNuSigmaF()[g] / _keff 
                        * meshCell->getNewFlux()[g] * ONE_OVER_FOUR_PI;
                }
                /* getOldSrc()[e] returns the $\bar{Q}_g^{(m)}$ */
                src_ratio = new_src[i][e] / meshCell->getOldSrc()[e];

                log_printf(DEBUG, " cell %d e %d, src_ratio -1 = %e",
                           i, e, src_ratio - 1.0);

                for (int t = 0; t < 8; t++)
                {
                    int d = e * 8 + t;
                    new_quad_src[i][d] = meshCell->getQuadSrc()[d] * src_ratio;
                }
            }
        } /* finishing looping over i; exit to iter level */

        double leak_tot = 0.0;

        /* Sweeps over geometry, solve LOO MOC */
        for (int e = 0; e < _ng; e++)
        {
            double flux = 0, initial_flux = 0, delta = 0;
            int i, t, d;

            /* Forward Directions */
            for (int j = 0; j < _num_loop; j++)
            {
                /* Get the initial angular flux */
                /* FIXME: should be able to get rid of the condition here */
                if (_bc[1] == REFLECTIVE)
                {
                    flux = _mesh->getCells(_i_array[_num_track * j])
                        ->getMeshSurfaces(1)->getQuadFlux(e, 1);
                }
                else if (_bc[1] == VACUUM)
                    flux = 0;
                else
                    log_printf(ERROR, "Unknown BC at 1st surface");

                /* Store initial flux for debugging */
                initial_flux = flux;
                log_printf(INFO, "Sweeping loop %d forward", j);

                for (int x = _num_track * j; x < _num_track * (j + 1); x++)
                {
                    i = _i_array[x];
                    t = _t_array[x];
                    d = e * 8 + t;

                    /* Set flux to zero if incoming from a vacuum boundary */
                    if (onVacuumBoundary(t, i, 0))
                    {
                        // there is a hack here: if initial side is
                        // vacuum, flux is already 0. 
                        leak_tot += flux * getSurf(i, t, 0); 

                        log_printf(DEBUG, "cell %d track %d (forward) starts"
                                   " from a VAC", i, t);
                        flux = 0.0; 
                    }
	
                    if (_converged && _update_boundary)
                    {
                        int surf_id = onReflectiveBoundary(t, i, 0);

                        if ((surf_id == 0) || (surf_id == 2))
                        {
                            _mesh->getCells(i)->getMeshSurfaces(surf_id)
                                ->setQuadFlux(flux, e, 0);
                        }
                        else if (surf_id > -1)
                        {
                            _mesh->getCells(i)->getMeshSurfaces(surf_id)
                                ->setQuadFlux(flux, e, 1);
                        }
                    }

                    delta = (flux - new_quad_src[i][d] / (double)quad_xs[i][e])
                        * (1 - expo[i][e]);

                    sum_quad_flux[i][e] += delta / tau[i][e] + 
                        new_quad_src[i][d] / quad_xs[i][e];
#if NEW
                    net_current[i][e] -= flux * getSurf(i, t, 0); 
                    flux -= delta;
                    net_current[i][e] += flux * getSurf(i, t, 1);
#else
                    flux -= delta;
                    net_current[i][e] -= delta;
#endif
                }

                if (_bc[1] == REFLECTIVE)
                {
                    _mesh->getCells(_i_array[_num_track * j])
                        ->getMeshSurfaces(1)->setQuadFlux(flux, e, 1);
                    log_printf(DEBUG, "update boundary for cell %d"
                               " energy %d surface 1 forward", 
                               _i_array[_num_track * j], e);
                }
                else if (_bc[1] == VACUUM)
                {
                    // setting to zero should be redundant 
                    //_mesh->getCells(_i_array[_num_track * j])
                    //    ->getMeshSurfaces(1)->setQuadFlux(0.0, e, 1);
                    leak_tot += flux * getSurf(_i_array[_num_track * j], 0, 0);
                }
                else
                    log_printf(ERROR, "spot unknonwn BC at surface 1");

                if (initial_flux > 1e-10)
                {
                    log_printf(DEBUG, "  Energy %d, loop %d, fwd, %f -> %f,"
                               " %e",
                               e, j, initial_flux, flux, 
                               flux / initial_flux - 1.0);
                }
            } /* exit this loop j */

            /* Backward Directions */
            for (int j = 0; j < _num_loop; j++)
            {
                if (_bc[1] == REFLECTIVE)
                {
                    flux = _mesh->getCells(_i_array[_num_track * j])
                        ->getMeshSurfaces(1)->getQuadFlux(e, 0);
                }
                else if (_bc[1] == VACUUM)
                    flux = 0.0;
                else
                    log_printf(ERROR, "spot unknown BC at surface 1");
					
                initial_flux = flux; 
                log_printf(INFO, "Sweeping loop %d backward", j);
                for (int x = _num_track * j + _num_track - 1; 
                     x > _num_track * j - 1; x--)
                {
                    i = _i_array[x];
                    t = _t_arrayb[x];
                    d = e * 8 + t;

                    if (onVacuumBoundary(t, i, 1))
                    {							
                        // FIXME: should not need to add initial to leak_tot
                        //if (x < _num_track * j + _num_track - 1)
                        leak_tot += flux * getSurf(i, t, 0);

                        log_printf(DEBUG, "cell %d track %d (backward)"
                                   " starts from a VAC", i, t);

                        flux = 0.0;
                    }

                    if (_converged && _update_boundary)
                    {
                        int surf_id = onReflectiveBoundary(t, i, 1);

                        if ((surf_id == 0) || (surf_id == 2))
                        {
                            _mesh->getCells(i)->getMeshSurfaces(surf_id)
                                ->setQuadFlux(flux, e, 1);
                        }
                        else if (surf_id > -1)
                        {
                            _mesh->getCells(i)->getMeshSurfaces(surf_id)
                                ->setQuadFlux(flux, e, 0);
                        }
                    } 

                    delta = (flux - new_quad_src[i][d] / quad_xs[i][e])
                        * (1.0 - expo[i][e]);

                    sum_quad_flux[i][e] += delta / tau[i][e] + 
                        new_quad_src[i][d] / quad_xs[i][e];

#if NEW
                    net_current[i][e] -= flux * getSurf(i, t, 0); 
                    flux -= delta;
                    net_current[i][e] += flux * getSurf(i, t, 1);
#else
                    flux -= delta;
                    net_current[i][e] -= delta;                 
#endif
                }

                if (_bc[1] == REFLECTIVE)
                {
                    _mesh->getCells(_i_array[_num_track * j])
                        ->getMeshSurfaces(1)->setQuadFlux(flux, e, 0);
                    log_printf(DEBUG, "update boundary for cell %d"
                               " energy %d surface 1 backward", 
                               _i_array[_num_track * j], e);
                }
                else if (_bc[1] == VACUUM)
                {
                    // again redundant
                    //_mesh->getCells(_i_array[_num_track * j])
                    //    ->getMeshSurfaces(1)->setQuadFlux(0.0, e, 0);
                    leak_tot += flux * getSurf(_i_array[_num_track * j], 0, 0);
                }

                if (initial_flux > 1e-10) 
                {
                    log_printf(DEBUG, "  Energy %d, loop %d, bwd, %f -> %f,"
                               " %e",
                               e, j, initial_flux, flux, 
                               flux / initial_flux - 1.0);
                }
            }
        } /* finish looping over energy; exit to iter level */


        /* Computs new cell-averaged scalar flux */
        double new_flux = 0;
        if (_run_loo_phi)
        {
            for (int i = 0; i < _cw * _ch; i++)
            {
                meshCell = _mesh->getCells(i);
#if NEW
                double vol = meshCell->getATVolume(); 
#else
                double vol = meshCell->getATVolume() / meshCell->getWidth();
#endif

                for (int e = 0; e < _ng; e++)
                {                    
                    net_current[i][e] *= SIN_THETA_45 * P0;

                    new_flux = (FOUR_PI * new_src[i][e] 
                                - net_current[i][e] / vol)
                        / meshCell->getSigmaT()[e];

                    if (new_flux < 0)
                    {
                        log_printf(DEBUG, "Cell %d e %d new / old flux"
                                   " %f/ %f", i, e, 
                                   new_flux, meshCell->getNewFlux()[e]);
                        new_flux = 1e-5;
                    }

                    meshCell->setNewFlux(new_flux, e);		       

                }
            }
        }
        else if (_run_loo_psi)
        {
            double phi_ratio;
            for (int i = 0; i < _cw * _ch; i++)
            {
                meshCell = _mesh->getCells(i);
                for (int e = 0; e < _ng; e++) 
                {
                    phi_ratio =  sum_quad_flux[i][e] 
                        / meshCell->getSumQuadFlux()[e];

                    if (phi_ratio < 0)
                    {
                        log_printf(DEBUG, "Cell %d e %d scalar flux update"
                                   "by (before normalization) = %e", 
                                   i, e, phi_ratio);
                        phi_ratio = 1e-5;
                    }

                    new_flux = meshCell->getOldFlux()[e] * phi_ratio;

                    meshCell->setNewFlux(new_flux, e);
                }
            }
        }
        else
            log_printf(ERROR, "Neither LOO-psi nor LOO-phi was requested");

        log_printf(DEBUG, "raw leakage = %.10f", leak_tot);

        /* Computes normalization factor based on fission source */
        double normalize_factor = computeNormalization();
        log_printf(DEBUG, "normalize_factor = %.10f", normalize_factor);

        /* FIXME: should or should not normalize leak_tot? */
        //leak_tot *= normalize_factor;

        double net_current_tot = 0;
        for (int i = 0; i < _cw * _ch; i++)
        {
            for (int e = 0; e < _ng; e++)
            {
                net_current_tot += net_current[i][e];
                //net_current[i][e] *= normalize_factor;
                _mesh->getCells(i)->setNetCurrent(net_current[i][e], e);
            }
        }

        normalizeFlux(normalize_factor);

        /* Computes keff with leakage */
        double fis_tot = 0, abs_tot = 0, vol = 0;
        for (int i = 0; i < _cw * _ch; i++)
        {
            meshCell = _mesh->getCells(i);
            vol = meshCell->getATVolume();
            for (int e = 0; e < _ng; e++)
            {
                double flux = meshCell->getNewFlux()[e];
                abs_tot += meshCell->getSigmaA()[e] * flux * vol;
                fis_tot += meshCell->getNuSigmaF()[e] * flux * vol;
            }
        }
#if NEW
        leak_tot *= SIN_THETA_45 * P0;
#else
        leak_tot *= SIN_THETA_45 * _mesh->getCells(0)->getWidth();
#endif

        log_printf(DEBUG, "net_current_tot = %f,"
                   " leak_tot = %f", net_current_tot, leak_tot);

        double old_keff = _keff;
        _keff = fis_tot / (abs_tot + leak_tot); 
        log_printf(DEBUG, "%d: %.10f / (%.10f + %.10f) = %f", 
                   loo_iter, fis_tot, abs_tot, leak_tot, _keff);

        if (_converged)
        {
            if (_plotter->plotKeff())
                _plotter->plotCMFDKeff(_mesh, moc_iter);

            if (_plotter->plotCurrent())
                _plotter->plotNetCurrents(_mesh, moc_iter);

            break;
        }

        /* Computes the L2 norm of point-wise-division of energy-integrated
         * fission source of mesh cells between LOO iterations */
        for (int i = 0; i < _cw * _ch; i++)
        {
            meshCell = _mesh->getCells(i);
            new_power[i] = 0;
            for (int e = 0; e < _ng; e++)
            {
                new_power[i] += meshCell->getNuSigmaF()[e] 
                    * meshCell->getNewFlux()[e];
            }
        }
        
        eps = computeCellSourceNormGivenTwoSources(old_power, new_power);

        for (int i = 0; i < _cw * _ch; i++)
            old_power[i] = new_power[i];

        /* In DEBUG mode (run CMFD or LOO after MOC converges), 
         * moc_iter = 10000 */
        if (moc_iter == 10000)
            log_printf(NORMAL, " %d-th LOO iteration k = %.10f, eps = %e"
                       " k eps = %e", 
                       loo_iter, _keff, eps, (_keff - old_keff) / old_keff);
        else
            log_printf(DEBUG, " %d-th LOO iteration k = %.10f, eps = %e", 
                       loo_iter, _keff, eps);

        /* If LOO iterative solver converges */
        if ((eps < _l2_norm_conv_thresh) && (loo_iter > min_outer))
            _converged = true;

        if (loo_iter > max_outer - 3)
            _converged = true;
    } 

    _num_iter_to_conv = loo_iter;

    /* Computes the L2 norm of point-wise-division of energy-integrated
     * fission source of mesh cells relative to (m+1/2) */
    computeLooL2Norm(moc_iter);
	
    /* Cleaning up; */
    for (int i = 0; i < _cw * _ch; i++)
    {
        delete[] sum_quad_flux[i];
        delete[] quad_xs[i];
        delete[] expo[i];
        delete[] tau[i];
        delete[] new_src[i];
        delete[] new_quad_src[i];
        delete[] net_current[i];
    }
    delete[] sum_quad_flux;
    delete[] quad_xs;
    delete[] tau;
    delete[] new_src;
    delete[] new_quad_src;
    delete[] net_current;

    delete[] old_power;
    delete[] new_power;

    return _keff;
}

bool Cmfd::onAnyBoundary(int i, int surf_id)
{
    if ((surf_id == 0) && (i % _cw == 0))
        return true;
    if ((surf_id == 1) && (i >= _cw * (_ch - 1)))
        return true;
    if ((surf_id == 2) && ((i + 1) % _cw == 0))
        return true;
    if ((surf_id == 3) && (i < _cw))
        return true;
    return false;
}

/* a track $t$ in cell $i$ starts from surface surf given it is a
  forward or backward track. Technically dir is redundant as t already
  includes that */
bool Cmfd::onBoundary(int t, int i, int surf, int dir)
{
    /* dir = 0 means forward:  t = 6, 0, 2, 4 
     * dir = 1 means backward: t = 5, 7, 1, 3 */
    if ((surf == 0) && (t == 6 - dir) && (i % _cw == 0))
        return true;
    if ((surf == 1) && (t == dir * 7) && (i >= _cw * (_ch - 1)))
        return true;
    if ((surf == 2) && (t == 2 - dir) && ((i + 1) % _cw == 0))
        return true;
    if ((surf == 3) && (t == 4 - dir) && (i < _cw))
        return true;
    return false;
}

bool Cmfd::onVacuumBoundary(int t, int i, int dir)
{
    for (int s = 0; s < 4; s++)
    {
        if ((_bc[s] == VACUUM) && (onBoundary(t, i, s, dir)))
            return true;
    }
    return false;
}

int Cmfd::onReflectiveBoundary(int t, int i, int dir)
{
    for (int surf = 0; surf < 4; surf++)
    {
        if ((_bc[surf] == REFLECTIVE) && (onBoundary(t, i, surf, dir)))
            return surf;
    }
    return -1;
}

double Cmfd::computeNormalization()
{
    double fis_tot = 0, vol_tot = 0, flux = 0, vol = 0;
    MeshCell *meshCell;

    for (int i = 0; i < _cw * _ch; i++)
    {
        meshCell = _mesh->getCells(i);
        vol_tot += meshCell->getATVolume();
        for (int e = 0; e < _ng; e++)
        {
            flux = meshCell->getNewFlux()[e];
            vol = meshCell->getATVolume();
            fis_tot += meshCell->getNuSigmaF()[e] * flux * vol;
        }
    }
	
    return vol_tot / fis_tot;
}

void Cmfd::normalizeFlux(double normalize)
{
    MeshCell *meshCell;
    for (int i = 0; i < _cw * _ch; i++)
    {
        meshCell = _mesh->getCells(i);
        for (int e = 0; e < _ng; e++)
        {
            meshCell->updateNewFlux(normalize, e);
            log_printf(DEBUG, "Cell %d energy %d scalar flux update by"
                       " (after normalization) - 1 =  %e", i, e, 
                       meshCell->getNewFlux()[e] 
                       / meshCell->getOldFlux()[e] - 1.0);
        }
    }

    if (_update_boundary && _any_reflective)
    {
        int x, i;
        for (x = 0; x < _cw; x++)
        {
            i = (_ch - 1) * _cw + x;
            
            log_printf(INFO, "update cell %d, surface 1", i);
            for (int jj = 0; jj < 2; jj++)
            {
                for (int e = 0; e < _ng; e++)
                {
                    _mesh->getCells(i)->getMeshSurfaces(1)
                        ->updateQuadFlux(normalize, e, jj);
                }        
            }
        }

        int y;
        for (y = 0; y < _ch; y++)
        {
            i = y * _cw;
            log_printf(INFO, "update cell %d, surface 0", i);
            for (int e = 0; e < _ng; e++)
            {
                for (int jj = 0; jj < 2; jj++)
                {
                    _mesh->getCells(i)->getMeshSurfaces(0)
                        ->updateQuadFlux(normalize, e, jj);
                }        
            }
        }

        for (y = 0; y < _ch; y++)
        {
            i = y * _cw + _cw - 1;

            log_printf(INFO, "update cell %d, surface 2", i);
            for (int e = 0; e < _ng; e++)
            {
                for (int jj = 0; jj < 2; jj++)
                {
                    _mesh->getCells(i)->getMeshSurfaces(2)
                        ->updateQuadFlux(normalize, e, jj);
                }        
            }
        }

        for (x = 0; x < _cw; x++)
        {
            i = x;
            log_printf(INFO, "update cell %d, surface 3", i);
            for (int e = 0; e < _ng; e++)
            {
                for (int jj = 0; jj < 2; jj++)
                {
                    _mesh->getCells(i)->getMeshSurfaces(3)
                        ->updateQuadFlux(normalize, e, jj);
                }        
            }
        }
    }

    return;
}

void Cmfd::generateTrack(int *i_array, int *t_array, int *t_arrayb)
{
    int nl, nt, i;
    int len1, len2; 
    int nw = _cw;
    int i_start; 

    len1 = _num_track / 2 - 1;
    len2 = 1;
    for (nl = 0; nl < _num_loop; nl++)
    {
        i_start = (_ch - 1) * _cw + nl;

        for (nt = 0; nt < _num_track; nt++)
        {
            i = nl * _num_track + nt; 
			
            /* First side: start with 0 index, goes like 0,5,0,5,... */
            if (nt < len1)
            {
                if (nt % 2 == 1)
                    i_start += 1;
                else if (nt != 0)
                    i_start -= nw;
                i_array[i] = i_start;


                if (nt % 2 == 0)
                {
                    t_array[i] = 0;
                    t_arrayb[i] = 1;
                }
                else
                {
                    t_array[i] = 5;
                    t_arrayb[i] = 4;
                }
            }
            /* 2nd side: start with odd index, goes like 2,7,... */
            else if (nt < len1 + len2)
            {
                if (nt % 2 == 0)
                    i_start -= nw;
                else if (nt != len1)
                    i_start -= 1;
                i_array[i] = i_start;

                if (nt % 2 == 1)
                {
                    t_array[i] = 2;
                    t_arrayb[i] = 3;
                }
                else
                {
                    t_array[i] = 7;
                    t_arrayb[i] = 6;
                }
            }
            /* 3rd side: start with even index, goes like 4,1,...*/
            else if (nt < len1 + len2 + len1)
            {
                if (nt % 2 == 1)
                    i_start -= 1;
                else if (nt != len1 + len2)
                    i_start += nw;
                i_array[i] = i_start;

                if (nt % 2 == 0)
                {
                    t_array[i] = 4;
                    t_arrayb[i] = 5;
                }
                else
                {
                    t_array[i] = 1;
                    t_arrayb[i] = 0;
                }
            }	
            /* last side */
            else
            {
                if (nt % 2 == 0)
                    i_start += nw;
                else if (nt != len1 + len2 + len1)
                    i_start += 1;
                i_array[i] = i_start;


                if (nt % 2 == 1)
                {
                    t_array[i] = 6;
                    t_arrayb[i] = 7;
                }
                else
                {
                    t_array[i] = 3;
                    t_arrayb[i] = 2;
                }
            }
        }

        len1 -= 2;
        len2 += 2;
    }

    return;
}

void Cmfd::checkTrack()
{
    for (int iii = 0; iii < _num_loop; iii++)
    {
        for (int jj = 0; jj < _num_track; jj++)
            printf(" %d", _i_array[iii * _num_track + jj]);
        printf("\n");
    }
    printf("\n");
    for (int iii = 0; iii < _num_loop; iii++)
    {
        for (int jj = 0; jj < _num_track; jj++)
            printf(" %d", _t_array[iii * _num_track + jj]);
        printf("\n");
    }
    printf("\n");
    for (int iii = 0; iii < _num_loop; iii++)
    {
        for (int jj = 0; jj < _num_track; jj++)
            printf(" %d", _t_arrayb[iii * _num_track + jj]);
        printf("\n");
    }
    printf("\n");
    return;
}



/* Fill in the values in the A matrix, M matrix, and phi_old vector
 * @param A matrix
 * @param M matrix
 * @param old flux vector
 * @param solve methed - either DIFFUSION or CMFD
 * @return petsc error indicator
 */
int Cmfd::constructAMPhi(Mat A, Mat M, Vec phi_old, solveType solveMethod)
{
    /* initialized variables */
    MeshCell* meshCell;
    int petsc_err = 0;
    PetscInt indice1, indice2;
    PetscScalar value;
    double vol; 

    /* loop over mesh cells in y direction */
    for (int y = 0; y < _ch; y++)
    {
        /* loop over mesh cells in x direction */
        for (int x = 0; x < _cw; x++)
        {
            /* get mesh cell */
            meshCell = _mesh->getCells(y * _cw + x);
            vol = meshCell->getATVolume();

            /* loop over energy groups */
            for (int e = 0; e < _ng; e++)
            {
                /* this index will be used multiple times in this loop */
                indice1 = (PetscInt) ((y * _cw + x) * _ng + e);

                /* Construct flux */
                value = (PetscScalar) meshCell->getOldFlux()[e];
                petsc_err = VecSetValues(phi_old, 1, &indice1, 
                                         &value, INSERT_VALUES);
                petsc_err = VecAssemblyBegin(phi_old);
                petsc_err = VecAssemblyEnd(phi_old);
                CHKERRQ(petsc_err);

                /* diagonal - A */
                /* add absorption term to diagonals of A */
                value = (PetscScalar) meshCell->getSigmaA()[e] * vol;
                petsc_err = MatSetValues(A, 1, &indice1, 1, &indice1, &value, 
                                         ADD_VALUES);
                petsc_err = MatAssemblyBegin(A, MAT_FLUSH_ASSEMBLY);
                petsc_err = MatAssemblyEnd(A, MAT_FLUSH_ASSEMBLY);
                CHKERRQ(petsc_err);

                /* add out-scattering term to diagonals of A */
                value = 0.0;
                for (int g = 0; g < _ng; g++)
                    value += meshCell->getSigmaS()[e*_ng + g] * vol;
                petsc_err = MatSetValues(A, 1, &indice1, 1, &indice1, 
                                         &value, ADD_VALUES);
                petsc_err = MatAssemblyBegin(A, MAT_FLUSH_ASSEMBLY);
                petsc_err = MatAssemblyEnd(A, MAT_FLUSH_ASSEMBLY);
                CHKERRQ(petsc_err);

                /* add in-scattering terms to off-diagonals of A */
                for (int g = 0; g < _ng; g++)
                {
                    value = - meshCell->getSigmaS()[g*_ng + e] * vol;
                    indice2 = (y*_cw + x)*_ng + g;
                    petsc_err = MatSetValues(A, 1, &indice1, 1, &indice2, 
                                             &value, ADD_VALUES);
                    CHKERRQ(petsc_err);
                }

                /* set fission terms to M */
                for (int g = 0; g < _ng; g++)
                {
                    value = meshCell->getChi()[e] * meshCell->getNuSigmaF()[g] 
                        *vol;
                    indice2 = (y*_cw + x)*_ng + g;
                    petsc_err = MatSetValues(M, 1, &indice1, 1, &indice2, 
                                             &value, INSERT_VALUES);
                }
                CHKERRQ(petsc_err);
				
                /* RIGHT SURFACE */
                /* set transport-out term on diagonal of A */
                if (solveMethod == CMFD)
                {
                    value = (meshCell->getMeshSurfaces(2)->getDHat()[e]      
                             - meshCell->getMeshSurfaces(2)->getDTilde()[e]) 
                        * meshCell->getHeight();
                }
                else if (solveMethod == DIFFUSION)
                {
                    value = meshCell->getMeshSurfaces(2)->getDHat()[e] 
                        * meshCell->getHeight();
                }
                petsc_err = MatSetValues(A, 1, &indice1,1 , &indice1, &value, 
                                         ADD_VALUES);
                CHKERRQ(petsc_err);

                /* set transport-in terms on off diagonals */
                if (x != _cw - 1)
                {
                    if (solveMethod == CMFD)
                    {
                        value = - (meshCell->getMeshSurfaces(2)->getDHat()[e] + 
                                   meshCell->getMeshSurfaces(2)->getDTilde()[e])
                            * meshCell->getHeight();
                    }
                    else if (solveMethod == DIFFUSION)
                    {
                        value = - meshCell->getMeshSurfaces(2)->getDHat()[e] 
                            * meshCell->getHeight();
                    }
                    indice2 = (y*_cw + x + 1)*_ng + e;
                    petsc_err = MatSetValues(A, 1, &indice1, 1, &indice2, 
                                             &value, ADD_VALUES);
                }
                CHKERRQ(petsc_err);

                /* LEFT SURFACE */
                /* set transport term on diagonal */
                if (solveMethod == CMFD)
                    value = (meshCell->getMeshSurfaces(0)->getDHat()[e] + 
                             meshCell->getMeshSurfaces(0)->getDTilde()[e]) 
                        * meshCell->getHeight();
                else if (solveMethod == DIFFUSION)
                    value = meshCell->getMeshSurfaces(0)->getDHat()[e] 
                        * meshCell->getHeight();

                petsc_err = MatSetValues(A, 1, &indice1, 1, &indice1, 
                                         &value, ADD_VALUES);
                petsc_err = MatAssemblyBegin(A, MAT_FLUSH_ASSEMBLY);
                petsc_err = MatAssemblyEnd(A, MAT_FLUSH_ASSEMBLY);
                CHKERRQ(petsc_err);

                /* set transport terms on off diagonals */
                if (x != 0){
                    if (solveMethod == CMFD)
                        value = - (meshCell->getMeshSurfaces(0)->getDHat()[e] -
                                   meshCell->getMeshSurfaces(0)->getDTilde()[e])
                            * meshCell->getHeight();
                    else if (solveMethod == DIFFUSION)
                        value = - meshCell->getMeshSurfaces(0)->getDHat()[e] 
                            * meshCell->getHeight();

                    indice2 = (y*_cw + x - 1)*_ng + e;
                    petsc_err = MatSetValues(A, 1, &indice1, 1, &indice2, 
                                             &value, ADD_VALUES);
                    CHKERRQ(petsc_err);
                }

                /* BOTTOM SURFACE */
                /* set transport term on diagonal */
                if (solveMethod == CMFD)
                {
                    value = (meshCell->getMeshSurfaces(1)->getDHat()[e] 
                             -
                             meshCell->getMeshSurfaces(1)->getDTilde()[e]) 
                        * meshCell->getWidth();
                }
                else if (solveMethod == DIFFUSION)
                {
                    value = meshCell->getMeshSurfaces(1)->getDHat()[e] 
                        * meshCell->getWidth();
                }
                petsc_err = MatSetValues(A, 1, &indice1, 1, &indice1, &value, 
                                         ADD_VALUES);
                CHKERRQ(petsc_err);

                /* set transport terms on off diagonals */
                if (y != _ch - 1)
                {
                    if (solveMethod == CMFD)
                    {
                        value = - (meshCell->getMeshSurfaces(1)->getDHat()[e] 
                                   +
                                   meshCell->getMeshSurfaces(1)->getDTilde()[e])
                            * meshCell->getWidth();
                    }
                    else if (solveMethod == DIFFUSION)
                    {
                        value = - meshCell->getMeshSurfaces(1)->getDHat()[e] 
                            * meshCell->getWidth();
                    }

                    indice2 = ((y+1)*_cw + x)*_ng + e;
                    petsc_err = MatSetValues(A, 1, &indice1, 1, &indice2, 
                                             &value, ADD_VALUES);
                    CHKERRQ(petsc_err);
                }

                /* TOP SURFACE */
                /* set transport-out term on diagonal */
                if (solveMethod == CMFD)
                {
                    value = (meshCell->getMeshSurfaces(3)->getDHat()[e] 
                             +
                             meshCell->getMeshSurfaces(3)->getDTilde()[e]) 
                        * meshCell->getWidth();
                }
                else if (solveMethod == DIFFUSION)
                {
                    value = meshCell->getMeshSurfaces(3)->getDHat()[e] 
                        * meshCell->getWidth();
                }

                petsc_err = MatSetValues(A, 1, &indice1, 1, &indice1, 
                                         &value, ADD_VALUES);
                CHKERRQ(petsc_err);

                /* set transport terms on off diagonals */
                if (y != 0)
                {
                    if (solveMethod == CMFD)
                    {
                        value = - (meshCell->getMeshSurfaces(3)->getDHat()[e] 
                                   -
                                   meshCell->getMeshSurfaces(3)->getDTilde()[e])
                            * meshCell->getWidth();
                    }
                    else if (solveMethod == DIFFUSION)
                    {
                        value = - meshCell->getMeshSurfaces(3)->getDHat()[e] 
                            * meshCell->getWidth();
                    }
                    indice2 = ((y-1)*_cw + x)*_ng + e;
                    petsc_err = MatSetValues(A, 1, &indice1, 1, &indice2, 
                                             &value, ADD_VALUES);
                    CHKERRQ(petsc_err);
                }
            }
        }
    }

    return petsc_err;
}


/* Update the MOC flux in each FSR
 * @param MOC iteration number
 */
void Cmfd::updateFSRScalarFlux(int moc_iter)
{
    log_printf(INFO, "Updating MOC flux...");

    /* initialize variables */
    MeshCell* meshCell;
    FlatSourceRegion* fsr;
    double old_flux, new_flux;
    double* flux;
    double *CMCO = NULL;
    CMCO = new double[_cw * _ch];
    int i, e;
    std::vector<int>::iterator iter;
    double under_relax = 1.0;
	
    /* only apply damping here for LOO, because CMFD damps Dtilde */
    if (_run_loo)
        under_relax = _damp_factor;

    double max_range = INFINITY, min_range = -INFINITY;
    //double max_range = 100.0, min_range = 1.0 / max_range; 

    double tmp_max = 0, tmp_cmco;

    for (i = 0; i < _cw * _ch; i++)
    {
        meshCell = _mesh->getCells(i);

        CMCO[i] = 0.0;
        for (e = 0; e < _ng; e++)
        {
            old_flux = meshCell->getOldFlux()[e];
            new_flux = meshCell->getNewFlux()[e];

            tmp_cmco = new_flux / old_flux;

            // Safety precaution: if negative number shows up, force it to zero
            /*
            if (tmp_cmco < 0)
            {
                log_printf(WARNING, " iter %d update cell %d energy %d with"
                           " %f / %f = %f", moc_iter, i, e, 
                           new_flux, old_flux, tmp_cmco);
                tmp_cmco = 0;
            }
            */

            tmp_max = fabs(new_flux / old_flux - 1.0);
            CMCO[i] += tmp_max;

            /*
            if (tmp_max > max)
            {
                max = tmp_max;
                max_i = i;
                max_e = e;
            }
            */

            log_printf(DEBUG, "Flux prolongation for cell %d"
                       " old =  %f, new = %f, new/old = %f", 
                       i, old_flux, new_flux, tmp_cmco);

            /* loop over FRSs in mesh cell */
            for (iter = meshCell->getFSRs()->begin(); 
                 iter != meshCell->getFSRs()->end(); ++iter) 
            {
                fsr = &_flat_source_regions[*iter];
                flux = fsr->getFlux();

                if (moc_iter == 0)
                    fsr->setFlux(e, new_flux);
                else
                {
                    if (tmp_cmco > max_range)
                        tmp_cmco = max_range;
                    else if (tmp_cmco < min_range)
                        tmp_cmco = min_range;
                    
                    fsr->setFlux(e, under_relax * tmp_cmco * flux[e]
                                 + (1.0 - under_relax) * flux[e]);
                }
            }
        } /* exit looping over energy */
    } /* exit mesh cells */

    /* plots the scalar flux ratio */
    if (_plot_prolongation)
        _plotter->plotCmfdFluxUpdate(_mesh, moc_iter);

    delete[] CMCO;
    return;
}

void Cmfd::updateBoundaryFlux(int moc_iter)
{
    Track *track;
    segment *seg;
    FlatSourceRegion *fsr;
    MeshCell *meshCell;
    double factor;
    int num_segments, pe, num_updated = 0, meshCell_id;

    for (int i = 0; i < _num_azim; i++) 
    {
        for (int j = 0; j < _num_tracks[i]; j++)
        {
            track = &_tracks[i][j];
            num_segments = track->getNumSegments();

            /* Forward direction is 0, backward is 1 */
            for (int dir = 0; dir < 2; dir++)
            {
                /* forward gets 0, backwards get num_segments - 1 */
                seg = track->getSegment(dir * (num_segments - 1)); 
                fsr =&_flat_source_regions[seg->_region_id];
                meshCell_id = fsr->getMeshCellId();
                meshCell = _mesh->getCells(meshCell_id);
                pe = dir * GRP_TIMES_ANG;
                for (int e = 0; e < NUM_ENERGY_GROUPS; e++)
                {
                    if (meshCell->getOldFlux()[e] > 0)
                    {
                        /* Need to be 1/4pi tested in homogeneous gemetry */
                        factor = meshCell->getNewFlux()[e] / FOUR_PI; 
                        for (int p = 0; p < NUM_POLAR_ANGLES; p++)
                        {
                            track->setPolarFluxesByIndex(pe, factor);
                            pe++;
                            num_updated++;
                        }	
                    }	
                }
            }
        }
    }

    log_printf(DEBUG, "updated boundary flux by mesh cell per energy: %f", 
               num_updated / (double) NUM_ENERGY_GROUPS);
    return;
}

void Cmfd::updateBoundaryFluxByScalarFlux(int moc_iter)
{
    Track *track;
    segment *seg;
    FlatSourceRegion *fsr;
    MeshCell *meshCell;
    double factor;
    int num_segments, pe, num_updated = 0, meshCell_id;

    for (int i = 0; i < _num_azim; i++) 
    {
        for (int j = 0; j < _num_tracks[i]; j++)
        {
            track = &_tracks[i][j];
            num_segments = track->getNumSegments();

            /* Forward direction is 0, backward is 1 */
            for (int dir = 0; dir < 2; dir++)
            {
                /* forward gets 0, backwards get num_segments - 1 */
                seg = track->getSegment(dir * (num_segments - 1)); 
                fsr =&_flat_source_regions[seg->_region_id];
                meshCell_id = fsr->getMeshCellId();
                meshCell = _mesh->getCells(meshCell_id);
                pe = dir * GRP_TIMES_ANG;
                for (int e = 0; e < NUM_ENERGY_GROUPS; e++)
                {
                    if (meshCell->getOldFlux()[e] > 0)
                    {
                        factor = meshCell->getNewFlux()[e]
                            / meshCell->getOldFlux()[e];
                        log_printf(DEBUG, "factor = %.10f", factor);
                        for (int p = 0; p < NUM_POLAR_ANGLES; p++)
                        {
                            if (moc_iter == 0)
                            {
                                track->setPolarFluxesByIndex
                                    (pe, meshCell->getNewFlux()[e] 
                                     * ONE_OVER_FOUR_PI);
                            }
                            else
                                track->updatePolarFluxes(pe, factor);
                            
                            pe++;
                            num_updated++;
                        }	
                    }	
                }
            }
        }
    }

    log_printf(DEBUG, "updated boundary flux by mesh cell per energy: %f", 
               num_updated / (double) NUM_ENERGY_GROUPS);
    return;
}

void Cmfd::updateBoundaryFluxByNetCurrent(int moc_iter)
{
    Track *track;
    segment *seg;
    FlatSourceRegion *fsr;
    MeshCell *meshCell;
    double factor;
    int num_segments, pe, num_updated = 0, meshCell_id;

    for (int i = 0; i < _num_azim; i++) 
    {
        for (int j = 0; j < _num_tracks[i]; j++)
        {
            track = &_tracks[i][j];
            num_segments = track->getNumSegments();

            /* Forward direction is 0, backward is 1 */
            for (int dir = 0; dir < 2; dir++)
            {
                /* forward gets 0, backwards get num_segments - 1 */
                seg = track->getSegment(dir * (num_segments - 1)); 
                fsr =&_flat_source_regions[seg->_region_id];
                meshCell_id = fsr->getMeshCellId();
                meshCell = _mesh->getCells(meshCell_id);
                pe = dir * GRP_TIMES_ANG;
                for (int e = 0; e < NUM_ENERGY_GROUPS; e++)
                {
                    factor = meshCell->getNetCurrent()[e]
                        / meshCell->getOldNetCurrent()[e];
                    log_printf(DEBUG, "factor = %.10f", factor);
                    for (int p = 0; p < NUM_POLAR_ANGLES; p++)
                    {
                        track->updatePolarFluxes(pe, factor);
                        pe++;
                        num_updated++;
                    }	
                }
            }
        }
    }

    log_printf(DEBUG, "updated boundary flux by mesh cell per energy: %f", 
               num_updated / (double) NUM_ENERGY_GROUPS);
    return;
}

void Cmfd::updateBoundaryFluxBySrc(int moc_iter)
{
    Track *track;
    segment *seg;
    FlatSourceRegion *fsr;
    MeshCell *meshCell;
    double factor;
    int num_segments, pe, num_updated = 0, meshCell_id;

    for (int i = 0; i < _num_azim; i++) 
    {
        for (int j = 0; j < _num_tracks[i]; j++)
        {
            track = &_tracks[i][j];
            num_segments = track->getNumSegments();

            /* Forward direction is 0, backward is 1 */
            for (int dir = 0; dir < 2; dir++)
            {
                /* forward gets 0, backwards get num_segments - 1 */
                seg = track->getSegment(dir * (num_segments - 1)); 
                fsr =&_flat_source_regions[seg->_region_id];
                meshCell_id = fsr->getMeshCellId();
                meshCell = _mesh->getCells(meshCell_id);
                pe = dir * GRP_TIMES_ANG;
                for (int e = 0; e < NUM_ENERGY_GROUPS; e++)
                {
                    factor = meshCell->getSrc()[e] / meshCell->getOldSrc()[e];
                    log_printf(DEBUG, "factor = %.10f", factor);
                    for (int p = 0; p < NUM_POLAR_ANGLES; p++)
                    {
                        track->updatePolarFluxes(pe, factor);
                        pe++;
                        num_updated++;
                    }	
                }
            }
        }
    }

    log_printf(DEBUG, "updated boundary flux by mesh cell per energy: %f", 
               num_updated / (double) NUM_ENERGY_GROUPS);
    return;
}

double Cmfd::computeDiffCorrect(double d, double h){

    if (_use_diffusion_correction == false)
    {
        return 1.0;
    }
    else
    {
        /* compute correction - F */
        double alpha, mu, expon;
        double rho, F;
        rho = 0.0;
        for (int p = 0; p < NUM_POLAR_ANGLES; p++){
            mu = std::cos(std::asin(_quad->getSinTheta(p)));
            expon = exp(- h / (3 * d * mu));
            alpha = (1 + expon) / (1 - expon) - 2 * mu / h;
            rho += mu * _quad->getWeight(p) * alpha;
        }

        F = 1 + h * rho / (2 * d);
        return F;
    }

}

void Cmfd::computeLooL2Norm(int moc_iter)
{
    MeshCell *meshCell;
    double eps = 0;
    int num_counted = 0;
    double xs;
    double *old_power = NULL, *new_power = NULL;
    old_power = new double[_cw * _ch];
    new_power = new double[_cw * _ch];

    for (int i = 0; i < _cw * _ch; i++)
    {
        meshCell = _mesh->getCells(i);
        old_power[i] = 0;
        new_power[i] = 0;
        /* integrates over energy */
        for (int e = 0; e < _ng; e++)
        {
            xs = meshCell->getNuSigmaF()[e];
            old_power[i] += xs * meshCell->getOldFlux()[e];
            new_power[i] += xs * meshCell->getNewFlux()[e];
        } 
        if (old_power[i] > 0)
        {
            eps += pow(new_power[i] / old_power[i] - 1.0, 2);
            num_counted++;
        }
    }
    eps /= (double) num_counted;
    _l2_norm = pow(eps, 0.5);

    log_printf(DEBUG, " iteration %d, L2 norm of cell power error = %e", 
               moc_iter, _l2_norm);

    delete[] old_power;
    delete[] new_power;
}

/* compute the L2 norm of consecutive fission sources
 * @retun L2 norm
 */
int Cmfd::computeCmfdL2Norm(Vec snew, int moc_iter)
{
    int petsc_err = 0;
    PetscScalar *old_source;
    PetscScalar *new_source;
    petsc_err = VecGetArray(_source_old, &old_source);
    petsc_err = VecGetArray(snew, &new_source);
    CHKERRQ(petsc_err);

    _l2_norm = 0.0;
    int num_counted = 0;
    for (int i = 0; i < _cw*_ch; i++)
    {
        for (int e = 0; e < _ng; e++)
        {
            if (old_source[i * _ng + e] > 0)
            {
                _l2_norm += pow(new_source[i * _ng + e] 
                                / old_source[i * _ng + e]
                                - 1.0, 2);
            }
            num_counted ++;
        }
    }
    _l2_norm /= (double) num_counted; 
    _l2_norm = pow(_l2_norm, 0.5);

    petsc_err = VecRestoreArray(_source_old, &old_source);
    petsc_err = VecRestoreArray(snew, &new_source);
    CHKERRQ(petsc_err);

    return petsc_err;
} 

double Cmfd::getL2Norm(){
    return _l2_norm;
}

Mat Cmfd::getA(){
    return _A;
}

Mat Cmfd::getM(){
    return _M;
}

Vec Cmfd::getPhiNew(){
    return _phi_new;

}

double Cmfd::getKeff(){
    return _keff;
}

/* Store the mesh cell averaged source before a MOC sweep */
void Cmfd::storePreMOCMeshSource(FlatSourceRegion* fsrs)
{
    /* initialize variables */
    double volume, source;
    double vol_tally_cell;
    double source_tally_cell, source_tally;

    MeshCell* meshCell;
    FlatSourceRegion* fsr;

    log_printf(DEBUG, "Enter Cmfd::storePreMOCMeshSource(..)");

    /* For each mesh cell, we compute homogenized xs */
    for (int i = 0; i < _cw * _ch; i++)
    {
        meshCell = _mesh->getCells(i);

        /* Zeroes tallies for this mesh */
        source_tally = 0;

        /* Computes flux weighted xs for each energy group */
        for (int e = 0; e < NUM_ENERGY_GROUPS; e++) 
        {
            vol_tally_cell = 0;
            source_tally_cell = 0;

            /* loop over FSRs in mesh cell, accumulate cell tally */
            std::vector<int>::iterator iter;
            for (iter = meshCell->getFSRs()->begin(); 
                 iter != meshCell->getFSRs()->end(); ++iter)
            {
                fsr = &fsrs[*iter];
                volume = fsr->getVolume();
                source = fsr->getSource()[e]; 

                source_tally_cell += source * volume;
                vol_tally_cell += volume;
            } 

            /* For multi energy groups, we go ahead and set the xs for this 
             * energy group */
            if (_mesh->getMultigroup())
            {
                meshCell->setOldSrc(source_tally_cell / vol_tally_cell, e);
                log_printf(DEBUG, " cell %d Q_%d^(m) = %.10f", 
                           i, e, source_tally_cell / vol_tally_cell);
            }
            else /* For homogenized one energy group, we tally over all e's */
                source_tally += source_tally_cell;
        }

        /* For homogenized one energy group, set xs after all e's are done */
        if (_mesh->getMultigroup() == false)
            meshCell->setOldSrc(source_tally / vol_tally_cell, 0);

        log_printf(INFO, "as tracked vol / physical =  %.10f", 
                   vol_tally_cell / meshCell->getWidth() 
                   / meshCell->getHeight());
    }
    return;
}

/* Set the old flux for each FSR equal to FSR flux */
void Cmfd::setOldFSRFlux()
{
    /* initialize variables */
    FlatSourceRegion* fsr;
    int num_fsrs = _geom->getNumFSRs();

    /* Compute total fission source for this region */
    for (int r = 0; r < num_fsrs; r++) 
    {
        /* Get pointers to important data structures */
        fsr = &_flat_source_regions[r];
	  
        /* loop over energy groups */
        for (int e = 0; e < NUM_ENERGY_GROUPS; e++)
            fsr->setOldFlux(e, fsr->getFlux()[e]);
    }
}


/* set pointer to array of fsrs, give each fsr the ID of the mesh cell it is in
 * @param pointer to arrary of fsrs
 */
void Cmfd::setFSRs(FlatSourceRegion* fsrs)
{
    _flat_source_regions = fsrs;

    MeshCell *meshCell;
    std::vector<int>::iterator iter;
    FlatSourceRegion *fsr;

    // FIXME: the following line generates a bad read message in Valgrind
    for (int i = 0; i < _ch * _cw; i++)
    {
        meshCell = _mesh->getCells(i);
        for (iter = meshCell->getFSRs()->begin(); 
             iter != meshCell->getFSRs()->end(); iter++)
        {
            fsr = &_flat_source_regions[*iter];
            fsr->setMeshCellId(i);
        }
    }
    return;
} 

/* set pointer to tracks
 * @param pointer to tracks
 */
void Cmfd::setTracks(Track **tracks)
{
    _tracks = tracks;
} 

int Cmfd::getNumIterToConv()
{
    return _num_iter_to_conv;
}

