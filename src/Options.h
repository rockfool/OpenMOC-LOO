/*
 * Options.h
 *
 *  Created on: Jan 21, 2012
 *      Author: Lulu Li
 *				MIT, Course 22
 *              lululi@mit.edu
 *
 *  Stores global program options
 *
 */

#ifndef OPTIONS_H
#define OPTIONS_H

#include <string.h>
#include <stdlib.h>
#include "log.h"

class Options {
private:
	std::string _relative_path;
	std::string _geometry_file;
	std::string _material_file;
	std::string _track_input_file;
	std::string _extension;
	double _track_spacing;
	int _num_azim;
	int _bit_dimension;
	std::string _verbosity;
	bool _dump_geometry;
	bool _plot_specs;
	bool _plot_fluxes;
	bool _compute_pin_powers;
	bool _compress_cross_sections;
	bool _cmfd;
	bool _loo;
	bool _plot_quad_flux;
	bool _plot_current;
	bool _plot_diffusion;
	bool _plot_keff;
	bool _update_flux;
	bool _multigroup;
	bool _print_matrices;
	bool _diffusion;
	double _l2_norm_conv_thresh;
	int _cmfd_level;
	double _k_guess;
	bool _diffusion_correction;
public:
    Options(int argc, char **argv);
    ~Options(void);
    const char *getGeometryFile() const;
    const char *getMaterialFile() const;
    bool dumpGeometry();
    double getNumAzim();
    int getBitDimension();
    double getTrackSpacing();
    char* getVerbosity();
    std::string getExtension();
    bool plotSpecs();
    bool plotFluxes();
    bool computePinPowers();
    bool compressCrossSections();
	bool cmfd();
	bool plotQuadFlux();
	bool plotCurrent();
	bool plotDiffusion();
	bool plotKeff();
	bool updateFlux();
	double getL2NormConvThresh();
	double getKGuess();
	bool getGroupStructure();
	bool getPrintMatrices();
	int getCmfdLevel();
	bool getCmfd();
	bool getLoo();
	bool getDiffusion();
	bool getDiffusionCorrection();
};

#endif
