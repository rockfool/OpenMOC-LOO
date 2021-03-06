/*
 * openmoc.cpp
 *
 *  Created on: Jan 21, 2012
 *      Author: Lulu Li
 *				MIT, Course 22
 *              lululi@mit.edu
 *
 *  This file defines the executable for OpenMOC.
 *
 */

#include "Geometry.h"
#include "TrackGenerator.h"
#include "Parser.h"
#include "Options.h"
#include "Solver.h"
#include "Timer.h"
#include "log.h"
#include "configurations.h"
#include "Plotter.h"
#include "Cmfd.h"
#include "petsc.h"
#include "mpi.h"
#include <petscmat.h>
#include <fenv.h>

// FIXME: These should be removed when main() is properly implemented
#pragma GCC diagnostic ignored "-Wunused"
#pragma GCC diagnostic ignored "-Wunused-variable"

static char help[] = "Running CMFD with Petsc\n";

int main(int argc, char **argv) {
    //feenableexcept(FE_DIVBYZERO);
    //feenableexcept(FE_UNDERFLOW);
    feenableexcept(FE_OVERFLOW);
    //feenableexcept(FE_INVALID);
    //feenableexcept(FE_INEXACT);

    double k_eff;
    Timer timer;

    /* Create an options class to parse command line options */
    Options opts(argc, argv);

    /* initialize Petsc */
    /* FIXME: Valgrine shows bad read from the help parameter */
    int petsc_err = 0;
    //PetscInitialize(&(opts.extra_argc), 
    //                &(opts.extra_argv), 
    //                (char*)0, 
    //                help);
    PetscInitializeNoArguments();
    CHKERRQ(petsc_err);

    /* Set the verbosity */
    log_setlevel(opts.getVerbosity());

    /* Initialize the parser and time the parser */
    timer.start();
    Parser parser(&opts);
    timer.stop();
    timer.recordSplit("Parsing input files");

    /* Initialize the geometry with surfaces, cells & materials */
    timer.reset();
    timer.start();
    Geometry geometry(&parser, &opts);
    timer.stop();
    timer.recordSplit("Geometry initialization");

    /* Print out geometry to console if requested at runtime*/
    if (opts.dumpGeometry())
        geometry.printString();

    /* Compress cross-sections if requested at runtime */
    if (opts.compressCrossSections())
        geometry.compressCrossSections();

    /* Initialize plotter */
    Plotter plotter(&geometry, opts.getBitDimension(), opts.getExtension(),
                    opts.plotSpecs(), opts.plotFluxes(), opts.plotCurrent(), 
                    opts.plotDiffusion(), opts.plotKeff(), opts.plotQuadFlux());

    /* Initialize track generator */
    TrackGenerator track_generator(&geometry, &plotter, &opts);

    /* Tell geometry whether CMFD is on/off */
    geometry.setCmfd(opts.getCmfd());
    geometry.setLoo(opts.getLoo());

    /* Make CMFD mesh */
    geometry.makeCMFDMesh(geometry.getMesh(), opts.getNumAzim(), 
                          opts.getGroupStructure(), opts.getPrintMatrices(),
                          opts.getCmfdLevel());

    /* make FSR map for plotting */
    if (opts.plotCurrent() || opts.plotDiffusion() || opts.plotFluxes() || 
        opts.plotSpecs())
        plotter.makeFSRMap();

    /* plot CMFD mesh */
    if (opts.plotSpecs() && opts.getCmfd())
    {
        plotter.plotCMFDMesh(geometry.getMesh());
    }

    /* Generate tracks */
    timer.reset();
    timer.start();
    track_generator.generateTracks();
    track_generator.makeReflective();
    timer.stop();
    timer.recordSplit("Generating tracks");

    track_generator.plotSpec();

    /* Create CMFD class */
    Cmfd cmfd(&geometry, &plotter, geometry.getMesh(), &track_generator, &opts);

    /* Creat Solver class */
    Solver solver(&geometry, &track_generator, &plotter, &cmfd, &opts);

    cmfd.setFSRs(solver.getFSRs());

    /* Solve steady state problem */
    if (opts.getRunAll())
    {
        solver.runCmfd();
        timer.reset();
        timer.start();
        k_eff = solver.kernel(MAX_ITERATIONS);
        timer.stop();
        timer.recordSplit("Fixed source iteration");
        log_printf(RESULT, "k_eff = %.10f", k_eff);

        geometry.setCmfd(false);
        geometry.setLoo(true);
        solver.setK(1.0);
        solver.runLoo1();
        timer.reset();
        timer.start();
        k_eff = solver.kernel(MAX_ITERATIONS);
        timer.stop();
        timer.recordSplit("Fixed source iteration");
        log_printf(RESULT, "k_eff = %.10f", k_eff);

        solver.runLoo2();
        timer.reset();
        timer.start();
        solver.setK(1.0);
        k_eff = solver.kernel(MAX_ITERATIONS);
        timer.stop();
        timer.recordSplit("Fixed source iteration");
        log_printf(RESULT, "k_eff = %.10f", k_eff);
    }
    else
    {
        timer.reset();
        timer.start();
        k_eff = solver.kernel(MAX_ITERATIONS);
        timer.stop();
        timer.recordSplit("Fixed source iteration");
        log_printf(RESULT, "k_eff = %.10f", k_eff);
    }

    /* Finalize petsc */
    PetscFinalize();
    CHKERRQ(petsc_err);

    /* Print timer splits to console */
    log_printf(NORMAL, "Program complete");
    timer.printSplits();
}
