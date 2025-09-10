#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_SIZE (1024 * 1024)  // 1MB test buffer

int main(int argc, char *argv[]) {
    int rank, size;
    int provided;
    
    // Initialize MPI with thread support
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    
    printf("[Rank %d] MPI initialized with %d processes\n", rank, size);
    
    // Test 1: MPI_Alloc_mem
    printf("[Rank %d] Testing MPI_Alloc_mem...\n", rank);
    void *alloc_buf;
    int ret = MPI_Alloc_mem(TEST_SIZE, MPI_INFO_NULL, &alloc_buf);
    if (ret == MPI_SUCCESS) {
        printf("[Rank %d] MPI_Alloc_mem succeeded, buffer at %p\n", rank, alloc_buf);
        
        // Fill buffer with rank-specific data
        memset(alloc_buf, rank + 1, TEST_SIZE);
        
        // Verify write
        unsigned char *p = (unsigned char *)alloc_buf;
        int errors = 0;
        for (int i = 0; i < 100; i++) {
            if (p[i] != (rank + 1)) errors++;
        }
        printf("[Rank %d] Memory verification: %s\n", rank, 
               errors == 0 ? "PASSED" : "FAILED");
        
        MPI_Free_mem(alloc_buf);
    } else {
        printf("[Rank %d] MPI_Alloc_mem failed\n", rank);
    }
    
    // Test 2: Point-to-point communication
    if (size >= 2) {
        printf("[Rank %d] Testing point-to-point communication...\n", rank);
        
        int *send_buf = malloc(1024 * sizeof(int));
        int *recv_buf = malloc(1024 * sizeof(int));
        
        for (int i = 0; i < 1024; i++) {
            send_buf[i] = rank * 1000 + i;
        }
        
        if (rank == 0) {
            MPI_Send(send_buf, 1024, MPI_INT, 1, 99, MPI_COMM_WORLD);
            MPI_Recv(recv_buf, 1024, MPI_INT, 1, 88, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            printf("[Rank 0] Received first element: %d (expected 1000)\n", recv_buf[0]);
        } else if (rank == 1) {
            MPI_Recv(recv_buf, 1024, MPI_INT, 0, 99, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            printf("[Rank 1] Received first element: %d (expected 0)\n", recv_buf[0]);
            MPI_Send(send_buf, 1024, MPI_INT, 0, 88, MPI_COMM_WORLD);
        }
        
        free(send_buf);
        free(recv_buf);
    }
    
    // Test 3: RMA Window with shared memory
    if (size >= 2) {
        printf("[Rank %d] Testing MPI RMA window...\n", rank);
        
        MPI_Win win;
        int *win_buf;
        MPI_Aint win_size = (rank == 0) ? 1024 * sizeof(int) : 0;
        
        ret = MPI_Win_allocate(win_size, sizeof(int), MPI_INFO_NULL, 
                              MPI_COMM_WORLD, &win_buf, &win);
        
        if (ret == MPI_SUCCESS) {
            printf("[Rank %d] Window allocated, buffer at %p\n", rank, win_buf);
            
            if (rank == 0) {
                // Initialize window buffer
                for (int i = 0; i < 1024; i++) {
                    win_buf[i] = i;
                }
            }
            
            MPI_Win_fence(0, win);
            
            if (rank == 1) {
                int local_buf[10];
                MPI_Get(local_buf, 10, MPI_INT, 0, 0, 10, MPI_INT, win);
                MPI_Win_fence(0, win);
                
                printf("[Rank 1] Got from window: ");
                for (int i = 0; i < 10; i++) {
                    printf("%d ", local_buf[i]);
                }
                printf("\n");
            } else {
                MPI_Win_fence(0, win);
            }
            
            MPI_Win_free(&win);
        } else {
            printf("[Rank %d] MPI_Win_allocate failed\n", rank);
        }
    }
    
    // Test 4: Shared memory window
    if (size >= 2) {
        printf("[Rank %d] Testing MPI shared memory window...\n", rank);
        
        MPI_Comm node_comm;
        MPI_Comm_split_type(MPI_COMM_WORLD, MPI_COMM_TYPE_SHARED, rank, 
                           MPI_INFO_NULL, &node_comm);
        
        int node_rank, node_size;
        MPI_Comm_rank(node_comm, &node_rank);
        MPI_Comm_size(node_comm, &node_size);
        
        if (node_size > 1) {
            MPI_Win shm_win;
            int *shm_buf;
            MPI_Aint shm_size = 256 * sizeof(int);
            
            ret = MPI_Win_allocate_shared(shm_size, sizeof(int), MPI_INFO_NULL,
                                         node_comm, &shm_buf, &shm_win);
            
            if (ret == MPI_SUCCESS) {
                printf("[Rank %d] Shared window allocated at %p\n", rank, shm_buf);
                
                // Each rank writes to its portion
                MPI_Win_lock_all(0, shm_win);
                for (int i = 0; i < 10; i++) {
                    shm_buf[node_rank * 10 + i] = node_rank * 100 + i;
                }
                MPI_Win_unlock_all(shm_win);
                
                MPI_Barrier(node_comm);
                
                // Read from neighbor's portion
                if (node_rank == 0 && node_size > 1) {
                    printf("[Rank %d] Reading neighbor's data: ", rank);
                    for (int i = 0; i < 10; i++) {
                        printf("%d ", shm_buf[10 + i]);
                    }
                    printf("\n");
                }
                
                MPI_Win_free(&shm_win);
            } else {
                printf("[Rank %d] MPI_Win_allocate_shared failed\n", rank);
            }
        }
        
        MPI_Comm_free(&node_comm);
    }
    
    // Test 5: Non-blocking communication
    if (size >= 2) {
        printf("[Rank %d] Testing non-blocking communication...\n", rank);
        
        double *nbuf = malloc(1000 * sizeof(double));
        for (int i = 0; i < 1000; i++) {
            nbuf[i] = rank + i * 0.1;
        }
        
        MPI_Request req;
        if (rank == 0) {
            MPI_Isend(nbuf, 1000, MPI_DOUBLE, 1, 123, MPI_COMM_WORLD, &req);
            MPI_Wait(&req, MPI_STATUS_IGNORE);
            printf("[Rank 0] Non-blocking send completed\n");
        } else if (rank == 1) {
            MPI_Irecv(nbuf, 1000, MPI_DOUBLE, 0, 123, MPI_COMM_WORLD, &req);
            MPI_Wait(&req, MPI_STATUS_IGNORE);
            printf("[Rank 1] Non-blocking recv completed, first element: %f\n", nbuf[0]);
        }
        
        free(nbuf);
    }
    
    MPI_Barrier(MPI_COMM_WORLD);
    printf("[Rank %d] All tests completed\n", rank);
    
    MPI_Finalize();
    return 0;
}