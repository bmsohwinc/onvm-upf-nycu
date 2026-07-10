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
struct onvm_loop_prof_mem *onvm_loop_prof_stats;
__thread unsigned onvm_prof_slot_id = ONVM_PROF_MAX_SLOTS;

static FILE *onvm_prof_log_fp;
static FILE *onvm_loop_prof_log_fp;
static struct onvm_prof_counter onvm_prof_last[ONVM_PROF_MAX_SLOTS][ONVM_PROF_KEY_COUNT];
static struct onvm_loop_prof_counter onvm_loop_prof_last[MAX_NFS][ONVM_LOOP_PROF_EVENT_COUNT];

static const char *const onvm_prof_key_names[ONVM_PROF_KEY_COUNT] = {
        [ONVM_PROF_UPFU_PACKET_HANDLER] = "upf_u.packet_handler",
};

static const char *const onvm_loop_prof_event_names[ONVM_LOOP_PROF_EVENT_COUNT] = {
        [ONVM_LOOP_PROF_MAIN_LOOP] = "main_loop",
        [ONVM_LOOP_PROF_DEQUEUE_PACKETS] = "onvm_nflib_dequeue_packets",
        [ONVM_LOOP_PROF_PACKET_HANDLER] = "packet_handler",
        [ONVM_LOOP_PROF_TIMEOUT_HANDLER] = "timeout_packet_handler",
        [ONVM_LOOP_PROF_PROCESS_TX_BATCH] = "onvm_pkt_process_tx_batch",
        [ONVM_LOOP_PROF_ENQUEUE_TX_THREAD] = "onvm_pkt_enqueue_tx_thread",
        [ONVM_LOOP_PROF_FLUSH_ALL_NFS] = "onvm_pkt_flush_all_nfs",
        [ONVM_LOOP_PROF_DEQUEUE_MESSAGES] = "onvm_nflib_dequeue_messages",
        [ONVM_LOOP_PROF_USER_ACTIONS] = "user_actions",
        [ONVM_LOOP_PROF_SHARED_CORE_WAIT] = "shared_core_wait",
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
onvm_loop_prof_init_mgr(void) {
        const struct rte_memzone *mz;

        mz = rte_memzone_reserve(MZ_ONVM_LOOP_PROF_STATS, sizeof(struct onvm_loop_prof_mem), rte_socket_id(), 0);
        if (mz == NULL)
                return -1;

        memset(mz->addr, 0, sizeof(struct onvm_loop_prof_mem));
        memset(onvm_loop_prof_last, 0, sizeof(onvm_loop_prof_last));
        onvm_loop_prof_stats = mz->addr;
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

int
onvm_loop_prof_init_nf(void) {
        const struct rte_memzone *mz;

        mz = rte_memzone_lookup(MZ_ONVM_LOOP_PROF_STATS);
        if (mz == NULL)
                return -1;

        onvm_loop_prof_stats = mz->addr;
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
onvm_loop_prof_register_nf(struct onvm_nf *nf) {
        struct onvm_loop_prof_nf *entry;

        if (onvm_loop_prof_stats == NULL || nf == NULL || nf->instance_id >= MAX_NFS)
                return;

        entry = &onvm_loop_prof_stats->nfs[nf->instance_id];
        memset(entry, 0, sizeof(*entry));
        entry->instance_id = nf->instance_id;
        entry->service_id = nf->service_id;
        entry->core = nf->thread_info.core;
        if (nf->tag != NULL) {
                strncpy(entry->tag, nf->tag, sizeof(entry->tag));
                entry->tag[sizeof(entry->tag) - 1] = '\0';
        }
        entry->in_use = 1;
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
onvm_loop_prof_log_periodic(void) {
        time_t now;
        char time_buf[32];
        struct tm *tm_info;
        uint16_t nf_id;
        int event;

        if (onvm_loop_prof_stats == NULL)
                return;

        if (onvm_loop_prof_log_fp == NULL) {
                onvm_loop_prof_log_fp = fopen(ONVM_LOOP_PROF_LOG_FILE, "w");
                if (onvm_loop_prof_log_fp == NULL)
                        return;

                fprintf(onvm_loop_prof_log_fp,
                        "timestamp,nf_tag,event_name,cycles,calls,avg_cycles,instance_id,"
                        "service_id,core,cycles_total,calls_total\n");
        }

        now = time(NULL);
        tm_info = localtime(&now);
        if (tm_info == NULL || strftime(time_buf, sizeof(time_buf), "%F %T", tm_info) == 0)
                snprintf(time_buf, sizeof(time_buf), "unknown");

        for (nf_id = 0; nf_id < MAX_NFS; nf_id++) {
                struct onvm_loop_prof_nf *nf_entry;
                const char *tag;

                nf_entry = &onvm_loop_prof_stats->nfs[nf_id];
                if (nf_entry->in_use == 0)
                        continue;

                tag = nf_entry->tag[0] != '\0' ? nf_entry->tag : "unknown";
                for (event = 0; event < ONVM_LOOP_PROF_EVENT_COUNT; event++) {
                        struct onvm_loop_prof_counter cur;
                        struct onvm_loop_prof_counter *last;
                        uint64_t calls_delta;
                        uint64_t cycles_delta;
                        uint64_t avg_cycles;

                        cur.calls = nf_entry->counters[event].calls;
                        cur.cycles_total = nf_entry->counters[event].cycles_total;

                        last = &onvm_loop_prof_last[nf_id][event];
                        calls_delta = cur.calls >= last->calls ? cur.calls - last->calls : cur.calls;
                        cycles_delta = cur.cycles_total >= last->cycles_total ? cur.cycles_total - last->cycles_total :
                                                                                cur.cycles_total;
                        if (calls_delta == 0)
                                continue;

                        avg_cycles = cycles_delta / calls_delta;
                        fprintf(onvm_loop_prof_log_fp,
                                "%s,%s,%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu16
                                ",%" PRIu16 ",%" PRIu16 ",%" PRIu64 ",%" PRIu64 "\n",
                                time_buf, tag, onvm_loop_prof_event_names[event], cycles_delta,
                                calls_delta, avg_cycles, nf_entry->instance_id, nf_entry->service_id,
                                nf_entry->core, cur.cycles_total, cur.calls);

                        *last = cur;
                }
        }

        fflush(onvm_loop_prof_log_fp);
}

void
onvm_prof_cleanup(void) {
        if (onvm_prof_log_fp != NULL) {
                fclose(onvm_prof_log_fp);
                onvm_prof_log_fp = NULL;
        }
}

void
onvm_loop_prof_cleanup(void) {
        if (onvm_loop_prof_log_fp != NULL) {
                fclose(onvm_loop_prof_log_fp);
                onvm_loop_prof_log_fp = NULL;
        }
}
