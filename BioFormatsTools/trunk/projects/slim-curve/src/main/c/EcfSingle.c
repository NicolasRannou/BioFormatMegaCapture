/* Based on the 2003 version of the ECF library.  This has been
   modified to remove modified Numeric Recipes code. Also, this
   takes account of the fact that we may be handling Poisson noise.

   This file contains functions for single transient analysis.
   Utility code is found in EcfUtil.c.
*/

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "EcfInternal.h"

/* Predeclarations */
int GCI_marquardt_step_instr(float xincr, float y[],
					int ndata, int fit_start, int fit_end,
					float instr[], int ninstr,
					noise_type noise, float sig[],
					float param[], int paramfree[], int nparam,
					restrain_type restrain,
					void (*fitfunc)(float, float [], float *, float [], int),
					float yfit[], float dy[],
					float **covar, float **alpha, float *chisq,
					float *alambda, int *pmfit, float *pochisq, float *paramtry, float *beta, float *dparam, 	
					float **pfnvals, float ***pdy_dparam_pure, float ***pdy_dparam_conv,
					int *pfnvals_len, int *pdy_dparam_nparam_size);

/* Globals */
static float global_chisq = 0.0f;
//static float *fnvals, **dy_dparam_pure, **dy_dparam_conv;
//static int fnvals_len=0, dy_dparam_nparam_size=0;
// was Global, now thread safe

/********************************************************************

					   SINGLE TRANSIENT FITTING

						TRIPLE INTEGRAL METHOD

 ********************************************************************/

/* Start with an easy one: the three integral method.  This returns 0
   on success, negative on error. */

int GCI_triple_integral(float xincr, float y[],
						int fit_start, int fit_end,
						noise_type noise, float sig[],
						float *Z, float *A, float *tau,
						float *fitted, float *residuals,
						float *chisq, int division)
{
	float d1, d2, d3, d12, d23;
	float t0, dt, exp_dt_tau, exp_t0_tau;
	int width;
	int i;
	float sigma2, res, chisq_local;

	width = (fit_end - fit_start) / division;
	if (width <= 0)
		return -1;

	t0 = fit_start * xincr;
	dt = width * xincr;

	d1 = d2 = d3 = 0;
	for (i=fit_start; i<fit_start+width; i++)           d1 += y[i];
	for (i=fit_start+width; i<fit_start+2*width; i++)   d2 += y[i];
	for (i=fit_start+2*width; i<fit_start+3*width; i++) d3 += y[i];

	// Those are raw sums, we now convert into areas */
	d1 *= xincr;
	d2 *= xincr;
	d3 *= xincr;

	d12 = d1 - d2;
	d23 = d2 - d3;
	if (d12 <= d23 || d23 <= 0)
		return -2;

	exp_dt_tau = d23 / d12;  /* exp(-dt/tau) */
	*tau = -dt / log(exp_dt_tau);
	exp_t0_tau = exp(-t0/(*tau));
	*A = d12 / ((*tau) * exp_t0_tau * (1 - 2*exp_dt_tau + exp_dt_tau*exp_dt_tau));
	*Z = (d1 - (*A) * (*tau) * exp_t0_tau * (1 - exp_dt_tau)) / dt;

	/* Now calculate the fitted curve and chi-squared if wanted. */
	if (fitted == NULL)
		return 0;

	for (i=0; i<fit_end; i++)
		fitted[i] = (*Z) + (*A) * exp(-i*xincr/(*tau));

	// OK, so now fitted contains our data for the timeslice of interest.
	// We can calculate a chisq value and plot the graph, along with
	// the residuals.

	if (residuals == NULL && chisq == NULL)
		return 0;

	chisq_local = 0.0;
	for (i=0; i<fit_start; i++) {
		res = y[i]-fitted[i];
		if (residuals != NULL)
			residuals[i] = res;
	}

	switch (noise) {
	case NOISE_CONST:
		/* Summation loop over all data */
		for ( ; i<fit_end; i++) {
			res = y[i] - fitted[i];
			if (residuals != NULL)
				residuals[i] = res;
			chisq_local += res * res;
		}

		chisq_local /= (sig[0]*sig[0]);
		break;

	case NOISE_GIVEN:
		/* Summation loop over all data */
		for ( ; i<fit_end; i++) {
			res = y[i] - fitted[i];
			if (residuals != NULL)
				residuals[i] = res;
			chisq_local += (res * res) / (sig[i] * sig[i]);
		}
		break;

	case NOISE_POISSON_DATA:
		/* Summation loop over all data */
		for ( ; i<fit_end; i++) {
			res = y[i] - fitted[i];
			if (residuals != NULL)
				residuals[i] = res;
			/* don't let variance drop below 1 */
			sigma2 = (y[i] > 1 ? 1.0/y[i] : 1.0);
			chisq_local += res * res * sigma2;
		}
		break;

	case NOISE_POISSON_FIT:
	case NOISE_GAUSSIAN_FIT:   //  NOISE_GAUSSIAN_FIT and NOISE_MLE not implemented for triple integral
	case NOISE_MLE:
		/* Summation loop over all data */
		for ( ; i<fit_end; i++) {
			res = y[i] - fitted[i];
			if (residuals != NULL)
				residuals[i] = res;
			/* don't let variance drop below 1 */
			sigma2 = (fitted[i] > 1 ? 1.0/fitted[i] : 1.0);
			chisq_local += res * res * sigma2;
		}
		break;

	default:
		return -3;
		/* break; */   // (unreachable code)
	}

	if (chisq != NULL)
		*chisq = chisq_local;

	return 0;
}


int GCI_triple_integral_instr(float xincr, float y[],
							  int fit_start, int fit_end,
							  float instr[], int ninstr,
							  noise_type noise, float sig[],
							  float *Z, float *A, float *tau,
							  float *fitted, float *residuals,
							  float *chisq, int division)
{
	float d1, d2, d3, d12, d23;
	float t0, dt, exp_dt_tau, exp_t0_tau;
	int width;
	int i, j;
	float sigma2, res, chisq_local;
	float sum, scaling;
	int fitted_preconv_size=0;   // was static but now thread safe
	float *fitted_preconv;   // was static but now thread safe

	width = (fit_end - fit_start) / division;
	if (width <= 0)
		return -1;

	t0 = fit_start * xincr;
	dt = width * xincr;

	d1 = d2 = d3 = 0;
	for (i=fit_start; i<fit_start+width; i++)
		d1 += y[i];
	for (i=fit_start+width; i<fit_start+2*width; i++)
		d2 += y[i];
	for (i=fit_start+2*width; i<fit_start+3*width; i++)
		d3 += y[i];

	// Those are raw sums, we now convert into areas */
	d1 *= xincr;
	d2 *= xincr;
	d3 *= xincr;

	d12 = d1 - d2;
	d23 = d2 - d3;
	if (d12 <= d23 || d23 <= 0)
		return -2;

	exp_dt_tau = d23 / d12;  /* exp(-dt/tau) */
	*tau = -dt / log(exp_dt_tau);
	exp_t0_tau = exp(-t0/(*tau));
	*A = d12 /
		  ((*tau) * exp_t0_tau * (1 - 2*exp_dt_tau + exp_dt_tau*exp_dt_tau));
	*Z = (d1 - (*A) * (*tau) * exp_t0_tau * (1 - exp_dt_tau)) / dt;

	/* We now convolve with the instrument response to hopefully get a
	   slightly better fit.  We'll also scale by an appropriate
	   scale factor, which turns out to be:

		  sum_{i=0}^{ninstr-1} instr[i]*exp(i*xincr/tau)

	   which should be only a little greater than the sum of the
	   instrument response values.
	*/

	sum = scaling = 0;
	for (i=0; i<ninstr; i++) {
		sum += instr[i];
		scaling += instr[i] * exp(i*xincr/(*tau));
	}

	scaling /= sum;  /* Make instrument response sum to 1.0 */
	(*A) /= scaling;

	/* Now calculate the fitted curve and chi-squared if wanted. */
	if (fitted == NULL)
		return 0;

//	if (fitted_preconv_size < fit_end) {
//		if (fitted_preconv_size > 0)
//			free(fitted_preconv);
		if ((fitted_preconv = (float *) malloc(fit_end * sizeof(float)))
			== NULL)
			return -3;
		else
			fitted_preconv_size = fit_end;
//	}

	for (i=0; i<fit_end; i++)
		fitted_preconv[i] = (*A) * exp(-i*xincr/(*tau));

	for (i=0; i<fit_end; i++) {
		int convpts;

		/* (Zero-basing everything in sight...)
		   We wish to find fitted = fitted_preconv * instr, so explicitly:
			  fitted[i] = sum_{j=0}^i fitted_preconv[i-j].instr[j]
		   But instr[k]=0 for k >= ninstr, so we only need to sum:
			  fitted[i] = sum_{j=0}^{min(ninstr-1,i)}
			                      fitted_preconv[i-j].instr[j]
		*/

		fitted[i] = 0;
		convpts = (ninstr <= i) ? ninstr-1 : i;
		for (j=0; j<=convpts; j++) {
			fitted[i] += fitted_preconv[i-j]*instr[j];
		}

		fitted[i] += *Z;
	}

	// OK, so now fitted contains our data for the timeslice of interest.
	// We can calculate a chisq value and plot the graph, along with
	// the residuals.

	if (residuals == NULL && chisq == NULL)
		return 0;

	chisq_local = 0.0;
	for (i=0; i<fit_start; i++) {
		res = y[i]-fitted[i];
		if (residuals != NULL)
			residuals[i] = res;
	}

	switch (noise) {
	case NOISE_CONST:
		/* Summation loop over all data */
		for ( ; i<fit_end; i++) {
			res = y[i] - fitted[i];
			if (residuals != NULL)
				residuals[i] = res;
			chisq_local += res * res;
		}

		chisq_local /= (sig[0]*sig[0]);
		break;

	case NOISE_GIVEN:
		/* Summation loop over all data */
		for ( ; i<fit_end; i++) {
			res = y[i] - fitted[i];
			if (residuals != NULL)
				residuals[i] = res;
			chisq_local += (res * res) / (sig[i] * sig[i]);
		}
		break;

	case NOISE_POISSON_DATA:
		/* Summation loop over all data */
		for ( ; i<fit_end; i++) {
			res = y[i] - fitted[i];
			if (residuals != NULL)
				residuals[i] = res;
			/* don't let variance drop below 1 */
			sigma2 = (y[i] > 1 ? 1.0/y[i] : 1.0);
			chisq_local += res * res * sigma2;
		}
		break;

	case NOISE_POISSON_FIT:
	case NOISE_GAUSSIAN_FIT:   //  NOISE_GAUSSIAN_FIT and NOISE_MLE not implemented for triple integral
	case NOISE_MLE:
		/* Summation loop over all data */
		for ( ; i<fit_end; i++) {
			res = y[i] - fitted[i];
			if (residuals != NULL)
				residuals[i] = res;
			/* don't let variance drop below 1 */
			sigma2 = (fitted[i] > 1 ? 1.0/fitted[i] : 1.0);
			chisq_local += res * res * sigma2;
		}
		break;

	default:
		return -3;
		/* break; */   // (unreachable code)
	}

	if (chisq != NULL)
		*chisq = chisq_local;

	return 0;
}

int GCI_triple_integral_fitting_engine(float xincr, float y[], int fit_start, int fit_end,
							  float instr[], int ninstr, noise_type noise, float sig[],
							  float *Z, float *A, float *tau, float *fitted, float *residuals,
							  float *chisq, float chisq_target)
{
	int tries=1, division=3;		 // the data 
	float local_chisq=3.0e38, oldChisq=3.0e38, oldZ, oldA, oldTau, *validFittedArray; // local_chisq a very high float but below oldChisq
		
	if (fitted==NULL)   // we require chisq but have not supplied a "fitted" array so must malloc one
	{
		if ((validFittedArray = malloc(fit_end * sizeof(float)))== NULL) return (-1);
	}
	else validFittedArray = fitted;
	
	if (instr==NULL)           // no instrument/prompt has been supplied
	{
		GCI_triple_integral(xincr, y, fit_start, fit_end, noise, sig,
								Z, A, tau, validFittedArray, residuals, &local_chisq, division);

		while (local_chisq>chisq_target && (local_chisq<=oldChisq) && tries<MAXREFITS)	 
		{
			oldChisq = local_chisq;
			oldZ = *Z;
			oldA = *A;
			oldTau = *tau;
//			division++;			 
			division+=division/3;			 
			tries++;
			GCI_triple_integral(xincr, y, fit_start, fit_end, noise, sig,
								Z, A, tau, validFittedArray, residuals, &local_chisq, division);
		}						  
	}
	else
	{
		GCI_triple_integral_instr(xincr, y, fit_start, fit_end, instr, ninstr, noise, sig,
								Z, A, tau, validFittedArray, residuals, &local_chisq, division);

		while (local_chisq>chisq_target && (local_chisq<=oldChisq) && tries<MAXREFITS)	 
		{
			oldChisq = local_chisq; 
			oldZ = *Z;
			oldA = *A;
			oldTau = *tau;
//			division++;			 
			division+=division/3;
			tries++;
			GCI_triple_integral_instr(xincr, y, fit_start, fit_end, instr, ninstr, noise, sig,
								Z, A, tau, validFittedArray, residuals, &local_chisq, division);

		}						  
	}

	if (local_chisq>oldChisq) 	   // the previous fit was better
	{
		local_chisq = oldChisq;
		*Z = oldZ;
		*A = oldA;
		*tau = oldTau;
	}
	
	if (chisq!=NULL) *chisq = local_chisq;

	if (fitted==NULL)  
	{
		free (validFittedArray);
	}

	return(tries);
}

/********************************************************************

					   SINGLE TRANSIENT FITTING

					  LEVENBERG-MARQUARDT METHOD

 ********************************************************************/

/* Now for the non-linear least squares fitting algorithms.

   The process is:
   - for Gaussian noise, use Levenberg-Marquardt directly
   - for Poisson noise, use Levenberg-Marquardt to get an initial
   estimate of the parameters assuming constant error variance, then
   use amoeba to improve the estimate, assuming that the error
   variance is proportional to the function value (with a minimum
   variance of 15 to handle the case when the Poisson distribution
   is not approximately Gaussian, so that the very noisy tails do
   not inappropriately weight the solution).


   This code contains two variants of the Levenberg-Marquardt method
   for slightly different situations.  This attempts to reduce the
   value chi^2 of a fit between a set of data points x[0..ndata-1],
   y[0..ndata-1] and a nonlinear function dependent on nparam
   coefficients param[0..nparam-1].  In the case that the x values are
   equally spaced and start at zero, we can also handle convolution
   with an instrument response instr[0..ninstr-1] and only look at the
   data points from fit_start..fit_end-1.  The first variant does not
   handle an instrument response and takes any values of
   x[0..ndata-1].  The second variant takes an xincr and will handle
   an instrument response if ninstr > 0.  The individual standard
   deviations of the errors are determined by the value of noise: if
   noise=NOISE_CONST, the standard deviations are constant, given by
   sig[0]=*sig, if noise=NOISE_GIVEN, the standard deviations are
   given by sig[0..ndata-1], if noise=NOISE_POISSON_DATA, the standard
   deviations are taken to be given by Poisson noise, and the
   variances are taken to be max(y[i],15), and if
   noise=NOISE_POISSON_FIT, the variances are taken to be
   max(yfit[i],15). If noise=NOISE_GAUSSIAN_FIT, the variances are taken to be
   yfit[i] and if noise=NOISE_MLE then a optimisation is for the maximum 
   likelihood approximation (Laurence and Chromy in press 2010 and 
   G. Nishimura, and M. Tamura Phys Med Biol 2005).
   
   The input array paramfree[0..nparam-1] indicates
   by nonzero entries those components that should be held fixed at
   their input values. The program returns current best-fit values for
   the parameters param[0..nparam-1] and chi^2 = chisq.  The arrays
   covar[0..nparam-1][0..nparam-1] and alpha[0..nparam-1][0..nparam-1]
   are used as working space during most isterations.  Supply a
   routine fitfunc(x,param,yfit,dy_dparam,nparam) that evaluates the
   fitting function fitfunc and its derivatives dydy[1..nparam-1] with
   respect to the fitting parameters param at x.  (See below for
   information about zero offsets, though.)  The values of fitfunc,
   modified by the instrument response, are returned in the array yfit
   and the differences y - yfit in dy.  The first call _must_ provide
   an initial guess for the parameters param and set alambda < 0 for
   initialisation (which then sets alambda = 0.001).  If a step
   succeeds, chisq becomes smaller and alambda decreases by a factor
   of 10.  You must call this routine repeatedly until convergence is
   achieved. Then make one final call with alambda=0 to perform
   cleanups and so that covar[0..nparam-1][0..nparam-1] returns the
   covariance matrix and alpha the curvature matrix.  (Parameters held
   fixed will return zero covariances.)

   One key extra piece which is particularly important in the
   instrument response case.  The parameter param[0] is assumed to be
   the zero offset of the signal, which applies before and after time
   zero.  It thus simply contributes param[0]*sum(instr) to the signal
   value rather than being convolved with the instrument response only
   from time zero.  For this reason, the fitfunc should ignore
   param[0], as the fitting routines will handle this offset.
*/

/* This functions does the whole job */

int GCI_marquardt(float x[], float y[], int ndata,
				  noise_type noise, float sig[],
				  float param[], int paramfree[], int nparam,
				  restrain_type restrain,
				  void (*fitfunc)(float, float [], float *, float [], int),
				  float *fitted, float *residuals,
				  float **covar, float **alpha, float *chisq,
				  float chisq_delta, float chisq_percent, float **erraxes)
{
	// Empty fn to allow compile in TRI2
}

#define do_frees \
	if (fnvals) free(fnvals);\
	if (dy_dparam_pure) GCI_ecf_free_matrix(dy_dparam_pure);\
	if (dy_dparam_conv) GCI_ecf_free_matrix(dy_dparam_conv);

int GCI_marquardt_instr(float xincr, float y[],
					int ndata, int fit_start, int fit_end,
					float instr[], int ninstr,
					noise_type noise, float sig[],
					float param[], int paramfree[], int nparam,
					restrain_type restrain,
					void (*fitfunc)(float, float [], float *, float [], int),
					float *fitted, float *residuals,
					float **covar, float **alpha, float *chisq,
					float chisq_delta, float chisq_percent, float **erraxes)
{
	float alambda, ochisq;
	int mfit, mfit2;
	float evals[MAXFIT];
	int i, k, itst, itst_max;

	// The following are declared here to retain some optimisation by not repeatedly mallocing
	// (only once per transient), but to remain thread safe.
	// They are malloced by lower fns but at the end, freed by this fn.
	// These vars were global or static before thread safety was introduced.
	float *fnvals=NULL, **dy_dparam_pure=NULL, **dy_dparam_conv=NULL;
	int fnvals_len=0, dy_dparam_nparam_size=0;
	float ochisq2, paramtry[MAXFIT], beta[MAXFIT], dparam[MAXFIT]; 

	itst_max = (restrain == ECF_RESTRAIN_DEFAULT) ? 4 : 6;

	mfit = 0;
	for (i=0; i<nparam; i++) {
		if (paramfree[i]) mfit++;
	}

	if (ecf_exportParams) ecf_ExportParams (param, nparam, *chisq);

	alambda = -1;
	if (GCI_marquardt_step_instr(xincr, y, ndata, fit_start, fit_end,
								 instr, ninstr, noise, sig,
								 param, paramfree, nparam, restrain,
								 fitfunc, fitted, residuals,
								 covar, alpha, chisq, &alambda, 
								 &mfit2, &ochisq2, paramtry, beta, dparam,
								 &fnvals, &dy_dparam_pure, &dy_dparam_conv,
								 &fnvals_len, &dy_dparam_nparam_size) != 0) {
		do_frees
		return -1;
	}

	if (ecf_exportParams) ecf_ExportParams (param, nparam, *chisq);

	k = 1;  /* Iteration counter */
	itst = 0;
	for (;;) {
		k++;
		if (k > MAXITERS) {
			do_frees
			return -2;
		}

		ochisq = *chisq;
		if (GCI_marquardt_step_instr(xincr, y, ndata, fit_start, fit_end,
									 instr, ninstr, noise, sig,
									 param, paramfree, nparam, restrain,
									 fitfunc, fitted, residuals,
									 covar, alpha, chisq, &alambda, 
									 &mfit2, &ochisq2, paramtry, beta, dparam,
									 &fnvals, &dy_dparam_pure, &dy_dparam_conv,
									 &fnvals_len, &dy_dparam_nparam_size) != 0) {
			do_frees
			return -3;
		}

		if (ecf_exportParams) ecf_ExportParams (param, nparam, *chisq);

		if (*chisq > ochisq)
			itst = 0;
		else if (ochisq - *chisq < chisq_delta)
			itst++;

		if (itst < itst_max) continue;

		/* Endgame */
		alambda = 0.0;
		if (GCI_marquardt_step_instr(xincr, y, ndata, fit_start, fit_end,
									 instr, ninstr, noise, sig,
									 param, paramfree, nparam, restrain,
									 fitfunc, fitted, residuals,
									 covar, alpha, chisq, &alambda, 
									 &mfit2, &ochisq2, paramtry, beta, dparam,
									 &fnvals, &dy_dparam_pure, &dy_dparam_conv,
									 &fnvals_len, &dy_dparam_nparam_size) != 0) {
			do_frees
			return -4;
		}

		if (erraxes == NULL){
			do_frees
			return k;
		}

		break;  /* We're done now */
	}

	do_frees
	return k;
}
#undef do_frees


int GCI_gauss_jordan(float **a, int n, float *b);

int GCI_marquardt_step_instr(float xincr, float y[],
					int ndata, int fit_start, int fit_end,
					float instr[], int ninstr,
					noise_type noise, float sig[],
					float param[], int paramfree[], int nparam,
					restrain_type restrain,
					void (*fitfunc)(float, float [], float *, float [], int),
					float yfit[], float dy[],
					float **covar, float **alpha, float *chisq,
					float *alambda, int *pmfit, float *pochisq, float *paramtry, float *beta, float *dparam, 	
					float **pfnvals, float ***pdy_dparam_pure, float ***pdy_dparam_conv,
					int *pfnvals_len, int *pdy_dparam_nparam_size)
{
	int j, k, l, ret;
//	static int mfit;   // was static but now thread safe
//	static float ochisq, paramtry[MAXFIT], beta[MAXFIT], dparam[MAXFIT];   // was static but now thread safe

	/*if (nparam > MAXFIT) {
            printf("GCI_marq_step_instr returns -10\n");
		return -10;
        }
	if (xincr <= 0) {
            printf("GCI_marq_step_instr returns -11\n");
		return -11;
        }
	if (fit_start < 0 || fit_start > fit_end || fit_end > ndata) {
            printf("GCI_marq_step_instr returns -12\n");
		return -12;
        }*/

	/* Initialisation */
	/* We assume we're given sensible starting values for param[] */
	if (*alambda < 0.0) {

		for ((*pmfit)=0, j=0; j<nparam; j++)
			if (paramfree[j])
				(*pmfit)++;
    global_chisq = 0.0f;
		if (GCI_marquardt_compute_fn_instr(xincr, y, ndata, fit_start, fit_end,
										   instr, ninstr, noise, sig,
										   param, paramfree, nparam, fitfunc,
										   yfit, dy, alpha, beta, chisq,
										   *alambda,	
											pfnvals, pdy_dparam_pure, pdy_dparam_conv,
											pfnvals_len, pdy_dparam_nparam_size) != 0) {
			return -2;
                }

		*alambda = 0.001;
		*pochisq = *chisq;
		for (j=0; j<nparam; j++)
			paramtry[j] = param[j];

	}

	/* Alter linearised fitting matrix by augmenting diagonal elements */
	for (j=0; j<(*pmfit); j++) {
		for (k=0; k<(*pmfit); k++)
			covar[j][k] = alpha[j][k];

		covar[j][j] = alpha[j][j] * (1.0 + (*alambda));
		dparam[j] = beta[j];
	}

	/* Matrix solution; GCI_gauss_jordan solves Ax=b rather than AX=B */
	if (GCI_gauss_jordan(covar, *pmfit, dparam) != 0) {
		return -1;
        }
        

	/* Once converged, evaluate covariance matrix */
	if (*alambda == 0) {
		if (GCI_marquardt_compute_fn_final_instr(
									xincr, y, ndata, fit_start, fit_end,
									instr, ninstr, noise, sig,
									param, paramfree, nparam, fitfunc,
									yfit, dy, chisq,	
									pfnvals, pdy_dparam_pure, pdy_dparam_conv,
									pfnvals_len, pdy_dparam_nparam_size) != 0) {
			return -3;
                }
		if (*pmfit < nparam) {  /* no need to do this otherwise */
			GCI_covar_sort(covar, nparam, paramfree, *pmfit);
			GCI_covar_sort(alpha, nparam, paramfree, *pmfit);
		}
		return 0;
	}

	/* Did the trial succeed? */
	for (j=0, l=0; l<nparam; l++)
		if (paramfree[l])
			paramtry[l] = param[l] + dparam[j++];

	if (restrain == ECF_RESTRAIN_DEFAULT)
		ret = check_ecf_params (paramtry, nparam, fitfunc);
	else
		ret = check_ecf_user_params (paramtry, nparam, fitfunc);

	if (ret != 0) {
		/* Bad parameters, increase alambda and return */
		*alambda *= 10.0;
		return 0;
	}
    global_chisq = *pochisq; //TODO this is a cheap mechanism to "pass in" the original chisq by using a global variable; seems to be a valid optimization, within this function don't setup matrices if chisq is not an improvment
	if (GCI_marquardt_compute_fn_instr(xincr, y, ndata, fit_start, fit_end,
									   instr, ninstr, noise, sig,
									   paramtry, paramfree, nparam, fitfunc,
									   yfit, dy, covar, dparam,
									   chisq, *alambda,	
									   pfnvals, pdy_dparam_pure, pdy_dparam_conv,
									   pfnvals_len, pdy_dparam_nparam_size) != 0) {
		return -2;
        }

	/* Success, accept the new solution */
	if (*chisq < *pochisq) {
		*alambda *= 0.1;
		*pochisq = *chisq;
		for (j=0; j<(*pmfit); j++) {
			for (k=0; k<(*pmfit); k++)
				alpha[j][k] = covar[j][k];
			beta[j] = dparam[j];
		}
		for (l=0; l<nparam; l++)
			param[l] = paramtry[l];
	} else { /* Failure, increase alambda and return */
		*alambda *= 10.0;
		*chisq = *pochisq;
	}

	return 0;
}


/* Used by GCI_marquardt to evaluate the linearised fitting matrix alpha
   and vector beta and to calculate chi^2.	The equations involved are
   given in section 15.5 of Numerical Recipes; basically:

     \chi^2(param) = \sum_{i=1}^N ( (y_i-y(x_i;param)) / sigma_i )^2

     beta_k = -1/2 (d/dparam_k)(chi^2)

     alpha_kl = \sum_{i=1}^N (1/sigma_i^2) .
                  (dy(x_i;param)/dparam_k) . (dy(x_i;param)/dparam_l)

   (where all of the derivatives are partial).

   If an instrument response is provided, we also take account of it
   now.	 We are given that:

     observed(t) = response(t) * instr(t)

   where response(t) is being fitted with fitfunc; it is also trivial to
   show that (assuming that instr(t) is known and fixed, with no
   dependencies on the param_k, the parameters being fitted):

     (d/dparam_k) observed(t) = ((d/dparam_k) response(t)) * instr(t)

   so we do not need to alter the response function in any way to
   determined the fitted convolved response.

   This is the variant which handles an instrument response. */

/* We assume that the function values are sensible. */

int GCI_marquardt_compute_fn_instr(float xincr, float y[], int ndata,
				   int fit_start, int fit_end,
				   float instr[], int ninstr,
				   noise_type noise, float sig[],
				   float param[], int paramfree[], int nparam,
				   void (*fitfunc)(float, float [], float *, float [], int),
				   float yfit[], float dy[],
				   float **alpha, float beta[], float *chisq,
				   float alambda,
					float **pfnvals, float ***pdy_dparam_pure, float ***pdy_dparam_conv,
					int *pfnvals_len, int *pdy_dparam_nparam_size)
{
	int i, j, k, l, m, mfit, ret;
	float wt, sig2i, y_ymod;

	/* Are we initialising? */
	// Malloc the arrays that will get used again in this fit via the pointers passed in
	// They will be freed by the higher fn that declared them.
	if (alambda < 0) {
		/* do any necessary initialisation */
		if ((*pfnvals_len) < ndata) {  /* we will need this length for
									  the final full computation */
			if ((*pfnvals_len)) {
				free((*pfnvals));
				GCI_ecf_free_matrix((*pdy_dparam_pure));
				GCI_ecf_free_matrix((*pdy_dparam_conv));
				(*pfnvals_len) = 0;
				(*pdy_dparam_nparam_size) = 0;
			}
		} else if ((*pdy_dparam_nparam_size) < nparam) {
			GCI_ecf_free_matrix((*pdy_dparam_pure));
			GCI_ecf_free_matrix((*pdy_dparam_conv));
			(*pdy_dparam_nparam_size) = 0;
		}
		if (! (*pfnvals_len)) {
			if (((*pfnvals) = (float *) malloc(ndata * sizeof(float)))
				== NULL)
				return -1;
			(*pfnvals_len) = ndata;
		}
		if (! (*pdy_dparam_nparam_size)) {
			/* use (*pfnvals_len), not ndata here! */
			if (((*pdy_dparam_pure) = GCI_ecf_matrix((*pfnvals_len), nparam)) == NULL) {
				/* Can't keep (*pfnvals) around in this case */
				free((*pfnvals));
				(*pfnvals_len) = 0;
				return -1;
			}
			if (((*pdy_dparam_conv) = GCI_ecf_matrix((*pfnvals_len), nparam)) == NULL) {
				/* Can't keep (*pfnvals) around in this case */
				free((*pfnvals));
				free((*pdy_dparam_pure));
				(*pfnvals_len) = 0;
				return -1;
			}
			(*pdy_dparam_nparam_size) = nparam;
		}
	}

	for (j=0, mfit=0; j<nparam; j++)
		if (paramfree[j]) mfit++;

	/* Calculation of the fitting data will depend upon the type of
	   noise and the type of instrument response */

	/* Need to calculate unconvolved values all the way down to 0 for
	   the instrument response case */
	if (ninstr > 0) {
		if (fitfunc == GCI_multiexp_lambda)
			ret = multiexp_lambda_array(xincr, param, (*pfnvals),
										(*pdy_dparam_pure), fit_end, nparam);
		else if (fitfunc == GCI_multiexp_tau)
			ret = multiexp_tau_array(xincr, param, (*pfnvals),
									 (*pdy_dparam_pure), fit_end, nparam);
		else if (fitfunc == GCI_stretchedexp)
			ret = stretchedexp_array(xincr, param, (*pfnvals),
									 (*pdy_dparam_pure), fit_end, nparam);
		else
			ret = -1;

		if (ret < 0)
			for (i=0; i<fit_end; i++)
				(*fitfunc)(xincr*i, param, &(*pfnvals)[i],
						   (*pdy_dparam_pure)[i], nparam);

		/* OK, we've got to convolve the model fit with the given
		   instrument response.	 What we'll do here, then, is to
		   generate the whole model fit first, then do the convolution
		   with the instrument response.  Only after doing that will
		   we attempt to calculate the goodness of fit matrices.  This
		   means that we will be looping through all of the points
		   twice, which is not worth it if there is no convolution
		   necessary. */

		for (i=fit_start; i<fit_end; i++) {
			int convpts;

			/* We wish to find yfit = (*pfnvals) * instr, so explicitly:
			     yfit[i] = sum_{j=0}^i (*pfnvals)[i-j].instr[j]
			   But instr[k]=0 for k >= ninstr, so we only need to sum:
			     yfit[i] = sum_{j=0}^{min(ninstr-1,i)}
			   (*pfnvals)[i-j].instr[j]
			*/

			/* Zero our adders */
			yfit[i] = 0;
			for (k=1; k<nparam; k++)
				(*pdy_dparam_conv)[i][k] = 0;

			convpts = (ninstr <= i) ? ninstr-1 : i;
			for (j=0; j<=convpts; j++) {
				yfit[i] += (*pfnvals)[i-j] * instr[j];
				for (k=1; k<nparam; k++)
					(*pdy_dparam_conv)[i][k] += (*pdy_dparam_pure)[i-j][k] * instr[j];
			}
		}
	} else {
		/* Can go straight into the final arrays in this case */
		if (fitfunc == GCI_multiexp_lambda)
			ret = multiexp_lambda_array(xincr, param, yfit,
										(*pdy_dparam_conv), fit_end, nparam);
		else if (fitfunc == GCI_multiexp_tau)
			ret = multiexp_tau_array(xincr, param, yfit,
									 (*pdy_dparam_conv), fit_end, nparam);
		else if (fitfunc == GCI_stretchedexp)
			ret = stretchedexp_array(xincr, param, yfit,
									 (*pdy_dparam_conv), fit_end, nparam);
		else
			ret = -1;

		if (ret < 0)
			for (i=0; i<fit_end; i++)
				(*fitfunc)(xincr*i, param, &yfit[i],
						   (*pdy_dparam_conv)[i], nparam);
	}

	/* OK, now we've got our (possibly convolved) data, we can do the
	   rest almost exactly as above. */
	{
        float alpha_weight[256]; //TODO establish maximum # bins and use elsewhere (#define)
        float beta_weight[256];
        int q;
        float weight;

        int i_free;
        int j_free;
        float dot_product;
        float beta_sum;
        float dy_dparam_k_i;
        float *alpha_weight_ptr;
        float *beta_weight_ptr;

        *chisq = 0.0f;

		switch (noise) {
            case NOISE_CONST:
            {
                float *alpha_ptr = &alpha_weight[fit_start];
                float *beta_ptr = &beta_weight[fit_start];
                for (q = fit_start; q < fit_end; ++q) {
                    (*pdy_dparam_conv)[q][0] = 1.0f;
                    yfit[q] += param[0];
                    dy[q] = y[q] - yfit[q];
                    weight = 1.0f;
                    *alpha_ptr++ = weight; //
                    //alpha_weight[q] = weight; // 1
                    weight *= dy[q];
                    //printf("beta weight %d %f is y %f - yfit %f\n", q, weight, y[q], yfit[q]);
                    *beta_ptr++ = weight; //
                    //beta_weight[q] = weight; // dy[q]
                    weight *= dy[q];
                    *chisq += weight; // dy[q] * dy[q]
                }
                break;
            }
            case NOISE_GIVEN:
            {
                for (q = fit_start; q < fit_end; ++q) {
                    (*pdy_dparam_conv)[q][0] = 1.0f;
                    yfit[q] += param[0];
                    dy[q] = y[q] - yfit[q];
                    weight = 1.0f / (sig[q] * sig[q]);
                    alpha_weight[q] = weight; // 1 / (sig[q] * sig[q])
                    weight *= dy[q];
                    beta_weight[q] = weight; // dy[q] / (sig[q] * sig[q])
                    weight *= dy[q];
                    *chisq += weight; // (dy[q] * dy[q]) / (sig[q] * sig[q])
                }
                break;
            }
            case NOISE_POISSON_DATA:
            {
                for (q = fit_start; q < fit_end; ++q) {
                    (*pdy_dparam_conv)[q][0] = 1.0f;
                    yfit[q] += param[0];
                    dy[q] = y[q] - yfit[q];
                    weight = (y[q] > 15 ? 1.0f / y[q] : 1.0f / 15);
                    alpha_weight[q] = weight; // 1 / sig(q)
                    weight *= dy[q];
                    beta_weight[q] = weight; // dy[q] / sig(q)
                    weight *= dy[q];
                    *chisq += weight; // (dy[q] * dy[q]) / sig(q)
                }
                break;
            }
            case NOISE_POISSON_FIT:
            {
                for (q = fit_start; q < fit_end; ++q) {
                    (*pdy_dparam_conv)[q][0] = 1.0f;
                    yfit[q] += param[0];
                    dy[q] = y[q] - yfit[q];
                    weight = (yfit[q] > 15 ? 1.0f / yfit[q] : 1.0f / 15);
                    alpha_weight[q] = weight; // 1 / sig(q)
                    weight *= dy[q];
                    beta_weight[q] = weight; // dy(q) / sig(q)
                    weight *= dy[q];
                    *chisq += weight; // (dy(q) * dy(q)) / sig(q)
                }
                break;
            }
            case NOISE_GAUSSIAN_FIT:
            {
                 for (q = fit_start; q < fit_end; ++q) {
                    (*pdy_dparam_conv)[q][0] = 1.0f;
                    yfit[q] += param[0];
                    dy[q] = y[q] - yfit[q];
                    weight = (yfit[q] > 1.0f ? 1.0f / yfit[q] : 1.0f);
                    alpha_weight[q] = weight; // 1 / sig(q)
                    weight *= dy[q];
                    beta_weight[q] = weight; // dy[q] / sig(q)
                    weight *= dy[q];
                    *chisq += weight;
                 }
                 break;
           }
            case NOISE_MLE:
            {
                for (q = fit_start; q < fit_end; ++q) {
                    (*pdy_dparam_conv)[q][0] = 1.0f;
                    yfit[q] += param[0];
                    dy[q] = y[q] - yfit[q];
                    weight = (yfit[q] > 1 ? 1.0f / yfit[i] : 1.0f);
                    alpha_weight[q] = weight * y[q] / yfit[q]; //TODO was y_ymod = y[i]/yfit[i], but y_ymod was float, s/b modulus?
                    beta_weight[q] = dy[q] * weight;
                    *chisq += (0.0f == y[q])
                            ? 2.0 * yfit[q]
                            : 2.0 * (yfit[q] - y[q]) - 2.0 * y[q] * log(yfit[q] / y[q]);
                }
	        if (*chisq <= 0.0f) {
                    *chisq = 1.0e38f; // don't let chisq=0 through yfit being all -ve
                }
                break;
            }
            default:
            {
                return -3;
            }
        }

        // Check if chi square worsened:
        if (0.0f != global_chisq && *chisq >= global_chisq) { //TODO pass in the old chi square as a parameter
            // don't bother to set up the matrices for solution
            return 0;
        }

        i_free = 0;
        // for all columns
        for (i = 0; i < nparam; ++i) {
            if (paramfree[i]) {
                j_free = 0;
                beta_sum = 0.0f;
                // row loop, only need to consider lower triangle
                for (j = 0; j <= i; ++j) {
                    if (paramfree[j]) {
                        dot_product = 0.0f;
                              alpha_weight_ptr = &alpha_weight[fit_start];
                        if (0 == j_free) {
                            beta_weight_ptr = &beta_weight[fit_start];
                            // for all data
                            for (k = fit_start; k < fit_end; ++k) {
                                dy_dparam_k_i = (*pdy_dparam_conv)[k][i];
                                dot_product += dy_dparam_k_i * (*pdy_dparam_conv)[k][j] * (*alpha_weight_ptr++); //alpha_weight[k]; //TODO make it [i][k] and just *dy_dparam++ it.
                                beta_sum += dy_dparam_k_i * (*beta_weight_ptr++); ///beta_weight[k];
                            }
                        }
                        else {
                            // for all data
                            for (k = fit_start; k < fit_end; ++k) {
                                dot_product += (*pdy_dparam_conv)[k][i] * (*pdy_dparam_conv)[k][j] * (*alpha_weight_ptr++); // alpha_weight[k];
                            }
                        } // k loop

                        alpha[j_free][i_free] = alpha[i_free][j_free] = dot_product;
                       // if (i_free != j_free) {
                       //     // matrix is symmetric
                       //     alpha[i_free][j_free] = dot_product; //TODO dotProduct s/b including fixed parameters????!!!
                       // }
                        ++j_free;
                    }
                } // j loop
                beta[i_free] = beta_sum;
                ++i_free;
            }
        } // i loop
	}
	return 0;
}

/* These two variants, used just before the Marquardt fitting
   functions terminate, compute the function values at all points,
   whether or not they are being fitted.  (All points are fitted in
   the non-instrument response variant.)  They also compute the
   residuals y - yfit at all of those points and compute a chi-squared
   value which is not modified at small data values in the POISSON
   noise models.  They do not calculate alpha or beta. */

/* This is the variant which handles an instrument response. */
/* We assume that the function values are sensible.  Note also that we
   have to be careful about which points which include in the
   chi-squared calculation.  Also, we are guaranteed that the
   initialisation of the convolution arrays has been performed. */

int GCI_marquardt_compute_fn_final_instr(float xincr, float y[], int ndata,
				   int fit_start, int fit_end,
				   float instr[], int ninstr,
				   noise_type noise, float sig[],
				   float param[], int paramfree[], int nparam,
				   void (*fitfunc)(float, float [], float *, float [], int),
				   float yfit[], float dy[], float *chisq,	
					float **pfnvals, float ***pdy_dparam_pure, float ***pdy_dparam_conv,
					int *pfnvals_len, int *pdy_dparam_nparam_size)
{
	int i, j, mfit, ret;
	float sig2i;
	float *fnvals, **dy_dparam_pure, **dy_dparam_conv;
	int fnvals_len = *pfnvals_len;
	int dy_dparam_nparam_size = *pdy_dparam_nparam_size;

	/* check the necessary initialisation for safety, bail out if
	   broken */
	if ((fnvals_len < ndata) || (dy_dparam_nparam_size < nparam))
		return -1;
	fnvals = *pfnvals;
	dy_dparam_pure = *pdy_dparam_pure;
	dy_dparam_conv = *pdy_dparam_conv;

	for (j=0, mfit=0; j<nparam; j++)
		if (paramfree[j]) mfit++;

	/* Calculation of the fitting data will depend upon the type of
	   noise and the type of instrument response */

	/* Need to calculate unconvolved values all the way down to 0 for
	   the instrument response case */
	if (ninstr > 0) {
		if (fitfunc == GCI_multiexp_lambda)
			ret = multiexp_lambda_array(xincr, param, fnvals,
										dy_dparam_pure, ndata, nparam);
		else if (fitfunc == GCI_multiexp_tau)
			ret = multiexp_tau_array(xincr, param, fnvals,
									 dy_dparam_pure, ndata, nparam);
		else if (fitfunc == GCI_stretchedexp)
			ret = stretchedexp_array(xincr, param, fnvals,
									 dy_dparam_pure, ndata, nparam);
		else
			ret = -1;

		if (ret < 0)
			for (i=0; i<ndata; i++)
				(*fitfunc)(xincr*i, param, &fnvals[i],
						   dy_dparam_pure[i], nparam);

		/* OK, we've got to convolve the model fit with the given
		   instrument response.	 What we'll do here, then, is to
		   generate the whole model fit first, then do the convolution
		   with the instrument response.  Only after doing that will
		   we attempt to calculate the goodness of fit matrices.  This
		   means that we will be looping through all of the points
		   twice, which is not worth it if there is no convolution
		   necessary. */

		for (i=0; i<ndata; i++) {
			int convpts;
		
			/* We wish to find yfit = fnvals * instr, so explicitly:
			     yfit[i] = sum_{j=0}^i fnvals[i-j].instr[j]
			   But instr[k]=0 for k >= ninstr, so we only need to sum:
			     yfit[i] = sum_{j=0}^{min(ninstr-1,i)}
			   fnvals[i-j].instr[j]
			*/

			/* Zero our adder; don't need to bother with dy_dparam
			   stuff here */
			yfit[i] = 0;

			convpts = (ninstr <= i) ? ninstr-1 : i;
			for (j=0; j<=convpts; j++)
				yfit[i] += fnvals[i-j] * instr[j];
		}
	} else {
		/* Can go straight into the final arrays in this case */
		if (fitfunc == GCI_multiexp_lambda)
			ret = multiexp_lambda_array(xincr, param, yfit,
										dy_dparam_conv, ndata, nparam);
		else if (fitfunc == GCI_multiexp_tau)
			ret = multiexp_tau_array(xincr, param, yfit,
									 dy_dparam_conv, ndata, nparam);
		else if (fitfunc == GCI_stretchedexp)
			ret = stretchedexp_array(xincr, param, yfit,
									 dy_dparam_conv, ndata, nparam);
		else
			ret = -1;

		if (ret < 0)
			for (i=0; i<ndata; i++)
				(*fitfunc)(xincr*i, param, &yfit[i],
						   dy_dparam_conv[i], nparam);
	}
	 
	/* OK, now we've got our (possibly convolved) data, we can do the
	   rest almost exactly as above. */

	switch (noise) {
	case NOISE_CONST:
		*chisq = 0.0;
		/* Summation loop over all data */
		for (i=0; i<fit_start; i++) {
			yfit[i] += param[0];
			dy[i] = y[i] - yfit[i];
		}
		for ( ; i<fit_end; i++) {
			yfit[i] += param[0];
			dy[i] = y[i] - yfit[i];
			/* And find chi^2 */
			*chisq += dy[i] * dy[i];
		}
		for ( ; i<ndata; i++) {
			yfit[i] += param[0];
			dy[i] = y[i] - yfit[i];
		}

		/* Now divide chi-squared by sigma^2 */
		sig2i = 1.0 / (sig[0] * sig[0]);
		*chisq *= sig2i;
		break;

	case NOISE_GIVEN:  /* This is essentially the NR version */
		*chisq = 0.0;
		/* Summation loop over all data */
		for (i=0; i<fit_start; i++) {
			yfit[i] += param[0];
			dy[i] = y[i] - yfit[i];
		}
		for ( ; i<fit_end; i++) {
			yfit[i] += param[0];
			dy[i] = y[i] - yfit[i];
			/* And find chi^2 */
			sig2i = 1.0 / (sig[i] * sig[i]);
			*chisq += dy[i] * dy[i] * sig2i;
		}
		for ( ; i<ndata; i++) {
			yfit[i] += param[0];
			dy[i] = y[i] - yfit[i];
		}
		break;

	case NOISE_POISSON_DATA:
		*chisq = 0.0;
		/* Summation loop over all data */
		for (i=0; i<fit_start; i++) {
			yfit[i] += param[0];
			dy[i] = y[i] - yfit[i];
		}
		for ( ; i<fit_end; i++) {
			yfit[i] += param[0];
			dy[i] = y[i] - yfit[i];
			/* And find chi^2 */
			/* we still don't let the variance drop below 1 */
			sig2i = (y[i] > 1 ? 1.0/y[i] : 1.0);
			*chisq += dy[i] * dy[i] * sig2i;
		}
		for (; i<ndata; i++) {
			yfit[i] += param[0];
			dy[i] = y[i] - yfit[i];
		}
		break;

	case NOISE_POISSON_FIT:
		*chisq = 0.0;
		// Summation loop over all data 
		for (i=0; i<fit_start; i++) {
			yfit[i] += param[0];
			dy[i] = y[i] - yfit[i];
		}
		for ( ; i<fit_end; i++) {
			yfit[i] += param[0];
			dy[i] = y[i] - yfit[i];
			// And find chi^2 
			// we still don't let the variance drop below 1 
			sig2i = (yfit[i] > 1 ? 1.0/yfit[i] : 1.0);
			*chisq += dy[i] * dy[i] * sig2i;
		}
		for ( ; i<ndata; i++) {
			yfit[i] += param[0];
			dy[i] = y[i] - yfit[i];
		}
		break;
		
	case NOISE_MLE:		  		     // for the final chisq report a normal chisq measure for MLE
		*chisq = 0.0;
		// Summation loop over all data 
		for (i=0; i<fit_start; i++) {
			yfit[i] += param[0];
			dy[i] = y[i] - yfit[i];
		}
		for ( ; i<fit_end; i++) {
			yfit[i] += param[0];
			dy[i] = y[i] - yfit[i];
			// And find chi^2 
//			sig2i = (yfit[i] > 1 ? 1.0/yfit[i] : 1.0);
			if (yfit[i]<=0.0)
				; // do nothing
			else if (y[i]==0.0)
				*chisq += 2.0*yfit[i];   // to avoid NaN from log
			else
				*chisq += 2.0*(yfit[i]-y[i]) - 2.0*y[i]*log(yfit[i]/y[i]); // was dy[i] * dy[i] * sig2i;
		}
		for ( ; i<ndata; i++) {
			yfit[i] += param[0];
			dy[i] = y[i] - yfit[i];
		}
		if (*chisq <= 0.0) *chisq = 1.0e308; // don't let chisq=0 through yfit being all -ve
		break;

	case NOISE_GAUSSIAN_FIT:
		*chisq = 0.0;
		// Summation loop over all data 
		for (i=0; i<fit_start; i++) {
			yfit[i] += param[0];
			dy[i] = y[i] - yfit[i];
		}
		for ( ; i<fit_end; i++) {
			yfit[i] += param[0];
			dy[i] = y[i] - yfit[i];
			// And find chi^2 
			sig2i = (yfit[i] > 1 ? 1.0/yfit[i] : 1.0);
			*chisq += dy[i] * dy[i] * sig2i;
		}
		for ( ; i<ndata; i++) {
			yfit[i] += param[0];
			dy[i] = y[i] - yfit[i];
		}
		break;
	
	default:
		return -3;
		/* break; */   // (unreachable code)
	}

	return 0;
}


//********************************* GCI_marquardt_fitting_engine **********************************************************************

// This returns the number of iterations or negative if an error occurred.
// passes all the data to the ecf routine, checks the returned chisq and re-fits if it is of benefit
// was DoEcfFittingEngine() included in EcfSingle.c by PRB, 3.9.03

int GCI_marquardt_fitting_engine(float xincr, float *trans, int ndata, int fit_start, int fit_end, 
						float prompt[], int nprompt,
						noise_type noise, float sig[],
						float param[], int paramfree[],
					   int nparam, restrain_type restrain,
					   void (*fitfunc)(float, float [], float *, float [], int),
					   float *fitted, float *residuals, float *chisq,
					   float **covar, float **alpha, float **erraxes,
					   float chisq_target, float chisq_delta, int chisq_percent)
{
	float oldChisq, local_chisq;
	int ret, tries=0;

	if (ecf_exportParams) ecf_ExportParams_OpenFile ();

	// All of the work is done by the ECF module 
	ret = GCI_marquardt_instr(xincr, trans, ndata, fit_start, fit_end,
							  prompt, nprompt, noise, sig,
							  param, paramfree, nparam, restrain, fitfunc,
							  fitted, residuals, covar, alpha, &local_chisq,
							  chisq_delta, chisq_percent, erraxes);
							  
	// changed this for version 2, did a quick test with 2150ps_200ps_50cts_450cts.ics to see that the results are the same
	// NB this is also in GCI_SPA_1D_marquardt_instr() and GCI_SPA_2D_marquardt_instr()
	oldChisq = 3.0e38;
	while (local_chisq>chisq_target && (local_chisq<oldChisq) && tries<(MAXREFITS*3))
	{
		oldChisq = local_chisq; 
		tries++;
		ret += GCI_marquardt_instr(xincr, trans, ndata, fit_start, fit_end,
							  prompt, nprompt, noise, sig,
							  param, paramfree, nparam, restrain, fitfunc,
							  fitted, residuals, covar, alpha, &local_chisq,
							  chisq_delta, chisq_percent, erraxes);
	}						  
   /*     if (local_chisq<=chisq_target) {
            printf("local_chisq %f target %f\n", local_chisq, chisq_target);
        }
        if (local_chisq>=oldChisq) {
            printf("local_chisq %f oldChisq %f\n", local_chisq, oldChisq);
        }
        if (tries>=(MAXREFITS*3)) {
            printf("tries is %d\n", tries);
            printf("local_chisq %f oldChisq %f\n", local_chisq, oldChisq);
        }

        show_timing();*/

	if (chisq!=NULL) *chisq = local_chisq;

	if (ecf_exportParams) ecf_ExportParams_CloseFile ();

	return ret;		// summed number of iterations
}

/* Cleanup function */
void GCI_marquardt_cleanup(void)
{
/*	if (fnvals_len) {
		free(fnvals);
		GCI_ecf_free_matrix(dy_dparam_pure);
		GCI_ecf_free_matrix(dy_dparam_conv);
		fnvals_len = 0;
		dy_dparam_nparam_size = 0;
	}
*/
}

// Emacs settings:
// Local variables:
// mode: c
// c-basic-offset: 4
// tab-width: 4
// End:
