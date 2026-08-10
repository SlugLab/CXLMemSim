CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
LDFLAGS ?=
LDLIBS ?=
NVCC ?= /usr/local/cuda/bin/nvcc
NVCC_HOST_COMPILER ?= /usr/bin/g++-13
XXD ?= xxd

CUDA_SHIM := libcuda.so.1
CUDA_LINK := libcuda.so

CUDA_TESTS := cuda_test cuda_advanced_test
BAR_TESTS := cxl_bar_benchmark
UNIT_TESTS := cxl_coherent_range_test
VECTORDB_TARGETS := vectordb_shared_index_guest vectordb_shared_index_native
VECTORDB_KERNEL_SOURCE := vectordb_shared_index_kernel.cu
VECTORDB_KERNEL_PTX := vectordb_shared_index_kernel.ptx
VECTORDB_KERNEL_HEADER := vectordb_shared_index_kernel_ptx.h
BAR_STATIC := cxl_bar_benchmark.static
TARGETS := $(CUDA_SHIM) $(CUDA_LINK) $(CUDA_TESTS) $(BAR_TESTS)

.PHONY: all clean static test test-binaries

all: $(TARGETS)

test-binaries: $(CUDA_TESTS) $(BAR_TESTS)

test: $(UNIT_TESTS)
	./cxl_coherent_range_test

$(CUDA_SHIM): libcuda.c cxl_gpu_cmd.h
	$(CC) $(CFLAGS) -shared -fPIC -o $@ libcuda.c -ldl $(LDFLAGS) $(LDLIBS)

$(CUDA_LINK): $(CUDA_SHIM)
	ln -sf $(CUDA_SHIM) $@

cuda_test: cuda_test.c $(CUDA_LINK)
	$(CC) $(CFLAGS) -o $@ cuda_test.c -L. -lcuda -ldl -lrt -Wl,-rpath,. $(LDFLAGS) $(LDLIBS)

cuda_advanced_test: cuda_advanced_test.c $(CUDA_LINK)
	$(CC) $(CFLAGS) -o $@ cuda_advanced_test.c -L. -lcuda -ldl -lrt -Wl,-rpath,. $(LDFLAGS) $(LDLIBS)

cxl_bar_benchmark: cxl_bar_benchmark.c cxl_gpu_cmd.h
	$(CC) $(CFLAGS) -o $@ cxl_bar_benchmark.c -lrt -lpthread $(LDFLAGS) $(LDLIBS)

cxl_coherent_range_test: cxl_coherent_range_test.c libcuda.c cxl_gpu_cmd.h
	$(CC) $(CFLAGS) -O0 -Wno-format-truncation -Wno-unused-result -o $@ cxl_coherent_range_test.c -ldl $(LDFLAGS) $(LDLIBS)

$(VECTORDB_KERNEL_PTX): $(VECTORDB_KERNEL_SOURCE)
	$(NVCC) -ccbin $(NVCC_HOST_COMPILER) -Xcompiler=-U_GNU_SOURCE -O3 --ptx \
		--gpu-architecture=compute_80 --fmad=false -o $@ $<

$(VECTORDB_KERNEL_HEADER): $(VECTORDB_KERNEL_PTX)
	$(XXD) -i -n kVectordbSharedIndexPtx $< $@

vectordb_shared_index_guest: vectordb_shared_index.c $(VECTORDB_KERNEL_HEADER) $(CUDA_LINK)
	$(CC) $(CFLAGS) -ffp-contract=off -o $@ vectordb_shared_index.c -L. -lcuda -ldl -lm \
		-Wl,-rpath,'$$ORIGIN' $(LDFLAGS) $(LDLIBS)

vectordb_shared_index_native: vectordb_shared_index.c $(VECTORDB_KERNEL_HEADER)
	$(CC) $(CFLAGS) -ffp-contract=off -DVECTORDB_NATIVE_BUILD=1 -o $@ vectordb_shared_index.c \
		-L/usr/lib/x86_64-linux-gnu -Wl,-rpath,/usr/lib/x86_64-linux-gnu -lcuda -ldl -lm $(LDFLAGS) $(LDLIBS)

static: $(BAR_STATIC)

$(BAR_STATIC): cxl_bar_benchmark.c cxl_gpu_cmd.h
	$(CC) $(CFLAGS) -static -o $@ cxl_bar_benchmark.c -lrt -lpthread $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(TARGETS) $(BAR_STATIC) $(UNIT_TESTS) $(VECTORDB_TARGETS) $(VECTORDB_KERNEL_PTX) $(VECTORDB_KERNEL_HEADER)
