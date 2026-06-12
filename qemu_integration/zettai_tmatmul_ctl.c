// SPDX-License-Identifier: MIT
/*
 * Guest-side helper for the current Zettai CXL Type-2 / switch CCI test ABI.
 *
 * The kernel driver currently exposes ioctl commands on /dev/zettai_cxl*.
 * It does not implement io_uring_cmd yet, so this helper intentionally uses
 * the ioctl ABI while keeping command names close to the CXL.mem/tmatmul flow.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define CXL_TYPE2_TMATMUL_UAPI_VERSION 1
#define CXL_TYPE2_TMATMUL_DEV_ID 0x544D4D31U

#define CXL_TYPE2_TMATMUL_RUN_SMOKE (1U << 0)

#define CXL_TYPE2_TMATMUL_RESULT_STALLED (1U << 0)
#define CXL_TYPE2_TMATMUL_RESULT_OUTPUT_ZERO (1U << 1)
#define CXL_TYPE2_TMATMUL_RESULT_DMA_ERROR (1U << 2)

#define CXL_TYPE2_MEM_REQ_READ 0
#define CXL_TYPE2_MEM_REQ_WRITE 1
#define CXL_TYPE2_MEM_REQ_MAX_BYTES (1U << 20)

struct cxl_type2_tmatmul_info {
    uint32_t version;
    uint32_t dev_id;
    uint32_t num_instances;
    uint32_t dim_d;
    uint32_t ddr_data_width;
    uint32_t mc_status;
    uint32_t reserved0;
    uint64_t default_hpa_base;
    uint64_t default_hpa_size;
    uint64_t reserved1[4];
};

struct cxl_type2_tmatmul_run {
    uint64_t hpa_base;
    uint64_t hpa_size;
    uint32_t timeout_ms;
    uint32_t flags;
    uint32_t dma_status;
    uint32_t stall_status;
    uint32_t instr_count;
    uint32_t dim_d;
    uint32_t result_flags;
    uint32_t reserved0;
    uint64_t reserved1[4];
};

struct cxl_type2_mem_req {
    uint64_t hpa_base;
    uint64_t hpa_size;
    uint64_t offset;
    uint64_t user_ptr;
    uint32_t size;
    uint32_t op;
    uint32_t flags;
    uint32_t reserved0;
    uint64_t reserved1[4];
};

#define CXL_TYPE2_TMATMUL_IOC_MAGIC 0xCE
#define CXL_TYPE2_TMATMUL_GET_INFO \
    _IOR(CXL_TYPE2_TMATMUL_IOC_MAGIC, 0x00, struct cxl_type2_tmatmul_info)
#define CXL_TYPE2_TMATMUL_RUN \
    _IOWR(CXL_TYPE2_TMATMUL_IOC_MAGIC, 0x01, struct cxl_type2_tmatmul_run)
#define CXL_TYPE2_MEM_IO \
    _IOWR(CXL_TYPE2_TMATMUL_IOC_MAGIC, 0x02, struct cxl_type2_mem_req)

enum action {
    ACTION_NONE,
    ACTION_INFO,
    ACTION_SMOKE,
    ACTION_MEM_READ,
    ACTION_MEM_WRITE,
};

struct options {
    const char *dev_path;
    enum action action;
    uint64_t hpa_base;
    uint64_t hpa_size;
    uint64_t offset;
    uint32_t size;
    uint32_t timeout_ms;
    uint8_t pattern;
};

static void usage(const char *prog)
{
    printf("Usage: %s [options] --info|--smoke|--mem-read|--mem-write\n", prog);
    printf("\n");
    printf("Options:\n");
    printf("  --dev PATH          Zettai char device (default: /dev/zettai_cxl0d003)\n");
    printf("  --hpa-base ADDR     CXL.mem HPA base. Required when driver default is zero.\n");
    printf("  --hpa-size BYTES    CXL.mem HPA window size (default: use driver default)\n");
    printf("  --offset BYTES      CXL.mem offset for read/write (default: 0)\n");
    printf("  --size BYTES        CXL.mem transfer size, max 1 MiB (default: 64)\n");
    printf("  --pattern BYTE      Pattern for --mem-write (default: 0x5a)\n");
    printf("  --timeout-ms MS     tmatmul smoke timeout (default: 1000)\n");
    printf("  --help              Show this help\n");
    printf("\n");
    printf("Note: the current kernel driver uses ioctl(), not io_uring_cmd.\n");
}

static int parse_u64(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(text, &end, 0);
    if (errno || !end || *end != '\0') {
        return -1;
    }

    *value = (uint64_t)parsed;
    return 0;
}

static int parse_u32(const char *text, uint32_t *value)
{
    uint64_t parsed;

    if (parse_u64(text, &parsed) || parsed > UINT32_MAX) {
        return -1;
    }

    *value = (uint32_t)parsed;
    return 0;
}

static int parse_u8(const char *text, uint8_t *value)
{
    uint64_t parsed;

    if (parse_u64(text, &parsed) || parsed > UINT8_MAX) {
        return -1;
    }

    *value = (uint8_t)parsed;
    return 0;
}

static int require_arg(int argc, char **argv, int *idx)
{
    if (*idx + 1 >= argc) {
        fprintf(stderr, "missing argument for %s\n", argv[*idx]);
        return -1;
    }

    (*idx)++;
    return 0;
}

static int parse_args(int argc, char **argv, struct options *opts)
{
    memset(opts, 0, sizeof(*opts));
    opts->dev_path = "/dev/zettai_cxl0d003";
    opts->action = ACTION_NONE;
    opts->size = 64;
    opts->timeout_ms = 1000;
    opts->pattern = 0x5a;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            exit(0);
        } else if (!strcmp(argv[i], "--info")) {
            opts->action = ACTION_INFO;
        } else if (!strcmp(argv[i], "--smoke")) {
            opts->action = ACTION_SMOKE;
        } else if (!strcmp(argv[i], "--mem-read")) {
            opts->action = ACTION_MEM_READ;
        } else if (!strcmp(argv[i], "--mem-write")) {
            opts->action = ACTION_MEM_WRITE;
        } else if (!strcmp(argv[i], "--dev")) {
            if (require_arg(argc, argv, &i)) {
                return -1;
            }
            opts->dev_path = argv[i];
        } else if (!strcmp(argv[i], "--hpa-base")) {
            if (require_arg(argc, argv, &i) || parse_u64(argv[i], &opts->hpa_base)) {
                fprintf(stderr, "invalid --hpa-base value\n");
                return -1;
            }
        } else if (!strcmp(argv[i], "--hpa-size")) {
            if (require_arg(argc, argv, &i) || parse_u64(argv[i], &opts->hpa_size)) {
                fprintf(stderr, "invalid --hpa-size value\n");
                return -1;
            }
        } else if (!strcmp(argv[i], "--offset")) {
            if (require_arg(argc, argv, &i) || parse_u64(argv[i], &opts->offset)) {
                fprintf(stderr, "invalid --offset value\n");
                return -1;
            }
        } else if (!strcmp(argv[i], "--size")) {
            if (require_arg(argc, argv, &i) || parse_u32(argv[i], &opts->size)) {
                fprintf(stderr, "invalid --size value\n");
                return -1;
            }
        } else if (!strcmp(argv[i], "--pattern")) {
            if (require_arg(argc, argv, &i) || parse_u8(argv[i], &opts->pattern)) {
                fprintf(stderr, "invalid --pattern value\n");
                return -1;
            }
        } else if (!strcmp(argv[i], "--timeout-ms")) {
            if (require_arg(argc, argv, &i) || parse_u32(argv[i], &opts->timeout_ms)) {
                fprintf(stderr, "invalid --timeout-ms value\n");
                return -1;
            }
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return -1;
        }
    }

    if (opts->action == ACTION_NONE) {
        fprintf(stderr, "select one action: --info, --smoke, --mem-read, or --mem-write\n");
        return -1;
    }

    if ((opts->action == ACTION_MEM_READ || opts->action == ACTION_MEM_WRITE) &&
        (!opts->size || opts->size > CXL_TYPE2_MEM_REQ_MAX_BYTES)) {
        fprintf(stderr, "--size must be between 1 and %u bytes\n", CXL_TYPE2_MEM_REQ_MAX_BYTES);
        return -1;
    }

    return 0;
}

static void print_info(const struct cxl_type2_tmatmul_info *info)
{
    printf("version=%u\n", info->version);
    printf("dev_id=0x%08x\n", info->dev_id);
    printf("tmatmul_present=%s\n", info->dev_id == CXL_TYPE2_TMATMUL_DEV_ID ? "yes" : "no");
    printf("num_instances=%u\n", info->num_instances);
    printf("dim_d=%u\n", info->dim_d);
    printf("ddr_data_width=%u\n", info->ddr_data_width);
    printf("mc_status=0x%08x\n", info->mc_status);
    printf("default_hpa_base=0x%016" PRIx64 "\n", info->default_hpa_base);
    printf("default_hpa_size=0x%016" PRIx64 "\n", info->default_hpa_size);
}

static void print_run(const struct cxl_type2_tmatmul_run *run)
{
    printf("dma_status=0x%08x\n", run->dma_status);
    printf("stall_status=0x%08x\n", run->stall_status);
    printf("instr_count=%u\n", run->instr_count);
    printf("dim_d=%u\n", run->dim_d);
    printf("result_flags=0x%08x", run->result_flags);
    if (run->result_flags) {
        printf(" [");
        if (run->result_flags & CXL_TYPE2_TMATMUL_RESULT_STALLED) {
            printf(" stalled");
        }
        if (run->result_flags & CXL_TYPE2_TMATMUL_RESULT_OUTPUT_ZERO) {
            printf(" output_zero");
        }
        if (run->result_flags & CXL_TYPE2_TMATMUL_RESULT_DMA_ERROR) {
            printf(" dma_error");
        }
        printf(" ]");
    }
    printf("\n");
}

static void hexdump(const uint8_t *buf, uint32_t size)
{
    uint32_t shown = size < 256 ? size : 256;

    for (uint32_t i = 0; i < shown; i += 16) {
        uint32_t line = shown - i < 16 ? shown - i : 16;

        printf("%08x:", i);
        for (uint32_t j = 0; j < line; j++) {
            printf(" %02x", buf[i + j]);
        }
        printf("\n");
    }

    if (shown != size) {
        printf("... %u bytes not shown\n", size - shown);
    }
}

static int do_info(int fd)
{
    struct cxl_type2_tmatmul_info info;

    memset(&info, 0, sizeof(info));
    if (ioctl(fd, CXL_TYPE2_TMATMUL_GET_INFO, &info) < 0) {
        perror("CXL_TYPE2_TMATMUL_GET_INFO");
        return 1;
    }

    print_info(&info);
    if (info.dev_id != CXL_TYPE2_TMATMUL_DEV_ID) {
        fprintf(stderr, "warning: tmatmul CSR block is not present on this device\n");
    }

    return 0;
}

static int do_smoke(int fd, const struct options *opts)
{
    struct cxl_type2_tmatmul_run run;
    int ret;
    int saved_errno;

    memset(&run, 0, sizeof(run));
    run.hpa_base = opts->hpa_base;
    run.hpa_size = opts->hpa_size;
    run.timeout_ms = opts->timeout_ms;
    run.flags = CXL_TYPE2_TMATMUL_RUN_SMOKE;

    ret = ioctl(fd, CXL_TYPE2_TMATMUL_RUN, &run);
    saved_errno = errno;
    print_run(&run);
    if (ret < 0) {
        errno = saved_errno;
        perror("CXL_TYPE2_TMATMUL_RUN");
        if (saved_errno == ENODEV) {
            fprintf(stderr, "hint: dmesg tmatmul=0 means QEMU did not expose the tmatmul CSR BAR window\n");
        } else if (saved_errno == ENOMEM && !opts->hpa_base) {
            fprintf(stderr, "hint: pass --hpa-base from a CXL region/decoder resource\n");
        }
        return 1;
    }

    return 0;
}

static int do_mem_io(int fd, const struct options *opts, int write_op)
{
    struct cxl_type2_mem_req req;
    uint8_t *buf;
    int ret;
    int saved_errno;

    buf = malloc(opts->size);
    if (!buf) {
        perror("malloc");
        return 1;
    }

    if (write_op) {
        memset(buf, opts->pattern, opts->size);
    } else {
        memset(buf, 0, opts->size);
    }

    memset(&req, 0, sizeof(req));
    req.hpa_base = opts->hpa_base;
    req.hpa_size = opts->hpa_size;
    req.offset = opts->offset;
    req.user_ptr = (uint64_t)(uintptr_t)buf;
    req.size = opts->size;
    req.op = write_op ? CXL_TYPE2_MEM_REQ_WRITE : CXL_TYPE2_MEM_REQ_READ;

    ret = ioctl(fd, CXL_TYPE2_MEM_IO, &req);
    saved_errno = errno;
    if (ret < 0) {
        errno = saved_errno;
        perror("CXL_TYPE2_MEM_IO");
        if (!opts->hpa_base) {
            fprintf(stderr, "hint: pass --hpa-base from a CXL region/decoder resource\n");
        }
        free(buf);
        return 1;
    }

    if (write_op) {
        printf("wrote %u bytes at hpa_base=0x%016" PRIx64 " offset=0x%016" PRIx64 " pattern=0x%02x\n",
               opts->size, opts->hpa_base, opts->offset, opts->pattern);
    } else {
        printf("read %u bytes at hpa_base=0x%016" PRIx64 " offset=0x%016" PRIx64 "\n",
               opts->size, opts->hpa_base, opts->offset);
        hexdump(buf, opts->size);
    }

    free(buf);
    return 0;
}

int main(int argc, char **argv)
{
    struct options opts;
    int fd;
    int rc;

    if (parse_args(argc, argv, &opts)) {
        usage(argv[0]);
        return 2;
    }

    fd = open(opts.dev_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror(opts.dev_path);
        return 1;
    }

    switch (opts.action) {
    case ACTION_INFO:
        rc = do_info(fd);
        break;
    case ACTION_SMOKE:
        rc = do_smoke(fd, &opts);
        break;
    case ACTION_MEM_READ:
        rc = do_mem_io(fd, &opts, 0);
        break;
    case ACTION_MEM_WRITE:
        rc = do_mem_io(fd, &opts, 1);
        break;
    default:
        rc = 2;
        break;
    }

    close(fd);
    return rc;
}
