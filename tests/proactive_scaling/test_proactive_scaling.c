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
static int starting, control_ready, control_error, spawn_calls;
static uint64_t now, last_queue_sample;
static uint32_t overloaded_samples;
struct Pending { uint32_t transaction_id, xact_index; };

static uint64_t rte_get_timer_cycles(void) { return now; }
static uint64_t rte_get_timer_hz(void) { return 1000; }
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
}
#define UTLT_Info log_message
#define UTLT_Warning log_message
static int start_worker(void) {
    assert(starting < 0 && !registry.nf_lock);
    spawn_calls++;
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
    context.scaling = (UpfScalingConfig){.slot_count = 3, .min_workers = 1, .max_workers = 3,
        .rx_queue_threshold = 40, .queue_sample_interval_ms = 10, .queue_consecutive_samples = 3};
    starting = -1;
    control_ready = 1;
    control_error = spawn_calls = overloaded_samples = 0;
    now = last_queue_sample = 0;
    for (int i = 0; i < 2; i++) {
        context.scaling.slots[i].service_id = 14 + i;
        registry.runtime[i].state = UPF_WORKER_READY;
        registry.runtime[i].instance_id = i + 1;
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
    assert(!overloaded_samples);
    now = 10;
    sample_load();
    assert(overloaded_samples == 1);
    sample_load(); /* Event-loop iterations are not sample rounds. */
    assert(overloaded_samples == 1);
    tick();
    assert(overloaded_samples == 2 && !spawn_calls);
    tick();
    assert(spawn_calls == 1 && starting == 2 && !overloaded_samples);
    for (int i = 0; i < 5; i++) tick();
    assert(spawn_calls == 1); /* No overlapping startup. */
    starting = -1;
    registry.runtime[2].state = UPF_WORKER_READY;
    for (int i = 0; i < 5; i++) tick();
    assert(spawn_calls == 1); /* At max_workers. */

    setup();
    tick(); tick();
    rings[1].count = 40; /* Equality resets the all-workers streak. */
    tick();
    assert(!overloaded_samples && !spawn_calls);
    rings[1].count = 41;
    tick(); tick();
    assert(!spawn_calls);
    tick();
    assert(spawn_calls == 1);

    setup();
    context.scaling.queue_sample_interval_ms = 20;
    context.scaling.queue_consecutive_samples = 2;
    tick();
    assert(!spawn_calls);
    tick();
    assert(spawn_calls == 1);

    setup();
    tick();
    registry.nf_lock = 1;
    tick();
    assert(!overloaded_samples && !spawn_calls); /* An unread sample breaks the streak. */
    registry.nf_lock = 0;
    control_ready = 0;
    for (int i = 0; i < 3; i++) tick();
    assert(!spawn_calls);

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
    puts("PASS: periodic scale-out, streak reset, startup/worker limits and READY-only admission");
    return 0;
}
