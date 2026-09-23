/* SPDX-License-Identifier: Apache-2.0 */
#include "upf_worker.h"

#include <errno.h>
#include <string.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_memzone.h>

UpfWorkerRegistry *g_upf_workers;
static uint32_t steer_consumed;

int UpfWorkerRegistryCreate(uint16_t service_limit) {
    if (rte_eal_process_type() != RTE_PROC_PRIMARY)
        return -EPERM;
    if (!service_limit)
        return -EINVAL;
    const struct rte_memzone *mz = rte_memzone_reserve(
        MZ_UPF_WORKERS, sizeof(UpfWorkerRegistry), SOCKET_ID_ANY, 0);
    if (!mz)
        return -rte_errno;
    g_upf_workers = mz->addr;
    memset(g_upf_workers, 0, sizeof(*g_upf_workers));
    g_upf_workers->abi_version = UPF_WORKERS_ABI_VERSION;
    g_upf_workers->service_limit = service_limit;
    return 0;
}

int UpfWorkerPollRequest(const UpfWorkerPollUpdate *update, uint32_t *sequence) {
    if (!UpfWorkerRegistryIsConfigured())
        return -ENOENT;
    int status = UpfWorkerPollingStatus();
    if (status != 1)
        return status < 0 ? status : -EAGAIN;
    if (!update || !sequence || update->slot >= g_upf_workers->config.slot_count ||
        !update->generation || !update->instance_id || update->enable > 1)
        return -EINVAL;
    uint32_t previous = __atomic_load_n(&g_upf_workers->poll_request_seq, __ATOMIC_RELAXED);
    if (__atomic_load_n(&g_upf_workers->poll_ack_seq, __ATOMIC_ACQUIRE) != previous)
        return -EBUSY;
    if (previous == UINT32_MAX)
        return -EOVERFLOW;
    g_upf_workers->poll_request = *update;
    *sequence = previous + 1;
    __atomic_store_n(&g_upf_workers->poll_request_seq, *sequence, __ATOMIC_RELEASE);
    return 0;
}

int UpfWorkerPollResult(uint32_t sequence, int32_t *result) {
    if (!g_upf_workers)
        return -ENOENT;
    if (!sequence || !result)
        return -EINVAL;
    if (__atomic_load_n(&g_upf_workers->poll_request_seq, __ATOMIC_RELAXED) != sequence)
        return -ESTALE;
    if (__atomic_load_n(&g_upf_workers->poll_ack_seq, __ATOMIC_ACQUIRE) != sequence)
        return -EINPROGRESS;
    *result = g_upf_workers->poll_result;
    return 0;
}

int UpfWorkerRegistryAttach(void) {
    const struct rte_memzone *mz = rte_memzone_lookup(MZ_UPF_WORKERS);
    if (!mz)
        return -ENOENT;
    if (mz->len < sizeof(UpfWorkerRegistry))
        return -EPROTO;
    UpfWorkerRegistry *registry = mz->addr;
    if (registry->abi_version != UPF_WORKERS_ABI_VERSION)
        return -EPROTO;
    g_upf_workers = registry;
    return 0;
}

int UpfSteerRequest(const UpfSteerUpdate *update, uint32_t *sequence) {
    if (!UpfWorkerRegistryIsConfigured()) return -ENOENT;
    if (!update || !sequence || update->operation < UPF_STEER_PROBE ||
        update->operation > UPF_STEER_SESSION_DEL || update->slot >= g_upf_workers->config.slot_count)
        return -EINVAL;
    uint32_t previous = __atomic_load_n(&g_upf_workers->steer_request_seq, __ATOMIC_RELAXED);
    if (previous != steer_consumed) return -EBUSY;
    if (__atomic_load_n(&g_upf_workers->steer_ack_seq, __ATOMIC_ACQUIRE) != previous) return -EBUSY;
    if (previous == UINT32_MAX) return -EOVERFLOW;
    g_upf_workers->steer_request = *update;
    *sequence = previous + 1;
    __atomic_store_n(&g_upf_workers->steer_request_seq, *sequence, __ATOMIC_RELEASE);
    return 0;
}

int UpfSteerResult(uint32_t sequence, int32_t *result) {
    if (!g_upf_workers) return -ENOENT;
    if (!sequence || !result) return -EINVAL;
    if (__atomic_load_n(&g_upf_workers->steer_request_seq, __ATOMIC_RELAXED) != sequence) return -ESTALE;
    if (__atomic_load_n(&g_upf_workers->steer_ack_seq, __ATOMIC_ACQUIRE) != sequence) return -EINPROGRESS;
    *result = g_upf_workers->steer_result;
    steer_consumed = sequence;
    return 0;
}

int UpfWorkerRegistryPublish(const UpfScalingConfig *config) {
    if (!g_upf_workers)
        return -ENOENT;
    if (!config || !config->slot_count || config->slot_count > UPF_MAX_WORKERS ||
        !config->min_workers || config->min_workers > config->max_workers ||
        config->max_workers > config->slot_count)
        return -EINVAL;
    if (UpfWorkerRegistryIsConfigured())
        return -EALREADY;

    /* All slots remain INACTIVE. Publishing config does not start processes,
     * enable port polling, or register classifier readers.
     */
    g_upf_workers->config = *config;
    __atomic_store_n(&g_upf_workers->configured, 1, __ATOMIC_RELEASE);
    return 0;
}
