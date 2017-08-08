#include <math.h>
#include "allvars.h"
#include "cosmology.h"
#include "endrun.h"
#include <gsl/gsl_integration.h>
#include <gsl/gsl_odeiv2.h>

void init_cosmology(Cosmology * CP)
{
    /*With slightly relativistic massive neutrinos, for consistency we need to include radiation.
     * A note on normalisation (as of 08/02/2012):
     * CAMB appears to set Omega_Lambda + Omega_Matter+Omega_K = 1,
     * calculating Omega_K in the code and specifying Omega_Lambda and Omega_Matter in the paramfile.
     * This means that Omega_tot = 1+ Omega_r + Omega_g, effectively
     * making h0 (very) slightly larger than specified, and the Universe is no longer flat!
     */
    CP->OmegaCDM = CP->Omega0 - CP->OmegaBaryon;
    CP->OmegaK = 1.0 - CP->Omega0 - CP->OmegaLambda;

    /* Omega_g = 4 \sigma_B T_{CMB}^4 8 \pi G / (3 c^3 H^2) */

    CP->OmegaG = 4 * STEFAN_BOLTZMANN
                  * pow(CP->CMBTemperature, 4)
                  * (8 * M_PI * GRAVITY)
                  / (3*C*C*C*HUBBLE*HUBBLE)
                  / (CP->HubbleParam*CP->HubbleParam);

    /* Neutrino + antineutrino background temperature as a ratio to T_CMB0
     * Note there is a slight correction from 4/11
     * due to the neutrinos being slightly coupled at e+- annihilation.
     * See Mangano et al 2005 (hep-ph/0506164)
     * The correction is (3.046/3)^(1/4), for N_eff = 3.046 */
    double TNu0_TCMB0 = pow(4/11., 1/3.) * 1.00328;

    /* For massless neutrinos,
     * rho_nu/rho_g = 7/8 (T_nu/T_cmb)^4 *N_eff,
     * but we absorbed N_eff into T_nu above. */
    CP->OmegaNu0 = CP->OmegaG * 7. / 8 * pow(TNu0_TCMB0, 4) * 3;
}

/*Hubble function at scale factor a, in dimensions of All.Hubble*/
double hubble_function(double a)
{

    double hubble_a;

    /* first do the terms in SQRT */
    hubble_a = All.CP.OmegaLambda;

    hubble_a += All.CP.OmegaK / (a * a);
    hubble_a += All.CP.Omega0 / (a * a * a);

    if(All.CP.RadiationOn) {
        hubble_a += All.CP.OmegaG / (a * a * a * a);
        /* massless neutrinos are added only if there is no (massive) neutrino particle.*/
        if(!NTotal[2])
            hubble_a += All.CP.OmegaNu0 / (a * a * a * a);
    }

    /* Now finish it up. */
    hubble_a = All.Hubble * sqrt(hubble_a);
    return (hubble_a);
}

static double growth(double a, double *dDda);

double GrowthFactor(double astart)
{
    return growth(astart, NULL) / growth(1.0, NULL);
}

int growth_ode(double a, const double yy[], double dyda[], void * params)
{
    const double hub = hubble_function(a)/All.Hubble;
    dyda[0] = yy[1]/pow(a,3)/hub;
    /*Only use gravitating part*/
    dyda[1] = yy[0] * 1.5 * a * All.CP.Omega0/(a*a*a) / hub;
    return GSL_SUCCESS;
}

/** The growth function is given as a 2nd order DE in Peacock 1999, Cosmological Physics.
 * D'' + a'/a D' - 1.5 * (a'/a)^2 D = 0
 * 1/a (a D')' - 1.5 (a'/a)^2 D
 * where ' is d/d tau = a^2 H d/da
 * Define F = a^3 H dD/da
 * and we have: dF/da = 1.5 a H D
 */
double growth(double a, double * dDda)
{
  gsl_odeiv2_system FF;
  FF.function = &growth_ode;
  FF.jacobian = NULL;
  FF.dimension = 2;
  gsl_odeiv2_driver * drive = gsl_odeiv2_driver_alloc_standard_new(&FF,gsl_odeiv2_step_rkf45, 1e-5, 1e-8,1e-8,1,1);
   /* We start early to avoid lambda.*/
  double curtime = 1e-5;
  /* Initial velocity chosen so that D = Omegar + 3/2 Omega_m a,
   * the solution for a matter/radiation universe.*
   * Note the normalisation of D is arbitrary
   * and never seen outside this function.*/
  double yinit[2] = {1.5 * All.CP.Omega0/(curtime*curtime), pow(curtime,3)*hubble_function(curtime)/All.Hubble * 1.5 * All.CP.Omega0/(curtime*curtime*curtime)};
  if(All.CP.RadiationOn)
      yinit[0] += (All.CP.OmegaG+All.CP.OmegaNu0)/pow(curtime,4);

  int stat = gsl_odeiv2_driver_apply(drive, &curtime,a, yinit);
  if (stat != GSL_SUCCESS) {
      endrun(1,"gsl_odeiv in growth: %d. Result at %g is %g %g\n",stat, curtime, yinit[0], yinit[1]);
  }
  gsl_odeiv2_driver_free(drive);
  /*Store derivative of D if needed.*/
  if(dDda) {
      *dDda = yinit[1]/pow(a,3)/(hubble_function(a)/All.Hubble);
  }
  return yinit[0];
}

/*
 * This is the Zeldovich approximation prefactor,
 * f1 = d ln D1 / dlna = a / D (dD/da)
 */
double F_Omega(double a)
{
    double dD1da=0;
    double D1 = growth(a, &dD1da);
    return a / D1 * dD1da;
}

static double sigma2_int(double k, void * p)
{
    void ** params = p;
    FunctionOfK * fk = params[0];
    double * R = params[1];
    double kr, kr3, kr2, w, x;

    kr = *R * k;
    kr2 = kr * kr;
    kr3 = kr2 * kr;

    if(kr < 1e-8)
        return 0;

    w = 3 * (sin(kr) / kr3 - cos(kr) / kr2);
    x = 4 * M_PI * k * k * w * w * function_of_k_eval(fk, k);

    return x;
}

double function_of_k_eval(FunctionOfK * fk, double k)
{
    /* ignore the 0 mode */

    if(k == 0) return 1;

    int l = 0;
    int r = fk->size - 1;

    while(r - l > 1) {
        int m = (r + l) / 2;
        if(k < fk->table[m].k)
            r = m;
        else
            l = m;
    }
    double k2 = fk->table[r].k,
           k1 = fk->table[l].k;
    double p2 = fk->table[r].P,
           p1 = fk->table[l].P;

    if(l == r) {
        return fk->table[l].P;
    }

    if(p1 == 0 || p2 == 0 || k1 == 0 || k2 == 0) {
        /* if any of the p is zero, use linear interpolation */
        double p = (k - k1) * p2 + (k2 - k) * p1;
        p /= (k2 - k1);
        return p;
    } else {
        k = log(k);
        p1 = log(p1);
        p2 = log(p2);
        k1 = log(k1);
        k2 = log(k2);
        double p = (k - k1) * p2 + (k2 - k) * p1;
        p /= (k2 - k1);
        return exp(p);
    }
}

double function_of_k_tophat_sigma(FunctionOfK * fk, double R)
{
    gsl_integration_workspace * w = gsl_integration_workspace_alloc (1000);
    void * params[] = {fk, &R};
    double result,abserr;
    gsl_function F;
    F.function = &sigma2_int;
    F.params = params;

    /* note: 500/R is here chosen as integration boundary (infinity) */
    gsl_integration_qags (&F, 0, 500. / R, 0, 1e-4,1000,w,&result, &abserr);
    //   printf("gsl_integration_qng in TopHatSigma2. Result %g, error: %g, intervals: %lu\n",result, abserr,w->size);
    gsl_integration_workspace_free (w);
    return sqrt(result);
}

void function_of_k_normalize_sigma(FunctionOfK * fk, double R, double sigma) {
    double old = function_of_k_tophat_sigma(fk, R);
    int i;
    for(i = 0; i < fk->size; i ++) {
        fk->table[i].P *= sigma / old;
    };
}


