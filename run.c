#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <ctype.h>

#include "allvars.h"
#include "proto.h"
#include "cooling.h"
#include "mymalloc.h"
#include "endrun.h"

/*! \file run.c
 *  \brief  iterates over timesteps, main loop
 */

/*! This routine contains the main simulation loop that iterates over
 * single timesteps. The loop terminates when the cpu-time limit is
 * reached, when a `stop' file is found in the output directory, or
 * when the simulation ends because we arrived at TimeMax.
 */
static int human_interaction();
int stopflag = 0;
void run(void)
{
    walltime_measure("/Misc");

    write_cpu_log();		/* produce some CPU usage info */

    do				/* main loop */
    {

        find_next_sync_point_and_drift();	/* find next synchronization point and drift particles to this time.
                                             * If needed, this function will also write an output file
                                             * at the desired time.
                                             */

        if(stopflag == 1 || stopflag == 2) {
            /* OK snapshot file is written, lets quit */
            return;
        }
        every_timestep_stuff();	/* write some info to log-files */

        IonizeParams(All.Time);		/* set UV background for the current time */

        compute_accelerations(0);	/* compute accelerations for
                                     * the particles that are to be advanced
                                     */

        /* check whether we want a full energy statistics */
        if(Flag_FullStep)
        {
            energy_statistics();	/* compute and output energy statistics */
        }

        advance_and_find_timesteps();	/* 'kick' active particles in
                                         * momentum space and compute new
                                         * timesteps for them
                                         */

        write_cpu_log();		/* produce some CPU usage info */

        All.NumCurrentTiStep++;

        stopflag = human_interaction();
        if(stopflag != 0) {
            All.Ti_nextoutput = All.Ti_Current;
            /* next loop will write a new snapshot file */
        }
        report_memory_usage("RUN");
    }
    while(All.Ti_Current < TIMEBASE && All.Time <= All.TimeMax);

    savepositions(All.SnapshotFileCount++, 0);

    /* write a last snapshot
     * file at final time (will
     * be overwritten if
     * All.TimeMax is increased
     * and the run is continued)
     */
}
static int human_interaction() {
        /* Check whether we need to interrupt the run */
    int stopflag = 0;
    char stopfname[4096], contfname[4096];
    char restartfname[4096];
    char ioctlfname[4096];

    sprintf(stopfname, "%s/stop", All.OutputDir);
    sprintf(restartfname, "%s/restart", All.OutputDir);
    sprintf(contfname, "%s/cont", All.OutputDir);
    sprintf(ioctlfname, "%s/ioctl", All.OutputDir);

    if(ThisTask == 0)
    {
        FILE * fd;
        if((fd = fopen(ioctlfname, "r"))) {
             /* there is an ioctl file, parse it and update
              * All.NumPartPerFile
              * All.NumWriters
              */
            size_t n = 0;
            char * line = NULL;
            while(-1 != getline(&line, &n, fd)) {
                sscanf(line, "NumPartPerFile %d", &All.NumPartPerFile);
                sscanf(line, "NumWriters %d", &All.NumWriters);
            }
            free(line);
            printf("New IO parameter recieved from %s:\n"
                   "NumPartPerfile %d\n"
                   "NumWriters %d\n",
                ioctlfname,
                All.NumPartPerFile,
                All.NumWriters);
            fclose(fd);
        }
        /* Is the stop-file present? If yes, interrupt the run. */
        if((fd = fopen(stopfname, "r")))
        {
            printf("human controlled stopping.\n");
            fclose(fd);
            stopflag = 1;
            unlink(stopfname);
        }

        /* are we running out of CPU-time ? If yes, interrupt run. */
        if(All.CT.ElapsedTime > 0.85 * All.TimeLimitCPU) {
            printf("reaching time-limit. stopping.\n");
            stopflag = 2;
        }
        if((fd = fopen(restartfname, "r")))
        {
            printf("human controlled snapshot.\n");
            fclose(fd);
            stopflag = 3;
            unlink(restartfname);
        }
        if((All.CT.ElapsedTime - All.TimeLastRestartFile) >= All.CpuTimeBetRestartFile) {
            All.TimeLastRestartFile = All.CT.ElapsedTime;
            printf("time to write a snapshot for restarting\n");
            stopflag = 3;
        }
    }

    MPI_Bcast(&stopflag, 1, MPI_INT, 0, MPI_COMM_WORLD);

    return stopflag;
}

/*! This function finds the next synchronization point of the system
 * (i.e. the earliest point of time any of the particles needs a force
 * computation), and drifts the system to this point of time.  If the
 * system dirfts over the desired time of a snapshot file, the
 * function will drift to this moment, generate an output, and then
 * resume the drift.
 */
void find_next_sync_point_and_drift(void)
{
    int n, i, prev, dt_bin, ti_next_for_bin, ti_next_kick, ti_next_kick_global;
    int64_t numforces2;
    double timeold;

    timeold = All.Time;

    /* find the next kick time */
    for(n = 0, ti_next_kick = TIMEBASE; n < TIMEBINS; n++)
    {
        if(TimeBinCount[n])
        {
            if(n > 0)
            {
                dt_bin = (1 << n);
                ti_next_for_bin = (All.Ti_Current / dt_bin) * dt_bin + dt_bin;	/* next kick time for this timebin */
            }
            else
            {
                ti_next_for_bin = All.Ti_Current;
            }

            if(ti_next_for_bin < ti_next_kick)
                ti_next_kick = ti_next_for_bin;
        }
    }

    MPI_Allreduce(&ti_next_kick, &ti_next_kick_global, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD);

    while(ti_next_kick_global >= All.Ti_nextoutput && All.Ti_nextoutput >= 0)
    {
        All.Ti_Current = All.Ti_nextoutput;

        double nexttime;

        nexttime = All.TimeBegin * exp(All.Ti_Current * All.Timebase_interval);

        set_global_time(nexttime);

        move_particles(All.Ti_nextoutput);

        savepositions(All.SnapshotFileCount++, stopflag);	/* write snapshot file */

        All.Ti_nextoutput = find_next_outputtime(All.Ti_nextoutput + 1);
    }

    All.Ti_Current = ti_next_kick_global;

    double nexttime;

    nexttime = All.TimeBegin * exp(All.Ti_Current * All.Timebase_interval);

    set_global_time(nexttime);

    All.TimeStep = All.Time - timeold;


    /* mark the bins that will be active */
    int NumForceUpdate;

    for(n = 1, TimeBinActive[0] = 1, NumForceUpdate = TimeBinCount[0]; n < TIMEBINS; n++)
    {
        dt_bin = (1 << n);

        if((ti_next_kick_global % dt_bin) == 0)
        {
            TimeBinActive[n] = 1;
            NumForceUpdate += TimeBinCount[n];
        }
        else
            TimeBinActive[n] = 0;
    }

    sumup_large_ints(1, &NumForceUpdate, &GlobNumForceUpdate);

    if(GlobNumForceUpdate >= All.TotNumPart)
        Flag_FullStep = 1;
    else
        Flag_FullStep = 0;

    All.NumForcesSinceLastDomainDecomp += GlobNumForceUpdate;


    FirstActiveParticle = -1;

    for(n = 0, prev = -1; n < TIMEBINS; n++)
    {
        if(TimeBinActive[n])
        {
            for(i = FirstInTimeBin[n]; i >= 0; i = NextInTimeBin[i])
            {
                if(prev == -1)
                    FirstActiveParticle = i;

                if(prev >= 0)
                    NextActiveParticle[prev] = i;

                prev = i;
            }
        }
    }

    if(prev >= 0)
        NextActiveParticle[prev] = -1;


    walltime_measure("/Misc");
    /* drift the active particles, others will be drifted on the fly if needed */
    NumForceUpdate = 0;
    for(i = FirstActiveParticle; i >= 0; i = NextActiveParticle[i])
    {
        drift_particle(i, All.Ti_Current);

        NumForceUpdate++;
    }

    sumup_large_ints(1, &NumForceUpdate, &numforces2);
    if(GlobNumForceUpdate != numforces2)
    {
        endrun(2, "terrible; this needs to be understood.");
    }

    walltime_measure("/Drift");
}

int ShouldWeDoDynamicUpdate(void)
{
    int n, num, dt_bin, ti_next_for_bin, ti_next_kick, ti_next_kick_global;
    int64_t numforces;


    /* find the next kick time */
    for(n = 0, ti_next_kick = TIMEBASE; n < TIMEBINS; n++)
    {
        if(TimeBinCount[n])
        {
            if(n > 0)
            {
                dt_bin = (1 << n);
                ti_next_for_bin = (All.Ti_Current / dt_bin) * dt_bin + dt_bin;	/* next kick time for this timebin */
            }
            else
            {
                ti_next_for_bin = All.Ti_Current;
            }

            if(ti_next_for_bin < ti_next_kick)
                ti_next_kick = ti_next_for_bin;
        }
    }

    MPI_Allreduce(&ti_next_kick, &ti_next_kick_global, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD);

    /* count the particles that will be active */
    for(n = 1, num = TimeBinCount[0]; n < TIMEBINS; n++)
    {
        dt_bin = (1 << n);

        if((ti_next_kick_global % dt_bin) == 0)
            num += TimeBinCount[n];
    }

    sumup_large_ints(1, &num, &numforces);

    message(0, "I'm guessing %013d particles to be active in the next step\n", numforces);

    if((All.NumForcesSinceLastDomainDecomp + numforces) >= All.TreeDomainUpdateFrequency * All.TotNumPart)
        return 0;
    else
        return 1;
}



/*! this function returns the next output time that is equal or larger to
 *  ti_curr
 */
int find_next_outputtime(int ti_curr)
{
    int i, ti_next=-1;

    for(i = 0; i < All.OutputListLength; i++)
    {
        const double time = All.OutputListTimes[i];

        if(time >= All.TimeBegin && time <= All.TimeMax)
        {
            const int ti = (int) (log(time / All.TimeBegin) / All.Timebase_interval);

            if(ti >= ti_curr)
            {
                ti_next = ti;
                break;
            }
        }
    }

    if(ti_next == -1)
    {
        ti_next = 2 * TIMEBASE;	/* this will prevent any further output */

        message(0, "There is no valid time for a further snapshot file.\n");
    }
    else
    {
        const double next = All.TimeBegin * exp(ti_next * All.Timebase_interval);

        message(0, "Setting next time for snapshot file to Time_next= %g \n", next);

    }

    return ti_next;
}




/*! This routine writes one line for every timestep.
 * FdCPU the cumulative cpu-time consumption in various parts of the
 * code is stored.
 */
void every_timestep_stuff(void)
{
    double z;
    int i;
    int64_t tot, tot_sph;
    int64_t tot_count[TIMEBINS];
    int64_t tot_count_sph[TIMEBINS];

    sumup_large_ints(TIMEBINS, TimeBinCount, tot_count);
    sumup_large_ints(TIMEBINS, TimeBinCountSph, tot_count_sph);

    /* let's update Tot counts in one place tot variables;
     * at this point there can still be holes in SphP
     * because rearrange_particle_squence is not called yet.
     * but anywaysTotN_sph variables are not well defined and
     * not used any places but printing.
     *
     * we shall just say they we sync these variables right after gravity
     * calculation in every timestep.
     * */

    sumup_large_ints(1, &NumPart, &All.TotNumPart);
    sumup_large_ints(1, &N_dm, &All.TotN_dm);
    sumup_large_ints(1, &N_sph, &All.TotN_sph);
    sumup_large_ints(1, &N_bh, &All.TotN_bh);
    sumup_large_ints(1, &N_star, &All.TotN_star);


    char extra[1024] = {0};

    if(All.PM_Ti_endstep == All.Ti_Current)
        strcat(extra, "PM-Step");

    z = 1.0 / (All.Time) - 1;
    message(0, "Begin Step %d, Time: %g, Redshift: %g, Nf = %014ld, Systemstep: %g, Dloga: %g, status: %s\n",
                All.NumCurrentTiStep, All.Time, z,
                GlobNumForceUpdate,
                All.TimeStep, log(All.Time) - log(All.Time - All.TimeStep),
                extra);

    message(0, "TotNumPart: %013ld SPH %013ld BH %010ld STAR %013ld \n",
                All.TotNumPart, All.TotN_sph, All.TotN_bh, All.TotN_star);
    message(0, "Occupied timebins: non-sph         sph       dt\n");
    for(i = TIMEBINS - 1, tot = tot_sph = 0; i >= 0; i--)
        if(tot_count_sph[i] > 0 || tot_count[i] > 0)
        {
            message(0, " %c  bin=%2d     %014ld %014ld   %6g\n",
                    TimeBinActive[i] ? 'X' : ' ',
                    i,
                    (tot_count[i] - tot_count_sph[i]),
                    tot_count_sph[i],
                    i > 0 ? (1 << i) * All.Timebase_interval : 0.0);
            if(TimeBinActive[i])
            {
                tot += tot_count[i];
                tot_sph += tot_count_sph[i];
            }
        }
    message(0, "               -----------------------------------\n");
    message(0, "Total:%014ld %014ld    Sum:%014ld\n",
        (tot - tot_sph),
        (tot_sph),
        (tot));


    set_random_numbers();
}



void write_cpu_log(void)
{
    All.Cadj_Cpu += walltime_get_time("/Tree/Walk1") + walltime_get_time("/Tree/Walk2");

    int64_t totBlockedPD = -1, totBlockedND = -1;
    int64_t totTotalPD = -1, totTotalND = -1;

#ifdef _OPENMP
    MPI_Reduce(&BlockedParticleDrifts, &totBlockedPD, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(&BlockedNodeDrifts, &totBlockedND, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(&TotalParticleDrifts, &totTotalPD, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(&TotalNodeDrifts, &totTotalND, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
#endif

    walltime_summary(0, MPI_COMM_WORLD);

    if(ThisTask == 0)
    {

        fprintf(FdCPU, "Step %d, Time: %g, MPIs: %d Threads: %d Elapsed: %g\n", All.NumCurrentTiStep, All.Time, NTask, All.NumThreads, All.CT.ElapsedTime);
#ifdef _OPENMP
        fprintf(FdCPU, "Blocked Drifts (Particle Node): %ld %ld\n", totBlockedPD, totBlockedND);
        fprintf(FdCPU, "Total Drifts (Particle Node): %ld %ld\n", totTotalPD, totTotalND);
#endif
        fflush(FdCPU);
    }
    walltime_report(FdCPU, 0, MPI_COMM_WORLD);
    if(ThisTask == 0) {
        fflush(FdCPU);
    }
}



/*! This routine first calls a computation of various global
 * quantities of the particle distribution, and then writes some
 * statistics about the energies in the various particle components to
 * the file FdEnergy.
 */
void energy_statistics(void)
{
    compute_global_quantities_of_system();

    message(0, "Time %g Mean Temperature of Gas %g\n",
                All.Time, SysState.TemperatureComp[0]);

    if(ThisTask == 0)
    {
        fprintf(FdEnergy,
                "%g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g %g\n",
                All.Time, SysState.TemperatureComp[0], SysState.EnergyInt, SysState.EnergyPot, SysState.EnergyKin, SysState.EnergyIntComp[0],
                SysState.EnergyPotComp[0], SysState.EnergyKinComp[0], SysState.EnergyIntComp[1],
                SysState.EnergyPotComp[1], SysState.EnergyKinComp[1], SysState.EnergyIntComp[2],
                SysState.EnergyPotComp[2], SysState.EnergyKinComp[2], SysState.EnergyIntComp[3],
                SysState.EnergyPotComp[3], SysState.EnergyKinComp[3], SysState.EnergyIntComp[4],
                SysState.EnergyPotComp[4], SysState.EnergyKinComp[4], SysState.EnergyIntComp[5],
                SysState.EnergyPotComp[5], SysState.EnergyKinComp[5], SysState.MassComp[0],
                SysState.MassComp[1], SysState.MassComp[2], SysState.MassComp[3], SysState.MassComp[4],
                SysState.MassComp[5]);

        fflush(FdEnergy);
    }
}
