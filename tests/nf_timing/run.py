#!/usr/bin/env python3
"""Check actual timing/dequeue code with DPDK stubs, then exercise CSV reporting."""
import csv
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("nf_timing", ROOT / "scripts/nf_timing.py")
reporter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reporter)
source = (ROOT / "onvm/onvm_nflib/onvm_nflib.c").read_text()
dequeue = re.search(r"static inline uint16_t\nonvm_nflib_dequeue_packets\([^;]+?\) \{.*?\n}", source, re.S)
assert dequeue

HARNESS = r'''
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define unlikely(x) (x)
#define likely(x) (x)
#define PACKET_READ_SIZE 32
#define ONVM_NF_HANDLE_TX 1
struct onvm_pkt_meta { int action; };
struct rte_mbuf { int retained; struct onvm_pkt_meta meta; };
struct rte_ring { unsigned count; struct rte_mbuf *packets[32]; };
struct packet_buf { uint16_t count; struct rte_mbuf *buffer[32]; };
struct onvm_nf {
    const char *tag;
    uint16_t instance_id, service_id;
    struct { unsigned core; } thread_info;
    struct rte_ring *rx_q, *tx_q;
    struct { uint64_t rx, rx_drop, tx_drop, act_drop, tx_buffer; } stats;
};
struct onvm_nf_local_ctx { struct onvm_nf *nf; };
typedef int (*nf_pkt_handler_fn)(struct rte_mbuf *, struct onvm_pkt_meta *, struct onvm_nf_local_ctx *);
static unsigned rte_ring_count(const struct rte_ring *r) { return r->count; }
static unsigned rte_ring_dequeue_burst(struct rte_ring *r, void **p, unsigned max, void *unused) {
    (void)unused;
    unsigned count = r->count < max ? r->count : max;
    memcpy(p, r->packets, count * sizeof(*p)); r->count -= count; return count;
}
static struct onvm_pkt_meta *onvm_get_pkt_meta(struct rte_mbuf *p, int offset) {
    (void)offset; return &p->meta;
}
static void onvm_pkt_enqueue_tx_thread(struct packet_buf *buf, struct onvm_nf *nf) {
    (void)buf; (void)nf; assert(0); /* Not taken in the existing HANDLE_TX mode. */
}
#include "onvm_nflib_timing.h"
#include "dequeue.inc"
static int handler(struct rte_mbuf *p, struct onvm_pkt_meta *m, struct onvm_nf_local_ctx *ctx) {
    (void)ctx; m->action = 42; return p->retained;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    struct rte_ring rx = {0}, tx = {0};
    struct onvm_nf nf = {.tag="upf_u", .instance_id=2, .service_id=14, .rx_q=&rx, .tx_q=&tx};
    struct onvm_nf_local_ctx ctx = {.nf=&nf};
    struct nf_timing off = {0}, other = {0}, timing = {0};
    unsetenv("ONVM_NF_TIMING_DIR"); nf_timing_init(&off, &nf); assert(!off.file);
    setenv("ONVM_NF_TIMING_DIR", argv[1], 1);
    nf.tag = "upf_c"; nf_timing_init(&other, &nf); assert(!other.file);
    nf.tag = "upf_u"; nf_timing_init(&timing, &nf); assert(timing.file);
    struct rte_mbuf p[3] = {{.retained=1}, {0}, {0}};
    void *out[32];
    for (int enabled=0; enabled<2; enabled++) {
        rx.count=3;
        for (int i=0; i<3; i++) rx.packets[i]=&p[i];
        timing.sample=1;
        if (enabled) { timing.window.polls++; nf_timing_begin(&timing, &nf); }
        assert(onvm_nflib_dequeue_packets(out, &ctx, handler, 0, enabled ? &timing : NULL)==2);
        assert(out[0]==&p[1] && out[1]==&p[2] && rx.count==0);
        for (int i=0; i<3; i++) assert(p[i].meta.action==42);
    }
    assert(nf.stats.tx_buffer==2);
    assert(timing.window.packets==3 && timing.window.sampled_packets==3);
    assert(timing.window.samples==1 && timing.window.bursts==1);
    assert(timing.window.dequeue_ticks==100 && timing.window.handler_ticks==100);
    timing.window.polls++; nf_timing_begin(&timing, &nf);
    assert(onvm_nflib_dequeue_packets(out, &ctx, handler, 0, &timing)==0);
    assert(timing.last_dequeued==0 && timing.window.samples==1);
    nf_timing_report(&timing, &nf, rte_get_tsc_cycles());
    assert(timing.window.packets==0 && timing.file);
    fclose(timing.file);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="nf-timing-") as tmp:
    directory = Path(tmp)
    (directory / "rte_cycles.h").write_text(
        "static uint64_t ticks;\n"
        "static uint64_t rte_get_tsc_cycles(void) { return ticks += 100; }\n"
        "static uint64_t rte_get_tsc_hz(void) { return 1000; }\n")
    (directory / "dequeue.inc").write_text(dequeue.group())
    (directory / "test.c").write_text(HARNESS)
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I" + tmp,
                    "-I" + str(ROOT / "onvm/onvm_nflib"), str(directory / "test.c"),
                    "-o", str(directory / "test")], check=True)
    subprocess.run([str(directory / "test"), tmp], check=True)
    raw = next(directory.glob("nf-timing-*.csv"))
    row = reporter.read_csv(raw)[0]
    assert None not in row and all(v is not None for v in row.values())
    assert int(row["packets"]) == 3 and int(row["rx_max"]) == 3
    # Known phase boundaries: exclude a crossing row, weight counts by duration,
    # preserve missing/reset counter deltas, and ignore an incomplete last line.
    rows = []
    for end, packets, samples, counter in [(2, 30, 3, 100), (3, 60, 6, 160), (4, 0, 0, 0)]:
        item = dict(row, time_ns=str(end * 10**9), interval_s="1", packets=str(packets),
                    sampled_packets=str(samples), samples=str(bool(samples) * 1),
                    nf_rx_total=str(counter), handler_ns="300" if samples else "0")
        rows.append(item)
    reporter.write_csv(raw, rows)
    with raw.open("a") as stream:
        stream.write("5000000000,partial")
    reporter.write_csv(directory / "phases.csv", [
        {"time_ns": 1500000000, "phase": "CROSSING"},
        {"time_ns": 2000000000, "phase": "BOTH"},
        {"time_ns": 4000000000, "phase": "END"}])
    reporter.report(directory)
    intervals = reporter.read_csv(directory / "timing-intervals.csv")
    assert len(intervals) == 2 and intervals[0]["handler_ns_per_packet"] == "50.0"
    assert intervals[0]["nf_rx_delta"] == "60"
    assert intervals[1]["handler_ns_per_packet"] == "" and intervals[1]["nf_rx_delta"] == ""
    summary = reporter.read_csv(directory / "timing-summary.csv")[0]
    assert summary["seconds"] == "2.0" and summary["dequeue_kpps"] == "0.03"
    assert summary["handler_ns_per_packet"] == "50.0" and summary["nf_rx_delta"] == ""
print("NF timing checks passed (mock DPDK; hardware overhead requires testbed measurement).")
