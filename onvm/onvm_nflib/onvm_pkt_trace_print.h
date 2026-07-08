/*********************************************************************
 *                     openNetVM
 *
 * Direct packet timestamp debug prints for low-rate latency tracing.
 ********************************************************************/

#ifndef _ONVM_PKT_TRACE_PRINT_H_
#define _ONVM_PKT_TRACE_PRINT_H_

#include <inttypes.h>
#include <stdio.h>

#include <rte_cycles.h>
#include <rte_lcore.h>

#ifndef ONVM_PKT_TRACE_PRINT
#define ONVM_PKT_TRACE_PRINT 1
#endif

static inline uint64_t
onvm_pkt_trace_print_cycles_to_ns(uint64_t cycles) {
        return (uint64_t)((__uint128_t)cycles * 1000000000ULL / rte_get_tsc_hz());
}

#if ONVM_PKT_TRACE_PRINT
#define ONVM_PKT_TS(stage_name, pkt_ptr)                                                        \
        do {                                                                                     \
                uint64_t __onvm_pkt_ts_tsc = rte_get_tsc_cycles();                               \
                fprintf(stderr, "ONVM_PKT_TS,%s,%p,%" PRIu64 ",%" PRIu64 ",%u\n",               \
                        (stage_name), (void *)(pkt_ptr), __onvm_pkt_ts_tsc,                      \
                        onvm_pkt_trace_print_cycles_to_ns(__onvm_pkt_ts_tsc), rte_lcore_id());   \
        } while (0)
#else
#define ONVM_PKT_TS(stage_name, pkt_ptr) do { (void)(pkt_ptr); } while (0)
#endif

#endif  // _ONVM_PKT_TRACE_PRINT_H_
