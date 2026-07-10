/*********************************************************************
 *                     openNetVM
 *
 * Lightweight per-scope cycle profiling shared between NFs and manager.
 ********************************************************************/

#ifndef _ONVM_PROF_H_
#define _ONVM_PROF_H_

#include <stdint.h>

#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_lcore.h>

#include "onvm_common.h"

#define MZ_ONVM_PROF_STATS "MProc_onvm_prof_stats"
#define MZ_ONVM_LOOP_PROF_STATS "MProc_onvm_loop_prof_stats"
#define ONVM_PROF_LOG_FILE "/tmp/onvm_prof_stats.csv"
#define ONVM_LOOP_PROF_LOG_FILE "/tmp/onvm_loop_prof_stats.csv"
#define ONVM_PROF_MAX_SLOTS (RTE_MAX_LCORE + 1)
#define ONVM_PROF_FALLBACK_SLOT RTE_MAX_LCORE

enum onvm_prof_key {
        ONVM_PROF_UPFU_PACKET_HANDLER = 0,
        ONVM_PROF_KEY_COUNT
};

enum onvm_loop_prof_event {
        ONVM_LOOP_PROF_MAIN_LOOP = 0,
        ONVM_LOOP_PROF_DEQUEUE_PACKETS,
        ONVM_LOOP_PROF_PACKET_HANDLER,
        ONVM_LOOP_PROF_TIMEOUT_HANDLER,
        ONVM_LOOP_PROF_PROCESS_TX_BATCH,
        ONVM_LOOP_PROF_ENQUEUE_TX_THREAD,
        ONVM_LOOP_PROF_FLUSH_ALL_NFS,
        ONVM_LOOP_PROF_DEQUEUE_MESSAGES,
        ONVM_LOOP_PROF_USER_ACTIONS,
        ONVM_LOOP_PROF_SHARED_CORE_WAIT,
        ONVM_LOOP_PROF_EVENT_COUNT
};

struct onvm_prof_counter {
        volatile uint64_t calls;
        volatile uint64_t cycles_total;
        volatile uint64_t cycles_min;
        volatile uint64_t cycles_max;
} __rte_cache_aligned;

struct onvm_prof_mem {
        struct onvm_prof_counter counters[ONVM_PROF_MAX_SLOTS][ONVM_PROF_KEY_COUNT];
};

struct onvm_loop_prof_counter {
        volatile uint64_t calls;
        volatile uint64_t cycles_total;
} __rte_cache_aligned;

struct onvm_loop_prof_nf {
        volatile uint8_t in_use;
        uint16_t instance_id;
        uint16_t service_id;
        uint16_t core;
        char tag[TAG_SIZE];
        struct onvm_loop_prof_counter counters[ONVM_LOOP_PROF_EVENT_COUNT];
} __rte_cache_aligned;

struct onvm_loop_prof_mem {
        struct onvm_loop_prof_nf nfs[MAX_NFS];
};

struct onvm_prof_scope {
        enum onvm_prof_key key;
        uint64_t start_cycles;
};

extern struct onvm_prof_mem *onvm_prof_stats;
extern struct onvm_loop_prof_mem *onvm_loop_prof_stats;
extern __thread unsigned onvm_prof_slot_id;

int
onvm_prof_init_mgr(void);

int
onvm_loop_prof_init_mgr(void);

int
onvm_prof_init_nf(void);

int
onvm_loop_prof_init_nf(void);

void
onvm_prof_log_periodic(void);

void
onvm_loop_prof_log_periodic(void);

void
onvm_prof_cleanup(void);

void
onvm_loop_prof_cleanup(void);

void
onvm_prof_set_slot(unsigned slot_id);

void
onvm_loop_prof_register_nf(struct onvm_nf *nf);

static inline void
onvm_prof_record(enum onvm_prof_key key, uint64_t cycles) {
        unsigned slot_id;
        struct onvm_prof_counter *counter;

        if (unlikely(onvm_prof_stats == NULL || key >= ONVM_PROF_KEY_COUNT))
                return;

        slot_id = onvm_prof_slot_id;
        if (unlikely(slot_id >= ONVM_PROF_MAX_SLOTS)) {
                slot_id = rte_lcore_id();
                if (unlikely(slot_id >= ONVM_PROF_MAX_SLOTS))
                        slot_id = ONVM_PROF_FALLBACK_SLOT;
        }

        counter = &onvm_prof_stats->counters[slot_id][key];
        counter->calls++;
        counter->cycles_total += cycles;

        if (counter->cycles_min == 0 || cycles < counter->cycles_min)
                counter->cycles_min = cycles;
        if (cycles > counter->cycles_max)
                counter->cycles_max = cycles;
}

static inline void
onvm_loop_prof_record(struct onvm_nf *nf, enum onvm_loop_prof_event event, uint64_t cycles) {
        struct onvm_loop_prof_counter *counter;

        if (unlikely(onvm_loop_prof_stats == NULL || nf == NULL || event >= ONVM_LOOP_PROF_EVENT_COUNT ||
                     nf->instance_id >= MAX_NFS))
                return;

        counter = &onvm_loop_prof_stats->nfs[nf->instance_id].counters[event];
        counter->calls++;
        counter->cycles_total += cycles;
}

static inline struct onvm_prof_scope
onvm_prof_scope_begin(enum onvm_prof_key key) {
        struct onvm_prof_scope scope;

        scope.key = key;
        scope.start_cycles = rte_get_tsc_cycles();
        return scope;
}

static inline void
onvm_prof_scope_end(struct onvm_prof_scope *scope) {
        uint64_t end_cycles;

        if (unlikely(scope == NULL))
                return;

        end_cycles = rte_get_tsc_cycles();
        onvm_prof_record(scope->key, end_cycles - scope->start_cycles);
}

#define ONVM_PROF_CONCAT2(a, b) a##b
#define ONVM_PROF_CONCAT(a, b) ONVM_PROF_CONCAT2(a, b)

#define ONVM_PROFILE_SCOPE(key)                                                            \
        struct onvm_prof_scope ONVM_PROF_CONCAT(__onvm_prof_scope_, __LINE__)              \
            __attribute__((cleanup(onvm_prof_scope_end))) = onvm_prof_scope_begin(key)

#define ONVM_LOOP_PROF_BEGIN() rte_get_tsc_cycles()
#define ONVM_LOOP_PROF_END(nf, event, start_cycles)                                        \
        onvm_loop_prof_record((nf), (event), rte_get_tsc_cycles() - (start_cycles))

#endif  // _ONVM_PROF_H_
