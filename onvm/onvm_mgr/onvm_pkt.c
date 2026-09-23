/*********************************************************************
 *                     openNetVM
 *              https://sdnfv.github.io
 *
 *   BSD LICENSE
 *
 *   Copyright(c)
 *            2015-2019 George Washington University
 *            2015-2019 University of California Riverside
 *            2010-2019 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * The name of the author may not be used to endorse or promote
 *       products derived from this software without specific prior
 *       written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ********************************************************************/

/******************************************************************************
                                 onvm_pkt.c

            This file contains all functions related to receiving or
            transmitting packets.

******************************************************************************/

#include "onvm_mgr.h"

#include "onvm_nf.h"
#include "onvm_pkt.h"
#include "onvm_upf.h"

/* Temporary port -> UPF-U service map. Match actual DPDK port IDs and worker
 * YAML: N3 VF0/VF1 = 0/1, N6 VF0/VF1 = 2/3. One NF per worker service.
 * Zero entries use normal service-chain dispatch. Used only until UPF-C
 * publishes a dynamic slot configuration; that mode replaces this entire map.
 */
uint16_t onvm_port_to_upf_service[RTE_MAX_ETHPORTS] = {
        [0] = 14, [1] = 15, [2] = 14, [3] = 15,
};

/**********************************Interfaces*********************************/

void
onvm_pkt_process_rx_batch(struct queue_mgr *rx_mgr, struct rte_mbuf *pkts[], uint16_t rx_count) {
        uint16_t i;
        struct onvm_pkt_meta *meta;
#ifdef FLOW_LOOKUP
        struct onvm_flow_entry *flow_entry;
        struct onvm_service_chain *sc;
        int ret;
#endif

        if (rx_mgr == NULL || pkts == NULL)
                return;

        for (i = 0; i < rx_count; i++) {
                meta = onvm_get_pkt_meta(pkts[i], onvm_config->dynfield_offset);
                memset(meta, 0, sizeof(*meta));
                uint16_t port = pkts[i]->port;
                if (onvm_upf_is_dynamic()) {
                        int instance = onvm_upf_port_destination(port);
                        if (instance < 0) {
                                rte_pktmbuf_free(pkts[i]);
                                continue;
                        }
                        if (instance > 0) {
                                meta->action = ONVM_NF_ACTION_TONF;
                                meta->destination = nfs[instance].service_id;
                                onvm_pkt_enqueue_nf_instance(rx_mgr, instance, pkts[i], NULL);
                                continue;
                        }
                }
                uint16_t service = !onvm_upf_is_dynamic() && port < RTE_MAX_ETHPORTS
                                     ? onvm_port_to_upf_service[port] : 0;
                if (service != 0) {
                        if (service >= num_services) {
                                rte_pktmbuf_free(pkts[i]);
                                continue;
                        }
                        meta->action = ONVM_NF_ACTION_TONF;
                        meta->destination = service;
                        onvm_pkt_enqueue_nf(rx_mgr, service, pkts[i], NULL);
                        continue;
                }
#ifdef FLOW_LOOKUP
                ret = onvm_flow_dir_get_pkt(pkts[i], &flow_entry);
                if (ret >= 0) {
                        sc = flow_entry->sc;
                        meta->action = onvm_sc_next_action(sc, pkts[i], onvm_config->dynfield_offset);
                        meta->destination = onvm_sc_next_destination(sc, pkts[i], onvm_config->dynfield_offset);
                } else {
#endif
                        meta->action = onvm_sc_next_action(default_chain, pkts[i], onvm_config->dynfield_offset);
                        meta->destination = onvm_sc_next_destination(default_chain, pkts[i], onvm_config->dynfield_offset);
#ifdef FLOW_LOOKUP
                }
#endif
                /* PERF: this might hurt performance since it will cause cache
                 * invalidations. Ideally the data modified by the NF manager
                 * would be a different line than that modified/read by NFs.
                 * That may not be possible.
                 */

                (meta->chain_index)++;
                onvm_pkt_enqueue_nf(rx_mgr, meta->destination, pkts[i], NULL);
        }

        onvm_pkt_flush_all_nfs(rx_mgr, NULL);
}

void
onvm_pkt_flush_all_ports(struct queue_mgr *tx_mgr) {
        uint16_t i;

        if (tx_mgr == NULL)
                return;

        for (i = 0; i < ports->num_ports; i++)
                onvm_pkt_flush_port_queue(tx_mgr, ports->id[i]);
}

void
onvm_pkt_drop_batch(struct rte_mbuf **pkts, uint16_t size) {
        uint16_t i;

        if (pkts == NULL)
                return;

        for (i = 0; i < size; i++)
                rte_pktmbuf_free(pkts[i]);
}
