/**
 * One-sided MPI operations test for CXL shim
 * Tests MPI RMA (Remote Memory Access) operations:
 * - MPI_Win_create / MPI_Win_allocate / MPI_Win_free
 * - MPI_Put / MPI_Get with fence synchronization
 * - MPI_Put / MPI_Get with lock/unlock synchronization
 * - MPI_Accumulate
 *
 * Compile:
 *   mpicc -o test_onesided test_onesided.c -Wall
 *
 * Run with CXL shim:
 *   mpirun -np 2 --host node0,node1 \
 *     -x LD_PRELOAD=/path/to/mpi_cxl_shim.so \
 *     -x CXL_DAX_PATH=/dev/dax0.0 \
 *     ./test_onesided
 */

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#define ARRAY_SIZE 1024
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

#define TEST_PASS(name) LOG(GREEN "PASS" RESET ": %s\n", name)
#define TEST_FAIL(name, reason) LOG(RED "FAIL" RESET ": %s - %s\n", name, reason)

/**
 * Test 1: MPI_Win_create with Put/Get using fence synchronization
 */
int test_win_create_fence(void) {
    int *local_buf;
    MPI_Win win;
    int errors = 0;

    LOG("=== Test: Win_create with fence ===\n");

    // Allocate local buffer
    local_buf = (int *)malloc(ARRAY_SIZE * sizeof(int));
    if (!local_buf) {
        TEST_FAIL("win_create_fence", "malloc failed");
        return 1;
    }

    // Initialize: rank 0 has data, rank 1 has zeros
    for (int i = 0; i < ARRAY_SIZE; i++) {
        local_buf[i] = (rank == 0) ? (i + 1) * 100 : 0;
    }

    // Create window
    MPI_Win_create(local_buf, ARRAY_SIZE * sizeof(int), sizeof(int),
                   MPI_INFO_NULL, MPI_COMM_WORLD, &win);

    // Fence to start epoch
    MPI_Win_fence(0, win);

    if (rank == 1) {
        // Rank 1 gets data from rank 0
        MPI_Get(local_buf, ARRAY_SIZE, MPI_INT, 0, 0, ARRAY_SIZE, MPI_INT, win);
    }

    // Fence to complete epoch
    MPI_Win_fence(0, win);

    // Verify on rank 1
    if (rank == 1) {
        for (int i = 0; i < ARRAY_SIZE; i++) {
            if (local_buf[i] != (i + 1) * 100) {
                errors++;
                if (errors <= 5) {
                    LOG("  Mismatch at [%d]: expected %d, got %d\n",
                        i, (i + 1) * 100, local_buf[i]);
                }
            }
        }
    }

    MPI_Win_free(&win);
    free(local_buf);

    MPI_Allreduce(MPI_IN_PLACE, &errors, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);

    if (rank == 0) {
        if (errors == 0) {
            TEST_PASS("win_create_fence (Get)");
        } else {
            TEST_FAIL("win_create_fence (Get)", "data mismatch");
        }
    }

    return errors;
}

/**
 * Test 2: MPI_Win_allocate with Put/Get using fence synchronization
 */
int test_win_allocate_fence(void) {
    int *win_buf;
    MPI_Win win;
    int errors = 0;

    LOG("=== Test: Win_allocate with fence ===\n");

    // Allocate window buffer
    MPI_Win_allocate(ARRAY_SIZE * sizeof(int), sizeof(int), MPI_INFO_NULL,
                     MPI_COMM_WORLD, &win_buf, &win);

    // Initialize window buffer
    for (int i = 0; i < ARRAY_SIZE; i++) {
        win_buf[i] = rank * 1000 + i;
    }

    MPI_Win_fence(0, win);

    if (rank == 0) {
        // Rank 0 puts data to rank 1
        int *send_buf = (int *)malloc(ARRAY_SIZE * sizeof(int));
        for (int i = 0; i < ARRAY_SIZE; i++) {
            send_buf[i] = 9999 - i;
        }
        MPI_Put(send_buf, ARRAY_SIZE, MPI_INT, 1, 0, ARRAY_SIZE, MPI_INT, win);
        free(send_buf);
    }

    MPI_Win_fence(0, win);

    // Verify on rank 1
    if (rank == 1) {
        for (int i = 0; i < ARRAY_SIZE; i++) {
            if (win_buf[i] != 9999 - i) {
                errors++;
                if (errors <= 5) {
                    LOG("  Mismatch at [%d]: expected %d, got %d\n",
                        i, 9999 - i, win_buf[i]);
                }
            }
        }
    }

    MPI_Win_free(&win);

    MPI_Allreduce(MPI_IN_PLACE, &errors, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);

    if (rank == 0) {
        if (errors == 0) {
            TEST_PASS("win_allocate_fence (Put)");
        } else {
            TEST_FAIL("win_allocate_fence (Put)", "data mismatch");
        }
    }

    return errors;
}

/**
 * Test 3: Put/Get with lock/unlock synchronization (passive target)
 */
int test_lock_unlock(void) {
    int *local_buf;
    MPI_Win win;
    int errors = 0;
    int target = (rank + 1) % size;

    LOG("=== Test: Lock/Unlock synchronization ===\n");

    local_buf = (int *)malloc(ARRAY_SIZE * sizeof(int));
    if (!local_buf) {
        TEST_FAIL("lock_unlock", "malloc failed");
        return 1;
    }

    // Initialize with rank-specific values
    for (int i = 0; i < ARRAY_SIZE; i++) {
        local_buf[i] = rank * 10000 + i;
    }

    MPI_Win_create(local_buf, ARRAY_SIZE * sizeof(int), sizeof(int),
                   MPI_INFO_NULL, MPI_COMM_WORLD, &win);

    MPI_Barrier(MPI_COMM_WORLD);

    // Each rank reads from the next rank using lock/unlock
    int *read_buf = (int *)malloc(ARRAY_SIZE * sizeof(int));
    memset(read_buf, 0, ARRAY_SIZE * sizeof(int));

    MPI_Win_lock(MPI_LOCK_SHARED, target, 0, win);
    MPI_Get(read_buf, ARRAY_SIZE, MPI_INT, target, 0, ARRAY_SIZE, MPI_INT, win);
    MPI_Win_unlock(target, win);

    // Verify
    for (int i = 0; i < ARRAY_SIZE; i++) {
        int expected = target * 10000 + i;
        if (read_buf[i] != expected) {
            errors++;
            if (errors <= 5) {
                LOG("  Mismatch at [%d]: expected %d, got %d\n",
                    i, expected, read_buf[i]);
            }
        }
    }

    free(read_buf);
    MPI_Win_free(&win);
    free(local_buf);

    MPI_Allreduce(MPI_IN_PLACE, &errors, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);

    if (rank == 0) {
        if (errors == 0) {
            TEST_PASS("lock_unlock (Get)");
        } else {
            TEST_FAIL("lock_unlock (Get)", "data mismatch");
        }
    }

    return errors;
}

/**
 * Test 4: MPI_Accumulate with MPI_SUM
 */
int test_accumulate(void) {
    int *local_buf;
    MPI_Win win;
    int errors = 0;

    LOG("=== Test: Accumulate (MPI_SUM) ===\n");

    local_buf = (int *)malloc(ARRAY_SIZE * sizeof(int));
    if (!local_buf) {
        TEST_FAIL("accumulate", "malloc failed");
        return 1;
    }

    // Initialize rank 0 with zeros, others with values
    for (int i = 0; i < ARRAY_SIZE; i++) {
        local_buf[i] = (rank == 0) ? 0 : (rank * 100 + i);
    }

    MPI_Win_create(local_buf, ARRAY_SIZE * sizeof(int), sizeof(int),
                   MPI_INFO_NULL, MPI_COMM_WORLD, &win);

    MPI_Win_fence(0, win);

    // All ranks (except 0) accumulate to rank 0
    if (rank != 0) {
        int *acc_buf = (int *)malloc(ARRAY_SIZE * sizeof(int));
        for (int i = 0; i < ARRAY_SIZE; i++) {
            acc_buf[i] = rank;  // Each rank adds its rank number
        }
        MPI_Accumulate(acc_buf, ARRAY_SIZE, MPI_INT, 0, 0, ARRAY_SIZE, MPI_INT,
                       MPI_SUM, win);
        free(acc_buf);
    }

    MPI_Win_fence(0, win);

    // Verify on rank 0: should have sum of all ranks (1 + 2 + ... + size-1)
    if (rank == 0) {
        int expected_sum = (size - 1) * size / 2;  // Sum of 1..size-1
        for (int i = 0; i < ARRAY_SIZE; i++) {
            if (local_buf[i] != expected_sum) {
                errors++;
                if (errors <= 5) {
                    LOG("  Mismatch at [%d]: expected %d, got %d\n",
                        i, expected_sum, local_buf[i]);
                }
            }
        }
    }

    MPI_Win_free(&win);
    free(local_buf);

    MPI_Allreduce(MPI_IN_PLACE, &errors, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);

    if (rank == 0) {
        if (errors == 0) {
            TEST_PASS("accumulate (MPI_SUM)");
        } else {
            TEST_FAIL("accumulate (MPI_SUM)", "data mismatch");
        }
    }

    return errors;
}

/**
 * Test 5: Bidirectional Put/Get
 * Each rank puts to next rank and gets from next rank
 * Ring pattern: rank N -> rank (N+1)%size
 */
int test_bidirectional(void) {
    int *win_buf;
    MPI_Win win;
    int errors = 0;

    LOG("=== Test: Bidirectional Put/Get ===\n");

    MPI_Win_allocate(ARRAY_SIZE * sizeof(int), sizeof(int), MPI_INFO_NULL,
                     MPI_COMM_WORLD, &win_buf, &win);

    // Initialize with rank-specific pattern
    for (int i = 0; i < ARRAY_SIZE; i++) {
        win_buf[i] = rank * 1000000 + i;
    }

    int *recv_buf = (int *)malloc(ARRAY_SIZE * sizeof(int));
    int *send_buf = (int *)malloc(ARRAY_SIZE * sizeof(int));

    for (int i = 0; i < ARRAY_SIZE; i++) {
        send_buf[i] = rank * 100 + i + 50000;
    }

    int put_target = (rank + 1) % size;  // We put TO this rank
    int put_source = (rank - 1 + size) % size;  // We receive PUT FROM this rank

    MPI_Win_fence(0, win);

    // Put to next rank, Get from next rank
    MPI_Put(send_buf, ARRAY_SIZE, MPI_INT, put_target, 0, ARRAY_SIZE, MPI_INT, win);
    MPI_Get(recv_buf, ARRAY_SIZE, MPI_INT, put_target, 0, ARRAY_SIZE, MPI_INT, win);

    MPI_Win_fence(0, win);

    // After fence:
    // - win_buf has data from put_source (the rank that Put to us)
    // - recv_buf has data from put_target's original win_buf

    // Verify win_buf: should have what put_source Put to us
    for (int i = 0; i < ARRAY_SIZE; i++) {
        int expected = put_source * 100 + i + 50000;
        if (win_buf[i] != expected) {
            errors++;
            if (errors <= 5) {
                LOG("  win_buf mismatch at [%d]: expected %d (from rank %d), got %d\n",
                    i, expected, put_source, win_buf[i]);
            }
        }
    }

    free(recv_buf);
    free(send_buf);
    MPI_Win_free(&win);

    MPI_Allreduce(MPI_IN_PLACE, &errors, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);

    if (rank == 0) {
        if (errors == 0) {
            TEST_PASS("bidirectional Put/Get");
        } else {
            TEST_FAIL("bidirectional Put/Get", "data mismatch");
        }
    }

    return errors;
}

/**
 * Test 6: Large data transfer
 */
int test_large_transfer(void) {
    size_t large_size = 1024 * 1024;  // 1M integers = 4MB
    int *local_buf;
    MPI_Win win;
    int errors = 0;

    LOG("=== Test: Large transfer (4MB) ===\n");

    local_buf = (int *)malloc(large_size * sizeof(int));
    if (!local_buf) {
        TEST_FAIL("large_transfer", "malloc failed");
        return 1;
    }

    // Initialize
    for (size_t i = 0; i < large_size; i++) {
        local_buf[i] = (rank == 0) ? (int)(i % 10000) : -1;
    }

    MPI_Win_create(local_buf, large_size * sizeof(int), sizeof(int),
                   MPI_INFO_NULL, MPI_COMM_WORLD, &win);

    MPI_Win_fence(0, win);

    if (rank == 1) {
        MPI_Get(local_buf, large_size, MPI_INT, 0, 0, large_size, MPI_INT, win);
    }

    MPI_Win_fence(0, win);

    if (rank == 1) {
        for (size_t i = 0; i < large_size; i++) {
            if (local_buf[i] != (int)(i % 10000)) {
                errors++;
                if (errors <= 5) {
                    LOG("  Mismatch at [%zu]: expected %d, got %d\n",
                        i, (int)(i % 10000), local_buf[i]);
                }
            }
        }
    }

    MPI_Win_free(&win);
    free(local_buf);

    MPI_Allreduce(MPI_IN_PLACE, &errors, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);

    if (rank == 0) {
        if (errors == 0) {
            TEST_PASS("large_transfer (4MB)");
        } else {
            TEST_FAIL("large_transfer (4MB)", "data mismatch");
        }
    }

    return errors;
}

/**
 * Test 7: Multiple iterations stress test
 */
int test_stress(void) {
    int *win_buf;
    MPI_Win win;
    int errors = 0;

    LOG("=== Test: Stress test (%d iterations) ===\n", TEST_ITERATIONS);

    MPI_Win_allocate(ARRAY_SIZE * sizeof(int), sizeof(int), MPI_INFO_NULL,
                     MPI_COMM_WORLD, &win_buf, &win);

    for (int iter = 0; iter < TEST_ITERATIONS; iter++) {
        // Initialize with iteration-specific values
        for (int i = 0; i < ARRAY_SIZE; i++) {
            win_buf[i] = rank * 10000 + iter * 100 + i;
        }

        MPI_Win_fence(0, win);

        int *recv_buf = (int *)malloc(ARRAY_SIZE * sizeof(int));
        int target = (rank + 1) % size;

        MPI_Get(recv_buf, ARRAY_SIZE, MPI_INT, target, 0, ARRAY_SIZE, MPI_INT, win);

        MPI_Win_fence(0, win);

        // Verify
        for (int i = 0; i < ARRAY_SIZE; i++) {
            int expected = target * 10000 + iter * 100 + i;
            if (recv_buf[i] != expected) {
                errors++;
            }
        }

        free(recv_buf);

        if (iter % 5 == 0 && rank == 0) {
            LOG("  Iteration %d/%d completed\n", iter + 1, TEST_ITERATIONS);
        }
    }

    MPI_Win_free(&win);

    MPI_Allreduce(MPI_IN_PLACE, &errors, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);

    if (rank == 0) {
        if (errors == 0) {
            TEST_PASS("stress test");
        } else {
            TEST_FAIL("stress test", "data mismatch");
        }
    }

    return errors;
}

/**
 * Test 8: Exclusive lock with Put
 */
int test_exclusive_lock(void) {
    int *local_buf;
    MPI_Win win;
    int errors = 0;

    LOG("=== Test: Exclusive lock Put ===\n");

    local_buf = (int *)malloc(ARRAY_SIZE * sizeof(int));
    if (!local_buf) {
        TEST_FAIL("exclusive_lock", "malloc failed");
        return 1;
    }

    // Initialize with zeros on rank 0
    for (int i = 0; i < ARRAY_SIZE; i++) {
        local_buf[i] = (rank == 0) ? 0 : rank * 1000 + i;
    }

    MPI_Win_create(local_buf, ARRAY_SIZE * sizeof(int), sizeof(int),
                   MPI_INFO_NULL, MPI_COMM_WORLD, &win);

    MPI_Barrier(MPI_COMM_WORLD);

    // Rank 1 puts to rank 0 with exclusive lock
    if (rank == 1) {
        int *put_buf = (int *)malloc(ARRAY_SIZE * sizeof(int));
        for (int i = 0; i < ARRAY_SIZE; i++) {
            put_buf[i] = 7777 + i;
        }

        MPI_Win_lock(MPI_LOCK_EXCLUSIVE, 0, 0, win);
        MPI_Put(put_buf, ARRAY_SIZE, MPI_INT, 0, 0, ARRAY_SIZE, MPI_INT, win);
        MPI_Win_unlock(0, win);

        free(put_buf);
    }

    MPI_Barrier(MPI_COMM_WORLD);

    // Verify on rank 0
    if (rank == 0) {
        for (int i = 0; i < ARRAY_SIZE; i++) {
            if (local_buf[i] != 7777 + i) {
                errors++;
                if (errors <= 5) {
                    LOG("  Mismatch at [%d]: expected %d, got %d\n",
                        i, 7777 + i, local_buf[i]);
                }
            }
        }
    }

    MPI_Win_free(&win);
    free(local_buf);

    MPI_Allreduce(MPI_IN_PLACE, &errors, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);

    if (rank == 0) {
        if (errors == 0) {
            TEST_PASS("exclusive_lock (Put)");
        } else {
            TEST_FAIL("exclusive_lock (Put)", "data mismatch");
        }
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
        printf(" MPI One-Sided Operations Test\n");
        printf(" Ranks: %d\n", size);
        printf("========================================\n\n");
    }

    MPI_Barrier(MPI_COMM_WORLD);

    // Run tests
    total_errors += test_win_create_fence();
    MPI_Barrier(MPI_COMM_WORLD);

    total_errors += test_win_allocate_fence();
    MPI_Barrier(MPI_COMM_WORLD);

    total_errors += test_lock_unlock();
    MPI_Barrier(MPI_COMM_WORLD);

    total_errors += test_accumulate();
    MPI_Barrier(MPI_COMM_WORLD);

    total_errors += test_bidirectional();
    MPI_Barrier(MPI_COMM_WORLD);

    total_errors += test_large_transfer();
    MPI_Barrier(MPI_COMM_WORLD);

    total_errors += test_stress();
    MPI_Barrier(MPI_COMM_WORLD);

    total_errors += test_exclusive_lock();
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
