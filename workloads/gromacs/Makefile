CC = mpicc
MPICC = mpicc
# -fno-tree-loop-distribute-patterns: prevents GCC from converting our
# volatile byte loops (cxl_safe_memset/memcpy) into libc memset/memcpy
# calls that use AVX-512/SIMD and SIGILL on CXL device memory.
CFLAGS = -Wall -O3 -fPIC -shared -pthread -g -fno-tree-loop-distribute-patterns
LDFLAGS = -ldl -lrt -pthread

SHIM_SRC = mpi_cxl_shim.c

# Cache coherence: 0 = no flush/invalidate, 1 = use explicit flush/invalidate
CXL_CC ?= 0

# Flush options (only when CXL_CC=1): clwb | clflush | clflushopt
# Invalidate options (only when CXL_CC=1): clflush | clflushopt
FLUSH ?= clwb
INV ?= clflush

# Validate FLUSH and INV when CXL_CC=1
ifeq ($(CXL_CC),1)
ifeq ($(filter $(FLUSH),clwb clflush clflushopt),)
$(error FLUSH must be clwb, clflush, or clflushopt)
endif
ifeq ($(filter $(INV),clflush clflushopt),)
$(error INV must be clflush or clflushopt)
endif
endif

# Library name: libmpi_cxl_shim_nocc.so (CC off) or libmpi_cxl_shim_cc_${FLUSH}_${INV}.so (CC on)
ifeq ($(CXL_CC),1)
SHIM_LIB = libmpi_cxl_shim_cc_$(FLUSH)_$(INV).so
else
SHIM_LIB = libmpi_cxl_shim_nocc.so
endif

# Default shim (backward compatible: libmpi_cxl_shim.so always -> nocc)
SHIM_DEFAULT = libmpi_cxl_shim.so

TEST_PROG = test_mpi_cxl
TEST_SRC = test_mpi_cxl.c

.PHONY: all clean test shim shim-all help

all: shim-all $(SHIM_DEFAULT) $(TEST_PROG)

# Build shim with selected FLUSH and INV
shim: $(SHIM_LIB)

# Build all shim variants: 1 nocc + 6 cc variants
SHIM_VARIANTS = \
	libmpi_cxl_shim_nocc.so \
	libmpi_cxl_shim_cc_clwb_clflush.so \
	libmpi_cxl_shim_cc_clwb_clflushopt.so \
	libmpi_cxl_shim_cc_clflush_clflush.so \
	libmpi_cxl_shim_cc_clflush_clflushopt.so \
	libmpi_cxl_shim_cc_clflushopt_clflush.so \
	libmpi_cxl_shim_cc_clflushopt_clflushopt.so

shim-all:
	@$(MAKE) CXL_CC=0 shim
	@for f in clwb clflush clflushopt; do \
	  for i in clflush clflushopt; do \
	    $(MAKE) CXL_CC=1 FLUSH=$$f INV=$$i shim; \
	  done; \
	done
	@echo "Built all shim variants: $(SHIM_VARIANTS)"

# Compiler defines: CXL_CACHE_COHERENCE when CC=1, plus flush/invalidate instruction selection
ifeq ($(CXL_CC),1)
SHIM_CFLAGS = $(CFLAGS) -DCXL_CACHE_COHERENCE -DCXL_FLUSH_$(shell echo $(FLUSH) | tr a-z A-Z) -DCXL_INV_$(shell echo $(INV) | tr a-z A-Z)
else
SHIM_CFLAGS = $(CFLAGS)
endif

$(SHIM_LIB): $(SHIM_SRC)
	$(CC) $(SHIM_CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "Built $@ (cache_coherence=$(CXL_CC), flush=$(FLUSH), invalidate=$(INV))"

# Default symlink: libmpi_cxl_shim.so always points to nocc variant
$(SHIM_DEFAULT):
	@$(MAKE) CXL_CC=0 shim
	@ln -sf libmpi_cxl_shim_nocc.so $(SHIM_DEFAULT) 2>/dev/null || cp libmpi_cxl_shim_nocc.so $(SHIM_DEFAULT)
	@echo "Built $(SHIM_DEFAULT) -> libmpi_cxl_shim_nocc.so"


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
	rm -f $(SHIM_VARIANTS) $(SHIM_DEFAULT) $(TEST_PROG) *.o
	rm -f libmpi_cxl_shim_*.so

install: $(SHIM_LIB)
	@echo "Installing $(SHIM_LIB) to /usr/local/lib/"
	@sudo cp $(SHIM_LIB) /usr/local/lib/
	@sudo ldconfig
	@echo "Installation complete. Use LD_PRELOAD=/usr/local/lib/$(SHIM_LIB) to enable."

help:
	@echo "MPI CXL Shim Build Options"
	@echo ""
	@echo "Targets:"
	@echo "  all        - Build all 7 shim variants, default symlink, and test"
	@echo "  shim       - Build shim with current options"
	@echo "  shim-all   - Build all 7 variants (1 nocc + 6 cc)"
	@echo "  clean      - Remove built libraries"
	@echo ""
	@echo "Variables:"
	@echo "  CXL_CC=0|1     - Cache coherence: 0=no flush/invalidate, 1=use clwb/clflush (default: 0)"
	@echo "  FLUSH=...      - When CXL_CC=1: clwb|clflush|clflushopt (default: clwb)"
	@echo "  INV=...        - When CXL_CC=1: clflush|clflushopt (default: clflush)"
	@echo ""
	@echo "Examples:"
	@echo "  make                    # All 7 variants + default symlink + test"
	@echo "  make shim CXL_CC=0      # Only libmpi_cxl_shim_nocc.so"
	@echo "  make shim CXL_CC=1 FLUSH=clflush INV=clflush  # Specific cc variant"
	@echo "  make shim-all           # All 7 variants"