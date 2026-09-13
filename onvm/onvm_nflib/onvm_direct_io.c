/* SPDX-License-Identifier: BSD-3-Clause */
#include "onvm_direct_io.h"
#include "onvm_nflib.h"

/* Process-local state: no changes to the shared NF/port structure layouts. */
static struct {
        struct onvm_nf *nf;
        uint16_t port[2];
        uint8_t next_rx;
} direct_io;

int
onvm_nflib_enable_direct_io(struct onvm_nf *nf, uint16_t n3_port, uint16_t n6_port) {
        const struct rte_memzone *mz;
        uint64_t reserved;
        const uint16_t selected[2] = {n3_port, n6_port};

        if (nf == NULL || nf->nf_tx_mgr == NULL || nf->nf_tx_mgr->mgr_type_t != NF ||
            ports == NULL || n3_port == n6_port)
                return -EINVAL;
        if (direct_io.nf != NULL)
                return -EALREADY;

        mz = rte_memzone_lookup(MZ_DIRECT_PORT_MASK);
        if (mz == NULL || mz->len < sizeof(reserved)) {
                fprintf(stderr, "Direct I/O requires a manager with --direct-port-mask\n");
                return -ENOENT;
        }
        reserved = *(const uint64_t *)mz->addr;

        for (unsigned i = 0; i < 2; i++) {
                const uint16_t port = selected[i];
                struct rte_eth_dev_info info;
                if (port >= RTE_MAX_ETHPORTS || port >= 64 ||
                    !rte_eth_dev_is_valid_port(port) || !ports->init[port] ||
                    !(reserved & (UINT64_C(1) << port))) {
                        fprintf(stderr, "Direct I/O port %u must be initialized and reserved by manager\n", port);
                        return -EINVAL;
                }
                int ret = rte_eth_dev_info_get(port, &info);
                if (ret != 0)
                        return ret;
                if (info.nb_rx_queues != 1 || info.nb_tx_queues != 1) {
                        fprintf(stderr, "Direct I/O port %u requires exactly one RX/TX queue pair\n", port);
                        return -EINVAL;
                }
        }

        direct_io.port[0] = n3_port;
        direct_io.port[1] = n6_port;
        direct_io.next_rx = 0;
        direct_io.nf = nf;
        printf("NF %u direct I/O: N3 port %u, N6 port %u, RX/TX queue 0\n",
               nf->instance_id, n3_port, n6_port);
        return 0;
}

int
onvm_direct_io_matches(const struct onvm_nf *nf) {
        return nf != NULL && direct_io.nf == nf;
}

uint16_t
onvm_direct_io_rx(struct onvm_nf *nf, struct rte_mbuf **pkts) {
        const uint16_t port = direct_io.port[direct_io.next_rx];
        direct_io.next_rx ^= 1;
        uint16_t count = rte_eth_rx_burst(port, 0, pkts, PACKET_READ_SIZE);
        nf->stats.rx += count;
        ports->rx_stats.rx[port] += count;

        for (uint16_t i = 0; i < count; i++) {
                struct onvm_pkt_meta *meta = onvm_get_pkt_meta(pkts[i], nf->dynfield_offset);
                /* NIC RX bypasses the manager's metadata initialization. */
                memset(meta, 0, sizeof(*meta));
                meta->action = ONVM_NF_ACTION_DROP;
                pkts[i]->port = port;
        }
        return count;
}

static int
direct_flush(struct onvm_nf *nf, uint16_t port, struct packet_buf *buf) {
        if (buf->count == 0)
                return 0;
        uint16_t sent = rte_eth_tx_burst(port, 0, buf->buffer, buf->count);
        uint16_t dropped = buf->count - sent;
        for (uint16_t i = sent; i < buf->count; i++)
                rte_pktmbuf_free(buf->buffer[i]);
        nf->stats.tx += sent;
        nf->stats.tx_drop += dropped;
        ports->tx_stats.tx[port] += sent;
        ports->tx_stats.tx_drop[port] += dropped;
        buf->count = 0;
        return dropped ? -ENOBUFS : 0;
}

int
onvm_direct_io_tx(struct onvm_nf *nf, struct rte_mbuf **pkts, uint16_t count) {
        struct packet_buf bufs[2];
        int result = 0;
        bufs[0].count = bufs[1].count = 0;

        /* Handles ordinary bursts, ARP returns, and deferred shaper/buffer TX. */
        for (uint16_t i = 0; i < count; i++) {
                struct onvm_pkt_meta *meta = onvm_get_pkt_meta(pkts[i], nf->dynfield_offset);
                meta->src = nf->instance_id;
                if (meta->action == ONVM_NF_ACTION_DROP) {
                        nf->stats.act_drop++;
                        rte_pktmbuf_free(pkts[i]);
                        continue;
                }
                if (meta->action != ONVM_NF_ACTION_OUT ||
                    (meta->destination != direct_io.port[0] && meta->destination != direct_io.port[1])) {
                        /* No manager/ring fallback for direct data-plane packets. */
                        nf->stats.tx_drop++;
                        rte_pktmbuf_free(pkts[i]);
                        result = -EINVAL;
                        continue;
                }
                nf->stats.act_out++;
                unsigned which = meta->destination == direct_io.port[0] ? 0 : 1;
                struct packet_buf *buf = &bufs[which];
                buf->buffer[buf->count++] = pkts[i];
                if (buf->count == PACKET_READ_SIZE && direct_flush(nf, direct_io.port[which], buf) < 0 && result == 0)
                        result = -ENOBUFS;
        }
        for (unsigned i = 0; i < 2; i++) {
                if (direct_flush(nf, direct_io.port[i], &bufs[i]) < 0 && result == 0)
                        result = -ENOBUFS;
        }
        return result;
}
