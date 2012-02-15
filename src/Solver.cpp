/*
 * Solver.cpp
 *
 *  Created on: Feb 7, 2012
 *      Author: William Boyd
 *				MIT, Course 22
 *              wboyd@mit.edu
 */

#include "Solver.h"


/**
 * Solver constructor
 * @param geom pointer to the geometry
 * @param track_generator pointer to the trackgenerator
 */
Solver::Solver(Geometry* geom, TrackGenerator* track_generator) {
	_geom = geom;
	_quad = new Quadrature(TABUCHI);
	_num_FSRs = geom->getNumFSRs();
	_tracks = track_generator->getTracks();
	_num_tracks = track_generator->getNumTracks();
	_num_azim = track_generator->getNumAzim();

	try{
		_flat_source_regions = new FlatSourceRegion[_num_FSRs];
	}
	catch(std::exception &e) {
		log_printf(ERROR, "Could not allocate memory for the solver's flat "
					"source region array. Backtrace:%s", e.what());
	}

	/* Pre-compute exponential pre-factors */
	precomputeFactors();
	initializeFSRs();
}


/**
 * Solver destructor deletes flat source regions array
 */
Solver::~Solver() {
	delete [] _flat_source_regions;
}



/**
 * Pre-computes exponential pre-factors for each segment of each track for
 * each polar angle. This method will store each pre-factor in an array inside
 * each segment if STORE_PREFACTORS is set to true inside the configurations.h
 * file. If it is not set to true then a hashmap will be generated which will
 * contain values of the pre-factor at for specific segment lengths (the keys
 * into the hashmap).
 */
void Solver::precomputeFactors() {

	log_printf(INFO, "Pre-computing exponential pre-factors...");

	Track* curr_track;
	double azim_weight;

	/* Precompute the total azimuthal weight for tracks at each polar angle */
	for (int i = 0; i < _num_azim; i++) {
		for (int j = 0; j < _num_tracks[i]; j++) {
			curr_track = &_tracks[i][j];
			azim_weight = curr_track->getAzimuthalWeight();

			for (int p = 0; p < NUM_POLAR_ANGLES; p++)
				curr_track->setPolarWeight(p, azim_weight*_quad->getMultiple(p));
		}
	}

/*Store pre-factors inside each segment */
#if STORE_PREFACTORS

	log_printf(INFO, "Pre-factors will be stored inside each segment...");

	segment* curr_seg;

	/* Loop over azimuthal angle, track, segment, polar angle, energy group */
	for (int i = 0; i < _num_azim; i++) {
		for (int j = 0; j < _num_tracks[i]; j++) {
			curr_track = &_tracks[i][j];

			for (int s = 0; s < curr_track->getNumSegments(); s++) {
				curr_seg = curr_track->getSegment(s);

				for (int p = 0; p < NUM_POLAR_ANGLES; p++) {
					for (int e = 0; e < NUM_ENERGY_GROUPS; e++) {
						curr_seg->_prefactors[p][e] = computePreFactor(curr_seg, e, p);
					}
				}
			}
		}
	}


/* Use hash map */
#else
	log_printf(ERROR, "Table lookup for exponential pre-factors is not yet"
			"implemented. Please set STORE_PREFACTORS to TRUE in"
			" configurations.h");
#endif

	return;
}



/**
 * Function to compute the exponential prefactor for the transport equation for
 * a given segment
 * @param seg pointer to a segment
 * @param energy energy group index
 * @param angle polar angle index
 * @return the pre-factor
 */
double Solver::computePreFactor(segment* seg, int energy, int angle) {
	double* sigma_t = seg->_material->getSigmaT();
	double prefactor = 1.0 - exp (-sigma_t[energy] * seg->_length
						/ _quad->getSinTheta(angle));
	return prefactor;
}


/**
 * Compute the ratio of source / sigma_t for each energy group in each flat
 * source region for efficient fixed source iteration
 */
void Solver::computeRatios() {
	for (int i = 0; i < _num_FSRs; i++)
		_flat_source_regions[i].computeRatios();
	return;
}


/**
 * Initializes each of the FlatSourceRegion objects inside the solver's
 * array of FSRs. This includes assigning each one a unique, monotonically
 * increasing id, setting the material for each FSR, and assigning a volume
 * based on the cumulative length of all of the segments inside the FSR.
 */
void Solver::initializeFSRs() {

	log_printf(NORMAL, "Initializing FSRs...");

	CellBasic* cell;
	Material* material;
	Universe* univ_zero = _geom->getUniverse(0);
	Track* track;
	segment* seg;
	FlatSourceRegion* fsr;

	/* Set each FSR's volume by accumulating the total length of all
	   tracks inside the FSR. Loop over azimuthal angle, track and segment */
	for (int i = 0; i < _num_azim; i++) {
		for (int j = 0; j < _num_tracks[i]; j++) {
			track = &_tracks[i][j];

			for (int s = 0; s < track->getNumSegments(); s++) {
				seg = track->getSegment(s);
				fsr =&_flat_source_regions[seg->_region_id];
				fsr->incrementVolume(seg->_length);
			}
		}
	}

	/* Loop over all FSRs */
	for (int r = 0; r < _num_FSRs; r++) {
		/* Set the id */
		_flat_source_regions[r].setId(r);

		/* Get the cell corresponding to this FSR from the geometry */
		cell = static_cast<CellBasic*>(_geom->findCell(univ_zero, r));

		/* Get the cell's material and assign it to the FSR */
		material = _geom->getMaterial(cell->getMaterial());
		_flat_source_regions[r].setMaterial(material);

		log_printf(INFO, "FSR id = %d has cell id = %d and material id = %d "
				"and volume = %f", r, cell->getId(), material->getId(),
				_flat_source_regions[r].getVolume());
	}

	return;
}


/**
 * Zero each track's incoming and outgoing polar fluxes
 */
void Solver::zeroTrackFluxes() {

	log_printf(INFO, "Setting all track polar fluxes to zero...");

	double* polar_fluxes;

	/* Loop over azimuthal angle, track, polar angle, energy group
	 * and set each track's incoming and outgoing flux to zero */
	for (int i = 0; i < _num_azim; i++) {
		for (int j = 0; j < _num_tracks[i]; j++) {
			polar_fluxes = _tracks[i][j].getPolarFluxes();

			for (int i = 0; i < GRP_TIMES_ANG * 2; i++) {
				polar_fluxes[i] = 0.0;
				polar_fluxes[i] = 0.0;
			}
		}
	}
}


/**
 * Set the scalar flux for each energy group inside each FSR
 * to unity
 */
void Solver::oneFSRFluxes() {

	log_printf(INFO, "Setting all FSR scalar fluxes to unity...");

	/* Loop over all FSRs and energy groups */
	for (int r = 0; r < _num_FSRs; r++) {
		for (int e = 0; e < NUM_ENERGY_GROUPS; e++)
			_flat_source_regions->setFlux(e, 1.0);
	}

	return;
}


/**
 * Set the scalar flux for each energy group inside each FSR
 * to zero
 */
void Solver::zeroFSRFluxes() {

	log_printf(INFO, "Setting all FSR scalar fluxes to zero...");

	/* Loop over all FSRs and energy groups */
	for (int r = 0; r < _num_FSRs; r++) {
		for (int e = 0; e < NUM_ENERGY_GROUPS; e++)
			_flat_source_regions->setFlux(e, 0.0);
	}

	return;
}


/**
 * Compute k_eff from the new and old source and the value of k_eff from
 * the previous iteration
 */
void Solver::updateKeff() {

	double tot_abs = 0.0;
	double tot_fission = 0.0;
	double* sigma_a;
	double* nu_sigma_f;
	double* flux;
	Material* material;
	FlatSourceRegion* fsr;

	for (int r = 0; r < _num_FSRs; r++) {
		fsr = &_flat_source_regions[r];
		material = fsr->getMaterial();
		sigma_a = material->getSigmaA();
		nu_sigma_f = material->getNuSigmaF();
		flux = fsr->getFlux();

		for (int e = 0; e < NUM_ENERGY_GROUPS; e++) {
			tot_abs += sigma_a[e] * flux[e] * fsr->getVolume();
			tot_fission += nu_sigma_f[e] * flux[e] * fsr->getVolume();
		}
	}

	_k_eff = tot_fission/tot_abs;
	log_printf(INFO, "Computed k_eff = %f", _k_eff);

	return;
}


void Solver::fixedSourceIteration(int max_iterations) {

	Track* track;
	int num_segments;
	std::vector<segment*> segments;
	double* weights;
	segment* segment;
	double* polar_fluxes;
	double* scalar_flux;
	double* old_scalar_flux;
	double* sigma_t;
	FlatSourceRegion* fsr;
	double* ratios;
	double delta;
	double volume;

	log_printf(INFO, "Fixed source iteration with max_iterations = %d",
														max_iterations);

	/* Loop for until converged or max_iterations is reached */
	for (int i = 0; i < max_iterations; i++) {

		/* Initialize flux in each region to zero */
		zeroFSRFluxes();

		/* Loop over azimuthal angle, track */
		for (int i = 0; i < _num_azim; i++) {
			for (int j = 0; j < _num_tracks[j]; j++) {

				/* Initialize local pointers to important data structures */
				track = &_tracks[i][j];
				num_segments = track->getNumSegments();
				segments = track->getSegments();
				weights = track->getPolarWeights();
				polar_fluxes = track->getPolarFluxes();

				/* Loop over each segment in forward direction */
				for (int s = 0; s < num_segments; s++) {
					segment = segments.at(s);
					fsr = &_flat_source_regions[segment->_region_id];
					ratios = fsr->getRatios();

					/* Loop over polar angles, energy groups */
					for (int p = 0; p < NUM_POLAR_ANGLES; p++) {
						for (int e = 0; e < NUM_ENERGY_GROUPS; e++) {
							delta = (polar_fluxes[GRP_TIMES_ANG + p*NUM_ENERGY_GROUPS + e] -
									ratios[e]) * segment->_prefactors[p][e];
							fsr->incrementFlux(e, delta*weights[p]);
							polar_fluxes[GRP_TIMES_ANG + p*NUM_ENERGY_GROUPS + e] -= delta;
						}
					}
				}

				/* Transfer flux to outgoing track */
				track->getTrackOut()->setPolarFluxes(!track->isReflOut(),
													GRP_TIMES_ANG, polar_fluxes);


				/* Loop over each segment in reverse direction */
				for (int s = num_segments-1; s > -1; s--) {
					segment = segments.at(s);
					fsr = &_flat_source_regions[segment->_region_id];
					ratios = fsr->getRatios();

					/* Loop over polar angles, energy groups */
					for (int p = 0; p < NUM_POLAR_ANGLES; p++) {
						for (int e = 0; e < NUM_ENERGY_GROUPS; e++) {
							delta = (polar_fluxes[p*NUM_ENERGY_GROUPS + e] -
									ratios[e]) * segment->_prefactors[p][e];
							fsr->incrementFlux(e, delta*weights[p]);
							polar_fluxes[p*NUM_ENERGY_GROUPS + e] -= delta;
						}
					}
				}

				/* Transfer flux to incoming track */
				track->getTrackIn()->setPolarFluxes(!track->isReflIn(),
													0, polar_fluxes);
			}
		}

		/* Add in source term and normalize flux to volume for each region */
		/* Loop over flat source regions, energy groups */
		for (int r = 0; r < _num_FSRs; r++) {
			fsr = &_flat_source_regions[r];
			scalar_flux = fsr->getFlux();
			ratios = fsr->getRatios();
			sigma_t = fsr->getMaterial()->getSigmaT();
			volume = fsr->getVolume();

			for (int e = 0; e < NUM_ENERGY_GROUPS; e++) {
				fsr->setFlux(e, scalar_flux[e] / 2.0);
				fsr->setFlux(e, FOUR_PI * ratios[e] + (scalar_flux[e] /
												(sigma_t[e] * volume)));
			}
		}


		/* Check for convergence if max_iterations > 1 */
		if (max_iterations > 1) {
			bool converged = true;
			for (int r = 0; r < _num_FSRs; r++) {
				fsr = &_flat_source_regions[r];
				scalar_flux = fsr->getFlux();
				old_scalar_flux = fsr->getOldFlux();

				for (int e = 0; e < NUM_ENERGY_GROUPS; e++) {
					if (fabs(scalar_flux[e] - old_scalar_flux[e] /
							old_scalar_flux[e]) > FLUX_CONVERGENCE_THRESH )
						converged = false;

					/* Update old scalar flux */
					old_scalar_flux[e] = scalar_flux[e];
				}
			}

			if (converged)
				return;
		}
	}

	log_printf(WARNING, "Scalar flux did not converge after %d iterations",
															max_iterations);

	return;
}


double Solver::computeKeff(int max_iterations) {

	double scatter_source, fission_source;
	double renorm_factor, volume;
	double* nu_sigma_f;
	double* sigma_s;
	double* sigma_t;
	double* chi;
	double* polar_flux;
	double* scalar_flux;
	double* source;
	double* old_source;
	double* ratios;
	FlatSourceRegion* fsr;
	Material* material;

	log_printf(NORMAL, "Computing k_eff...");

	/* Initial guess */
	_k_eff_old = 1.0;

	/* Set scalar flux to unit for each region */
	oneFSRFluxes();

	/* Set the old source to unity for each Region */
	for (int r = 0; r < _num_FSRs; r++) {
		fsr = &_flat_source_regions[r];

		for (int e = 0; e < NUM_ENERGY_GROUPS; e++)
			fsr->setOldSource(e, 1.0);
	}

	// Source iteration loop
	for (int i = 0; i < max_iterations; i++) {

		log_printf(NORMAL, "Iteration: %d%", i);

		/*********************************************************************
		 * Renormalize scalar and boundary fluxes
		 *********************************************************************/

		/* Initialize fission source to zero */
		fission_source = 0;

		/* Compute total fission source to zero for this region */
		for (int r = 0; r < _num_FSRs; r++) {

			/* Get pointers to important data structures */
			fsr = &_flat_source_regions[r];
			material = fsr->getMaterial();
			nu_sigma_f = material->getNuSigmaF();
			scalar_flux = fsr->getFlux();
			volume = fsr->getVolume();

			for (int e = 0; e < NUM_ENERGY_GROUPS; e++)
				fission_source += nu_sigma_f[e] * scalar_flux[e] * volume;
		}

		/* Renormalize scalar fluxes in each region */
		renorm_factor = 1.0 / fission_source;
		log_printf(INFO, "Renormalization factor = %f\n", renorm_factor);

		for (int r = 0; r < _num_FSRs; r++) {
			fsr = &_flat_source_regions[r];
			scalar_flux = fsr->getFlux();

			for (int e = 0; e < NUM_ENERGY_GROUPS; e++)
				fsr->setFlux(e, scalar_flux[e]*renorm_factor);
		}

		/* Renormalization angular boundary fluxes for each track */
		for (int i = 0; i < _num_azim; i++) {
			for (int j = 0; j < _num_tracks[i]; j++) {
				Track* track = &_tracks[i][j];
				polar_flux = track->getPolarFluxes();

				for (int p = 0; p < NUM_POLAR_ANGLES; p++) {
					for (int e = 0; e < NUM_ENERGY_GROUPS; e++) {
						polar_flux[GRP_TIMES_ANG + p*NUM_ENERGY_GROUPS + e] *= renorm_factor;
						polar_flux[p*NUM_ENERGY_GROUPS + e] *= renorm_factor;
					}
				}
			}
		}


		/*********************************************************************
		 * Compute the source for each region
		 *********************************************************************/

		/* For all regions, find the source */
		for (int r = 0; r < _num_FSRs; r++) {

			fsr = &_flat_source_regions[r];
			material = fsr->getMaterial();

			/* Initialize the fission source to zero for this region */
			fission_source = 0;
			scalar_flux = fsr->getFlux();
			source = fsr->getSource();
			ratios = fsr->getRatios();
			material = fsr->getMaterial();
			nu_sigma_f = material->getNuSigmaF();
			sigma_t = material->getSigmaT();
			chi = material->getChi();
			sigma_s = material->getSigmaS();

			/* Compute total fission source for current region */
			for (int e = 0; e < NUM_ENERGY_GROUPS; e++)
				fission_source += scalar_flux[e] * nu_sigma_f[e];

			/* Compute total scattering source for group G */
			for (int G = 0; G < NUM_ENERGY_GROUPS; G++) {
				scatter_source = 0;

				for (int g = 0; g < NUM_ENERGY_GROUPS; g++)
					scatter_source += sigma_s[G*NUM_ENERGY_GROUPS + g] * scalar_flux[g];

				/* Set the total source for region r in group G */
				source[G] = ((1.0 / (_k_eff_old)) * fission_source * chi[G] +
								scatter_source) * ONE_OVER_FOUR_PI;
				ratios[G] = source[G] / sigma_t[G];
			}
		}

		/*********************************************************************
		 * Update flux and check for convergence
		 *********************************************************************/

		/* Iteration the flux with the new source */
		fixedSourceIteration(1);

		/* Update k_eff */
		updateKeff();

		/* If k_eff converged, return k_eff */
		if (fabs(_k_eff_old - _k_eff) < KEFF_CONVERG_THRESH)
			return _k_eff;

		/* If not converged, old k_eff and sources are updated */
		_k_eff_old = _k_eff;
		for (int r = 0; r < _num_FSRs; r++) {
			fsr = &_flat_source_regions[r];
			source = fsr->getSource();
			old_source = fsr->getOldSource();

			for (int e = 0; e < NUM_ENERGY_GROUPS; e++)
				old_source[e] = source[e];
		}
	}

	log_printf(WARNING, "Unable to converge the source after %d iterations", max_iterations);

	return INT_MAX;
}
