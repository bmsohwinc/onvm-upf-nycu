/*********************************************************************
 *                     openNetVM
 *
 * Shared profiler setup and manager-side periodic logging.
 ********************************************************************/

#include "onvm_prof.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <rte_eal.h>
#include <rte_memzone.h>

struct onvm_prof_mem *onvm_prof_stats;
__thread unsigned onvm_prof_slot_id = ONVM_PROF_MAX_SLOTS;

static FILE *onvm_prof_log_fp;
static struct onvm_prof_counter onvm_prof_last[ONVM_PROF_MAX_SLOTS][ONVM_PROF_KEY_COUNT];

static const char *const onvm_prof_key_names[ONVM_PROF_KEY_COUNT] = {
        [ONVM_PROF_UPFU_PACKET_HANDLER] = "upf_u.packet_handler",
};

static inline uint64_t
onvm_prof_cycles_to_ns(uint64_t cycles) {
        return (uint64_t)((__uint128_t)cycles * 1000000000ULL / rte_get_tsc_hz());
}

int
onvm_prof_init_mgr(void) {
        const struct rte_memzone *mz;

        mz = rte_memzone_reserve(MZ_ONVM_PROF_STATS, sizeof(struct onvm_prof_mem), rte_socket_id(), 0);
        if (mz == NULL)
                return -1;

        memset(mz->addr, 0, sizeof(struct onvm_prof_mem));
        memset(onvm_prof_last, 0, sizeof(onvm_prof_last));
        onvm_prof_stats = mz->addr;
        return 0;
}

int
onvm_prof_init_nf(void) {
        const struct rte_memzone *mz;

        mz = rte_memzone_lookup(MZ_ONVM_PROF_STATS);
        if (mz == NULL)
                return -1;

        onvm_prof_stats = mz->addr;
        return 0;
}

void
onvm_prof_set_slot(unsigned slot_id) {
        if (slot_id < ONVM_PROF_MAX_SLOTS)
                onvm_prof_slot_id = slot_id;
        else
                onvm_prof_slot_id = ONVM_PROF_FALLBACK_SLOT;
}

void
onvm_prof_log_periodic(void) {
        time_t now;
        char time_buf[32];
        struct tm *tm_info;
        unsigned slot_id;
        int key;

        if (onvm_prof_stats == NULL)
                return;

        if (onvm_prof_log_fp == NULL) {
                onvm_prof_log_fp = fopen(ONVM_PROF_LOG_FILE, "w");
                if (onvm_prof_log_fp == NULL)
                        return;

                fprintf(onvm_prof_log_fp,
                        "timestamp,slot,key,calls_delta,cycles_delta,avg_cycles_delta,"
                        "avg_ns_delta,calls_total,cycles_total,min_cycles_total,"
                        "max_cycles_total,min_ns_total,max_ns_total\n");
        }

        now = time(NULL);
        tm_info = localtime(&now);
        if (tm_info == NULL || strftime(time_buf, sizeof(time_buf), "%F %T", tm_info) == 0)
                snprintf(time_buf, sizeof(time_buf), "unknown");

        for (slot_id = 0; slot_id < ONVM_PROF_MAX_SLOTS; slot_id++) {
                for (key = 0; key < ONVM_PROF_KEY_COUNT; key++) {
                        struct onvm_prof_counter cur;
                        struct onvm_prof_counter *last;
                        uint64_t calls_delta;
                        uint64_t cycles_delta;
                        uint64_t avg_cycles_delta;

                        cur.calls = onvm_prof_stats->counters[slot_id][key].calls;
                        cur.cycles_total = onvm_prof_stats->counters[slot_id][key].cycles_total;
                        cur.cycles_min = onvm_prof_stats->counters[slot_id][key].cycles_min;
                        cur.cycles_max = onvm_prof_stats->counters[slot_id][key].cycles_max;

                        last = &onvm_prof_last[slot_id][key];
                        calls_delta = cur.calls - last->calls;
                        cycles_delta = cur.cycles_total - last->cycles_total;
                        if (calls_delta == 0)
                                continue;

                        avg_cycles_delta = cycles_delta / calls_delta;
                        fprintf(onvm_prof_log_fp,
                                "%s,%u,%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                                ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                                ",%" PRIu64 "\n",
                                time_buf, slot_id, onvm_prof_key_names[key], calls_delta, cycles_delta,
                                avg_cycles_delta, onvm_prof_cycles_to_ns(avg_cycles_delta), cur.calls,
                                cur.cycles_total, cur.cycles_min, cur.cycles_max,
                                onvm_prof_cycles_to_ns(cur.cycles_min), onvm_prof_cycles_to_ns(cur.cycles_max));

                        *last = cur;
                }
        }

        fflush(onvm_prof_log_fp);
}

void
onvm_prof_cleanup(void) {
        if (onvm_prof_log_fp != NULL) {
                fclose(onvm_prof_log_fp);
                onvm_prof_log_fp = NULL;
        }
}
