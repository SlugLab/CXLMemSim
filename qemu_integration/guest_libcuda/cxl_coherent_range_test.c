#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "libcuda.c"

static uint8_t fake_bar2[CXL_GPU_CMD_REG_SIZE];
static uint8_t fake_bar4[256];

static void test_command_numbers_match_host(void) {
    assert(CXL_GPU_CMD_COHERENT_LOAD == 0x83);
    assert(CXL_GPU_CMD_COHERENT_STORE == 0x84);
    assert(CXL_GPU_CMD_COHERENT_FAA == 0x85);
    assert(CXL_GPU_CMD_COHERENT_CAS == 0x86);
    assert(CXL_GPU_CMD_COH_ACQUIRE_RANGE == 0xB2);
    assert(CXL_GPU_CMD_COH_RELEASE_RANGE == 0xB3);
    assert(CXL_GPU_ERROR_COHERENCY == 1000);
}

static void reset_fake_device(void) {
    memset(fake_bar2, 0, sizeof(fake_bar2));
    memset(fake_bar4, 0, sizeof(fake_bar4));
    g_regs = (volatile uint32_t *)fake_bar2;
    g_data = fake_bar2 + CXL_GPU_DATA_OFFSET;
    g_bar4_ptr = fake_bar4;
    g_bar4_size = sizeof(fake_bar4);
    g_pci_fd = -1;

    reg_write32(CXL_GPU_REG_CMD_STATUS, CXL_GPU_CMD_STATUS_COMPLETE);
    reg_write32(CXL_GPU_REG_CMD_RESULT, CXL_GPU_SUCCESS);
}

static void detach_fake_device(void) {
    g_regs = NULL;
    g_data = NULL;
    g_bar4_ptr = NULL;
    g_bar4_size = 0;
}

static void test_acquire_uses_range_abi(void) {
    uint64_t device_ptr = 0;
    uint64_t lines_granted = 0;

    reset_fake_device();
    reg_write64(CXL_GPU_REG_RESULT0, 2);
    reg_write64(CXL_GPU_REG_RESULT1, UINT64_C(0x123456789abcdef0));

    assert(cxlCoherentAcquireRange(fake_bar4 + 17, 81, CXL_COH_RANGE_WRITE, &device_ptr, &lines_granted) ==
           CXL_GPU_SUCCESS);
    assert(reg_read32(CXL_GPU_REG_CMD) == CXL_GPU_CMD_COH_ACQUIRE_RANGE);
    assert(reg_read64(CXL_GPU_REG_PARAM0) == 17);
    assert(reg_read64(CXL_GPU_REG_PARAM1) == 81);
    assert(reg_read64(CXL_GPU_REG_PARAM2) == CXL_COH_RANGE_WRITE);
    assert(lines_granted == 2);
    assert(device_ptr == UINT64_C(0x123456789abcdef0));
}

static void test_partial_acquire_reports_granted_lines(void) {
    uint64_t device_ptr = UINT64_MAX;
    uint64_t lines_granted = 0;

    reset_fake_device();
    reg_write32(CXL_GPU_REG_CMD_RESULT, CXL_GPU_ERROR_COHERENCY);
    reg_write64(CXL_GPU_REG_RESULT0, 1);
    reg_write64(CXL_GPU_REG_RESULT1, UINT64_C(0xfeedface));

    assert(cxlCoherentAcquireRange(fake_bar4, 128, CXL_COH_RANGE_READ, &device_ptr, &lines_granted) ==
           CXL_GPU_ERROR_COHERENCY);
    assert(lines_granted == 1);
    assert(device_ptr == 0);
}

static void test_non_coherency_error_does_not_report_stale_results(void) {
    uint64_t device_ptr = UINT64_MAX;
    uint64_t lines_granted = UINT64_MAX;

    reset_fake_device();
    reg_write32(CXL_GPU_REG_CMD_RESULT, CXL_GPU_ERROR_INVALID_VALUE);
    reg_write64(CXL_GPU_REG_RESULT0, 7);
    reg_write64(CXL_GPU_REG_RESULT1, UINT64_C(0xfeedface));

    assert(cxlCoherentAcquireRange(fake_bar4, 64, CXL_COH_RANGE_READ, &device_ptr, &lines_granted) ==
           CXL_GPU_ERROR_INVALID_VALUE);
    assert(lines_granted == 0);
    assert(device_ptr == 0);
}

static void test_release_uses_range_abi(void) {
    reset_fake_device();

    assert(cxlCoherentReleaseRange(fake_bar4 + 32, 64, 1) == CXL_GPU_SUCCESS);
    assert(reg_read32(CXL_GPU_REG_CMD) == CXL_GPU_CMD_COH_RELEASE_RANGE);
    assert(reg_read64(CXL_GPU_REG_PARAM0) == 32);
    assert(reg_read64(CXL_GPU_REG_PARAM1) == 64);
    assert(reg_read64(CXL_GPU_REG_PARAM2) == 1);
}

static void test_range_validation_is_fail_closed(void) {
    uint64_t device_ptr = UINT64_MAX;
    uint64_t lines_granted = UINT64_MAX;

    reset_fake_device();
    reg_write32(CXL_GPU_REG_CMD, UINT32_C(0xdeadbeef));

    assert(cxlCoherentAcquireRange(NULL, 1, CXL_COH_RANGE_READ, &device_ptr, &lines_granted) ==
           CXL_GPU_ERROR_INVALID_VALUE);
    assert(cxlCoherentAcquireRange(fake_bar4, 0, CXL_COH_RANGE_READ, &device_ptr, &lines_granted) ==
           CXL_GPU_ERROR_INVALID_VALUE);
    assert(cxlCoherentAcquireRange(fake_bar4, 1, 2, &device_ptr, &lines_granted) == CXL_GPU_ERROR_INVALID_VALUE);
    assert(cxlCoherentAcquireRange(fake_bar4, 1, CXL_COH_RANGE_READ, NULL, &lines_granted) ==
           CXL_GPU_ERROR_INVALID_VALUE);
    assert(cxlCoherentAcquireRange(fake_bar4, 1, CXL_COH_RANGE_READ, &device_ptr, NULL) == CXL_GPU_ERROR_INVALID_VALUE);
    assert(cxlCoherentAcquireRange(fake_bar4 + sizeof(fake_bar4) - 16, 17, CXL_COH_RANGE_READ, &device_ptr,
                                   &lines_granted) == CXL_GPU_ERROR_INVALID_VALUE);
    assert(cxlCoherentAcquireRange(fake_bar4, UINT64_MAX, CXL_COH_RANGE_READ, &device_ptr, &lines_granted) ==
           CXL_GPU_ERROR_INVALID_VALUE);
    assert(cxlCoherentReleaseRange(NULL, 1, 0) == CXL_GPU_ERROR_INVALID_VALUE);
    assert(cxlCoherentReleaseRange(fake_bar4, 0, 0) == CXL_GPU_ERROR_INVALID_VALUE);
    assert(cxlCoherentReleaseRange(fake_bar4, 1, 2) == CXL_GPU_ERROR_INVALID_VALUE);
    assert(cxlCoherentReleaseRange(fake_bar4 + sizeof(fake_bar4), 1, 0) == CXL_GPU_ERROR_INVALID_VALUE);
    assert(reg_read32(CXL_GPU_REG_CMD) == UINT32_C(0xdeadbeef));
    assert(device_ptr == 0);
    assert(lines_granted == 0);
}

static void test_htod_from_bar4_uses_bulk_command(void) {
    reset_fake_device();
    g_initialized = 1;

    assert(cuMemcpyHtoD_v2(UINT64_C(0x12345678), fake_bar4 + 17, 81) == CUDA_SUCCESS);
    assert(reg_read32(CXL_GPU_REG_CMD) == CXL_GPU_CMD_BULK_HTOD);
    assert(reg_read64(CXL_GPU_REG_PARAM0) == 17);
    assert(reg_read64(CXL_GPU_REG_PARAM1) == UINT64_C(0x12345678));
    assert(reg_read64(CXL_GPU_REG_PARAM2) == 81);
    g_initialized = 0;
}

int main(void) {
    test_command_numbers_match_host();
    test_acquire_uses_range_abi();
    test_partial_acquire_reports_granted_lines();
    test_non_coherency_error_does_not_report_stale_results();
    test_release_uses_range_abi();
    test_range_validation_is_fail_closed();
    test_htod_from_bar4_uses_bulk_command();
    detach_fake_device();
    puts("cxl coherent range guest tests: PASS");
    return 0;
}
