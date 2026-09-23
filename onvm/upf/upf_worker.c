/* SPDX-License-Identifier: Apache-2.0 */
#include "upf_worker.h"

#include <errno.h>
#include <string.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_memzone.h>

UpfWorkerRegistry *g_upf_workers;

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
