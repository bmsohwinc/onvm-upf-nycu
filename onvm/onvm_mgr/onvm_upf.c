/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>

#include "onvm_mgr.h"
#include "onvm_upf.h"
#include "upf_worker.h"

#if ONVM_NUM_RX_THREADS != 1
#error "UPF polling activation requires one manager RX thread"
#endif

rte_spinlock_t onvm_upf_lock = RTE_SPINLOCK_INITIALIZER;
rte_rwlock_t onvm_upf_tx_lock = RTE_RWLOCK_INITIALIZER;

void onvm_upf_nf_lock(void) {
    rte_rwlock_write_lock(&onvm_upf_tx_lock);
    rte_spinlock_lock(&onvm_upf_lock);
    while (!UpfWorkerNfTryLock()) rte_pause();
}
void onvm_upf_nf_unlock(void) {
    UpfWorkerNfUnlock();
    rte_spinlock_unlock(&onvm_upf_lock);
    rte_rwlock_write_unlock(&onvm_upf_tx_lock);
}

static int dynamic_mode;
static int config_status;
static uint16_t port_slot[RTE_MAX_ETHPORTS]; /* Slot + 1; zero is an ordinary port. */
static struct {
    uint32_t generation;
    uint16_t instance_id;
    uint8_t active;
} routes[UPF_MAX_WORKERS];

static int install_config(void) {
    const UpfScalingConfig *config = &g_upf_workers->config;
    if (!config->slot_count || config->slot_count > UPF_MAX_WORKERS)
        return -EINVAL;
    /* Check the whole map before changing any entry. */
    for (uint16_t i = 0; i < config->slot_count; i++) {
        const UpfWorkerSlotConfig *s = &config->slots[i];
        if (!s->service_id || s->service_id >= num_services || s->service_id >= MAX_SERVICES ||
            s->n3_port >= RTE_MAX_ETHPORTS || s->n6_port >= RTE_MAX_ETHPORTS ||
            !ports->init[s->n3_port] || !ports->init[s->n6_port] || s->n3_port == s->n6_port)
            return -EINVAL;
        for (uint16_t j = 0; j < i; j++) {
            const UpfWorkerSlotConfig *other = &config->slots[j];
            if (s->service_id == other->service_id || s->n3_port == other->n3_port ||
                s->n3_port == other->n6_port || s->n6_port == other->n3_port || s->n6_port == other->n6_port)
                return -EINVAL;
        }
    }
    for (uint16_t i = 0; i < config->slot_count; i++) {
        port_slot[config->slots[i].n3_port] = i + 1;
        port_slot[config->slots[i].n6_port] = i + 1;
    }
    return 1;
}

/* A reused VF may still contain packets from its previous worker. Bounded
 * cleanup runs on its sole RX thread, before publishing the new route. */
static int discard_rx(uint16_t port) {
    struct rte_mbuf *packets[PACKET_READ_SIZE];
    for (unsigned burst = 0; burst < 128; burst++) {
        uint16_t count = rte_eth_rx_burst(port, 0, packets, PACKET_READ_SIZE);
        for (uint16_t i = 0; i < count; i++) rte_pktmbuf_free(packets[i]);
        if (!count) return 0;
    }
    return -EBUSY;
}

static int apply_update(const UpfWorkerPollUpdate *update) {
    if (update->slot >= g_upf_workers->config.slot_count || !update->generation ||
        !update->instance_id || update->instance_id >= MAX_NFS || update->enable > 1)
        return -EINVAL;
    uint16_t slot = update->slot;
    int same_start = routes[slot].generation == update->generation &&
                     routes[slot].instance_id == update->instance_id;
    if (!update->enable) {
        if (!same_start)
            return -ESTALE;
        routes[slot].active = 0;
        return 0;
    }
    uint32_t state = __atomic_load_n(&g_upf_workers->runtime[slot].state, __ATOMIC_ACQUIRE);
    if (state == UPF_WORKER_FAILED || state == UPF_WORKER_STOPPING)
        return -ECANCELED;
    if (routes[slot].active && !same_start)
        return -EBUSY;
    if (!routes[slot].active && update->generation <= routes[slot].generation)
        return -ESTALE;

    const UpfWorkerSlotConfig *s = &g_upf_workers->config.slots[slot];
    struct onvm_nf *nf = &nfs[update->instance_id];
    if (!onvm_nf_is_valid(nf) || !nf->rx_q || !nf->tx_q)
        return -EAGAIN;
    if (nf->instance_id != update->instance_id || nf->service_id != s->service_id ||
        nf->thread_info.core != s->core || nf_per_service_count[s->service_id] != 1 ||
        services[s->service_id][0] != update->instance_id)
        return -EINVAL;

    if (!routes[slot].active && routes[slot].generation &&
        (discard_rx(s->n3_port) || discard_rx(s->n6_port))) return -EBUSY;
    routes[slot].generation = update->generation;
    routes[slot].instance_id = update->instance_id;
    routes[slot].active = 1;
    return 0;
}

void onvm_upf_sync(void) {
    if (!dynamic_mode) {
        if (!UpfWorkerRegistryIsConfigured())
            return;
        dynamic_mode = 1;
        config_status = install_config();
        __atomic_store_n(&g_upf_workers->polling_status, config_status, __ATOMIC_RELEASE);
        RTE_LOG(INFO, APP, "UPF polling configuration: %d (1 = installed, all slots inactive)\n", config_status);
    }
    if (config_status != 1)
        return;
    for (uint16_t i = 0; i < g_upf_workers->config.slot_count; i++)
        if (__atomic_load_n(&g_upf_workers->runtime[i].state, __ATOMIC_ACQUIRE) == UPF_WORKER_FAILED)
            routes[i].active = 0;
    uint32_t sequence = __atomic_load_n(&g_upf_workers->poll_request_seq, __ATOMIC_ACQUIRE);
    if (sequence == __atomic_load_n(&g_upf_workers->poll_ack_seq, __ATOMIC_RELAXED))
        return;
    g_upf_workers->poll_result = apply_update(&g_upf_workers->poll_request);
    /* Both ports resolve through one slot entry. All prior RX batches have
     * finished; publish the ACK after installing the next pass's mapping.
     */
    __atomic_store_n(&g_upf_workers->poll_ack_seq, sequence, __ATOMIC_RELEASE);
}

void onvm_upf_forget_nf(uint16_t instance_id) {
    for (uint16_t i = 0; i < UPF_MAX_WORKERS; i++) {
        if (routes[i].instance_id == instance_id)
            routes[i].active = 0;
    }
}

int onvm_upf_is_dynamic(void) {
    return dynamic_mode;
}

int onvm_upf_port_destination(uint16_t port) {
    if (port >= RTE_MAX_ETHPORTS || (dynamic_mode && config_status != 1))
        return -1;
    if (!port_slot[port])
        return 0;
    uint16_t slot = port_slot[port] - 1;
    return routes[slot].active ? routes[slot].instance_id : -1;
}
