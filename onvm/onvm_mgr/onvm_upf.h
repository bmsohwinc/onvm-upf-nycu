/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ONVM_UPF_H
#define ONVM_UPF_H

#include <stdint.h>
#include <rte_spinlock.h>
#include <rte_rwlock.h>

/* Serializes an RX pass with manager NF registration/removal. In particular,
 * a mapped RX ring cannot be freed while that pass is still using it.
 */
extern rte_spinlock_t onvm_upf_lock;
/* TX passes may run concurrently, but NF removal must wait for all of them. */
extern rte_rwlock_t onvm_upf_tx_lock;

/* Caller holds onvm_upf_lock. Sync runs before polling, after the previous
 * pass's NF buffers have been flushed. This implementation requires one RX thread.
 */
void onvm_upf_sync(void);
void onvm_upf_forget_nf(uint16_t instance_id);
void onvm_upf_nf_lock(void);
void onvm_upf_nf_unlock(void);
void onvm_upf_steer_poll(void); /* Master thread; never the RX thread. */
void onvm_upf_steer_shutdown(void);
int onvm_upf_is_dynamic(void);
/* -1: inactive/drop, 0: ordinary service chain, >0: exact NF instance. */
int onvm_upf_port_destination(uint16_t port);

#endif
