/*Tests for the cosmology module, ported from N-GenIC.*/

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "stub.h"
#include <libgadget/config.h>
#include <libgenic/power.h>
#include <libgadget/cosmology.h>

struct test_state
{
    struct power_params PowerP;
    Cosmology CP;
};

/*Simple test without rescaling*/
static void
test_read_no_rescale(void ** state)
{
    /*Do setup*/
    struct power_params PowerP = ((struct test_state *) (*state))->PowerP;
    /*Test without rescaling*/
    PowerP.InputPowerRedshift = -1;
    PowerP.DifferentTransferFunctions = 1;
    /*Test without rescaling*/
    int nentry = initialize_powerspectrum(0, 0.01, 3.085678e21, NULL, &PowerP);
    assert_int_equal(nentry, 335);
    /*Check that the tabulated power spectrum gives the right answer
     * First check ranges: these should both be out of range.
     * Should be the same k as in the file (but /10^3 for Mpc -> kpc)
     * Note that our PowerSpec function is 2pi^3 larger than that in S-GenIC.*/
    assert_true(PowerSpec(9.8e-9, 7) < 2e-30);
    assert_true(PowerSpec(300, 7) < 2e-30);
    //Now check total power: k divided by 10^3,
    //Conversion for P(k) is 10^9/(2pi)^3
    assert_true(fabs(PowerSpec(1.124995061548053968e-02/1e3, 7) / 4.745074933325402533/1e9 - 1) < 1e-5);
    assert_true(fabs(PowerSpec(1.010157135208153312e+00/1e3, 7) / 1.15292e-02/1e9 - 1) < 1e-5);
    //Check that it gives reasonable results when interpolating
    int k;
    for (k = 1; k < 100; k++) {
        double newk = 0.10022E+01/1e3+ k*(0.10362E+01-0.10022E+01)/1e3/100;
        assert_true(PowerSpec(newk,7) < PowerSpec(0.10022E+01/1e3,7));
        assert_true(PowerSpec(newk,7) > PowerSpec(0.10362E+01/1e3,7));
        assert_true(PowerSpec(newk,0)/PowerSpec(0.10362E+01/1e3,1) < 1);
    }
    //Now check transfer functions: ratio of total to species should be ratio of T_s to T_tot squared: large scales where T~ 1
    //CDM
    assert_true(fabs(PowerSpec(2.005305808001081169e-03/1e3,1)/PowerSpec(2.005305808001081169e-03/1e3,7)- pow(1.193460280018762132e+05/1.193185119820504624e+05,2)) < 1e-5);
    //Small scales where there are differences
    //T_tot=0.255697E+06
    //Baryons
    assert_true(fabs(PowerSpec(1.079260830861467901e-01/1e3,0)/PowerSpec(1.079260830861467901e-01/1e3,6)- pow(9.735695830700024089e+03/1.394199788775037632e+04,2)) < 1e-6);
    //CDM
    assert_true(fabs(PowerSpec(1.079260830861467901e-01/1e3,1)/PowerSpec(1.079260830861467901e-01/1e3,6)- pow(1.477251880454670209e+04/1.394199788775037632e+04,2)) < 1e-6);
    //Test the growth function
    assert_true(fabs(PowerSpec(1.079260830861467901e-01/1e3,1)/PowerSpec(1.079260830861467901e-01/1e3,6)- pow(1.477251880454670209e+04/1.394199788775037632e+04,2)) < 1e-6);
}

static void
test_growth_numerical(void ** state)
{
    /*Do setup*/
    struct power_params PowerP = ((struct test_state *) (*state))->PowerP;
    Cosmology CP = ((struct test_state *) (*state))->CP;
    /*Test without rescaling*/
    PowerP.InputPowerRedshift = -1;
    PowerP.DifferentTransferFunctions = 1;
    int nentry = initialize_powerspectrum(0, 0.01, 3.085678e21, &CP, &PowerP);
    assert_int_equal(nentry, 335);
    //Test sub-horizon scales
    int k, nk = 100;
    double lowk = 5e-2;
    double highk = 10;
    for (k = 1; k < nk; k++) {
        double newk = exp(log(lowk) + k*(log(highk) - log(lowk))/nk);
        newk/=1e3;
        //Total growth should be very close to F_Omega.
/*         message(1,"k=%g G = %g F = %g G0 = %g\n",newk*1e3,dlogGrowth(newk, 7), F_Omega(0.01),dlogGrowth(newk, 0)); */
        //Why the slight difference?
        assert_true(fabs(dlogGrowth(newk,7)  - F_Omega(0.01)) < 0.05);
        //Growth of CDM should be lower, growth of baryons should be higher.
        assert_true(dlogGrowth(newk,1) < F_Omega(0.01));
        assert_true(dlogGrowth(newk,1) > 0.9);
        //The BAO wiggles make this hard to bound
        assert_true(dlogGrowth(newk,0) > 1);
        assert_true(dlogGrowth(newk,0) < 1.5);
    }
    //Test super-horizon scales
    lowk = 1e-3;
    highk = 5e-3;
    for (k = 1; k < nk; k++) {
        double newk = exp(log(lowk) + k*(log(highk) - log(lowk))/nk);
        newk/=1e3;
/*         message(1,"k=%g G = %g F = %g\n",newk*1e3,dlogGrowth(newk, 7), dlogGrowth(newk, 1)); */
        //Total growth should be around 1.05
        assert_true(dlogGrowth(newk,7) < 1.05);
        assert_true(dlogGrowth(newk,7) > 1.);
        //CDM and baryons should match total
        assert_true(fabs(dlogGrowth(newk,0)/dlogGrowth(newk,7) -1)  < 0.008);
        assert_true(fabs(dlogGrowth(newk,1)/dlogGrowth(newk,7) -1)  < 0.008);
    }
}

/*Check normalising to a different sigma8 and redshift*/
static void
test_read_rescale_sigma8(void ** state)
{
    /*Do setup*/
    struct power_params PowerP = ((struct test_state *) (*state))->PowerP;
    Cosmology CP = ((struct test_state *) (*state))->CP;
    /* Test rescaling to an earlier time
     * (we still use the same z=99 power which should not be rescaled in a real simulation)*/
    PowerP.InputPowerRedshift = 9;
    PowerP.DifferentTransferFunctions = 0;
    int nentry = initialize_powerspectrum(0, 0.05, 3.085678e21, &CP, &PowerP);
    assert_int_equal(nentry, 335);
    assert_true(fabs(PowerSpec(1.124995061548053968e-02/1e3, 7)*4 /4.745074933325402533/1e9 - 1) < 1e-2);
}


static void setup(void ** state)
{
    static struct test_state st;
    st.PowerP.InputPowerRedshift = -1;
    st.PowerP.DifferentTransferFunctions = 1;
    st.PowerP.Sigma8 = -1;
    st.PowerP.FileWithInputSpectrum = GADGET_TESTDATA_ROOT "/examples/camb_matterpow_99.dat";
    st.PowerP.FileWithTransferFunction = GADGET_TESTDATA_ROOT "/examples/camb_transfer_99.dat";
    st.PowerP.FileWithFutureTransferFunction = GADGET_TESTDATA_ROOT "/examples/camb_transfer_98.99.dat";
    st.PowerP.InputFutureRedshift = 98.99;
    st.PowerP.WhichSpectrum = 2;
    st.PowerP.SpectrumLengthScale = 1000;
    st.PowerP.PrimordialIndex = 1.0;
    st.CP.Omega0 = 0.2814;
    st.CP.OmegaLambda = 1 - st.CP.Omega0;
    st.CP.OmegaBaryon = 0.0464;
    st.CP.HubbleParam = 0.697;
    st.CP.Omega_fld = 0;
    st.CP.w0_fld = -1;
    st.CP.wa_fld = 0;
    st.CP.CMBTemperature = 2.7255;
    st.CP.RadiationOn = 1;
    st.CP.MNu[0] = 0;
    st.CP.MNu[1] = 0;
    st.CP.MNu[2] = 0;
    st.CP.Hubble =  3.2407789e-18 * 3.08568e+16;
    init_cosmology(&st.CP, 0.01);
    *state = &st;
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_read_no_rescale),
        cmocka_unit_test(test_read_rescale_sigma8),
        cmocka_unit_test(test_growth_numerical)
    };
    return cmocka_run_group_tests_mpi(tests, setup, NULL);
}
