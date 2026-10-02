/* SPDX-License-Identifier: BSD-3-Clause
 * Optional, thread-local UPF-U diagnostics. No shared dataplane layout changes.
 * Include after onvm_nflib.h. Prefer a tmpfs output directory to limit I/O stalls.
 */
#ifndef ONVM_NFLIB_TIMING_H
#define ONVM_NFLIB_TIMING_H

#include <time.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <rte_cycles.h>

#define NF_TIMING_SAMPLE_MASK 1023u

struct nf_timing_window {
        uint64_t polls, packets, bursts, full_bursts;
        uint64_t queue_samples, rx_sum, tx_sum;
        unsigned rx_max, tx_max;
        uint64_t samples, sampled_packets;
        uint64_t dequeue_ticks, handler_ticks, tx_ticks, callback_ticks;
        uint64_t handler_max_ticks;
};

struct nf_timing {
        FILE *file;
        uint64_t hz, window_start, next_report;
        uint64_t begin, dequeued, handled;
        uint64_t previous_write_ticks;
        unsigned sequence;
        int sample;
        uint16_t last_dequeued;
        long tid;
        struct nf_timing_window window;
};

static void
nf_timing_init(struct nf_timing *t, const struct onvm_nf *nf) {
        const char *dir = getenv("ONVM_NF_TIMING_DIR");
        if (!dir || !*dir || !nf->tag || strcmp(nf->tag, "upf_u")) return;
        char path[4096];
        int n = snprintf(path, sizeof(path), "%s/nf-timing-%ld-%u.csv",
                         dir, (long)getpid(), nf->instance_id);
        if (n < 0 || (size_t)n >= sizeof(path)) {
                fprintf(stderr, "NF timing disabled: output path too long\n");
                return;
        }
        t->file = fopen(path, "wx");
        if (!t->file) {
                fprintf(stderr, "NF timing disabled: %s: %s\n", path, strerror(errno));
                return;
        }
#ifdef __linux__
        t->tid = syscall(SYS_gettid);
#else
        t->tid = (long)getpid();
#endif
        fputs("time_ns,pid,tid,instance,service,core,interval_s,polls,packets,bursts,full_bursts,"
              "queue_samples,rx_sum,rx_max,tx_sum,tx_max,rx_now,tx_now,"
              "samples,sampled_packets,dequeue_ns,handler_ns,tx_ns,callback_ns,handler_max_ns,"
              "nf_rx_total,nf_rx_drop_total,nf_tx_drop_total,nf_action_drop_total,previous_write_ns\n", t->file);
        if (fflush(t->file) || ferror(t->file)) {
                fprintf(stderr, "NF timing disabled: cannot write %s\n", path);
                fclose(t->file);
                t->file = NULL;
                return;
        }
        t->hz = rte_get_tsc_hz();
        t->window_start = rte_get_tsc_cycles();
        t->next_report = t->window_start + t->hz;
        fprintf(stderr, "NF timing: %s (tid=%ld, 1/1024 loops, ~1 s rows)\n", path, t->tid);
}

/* Called only for sampled loops; queue observations include idle loops. */
static inline void
nf_timing_begin(struct nf_timing *t, const struct onvm_nf *nf) {
        struct nf_timing_window *w = &t->window;
        unsigned rx = rte_ring_count(nf->rx_q), tx = rte_ring_count(nf->tx_q);
        w->queue_samples++;
        w->rx_sum += rx;
        w->tx_sum += tx;
        if (rx > w->rx_max) w->rx_max = rx;
        if (tx > w->tx_max) w->tx_max = tx;
        t->begin = rte_get_tsc_cycles();
}

static void
nf_timing_report(struct nf_timing *t, const struct onvm_nf *nf, uint64_t now) {
        if (now <= t->window_start) return;
        struct timespec wall;
        if (clock_gettime(CLOCK_REALTIME, &wall)) memset(&wall, 0, sizeof(wall));
        uint64_t time_ns = (uint64_t)wall.tv_sec * 1000000000ULL + wall.tv_nsec;
        struct nf_timing_window *w = &t->window;
        double ns = 1e9 / t->hz;
        uint64_t write_start = rte_get_tsc_cycles();
        fprintf(t->file,
                "%" PRIu64 ",%ld,%ld,%u,%u,%u,%.9f,"
                "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
                "%" PRIu64 ",%" PRIu64 ",%u,%" PRIu64 ",%u,%u,%u,"
                "%" PRIu64 ",%" PRIu64 ",%.0f,%.0f,%.0f,%.0f,%.0f,"
                "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.0f\n",
                time_ns, (long)getpid(), t->tid, nf->instance_id, nf->service_id,
                nf->thread_info.core, (double)(now - t->window_start) / t->hz,
                w->polls, w->packets, w->bursts, w->full_bursts,
                w->queue_samples, w->rx_sum, w->rx_max, w->tx_sum, w->tx_max,
                rte_ring_count(nf->rx_q), rte_ring_count(nf->tx_q),
                w->samples, w->sampled_packets, w->dequeue_ticks * ns,
                w->handler_ticks * ns, w->tx_ticks * ns, w->callback_ticks * ns,
                w->handler_max_ticks * ns, nf->stats.rx, nf->stats.rx_drop,
                nf->stats.tx_drop, nf->stats.act_drop, t->previous_write_ticks * ns);
        int rc = fflush(t->file);
        t->previous_write_ticks = rte_get_tsc_cycles() - write_start;
        if (rc || ferror(t->file)) {
                fprintf(stderr, "NF timing disabled: CSV write failed\n");
                fclose(t->file);
                t->file = NULL;
        }
        memset(w, 0, sizeof(*w));
        /* Include this write's pause in the next interval's packet rate. */
        t->window_start = now;
        t->next_report = now + t->hz;
}

#endif
