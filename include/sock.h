//
// Created by root on 11/21/23.
//

#ifndef CXLMEMSIM_SOCK_H
#define CXLMEMSIM_SOCK_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
enum opcode {
    CXLMEMSIM_PROCESS_CREATE = 0,
    CXLMEMSIM_THREAD_CREATE = 1,
    CXLMEMSIM_THREAD_EXIT = 2,
    CXLMEMSIM_STABLE_SIGNAL = 3,
};
struct op_data {
    uint32_t tgid;
    uint32_t tid;
    uint32_t opcode;
};
#define SOCKET_PATH "/tmp/cxl_mem_simulator.sock"
#define OUTPUT_PMU_PATH "./output_pmu.csv"

#ifdef __cplusplus
}
#endif
#endif // CXLMEMSIM_SOCK_H
