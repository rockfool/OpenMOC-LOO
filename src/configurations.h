/*
 * configurations.h
 *
 *  Created on: Jan 16, 2012
 *      Author: will
 */

#ifndef CONFIGURATIONS_H_
#define CONFIGURATIONS_H_

#include <math.h>

/******************************************************************************
 ****************************** USER DEFINED **********************************
 *****************************************************************************/

#define NUM_POLAR_ANGLES 3
#define NUM_ENERGY_GROUPS 7
#define GRP_TIMES_ANG NUM_POLAR_ANGLES*NUM_ENERGY_GROUPS

/* Precompute and store exponential pre-factors in transport equation */
#define STORE_PREFACTORS true

/* Max number of regions for precomputing pre-factors in solver */
#define FSR_HASHMAP_THRESH 10000

/* Number of significant digits for computing hashmap exponential prefactors */
#define FSR_HASHMAP_PRECISION 5


/******************************************************************************
 *********************** PHYSICAL CONSTANTS ***********************************
 *****************************************************************************/

#define FOUR_PI 12.5663706143
#define ONE_OVER_FOUR_PI 0.0795774715



/******************************************************************************
 *************************** ERROR THRESHOLDS *********************************
 *****************************************************************************/

/* Error threshold for determining how close a point needs to be to a surface
 * to be considered on it */
#define ON_SURFACE_THRESH 1E-12

/* Error threshold for determining how close to the boundary of a lattice cell
 * a point needs to be to be considered on it */
#define ON_LATTICE_CELL_THRESH 1E-12

/* Distance a point is moved to cross over a surface into a new cell during
 * track segmentation */
#define TINY_MOVE 1E-8



/******************************************************************************
 ************************ HELPFUL MACROS **************************************
 *****************************************************************************/

#define FLUX_INDEX(d,p,e) d*GRP_TIMES_ANG + p*NUM_ENERGY_GROUPS + e

#endif /* CONFIGURATIONS_H_ */
