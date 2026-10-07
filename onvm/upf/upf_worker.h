/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UPF_WORKER_H
#define UPF_WORKER_H

#include <stdint.h>
#include <netinet/in.h>
#include <net/if.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UPF_MAX_WORKERS 32
#define UPF_WORKER_PATH_LEN 512
#define MZ_UPF_WORKERS "UPF_WORKERS"
#define UPF_WORKERS_ABI_VERSION 7
#define UPF_MAX_SESSION_RULES 1024
#define UPF_MAX_QUEUE_WINDOW_SAMPLES 1024

/* A configured slot reserves resources; its array index is the slot ID.
 * Ports are DPDK port IDs. VF indices are relative to their respective PF.
 * IP addresses use network byte order; other numeric fields use host order.
 */
typedef struct {
    uint16_t service_id;
    uint16_t core;
    uint16_t n3_port;
    uint16_t n6_port;
    uint16_t n3_vf;
    uint16_t n6_vf;
    struct in_addr n3_addr;
    struct in_addr n6_addr;
} UpfWorkerSlotConfig;

typedef struct {
    uint16_t slot_count;         /* Zero disables dynamic worker mode. */
    uint16_t min_workers;
    uint16_t max_workers;
    uint32_t rx_queue_threshold; /* Scale out when all READY RX averages exceed it. */
    uint32_t queue_sample_interval_ms;
    uint32_t queue_window_samples; /* Sliding average, 1..UPF_MAX_QUEUE_WINDOW_SAMPLES. */
    uint32_t scale_down_queue_threshold;
    uint32_t scale_down_hold_ms;
    uint32_t worker_stop_timeout_ms;
    uint32_t teid_first;         /* Per-session allocation range, not per-worker. */
    uint32_t teid_last;
    char worker_binary[UPF_WORKER_PATH_LEN];
    char n3_pf[IF_NAMESIZE];
    char n6_pf[IF_NAMESIZE];
    char file_prefix[64];        /* Same EAL namespace as manager and UPF-C. */
    struct in_addr n3_peer_addr;
    struct in_addr n6_peer_addr;
    uint32_t startup_timeout_ms;
    UpfWorkerSlotConfig slots[UPF_MAX_WORKERS];
} UpfScalingConfig;

typedef enum {
    UPF_WORKER_INACTIVE = 0,
    UPF_WORKER_STARTING,
    UPF_WORKER_READY,
    UPF_WORKER_STOPPING,
    UPF_WORKER_FAILED,
} UpfWorkerState;

typedef struct {
    uint64_t seid;
    uint32_t session_index, generation, version;
} UpfSessionCleanup;

typedef struct {
    uint32_t state;              /* Atomic publication point; UPF-C is sole writer. */
    uint32_t generation;         /* Distinguishes successive starts of a slot. */
    uint16_t instance_id;        /* Assigned by manager; zero until registered. */
    uint32_t reader_generation;  /* UPF-C registers before spawn; clears only after exit. */
    uint32_t registered_instance;/* UPF-U publishes after attaching its slot. */
    uint32_t ack_version;        /* UPF-U publishes at a classifier quiescent point. */
    uint32_t ack_generation;
    UpfSessionCleanup cleanup_request;
    uint32_t cleanup_request_seq, cleanup_consumed_seq; /* UPF-C owns requests. */
    int32_t cleanup_result;                            /* UPF-U owns results. */
    uint32_t cleanup_ack_seq;
} UpfWorkerRuntime;

typedef struct {
    uint32_t generation;         /* Nonzero, increases on each start of a slot. */
    uint16_t slot;
    uint16_t instance_id;
    uint8_t enable;              /* Zero disables polling for rollback/shutdown. */
} UpfWorkerPollUpdate;

typedef enum {
    UPF_STEER_PROBE = 1,
    UPF_STEER_N3_ADD,
    UPF_STEER_SESSION_ADD,
    UPF_STEER_SESSION_DEL,
    UPF_STEER_N3_DEL,
} UpfSteerOperation;

typedef struct {
    uint32_t operation;
    uint32_t generation;
    uint32_t session_index;
    uint16_t slot;
    struct in_addr ue_addr;     /* Network byte order. */
} UpfSteerUpdate;

/* Manager creates the registry. UPF-C publishes configuration once per run.
 * The shared structures contain values only, never process-private pointers.
 * Readers must acquire configured before accessing config. UPF-C owns runtime
 * except the worker registration/ACK fields. Manager RX owns polling_status and
 * polling ACKs; manager master owns steering ACKs. Each mailbox has one UPF-C
 * producer and at most one outstanding request.
 */
typedef struct {
    uint32_t abi_version;
    uint32_t configured;
    uint16_t service_limit;      /* Manager's configured service-ID bound. */
    UpfScalingConfig config;
    UpfWorkerRuntime runtime[UPF_MAX_WORKERS];
    int32_t polling_status;      /* 0 pending, 1 installed, negative errno. */
    UpfWorkerPollUpdate poll_request;
    uint32_t poll_request_seq;   /* Release publishes request fields. */
    int32_t poll_result;         /* 0 success or negative errno. */
    uint32_t poll_ack_seq;       /* Release publishes result and applied map. */
    uint32_t nf_lock;            /* Protects NF/ring lifetime during UPF-C queue reads. */
    UpfSteerUpdate steer_request;
    uint32_t steer_request_seq;
    int32_t steer_result;
    uint32_t steer_ack_seq;
} UpfWorkerRegistry;

extern UpfWorkerRegistry *g_upf_workers;

int UpfWorkerRegistryCreate(uint16_t service_limit); /* Manager only, before NFs. */
int UpfWorkerRegistryAttach(void);  /* Secondary processes; never creates. */
int UpfWorkerRegistryPublish(const UpfScalingConfig *config); /* UPF-C only. */

/* UPF-C event-loop API: never waits. Do not reuse the mailbox until Result
 * completes. An unacknowledged request must not be cancelled by overwriting it.
 */
int UpfWorkerPollRequest(const UpfWorkerPollUpdate *update, uint32_t *sequence);
int UpfWorkerPollResult(uint32_t sequence, int32_t *result); /* -EINPROGRESS while pending. */
int UpfSteerRequest(const UpfSteerUpdate *update, uint32_t *sequence);
int UpfSteerResult(uint32_t sequence, int32_t *result);
int UpfWorkerCleanupRequest(uint16_t slot, const UpfSessionCleanup *update, uint32_t *sequence);
int UpfWorkerCleanupResult(uint16_t slot, uint32_t sequence, int32_t *result);

static inline int UpfWorkerNfTryLock(void) {
    return g_upf_workers && !__atomic_exchange_n(&g_upf_workers->nf_lock, 1, __ATOMIC_ACQUIRE);
}
static inline void UpfWorkerNfUnlock(void) {
    __atomic_store_n(&g_upf_workers->nf_lock, 0, __ATOMIC_RELEASE);
}

static inline int UpfWorkerPollingStatus(void) {
    return g_upf_workers ? __atomic_load_n(&g_upf_workers->polling_status, __ATOMIC_ACQUIRE) : 0;
}

static inline int UpfWorkerRegistryIsConfigured(void) {
    return g_upf_workers && __atomic_load_n(&g_upf_workers->configured, __ATOMIC_ACQUIRE);
}

#ifdef __cplusplus
}
#endif
#endif
