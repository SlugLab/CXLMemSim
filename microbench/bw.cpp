
#ifdef SSE4_VERSION

#include <math.h>
#include <assert.h>
#include <stdint.h>
#include <pthread.h>
#include <string.h>
#include <numa.h>
#include "monotonic_timer.h"
#include "interpose.h"



#ifdef __SSE4_1__
#include <smmintrin.h>
#endif

#define BYTES_PER_GB (1024*1024*1024LL)
#define BYTES_PER_MB (1024*1024LL)

// flag for terminating current test
int g_done;

// global current number of threads
int g_nthreads = 0;

// synchronization barrier for current thread counter
pthread_barrier_t g_barrier;

// thread shared parameters for test function
void* g_array;
size_t g_thrsize;
int g_times;
void (*g_func)(void*, size_t);

// Compute bandwidth in MB/s.
static inline double to_bw(size_t bytes, double secs) {
  double size_bytes = (double) bytes;
  double size_mb = size_bytes / ((double) BYTES_PER_MB);
  return size_mb / secs;
}

void* thread_worker(void* arg)
{
    int j;
    unsigned int thread_num = (uintptr_t) arg;

    while (1)
    {
        // *** Barrier ****
        pthread_barrier_wait(&g_barrier);

        if (g_done) break;

        for (j = 0; j < g_times; j++) {
            g_func(&((char*) g_array)[g_thrsize * thread_num], g_thrsize);
        }

        // *** Barrier ****
        pthread_barrier_wait(&g_barrier);
    }

    return NULL;
}


int timeitp(void (*function)(void*, size_t), int nthreads, void* array, size_t size, int samples, int times) {
    double min = INFINITY;
    double runtime;
    size_t i, j, p;
    int thread_num;

    // globally set test function and thread number
    g_func = function;
    g_nthreads = nthreads;
    g_array = array;
    g_thrsize = size / nthreads;
    g_times = times;

    // create barrier and run threads
    pthread_barrier_init(&g_barrier, NULL, nthreads);

    pthread_t thr[nthreads];
    //__lib_pthread_create(&thr[0], NULL, thread_master, new int(0));
    for (p = 1; p < nthreads; ++p) {
    	assert(__lib_pthread_create);
        __lib_pthread_create(&thr[p], NULL, thread_worker, (void *) p);
    }

    // use current thread as master thread;
    g_done = 0;
    thread_num = 0;
    for (i = 0; i < samples; i++) 
    {
        pthread_barrier_wait(&g_barrier);

        assert(!g_done);

        double ts1 = monotonic_time();

        for (j = 0; j < times; j++) {
            g_func(&((char*)g_array)[g_thrsize * thread_num], g_thrsize);
        }

        pthread_barrier_wait(&g_barrier);
        double ts2 = monotonic_time();

        runtime = ts2 - ts1;
        if (runtime < min) {
            min = runtime;
        }
    }
    g_done = 1;

    pthread_barrier_wait(&g_barrier);

    for (p = 1; p < nthreads; ++p) {
        pthread_join(thr[p], NULL);
    }

    pthread_barrier_destroy(&g_barrier);

    return to_bw(size * times, min);
}


int timeit(void (*function)(void*, size_t), void* array, size_t size, int samples, int times) {
    double min = INFINITY;
    size_t i;

    // force allocation of physical pages
    memset(array, 0xff, size);

    for (i = 0; i < samples; i++) {
        double before, after, total;

        before = monotonic_time();
        int j;
        for (j = 0; j < times; j++) {
            function(array, size);
        }
        after = monotonic_time();

        total = after - before;
        if (total < min) {
            min = total;
        }
    }

    return to_bw(size * times, min);
}


#ifdef __SSE4_1__
void write_memory_nontemporal_sse(void* array, size_t size) {
  __m128i* varray = (__m128i*) array;

  __m128i vals = _mm_set1_epi32(1);
  size_t i;
  for (i = 0; i < size / sizeof(__m128i); i++) {
    _mm_stream_si128(&varray[i], vals);
    vals = _mm_add_epi16(vals, vals);
  }
}

void write_memory_sse(void* array, size_t size) {
  __m128i* varray = (__m128i*) array;

  __m128i vals = _mm_set1_epi32(1);
  size_t i;
  for (i = 0; i < size / sizeof(__m128i); i++) {
    _mm_store_si128(&varray[i], vals);
    vals = _mm_add_epi16(vals, vals);
  }
}

void read_memory_sse(void* array, size_t size) {
  __m128i* varray = (__m128i*) array;
  __m128i accum = _mm_set1_epi32(0xDEADBEEF);
  size_t i;
  for (i = 0; i < size / sizeof(__m128i); i++) {
    accum = _mm_add_epi16(varray[i], accum);
  }

  // This is unlikely, and we want to make sure the reads are not optimized
  // away.
  assert(!_mm_testz_si128(accum, accum));
}
#else
# error "No compiler support for SSE instructions"
#endif

//static char array[1024*1024*1024];

double measure_read_bw(int cpu_node, int mem_node)
{
    char* array;
    size_t size = 1024*1024*1024;
    double bw;
    int nthreads = 16;

    array = numa_alloc_onnode(size, mem_node);
    assert(array);
    numa_run_on_node(cpu_node);
    // force allocation of physical pages
    memset(array, 0xff, size);
    bw = timeitp(read_memory_sse, nthreads, array, size, 5, 1);
    numa_free(array, size);
    return bw;
}

double measure_write_bw(int cpu_node, int mem_node)
{
    char* array;
    size_t size = 1024*1024*1024;
    double bw;
    int nthreads = 16;

    array = numa_alloc_onnode(size, mem_node);
    assert(array);
    numa_run_on_node(cpu_node);
    // force allocation of physical pages
    memset(array, 0xff, size);
    bw = timeitp(write_memory_nontemporal_sse, nthreads, array, size, 5, 1);
    numa_free(array, size);
    return bw;
}

#else // SSE4_VERSION


#include <stdio.h>
#include <math.h>
#include <float.h>
#include <limits.h>
#include <sys/time.h>
#include <numa.h>
#include <numaif.h>
#include <omp.h>
#include "monotonic_timer.h"
#include "debug.h"


# define N	20000000
# define NTIMES	10
# define OFFSET	0

# define HLINE "-------------------------------------------------------------\n"

# ifndef MIN
# define MIN(x,y) ((x)<(y)?(x):(y))
# endif
# ifndef MAX
# define MAX(x,y) ((x)>(y)?(x):(y))
# endif


static double	mintime[4] = {FLT_MAX,FLT_MAX,FLT_MAX,FLT_MAX};

static double	bytes[4] = {
    2 * sizeof(double) * N,
    2 * sizeof(double) * N,
    3 * sizeof(double) * N,
    3 * sizeof(double) * N
    };

//extern double mysecond();

double measure_read_bw(int cpu_node, int mem_node)
    {
    register int	j, k;
    double		t, times[4][NTIMES];
    double *a, *c;
    //struct bitmask* membind;

    /* --- SETUP --- determine precision and check timing --- */

    //membind = numa_allocate_nodemask();
    //numa_bitmask_setbit(membind, mem_node);
    //numa_bind(membind);
    //numa_free_nodemask(membind);
    numa_run_on_node(cpu_node);

    omp_set_num_threads(10);

    // allocate memory dynamically to make sure the data is stored on the expected NUMA node
    a = (double *)numa_alloc_onnode( (N+OFFSET) * sizeof(double), mem_node);
    c = (double *)numa_alloc_onnode( (N+OFFSET) * sizeof(double), mem_node);

    DBG_LOG(DEBUG, "Measuring read BW on cpu node %d and mem node %d\n", cpu_node, mem_node);

    /* Get initial value for system clock. */
#pragma omp parallel for
    for (j=0; j<N; j++) {
	a[j] = (double)random(); //1.0;
	c[j] = 0.0;
	}

    t = monotonic_time(); //mysecond();
#pragma omp parallel for
    for (j = 0; j < N; j++)
	a[j] = 2.0E0 * a[j];
    t = 1.0E6 * (monotonic_time() - t);

    /*	--- MAIN LOOP --- repeat test cases NTIMES times --- */

    for (k=0; k<NTIMES; k++)
	{
	times[0][k] = monotonic_time(); //mysecond();
#pragma omp parallel for
	for (j=0; j<N; j++)
	    c[j] = a[j];
	times[0][k] = monotonic_time() - times[0][k];
	}

    /*	--- SUMMARY --- */
    
    mintime[0] = FLT_MAX;
    for (k=1; k<NTIMES; k++) 
	{
	    mintime[0] = MIN(mintime[0], times[0][k]);
	}

    numa_free(a, (N+OFFSET) * sizeof(double));
    numa_free(c, (N+OFFSET) * sizeof(double));

    // reset NUMA binding
    //numa_run_on_node_mask(numa_all_nodes_ptr);
    //numa_set_membind(numa_all_nodes_ptr);
    //numa_bind(numa_all_nodes_ptr);
    numa_run_on_node(-1);

    return 1.0E-06 * bytes[0]/mintime[0]; // bytes to MiB/s 
}
