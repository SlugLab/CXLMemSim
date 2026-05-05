#include <faiss/IndexFlat.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

extern "C" {
#include <pgas/cxl_backend.h>
}

namespace {

using Clock = std::chrono::steady_clock;

int envInt(const char *name, int fallback) {
    const char *v = std::getenv(name);
    return v ? std::atoi(v) : fallback;
}

struct Args {
    std::string storage = "native";
    std::string pool = "/faiss_cxl_pool";
    std::string dax_path = "/dev/dax0.0";
    std::string dax_load = "byte";
    int node = envInt("PGAS_LOCAL_NODE", 0);
    size_t nb = 50000;
    size_t nq = 128;
    size_t dim = 64;
    size_t k = 10;
    size_t iters = 2;
    size_t block = 4096;
    size_t pool_mb = 512;
    int threads = 1;
    bool skip_dax_write = false;
    bool parallel_dax_write = true;
};

double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

size_t parseSize(const char *text, const char *name) {
    char *end = nullptr;
    unsigned long long value = std::strtoull(text, &end, 0);
    if (!end || *end != '\0') {
        throw std::runtime_error(std::string("invalid numeric value for ") + name + ": " + text);
    }
    return static_cast<size_t>(value);
}

Args parseArgs(int argc, char **argv) {
    Args args;
    for (int i = 1; i < argc; i++) {
        std::string key = argv[i];
        auto need = [&](const char *name) -> const char * {
            if (i + 1 >= argc)
                throw std::runtime_error(std::string("missing value for ") + name);
            return argv[++i];
        };
        if (key == "--storage")
            args.storage = need("--storage");
        else if (key == "--pool")
            args.pool = need("--pool");
        else if (key == "--dax-path")
            args.dax_path = need("--dax-path");
        else if (key == "--dax-load")
            args.dax_load = need("--dax-load");
        else if (key == "--node")
            args.node = static_cast<int>(parseSize(need("--node"), "--node"));
        else if (key == "--nb")
            args.nb = parseSize(need("--nb"), "--nb");
        else if (key == "--nq")
            args.nq = parseSize(need("--nq"), "--nq");
        else if (key == "--dim")
            args.dim = parseSize(need("--dim"), "--dim");
        else if (key == "--k")
            args.k = parseSize(need("--k"), "--k");
        else if (key == "--iters")
            args.iters = parseSize(need("--iters"), "--iters");
        else if (key == "--block")
            args.block = parseSize(need("--block"), "--block");
        else if (key == "--pool-mb")
            args.pool_mb = parseSize(need("--pool-mb"), "--pool-mb");
        else if (key == "--threads")
            args.threads = static_cast<int>(parseSize(need("--threads"), "--threads"));
        else if (key == "--skip-dax-write")
            args.skip_dax_write = true;
        else if (key == "--serial-dax-write")
            args.parallel_dax_write = false;
        else if (key == "--help") {
            std::cout << "Usage: faiss_cpu_splash [--storage native|cxl-pool|dax|dax-zero-copy|dax-zc]"
                         " [--pool /name] [--dax-load u32|byte|float]"
                         " [--skip-dax-write] [--serial-dax-write]\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + key);
        }
    }
    if (args.storage == "dax-zc")
        args.storage = "dax-zero-copy";
    if (args.storage != "native" && args.storage != "cxl-pool" && args.storage != "dax" &&
        args.storage != "dax-zero-copy") {
        throw std::runtime_error("--storage must be native, cxl-pool, dax, or dax-zero-copy");
    }
    if (args.dax_load != "u32" && args.dax_load != "float" && args.dax_load != "byte") {
        throw std::runtime_error("--dax-load must be u32, byte, or float");
    }
    if (args.k == 0 || args.dim == 0 || args.nb == 0 || args.nq == 0 || args.iters == 0) {
        throw std::runtime_error("nb, nq, dim, k, and iters must be non-zero");
    }
    args.k = std::min(args.k, args.nb);
    args.block = std::max<size_t>(1, std::min(args.block, args.nb));
    return args;
}

void fillVectors(float *data, size_t rows, size_t dim, uint32_t seed) {
    for (size_t i = 0; i < rows; i++) {
        for (size_t j = 0; j < dim; j++) {
            uint32_t x = seed + static_cast<uint32_t>(i * 1315423911u + j * 2654435761u);
            x ^= x >> 16;
            x *= 0x7feb352du;
            x ^= x >> 15;
            float v = static_cast<float>(x & 0xffff) / 65535.0f;
            data[i * dim + j] = std::sin(v * 6.2831853f) + 0.01f * static_cast<float>(j % 7);
        }
    }
}

struct CxlPool {
    cxl_backend_t *backend = nullptr;

    CxlPool(const std::string &name, size_t pool_mb) {
        cxl_backend_config_t cfg;
        std::memset(&cfg, 0, sizeof(cfg));
        cfg.type = CXL_BACKEND_SHMEM;
        std::strncpy(cfg.shmem.shm_name, name.c_str(), sizeof(cfg.shmem.shm_name) - 1);
        cfg.shmem.shm_size = pool_mb * 1024ULL * 1024ULL;
        cfg.shmem.is_server = false;

        backend = cxl_backend_create(CXL_BACKEND_SHMEM, &cfg);
        if (!backend)
            throw std::runtime_error("cxl_backend_create failed");
        if (backend->ops->connect(backend) != 0) {
            cxl_backend_destroy(backend);
            backend = nullptr;
            throw std::runtime_error("failed to connect to CXL SHMEM pool " + name);
        }
    }

    ~CxlPool() {
        if (backend)
            cxl_backend_destroy(backend);
    }

    void write(uint64_t addr, const void *src, size_t bytes) {
        const auto *p = static_cast<const uint8_t *>(src);
        for (size_t off = 0; off < bytes; off += 64) {
            size_t chunk = std::min<size_t>(64, bytes - off);
            if (backend->ops->write(backend, addr + off, p + off, chunk, nullptr) != 0) {
                throw std::runtime_error("CXL pool write failed");
            }
        }
    }

    void read(uint64_t addr, void *dst, size_t bytes) {
        auto *p = static_cast<uint8_t *>(dst);
        for (size_t off = 0; off < bytes; off += 64) {
            size_t chunk = std::min<size_t>(64, bytes - off);
            if (backend->ops->read(backend, addr + off, p + off, chunk, nullptr) != 0) {
                throw std::runtime_error("CXL pool read failed");
            }
        }
    }
};

struct DaxMapping {
    int fd = -1;
    void *addr = MAP_FAILED;
    size_t size = 0;

    DaxMapping(const std::string &path, size_t bytes) {
        fd = open(path.c_str(), O_RDWR);
        if (fd < 0)
            throw std::runtime_error("failed to open " + path);

        size = (bytes + 2097151ULL) & ~2097151ULL;
        addr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (addr == MAP_FAILED) {
            close(fd);
            fd = -1;
            throw std::runtime_error("failed to mmap " + path);
        }
    }

    ~DaxMapping() {
        if (addr != MAP_FAILED)
            munmap(addr, size);
        if (fd >= 0)
            close(fd);
    }

    float *data() { return static_cast<float *>(addr); }
};

void copyToDax(volatile uint8_t *dst, const uint8_t *src, size_t count) {
    for (size_t i = 0; i < count; i++) {
        dst[i] = src[i];
    }
}

void copyToDaxParallel(volatile uint8_t *dst, const uint8_t *src, size_t count, int num_threads) {
#ifdef _OPENMP
#pragma omp parallel for schedule(static) num_threads(num_threads)
#endif
    for (size_t i = 0; i < count; i++) {
        dst[i] = src[i];
    }
}

void copyFromDax(uint8_t *dst, volatile const uint8_t *src, size_t count) {
    for (size_t i = 0; i < count; i++) {
        dst[i] = src[i];
    }
}

float loadFloatFromDax(volatile const uint8_t *src) {
    uint32_t bits = static_cast<uint32_t>(src[0]) | (static_cast<uint32_t>(src[1]) << 8) |
                    (static_cast<uint32_t>(src[2]) << 16) | (static_cast<uint32_t>(src[3]) << 24);

    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

float loadFloatU32FromDax(volatile const uint8_t *src) {
    uint32_t bits = *reinterpret_cast<volatile const uint32_t *>(src);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

float loadFloatDirectFromDax(volatile const uint8_t *src) { return *reinterpret_cast<volatile const float *>(src); }

int activeThreadCount(const Args &args) {
#ifdef _OPENMP
    return args.threads > 0 ? args.threads : omp_get_max_threads();
#else
    (void)args;
    return 1;
#endif
}

void insertBest(float *best_dist, faiss::idx_t *best_label, size_t k, float dist, faiss::idx_t label) {
    if (dist >= best_dist[k - 1])
        return;

    size_t slot = k - 1;
    while (slot > 0 && dist < best_dist[slot - 1]) {
        best_dist[slot] = best_dist[slot - 1];
        best_label[slot] = best_label[slot - 1];
        slot--;
    }
    best_dist[slot] = dist;
    best_label[slot] = label;
}

double runNative(const Args &args, const std::vector<float> &xb, const std::vector<float> &xq, uint64_t &checksum) {
    auto start = Clock::now();
    faiss::IndexFlatL2 index(static_cast<faiss::idx_t>(args.dim));
    index.add(static_cast<faiss::idx_t>(args.nb), xb.data());

    std::vector<float> distances(args.nq * args.k);
    std::vector<faiss::idx_t> labels(args.nq * args.k);
    for (size_t iter = 0; iter < args.iters; iter++) {
        index.search(static_cast<faiss::idx_t>(args.nq), xq.data(), static_cast<faiss::idx_t>(args.k), distances.data(),
                     labels.data());
        for (faiss::idx_t label : labels)
            checksum += static_cast<uint64_t>(label + 1);
    }
    return msSince(start);
}

double runCxlPool(const Args &args, const std::vector<float> &xb, const std::vector<float> &xq, double &pool_write_ms,
                  double &pool_read_ms, uint64_t &checksum) {
    CxlPool pool(args.pool, args.pool_mb);
    const size_t vector_bytes = args.dim * sizeof(float);
    const size_t total_bytes = args.nb * vector_bytes;

    auto write_start = Clock::now();
    pool.write(0, xb.data(), total_bytes);
    pool_write_ms = msSince(write_start);

    std::vector<float> batch(args.block * args.dim);
    std::vector<float> distances(args.nq * args.k);
    std::vector<faiss::idx_t> labels(args.nq * args.k);
    std::vector<float> best_distances(args.nq * args.k);
    std::vector<faiss::idx_t> best_labels(args.nq * args.k);

    auto total_start = Clock::now();
    pool_read_ms = 0.0;
    for (size_t iter = 0; iter < args.iters; iter++) {
        std::fill(best_distances.begin(), best_distances.end(), std::numeric_limits<float>::infinity());
        std::fill(best_labels.begin(), best_labels.end(), faiss::idx_t{-1});

        for (size_t base = 0; base < args.nb; base += args.block) {
            size_t rows = std::min(args.block, args.nb - base);
            size_t bytes = rows * vector_bytes;
            auto read_start = Clock::now();
            pool.read(base * vector_bytes, batch.data(), bytes);
            pool_read_ms += msSince(read_start);

            faiss::IndexFlatL2 index(static_cast<faiss::idx_t>(args.dim));
            index.add(static_cast<faiss::idx_t>(rows), batch.data());
            index.search(static_cast<faiss::idx_t>(args.nq), xq.data(), static_cast<faiss::idx_t>(args.k),
                         distances.data(), labels.data());
            for (size_t qi = 0; qi < args.nq; qi++) {
                float *best_dist = best_distances.data() + qi * args.k;
                faiss::idx_t *best_label = best_labels.data() + qi * args.k;
                for (size_t rank = 0; rank < args.k; rank++) {
                    faiss::idx_t label = labels[qi * args.k + rank];
                    if (label < 0)
                        continue;
                    insertBest(best_dist, best_label, args.k, distances[qi * args.k + rank],
                               static_cast<faiss::idx_t>(base) + label);
                }
            }
        }
        for (faiss::idx_t label : best_labels) {
            checksum += static_cast<uint64_t>(std::max<faiss::idx_t>(0, label) + 1);
        }
    }
    return msSince(total_start);
}

double runDax(const Args &args, const std::vector<float> &xb, const std::vector<float> &xq, double &dax_write_ms,
              double &dax_read_ms, uint64_t &checksum) {
    const size_t vector_bytes = args.dim * sizeof(float);
    const size_t total_bytes = args.nb * vector_bytes;
    DaxMapping dax(args.dax_path, total_bytes);

    if (!args.skip_dax_write) {
        auto write_start = Clock::now();
        if (args.parallel_dax_write) {
            copyToDaxParallel(reinterpret_cast<volatile uint8_t *>(dax.data()),
                              reinterpret_cast<const uint8_t *>(xb.data()), total_bytes, activeThreadCount(args));
        } else {
            copyToDax(reinterpret_cast<volatile uint8_t *>(dax.data()), reinterpret_cast<const uint8_t *>(xb.data()),
                      total_bytes);
        }
        dax_write_ms = msSince(write_start);
    }

    std::vector<float> batch(args.block * args.dim);
    std::vector<float> distances(args.nq * args.k);
    std::vector<faiss::idx_t> labels(args.nq * args.k);
    std::vector<float> best_distances(args.nq * args.k);
    std::vector<faiss::idx_t> best_labels(args.nq * args.k);
    auto total_start = Clock::now();
    dax_read_ms = 0.0;
    for (size_t iter = 0; iter < args.iters; iter++) {
        std::fill(best_distances.begin(), best_distances.end(), std::numeric_limits<float>::infinity());
        std::fill(best_labels.begin(), best_labels.end(), faiss::idx_t{-1});

        for (size_t base = 0; base < args.nb; base += args.block) {
            size_t rows = std::min(args.block, args.nb - base);
            volatile const uint8_t *block = reinterpret_cast<volatile const uint8_t *>(dax.data() + base * args.dim);
            auto read_start = Clock::now();
            copyFromDax(reinterpret_cast<uint8_t *>(batch.data()), block, rows * vector_bytes);
            dax_read_ms += msSince(read_start);
            faiss::IndexFlatL2 index(static_cast<faiss::idx_t>(args.dim));
            index.add(static_cast<faiss::idx_t>(rows), batch.data());
            index.search(static_cast<faiss::idx_t>(args.nq), xq.data(), static_cast<faiss::idx_t>(args.k),
                         distances.data(), labels.data());
            for (size_t qi = 0; qi < args.nq; qi++) {
                float *best_dist = best_distances.data() + qi * args.k;
                faiss::idx_t *best_label = best_labels.data() + qi * args.k;
                for (size_t rank = 0; rank < args.k; rank++) {
                    faiss::idx_t label = labels[qi * args.k + rank];
                    if (label < 0)
                        continue;
                    insertBest(best_dist, best_label, args.k, distances[qi * args.k + rank],
                               static_cast<faiss::idx_t>(base) + label);
                }
            }
        }
        for (faiss::idx_t label : best_labels) {
            checksum += static_cast<uint64_t>(std::max<faiss::idx_t>(0, label) + 1);
        }
    }
    return msSince(total_start);
}

double runDaxZeroCopy(const Args &args, const std::vector<float> &xb, const std::vector<float> &xq,
                      double &dax_write_ms, double &dax_read_ms, uint64_t &checksum) {
    const size_t vector_bytes = args.dim * sizeof(float);
    const size_t total_bytes = args.nb * vector_bytes;
    DaxMapping dax(args.dax_path, total_bytes);

    if (!args.skip_dax_write) {
        auto write_start = Clock::now();
        if (args.parallel_dax_write) {
            copyToDaxParallel(reinterpret_cast<volatile uint8_t *>(dax.data()),
                              reinterpret_cast<const uint8_t *>(xb.data()), total_bytes, activeThreadCount(args));
        } else {
            copyToDax(reinterpret_cast<volatile uint8_t *>(dax.data()), reinterpret_cast<const uint8_t *>(xb.data()),
                      total_bytes);
        }
        dax_write_ms = msSince(write_start);
    }

    const int num_threads = activeThreadCount(args);
    std::vector<float> distances(args.nq * args.k);
    std::vector<faiss::idx_t> labels(args.nq * args.k);
    std::vector<float> thread_distances(static_cast<size_t>(num_threads) * args.nq * args.k);
    std::vector<faiss::idx_t> thread_labels(static_cast<size_t>(num_threads) * args.nq * args.k);
    std::vector<float> xq_t(args.dim * args.nq);
    for (size_t qi = 0; qi < args.nq; qi++) {
        for (size_t di = 0; di < args.dim; di++) {
            xq_t[di * args.nq + qi] = xq[qi * args.dim + di];
        }
    }
    volatile const uint8_t *db = reinterpret_cast<volatile const uint8_t *>(dax.data());

    auto total_start = Clock::now();
    for (size_t iter = 0; iter < args.iters; iter++) {
        std::fill(distances.begin(), distances.end(), std::numeric_limits<float>::infinity());
        std::fill(labels.begin(), labels.end(), faiss::idx_t{-1});
        std::fill(thread_distances.begin(), thread_distances.end(), std::numeric_limits<float>::infinity());
        std::fill(thread_labels.begin(), thread_labels.end(), faiss::idx_t{-1});

#ifdef _OPENMP
#pragma omp parallel num_threads(num_threads)
#endif
        {
            int tid = 0;
#ifdef _OPENMP
            tid = omp_get_thread_num();
#endif
            float *local_dist = thread_distances.data() + static_cast<size_t>(tid) * args.nq * args.k;
            faiss::idx_t *local_label = thread_labels.data() + static_cast<size_t>(tid) * args.nq * args.k;
            std::vector<float> accum(args.nq);

#ifdef _OPENMP
#pragma omp for schedule(static)
#endif
            for (size_t bi = 0; bi < args.nb; bi++) {
                std::fill(accum.begin(), accum.end(), 0.0f);
                volatile const uint8_t *vec = db + bi * vector_bytes;

                for (size_t di = 0; di < args.dim; di++) {
                    volatile const uint8_t *value = vec + di * sizeof(float);
                    float db_value = 0.0f;
                    if (args.dax_load == "byte") {
                        db_value = loadFloatFromDax(value);
                    } else if (args.dax_load == "float") {
                        db_value = loadFloatDirectFromDax(value);
                    } else {
                        db_value = loadFloatU32FromDax(value);
                    }

                    const float *query_column = xq_t.data() + di * args.nq;
                    for (size_t qi = 0; qi < args.nq; qi++) {
                        float diff = query_column[qi] - db_value;
                        accum[qi] += diff * diff;
                    }
                }

                for (size_t qi = 0; qi < args.nq; qi++) {
                    insertBest(local_dist + qi * args.k, local_label + qi * args.k, args.k, accum[qi],
                               static_cast<faiss::idx_t>(bi));
                }
            }
        }

        for (int tid = 0; tid < num_threads; tid++) {
            const float *local_dist = thread_distances.data() + static_cast<size_t>(tid) * args.nq * args.k;
            const faiss::idx_t *local_label = thread_labels.data() + static_cast<size_t>(tid) * args.nq * args.k;
            for (size_t qi = 0; qi < args.nq; qi++) {
                float *best_dist = distances.data() + qi * args.k;
                faiss::idx_t *best_label = labels.data() + qi * args.k;
                for (size_t rank = 0; rank < args.k; rank++) {
                    faiss::idx_t label = local_label[qi * args.k + rank];
                    if (label < 0)
                        continue;
                    insertBest(best_dist, best_label, args.k, local_dist[qi * args.k + rank], label);
                }
            }
        }

        for (faiss::idx_t label : labels) {
            checksum += static_cast<uint64_t>(std::max<faiss::idx_t>(0, label) + 1);
        }
    }
    dax_read_ms = msSince(total_start);
    return dax_read_ms;
}

} // namespace

int main(int argc, char **argv) {
    try {
        Args args = parseArgs(argc, argv);
#ifdef _OPENMP
        if (args.threads > 0)
            omp_set_num_threads(args.threads);
#endif

        const size_t db_bytes = args.nb * args.dim * sizeof(float);
        if (args.storage == "cxl-pool" && db_bytes > args.pool_mb * 1024ULL * 1024ULL) {
            throw std::runtime_error("database does not fit in requested CXL pool");
        }

        std::vector<float> xb(args.nb * args.dim);
        std::vector<float> xq(args.nq * args.dim);
        fillVectors(xb.data(), args.nb, args.dim, 0x1234u + static_cast<uint32_t>(args.node));
        fillVectors(xq.data(), args.nq, args.dim, 0x9876u + static_cast<uint32_t>(args.node));

        uint64_t checksum = 0;
        double total_ms = 0.0;
        double pool_write_ms = 0.0;
        double pool_read_ms = 0.0;
        if (args.storage == "native") {
            total_ms = runNative(args, xb, xq, checksum);
        } else if (args.storage == "dax") {
            total_ms = runDax(args, xb, xq, pool_write_ms, pool_read_ms, checksum);
        } else if (args.storage == "dax-zero-copy") {
            total_ms = runDaxZeroCopy(args, xb, xq, pool_write_ms, pool_read_ms, checksum);
        } else {
            total_ms = runCxlPool(args, xb, xq, pool_write_ms, pool_read_ms, checksum);
        }

        double effective_queries = static_cast<double>(args.nq) * static_cast<double>(args.iters);
        std::cout << "FAISS_SPLASH_RESULT"
                  << " node=" << args.node << " storage=" << args.storage << " nb=" << args.nb << " nq=" << args.nq
                  << " dim=" << args.dim << " k=" << args.k << " iters=" << args.iters << " block=" << args.block
                  << " db_mb=" << (static_cast<double>(db_bytes) / (1024.0 * 1024.0)) << " total_ms=" << total_ms
                  << " pool_write_ms=" << pool_write_ms << " pool_read_ms=" << pool_read_ms
                  << " qps=" << (effective_queries / (total_ms / 1000.0)) << " checksum=" << checksum << "\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAISS_SPLASH_ERROR " << e.what() << "\n";
        return 1;
    }
}
