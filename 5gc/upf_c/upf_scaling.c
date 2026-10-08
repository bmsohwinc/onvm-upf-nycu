/* SPDX-License-Identifier: Apache-2.0
 * One UPF-C event loop owns session and worker lifecycles.
 * Queue averages drive scaling; arrivals only select READY workers.
 */
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <rte_eal.h>
#include <rte_memzone.h>
#include <rte_cycles.h>
#include "upf_scaling.h"
#include "upf_context.h"
#include "upf_cls_ctrl.h"
#include "onvm_sc_common.h"
#include "n4_dispatcher.h"
#include "n4_onvm_pfcp_handler.h"

#define MAX_PENDING 64
extern char **environ;
static const struct core_status *manager_cores;
static const uint16_t *service_counts;
static pid_t children[UPF_MAX_WORKERS];
static uint64_t stop_deadline[UPF_MAX_WORKERS];
enum { START_WAIT_REGISTRATION, START_WAIT_CLASSIFIER, START_WAIT_N3_ACK,
       START_ENABLE_POLLING, START_WAIT_POLL_ACK };
static int starting = -1, startup_stage, control_ready, control_error;
static uint32_t startup_sequence, probe_sequence;
static uint64_t startup_deadline, startup_started, next_teid;
static uint64_t last_queue_sample;
static uint32_t session_count[UPF_MAX_WORKERS];
enum { STOP_N3, STOP_WAIT_N3, STOP_POLL, STOP_WAIT_POLL, STOP_DRAIN, STOP_EXIT };
static int stopping = -1, shutdown_stage, down_candidate = -1;
static uint32_t shutdown_sequence;
static uint64_t shutdown_deadline, shutdown_started, down_since;

enum { WAIT_SLOT, INSTALL_STEERING, WAIT_STEERING, WAIT_CLASSIFIER,
       ROLLBACK_STEERING, WAIT_ROLLBACK, RETIRE_RULES, WAIT_RETIREMENT,
       WAIT_SESSION_CLEANUP, DELETE_RESPONSE, QUARANTINED };
static struct Pending {
    Bufblk *message;
    PfcpXact *xact;
    uint32_t xact_index, transaction_id;
    PfcpNode *peer;
    UpfSession *session;
    int slot, stage, responded, deleting;
    uint32_t sequence, version, cleanup_sequence;
    uint64_t deadline, admission_started, attach_started, smf_seid;
} requests[MAX_PENDING];

static double elapsed_ms(uint64_t start, uint64_t end) {
    return (double)(end - start) * 1000.0 / rte_get_timer_hz();
}

static uint64_t after_ms(uint32_t ms) {
    return rte_get_timer_cycles() + (uint64_t)ms * rte_get_timer_hz() / 1000;
}

static void state(uint16_t slot, UpfWorkerState value) {
    __atomic_store_n(&g_upf_workers->runtime[slot].state, value, __ATOMIC_RELEASE);
}

static int live_count(void) {
    int count = 0;
    for (uint16_t i = 0; i < Self()->scaling.slot_count; i++)
        count += children[i] > 0;
    return count;
}

static int spawn_worker(pid_t *pid, char *const argv[]) {
    posix_spawnattr_t attr;
    int rc = posix_spawnattr_init(&attr);
    if (rc) return rc;
    /* ONVM's polling thread blocks termination signals; the child must not inherit that mask. */
    sigset_t mask;
    sigemptyset(&mask);
    rc = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK);
    if (!rc) rc = posix_spawnattr_setpgroup(&attr, 0);
    if (!rc) rc = posix_spawnattr_setsigmask(&attr, &mask);
    if (!rc) rc = posix_spawn(pid, argv[0], NULL, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);
    return rc;
}

static const char *stop_stage(void) {
    static const char *const names[] = {"n3-delete", "n3-ack", "poll-disable", "poll-ack", "queue-drain", "process-exit"};
    return shutdown_stage == STOP_EXIT && !children[stopping] ? "manager-release" : names[shutdown_stage];
}

static int start_worker(void) {
    if (stopping >= 0) return -EAGAIN;
    if (starting >= 0) return starting;
    if (live_count() >= Self()->scaling.max_workers) return -ENOSPC;
    int slot = -1;
    for (uint16_t i = 0; i < Self()->scaling.slot_count; i++)
        if (g_upf_workers->runtime[i].state == UPF_WORKER_INACTIVE) { slot = i; break; }
    if (slot < 0) return -ENOSPC;
    const UpfWorkerSlotConfig *s = &Self()->scaling.slots[slot];
    if (!UpfWorkerNfTryLock()) return -EAGAIN;
    int busy = service_counts[s->service_id] || manager_cores[s->core].nf_count ||
        manager_cores[s->core].is_dedicated_core || !manager_cores[s->core].enabled;
    UpfWorkerNfUnlock();
    if (busy) { state(slot, UPF_WORKER_FAILED); return -EBUSY; }

    UpfWorkerRuntime *r = &g_upf_workers->runtime[slot];
    if (r->generation == UINT32_MAX) { state(slot, UPF_WORKER_FAILED); return -EOVERFLOW; }
    r->instance_id = r->registered_instance = r->ack_version = r->ack_generation = 0;
    r->generation++;
    __atomic_store_n(&r->reader_generation, r->generation, __ATOMIC_RELEASE);
    state(slot, UPF_WORKER_STARTING); /* Register reader before it can see a snapshot. */
    char core[16], service[16];
    snprintf(core, sizeof(core), "%u", s->core);
    snprintf(service, sizeof(service), "%u", s->service_id);
    char *argv[] = {Self()->scaling.worker_binary, "-l", core, "-n", "4",
        "--proc-type=secondary", "--file-prefix", Self()->scaling.file_prefix,
        "--no-pci", "--", "-r", service, "-m", "--", NULL};
    startup_started = rte_get_timer_cycles();
    int rc = spawn_worker(&children[slot], argv);
    uint64_t spawn_finished = rte_get_timer_cycles();
    if (rc) {
        children[slot] = 0;
        __atomic_store_n(&r->reader_generation, 0, __ATOMIC_RELEASE);
        state(slot, UPF_WORKER_FAILED);
        UTLT_Error("Spawn slot %d: %s spawn_ms=%.3f", slot, strerror(rc),
                   elapsed_ms(startup_started, spawn_finished));
        return -rc;
    }
    starting = slot; startup_stage = START_WAIT_REGISTRATION; startup_sequence = 0;
    startup_deadline = after_ms(Self()->scaling.startup_timeout_ms);
    UTLT_Info("Spawned UPF-U slot=%d pid=%d service=%u core=%u spawn_ms=%.3f",
              slot, children[slot], s->service_id, s->core,
              elapsed_ms(startup_started, spawn_finished));
    return slot;
}

struct QueueSample {
    uint16_t slot, instance;
    uint32_t generation;
    unsigned count, capacity;
};

/* Protect ring lifetime; copy counts and release the lock before logging. */
static int sample_queues(struct QueueSample samples[UPF_MAX_WORKERS]) {
    if (!UpfWorkerNfTryLock()) return -EAGAIN;
    int sample_count = 0;
    for (uint16_t i = 0; i < Self()->scaling.slot_count; i++) {
        UpfWorkerRuntime *r = &g_upf_workers->runtime[i];
        if (r->state != UPF_WORKER_READY || !r->instance_id || r->instance_id >= MAX_NFS) continue;
        struct onvm_nf *nf = &nfs[r->instance_id];
        if (!onvm_nf_is_valid(nf) || nf->service_id != Self()->scaling.slots[i].service_id || !nf->rx_q) continue;
        unsigned count = rte_ring_count(nf->rx_q);
        samples[sample_count].slot = i;
        samples[sample_count].instance = r->instance_id;
        samples[sample_count].generation = r->generation;
        samples[sample_count].count = count;
        samples[sample_count++].capacity = rte_ring_get_capacity(nf->rx_q);
    }
    UpfWorkerNfUnlock();
    return sample_count;
}

/* Complete rounds share a cursor; each worker has its own history and sum.
 * History stays private to UPF-C and needs no allocation in the event loop. */
static struct QueueWindow {
    unsigned values[UPF_MAX_QUEUE_WINDOW_SAMPLES];
    uint64_t sum;
    uint32_t generation;
    uint16_t instance;
} queue_windows[UPF_MAX_WORKERS];
static uint32_t queue_samples, queue_next, queue_ready_mask;

static void cancel_downscale(void) {
    if (down_candidate >= 0) UTLT_Info("Scale-down hold cancelled: slot=%d", down_candidate);
    down_candidate = -1;
    down_since = 0;
}

static void reset_queue_windows(void) {
    cancel_downscale();
    queue_samples = queue_next = queue_ready_mask = 0;
    /* Sums are cleared on the first sample; old values are overwritten before
     * they can be subtracted. No need to clear the whole history buffer. */
}

static void sample_downscale(int ready, uint64_t max_sum, uint64_t now) {
    const UpfScalingConfig *c = &Self()->scaling;
    int candidate = -1;
    if (ready > c->min_workers && max_sum <= (uint64_t)c->scale_down_queue_threshold * c->queue_window_samples) {
        for (int i = c->slot_count - 1; i >= 0; i--)
            if (g_upf_workers->runtime[i].state == UPF_WORKER_READY && !session_count[i]) { candidate = i; break; }
        for (unsigned i = 0; i < MAX_PENDING; i++)
            if (requests[i].message && !requests[i].deleting && !requests[i].responded) candidate = -1;
    }
    if (candidate != down_candidate) {
        cancel_downscale();
        down_candidate = candidate;
        down_since = now;
        if (candidate >= 0) UTLT_Info("Scale-down hold started: slot=%d hold_ms=%u", candidate, c->scale_down_hold_ms);
    }
    if (candidate < 0 || now - down_since < (uint64_t)c->scale_down_hold_ms * rte_get_timer_hz() / 1000) return;
    stopping = candidate;
    shutdown_stage = STOP_N3;
    shutdown_sequence = 0;
    shutdown_started = now;
    shutdown_deadline = after_ms(c->worker_stop_timeout_ms);
    state(stopping, UPF_WORKER_STOPPING);
    down_candidate = -1;
    reset_queue_windows();
    UTLT_Info("UPF-U slot=%d STOPPING sessions=0 max_rx_avg=%.3f hold_ms=%u",
              stopping, (double)max_sum / c->queue_window_samples, c->scale_down_hold_ms);
}

static void sample_load(void) {
    const UpfScalingConfig *c = &Self()->scaling;
    uint64_t now = rte_get_timer_cycles();
    uint64_t interval = (uint64_t)c->queue_sample_interval_ms * rte_get_timer_hz() / 1000;
    uint64_t elapsed = now - last_queue_sample;
    if (elapsed < interval) return;
    last_queue_sample = now;
    if (!control_ready || control_error || starting >= 0 || stopping >= 0) {
        reset_queue_windows();
        return;
    }
    /* A missed interval invalidates history; never invent catch-up samples. */
    if (elapsed >= 2 * interval) reset_queue_windows();
    struct QueueSample samples[UPF_MAX_WORKERS];
    int count = sample_queues(samples);
    if (count <= 0) { reset_queue_windows(); return; }
    uint32_t ready_mask = 0, sampled_mask = 0;
    int changed = 0;
    for (uint16_t i = 0; i < c->slot_count; i++)
        if (g_upf_workers->runtime[i].state == UPF_WORKER_READY) ready_mask |= UINT32_C(1) << i;
    for (int i = 0; i < count; i++) {
        const struct QueueSample *s = &samples[i];
        const struct QueueWindow *w = &queue_windows[s->slot];
        sampled_mask |= UINT32_C(1) << s->slot;
        changed |= w->generation != s->generation || w->instance != s->instance;
    }
    /* sample_queues() may omit a READY worker whose NF/ring is unavailable.
     * Admission can use the others; scaling requires the entire READY set. */
    if (sampled_mask != ready_mask) { reset_queue_windows(); return; }
    if (changed || ready_mask != queue_ready_mask) reset_queue_windows();
    queue_ready_mask = ready_mask;

    uint64_t min_sum = UINT64_MAX, max_sum = 0;
    for (int i = 0; i < count; i++) {
        const struct QueueSample *s = &samples[i];
        struct QueueWindow *w = &queue_windows[s->slot];
        if (!queue_samples) {
            w->sum = 0;
            w->generation = s->generation;
            w->instance = s->instance;
        }
        if (queue_samples == c->queue_window_samples) w->sum -= w->values[queue_next];
        w->values[queue_next] = s->count;
        w->sum += s->count;
        if (w->sum < min_sum) min_sum = w->sum;
        if (w->sum > max_sum) max_sum = w->sum;
    }
    queue_next = (queue_next + 1) % c->queue_window_samples;
    if (queue_samples < c->queue_window_samples) queue_samples++;
    if (queue_samples < c->queue_window_samples) return;
    /* Compare sums to preserve fractional averages at the strict threshold. */
    if (live_count() >= c->max_workers || min_sum <= (uint64_t)c->rx_queue_threshold * c->queue_window_samples) {
        sample_downscale(count, max_sum, now);
        return;
    }
    UTLT_Info("Scale-out trigger: ready=%d min_rx_avg=%.3f threshold=%u window_samples=%u interval_ms=%u",
              count, (double)min_sum / c->queue_window_samples, c->rx_queue_threshold,
              c->queue_window_samples, c->queue_sample_interval_ms);
    reset_queue_windows();
    int slot = start_worker();
    if (slot < 0) UTLT_Warning("Scale-out spawn deferred/failed: error=%d", slot);
}

/* Session placement never starts a worker and never waits for a STARTING slot
 * when a READY worker is available. */
static int select_worker(const struct Pending *p) {
    if (!control_ready) return control_error ? control_error : -EAGAIN;
    struct QueueSample samples[UPF_MAX_WORKERS];
    int sample_count = sample_queues(samples);
    if (sample_count <= 0) return sample_count < 0 ? sample_count : -EAGAIN;
    int best = -1;
    unsigned length = UINT32_MAX;
    for (int i = 0; i < sample_count; i++) {
        if (samples[i].count < length) { best = samples[i].slot; length = samples[i].count; }
    }
    for (int i = 0; i < sample_count; i++) {
        uint16_t slot = samples[i].slot;
        UTLT_Info("Admission queue: xid=%u xact=%u slot=%u instance=%u service=%u rx=%u capacity=%u threshold=%u",
                  p->transaction_id, p->xact_index, slot, samples[i].instance,
                  Self()->scaling.slots[slot].service_id, samples[i].count,
                  samples[i].capacity, Self()->scaling.rx_queue_threshold);
    }
    UTLT_Info("Admission decision: xid=%u xact=%u ready=%d selected_slot=%d min_rx=%u reason=least_queued",
              p->transaction_id, p->xact_index, sample_count, best, length);
    return best;
}

static void reap_workers(void) {
    for (uint16_t i = 0; i < Self()->scaling.slot_count; i++) {
        if (children[i] <= 0) continue;
        int status;
        pid_t rc = waitpid(children[i], &status, WNOHANG);
        if (!rc) {
            if (stop_deadline[i] && rte_get_timer_cycles() > stop_deadline[i]) {
                state(i, UPF_WORKER_FAILED);
                UTLT_Error("UPF-U slot %u stop timed out: stage=%s; sending SIGKILL; slot will not be reused",
                           i, i == stopping ? stop_stage() : "failure-exit");
                kill(-children[i], SIGKILL); stop_deadline[i] = 0;
            }
            continue;
        }
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0 && errno != ECHILD) continue;
        int planned = i == stopping && shutdown_stage == STOP_EXIT &&
            g_upf_workers->runtime[i].state == UPF_WORKER_STOPPING && rc > 0 &&
            WIFEXITED(status) && !WEXITSTATUS(status);
        UTLT_Info("UPF-U slot=%u process exited: pid=%d exit_code=%d signal=%d planned=%d",
                  i, children[i], rc > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                  rc > 0 && WIFSIGNALED(status) ? WTERMSIG(status) : 0, planned);
        if (!planned) {
            UTLT_Warning("UPF-U slot %u exited; existing sessions are not reassigned", i);
            state(i, UPF_WORKER_FAILED);
        }
        children[i] = 0;
        stop_deadline[i] = 0;
        /* Only a confirmed process exit removes a reader from reclamation. */
        __atomic_store_n(&g_upf_workers->runtime[i].reader_generation, 0, __ATOMIC_RELEASE);
    }
}

static void fail_stop(int error) {
    UTLT_Error("UPF-U slot=%d stop failed: stage=%s error=%d; slot retained FAILED", stopping, stop_stage(), error);
    state(stopping, UPF_WORKER_FAILED);
    if (children[stopping] > 0) {
        kill(-children[stopping], SIGTERM);
        stop_deadline[stopping] = after_ms(2000);
    }
}

static void advance_stop(void) {
    if (stopping < 0) return;
    UpfWorkerRuntime *r = &g_upf_workers->runtime[stopping];
    const UpfWorkerSlotConfig *s = &Self()->scaling.slots[stopping];
    if (r->state != UPF_WORKER_FAILED && rte_get_timer_cycles() > shutdown_deadline) fail_stop(-ETIMEDOUT);
    int rc;
    int32_t result;
    /* Even after timeout, consume submitted operations before reusing a mailbox. */
    if (shutdown_sequence) {
        rc = shutdown_stage == STOP_WAIT_N3 ? UpfSteerResult(shutdown_sequence, &result) :
                                             UpfWorkerPollResult(shutdown_sequence, &result);
        if (rc == -EINPROGRESS) return;
        shutdown_sequence = 0;
        if (r->state != UPF_WORKER_FAILED && (rc < 0 || result < 0)) fail_stop(rc < 0 ? rc : result);
        shutdown_stage++;
    }
    if (r->state == UPF_WORKER_FAILED) {
        if (!children[stopping]) { stopping = -1; reset_queue_windows(); }
        return;
    }
    if (shutdown_stage == STOP_N3) {
        UpfSteerUpdate update = {.operation = UPF_STEER_N3_DEL, .slot = stopping, .generation = r->generation};
        rc = UpfSteerRequest(&update, &shutdown_sequence);
        if (!rc) shutdown_stage = STOP_WAIT_N3;
        else if (rc != -EBUSY) fail_stop(rc);
    } else if (shutdown_stage == STOP_POLL) {
        UpfWorkerPollUpdate update = {.slot = stopping, .instance_id = r->instance_id, .generation = r->generation};
        rc = UpfWorkerPollRequest(&update, &shutdown_sequence);
        if (!rc) shutdown_stage = STOP_WAIT_POLL;
        else if (rc != -EBUSY && rc != -EAGAIN) fail_stop(rc);
    } else if (shutdown_stage == STOP_DRAIN) {
        if (!UpfWorkerNfTryLock()) return;
        struct onvm_nf *nf = &nfs[r->instance_id];
        int drained = onvm_nf_is_valid(nf) && nf->service_id == s->service_id && nf->rx_q && nf->tx_q &&
            !rte_ring_count(nf->rx_q) && !rte_ring_count(nf->tx_q);
        UpfWorkerNfUnlock();
        if (!drained) return;
        shutdown_stage = STOP_EXIT;
        if (kill(-children[stopping], SIGTERM) < 0) { fail_stop(-errno); return; }
        UTLT_Info("UPF-U slot=%d SIGTERM sent: pid=%d; waiting for process exit", stopping, children[stopping]);
        stop_deadline[stopping] = shutdown_deadline;
    } else if (shutdown_stage == STOP_EXIT && !children[stopping]) {
        if (!UpfWorkerNfTryLock()) return;
        int busy = service_counts[s->service_id] || manager_cores[s->core].nf_count || manager_cores[s->core].is_dedicated_core;
        UpfWorkerNfUnlock();
        if (busy) return;
        r->instance_id = r->registered_instance = r->ack_version = r->ack_generation = 0;
        state(stopping, UPF_WORKER_INACTIVE);
        UTLT_Info("UPF-U slot=%d INACTIVE stop_ms=%.3f manager_released=1", stopping,
                  elapsed_ms(shutdown_started, rte_get_timer_cycles()));
        stopping = -1;
        reset_queue_windows();
    }
}

static void fail_start(int error) {
    UTLT_Error("UPF-U slot %d startup failed: %s elapsed_ms=%.3f", starting, strerror(-error),
               elapsed_ms(startup_started, rte_get_timer_cycles()));
    state(starting, UPF_WORKER_FAILED);
    if (children[starting] > 0) {
        kill(-children[starting], SIGTERM);
        stop_deadline[starting] = after_ms(2000);
    }
    starting = -1; startup_sequence = 0;
}

static void advance_start(void) {
    if (starting < 0) return;
    UpfWorkerRuntime *r = &g_upf_workers->runtime[starting];
    const UpfWorkerSlotConfig *s = &Self()->scaling.slots[starting];
    int32_t result;
    int rc;
    /* Never abandon a mailbox request. Consume its ACK before timeout cleanup. */
    if (startup_sequence) {
        rc = startup_stage == START_WAIT_N3_ACK ? UpfSteerResult(startup_sequence, &result) : UpfWorkerPollResult(startup_sequence, &result);
        if (rc == -EINPROGRESS) return;
        startup_sequence = 0;
        if (rc < 0 || result < 0) { fail_start(rc < 0 ? rc : result); return; }
        if (startup_stage == START_WAIT_N3_ACK) startup_stage = START_ENABLE_POLLING;
        else {
            if (r->state == UPF_WORKER_FAILED) { starting = -1; return; }
            state(starting, UPF_WORKER_READY);
            uint64_t ready = rte_get_timer_cycles();
            UTLT_Info("UPF-U slot=%d instance=%u READY ready_ms=%.3f", starting, r->instance_id,
                      elapsed_ms(startup_started, ready));
            starting = -1;
            return;
        }
    }
    if (r->state == UPF_WORKER_FAILED || rte_get_timer_cycles() > startup_deadline) {
        fail_start(-ETIMEDOUT); return;
    }
    if (startup_stage == START_WAIT_REGISTRATION) {
        uint32_t instance = __atomic_load_n(&r->registered_instance, __ATOMIC_ACQUIRE);
        if (!instance || instance >= MAX_NFS || !UpfWorkerNfTryLock()) return;
        struct onvm_nf *nf = &nfs[instance];
        int valid = onvm_nf_is_valid(nf) && nf->service_id == s->service_id &&
            nf->thread_info.core == s->core && nf->rx_q && nf->tx_q && service_counts[s->service_id] == 1;
        UpfWorkerNfUnlock();
        if (!valid) return;
        r->instance_id = instance;
        startup_stage = START_WAIT_CLASSIFIER;
    }
    if (startup_stage == START_WAIT_CLASSIFIER) {
        uint32_t version = __atomic_load_n(&g_upf_cls_ctrl->version, __ATOMIC_ACQUIRE);
        if (__atomic_load_n(&r->ack_generation, __ATOMIC_ACQUIRE) != r->generation ||
            __atomic_load_n(&r->ack_version, __ATOMIC_ACQUIRE) < version) return;
        UpfSteerUpdate update = {.operation = UPF_STEER_N3_ADD, .slot = starting, .generation = r->generation};
        rc = UpfSteerRequest(&update, &startup_sequence);
        if (!rc) startup_stage = START_WAIT_N3_ACK;
        else if (rc != -EBUSY) fail_start(rc);
    } else if (startup_stage == START_ENABLE_POLLING) {
        UpfWorkerPollUpdate update = {.slot = starting, .instance_id = r->instance_id,
            .generation = r->generation, .enable = 1};
        rc = UpfWorkerPollRequest(&update, &startup_sequence);
        if (!rc) startup_stage = START_WAIT_POLL_ACK;
        else if (rc != -EBUSY && rc != -EAGAIN) fail_start(rc);
    }
}

static PfcpXact *transaction(struct Pending *p) {
    PfcpXact *x = PfcpXactFind(p->xact_index);
    return x && x == p->xact && x->transactionId == p->transaction_id && x->gnode == p->peer ? x : NULL;
}

static void release_request(struct Pending *p) {
    PfcpStructFree(p->message->buf);
    BufblkFree(p->message);
    memset(p, 0, sizeof(*p));
}

static void reject(struct Pending *p, uint8_t cause) {
    if (!p->responded) {
        UTLT_Warning("%s failed: xid=%u xact=%u slot=%d cause=%u elapsed_ms=%.3f",
                     p->deleting ? "Deletion" : "Admission", p->transaction_id, p->xact_index, p->slot, cause,
                     elapsed_ms(p->admission_started, rte_get_timer_cycles()));
        PfcpXact *x = transaction(p);
        if (x) {
            if (p->deleting) UpfN4SendDeletionResponse(p->smf_seid, x, cause);
            else UpfRejectSessionEstablishment(p->message->buf, x, cause);
            x->applicationPending = 0;
            if (x->timerHolding) TimerStart(x->timerHolding);
        }
        p->responded = 1;
    }
    if (p->deleting) {
        if (!p->sequence && !p->cleanup_sequence) p->stage = QUARANTINED;
        return;
    }
    if (!p->session) release_request(p);
    else if (p->stage < ROLLBACK_STEERING) {
        /* A submitted request must complete before sending the rollback. */
        if (p->stage != WAIT_STEERING) p->stage = ROLLBACK_STEERING;
    }
}

static int request_steering(struct Pending *p, int operation) {
    UpfSteerUpdate update = {.operation = operation, .slot = p->slot,
        .generation = g_upf_workers->runtime[p->slot].generation,
        .session_index = p->session->index,
        .ue_addr = {.s_addr = htonl(p->session->ueIpv4.addr4.s_addr)}};
    return UpfSteerRequest(&update, &p->sequence);
}

static void free_retired_session(struct Pending *p) {
    uint64_t seid = p->session->upfSeid;
    UpfN4FreePendingSession(p->session);
    p->session = NULL;
    session_count[p->slot]--;
    if (p->deleting) {
        UTLT_Info("Deleted SEID=%" PRIu64 " slot=%d sessions=%u", seid, p->slot, session_count[p->slot]);
        p->stage = DELETE_RESPONSE;
    } else release_request(p);
}

static void advance_request(struct Pending *p) {
    if (p->stage == QUARANTINED) return;
    if (p->stage == DELETE_RESPONSE) {
        PfcpXact *x = transaction(p);
        if (x) {
            int rc = UpfN4SendDeletionResponse(p->smf_seid, x, PFCP_CAUSE_REQUEST_ACCEPTED);
            if (rc != STATUS_OK && x->step < 2) return; /* Retry building/caching, never free twice. */
            x->applicationPending = 0;
            if (x->timerHolding) TimerStart(x->timerHolding);
        }
        release_request(p);
        return;
    }
    if (!p->responded && (!transaction(p) || rte_get_timer_cycles() > p->deadline ||
        (p->slot >= 0 && g_upf_workers->runtime[p->slot].state == UPF_WORKER_FAILED))) {
        reject(p, PFCP_CAUSE_NO_RESOURCES_AVAILABLE);
        if (!p->message) return;
    }
    PfcpMessage *message = p->message->buf;
    int rc;
    int32_t result;
    switch (p->stage) {
    case WAIT_SLOT: {
        if (p->slot < 0) p->slot = select_worker(p);
        if (p->slot == -EAGAIN) return;
        if (p->slot < 0) { reject(p, PFCP_CAUSE_NO_RESOURCES_AVAILABLE); return; }
        if (g_upf_workers->runtime[p->slot].state != UPF_WORKER_READY) return;
        p->attach_started = rte_get_timer_cycles();
        if (next_teid > Self()->scaling.teid_last) { reject(p, PFCP_CAUSE_NO_RESOURCES_AVAILABLE); return; }
        const UpfWorkerSlotConfig *s = &Self()->scaling.slots[p->slot];
        UpfWorker selected = {.service_id = s->service_id, .n3_port = s->n3_port, .n6_port = s->n6_port,
            .n3_addr = s->n3_addr, .ul_teid = (uint32_t)next_teid++};
        uint8_t cause;
        p->session = UpfSessionAddByMessageForWorker(message, &cause, &selected);
        if (!p->session) { reject(p, cause); return; }
        session_count[p->slot]++;
        p->session->pfcpNode = p->peer;
        cause = UpfN4InstallSessionRules(p->session, &message->pFCPSessionEstablishmentRequest);
        p->version = __atomic_load_n(&g_upf_cls_ctrl->version, __ATOMIC_ACQUIRE);
        p->stage = INSTALL_STEERING;
        if (cause != PFCP_CAUSE_REQUEST_ACCEPTED) reject(p, cause);
        break;
    }
    case INSTALL_STEERING:
        rc = request_steering(p, UPF_STEER_SESSION_ADD);
        if (!rc) p->stage = WAIT_STEERING;
        else if (rc != -EBUSY) reject(p, PFCP_CAUSE_NO_RESOURCES_AVAILABLE);
        break;
    case WAIT_STEERING:
        rc = UpfSteerResult(p->sequence, &result);
        if (rc == -EINPROGRESS) break;
        p->sequence = 0;
        p->stage = p->responded ? ROLLBACK_STEERING : WAIT_CLASSIFIER;
        if (rc < 0 || result < 0) reject(p, PFCP_CAUSE_NO_RESOURCES_AVAILABLE);
        break;
    case WAIT_CLASSIFIER: {
        UpfWorkerRuntime *r = &g_upf_workers->runtime[p->slot];
        if (__atomic_load_n(&r->ack_generation, __ATOMIC_ACQUIRE) != r->generation ||
            __atomic_load_n(&r->ack_version, __ATOMIC_ACQUIRE) < p->version) break;
        PfcpXact *x = transaction(p);
        if (!x) { reject(p, PFCP_CAUSE_NO_RESOURCES_AVAILABLE); break; }
        uint64_t attached = rte_get_timer_cycles();
        /* Once a success response is cached, retain the session even if the
         * first send fails: PFCP retransmission will replay the same endpoint. */
        rc = UpfN4SendEstablishmentResponse(p->session, x, &message->pFCPSessionEstablishmentRequest,
                                           PFCP_CAUSE_REQUEST_ACCEPTED);
        uint64_t response_finished = rte_get_timer_cycles();
        if (rc != STATUS_OK && x->step < 2) {
            reject(p, PFCP_CAUSE_NO_RESOURCES_AVAILABLE);
            break;
        }
        x->applicationPending = 0;
        if (x->timerHolding) TimerStart(x->timerHolding);
        UTLT_Info("Admitted SEID=%" PRIu64 " slot=%d TEID=%u xid=%u xact=%u worker_wait_ms=%.3f attach_ms=%.3f response_ms=%.3f admission_ms=%.3f response_rc=%d",
                  p->session->upfSeid, p->slot, p->session->teid, p->transaction_id, p->xact_index,
                  elapsed_ms(p->admission_started, p->attach_started),
                  elapsed_ms(p->attach_started, attached),
                  elapsed_ms(attached, response_finished),
                  elapsed_ms(p->admission_started, response_finished), rc);
        release_request(p);
        break;
    }
    case ROLLBACK_STEERING:
        rc = request_steering(p, UPF_STEER_SESSION_DEL);
        if (!rc) p->stage = WAIT_ROLLBACK;
        else if (rc != -EBUSY) p->stage = QUARANTINED;
        break;
    case WAIT_ROLLBACK:
        rc = UpfSteerResult(p->sequence, &result);
        if (rc == -EINPROGRESS) break;
        p->sequence = 0;
        p->stage = rc < 0 || result < 0 || (p->deleting && p->responded) ? QUARANTINED : RETIRE_RULES;
        break;
    case RETIRE_RULES:
        p->stage = UpfN4AbortPendingSession(p->session, &p->version) < 0 ? QUARANTINED : WAIT_RETIREMENT;
        break;
    case WAIT_RETIREMENT:
        if (UpfClsSafeVersion() >= p->version) {
            if (!p->deleting) { free_retired_session(p); break; }
            UpfSessionCleanup update = {.seid = p->session->upfSeid, .session_index = p->session->index,
                .generation = g_upf_workers->runtime[p->slot].generation, .version = p->version};
            rc = UpfWorkerCleanupRequest(p->slot, &update, &p->cleanup_sequence);
            if (!rc) p->stage = WAIT_SESSION_CLEANUP;
            else if (rc != -EBUSY) p->stage = QUARANTINED;
        }
        break;
    case WAIT_SESSION_CLEANUP:
        rc = UpfWorkerCleanupResult(p->slot, p->cleanup_sequence, &result);
        if (rc == -EINPROGRESS) break;
        p->cleanup_sequence = 0;
        if (rc < 0 || result < 0 || p->responded) p->stage = QUARANTINED;
        else free_retired_session(p);
        break;
    }
    if (p->stage == QUARANTINED) {
        if (p->deleting && !p->responded) reject(p, PFCP_CAUSE_SYSTEM_FAILURE);
        UTLT_Error("Session SEID=%" PRIu64 " retained inactive: cleanup uncertain; restart after repairing PF/classifier state",
                   p->session->upfSeid);
    }
}

int UpfScalingInit(void) {
    if (!Self()->scaling.slot_count) return 0;
    const char *runtime = rte_eal_get_runtime_dir();
    const char *prefix = runtime ? strrchr(runtime, '/') : NULL;
    if (!prefix || strcmp(prefix + 1, Self()->scaling.file_prefix)) {
        UTLT_Error("scaling.file_prefix must match UPF-C's EAL namespace");
        return -EINVAL;
    }
    if (onvm_nflib_get_onvm_config()->flags.ONVM_NF_SHARE_CORES) {
        UTLT_Error("Dynamic scaling requires dedicated polling cores; disable manager shared-core sleep mode");
        return -EOPNOTSUPP;
    }
    if (access(Self()->scaling.worker_binary, X_OK) < 0) return -errno;
    const struct rte_memzone *core = rte_memzone_lookup(MZ_CORES_STATUS);
    const struct rte_memzone *counts = rte_memzone_lookup(MZ_NF_PER_SERVICE_INFO);
    if (!core || !counts) return -ENOENT;
    manager_cores = core->addr; service_counts = counts->addr;
    next_teid = Self()->scaling.teid_first;
    return UpfClsRebuildAndPublish(NULL) ? 0 : -ENOMEM;
}

int UpfScalingEnqueue(Bufblk *message, PfcpXact *xact) {
    uint64_t admission_started = rte_get_timer_cycles();
    if (control_error) return control_error;
    cancel_downscale();
    for (unsigned i = 0; i < MAX_PENDING; i++) {
        struct Pending *p = &requests[i];
        if (p->message) continue;
        *p = (struct Pending){.message = message, .xact = xact, .peer = xact->gnode,
            .xact_index = xact->index, .transaction_id = xact->transactionId,
            .slot = -1, .deadline = after_ms(Self()->scaling.startup_timeout_ms + 60000),
            .admission_started = admission_started};
        xact->applicationPending = 1;
        UTLT_Info("Admission received: xid=%u xact=%u", p->transaction_id, p->xact_index);
        p->slot = select_worker(p);
        return 0;
    }
    return -ENOSPC;
}

int UpfScalingDelete(Bufblk *message, PfcpXact *xact, UpfSession *session) {
    if (control_error) return control_error;
    if (session->admission_pending || session->deletion_pending) return -EBUSY;
    int slot = -1;
    for (uint16_t i = 0; i < Self()->scaling.slot_count; i++)
        if (Self()->scaling.slots[i].service_id == session->worker.service_id) { slot = i; break; }
    if (slot < 0 || g_upf_workers->runtime[slot].state != UPF_WORKER_READY) return -ENODEV;
    for (unsigned i = 0; i < MAX_PENDING; i++) {
        struct Pending *p = &requests[i];
        if (p->message) continue;
        *p = (struct Pending){.message = message, .xact = xact, .peer = xact->gnode,
            .xact_index = xact->index, .transaction_id = xact->transactionId,
            .session = session, .slot = slot, .deleting = 1, .smf_seid = session->smfSeid,
            .stage = ROLLBACK_STEERING, .deadline = after_ms(60000), .admission_started = rte_get_timer_cycles()};
        __atomic_store_n(&session->deletion_pending, 1, __ATOMIC_RELEASE);
        xact->applicationPending = 1;
        UTLT_Info("Deletion received: SEID=%" PRIu64 " slot=%d xid=%u", session->upfSeid, slot, xact->transactionId);
        return 0;
    }
    return -ENOSPC;
}

int UpfControlLoop(struct onvm_nf_local_ctx *ctx) {
    (void)ctx;
    if (!Self()->scaling.slot_count) return 0;
    static uint64_t last_timer;
    uint64_t now = rte_get_timer_cycles();
    if (now - last_timer >= rte_get_timer_hz() / 100) {
        TimerExpireCheck(&Self()->timerServiceList, Self()->eventQ);
        last_timer = now;
    }
    reap_workers();
    if (!control_ready && !control_error && UpfWorkerPollingStatus() == 1) {
        if (!probe_sequence) {
            UpfSteerUpdate probe = {.operation = UPF_STEER_PROBE};
            int rc = UpfSteerRequest(&probe, &probe_sequence);
            if (rc && rc != -EBUSY) control_error = rc;
        } else {
            int32_t result;
            int rc = UpfSteerResult(probe_sequence, &result);
            if (rc != -EINPROGRESS) {
                control_error = rc < 0 ? rc : result;
                control_ready = !control_error;
                if (control_error) UTLT_Error("Manager steering probe failed: %s", strerror(-control_error));
            }
        }
    }
    if (UpfWorkerPollingStatus() < 0) control_error = UpfWorkerPollingStatus();
    advance_start();
    advance_stop();
    if (control_ready && !control_error && starting < 0 && stopping < 0 && live_count() < Self()->scaling.min_workers) start_worker();
    for (unsigned i = 0; i < MAX_PENDING; i++) if (requests[i].message) advance_request(&requests[i]);
    sample_load();
    UpfClsCollect();
    return 0;
}

void UpfScalingStop(void) {
    for (uint16_t i = 0; i < Self()->scaling.slot_count; i++) {
        state(i, UPF_WORKER_FAILED);
        if (children[i] > 0) kill(-children[i], SIGTERM);
    }
    /* Manager tears down NFs. No classifier memory is reclaimed after shutdown. */
}
