/* ******************************************************************** */
/* See the file COPYRIGHT for a complete copyright notice, contact      */
/* person and disclaimer.                                               */        
/* ******************************************************************** */

/* ******************************************************************** */
/* Functions for the CG solver                                          */
/* ******************************************************************** */
/* Author        : Charles Tong (LLNL)                                  */
/* Date          : December, 1999                                       */
/* ******************************************************************** */

#ifndef __MLCG__
#define __MLCG__

#include "ml_krylov.h"

#ifdef __cplusplus
extern "C" {
#endif

extern int ML_CG_Solve(ML_Krylov *, int, double *, double *);
extern int ML_CG_ComputeEigenvalues(ML_Krylov *data, int length, int);


#ifdef __cplusplus
}
#endif
#endif

