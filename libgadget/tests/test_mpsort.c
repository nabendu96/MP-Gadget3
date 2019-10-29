#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdio.h>

#include "stub.h"

#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <string.h>
#include <unistd.h>

#include <mpi.h>
#include "../utils/mpsort.h"

static void radix_int(const void * ptr, uint64_t * radix, void * arg) {
    *radix = *(const uint64_t*) ptr + INT64_MIN;
}

static int64_t
checksum(int64_t * data, size_t localsize, MPI_Comm comm)
{
    int64_t sum = 0;
    size_t i;
    for(i = 0; i < localsize; i ++) {
        sum += data[i];
    }
    
    MPI_Allreduce(MPI_IN_PLACE, &sum, 1, MPI_LONG, MPI_SUM, comm);
    return sum;
}
static void
generate(int64_t * data, size_t localsize, int bits, int seed)
{
    /* only keep bits of precision. */
    srandom(seed);

    size_t i;
    unsigned shift = 64u - bits;
    for(i = 0; i < localsize; i ++) {
        uint64_t value = (uint64_t) random() * (uint64_t) random() * random() * random();
        data[i] = (signed) ((value << shift));
    }
}

static void
check_sorted(int64_t * data, size_t localsize, MPI_Comm comm)
{
    size_t i;
    int ThisTask, NTask;
    MPI_Comm_rank(comm, &ThisTask);
    MPI_Comm_size(comm, &NTask);

    const int TAG = 0xbeef;

    ptrdiff_t dummy[1] = {INT64_MIN};

    for(i = 1; i < localsize; i ++) {
        if(data[i] < data[i - 1]) {
            endrun(12, "Ordering of local array is broken i=%ld, d=%ld d-1=%ld. \n", i, data[i], data[i-1]);
        }
        assert_true(data[i] >= data[i - 1]);
    }

    if(NTask == 1) return;


    int64_t prev = -1;

    while(1) {
        if(ThisTask == 0) {
            int64_t * ptr = dummy;
            if(localsize > 0) {
                ptr = &data[localsize - 1];
            }
            MPI_Send(ptr, 1, MPI_LONG, ThisTask + 1, TAG, comm);
            break;
        }
        if(ThisTask == NTask - 1) {
            MPI_Recv(&prev, 1, MPI_LONG,
                    ThisTask - 1, TAG, comm, MPI_STATUS_IGNORE);
            break;
        }
        /* else */
        if(localsize == 0) {
            /* simply pass through whatever we get */
            MPI_Recv(&prev, 1, MPI_LONG, ThisTask - 1, TAG, comm, MPI_STATUS_IGNORE);
            MPI_Send(&prev, 1, MPI_LONG, ThisTask + 1, TAG, comm);
            break;
        }
        else
        { 
            MPI_Sendrecv(
                    &data[localsize - 1], 1, MPI_LONG, 
                    ThisTask + 1, TAG,
                    &prev, 1, MPI_LONG,
                    ThisTask - 1, TAG, comm, MPI_STATUS_IGNORE);
            break;
        }
    }

    if(ThisTask > 1) {
        if(localsize > 0) {
//                printf("ThisTask = %d prev = %d\n", ThisTask, prev);
            if(prev > data[0]) {
                endrun(12, "Ordering of global array is broken prev=%ld d=%ld. \n", prev, data[0]);
            }
            assert_true(prev < data[0]);
        }
    }
}

static void
do_mpsort_test(int64_t srcsize, int bits, int staggered, int gather)
{
    int ThisTask;
    int NTask;

    MPI_Comm_size(MPI_COMM_WORLD, &NTask);
    MPI_Comm_rank(MPI_COMM_WORLD, &ThisTask);

    if(gather == 1)
        mpsort_mpi_set_options(MPSORT_REQUIRE_GATHER_SORT);
    if(gather == 0)
        mpsort_mpi_set_options(MPSORT_DISABLE_GATHER_SORT);

//     message(0, "NTask = %d\n", NTask);
//     message(0, "src size = %ld\n", srcsize);

    if(staggered && (ThisTask % 2 == 0)) srcsize = 0;

    int64_t csize;

    MPI_Allreduce(&srcsize, &csize, 1, MPI_LONG, MPI_SUM, MPI_COMM_WORLD);

    int64_t destsize = csize * (ThisTask + 1) /  NTask - csize * (ThisTask) / NTask;

    message(0, "dest size = %ld\n", destsize);
//     message(0, "csize = %ld\n", csize);

    int64_t * src = mymalloc("src", srcsize * sizeof(int64_t));
    int64_t * dest = mymalloc("dest", destsize * sizeof(int64_t));

    int seed = 9999 * ThisTask;
    generate(src, srcsize, bits, seed);

    int64_t srcsum = checksum(src, srcsize, MPI_COMM_WORLD);

    {
        double start = MPI_Wtime();

        mpsort_mpi_newarray(src, srcsize,
                            dest, destsize,
                            sizeof(int64_t),
                            radix_int, 1, NULL,
                            MPI_COMM_WORLD);

        MPI_Barrier(MPI_COMM_WORLD);
        double end = MPI_Wtime();

        int64_t destsum = checksum(dest, destsize, MPI_COMM_WORLD);

        if(destsum != srcsum) {
            endrun(5, "MPSort checksum is inconsistent.\n");
            MPI_Abort(MPI_COMM_WORLD, -1);
        }

        check_sorted(dest, destsize, MPI_COMM_WORLD);

        message(0, "MPSort total time: %g\n", end - start);
//         if(ThisTask == 0) {
//             mpsort_mpi_report_last_run();
//         }
    }

    myfree(dest);
    myfree(src);
}

static void
test_mpsort_bits(void ** state)
{
    message(0, "16 bits!\n");
    /* With whatever gather we like*/
    do_mpsort_test(2000, 16, 0, -1);
    message(0, "32 bits!\n");
    do_mpsort_test(2000, 32, 0, -1);
    message(0, "64 bits!\n");
    do_mpsort_test(2000, 64, 0, -1);
}

static void
test_mpsort_stagger(void ** state)
{
    /* With stagger*/
    do_mpsort_test(2000, 32, 1, -1);
    /* Use a number that doesn't divide evenly so we get a different destsize*/
    do_mpsort_test(1999, 32, 0, -1);
    /* Empty*/
    do_mpsort_test(0, 32, 0, -1);

}

static void
test_mpsort_gather(void ** state)
{
    /* With forced gather*/
    do_mpsort_test(2000, 32, 0, 1);
    /* Without forced gather*/
    do_mpsort_test(2000, 32, 0, 0);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_mpsort_bits),
        cmocka_unit_test(test_mpsort_stagger),
        cmocka_unit_test(test_mpsort_gather),
    };
    return cmocka_run_group_tests_mpi(tests, NULL, NULL);
}
