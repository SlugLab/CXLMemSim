CC = mpicc
MPICC = mpicc
CFLAGS = -Wall -O3 -fPIC -shared -pthread -g
LDFLAGS = -ldl -lrt -pthread

SHIM_LIB = libmpi_cxl_shim.so
SHIM_SRC = mpi_cxl_shim.c

TEST_PROG = test_mpi_cxl
TEST_SRC = test_mpi_cxl.c

.PHONY: all clean test

all: $(SHIM_LIB) $(TEST_PROG)

$(SHIM_LIB): $(SHIM_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(TEST_PROG): $(TEST_SRC)
	$(MPICC) -o $@ $< -Wall -O2

test: $(SHIM_LIB) $(TEST_PROG)
	@echo "Running MPI CXL shim test..."
	@echo "Set environment variables:"
	@echo "  export LD_PRELOAD=./$(SHIM_LIB)"
	@echo "  export CXL_DAX_PATH=/dev/dax0.0  # or use shared memory"
	@echo "  export CXL_SHIM_VERBOSE=1"
	@echo "  export CXL_SHIM_ALLOC=1"
	@echo "  export CXL_SHIM_WIN=1"
	@echo "  export CXL_SHIM_COPY_SEND=1"
	@echo "  export CXL_SHIM_COPY_RECV=1"
	@echo "Then run: mpirun -np 2 ./$(TEST_PROG)"

clean:
	rm -f $(SHIM_LIB) $(TEST_PROG) *.o

install: $(SHIM_LIB)
	@echo "Installing $(SHIM_LIB) to /usr/local/lib/"
	@sudo cp $(SHIM_LIB) /usr/local/lib/
	@sudo ldconfig
	@echo "Installation complete. Use LD_PRELOAD=/usr/local/lib/$(SHIM_LIB) to enable."