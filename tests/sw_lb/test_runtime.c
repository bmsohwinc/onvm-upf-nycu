#include "stubs.h"

struct onvm_nf nfs[MAX_NFS];
static struct port_info port_storage = {.num_ports = 2, .id = {0, 1}};
struct port_info *ports = &port_storage;
static struct onvm_configuration configuration;
struct onvm_configuration *onvm_config = &configuration;
struct onvm_service_chain *default_chain;
int ONVM_NF_SHARE_CORES;
static upf_cls_ctrl_t ctrl;
upf_cls_ctrl_t *g_upf_cls_ctrl = &ctrl;
static UpfSessBuf buffers[1024];
UpfSessBuf *g_sess_buf = buffers;
uint8_t g_nat_enabled;
uint16_t g_n3_port = 0, g_n6_port = 1;
uint32_t g_n3_ip_be, g_n6_ip_be;
static struct rte_memzone zone;
static unsigned freed, fallback, ack_count, ack_version, drained, drain_owner, drain_index;
static int copy_fail, ack_fail;
static UpfSession sessions[] = {{0, 0x0a3c0001}, {33, 0x0a3c0002}};

void rte_exit(int status, const char *fmt, ...) { (void)fmt; exit(status); }
const struct rte_memzone *rte_memzone_reserve_aligned(const char *name, size_t size, int socket, int flags, unsigned align) {
    (void)name; (void)socket; (void)flags;
    assert(posix_memalign(&zone.addr, align, size) == 0); zone.len = size;
    return &zone;
}
const struct rte_memzone *rte_memzone_lookup(const char *name) { (void)name; return zone.addr ? &zone : NULL; }
void rte_pktmbuf_free(struct rte_mbuf *p) { freed++; free(p); }
void onvm_pkt_drop(struct rte_mbuf *p) { rte_pktmbuf_free(p); }
struct rte_mbuf *rte_pktmbuf_copy(const struct rte_mbuf *p, void *pool, uint32_t start, uint32_t len) {
    (void)pool; assert(start == 0 && len == UINT32_MAX);
    if (copy_fail) return NULL;
    struct rte_mbuf *copy = malloc(sizeof(*copy)); assert(copy); *copy = *p; return copy;
}
int rte_ring_enqueue_bulk(struct rte_ring *r, void **p, unsigned n, void *unused) {
    (void)unused;
    if (r->count + n > r->capacity) return 0;
    for (unsigned i = 0; i < n; i++) r->packets[r->count++] = p[i];
    return n;
}
void onvm_pkt_flush_port_queue(struct queue_mgr *q, uint16_t p) { (void)q; (void)p; }
void onvm_pkt_enqueue_nf(struct queue_mgr *q, uint16_t d, struct rte_mbuf *p, struct onvm_nf *s) {
    (void)q; (void)d; (void)s; fallback++; rte_pktmbuf_free(p);
}
uint8_t onvm_sc_next_action(struct onvm_service_chain *s, struct rte_mbuf *p, int off) {
    (void)s; (void)p; (void)off; return 2;
}
uint16_t onvm_sc_next_destination(struct onvm_service_chain *s, struct rte_mbuf *p, int off) {
    (void)s; (void)p; (void)off; return 1;
}
int onvm_flow_dir_get_pkt(struct rte_mbuf *p, struct onvm_flow_entry **e) { (void)p; (void)e; return -1; }
int UpfSendEvt1(uint16_t sid, uint32_t type, uintptr_t version) {
    assert(sid == 2 && type == EVT_CLS_GC_ACK);
    if (ack_fail) return -1;
    ack_count++; ack_version = version; return 0;
}
UpfSession *UpfSessionFindByUeIP(uint32_t ip) {
    for (unsigned i = 0; i < 2; i++) if (sessions[i].ue_ip == ip) return &sessions[i];
    return NULL;
}
static uint32_t drain(int index, uint32_t limit, struct onvm_nf *nf) {
    assert(limit == UINT32_MAX && !g_sess_buf[index].is_buffering);
    drained++; drain_owner = nf->instance_id; drain_index = index; return 1;
}
static struct rte_mbuf *packet(unsigned ue) {
    struct rte_mbuf *p = calloc(1, sizeof(*p)); assert(p);
    p->port = 1; p->nb_segs = 1; p->data_len = 42;
    p->data[12] = 8; p->data[14] = 0x45; p->data[17] = 28; p->data[23] = 17;
    p->data[30] = 10; p->data[31] = 60; p->data[33] = ue; p->data[39] = 8;
    return p;
}
static void empty(struct rte_ring *r) {
    for (unsigned i = 0; i < r->count; i++) rte_pktmbuf_free(r->packets[i]);
    r->count = 0;
}

int main(int argc, char **argv) {
    assert(argc == 2); onvm_upf_lb_path = argv[1]; onvm_upf_lb_init();
    struct rte_ring rings[2] = {{.capacity = 256}, {.capacity = 256}};
    struct queue_mgr q = {0};
    for (unsigned id = 14; id <= 15; id++) {
        nfs[id].instance_id = id; nfs[id].service_id = 1; nfs[id].tag = "upf_u";
        nfs[id].status = 1; nfs[id].rx_q = &rings[id - 14];
    }
    g_n3_ip_be = htonl(onvm_upf_lb->n3_ip); g_n6_ip_be = htonl(onvm_upf_lb->n6_ip);
    g_nat_enabled = 1; assert(upf_u_lb_init(&nfs[14]) < 0); g_nat_enabled = 0;
    assert(upf_u_lb_init(&nfs[14]) == 0 && upf_u_lb_arp_responder());
    assert(upf_u_lb_init(&nfs[14]) < 0); /* no silent restart */
    assert(upf_u_lb_init(&nfs[15]) == 0 && !upf_u_lb_arp_responder());
    assert(upf_u_lb_enabled());
    struct rte_mbuf *batch[40];
    for (unsigned i = 0; i < 40; i++) batch[i] = packet(i % 2 + 1);
    onvm_pkt_process_rx_batch(&q, batch, 40);
    assert(rings[0].count == 20 && rings[1].count == 20 && !fallback);
    assert(nfs[14].stats.rx == 20 && nfs[15].stats.rx == 20);
    assert(rings[1].packets[0]->meta.destination == 1 && rings[1].packets[0]->meta.chain_index == 1);
    empty(&rings[0]); empty(&rings[1]);
    for (unsigned i = 0; i < 40; i++) batch[i] = packet(1);
    onvm_pkt_process_rx_batch(&q, batch, 40); assert(rings[0].count == 40); empty(&rings[0]);
    rings[1].capacity = 0;
    batch[0] = packet(2); onvm_pkt_process_rx_batch(&q, batch, 1);
    assert(nfs[15].stats.rx_drop == 1 && q.nf_rx_bufs[15].count == 0);
    rings[1].capacity = 256;
    /* Stop between enqueue and flush: production flush must release the batch. */
    assert(onvm_upf_lb_dispatch(&q, packet(2)) == 1);
    nfs[15].status = 0; onvm_pkt_flush_all_nfs(&q, NULL);
    assert(q.nf_rx_bufs[15].count == 0 && nfs[15].stats.rx_drop == 2);
    batch[0] = packet(2); onvm_pkt_process_rx_batch(&q, batch, 1);
    assert(nfs[15].stats.rx_drop == 3); nfs[15].status = 1;
    nfs[15].service_id = 2; assert(onvm_upf_lb_dispatch(&q, packet(2)) == 1); nfs[15].service_id = 1;
    assert(nfs[15].stats.rx_drop == 4);
    unsigned before = freed;
    batch[0] = packet(99); batch[1] = packet(1); batch[1]->nb_segs = 2;
    onvm_pkt_process_rx_batch(&q, batch, 2); assert(freed == before + 2);
    batch[0] = packet(1); batch[0]->port = 7;
    onvm_pkt_process_rx_batch(&q, batch, 1); assert(fallback == 1);
    batch[0] = packet(1); batch[0]->data[13] = 6;
    onvm_pkt_process_rx_batch(&q, batch, 1);
    assert(rings[0].count == 1 && rings[1].count == 1);
    assert(rings[0].packets[0] != rings[1].packets[0]);
    rings[0].packets[0]->data[0] = 42; assert(rings[1].packets[0]->data[0] == 0);
    empty(&rings[0]); empty(&rings[1]);
    copy_fail = 1; batch[0] = packet(1); batch[0]->data[13] = 6;
    onvm_pkt_process_rx_batch(&q, batch, 1); copy_fail = 0;
    assert(!rings[0].count && !rings[1].count);

    void *snapshots[2] = {0}; uint32_t versions[2] = {0};
    int old_snapshot = 1, new_snapshot = 2;
    ctrl.version = 2; ctrl.active = &old_snapshot;
    test_worker_select(14); upf_u_lb_poll(&snapshots[0], &versions[0], &nfs[14], drain);
    assert(!ack_count && snapshots[0] == &old_snapshot);
    test_worker_select(15); upf_u_lb_poll(&snapshots[1], &versions[1], &nfs[15], drain);
    assert(!ack_count);
    test_worker_select(14); upf_u_lb_poll(&snapshots[0], &versions[0], &nfs[14], drain);
    assert(ack_count == 1 && ack_version == 2);
    ctrl.version = 3; ctrl.active = &new_snapshot;
    upf_u_lb_poll(&snapshots[0], &versions[0], &nfs[14], drain);
    assert(snapshots[0] == &old_snapshot && versions[0] == 2);
    ctrl.version = 4;
    upf_u_lb_poll(&snapshots[0], &versions[0], &nfs[14], drain);
    assert(ack_count == 1); /* worker 15 still uses old snapshot */
    test_worker_select(15); upf_u_lb_poll(&snapshots[1], &versions[1], &nfs[15], drain);
    test_worker_select(14); ack_fail = 1;
    upf_u_lb_poll(&snapshots[0], &versions[0], &nfs[14], drain); assert(ack_count == 1);
    ack_fail = 0; upf_u_lb_poll(&snapshots[0], &versions[0], &nfs[14], drain);
    assert(ack_count == 2 && ack_version == 4);
    upf_u_lb_poll(&snapshots[0], &versions[0], &nfs[14], drain); assert(ack_count == 2);
    g_sess_buf[33].is_buffering = 1;
    upf_u_lb_relay_drain(33); upf_u_lb_relay_drain(33);
    upf_u_lb_poll(&snapshots[0], &versions[0], &nfs[14], drain); assert(!drained);
    test_worker_select(15); upf_u_lb_poll(&snapshots[1], &versions[1], &nfs[15], drain);
    assert(drained == 1 && drain_owner == 15 && drain_index == 33);
    upf_u_lb_relay_drain(-1); upf_u_lb_relay_drain(1024); upf_u_lb_relay_drain(1000);
    upf_u_lb_poll(&snapshots[1], &versions[1], &nfs[15], drain); assert(drained == 1);
    free(zone.addr);
    puts("manager dispatch/rings/ARP/fallback and worker snapshot/drain coordination: PASS");
    return 0;
}
