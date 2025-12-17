/**
 * Point-to-Point MPI operations correctness test for CXL shim
 * Tests Send/Recv operations:
 * - MPI_Send / MPI_Recv (blocking)
 * - MPI_Isend / MPI_Irecv (non-blocking)
 * - MPI_Sendrecv
 * - Various message sizes
 * - Ring communication pattern
 *
 * Compile:
 *   mpicc -o test_p2p test_p2p.c -Wall
 *
 * Run with CXL shim:
 *   mpirun -np 2 --host node0,node1 \
 *     -x LD_PRELOAD=/path/to/mpi_cxl_shim.so \
 *     -x CXL_DAX_PATH=/dev/dax0.0 \
 *     ./test_p2p
 */

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#define TEST_ITERATIONS 10

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
 * Test 1: Basic blocking Send/Recv
 */
int test_blocking_sendrecv(void) {
    int errors = 0;
    int msg_sizes[] = {1, 8, 64, 256, 1024, 4096, 16384};
    int num_sizes = sizeof(msg_sizes) / sizeof(msg_sizes[0]);

    if (rank == 0) {
        LOG("=== Test: Blocking Send/Recv ===\n");
    }

    for (int s = 0; s < num_sizes; s++) {
        int msg_size = msg_sizes[s];
        int *sendbuf = malloc(msg_size * sizeof(int));
        int *recvbuf = malloc(msg_size * sizeof(int));

        // Initialize send buffer with pattern
        for (int i = 0; i < msg_size; i++) {
            sendbuf[i] = rank * 10000 + i;
        }
        memset(recvbuf, 0, msg_size * sizeof(int));

        int partner = (rank + 1) % size;

        if (rank % 2 == 0) {
            // Even ranks send first, then receive
            MPI_Send(sendbuf, msg_size, MPI_INT, partner, 100 + s, MPI_COMM_WORLD);
            MPI_Recv(recvbuf, msg_size, MPI_INT, partner, 100 + s, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        } else {
            // Odd ranks receive first, then send
            MPI_Recv(recvbuf, msg_size, MPI_INT, partner, 100 + s, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            MPI_Send(sendbuf, msg_size, MPI_INT, partner, 100 + s, MPI_COMM_WORLD);
        }

        // Verify received data
        for (int i = 0; i < msg_size; i++) {
            int expected = partner * 10000 + i;
            if (recvbuf[i] != expected) {
                errors++;
                if (errors <= 5) {
                    LOG("  Size %d: mismatch at [%d], expected %d, got %d\n",
                        msg_size, i, expected, recvbuf[i]);
                }
            }
        }

        free(sendbuf);
        free(recvbuf);

        MPI_Barrier(MPI_COMM_WORLD);
    }

    MPI_Allreduce(MPI_IN_PLACE, &errors, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    if (errors == 0) {
        TEST_PASS("Blocking Send/Recv (multiple sizes)");
    } else {
        TEST_FAIL("Blocking Send/Recv", "data mismatch");
    }

    return errors;
}

/**
 * Test 2: Non-blocking Isend/Irecv
 */
int test_nonblocking_sendrecv(void) {
    int errors = 0;
    int msg_sizes[] = {1, 64, 1024, 4096};
    int num_sizes = sizeof(msg_sizes) / sizeof(msg_sizes[0]);

    if (rank == 0) {
        LOG("=== Test: Non-blocking Isend/Irecv ===\n");
    }

    for (int s = 0; s < num_sizes; s++) {
        int msg_size = msg_sizes[s];
        int *sendbuf = malloc(msg_size * sizeof(int));
        int *recvbuf = malloc(msg_size * sizeof(int));
        MPI_Request reqs[2];

        for (int i = 0; i < msg_size; i++) {
            sendbuf[i] = rank * 1000 + i + msg_size;
        }
        memset(recvbuf, 0, msg_size * sizeof(int));

        int partner = (rank + 1) % size;

        // Post non-blocking operations
        MPI_Irecv(recvbuf, msg_size, MPI_INT, partner, 200 + s, MPI_COMM_WORLD, &reqs[0]);
        MPI_Isend(sendbuf, msg_size, MPI_INT, partner, 200 + s, MPI_COMM_WORLD, &reqs[1]);

        // Wait for completion
        MPI_Waitall(2, reqs, MPI_STATUSES_IGNORE);

        // Verify
        for (int i = 0; i < msg_size; i++) {
            int expected = partner * 1000 + i + msg_size;
            if (recvbuf[i] != expected) {
                errors++;
                if (errors <= 5) {
                    LOG("  Size %d: mismatch at [%d], expected %d, got %d\n",
                        msg_size, i, expected, recvbuf[i]);
                }
            }
        }

        free(sendbuf);
        free(recvbuf);
    }

    MPI_Allreduce(MPI_IN_PLACE, &errors, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    if (errors == 0) {
        TEST_PASS("Non-blocking Isend/Irecv");
    } else {
        TEST_FAIL("Non-blocking Isend/Irecv", "data mismatch");
    }

    return errors;
}

/**
 * Test 3: MPI_Sendrecv
 */
int test_sendrecv(void) {
    int errors = 0;
    int msg_size = 512;

    if (rank == 0) {
        LOG("=== Test: MPI_Sendrecv ===\n");
    }

    int *sendbuf = malloc(msg_size * sizeof(int));
    int *recvbuf = malloc(msg_size * sizeof(int));

    for (int i = 0; i < msg_size; i++) {
        sendbuf[i] = rank * 5000 + i;
    }
    memset(recvbuf, 0, msg_size * sizeof(int));

    int partner = (rank + 1) % size;

    MPI_Sendrecv(sendbuf, msg_size, MPI_INT, partner, 300,
                 recvbuf, msg_size, MPI_INT, partner, 300,
                 MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    for (int i = 0; i < msg_size; i++) {
        int expected = partner * 5000 + i;
        if (recvbuf[i] != expected) {
            errors++;
            if (errors <= 5) {
                LOG("  Mismatch at [%d], expected %d, got %d\n",
                    i, expected, recvbuf[i]);
            }
        }
    }

    free(sendbuf);
    free(recvbuf);

    MPI_Allreduce(MPI_IN_PLACE, &errors, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    if (errors == 0) {
        TEST_PASS("MPI_Sendrecv");
    } else {
        TEST_FAIL("MPI_Sendrecv", "data mismatch");
    }

    return errors;
}

/**
 * Test 4: Ring communication pattern
 */
int test_ring(void) {
    int errors = 0;
    int msg_size = 256;

    if (rank == 0) {
        LOG("=== Test: Ring communication ===\n");
    }

    int *sendbuf = malloc(msg_size * sizeof(int));
    int *recvbuf = malloc(msg_size * sizeof(int));

    int next = (rank + 1) % size;
    int prev = (rank - 1 + size) % size;

    // Multiple rounds of ring communication
    for (int round = 0; round < 5; round++) {
        for (int i = 0; i < msg_size; i++) {
            sendbuf[i] = rank * 1000 + round * 100 + i;
        }
        memset(recvbuf, 0, msg_size * sizeof(int));

        MPI_Sendrecv(sendbuf, msg_size, MPI_INT, next, 400 + round,
                     recvbuf, msg_size, MPI_INT, prev, 400 + round,
                     MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        for (int i = 0; i < msg_size; i++) {
            int expected = prev * 1000 + round * 100 + i;
            if (recvbuf[i] != expected) {
                errors++;
            }
        }
    }

    free(sendbuf);
    free(recvbuf);

    MPI_Allreduce(MPI_IN_PLACE, &errors, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    if (errors == 0) {
        TEST_PASS("Ring communication");
    } else {
        TEST_FAIL("Ring communication", "data mismatch");
    }

    return errors;
}

/**
 * Test 5: Probe and MPI_Get_count
 */
int test_probe(void) {
    int errors = 0;
    int msg_sizes[] = {10, 100, 500};
    int num_sizes = sizeof(msg_sizes) / sizeof(msg_sizes[0]);

    if (rank == 0) {
        LOG("=== Test: MPI_Probe ===\n");
    }

    for (int s = 0; s < num_sizes; s++) {
        int msg_size = msg_sizes[s];
        int *sendbuf = malloc(msg_size * sizeof(int));
        int *recvbuf = malloc(msg_size * sizeof(int));

        for (int i = 0; i < msg_size; i++) {
            sendbuf[i] = i + s * 1000;
        }

        int partner = (rank + 1) % size;

        if (rank % 2 == 0) {
            MPI_Send(sendbuf, msg_size, MPI_INT, partner, 500 + s, MPI_COMM_WORLD);
        } else {
            MPI_Status status;
            int count;

            // Probe for message
            MPI_Probe(partner, 500 + s, MPI_COMM_WORLD, &status);
            MPI_Get_count(&status, MPI_INT, &count);

            if (count != msg_size) {
                errors++;
                LOG("  Probe size %d: expected count %d, got %d\n", msg_size, msg_size, count);
            }

            // Now receive
            MPI_Recv(recvbuf, count, MPI_INT, partner, 500 + s, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            for (int i = 0; i < count; i++) {
                if (recvbuf[i] != i + s * 1000) {
                    errors++;
                }
            }
        }

        // Swap roles
        MPI_Barrier(MPI_COMM_WORLD);

        if (rank % 2 == 1) {
            MPI_Send(sendbuf, msg_size, MPI_INT, partner, 600 + s, MPI_COMM_WORLD);
        } else {
            MPI_Status status;
            int count;

            MPI_Probe(partner, 600 + s, MPI_COMM_WORLD, &status);
            MPI_Get_count(&status, MPI_INT, &count);

            if (count != msg_size) {
                errors++;
            }

            MPI_Recv(recvbuf, count, MPI_INT, partner, 600 + s, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        }

        free(sendbuf);
        free(recvbuf);
    }

    MPI_Allreduce(MPI_IN_PLACE, &errors, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    if (errors == 0) {
        TEST_PASS("MPI_Probe");
    } else {
        TEST_FAIL("MPI_Probe", "data mismatch or wrong count");
    }

    return errors;
}

/**
 * Test 6: Large message transfer
 */
int test_large_message(void) {
    int errors = 0;
    size_t msg_size = 1024 * 1024;  // 1M integers = 4MB

    if (rank == 0) {
        LOG("=== Test: Large message (4MB) ===\n");
    }

    int *sendbuf = malloc(msg_size * sizeof(int));
    int *recvbuf = malloc(msg_size * sizeof(int));

    if (!sendbuf || !recvbuf) {
        if (sendbuf) free(sendbuf);
        if (recvbuf) free(recvbuf);
        TEST_FAIL("Large message", "allocation failed");
        return 1;
    }

    for (size_t i = 0; i < msg_size; i++) {
        sendbuf[i] = (int)(i % 100000) + rank * 100000;
    }
    memset(recvbuf, 0, msg_size * sizeof(int));

    int partner = (rank + 1) % size;

    double t_start = MPI_Wtime();

    if (rank % 2 == 0) {
        MPI_Send(sendbuf, msg_size, MPI_INT, partner, 700, MPI_COMM_WORLD);
        MPI_Recv(recvbuf, msg_size, MPI_INT, partner, 700, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    } else {
        MPI_Recv(recvbuf, msg_size, MPI_INT, partner, 700, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        MPI_Send(sendbuf, msg_size, MPI_INT, partner, 700, MPI_COMM_WORLD);
    }

    double t_end = MPI_Wtime();

    // Verify
    for (size_t i = 0; i < msg_size; i++) {
        int expected = (int)(i % 100000) + partner * 100000;
        if (recvbuf[i] != expected) {
            errors++;
            if (errors <= 5) {
                LOG("  Mismatch at [%zu], expected %d, got %d\n", i, expected, recvbuf[i]);
            }
        }
    }

    if (rank == 0) {
        double mbps = (msg_size * sizeof(int) * 2) / (t_end - t_start) / 1e6;
        LOG("  Transfer rate: %.2f MB/s\n", mbps);
    }

    free(sendbuf);
    free(recvbuf);

    MPI_Allreduce(MPI_IN_PLACE, &errors, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    if (errors == 0) {
        TEST_PASS("Large message (4MB)");
    } else {
        TEST_FAIL("Large message", "data mismatch");
    }

    return errors;
}

/**
 * Test 7: Stress test with many small messages
 */
int test_stress(void) {
    int errors = 0;
    int iterations = 100;
    int msg_size = 64;

    if (rank == 0) {
        LOG("=== Test: Stress test (%d iterations) ===\n", iterations);
    }

    int *sendbuf = malloc(msg_size * sizeof(int));
    int *recvbuf = malloc(msg_size * sizeof(int));
    int partner = (rank + 1) % size;

    double t_start = MPI_Wtime();

    for (int iter = 0; iter < iterations; iter++) {
        for (int i = 0; i < msg_size; i++) {
            sendbuf[i] = rank * 10000 + iter * 100 + i;
        }

        if (rank % 2 == 0) {
            MPI_Send(sendbuf, msg_size, MPI_INT, partner, 800, MPI_COMM_WORLD);
            MPI_Recv(recvbuf, msg_size, MPI_INT, partner, 800, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        } else {
            MPI_Recv(recvbuf, msg_size, MPI_INT, partner, 800, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            MPI_Send(sendbuf, msg_size, MPI_INT, partner, 800, MPI_COMM_WORLD);
        }

        for (int i = 0; i < msg_size; i++) {
            int expected = partner * 10000 + iter * 100 + i;
            if (recvbuf[i] != expected) {
                errors++;
            }
        }
    }

    double t_end = MPI_Wtime();

    if (rank == 0) {
        LOG("  %d roundtrips in %.3f ms (%.2f us/msg)\n",
            iterations, (t_end - t_start) * 1000,
            (t_end - t_start) * 1e6 / iterations);
    }

    free(sendbuf);
    free(recvbuf);

    MPI_Allreduce(MPI_IN_PLACE, &errors, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    if (errors == 0) {
        TEST_PASS("Stress test");
    } else {
        TEST_FAIL("Stress test", "data mismatch");
    }

    return errors;
}

/**
 * Test 8: Different datatypes
 */
int test_datatypes(void) {
    int errors = 0;

    if (rank == 0) {
        LOG("=== Test: Different datatypes ===\n");
    }

    int partner = (rank + 1) % size;

    // Test double
    {
        double sendbuf[100], recvbuf[100];
        for (int i = 0; i < 100; i++) {
            sendbuf[i] = rank * 1000.5 + i * 0.1;
        }

        if (rank % 2 == 0) {
            MPI_Send(sendbuf, 100, MPI_DOUBLE, partner, 900, MPI_COMM_WORLD);
            MPI_Recv(recvbuf, 100, MPI_DOUBLE, partner, 900, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        } else {
            MPI_Recv(recvbuf, 100, MPI_DOUBLE, partner, 900, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            MPI_Send(sendbuf, 100, MPI_DOUBLE, partner, 900, MPI_COMM_WORLD);
        }

        for (int i = 0; i < 100; i++) {
            double expected = partner * 1000.5 + i * 0.1;
            if (recvbuf[i] != expected) {
                errors++;
            }
        }
    }

    // Test char
    {
        char sendbuf[256], recvbuf[256];
        for (int i = 0; i < 256; i++) {
            sendbuf[i] = (char)((rank * 50 + i) % 256);
        }

        if (rank % 2 == 0) {
            MPI_Send(sendbuf, 256, MPI_CHAR, partner, 901, MPI_COMM_WORLD);
            MPI_Recv(recvbuf, 256, MPI_CHAR, partner, 901, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        } else {
            MPI_Recv(recvbuf, 256, MPI_CHAR, partner, 901, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            MPI_Send(sendbuf, 256, MPI_CHAR, partner, 901, MPI_COMM_WORLD);
        }

        for (int i = 0; i < 256; i++) {
            char expected = (char)((partner * 50 + i) % 256);
            if (recvbuf[i] != expected) {
                errors++;
            }
        }
    }

    // Test long long
    {
        long long sendbuf[50], recvbuf[50];
        for (int i = 0; i < 50; i++) {
            sendbuf[i] = (long long)rank * 1000000000LL + i;
        }

        if (rank % 2 == 0) {
            MPI_Send(sendbuf, 50, MPI_LONG_LONG, partner, 902, MPI_COMM_WORLD);
            MPI_Recv(recvbuf, 50, MPI_LONG_LONG, partner, 902, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        } else {
            MPI_Recv(recvbuf, 50, MPI_LONG_LONG, partner, 902, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            MPI_Send(sendbuf, 50, MPI_LONG_LONG, partner, 902, MPI_COMM_WORLD);
        }

        for (int i = 0; i < 50; i++) {
            long long expected = (long long)partner * 1000000000LL + i;
            if (recvbuf[i] != expected) {
                errors++;
            }
        }
    }

    MPI_Allreduce(MPI_IN_PLACE, &errors, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    if (errors == 0) {
        TEST_PASS("Different datatypes");
    } else {
        TEST_FAIL("Different datatypes", "data mismatch");
    }

    return errors;
}

int main(int argc, char **argv) {
    int total_errors = 0;

    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    gethostname(hostname, sizeof(hostname));

    if (size < 2) {
        if (rank == 0) {
            printf("This test requires at least 2 processes\n");
        }
        MPI_Finalize();
        return 1;
    }

    if (rank == 0) {
        printf("\n");
        printf("========================================\n");
        printf(" MPI Point-to-Point Operations Test\n");
        printf(" Ranks: %d\n", size);
        printf("========================================\n\n");
    }

    MPI_Barrier(MPI_COMM_WORLD);

    // Run tests
    total_errors += test_blocking_sendrecv();
    MPI_Barrier(MPI_COMM_WORLD);

    total_errors += test_nonblocking_sendrecv();
    MPI_Barrier(MPI_COMM_WORLD);

    total_errors += test_sendrecv();
    MPI_Barrier(MPI_COMM_WORLD);

    total_errors += test_ring();
    MPI_Barrier(MPI_COMM_WORLD);

    total_errors += test_probe();
    MPI_Barrier(MPI_COMM_WORLD);

    total_errors += test_large_message();
    MPI_Barrier(MPI_COMM_WORLD);

    total_errors += test_stress();
    MPI_Barrier(MPI_COMM_WORLD);

    total_errors += test_datatypes();
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
