/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include "upf_worker.h"

#define MAX_NFS 8
struct rte_ring { unsigned count; } rings[3];
struct onvm_nf { int valid; uint16_t service_id; struct rte_ring *rx_q; } nfs[MAX_NFS];
static struct { UpfScalingConfig scaling; } context;
#define Self() (&context)
static UpfWorkerRegistry registry;
UpfWorkerRegistry *g_upf_workers = &registry;
static pid_t children[UPF_MAX_WORKERS];
static int starting, control_ready, control_error, spawn_calls, spawn_error;
static uint64_t now, last_queue_sample;
static uint32_t session_count[UPF_MAX_WORKERS], shutdown_sequence;
static int stopping = -1, shutdown_stage, down_candidate = -1;
static uint64_t shutdown_started, shutdown_deadline, down_since;
#define STOP_N3 0
#define MAX_PENDING 4
static char trigger_log[512];
struct Pending { uint32_t transaction_id, xact_index; int deleting, responded; void *message; };
static struct Pending requests[MAX_PENDING];

static uint64_t rte_get_timer_cycles(void) { return now; }
static uint64_t rte_get_timer_hz(void) { return 1000; }
static uint64_t after_ms(uint32_t ms) { return now + ms; }
static void state(uint16_t slot, UpfWorkerState value) { registry.runtime[slot].state = value; }
static int onvm_nf_is_valid(const struct onvm_nf *nf) { return nf->valid; }
static unsigned rte_ring_count(const struct rte_ring *ring) {
    assert(registry.nf_lock);
    return ring->count;
}
static unsigned rte_ring_get_capacity(const struct rte_ring *ring) { (void)ring; return 4095; }
static void log_message(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void log_message(const char *format, ...) {
    assert(!registry.nf_lock);
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (!strncmp(text, "Scale-out trigger:", 18)) strcpy(trigger_log, text);
}
#define UTLT_Info log_message
#define UTLT_Warning log_message
static int start_worker(void) {
    assert(starting < 0 && !registry.nf_lock);
    spawn_calls++;
    if (spawn_error) return spawn_error;
    starting = 2;
    registry.runtime[2].state = UPF_WORKER_STARTING;
    children[2] = 102;
    return 2;
}

#include "policy.inc"

static void setup(void) {
    memset(&registry, 0, sizeof(registry));
    memset(children, 0, sizeof(children));
    memset(nfs, 0, sizeof(nfs));
    memset(session_count, 0, sizeof(session_count));
    memset(requests, 0, sizeof(requests));
    context.scaling = (UpfScalingConfig){.slot_count = 3, .min_workers = 1, .max_workers = 3,
        .rx_queue_threshold = 40, .queue_sample_interval_ms = 10, .queue_window_samples = 3,
        .scale_down_queue_threshold = 10, .scale_down_hold_ms = 30000, .worker_stop_timeout_ms = 5000};
    stopping = down_candidate = -1;
    starting = -1;
    control_ready = 1;
    control_error = spawn_calls = spawn_error = 0;
    trigger_log[0] = '\0';
    reset_queue_windows();
    now = last_queue_sample = 0;
    for (int i = 0; i < 2; i++) {
        context.scaling.slots[i].service_id = 14 + i;
        registry.runtime[i].state = UPF_WORKER_READY;
        registry.runtime[i].instance_id = i + 1;
        registry.runtime[i].generation = 1;
        rings[i].count = 41 + i;
        nfs[i + 1] = (struct onvm_nf){.valid = 1, .service_id = 14 + i, .rx_q = &rings[i]};
        children[i] = 100 + i;
    }
}
static void tick(void) {
    now += context.scaling.queue_sample_interval_ms;
    sample_load();
}

int main(void) {
    setup();
    now = 9;
    sample_load();
    assert(!queue_samples);
    now = 10;
    sample_load();
    assert(queue_samples == 1);
    sample_load(); /* Event-loop iterations are not sample rounds. */
    assert(queue_samples == 1);
    tick();
    assert(queue_samples == 2 && !spawn_calls);
    tick();
    assert(spawn_calls == 1 && starting == 2 && !queue_samples);
    assert(strstr(trigger_log, "min_rx_avg=41.000") && strstr(trigger_log, "window_samples=3"));
    for (int i = 0; i < 5; i++) tick();
    assert(spawn_calls == 1); /* No overlapping startup. */
    starting = -1;
    context.scaling.slots[2].service_id = 16;
    registry.runtime[2].state = UPF_WORKER_READY;
    registry.runtime[2].instance_id = 3;
    registry.runtime[2].generation = 1;
    nfs[3] = (struct onvm_nf){.valid = 1, .service_id = 16, .rx_q = &rings[2]};
    rings[2].count = 0;
    for (int i = 0; i < 5; i++) tick();
    assert(spawn_calls == 1); /* At max_workers. */
    assert(queue_samples == 3); /* Sampling continues at the process limit. */

    /* A low observation remains in the average rather than resetting it. */
    setup();
    rings[0].count = rings[1].count = 80;
    tick();
    rings[0].count = rings[1].count = 0;
    tick();
    assert(!spawn_calls);
    rings[0].count = rings[1].count = 80;
    tick();
    assert(spawn_calls == 1);

    /* Average each worker over time, not the round's minimum or fleet mean. */
    setup();
    rings[0].count = 100; rings[1].count = 0;
    tick();
    rings[0].count = 0; rings[1].count = 100;
    tick();
    rings[0].count = rings[1].count = 100;
    tick();
    assert(spawn_calls == 1); /* Each average is 66.67; average of minima is 33.33. */

    setup();
    rings[0].count = 1000; rings[1].count = 0;
    for (int i = 0; i < 8; i++) tick();
    assert(!spawn_calls); /* A hot worker must not hide an idle one. */

    /* Equality does not trigger; fractional averages above 40 do. */
    setup();
    rings[0].count = rings[1].count = 40;
    tick(); tick(); tick();
    assert(!spawn_calls);
    rings[0].count = rings[1].count = 41;
    tick();
    assert(spawn_calls == 1);
    assert(strstr(trigger_log, "min_rx_avg=40.333"));

    /* Only the last W observations count, across repeated cursor wraps. */
    setup();
    rings[0].count = rings[1].count = 90;
    tick();
    rings[0].count = rings[1].count = 0;
    for (int i = 0; i < 7; i++) tick();
    rings[0].count = rings[1].count = 50;
    tick(); tick();
    assert(!spawn_calls); /* [0, 50, 50] */
    tick();
    assert(spawn_calls == 1); /* [50, 50, 50] */

    /* A short sufficiently large burst can exceed the average threshold. */
    setup();
    rings[0].count = rings[1].count = 0;
    tick(); tick();
    rings[0].count = rings[1].count = 150;
    tick();
    assert(spawn_calls == 1);

    setup();
    context.scaling.queue_sample_interval_ms = 20;
    context.scaling.queue_window_samples = 2;
    now = 19;
    sample_load();
    assert(!queue_samples);
    tick();
    assert(!spawn_calls);
    tick();
    assert(spawn_calls == 1);

    setup();
    context.scaling.queue_window_samples = 1;
    tick();
    assert(spawn_calls == 1);

    setup();
    context.scaling.queue_window_samples = UPF_MAX_QUEUE_WINDOW_SAMPLES;
    rings[0].count = rings[1].count = 40;
    for (unsigned i = 0; i < UPF_MAX_QUEUE_WINDOW_SAMPLES + 5; i++) tick();
    assert(!spawn_calls);
    rings[0].count = rings[1].count = 41;
    tick();
    assert(spawn_calls == 1); /* Also exercises the maximum cursor bound. */

    /* A missing read or a full missed interval invalidates earlier samples. */
    setup();
    tick(); tick();
    registry.nf_lock = 1;
    tick();
    assert(!queue_samples && !spawn_calls);
    registry.nf_lock = 0;
    tick(); tick();
    assert(!spawn_calls);
    tick();
    assert(spawn_calls == 1);

    setup();
    tick(); tick();
    now += context.scaling.queue_sample_interval_ms; /* Skip one scheduled read. */
    tick();
    assert(queue_samples == 1 && !spawn_calls);
    tick();
    assert(!spawn_calls);
    tick();
    assert(spawn_calls == 1);

    /* An invalid READY worker must not silently disappear from the decision. */
    for (int invalid = 0; invalid < 5; invalid++) {
        setup();
        tick(); tick();
        switch (invalid) {
        case 0: nfs[2].valid = 0; break;
        case 1: nfs[2].rx_q = NULL; break;
        case 2: nfs[2].service_id = 99; break;
        case 3: registry.runtime[1].instance_id = 0; break;
        case 4: registry.runtime[1].instance_id = MAX_NFS; break;
        }
        tick();
        assert(!spawn_calls && !queue_samples);
        nfs[2] = (struct onvm_nf){.valid = 1, .service_id = 15, .rx_q = &rings[1]};
        registry.runtime[1].instance_id = 2;
        tick(); tick();
        assert(!spawn_calls);
        tick();
        assert(spawn_calls == 1);
    }

    /* Membership, generation and instance changes each start a fresh window. */
    for (int change = 0; change < 3; change++) {
        setup();
        tick(); tick();
        switch (change) {
        case 0: registry.runtime[1].state = UPF_WORKER_FAILED; break;
        case 1: registry.runtime[1].generation++; break;
        case 2: nfs[3] = nfs[2]; registry.runtime[1].instance_id = 3; break;
        }
        tick(); tick();
        assert(!spawn_calls);
        tick();
        assert(spawn_calls == 1);
    }

    setup();
    control_ready = 0;
    for (int i = 0; i < 3; i++) tick();
    assert(!spawn_calls);
    control_ready = 1;
    tick(); tick();
    control_error = -EIO;
    tick();
    assert(!spawn_calls && !queue_samples);
    control_error = 0;
    tick(); tick();
    assert(!spawn_calls);
    tick();
    assert(spawn_calls == 1);

    setup();
    context.scaling.max_workers = 2;
    for (int i = 0; i < 5; i++) tick();
    assert(!spawn_calls && queue_samples == 3);

    setup();
    spawn_error = -EAGAIN;
    tick(); tick(); tick();
    assert(spawn_calls == 1);
    tick(); tick();
    assert(spawn_calls == 1); /* A failed spawn cannot retry on every sample. */
    spawn_error = 0;
    tick();
    assert(spawn_calls == 2);

    /* Sparse slot IDs, including the top mask bit, are valid. */
    setup();
    context.scaling.slot_count = UPF_MAX_WORKERS;
    context.scaling.slots[31].service_id = 45;
    registry.runtime[31] = registry.runtime[1];
    registry.runtime[1].state = UPF_WORKER_INACTIVE;
    children[31] = children[1]; children[1] = 0;
    nfs[2].service_id = 45;
    tick(); tick(); tick();
    assert(spawn_calls == 1);

    setup();
    struct Pending request = {.transaction_id = 1, .xact_index = 2};
    rings[0].count = 200;
    rings[1].count = 100;
    assert(select_worker(&request) == 1 && !spawn_calls); /* Both above threshold. */
    starting = 2;
    registry.runtime[2].state = UPF_WORKER_STARTING;
    assert(select_worker(&request) == 1 && !spawn_calls);
    rings[0].count = 0;
    assert(select_worker(&request) == 0);
    rings[1].count = 0;
    assert(select_worker(&request) == 0); /* Ties keep the first slot. */
    registry.runtime[0].state = registry.runtime[1].state = UPF_WORKER_INACTIVE;
    assert(select_worker(&request) == -EAGAIN && !spawn_calls);
    starting = -1;
    for (int i = 0; i < 3; i++) tick();
    assert(!spawn_calls); /* No READY queues is not overload. */
    puts("PASS: sliding queue averages, full/fresh windows, worker limits and READY-only admission");
    return 0;
}
