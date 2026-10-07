/* Only the DPDK/ONVM environment is mocked; tests include the production C files. */
#ifndef TEST_MANAGER_H
#define TEST_MANAGER_H
#define _ONVM_MGR_H_
#define _ONVM_NF_H_
#define _ONVM_PKT_COMMON_H_
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <rte_eal.h>
#include <rte_memzone.h>
#include <rte_rwlock.h>
extern rte_rwlock_t onvm_upf_tx_lock;

#define RTE_MAX_ETHPORTS 32
#define MAX_NFS 128
#define MAX_SERVICES 32
#define PACKET_READ_SIZE 32
#define ONVM_NUM_RX_THREADS 1
#define NF 1
#define MGR 2
#define ONVM_NF_ACTION_DROP 0
#define ONVM_NF_ACTION_TONF 1
#define ONVM_NF_ACTION_OUT 2
#define ONVM_NF_ACTION_NEXT 3
#define likely(x) (x)
#define unlikely(x) (x)
#define RTE_LOG(...) ((void)0)
#define ONVM_EVENT_WITH_CORE 0

struct onvm_pkt_meta { uint8_t action; uint16_t destination, src; uint8_t chain_index, flags; };
struct rte_mbuf { uint16_t port; struct onvm_pkt_meta meta; unsigned freed; };
struct rte_ring { void *pkts[128]; unsigned count, capacity; };
struct packet_buf { struct rte_mbuf *buffer[PACKET_READ_SIZE]; uint16_t count; };
struct tx_info { struct packet_buf port_tx_bufs[RTE_MAX_ETHPORTS]; unsigned first_nf, last_nf; };
struct queue_mgr { int mgr_type_t, id; struct packet_buf nf_rx_bufs[MAX_NFS], *to_tx_buf; struct tx_info *tx_thread_info; };
struct onvm_nf {
    uint16_t instance_id, service_id;
    int valid;
    struct { uint16_t core; } thread_info;
    struct rte_ring *rx_q, *tx_q;
    struct { unsigned rx, rx_drop, tx, tx_drop, tx_buffer, tx_returned, act_out, act_drop, act_next, act_tonf; } stats;
} nfs[MAX_NFS];
struct tx_stats { unsigned tx[RTE_MAX_ETHPORTS], tx_drop[RTE_MAX_ETHPORTS]; };
struct port_info {
    uint16_t num_ports, id[RTE_MAX_ETHPORTS];
    int init[RTE_MAX_ETHPORTS];
    struct tx_stats tx_stats;
    struct { unsigned rx[RTE_MAX_ETHPORTS]; } rx_stats;
} port_data, *ports = &port_data;
uint16_t num_services = MAX_SERVICES, num_nfs = 1;
uint16_t service_data[MAX_SERVICES][MAX_NFS], *services[MAX_SERVICES], nf_per_service_count[MAX_SERVICES];
struct { int dynfield_offset; } config, *onvm_config = &config;
struct onvm_service_chain { int unused; } chain, *default_chain = &chain;
struct onvm_flow_entry { struct onvm_service_chain *sc; } flow_entry = {&chain};
static unsigned chain_calls, service_calls, nic_calls[RTE_MAX_ETHPORTS], tx_calls;
static uint8_t worker_keep_running;
static struct rte_mbuf *next_rx[RTE_MAX_ETHPORTS];

int rte_errno;
static void rte_pause(void) {}
enum rte_proc_type_t rte_eal_process_type(void) { return RTE_PROC_PRIMARY; }
const struct rte_memzone *rte_memzone_lookup(const char *name) { (void)name; return NULL; }
const struct rte_memzone *rte_memzone_reserve(const char *name, size_t size, int socket, unsigned flags) {
    (void)name; (void)size; (void)socket; (void)flags; rte_errno = ENOMEM; return NULL;
}
static struct onvm_pkt_meta *onvm_get_pkt_meta(struct rte_mbuf *pkt, int offset) { (void)offset; return &pkt->meta; }
static void rte_pktmbuf_free(struct rte_mbuf *pkt) { assert(!pkt->freed); pkt->freed++; }
static unsigned rte_ring_enqueue_bulk(struct rte_ring *ring, void **pkts, unsigned count, void *unused) {
    (void)unused;
    if (ring->count + count > ring->capacity) return 0;
    memcpy(ring->pkts + ring->count, pkts, count * sizeof(*pkts)); ring->count += count; return count;
}
static unsigned rte_ring_dequeue_burst(struct rte_ring *ring, void **pkts, unsigned count, void *unused) {
    (void)unused;
    assert(pthread_rwlock_trywrlock(&onvm_upf_tx_lock) != 0); /* NF removal cannot overlap TX. */
    if (count > ring->count) count = ring->count;
    memcpy(pkts, ring->pkts, count * sizeof(*pkts));
    memmove(ring->pkts, ring->pkts + count, (ring->count - count) * sizeof(*pkts));
    ring->count -= count;
    worker_keep_running = 0; /* One TX pass. */
    return count;
}
static int onvm_nf_is_valid(struct onvm_nf *nf) { return nf->valid; }
static uint16_t onvm_sc_service_to_nf_map(uint16_t service, struct rte_mbuf *pkt) {
    (void)pkt; service_calls++; assert(service < num_services);
    return nf_per_service_count[service] ? services[service][0] : 0;
}
static int onvm_sc_next_action(struct onvm_service_chain *sc, struct rte_mbuf *pkt, int offset) {
    (void)sc; (void)pkt; (void)offset; chain_calls++; return ONVM_NF_ACTION_TONF;
}
static int onvm_sc_next_destination(struct onvm_service_chain *sc, struct rte_mbuf *pkt, int offset) {
    (void)sc; (void)pkt; (void)offset; return 5;
}
static int onvm_flow_dir_get_pkt(struct rte_mbuf *pkt, struct onvm_flow_entry **entry) {
    *entry = &flow_entry; return pkt->port == 5 ? 0 : -1;
}
static uint16_t rte_eth_tx_burst(uint16_t port, uint16_t queue, struct rte_mbuf **pkts, uint16_t count) {
    (void)port; (void)queue; (void)pkts; tx_calls++; return count;
}
static uint16_t rte_eth_rx_burst(uint16_t port, uint16_t queue, struct rte_mbuf **pkts, uint16_t count) {
    assert(queue == 0 && count == PACKET_READ_SIZE); nic_calls[port]++;
    /* Port 5 is the final, ordinary port; end after one complete RX pass. */
    if (port == 5) worker_keep_running = 0;
    if (!next_rx[port]) return 0;
    pkts[0] = next_rx[port]; next_rx[port] = NULL; return 1;
}
static unsigned rte_lcore_id(void) { return 1; }
static void onvm_stats_gen_event_info(const char *name, int type, void *data) { (void)name; (void)type; (void)data; }

void onvm_pkt_flush_nf_queue(struct queue_mgr *, uint16_t, struct onvm_nf *);
void onvm_pkt_flush_all_nfs(struct queue_mgr *, struct onvm_nf *);
void onvm_pkt_enqueue_nf(struct queue_mgr *, uint16_t, struct rte_mbuf *, struct onvm_nf *);
void onvm_pkt_enqueue_nf_instance(struct queue_mgr *, uint16_t, struct rte_mbuf *, struct onvm_nf *);
void onvm_pkt_enqueue_tx_thread(struct packet_buf *, struct onvm_nf *);
void onvm_pkt_flush_port_queue(struct queue_mgr *, uint16_t);
#endif
