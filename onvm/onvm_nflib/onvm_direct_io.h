/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef ONVM_DIRECT_IO_H
#define ONVM_DIRECT_IO_H

#include "onvm_common.h"

/* Internal helpers for one explicitly enabled NF per process. */
int onvm_direct_io_matches(const struct onvm_nf *nf);
uint16_t onvm_direct_io_rx(struct onvm_nf *nf, struct rte_mbuf **pkts);
int onvm_direct_io_tx(struct onvm_nf *nf, struct rte_mbuf **pkts, uint16_t count);

#endif
