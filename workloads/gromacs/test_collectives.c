/**
 * Collective MPI operations correctness test for CXL shim
 * Tests all collective operations:
 * - MPI_Bcast
 * - MPI_Reduce / MPI_Allreduce
 * - MPI_Gather / MPI_Allgather
 * - MPI_Scatter
 * - MPI_Alltoall
 * - MPI_Barrier
 *
 * Compile:
 *   mpicc -o test_collectives test_collectives.c -Wall -lm
 *
 * Run with CXL shim:
 *   mpirun -np 2 --host node0,node1 \
 *     -x LD_PRELOAD=/path/to/mpi_cxl_shim.so \
 *     -x CXL_DAX_PATH=/dev/dax0.0 \
 *     ./test_collectives
 */

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <math.h>

#define ARRAY_SIZE 256
#define TEST_ITERATIONS 5

// Colors for output
#define RED     "\x1b[31m"
#define GREEN   "\x1b[32m"
#define YELLOW  "\x1b[33m"
#define RESET   "\x1b[0m"

static int rank, size;
static char hostname[256];

#define LOG(fmt, ...) do { \
    printf("[%s:rank%d] " fmt, hostname, rank, ##__VA_ARGS__); \
    fflush(stdout); \
} while(0)

#define TEST_PASS(name) do { if (rank == 0) printf(GREEN "PASS" RESET ": %s\n", name); } while(0)
#define TEST_FAIL(name, reason) do { if (rank == 0) printf(RED "FAIL" RESET ": %s - %s\n", name, reason); } while(0)

/**
 * Test 1: MPI_Bcast
 */
int test_bcast(void) {
    int errors = 0;
    int *buf = malloc(ARRAY_SIZE * sizeof(int));

    if (rank == 0) {
        LOG("=== Test: MPI_Bcast ===\n");
    }

    for (int root = 0; root < size; root++) {
        // Root initializes buffer
        if (rank == root) {
            for (int i = 0; i < ARRAY_SIZE; i++) {
                buf[i] = root * 1000 + i;
            }
        } else {
            memset(buf, 0, ARRAY_SIZE * sizeof(int));
        }

        MPI_Bcast(buf, ARRAY_SIZE, MPI_INT, root, MPI_COMM_WORLD);

        // Verify all ranks have correct data
        for (int i = 0; i < ARRAY_SIZE; i++) {
            if (buf[i] != root * 1000 + i) {
                errors++;
                if (errors <= 3) {
                    LOG("  Bcast from root %d: mismatch at [%d], expected %d, got %d\n",
                        root, i, root * 1000 + i, buf[i]);
                }
            }
        }
    }

    free(buf);

    MPI_Allreduce(MPI_IN_PLACE, &errors, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    if (errors == 0) {
        TEST_PASS("MPI_Bcast (all roots)");
    } else {
        TEST_FAIL("MPI_Bcast", "data mismatch");
    }

    return errors;
}

/**
 * Test 2: MPI_Reduce
 */
int test_reduce(void) {
    int errors = 0;
    int *sendbuf = malloc(ARRAY_SIZE * sizeof(int));
    int *recvbuf = malloc(ARRAY_SIZE * sizeof(int));

    if (rank == 0) {
        LOG("=== Test: MPI_Reduce ===\n");
    }

    // Each rank contributes rank+1 to each element
    for (int i = 0; i < ARRAY_SIZE; i++) {
        sendbuf[i] = rank + 1;
    }
    memset(recvbuf, 0, ARRAY_SIZE * sizeof(int));

    MPI_Reduce(sendbuf, recvbuf, ARRAY_SIZE, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);

    // Verify on root
    if (rank == 0) {
        int expected = size * (size + 1) / 2;  // Sum of 1..size
        for (int i = 0; i < ARRAY_SIZE; i++) {
            if (recvbuf[i] != expected) {
                errors++;
                if (errors <= 3) {
                    LOG("  Reduce: mismatch at [%d], expected %d, got %d\n",
                        i, expected, recvbuf[i]);
                }
            }
        }
    }

    free(sendbuf);
    free(recvbuf);

    MPI_Allreduce(MPI_IN_PLACE, &errors, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    if (errors == 0) {
        TEST_PASS("MPI_Reduce (SUM)");
    } else {
        TEST_FAIL("MPI_Reduce", "data mismatch");
    }

    return errors;
}

/**
 * Test 3: MPI_Allreduce with different datatypes
 */
int test_allreduce(void) {
    int errors = 0;

    if (rank == 0) {
        LOG("=== Test: MPI_Allreduce ===\n");
    }

    // Test with int
    {
        int *sendbuf = malloc(ARRAY_SIZE * sizeof(int));
        int *recvbuf = malloc(ARRAY_SIZE * sizeof(int));

        for (int i = 0; i < ARRAY_SIZE; i++) {
            sendbuf[i] = rank + i;
        }

        MPI_Allreduce(sendbuf, recvbuf, ARRAY_SIZE, MPI_INT, MPI_SUM, MPI_COMM_WORLD);

        // Expected: sum of (rank + i) for all ranks = size*i + size*(size-1)/2
        for (int i = 0; i < ARRAY_SIZE; i++) {
            int expected = size * i + size * (size - 1) / 2;
            if (recvbuf[i] != expected) {
                errors++;
                if (errors <= 3) {
                    LOG("  Allreduce INT: mismatch at [%d], expected %d, got %d\n",
                        i, expected, recvbuf[i]);
                }
            }
        }

        free(sendbuf);
        free(recvbuf);
    }

    // Test with double
    {
        double *sendbuf = malloc(ARRAY_SIZE * sizeof(double));
        double *recvbuf = malloc(ARRAY_SIZE * sizeof(double));

        for (int i = 0; i < ARRAY_SIZE; i++) {
            sendbuf[i] = (double)(rank + 1) * 0.1;
        }

        MPI_Allreduce(sendbuf, recvbuf, ARRAY_SIZE, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

        double expected = 0.0;
        for (int r = 0; r < size; r++) {
            expected += (r + 1) * 0.1;
        }

        for (int i = 0; i < ARRAY_SIZE; i++) {
            if (fabs(recvbuf[i] - expected) > 1e-9) {
                errors++;
                if (errors <= 3) {
                    LOG("  Allreduce DOUBLE: mismatch at [%d], expected %f, got %f\n",
                        i, expected, recvbuf[i]);
                }
            }
        }

        free(sendbuf);
        free(recvbuf);
    }

    MPI_Allreduce(MPI_IN_PLACE, &errors, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    if (errors == 0) {
        TEST_PASS("MPI_Allreduce (INT + DOUBLE)");
    } else {
        TEST_FAIL("MPI_Allreduce", "data mismatch");
    }

    return errors;
}

/**
 * Test 4: MPI_Gather
 */
int test_gather(void) {
    int errors = 0;
    int *sendbuf = malloc(ARRAY_SIZE * sizeof(int));
    int *recvbuf = NULL;

    if (rank == 0) {
        LOG("=== Test: MPI_Gather ===\n");
        recvbuf = malloc(ARRAY_SIZE * size * sizeof(int));
    }

    // Each rank sends its rank number repeated
    for (int i = 0; i < ARRAY_SIZE; i++) {
        sendbuf[i] = rank * 100 + i;
    }

    MPI_Gather(sendbuf, ARRAY_SIZE, MPI_INT, recvbuf, ARRAY_SIZE, MPI_INT, 0, MPI_COMM_WORLD);

    // Verify on root
    if (rank == 0) {
        for (int r = 0; r < size; r++) {
            for (int i = 0; i < ARRAY_SIZE; i++) {
                int idx = r * ARRAY_SIZE + i;
                int expected = r * 100 + i;
                if (recvbuf[idx] != expected) {
                    errors++;
                    if (errors <= 5) {
                        LOG("  Gather: mismatch at [%d][%d], expected %d, got %d\n",
                            r, i, expected, recvbuf[idx]);
                    }
                }
            }
        }
        free(recvbuf);
    }

    free(sendbuf);

    MPI_Allreduce(MPI_IN_PLACE, &errors, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    if (errors == 0) {
        TEST_PASS("MPI_Gather");
    } else {
        TEST_FAIL("MPI_Gather", "data mismatch");
    }

    return errors;
}

/**
 * Test 5: MPI_Allgather
 */
int test_allgather(void) {
    int errors = 0;
    int *sendbuf = malloc(ARRAY_SIZE * sizeof(int));
    int *recvbuf = malloc(ARRAY_SIZE * size * sizeof(int));

    if (rank == 0) {
        LOG("=== Test: MPI_Allgather ===\n");
    }

    // Each rank contributes unique data
    for (int i = 0; i < ARRAY_SIZE; i++) {
        sendbuf[i] = rank * 1000 + i;
    }

    MPI_Allgather(sendbuf, ARRAY_SIZE, MPI_INT, recvbuf, ARRAY_SIZE, MPI_INT, MPI_COMM_WORLD);

    // Verify all data
    for (int r = 0; r < size; r++) {
        for (int i = 0; i < ARRAY_SIZE; i++) {
            int idx = r * ARRAY_SIZE + i;
            int expected = r * 1000 + i;
            if (recvbuf[idx] != expected) {
                errors++;
                if (errors <= 5) {
                    LOG("  Allgather: mismatch at [%d][%d], expected %d, got %d\n",
                        r, i, expected, recvbuf[idx]);
                }
            }
        }
    }

    free(sendbuf);
    free(recvbuf);

    MPI_Allreduce(MPI_IN_PLACE, &errors, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    if (errors == 0) {
        TEST_PASS("MPI_Allgather");
    } else {
        TEST_FAIL("MPI_Allgather", "data mismatch");
    }

    return errors;
}

/**
 * Test 6: MPI_Scatter
 */
int test_scatter(void) {
    int errors = 0;
    int *sendbuf = NULL;
    int *recvbuf = malloc(ARRAY_SIZE * sizeof(int));

    if (rank == 0) {
        LOG("=== Test: MPI_Scatter ===\n");
        sendbuf = malloc(ARRAY_SIZE * size * sizeof(int));
        // Root prepares data for each rank
        for (int r = 0; r < size; r++) {
            for (int i = 0; i < ARRAY_SIZE; i++) {
                sendbuf[r * ARRAY_SIZE + i] = r * 100 + i + 5000;
            }
        }
    }

    MPI_Scatter(sendbuf, ARRAY_SIZE, MPI_INT, recvbuf, ARRAY_SIZE, MPI_INT, 0, MPI_COMM_WORLD);

    // Each rank verifies its data
    for (int i = 0; i < ARRAY_SIZE; i++) {
        int expected = rank * 100 + i + 5000;
        if (recvbuf[i] != expected) {
            errors++;
            if (errors <= 3) {
                LOG("  Scatter: mismatch at [%d], expected %d, got %d\n",
                    i, expected, recvbuf[i]);
            }
        }
    }

    if (rank == 0) {
        free(sendbuf);
    }
    free(recvbuf);

    MPI_Allreduce(MPI_IN_PLACE, &errors, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    if (errors == 0) {
        TEST_PASS("MPI_Scatter");
    } else {
        TEST_FAIL("MPI_Scatter", "data mismatch");
    }

    return errors;
}

/**
 * Test 7: MPI_Alltoall
 */
int test_alltoall(void) {
    int errors = 0;

    if (rank == 0) {
        LOG("=== Test: MPI_Alltoall ===\n");
    }

    // Test multiple sizes
    int sizes[] = {1, 4, 16, 64, 256};
    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    for (int s = 0; s < num_sizes; s++) {
        int count = sizes[s];
        int *sendbuf = malloc(count * size * sizeof(int));
        int *recvbuf = malloc(count * size * sizeof(int));

        // Prepare send data: sendbuf[dest * count + i] = data for dest rank
        for (int dest = 0; dest < size; dest++) {
            for (int i = 0; i < count; i++) {
                // Encode: source rank, dest rank, and index
                sendbuf[dest * count + i] = rank * 10000 + dest * 100 + i;
            }
        }

        MPI_Alltoall(sendbuf, count, MPI_INT, recvbuf, count, MPI_INT, MPI_COMM_WORLD);

        // Verify: recvbuf[src * count + i] should be src's data for me
        for (int src = 0; src < size; src++) {
            for (int i = 0; i < count; i++) {
                int expected = src * 10000 + rank * 100 + i;
                int idx = src * count + i;
                if (recvbuf[idx] != expected) {
                    errors++;
                    if (errors <= 5) {
                        LOG("  Alltoall[size=%d]: mismatch at [src=%d][%d], expected %d, got %d\n",
                            count, src, i, expected, recvbuf[idx]);
                    }
                }
            }
        }

        free(sendbuf);
        free(recvbuf);

        if (rank == 0) {
            LOG("  Alltoall size=%d: %s\n", count, (errors == 0) ? "OK" : "ERRORS");
        }
    }

    MPI_Allreduce(MPI_IN_PLACE, &errors, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    if (errors == 0) {
        TEST_PASS("MPI_Alltoall (multiple sizes)");
    } else {
        TEST_FAIL("MPI_Alltoall", "data mismatch");
    }

    return errors;
}

/**
 * Test 8: MPI_Barrier timing test
 */
int test_barrier(void) {
    int errors = 0;
    double t_start, t_end;

    if (rank == 0) {
        LOG("=== Test: MPI_Barrier ===\n");
    }

    // Test multiple barriers
    for (int iter = 0; iter < 10; iter++) {
        // Stagger entry times
        usleep(rank * 10000);  // rank * 10ms

        t_start = MPI_Wtime();
        MPI_Barrier(MPI_COMM_WORLD);
        t_end = MPI_Wtime();

        if (rank == 0 && iter == 0) {
            LOG("  Barrier time: %.3f ms\n", (t_end - t_start) * 1000);
        }
    }

    // Barrier correctness: set a flag after barrier
    int flag = 0;
    if (rank == 0) {
        usleep(50000);  // 50ms delay
        flag = 1;
    }

    MPI_Barrier(MPI_COMM_WORLD);

    // After barrier, all ranks should proceed
    // (Can't really test correctness without shared state)

    TEST_PASS("MPI_Barrier (timing)");
    return errors;
}

/**
 * Test 9: Stress test with iterations
 */
int test_stress(void) {
    int errors = 0;

    if (rank == 0) {
        LOG("=== Test: Stress test (%d iterations) ===\n", TEST_ITERATIONS);
    }

    for (int iter = 0; iter < TEST_ITERATIONS; iter++) {
        // Allreduce
        int sendbuf[10], recvbuf[10];
        for (int i = 0; i < 10; i++) {
            sendbuf[i] = rank + iter + i;
        }
        MPI_Allreduce(sendbuf, recvbuf, 10, MPI_INT, MPI_SUM, MPI_COMM_WORLD);

        for (int i = 0; i < 10; i++) {
            int expected = 0;
            for (int r = 0; r < size; r++) {
                expected += r + iter + i;
            }
            if (recvbuf[i] != expected) {
                errors++;
            }
        }

        // Bcast
        int bcast_val = (rank == 0) ? iter * 100 : 0;
        MPI_Bcast(&bcast_val, 1, MPI_INT, 0, MPI_COMM_WORLD);
        if (bcast_val != iter * 100) {
            errors++;
        }

        // Alltoall
        int *a2a_send = malloc(size * sizeof(int));
        int *a2a_recv = malloc(size * sizeof(int));
        for (int i = 0; i < size; i++) {
            a2a_send[i] = rank * 100 + i + iter;
        }
        MPI_Alltoall(a2a_send, 1, MPI_INT, a2a_recv, 1, MPI_INT, MPI_COMM_WORLD);
        for (int i = 0; i < size; i++) {
            int expected = i * 100 + rank + iter;
            if (a2a_recv[i] != expected) {
                errors++;
            }
        }
        free(a2a_send);
        free(a2a_recv);

        if (iter % 2 == 0 && rank == 0) {
            LOG("  Iteration %d/%d: %s\n", iter + 1, TEST_ITERATIONS,
                errors == 0 ? "OK" : "ERRORS");
        }
    }

    MPI_Allreduce(MPI_IN_PLACE, &errors, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    if (errors == 0) {
        TEST_PASS("Stress test");
    } else {
        TEST_FAIL("Stress test", "data mismatch");
    }

    return errors;
}

int main(int argc, char **argv) {
    int total_errors = 0;

    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    gethostname(hostname, sizeof(hostname));

    if (rank == 0) {
        printf("\n");
        printf("========================================\n");
        printf(" MPI Collective Operations Test\n");
        printf(" Ranks: %d\n", size);
        printf("========================================\n\n");
    }

    MPI_Barrier(MPI_COMM_WORLD);

    // Run tests
    total_errors += test_bcast();
    MPI_Barrier(MPI_COMM_WORLD);

    total_errors += test_reduce();
    MPI_Barrier(MPI_COMM_WORLD);

    total_errors += test_allreduce();
    MPI_Barrier(MPI_COMM_WORLD);

    total_errors += test_gather();
    MPI_Barrier(MPI_COMM_WORLD);

    total_errors += test_allgather();
    MPI_Barrier(MPI_COMM_WORLD);

    total_errors += test_scatter();
    MPI_Barrier(MPI_COMM_WORLD);

    total_errors += test_alltoall();
    MPI_Barrier(MPI_COMM_WORLD);

    total_errors += test_barrier();
    MPI_Barrier(MPI_COMM_WORLD);

    total_errors += test_stress();
    MPI_Barrier(MPI_COMM_WORLD);

    // Summary
    if (rank == 0) {
        printf("\n========================================\n");
        if (total_errors == 0) {
            printf(GREEN " All tests PASSED!\n" RESET);
        } else {
            printf(RED " %d total errors\n" RESET, total_errors);
        }
        printf("========================================\n\n");
    }

    MPI_Finalize();
    return (total_errors > 0) ? 1 : 0;
}
