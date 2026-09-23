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
#define UPF_WORKERS_ABI_VERSION 1

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
    uint32_t rx_queue_threshold; /* Read on session arrival, not on a timer. */
    uint32_t teid_first;         /* Per-session allocation range, not per-worker. */
    uint32_t teid_last;
    char worker_binary[UPF_WORKER_PATH_LEN];
    char n3_pf[IF_NAMESIZE];
    char n6_pf[IF_NAMESIZE];
    UpfWorkerSlotConfig slots[UPF_MAX_WORKERS];
} UpfScalingConfig;

typedef enum {
    UPF_WORKER_INACTIVE = 0,
    UPF_WORKER_STARTING,
    UPF_WORKER_READY,
    UPF_WORKER_FAILED,
} UpfWorkerState;

typedef struct {
    uint32_t state;              /* Atomic publication point; UPF-C is sole writer. */
    uint32_t generation;         /* Distinguishes successive starts of a slot. */
    uint16_t instance_id;        /* Assigned by manager; zero until registered. */
} UpfWorkerRuntime;

/* Manager creates the registry. UPF-C publishes configuration once per run.
 * The shared structures contain values only, never process-private pointers.
 * Readers must acquire configured before accessing config/runtime. Runtime
 * transitions and registration will be implemented with worker activation.
 */
typedef struct {
    uint32_t abi_version;
    uint32_t configured;
    uint16_t service_limit;      /* Manager's configured service-ID bound. */
    UpfScalingConfig config;
    UpfWorkerRuntime runtime[UPF_MAX_WORKERS];
} UpfWorkerRegistry;

extern UpfWorkerRegistry *g_upf_workers;

int UpfWorkerRegistryCreate(uint16_t service_limit); /* Manager only, before NFs. */
int UpfWorkerRegistryAttach(void);  /* Secondary processes; never creates. */
int UpfWorkerRegistryPublish(const UpfScalingConfig *config); /* UPF-C only. */

static inline int UpfWorkerRegistryIsConfigured(void) {
    return g_upf_workers && __atomic_load_n(&g_upf_workers->configured, __ATOMIC_ACQUIRE);
}

#ifdef __cplusplus
}
#endif
#endif
