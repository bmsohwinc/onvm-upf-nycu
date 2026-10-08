/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "upf_worker.h"
#include "../../../onvm/upf/upf_worker.c"

#define MAX_NFS 8
#define SESS_BUF_MAX_USERS UPF_MAX_SESSION_RULES
#define STATUS_OK 0
#define STATUS_ERROR -1
#define STATUS_EAGAIN -2
#define PFCP_CAUSE_REQUEST_ACCEPTED 1
#define PFCP_CAUSE_NO_RESOURCES_AVAILABLE 75
#define PFCP_CAUSE_SYSTEM_FAILURE 77
#define PFCP_SESSION_DELETION_RESPONSE 55
#define PFCP_SESSION_DELETION_REQUEST 54
#define PFCP_SESSION_ESTABLISHMENT_REQUEST 50
#define PFCP_SESSION_REPORT_RESPONSE 57
#define PFCP_CAUSE_SESSION_CONTEXT_NOT_FOUND 65
#define PFCP_CAUSE_REQUEST_REJECTED 64
enum { PFCP_LOCAL_ORIGINATOR, PFCP_REMOTE_ORIGINATOR };
typedef enum { PFCP_XACT_INITIAL_STAGE, PFCP_XACT_INTERMEDIATE_STAGE, PFCP_XACT_FINAL_STAGE } PfcpXactStage;
typedef int Status;
typedef struct { int unused; } PfcpNode;
typedef struct { unsigned type, seidP; uint64_t seid; } PfcpHeader;
typedef struct { int presence, len; void *value; } Tlv;
typedef struct { Tlv cause; } PFCPSessionDeletionResponse;
typedef struct { int unused; } Establishment;
typedef struct {
    PfcpHeader header;
    Establishment pFCPSessionEstablishmentRequest;
    PFCPSessionDeletionResponse pFCPSessionDeletionResponse;
} PfcpMessage;
typedef struct { uint16_t service_id, n3_port, n6_port; struct in_addr n3_addr; uint32_t ul_teid; } UpfWorker;
typedef struct _UpfSession {
    int index;
    uint64_t upfSeid, smfSeid;
    uint32_t teid, admission_pending, deletion_pending;
    struct { struct in_addr addr4; } ueIpv4;
    UpfWorker worker;
    PfcpNode *pfcpNode;
} UpfSession;
typedef struct { void *buf; } Bufblk;
typedef struct {
    uint32_t index, transactionId;
    PfcpNode *gnode;
    int applicationPending, timerHolding, step, cached_cause;
    uint64_t cached_seid;
    int origin, timerResponse;
    struct { uint8_t type; Bufblk *bufBlk; } seq[3];
} PfcpXact;
static Bufblk cached_response;
struct core_status { int nf_count, is_dedicated_core, enabled; } cores[32];
struct rte_ring { unsigned count; } rx[MAX_NFS], tx[MAX_NFS];
struct onvm_nf {
    int valid;
    uint16_t service_id;
    struct { unsigned core; } thread_info;
    struct rte_ring *rx_q, *tx_q;
} nfs[MAX_NFS];
static struct { UpfScalingConfig scaling; } context;
#define Self() (&context)
static UpfWorkerRegistry registry;
static uint16_t counts[32];
static PfcpNode peer;
static PfcpXact xact;
static UpfSession session;
static struct { uint32_t version; } classifier, *g_upf_cls_ctrl = &classifier;
static uint64_t now;
static uint32_t safe_version;
static int frees, responses, term_calls, kill_calls, exit_ready, spawn_calls, retire_error, cache_error;
static int exit_status;
static uint64_t built_seid;
static uint8_t built_cause;
int rte_errno;
enum rte_proc_type_t rte_eal_process_type(void) { return RTE_PROC_PRIMARY; }
const struct rte_memzone *rte_memzone_lookup(const char *name) { (void)name; return NULL; }
const struct rte_memzone *rte_memzone_reserve(const char *n, size_t s, int i, unsigned f) {
    (void)n; (void)s; (void)i; (void)f; return NULL;
}
static void log_message(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_message(const char *fmt, ...) { (void)fmt; }
#define UTLT_Info log_message
#define UTLT_Warning(...) ((void)0)
#define UTLT_Error log_message
#define UTLT_Debug log_message
#define UTLT_Trace(...) ((void)0)
#define UTLT_Assert(cond, action, ...) do { if (!(cond)) { action; } } while (0)
static uint64_t rte_get_timer_cycles(void) { return now; }
static uint64_t rte_get_timer_hz(void) { return 1000; }
static int onvm_nf_is_valid(const struct onvm_nf *nf) { return nf->valid; }
static unsigned rte_ring_count(const struct rte_ring *q) { assert(registry.nf_lock); return q->count; }
static unsigned rte_ring_get_capacity(const struct rte_ring *q) { (void)q; return 4095; }
static void TimerStart(int timer) { (void)timer; }
static void TimerStop(int timer) { (void)timer; }
static PfcpXact *PfcpXactFind(uint32_t index) { return index == xact.index ? &xact : NULL; }
static void PfcpStructFree(void *message) { (void)message; }
static void BufblkFree(Bufblk *message) { free(message->buf); free(message); }
static int PfcpBuildMessage(Bufblk **out, PfcpMessage *message) {
    built_seid = message->header.seid;
    built_cause = *(uint8_t *)message->pFCPSessionDeletionResponse.cause.value;
    assert(message->header.seidP && message->header.type == PFCP_SESSION_DELETION_RESPONSE);
    *out = calloc(1, sizeof(**out));
    return STATUS_OK;
}
static int PfcpXactUpdateTx(PfcpXact *x, PfcpHeader *header, Bufblk *response) {
    if (cache_error) return STATUS_ERROR;
    assert(header->seid == built_seid);
    x->step = 2; x->cached_seid = built_seid; x->cached_cause = built_cause;
    x->seq[1].type = header->type; x->seq[1].bufBlk = &cached_response;
    BufblkFree(response);
    return STATUS_OK;
}
static int PfcpXactCommit(PfcpXact *x) { (void)x; responses++; return STATUS_OK; }
#include "response.inc"
static void UpfRejectSessionEstablishment(void *m, PfcpXact *x, uint8_t cause) {
    (void)m; x->cached_cause = cause;
}
static UpfSession *UpfSessionAddByMessageForWorker(void *m, uint8_t *cause, const UpfWorker *w) {
    (void)m; *cause = PFCP_CAUSE_REQUEST_ACCEPTED; session.worker = *w; return &session;
}
static uint8_t UpfN4InstallSessionRules(UpfSession *s, Establishment *e) {
    (void)s; (void)e; return PFCP_CAUSE_REQUEST_ACCEPTED;
}
static int UpfN4SendEstablishmentResponse(UpfSession *s, PfcpXact *x, Establishment *e, uint8_t cause) {
    (void)s; (void)e; (void)cause; x->step = 2; return STATUS_OK;
}
static int UpfN4AbortPendingSession(UpfSession *s, uint32_t *version) {
    assert(s == &session); *version = 4; return retire_error;
}
static uint32_t UpfClsSafeVersion(void) { return safe_version; }
static void UpfN4FreePendingSession(UpfSession *s) { assert(s == &session); frees++; }
static int mock_spawn(pid_t *pid, const char *p, const posix_spawn_file_actions_t *a,
                      const posix_spawnattr_t *attr, char *const argv[], char *const env[]) {
    (void)p; (void)a; (void)attr; (void)argv; (void)env; *pid = 1234; spawn_calls++; return 0;
}
static int mock_kill(pid_t pid, int signal) {
    assert(pid < 0); if (signal == SIGTERM) term_calls++; else { assert(signal == SIGKILL); kill_calls++; }
    return 0;
}
static pid_t mock_waitpid(pid_t pid, int *status, int flags) {
    assert(flags == WNOHANG);
    if (!exit_ready || pid != 101) return 0;
    *status = exit_status; exit_ready = 0; return pid;
}
#define posix_spawn mock_spawn
#define kill mock_kill
#define waitpid mock_waitpid
#include "lifecycle.inc"

static int g_worker_slot, purges;
static uint32_t g_worker_generation;
static uint16_t g_upfu_service_id;
static struct { uint32_t ver; } g_cls_local;
static UpfSession *UpfGetSessionByIndex(uint32_t index) { assert(index == (uint32_t)session.index); return &session; }
static int findIndexByUeIpAddress(uint32_t ue) { assert(ue == session.ueIpv4.addr4.s_addr); return 7; }
static void upf_u_shaper_forget_ue(int index) { assert(index == 7); purges++; }
static void removeEntrybyUeIp(uint32_t ue) { assert(ue == session.ueIpv4.addr4.s_addr); }
#include "cleanup.inc"

static PfcpXactStage PfcpXactGetStage(uint8_t type, uint32_t id) {
    (void)id; assert(type == PFCP_SESSION_DELETION_REQUEST); return PFCP_XACT_INITIAL_STAGE;
}
static int PfcpSend(PfcpNode *node, Bufblk *buf) { assert(node == &peer && buf == &cached_response); responses++; return STATUS_OK; }
#include "receive.inc"
static int PfcpXactReceive(PfcpNode *node, PfcpHeader *header, PfcpXact **out) {
    assert(node == &peer); *out = &xact; return PfcpXactUpdateRx(&xact, header->type);
}
static UpfSession *UpfSessionFindBySeid(uint64_t seid) { return !frees && seid == session.upfSeid ? &session : NULL; }
static void dispatch_deletion(Bufblk *bufBlk) {
    PfcpMessage *pfcpMessage = bufBlk->buf;
    pfcpMessage->header = (PfcpHeader){.type = PFCP_SESSION_DELETION_REQUEST, .seidP = 1, .seid = session.upfSeid};
    PfcpXact *xact = NULL;
    UpfSession *session = NULL;
    PfcpNode *upf = &peer;
    Status status;
#include "dispatch.inc"
freeBuf:
    if (pfcpMessage) PfcpStructFree(pfcpMessage);
    if (bufBlk) BufblkFree(bufBlk);
}

static Bufblk *message(void) {
    Bufblk *b = calloc(1, sizeof(*b)); b->buf = calloc(1, sizeof(PfcpMessage)); return b;
}
static void setup(void) {
    for (unsigned i = 0; i < MAX_PENDING; i++) if (requests[i].message) release_request(&requests[i]);
    memset(&registry, 0, sizeof(registry)); memset(children, 0, sizeof(children));
    memset(stop_deadline, 0, sizeof(stop_deadline)); memset(cores, 0, sizeof(cores));
    memset(counts, 0, sizeof(counts)); memset(rx, 0, sizeof(rx)); memset(tx, 0, sizeof(tx));
    memset(session_count, 0, sizeof(session_count)); memset(&session, 0, sizeof(session));
    context.scaling = (UpfScalingConfig){.slot_count = 2, .min_workers = 1, .max_workers = 2,
        .rx_queue_threshold = 40, .queue_window_samples = 2, .queue_sample_interval_ms = 10,
        .scale_down_queue_threshold = 10, .scale_down_hold_ms = 50, .worker_stop_timeout_ms = 100,
        .teid_first = 4097, .teid_last = 10000};
    strcpy(context.scaling.worker_binary, "/mock/upf"); strcpy(context.scaling.file_prefix, "test");
    registry.config = context.scaling; registry.configured = registry.polling_status = 1;
    g_upf_workers = &registry; manager_cores = cores; service_counts = counts;
    starting = stopping = down_candidate = -1; control_ready = 1; control_error = 0;
    now = last_queue_sample = 0; steer_consumed = startup_sequence = shutdown_sequence = 0;
    frees = responses = term_calls = kill_calls = exit_ready = spawn_calls = retire_error = cache_error = purges = 0;
    exit_status = 0;
    safe_version = 0; next_teid = 4097;
    reset_queue_windows();
    xact = (PfcpXact){.index = 1, .transactionId = 10, .gnode = &peer, .timerHolding = 1,
        .origin = PFCP_REMOTE_ORIGINATOR, .step = 1, .seq = {{.type = PFCP_SESSION_DELETION_REQUEST}}};
    for (int i = 0; i < 2; i++) {
        context.scaling.slots[i] = (UpfWorkerSlotConfig){.service_id = 14 + i, .core = 3 + i};
        registry.config.slots[i] = context.scaling.slots[i];
        registry.runtime[i] = (UpfWorkerRuntime){.state = UPF_WORKER_READY, .generation = 1,
            .reader_generation = 1, .instance_id = i + 1, .registered_instance = i + 1};
        nfs[i + 1] = (struct onvm_nf){.valid = 1, .service_id = 14 + i, .rx_q = &rx[i + 1], .tx_q = &tx[i + 1]};
        cores[3 + i] = (struct core_status){.enabled = 1, .nf_count = 1, .is_dedicated_core = 1};
        counts[14 + i] = 1; children[i] = 100 + i;
    }
    session = (UpfSession){.index = 5, .upfSeid = 7, .smfSeid = 20, .worker = {.service_id = 15},
        .ueIpv4 = {.addr4 = {.s_addr = 0x0a3c0001}}};
    g_worker_slot = 1; g_worker_generation = 1; g_upfu_service_id = 15; g_cls_local.ver = 0;
}
static void tick(void) { now += 10; sample_load(); }
static void ack_steer(int result) {
    registry.steer_result = result; registry.steer_ack_seq = registry.steer_request_seq;
}
static void ack_poll(int result) {
    registry.poll_result = result; registry.poll_ack_seq = registry.poll_request_seq;
}
static void start_stop(void) {
    for (int i = 0; i < 6; i++) tick();
    assert(stopping < 0); tick();
    assert(stopping == 1 && registry.runtime[1].state == UPF_WORKER_STOPPING);
}
static void delete_to_cleanup(void) {
    session_count[1] = 1;
    assert(UpfScalingDelete(message(), &xact, &session) == 0);
    assert(session.deletion_pending && xact.applicationPending);
    advance_request(&requests[0]);
    assert(registry.steer_request.operation == UPF_STEER_SESSION_DEL);
    advance_request(&requests[0]); assert(!frees && session_count[1] == 1);
    ack_steer(0); advance_request(&requests[0]); advance_request(&requests[0]);
    assert(requests[0].stage == WAIT_RETIREMENT);
    advance_request(&requests[0]); assert(!registry.runtime[1].cleanup_request_seq);
    safe_version = 4; advance_request(&requests[0]);
    assert(requests[0].stage == WAIT_SESSION_CLEANUP && !frees);
}

int main(void) {
    setup(); session_count[0] = 1; start_stop();
    assert(start_worker() == -EAGAIN && !spawn_calls);
    struct Pending admission = {0}; assert(select_worker(&admission) == 0);
    advance_stop(); assert(shutdown_stage == STOP_WAIT_N3 && !term_calls);
    advance_stop(); assert(shutdown_sequence && !term_calls);
    ack_steer(0); advance_stop(); assert(shutdown_stage == STOP_WAIT_POLL && !term_calls);
    rx[2].count = 1; ack_poll(0); advance_stop(); assert(!term_calls);
    rx[2].count = 0; tx[2].count = 1; advance_stop(); assert(!term_calls);
    tx[2].count = 0; advance_stop(); assert(term_calls == 1 && shutdown_stage == STOP_EXIT);
    advance_stop(); assert(registry.runtime[1].reader_generation == 1);
    exit_ready = 1; reap_workers(); advance_stop();
    assert(registry.runtime[1].reader_generation == 0 && stopping == 1); /* Manager still owns core/service. */
    assert(!strcmp(stop_stage(), "manager-release"));
    counts[15] = cores[4].nf_count = cores[4].is_dedicated_core = 0;
    advance_stop(); assert(stopping < 0 && registry.runtime[1].state == UPF_WORKER_INACTIVE);
    assert(!registry.runtime[1].registered_instance && registry.runtime[1].generation == 1);
    assert(start_worker() == 1 && spawn_calls == 1 && registry.runtime[1].generation == 2);
    assert(registry.runtime[1].reader_generation == 2);

    setup(); session_count[0] = session_count[1] = 1;
    for (int i = 0; i < 20; i++) tick();
    assert(stopping < 0); /* Silent established sessions remain occupied. */
    setup(); context.scaling.min_workers = 2;
    for (int i = 0; i < 20; i++) tick(); assert(stopping < 0);
    setup(); rx[1].count = 41;
    for (int i = 0; i < 20; i++) tick(); assert(stopping < 0); /* Keep spare for a busy peer. */
    setup(); tick(); tick(); tick();
    registry.nf_lock = 1; tick(); registry.nf_lock = 0;
    assert(down_candidate < 0); start_stop(); /* New full hold after missing data. */
    setup(); tick(); tick(); tick();
    Bufblk *pending = message(); assert(UpfScalingEnqueue(pending, &xact) == 0);
    assert(down_candidate < 0);
    for (int i = 0; i < 20; i++) tick(); assert(stopping < 0);

    setup(); start_stop(); advance_stop();
    now += 101; advance_stop();
    assert(registry.runtime[1].state == UPF_WORKER_FAILED && shutdown_sequence);
    uint32_t seq; UpfSteerUpdate update = {.operation = UPF_STEER_N3_ADD};
    assert(UpfSteerRequest(&update, &seq) == -EBUSY); /* Timed-out request still owns mailbox. */
    ack_steer(0); advance_stop(); exit_ready = 1; reap_workers(); advance_stop();
    assert(stopping < 0 && registry.runtime[1].state == UPF_WORKER_FAILED);
    assert(start_worker() == -ENOSPC);
    setup(); start_stop(); advance_stop(); ack_steer(-EIO); advance_stop();
    assert(registry.runtime[1].state == UPF_WORKER_FAILED && !registry.poll_request_seq);

    setup(); start_stop(); advance_stop(); ack_steer(0); advance_stop(); ack_poll(0); advance_stop();
    assert(term_calls == 1 && !strcmp(stop_stage(), "process-exit"));
    now += 101; reap_workers();
    assert(kill_calls == 1 && registry.runtime[1].state == UPF_WORKER_FAILED);
    exit_ready = 1; exit_status = SIGKILL; reap_workers(); advance_stop();
    assert(stopping < 0 && registry.runtime[1].state == UPF_WORKER_FAILED && !registry.runtime[1].reader_generation);
    assert(start_worker() == -ENOSPC);

    setup(); delete_to_cleanup();
    dispatch_deletion(message()); assert(!responses && !frees && session_count[1] == 1);
    Bufblk *duplicate = message(); assert(UpfScalingDelete(duplicate, &xact, &session) == -EBUSY); BufblkFree(duplicate);
    cleanup_session(); assert(!purges); /* Owner has not adopted the retiring classifier yet. */
    g_cls_local.ver = 4; cleanup_session(); assert(purges == 1);
    cleanup_session(); assert(purges == 1); /* ACK is idempotent. */
    advance_request(&requests[0]); assert(frees == 1 && !session_count[1] && !responses);
    cache_error = 1; advance_request(&requests[0]); assert(frees == 1 && requests[0].message);
    cache_error = 0; advance_request(&requests[0]);
    assert(responses == 1 && xact.cached_cause == PFCP_CAUSE_REQUEST_ACCEPTED && xact.cached_seid == 20);
    assert(!requests[0].message && !xact.applicationPending);
    dispatch_deletion(message()); assert(responses == 2 && frees == 1);
    assert(xact.cached_cause == PFCP_CAUSE_REQUEST_ACCEPTED && xact.cached_seid == 20);
    xact.step = 0; memset(xact.seq, 0, sizeof(xact.seq)); /* New transaction, genuinely missing session. */
    dispatch_deletion(message());
    assert(responses == 3 && xact.cached_cause == PFCP_CAUSE_SESSION_CONTEXT_NOT_FOUND && !xact.cached_seid);

    setup(); delete_to_cleanup(); session.upfSeid++; g_cls_local.ver = 4;
    cleanup_session(); advance_request(&requests[0]);
    assert(!purges && !frees && session_count[1] == 1 && requests[0].stage == QUARANTINED);
    assert(xact.cached_cause == PFCP_CAUSE_SYSTEM_FAILURE);
    setup(); delete_to_cleanup(); now += 60001; advance_request(&requests[0]);
    assert(!frees && session_count[1] == 1 && requests[0].cleanup_sequence);
    g_cls_local.ver = 4; cleanup_session(); advance_request(&requests[0]);
    assert(requests[0].stage == QUARANTINED && !frees);

    setup();
    assert(UpfScalingEnqueue(message(), &xact) == 0); advance_request(&requests[0]);
    assert(session_count[0] == 1);
    reject(&requests[0], PFCP_CAUSE_SYSTEM_FAILURE); advance_request(&requests[0]);
    ack_steer(0); advance_request(&requests[0]); advance_request(&requests[0]);
    safe_version = 4; advance_request(&requests[0]);
    assert(frees == 1 && !session_count[0] && !requests[0].message);
    setup();
    puts("PASS: hold policy, deletion/cleanup ACKs, rollback counts, shutdown ordering and slot reuse");
    return 0;
}
