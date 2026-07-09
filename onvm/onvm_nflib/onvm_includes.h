/*********************************************************************
 *                     openNetVM
 *              https://sdnfv.github.io
 *
 *   BSD LICENSE
 *
 *   Copyright(c)
 *            2015-2019 George Washington University
 *            2015-2019 University of California Riverside
 *            2010-2019 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * The name of the author may not be used to endorse or promote
 *       products derived from this software without specific prior
 *       written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ********************************************************************/

/******************************************************************************

                               onvm_includes.h


         Header file containing all shared headers and data structures


******************************************************************************/

#ifndef _ONVM_INCLUDES_H_
#define _ONVM_INCLUDES_H_

/******************************Standard C library*****************************/

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <unistd.h>

/********************************DPDK library*********************************/

#include <rte_atomic.h>
#include <rte_branch_prediction.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_debug.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_interrupts.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_mbuf.h>
#include <rte_memory.h>
#include <rte_mempool.h>
#include <rte_memzone.h>
#include <rte_pci.h>
#include <rte_per_lcore.h>
#include <rte_ring.h>
#include <rte_string_fns.h>
#include <rte_tailq.h>

/******************************Internal headers*******************************/

#include "onvm_common.h"

/***********************************Macros************************************/

#ifndef ONVM_STARTUP_TRACE_PRINT
#define ONVM_STARTUP_TRACE_PRINT 1
#endif

static inline uint64_t
onvm_startup_trace_print_cycles_to_ns(uint64_t cycles) {
        const uint64_t hz = rte_get_tsc_hz();
        if (hz == 0)
                return 0;
        return (uint64_t)((__uint128_t)cycles * 1000000000ULL / hz);
}

#if ONVM_STARTUP_TRACE_PRINT
#define ONVM_STARTUP_TIMESTAMP(event, role) do { \
        uint64_t __onvm_startup_ts_tsc = rte_get_tsc_cycles(); \
        fprintf(stderr, "ONVM_STARTUP_TS,%s,%s,%" PRIu64 ",%" PRIu64 ",%u\n", \
                event, role, __onvm_startup_ts_tsc, \
                onvm_startup_trace_print_cycles_to_ns(__onvm_startup_ts_tsc), rte_lcore_id()); \
        fflush(stderr); \
} while (0)
#else
#define ONVM_STARTUP_TIMESTAMP(event, role) do { (void)(event); (void)(role); } while (0)
#endif

#endif  // _ONVM_INCLUDES_H_
