/*
 * CXLMemSim bandwidth test
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
 *      Brian Zhao
 *  SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */

#include <cerrno>
#include <cstdbool>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/io.h>
#include <sys/mman.h>
#include <unistd.h>

#define MAX_CH 8 // Maximum number of channels
#define MAX_DIMM 2 // Maximum DIMMs per channel
#define MAX_SOCKET 2 // Maximum number of sockets

// PCI configuration space access methods
#define PCI_CONF_METHOD1 1
#define PCI_CONF_METHOD2 2

// PCI configuration registers
#define PCI_ADDR_PORT 0xCF8
#define PCI_DATA_PORT 0xCFC

// SPR Memory Controller PCI configuration
#define MC_BUS 0x00 // Bus 0x00 where memory controller is
#define MC_DEV_BASE 0x14 // Device 0x14 from lspci
#define MC_FUNC_BASE 0x02 // Function 2 from lspci
typedef union {
    struct {
        uint32_t thrt_mid : 8; // Bits 7:0   - Default: 0xFF
        uint32_t thrt_hi : 8; // Bits 15:8  - Default: 0x0F
        uint32_t thrt_crit : 8; // Bits 23:16 - Default: 0x00
        uint32_t reserved : 8; // Bits 31:24 - Reserved, read-only
    } Bits;
    uint32_t Data;
} DIMM_TEMP_THRT_LMT_STRUCT;

// Event Assert Register structure (at offset 22408h)
typedef union {
    struct {
        uint32_t ev_asrt_dimm0 : 1; // Bit 0 - Event Asserted on DIMM ID 0
        uint32_t ev_asrt_dimm1 : 1; // Bit 1 - Event Asserted on DIMM ID 1
        uint32_t reserved : 30; // Bits 31:2 - Reserved
    } Bits;
    uint32_t Data;
} DIMM_TEMP_THRT_EV_STRUCT;
typedef struct {
    uint8_t maxDimm; // Number of DIMMs installed in this channel
} CHANNEL_CONFIG;

// Constants for register offsets
#define DIMM_TEMP_THRT_LMT_0_MCDDC_CTL_REG 0x2241C // Throttling limits register
#define DIMM_TEMP_THRT_EV_REG 0x22408 // Event assert register

// Default values
#define DIMM_TEMP_THRT_LMT_DEFAULT 0x00000FFF // Default value per documentation
#define THRT_MID_DEFAULT 0xFF // Default for THRT_MID
#define THRT_HI_DEFAULT 0x0F // Default for THRT_HI
#define THRT_CRIT_DEFAULT 0x00 // Default for THRT_CRIT
// Helper function to get device and function numbers for a specific channel

// Initialize PCI access
static int InitPciAccess(void) {
    // Request permission to access I/O ports
    if (iopl(3) < 0) {
        perror("Failed to get I/O permission");
        return -1;
    }
    return 0;
}

// Generate PCI configuration address
static uint32_t GeneratePciAddr(uint8_t bus, uint8_t dev, uint8_t func, uint32_t reg) {
    uint32_t addr;
    addr =
        ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)func << 8) | (reg & 0xFC) | 0x80000000; // Enable bit
    return addr;
}
static void GetMcDevFunc(uint8_t socket, uint8_t channel, uint8_t *dev, uint8_t *func) {
    uint8_t mcIdx = channel / 2; // 2 channels per MC
    *dev = MC_DEV_BASE + (socket * 4) + mcIdx; // 4 MCs per socket
    *func = MC_FUNC_BASE + (channel % 2); // Alternate functions for channels

    printf("Debug - Socket %d, Channel %d -> Bus 0x%02X, Dev 0x%02X, Func 0x%02X\n", socket, channel, MC_BUS, *dev,
           *func);
}
// Function to read PCI config space using sysfs
uint32_t ReadPciConfig(uint8_t bus, uint8_t dev, uint8_t func, uint32_t reg) {
    char path[256];
    uint32_t data = 0xFFFFFFFF;

    // Format sysfs path for PCI device
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/0000:%02x:%02x.%x/config", bus, dev, func);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", path, strerror(errno));
        return 0xFFFFFFFF;
    }

    // Seek to register offset
    if (lseek(fd, reg, SEEK_SET) != reg) {
        fprintf(stderr, "Failed to seek to register 0x%x: %s\n", reg, strerror(errno));
        close(fd);
        return 0xFFFFFFFF;
    }

    // Read register value
    if (read(fd, &data, sizeof(data)) != sizeof(data)) {
        fprintf(stderr, "Failed to read register 0x%x: %s\n", reg, strerror(errno));
        data = 0xFFFFFFFF;
    }

    close(fd);
    return data;
}

// Function to write PCI config space using sysfs
void WritePciConfig(uint8_t bus, uint8_t dev, uint8_t func, uint32_t reg, uint32_t value) {
    char path[256];

    // Format sysfs path for PCI device
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/0000:%02x:%02x.%x/config", bus, dev, func);

    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", path, strerror(errno));
        return;
    }

    // Seek to register offset
    if (lseek(fd, reg, SEEK_SET) != reg) {
        fprintf(stderr, "Failed to seek to register 0x%x: %s\n", reg, strerror(errno));
        close(fd);
        return;
    }

    // Write register value
    if (write(fd, &value, sizeof(value)) != sizeof(value)) {
        fprintf(stderr, "Failed to write register 0x%x: %s\n", reg, strerror(errno));
    }

    close(fd);
}

// Updated MemReadPciCfgEp to use sysfs
uint32_t MemReadPciCfgEp(uint8_t socket, uint8_t ch, uint32_t reg) {
    uint8_t dev, func;
    GetMcDevFunc(socket, ch, &dev, &func);

    uint32_t data = ReadPciConfig(MC_BUS, dev, func, reg);
    printf("Debug - PCI Read - Bus:0x%02x Dev:0x%02x Func:0x%02x Reg:0x%04x Data:0x%08x\n", MC_BUS, dev, func, reg,
           data);

    return data;
}

// Updated MemWritePciCfgEp to use sysfs
void MemWritePciCfgEp(uint8_t socket, uint8_t ch, uint32_t reg, uint32_t value) {
    uint8_t dev, func;
    GetMcDevFunc(socket, ch, &dev, &func);

    printf("Debug - PCI Write - Bus:0x%02x Dev:0x%02x Func:0x%02x Reg:0x%04x Data:0x%08x\n", MC_BUS, dev, func, reg,
           value);

    WritePciConfig(MC_BUS, dev, func, reg, value);
}
// Function to set THRT_HI value for a specific DIMM
void SetDimmThrtHi(uint8_t socket, uint8_t ch, uint8_t dimm, bool isTTMode, uint8_t ttThrtHi,
                   uint8_t peakBWLimitPercent, CHANNEL_CONFIG *channelConfig) {

    DIMM_TEMP_THRT_LMT_STRUCT dimmTempThrtLmt;
    uint32_t regOffset;

    // Calculate register offset for this DIMM
    regOffset = DIMM_TEMP_THRT_LMT_0_MCDDC_CTL_REG + (dimm * 4);

    // Read current register value
    dimmTempThrtLmt.Data = MemReadPciCfgEp(socket, ch, regOffset);

    if (isTTMode) {
        // Set THRT_HI directly from TTMODE value
        dimmTempThrtLmt.Bits.thrt_hi = ttThrtHi;
    } else {
        // Calculate THRT_HI based on peak bandwidth limit
        // Formula: (peakBWLimitPercent * 255 / 100) / maxDimm
        if (channelConfig[ch].maxDimm > 0) {
            dimmTempThrtLmt.Bits.thrt_hi = ((peakBWLimitPercent * 255) / 100) / channelConfig[ch].maxDimm;
        }
    }

    // Write back the updated value
    MemWritePciCfgEp(socket, ch, regOffset, dimmTempThrtLmt.Data);
}

// Function to scan for memory controllers
void ScanMemoryControllers(void) {
    DIR *dir;
    struct dirent *entry;
    char path[] = "/sys/bus/pci/devices/";

    printf("Scanning for memory controllers...\n");

    dir = opendir(path);
    if (!dir) {
        perror("Failed to open /sys/bus/pci/devices");
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_LNK) { // Look for symbolic links
            char class_path[512];
            char class_id[8];
            int fd;

            snprintf(class_path, sizeof(class_path), "%s%s/class", path, entry->d_name);

            fd = open(class_path, O_RDONLY);
            if (fd >= 0) {
                if (read(fd, class_id, sizeof(class_id)) > 0) {
                    // Class 0x058000 is memory controller
                    if (strncmp(class_id, "0x058000", 8) == 0) {
                        printf("Found memory controller at %s\n", entry->d_name);
                    }
                }
                close(fd);
            }
        }
    }

    closedir(dir);
}

int set_bw() {
    // First scan for memory controllers
    ScanMemoryControllers();
    // Example usage
    CHANNEL_CONFIG channelConfig[MAX_CH] = {0};
    uint32_t beforeVal, afterVal;
    uint32_t regOffset;

    // Example configuration: 2 DIMMs per channel
    for (int i = 0; i < MAX_CH; i++) {
        channelConfig[i].maxDimm = 2;
    }

    printf("\nSetting THRT_HI values for all sockets and channels...\n");
    for (int socket = 0; socket < MAX_SOCKET; socket++) {
        for (int i = 0; i < MAX_CH; i++) {
            for (int j = 0; j < channelConfig[i].maxDimm; j++) {
                // Calculate register offset
                regOffset = DIMM_TEMP_THRT_LMT_0_MCDDC_CTL_REG + (j * 4);

                // Read before value
                beforeVal = MemReadPciCfgEp(socket, i, regOffset);
                printf("Socket %d, Channel %d, DIMM %d - Before: 0x%08X\n", socket, i, j, beforeVal);

                // Set new value
                SetDimmThrtHi(socket, i, j,
                              true, // TTMODE enabled
                              0x80, // TTTHRT_HI value
                              0, // peak BW limit not used in TTMODE
                              channelConfig);

                // Read after value
                afterVal = MemReadPciCfgEp(socket, i, regOffset);
                printf("Socket %d, Channel %d, DIMM %d - After:  0x%08X\n", socket, i, j, afterVal);

                // Print the specific THRT_HI field values
                DIMM_TEMP_THRT_LMT_STRUCT before = {.Data = beforeVal};
                DIMM_TEMP_THRT_LMT_STRUCT after = {.Data = afterVal};
                printf("THRT_HI field - Before: 0x%02X, After: 0x%02X\n\n", before.Bits.thrt_hi, after.Bits.thrt_hi);
            }
        }
    }

    return 0;
}