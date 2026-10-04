/* SPDX-License-Identifier: Apache-2.0
 * One UPF-C event loop owns admission, child processes and pending requests.
 * Queue counts are read once at placement, never by a background sampler.
 */
#include <errno.h>
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

enum { WAIT_SLOT, INSTALL_STEERING, WAIT_STEERING, WAIT_CLASSIFIER,
       ROLLBACK_STEERING, WAIT_ROLLBACK, RETIRE_RULES, WAIT_RETIREMENT, QUARANTINED };
static struct Pending {
    Bufblk *message;
    PfcpXact *xact;
    uint32_t xact_index, transaction_id;
    PfcpNode *peer;
    UpfSession *session;
    int slot, stage, responded;
    uint32_t sequence, version;
    uint64_t deadline, admission_started, attach_started;
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

static int start_worker(void) {
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
    r->generation++;
    __atomic_store_n(&r->reader_generation, r->generation, __ATOMIC_RELEASE);
    state(slot, UPF_WORKER_STARTING); /* Register reader before it can see a snapshot. */
    char core[16], service[16];
    snprintf(core, sizeof(core), "%u", s->core);
    snprintf(service, sizeof(service), "%u", s->service_id);
    char *argv[] = {Self()->scaling.worker_binary, "-l", core, "-n", "4",
        "--proc-type=secondary", "--file-prefix", Self()->scaling.file_prefix,
        "--no-pci", "--", "-r", service, "-m", "--", NULL};
    posix_spawnattr_t attr;
    startup_started = rte_get_timer_cycles();
    int rc = posix_spawnattr_init(&attr);
    if (!rc) {
        rc = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
        if (!rc) rc = posix_spawnattr_setpgroup(&attr, 0);
        if (!rc) rc = posix_spawn(&children[slot], argv[0], NULL, &attr, argv, environ);
        posix_spawnattr_destroy(&attr);
    }
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

/* Called at admission only. An in-progress worker absorbs concurrent requests;
 * at the configured limit, use the least queued ready worker. */
static int select_worker(const struct Pending *p) {
    if (!control_ready) return control_error ? control_error : -EAGAIN;
    if (!UpfWorkerNfTryLock()) return -EAGAIN;
    struct {
        uint16_t slot, instance;
        unsigned count, capacity;
    } samples[UPF_MAX_WORKERS];
    unsigned sample_count = 0;
    int best = -1;
    unsigned length = UINT32_MAX;
    for (uint16_t i = 0; i < Self()->scaling.slot_count; i++) {
        UpfWorkerRuntime *r = &g_upf_workers->runtime[i];
        if (r->state != UPF_WORKER_READY || !r->instance_id || r->instance_id >= MAX_NFS) continue;
        struct onvm_nf *nf = &nfs[r->instance_id];
        if (!onvm_nf_is_valid(nf) || nf->service_id != Self()->scaling.slots[i].service_id || !nf->rx_q) continue;
        unsigned count = rte_ring_count(nf->rx_q);
        samples[sample_count].slot = i;
        samples[sample_count].instance = r->instance_id;
        samples[sample_count].count = count;
        samples[sample_count++].capacity = rte_ring_get_capacity(nf->rx_q);
        if (count < length) { best = i; length = count; }
    }
    UpfWorkerNfUnlock();
    int selected, spawn_error = 0;
    const char *reason;
    if (best >= 0 && length < Self()->scaling.rx_queue_threshold) {
        selected = best;
        reason = "below_threshold";
    } else if (starting >= 0) {
        selected = starting;
        reason = "wait_starting";
    } else {
        int slot = start_worker();
        if (slot == -EAGAIN) return slot; /* Retry without logging lock contention. */
        selected = slot >= 0 ? slot : (best >= 0 ? best : slot);
        spawn_error = slot < 0 ? slot : 0;
        reason = slot >= 0 ? (best >= 0 ? "spawn_threshold" : "spawn_no_ready") :
            (slot == -ENOSPC ? "worker_limit" : "spawn_failed");
    }
    /* Log the values used above, never reread rings or hold the NF lifecycle
     * lock during logging. */
    for (unsigned i = 0; i < sample_count; i++) {
        uint16_t slot = samples[i].slot;
        UTLT_Info("Admission queue: xid=%u xact=%u slot=%u instance=%u service=%u rx=%u capacity=%u threshold=%u",
                  p->transaction_id, p->xact_index, slot, samples[i].instance,
                  Self()->scaling.slots[slot].service_id, samples[i].count,
                  samples[i].capacity, Self()->scaling.rx_queue_threshold);
    }
    UTLT_Info("Admission decision: xid=%u xact=%u ready=%u best_slot=%d min_rx=%u selected_slot=%d reason=%s spawn_error=%d",
              p->transaction_id, p->xact_index, sample_count, best, best >= 0 ? length : 0,
              selected, reason, spawn_error);
    return selected;
}

static void reap_workers(void) {
    for (uint16_t i = 0; i < Self()->scaling.slot_count; i++) {
        if (children[i] <= 0) continue;
        int status;
        pid_t rc = waitpid(children[i], &status, WNOHANG);
        if (!rc) {
            if (stop_deadline[i] && rte_get_timer_cycles() > stop_deadline[i]) {
                kill(-children[i], SIGKILL); stop_deadline[i] = 0;
            }
            continue;
        }
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0 && errno != ECHILD) continue;
        UTLT_Warning("UPF-U slot %u exited; existing sessions are not reassigned", i);
        children[i] = 0;
        state(i, UPF_WORKER_FAILED);
        /* Only a confirmed process exit removes a reader from reclamation. */
        __atomic_store_n(&g_upf_workers->runtime[i].reader_generation, 0, __ATOMIC_RELEASE);
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
        UTLT_Warning("Admission failed: xid=%u xact=%u slot=%d cause=%u elapsed_ms=%.3f",
                     p->transaction_id, p->xact_index, p->slot, cause,
                     elapsed_ms(p->admission_started, rte_get_timer_cycles()));
        PfcpXact *x = transaction(p);
        if (x) {
            UpfRejectSessionEstablishment(p->message->buf, x, cause);
            x->applicationPending = 0;
            if (x->timerHolding) TimerStart(x->timerHolding);
        }
        p->responded = 1;
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

static void advance_request(struct Pending *p) {
    if (p->stage == QUARANTINED) return;
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
        UTLT_Info("Admitted SEID=%lu slot=%d TEID=%u xid=%u xact=%u worker_wait_ms=%.3f attach_ms=%.3f response_ms=%.3f admission_ms=%.3f response_rc=%d",
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
        p->stage = rc < 0 || result < 0 ? QUARANTINED : RETIRE_RULES;
        break;
    case RETIRE_RULES:
        p->stage = UpfN4AbortPendingSession(p->session, &p->version) < 0 ? QUARANTINED : WAIT_RETIREMENT;
        break;
    case WAIT_RETIREMENT:
        if (UpfClsSafeVersion() >= p->version) {
            UpfN4FreePendingSession(p->session);
            release_request(p);
        }
        break;
    }
    if (p->stage == QUARANTINED)
        UTLT_Error("Failed establishment SEID=%lu retained inactive: rollback uncertain; restart after repairing PF/classifier state",
                   p->session->upfSeid);
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
    for (unsigned i = 0; i < MAX_PENDING; i++) {
        struct Pending *p = &requests[i];
        if (p->message) continue;
        *p = (struct Pending){.message = message, .xact = xact, .peer = xact->gnode,
            .xact_index = xact->index, .transaction_id = xact->transactionId,
            .slot = -1, .deadline = after_ms(Self()->scaling.startup_timeout_ms + 60000),
            .admission_started = admission_started};
        xact->applicationPending = 1;
        p->slot = select_worker(p);
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
    if (control_ready && starting < 0 && live_count() < Self()->scaling.min_workers) start_worker();
    for (unsigned i = 0; i < MAX_PENDING; i++) if (requests[i].message) advance_request(&requests[i]);
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
