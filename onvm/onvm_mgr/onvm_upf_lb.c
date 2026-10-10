#include "onvm_mgr.h"
#include "onvm_nf.h"
#include "onvm_upf_lb.h"
#include "onvm_init.h"
#include <arpa/inet.h>

struct upf_sw_lb *onvm_upf_lb;
const char *onvm_upf_lb_path;
_Static_assert(UPF_SW_LB_WORKERS == MAX_NFS, "UPF LB instance limit must match ONVM");

void
onvm_upf_lb_init(void) {
    if (!onvm_upf_lb_path) return;
    if (ONVM_NUM_RX_THREADS != 1)
        rte_exit(EXIT_FAILURE, "UPF packet learning requires one manager RX thread\n");
    const struct rte_memzone *mz = rte_memzone_reserve_aligned(
        UPF_SW_LB_MZ, sizeof(struct upf_sw_lb), rte_socket_id(), 0, RTE_CACHE_LINE_SIZE);
    if (!mz) rte_exit(EXIT_FAILURE, "Cannot reserve UPF software LB configuration\n");
    onvm_upf_lb = mz->addr;
    if (upf_sw_lb_load(onvm_upf_lb, onvm_upf_lb_path))
        rte_exit(EXIT_FAILURE, "Invalid UPF software LB configuration\n");
    int n3_found = 0, n6_found = 0;
    for (unsigned i = 0; i < ports->num_ports; i++) {
        n3_found |= ports->id[i] == onvm_upf_lb->n3_port;
        n6_found |= ports->id[i] == onvm_upf_lb->n6_port;
    }
    if (!n3_found || !n6_found || ONVM_NF_SHARE_CORES)
        rte_exit(EXIT_FAILURE, "UPF LB requires enabled N3/N6 ports and dedicated worker cores\n");
    printf("UPF software LB: packet-learned round robin, %u workers, N3=%u N6=%u, leader instance=%u\n",
           onvm_upf_lb->worker_count,
           onvm_upf_lb->n3_port, onvm_upf_lb->n6_port, onvm_upf_lb->leader);
}

static void
enqueue_instance(struct queue_mgr *mgr, struct rte_mbuf *pkt, uint16_t id) {
    struct onvm_nf *nf = &nfs[id];
    /* Mapped IDs are reserved for UPF-U at NF startup. */
    if (!onvm_nf_is_valid(nf) || !nf->rx_q || nf->service_id != 1 ||
        !__atomic_load_n(&onvm_upf_lb->state[id].started, __ATOMIC_ACQUIRE)) {
        nf->stats.rx_drop++;
        rte_pktmbuf_free(pkt);
        return;
    }
    struct onvm_pkt_meta *meta = onvm_get_pkt_meta(pkt, onvm_config->dynfield_offset);
    memset(meta, 0, sizeof(*meta));
    meta->action = ONVM_NF_ACTION_TONF;
    meta->destination = 1; /* metadata still uses service IDs */
    meta->chain_index = 1;
    struct packet_buf *buf = &mgr->nf_rx_bufs[id];
    buf->buffer[buf->count++] = pkt;
    if (buf->count == PACKET_READ_SIZE) onvm_pkt_flush_nf_queue(mgr, id, NULL);
}

int
onvm_upf_lb_dispatch(struct queue_mgr *mgr, struct rte_mbuf *pkt) {
    if (pkt->port != onvm_upf_lb->n3_port && pkt->port != onvm_upf_lb->n6_port) return 0;
    int id = UPF_SW_LB_DROP;
    if (pkt->nb_segs == 1)
        id = upf_sw_lb_classify(onvm_upf_lb, pkt->port,
                               rte_pktmbuf_mtod(pkt, const uint8_t *), rte_pktmbuf_data_len(pkt));
    if (id > 0) {
        if (onvm_upf_lb->last_learned.instance) {
            struct upf_sw_lb_route *r = &onvm_upf_lb->last_learned;
            struct in_addr address = {.s_addr = htonl(r->ue_ip)};
            char ip[INET_ADDRSTRLEN], teid[40];
            inet_ntop(AF_INET, &address, ip, sizeof(ip));
            if (r->teid) snprintf(teid, sizeof(teid), "%u (0x%08x)", r->teid, r->teid);
            else snprintf(teid, sizeof(teid), "pending first UL");
            printf("UPF LB learned: UE=%s UL_TEID=%s instance=%u worker_sessions=%u total_sessions=%u\n",
                   ip, teid, r->instance, onvm_upf_lb->worker_sessions[r->instance],
                   onvm_upf_lb->route_count);
            r->instance = 0;
        }
        enqueue_instance(mgr, pkt, id);
    } else if (id == UPF_SW_LB_ARP) {
        /* Independent mbufs: workers modify metadata and may generate replies.
         * This is only the ARP slow path; user packets are never copied. */
        for (unsigned i = 1; i < UPF_SW_LB_WORKERS; i++) {
            if (!onvm_upf_lb->workers[i]) continue;
            struct rte_mbuf *copy = rte_pktmbuf_copy(pkt, pkt->pool, 0, UINT32_MAX);
            if (copy) enqueue_instance(mgr, copy, i);
            else nfs[i].stats.rx_drop++;
        }
        rte_pktmbuf_free(pkt);
    } else {
        /* rx_drop is available per NF; attribute classifier rejects to leader. */
        nfs[onvm_upf_lb->leader].stats.rx_drop++;
        rte_pktmbuf_free(pkt);
    }
    return 1;
}
