#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
    int rank, size;

    printf("[TEST] Before MPI_Init\n");
    fflush(stdout);

    MPI_Init(&argc, &argv);

    printf("[TEST] After MPI_Init\n");
    fflush(stdout);

    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    printf("[TEST] Rank %d of %d\n", rank, size);
    fflush(stdout);

    // Test MPI_Send and MPI_Recv
    if (size >= 2) {
        int data = 42 + rank;
        if (rank == 0) {
            printf("[TEST] Rank 0: Sending data %d to rank 1\n", data);
            fflush(stdout);
            MPI_Send(&data, 1, MPI_INT, 1, 99, MPI_COMM_WORLD);
            printf("[TEST] Rank 0: Send completed\n");
            fflush(stdout);
        } else if (rank == 1) {
            int recv_data;
            printf("[TEST] Rank 1: Receiving data from rank 0\n");
            fflush(stdout);
            MPI_Recv(&recv_data, 1, MPI_INT, 0, 99, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            printf("[TEST] Rank 1: Received data %d\n", recv_data);
            fflush(stdout);
        }
    }

    // Test MPI_Alloc_mem
    void *mem_ptr = NULL;
    int ret = MPI_Alloc_mem(1024, MPI_INFO_NULL, &mem_ptr);
    printf("[TEST] Rank %d: MPI_Alloc_mem returned %d, ptr=%p\n", rank, ret, mem_ptr);
    fflush(stdout);

    if (ret == MPI_SUCCESS && mem_ptr) {
        MPI_Free_mem(mem_ptr);
        printf("[TEST] Rank %d: MPI_Free_mem completed\n", rank);
        fflush(stdout);
    }

    printf("[TEST] Rank %d: Before MPI_Finalize\n", rank);
    fflush(stdout);

    MPI_Finalize();

    printf("[TEST] Rank %d: After MPI_Finalize\n", rank);
    fflush(stdout);

    return 0;
}