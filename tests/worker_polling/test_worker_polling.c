/* SPDX-License-Identifier: Apache-2.0 */
#include <sched.h>
#include "manager.h"
#include "../../../onvm/upf/upf_worker.c"
#include "../../../onvm/onvm_mgr/onvm_upf.c"
#include "../../../onvm/onvm_nflib/onvm_pkt_common.c"
#include "../../../onvm/onvm_mgr/onvm_pkt.c"
#include "rx_loop.inc"
#include "tx_loop.inc"

static UpfWorkerRegistry registry;
static struct rte_ring rx[MAX_NFS], tx[MAX_NFS];
static struct queue_mgr rx_manager = {.mgr_type_t = MGR};

static void sync_manager(void) {
    rte_spinlock_lock(&onvm_upf_lock);
    onvm_upf_sync();
    rte_spinlock_unlock(&onvm_upf_lock);
}

static void rx_pass(void) {
    memset(nic_calls, 0, sizeof(nic_calls));
    worker_keep_running = 1;
    assert(rx_thread_main(&rx_manager) == 0);
}

static uint32_t request(uint16_t slot, uint16_t instance, uint32_t generation, uint8_t enable) {
    UpfWorkerPollUpdate update = {.slot = slot, .instance_id = instance, .generation = generation, .enable = enable};
    uint32_t sequence = 0;
    assert(UpfWorkerPollRequest(&update, &sequence) == 0 && sequence);
    int32_t result = 1234;
    assert(UpfWorkerPollResult(sequence, &result) == -EINPROGRESS && result == 1234);
    uint32_t second = 0;
    assert(UpfWorkerPollRequest(&update, &second) == -EBUSY && !second);
    return sequence;
}

static void result(uint32_t sequence, int expected) {
    int32_t status = 1234;
    assert(UpfWorkerPollResult(sequence, &status) == 0 && status == expected);
}

static void update(uint16_t slot, uint16_t instance, uint32_t generation, uint8_t enable, int expected) {
    uint32_t sequence = request(slot, instance, generation, enable);
    sync_manager();
    result(sequence, expected);
}

static void setup(void) {
    g_upf_workers = &registry;
    registry.service_limit = MAX_SERVICES;
    for (unsigned i = 0; i < MAX_SERVICES; i++) services[i] = service_data[i];
    for (unsigned i = 0; i < 6; i++) { ports->id[i] = i; ports->init[i] = 1; }
    ports->num_ports = 6;
    for (unsigned i = 0; i < MAX_NFS; i++) {
        rx[i].capacity = tx[i].capacity = 128;
        nfs[i] = (struct onvm_nf){.instance_id = i, .service_id = i, .valid = 1, .rx_q = &rx[i], .tx_q = &tx[i]};
    }
    nfs[7].service_id = 14; nfs[7].thread_info.core = 3;
    nfs[8].service_id = 15; nfs[8].thread_info.core = 4;
    services[14][0] = 7; services[15][0] = 8; services[5][0] = 5;
    nf_per_service_count[14] = nf_per_service_count[15] = nf_per_service_count[5] = 1;
}

/* UPF-C and RX run concurrently. An ACK must expose both mappings and must
 * never be confused with the preceding update's success/failure result.
 */
static int stress_done;
static void *manager_thread(void *unused) {
    (void)unused;
    while (!__atomic_load_n(&stress_done, __ATOMIC_ACQUIRE)) {
        sync_manager();
        sched_yield();
    }
    return NULL;
}

static void concurrent_updates(void) {
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, manager_thread, NULL));
    for (uint32_t generation = 10; generation < 1010; generation++) {
        for (int step = 0; step < 3; step++) {
            int enable = step != 2;
            UpfWorkerPollUpdate change = {.slot = 0, .instance_id = 7,
                .generation = generation + (step == 1), .enable = enable};
            uint32_t sequence;
            assert(UpfWorkerPollRequest(&change, &sequence) == 0);
            int32_t status = 1234;
            int pending;
            while ((pending = UpfWorkerPollResult(sequence, &status)) == -EINPROGRESS) sched_yield();
            assert(pending == 0 && status == (step == 1 ? -EBUSY : 0));
            rte_spinlock_lock(&onvm_upf_lock);
            assert(onvm_upf_port_destination(0) == (enable ? 7 : -1));
            assert(onvm_upf_port_destination(2) == (enable ? 7 : -1));
            rte_spinlock_unlock(&onvm_upf_lock);
        }
    }
    __atomic_store_n(&stress_done, 1, __ATOMIC_RELEASE);
    assert(!pthread_join(thread, NULL));
}

int main(int argc, char **argv) {
    (void)argv;
    setup();
    UpfScalingConfig pool = {.slot_count = 2, .min_workers = 1, .max_workers = 2};
    pool.slots[0] = (UpfWorkerSlotConfig){.service_id = 14, .core = 3, .n3_port = 0, .n6_port = 2};
    pool.slots[1] = (UpfWorkerSlotConfig){.service_id = 15, .core = 4, .n3_port = 1, .n6_port = 3};
    UpfWorkerPollUpdate change = {.slot = 0, .instance_id = 7, .generation = 1, .enable = 1};
    uint32_t sequence;
    assert(UpfWorkerPollRequest(&change, &sequence) == -ENOENT);
    if (argc > 1) {
        pool.slots[1].n6_port = 0;
        assert(UpfWorkerRegistryPublish(&pool) == 0);
        sync_manager();
        assert(UpfWorkerPollingStatus() == -EINVAL);
        assert(UpfWorkerPollRequest(&change, &sequence) == -EINVAL);
        assert(onvm_upf_port_destination(0) == -1);
        puts("PASS: invalid manager configuration fails closed");
        return 0;
    }

    /* Legacy mode still polls every port and uses its service map. */
    struct rte_mbuf legacy = {.port = 0}; next_rx[0] = &legacy;
    rx_pass();
    assert(!onvm_upf_is_dynamic() && rx[7].count == 1);
    for (unsigned i = 0; i < 6; i++) assert(nic_calls[i] == 1);
    rx[7].count = 0;
    num_services = 10;
    struct rte_mbuf out_of_range = {.port = 0}; next_rx[0] = &out_of_range;
    rx_pass(); assert(out_of_range.freed); num_services = MAX_SERVICES;
    onvm_port_to_upf_service[4] = 14; /* Must be ignored after dynamic publication. */
    assert(UpfWorkerRegistryPublish(&pool) == 0);
    assert(UpfWorkerPollRequest(&change, &sequence) == -EAGAIN);
    rx_pass();
    assert(UpfWorkerPollingStatus() == 1 && onvm_upf_is_dynamic());
    for (unsigned i = 0; i < 4; i++) assert(nic_calls[i] == 0 && onvm_upf_port_destination(i) == -1);
    assert(nic_calls[4] == 1 && nic_calls[5] == 1);
    assert(ports->num_ports == 6 && ports->init[0] && ports->init[2]);

    nfs[7].valid = 0; update(0, 7, 1, 1, -EAGAIN); nfs[7].valid = 1;
    nfs[7].rx_q = NULL; update(0, 7, 1, 1, -EAGAIN); nfs[7].rx_q = &rx[7];
    nfs[7].tx_q = NULL; update(0, 7, 1, 1, -EAGAIN); nfs[7].tx_q = &tx[7];
    nfs[7].service_id = 13; update(0, 7, 1, 1, -EINVAL); nfs[7].service_id = 14;
    nfs[7].thread_info.core = 4; update(0, 7, 1, 1, -EINVAL); nfs[7].thread_info.core = 3;
    nf_per_service_count[14] = 2; update(0, 7, 1, 1, -EINVAL); nf_per_service_count[14] = 1;
    services[14][0] = 14; update(0, 7, 1, 1, -EINVAL); services[14][0] = 7;
    update(0, MAX_NFS, 1, 1, -EINVAL);
    assert(onvm_upf_port_destination(0) == -1 && onvm_upf_port_destination(2) == -1);

    /* The ACK happens at the RX boundary before packets use either port. */
    sequence = request(0, 7, 1, 1);
    assert(onvm_upf_port_destination(0) == -1);
    struct rte_mbuf packets[4] = {{.port = 0}, {.port = 2}, {.port = 4}, {.port = 1}};
    for (unsigned i = 0; i < 3; i++) {
        memset(&packets[i].meta, 0xff, sizeof(packets[i].meta));
        next_rx[packets[i].port] = &packets[i];
    }
    chain_calls = service_calls = 0;
    rx_pass(); result(sequence, 0);
    assert(nic_calls[0] && nic_calls[2] && !nic_calls[1] && !nic_calls[3]);
    assert(rx[7].count == 2 && rx[7].pkts[0] == &packets[0] && rx[7].pkts[1] == &packets[1]);
    assert(rx[14].count == 0 && rx[5].count == 1 && rx[5].pkts[0] == &packets[2]);
    assert(chain_calls == 1 && service_calls == 1);
    assert(packets[0].meta.destination == 14 && !packets[0].meta.chain_index && !packets[0].meta.flags);
    struct rte_mbuf *batch[] = {&packets[3]};
    onvm_pkt_process_rx_batch(&rx_manager, batch, 1); assert(packets[3].freed && chain_calls == 1);

    rx[7].capacity = rx[7].count;
    struct rte_mbuf full = {.port = 0}; next_rx[0] = &full;
    rx_pass(); assert(full.freed && nfs[7].stats.rx_drop == 1); rx[7].capacity = 128;
    update(1, 8, 1, 1, 0);
    rx_pass(); for (unsigned i = 0; i < 6; i++) assert(nic_calls[i]);
    update(0, 7, 1, 1, 0); /* Duplicate activation is idempotent. */
    update(0, 7, 2, 1, -EBUSY);
    update(0, 7, 2, 0, -ESTALE);
    update(0, 7, 1, 0, 0);
    update(0, 7, 1, 0, 0); /* Duplicate disable is idempotent. */
    update(0, 7, 1, 1, -ESTALE);
    assert(onvm_upf_port_destination(1) == 8 && onvm_upf_port_destination(3) == 8);
    registry.runtime[0].state = UPF_WORKER_STOPPING;
    update(0, 7, 2, 1, -ECANCELED);
    registry.runtime[0].state = UPF_WORKER_STARTING;
    struct rte_mbuf stale_n3 = {.port = 0}, stale_n6 = {.port = 2};
    next_rx[0] = &stale_n3; next_rx[2] = &stale_n6;
    unsigned previous_rx = rx[7].count;
    update(0, 7, 2, 1, 0);
    assert(stale_n3.freed && stale_n6.freed && rx[7].count == previous_rx);
    /* Manager STOP invalidates the mapping, even if the instance ID is reused. */
    rte_spinlock_lock(&onvm_upf_lock); onvm_upf_forget_nf(7); rte_spinlock_unlock(&onvm_upf_lock);
    rx_pass(); assert(!nic_calls[0] && !nic_calls[2] && nic_calls[1] && nic_calls[3]);
    update(0, 7, 2, 1, -ESTALE);
    update(0, 7, 3, 1, 0);
    update(0, 7, 3, 0, 0);

    /* Outbound worker packets still traverse the manager's ordinary TX path. */
    struct rte_mbuf output = {.meta = {.action = ONVM_NF_ACTION_OUT, .destination = 0}};
    struct rte_mbuf *outputs[] = {&output};
    struct tx_info txinfo = {0}; struct queue_mgr tx_manager = {.mgr_type_t = MGR, .tx_thread_info = &txinfo};
    assert(rte_ring_enqueue_bulk(&tx[7], (void **)outputs, 1, NULL) == 1);
    txinfo.first_nf = 7; txinfo.last_nf = 8;
    worker_keep_running = 1;
    assert(tx_thread_main(&tx_manager) == 0);
    assert(tx_calls == 1 && !output.freed && !tx[7].count);
    onvm_upf_nf_lock();
    assert(registry.nf_lock && pthread_rwlock_tryrdlock(&onvm_upf_tx_lock) != 0);
    onvm_upf_nf_unlock();

    concurrent_updates();
    int32_t status;
    assert(UpfWorkerPollResult(sequence, &status) == -ESTALE);
    assert(UpfWorkerPollResult(0, &status) == -EINVAL);
    assert(UpfWorkerPollRequest(NULL, &sequence) == -EINVAL);
    change.slot = 2; assert(UpfWorkerPollRequest(&change, &sequence) == -EINVAL); change.slot = 0;
    change.generation = 0; assert(UpfWorkerPollRequest(&change, &sequence) == -EINVAL); change.generation = 2000;
    registry.poll_request_seq = registry.poll_ack_seq = UINT32_MAX;
    assert(UpfWorkerPollRequest(&change, &sequence) == -EOVERFLOW);
    puts("PASS: activation ACKs, RX polling/dispatch, rollback, stale requests, legacy/TX paths, concurrent mailbox");
    return 0;
}
