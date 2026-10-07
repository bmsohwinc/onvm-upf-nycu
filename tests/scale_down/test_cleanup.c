/* SPDX-License-Identifier: Apache-2.0 */
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/queue.h>
#include "upf_worker.h"

static UpfWorkerRegistry registry;
UpfWorkerRegistry *g_upf_workers = &registry;
static int probed = 1, filter_error, filter_calls;
static int probe(void) { return 0; }
static int filter(unsigned side, uint32_t location, uint16_t vf, struct in_addr address, int add) {
    (void)side; (void)location; (void)vf; (void)address; (void)add;
    filter_calls++; return filter_error;
}
#include "steering.inc"

#define MAX_UE 8
typedef int rte_spinlock_t;
static void rte_spinlock_lock(int *lock) { assert(!*lock); *lock = 1; }
static void rte_spinlock_unlock(int *lock) { assert(*lock); *lock = 0; }
struct rte_mbuf { int freed; };
struct rte_mempool { int returned, freed; };
static void rte_pktmbuf_free(struct rte_mbuf *pkt) { assert(!pkt->freed); pkt->freed++; }
static void rte_mempool_put(struct rte_mempool *pool, void *entry) { (void)entry; pool->returned++; }
static void rte_mempool_free(struct rte_mempool *pool) { assert(!pool->freed); pool->freed++; }
static struct { uint32_t ue_ip, tokens; } ue_table[MAX_UE];
static struct { uint32_t ue_ip; int ue_idx; bool in_use; } ue_hash[MAX_UE];
static rte_spinlock_t ue_table_lock, ue_tb_locks[MAX_UE];
#include "purge.inc"

int main(void) {
    registry.config.slot_count = 1;
    registry.runtime[0].generation = 1;
    UpfSteerUpdate update = {.slot = 0, .generation = 1, .operation = UPF_STEER_N3_ADD};
    assert(!apply(&update) && n3_generation[0] == 1);
    update.operation = UPF_STEER_SESSION_ADD;
    update.ue_addr.s_addr = htonl(0x0a3c0001);
    assert(!apply(&update));
    update.operation = UPF_STEER_N3_DEL;
    assert(apply(&update) == -EBUSY && filter_calls == 2);
    update.operation = UPF_STEER_SESSION_DEL;
    update.ue_addr.s_addr++;
    assert(apply(&update) == -ESTALE);
    update.ue_addr.s_addr--;
    filter_error = -EIO;
    assert(apply(&update) == -EIO && sessions[0].generation);
    filter_error = 0;
    assert(!apply(&update) && !sessions[0].generation);
    assert(!apply(&update)); /* Duplicate deletion never touches someone else's filter. */
    update.operation = UPF_STEER_N3_DEL;
    filter_error = -EIO;
    assert(apply(&update) == -EIO && n3_generation[0] == 1);
    filter_error = 0;
    assert(!apply(&update) && !n3_generation[0]);
    assert(!apply(&update));
    registry.runtime[0].generation = 2;
    assert(apply(&update) == -ESTALE);
    update.generation = 2; update.operation = UPF_STEER_N3_ADD;
    assert(!apply(&update) && n3_generation[0] == 2);

    /* Deleting a collision-chain head must preserve subsequent UE lookups. */
    for (int i = 0; i < 3; i++) {
        ue_table[i].ue_ip = 1 + i * MAX_UE;
        ue_table[i].tokens = 123;
        assert(ueHashInsert(ue_table[i].ue_ip, i));
    }
    removeEntrybyUeIp(1);
    assert(ueHashSearch(1) == -1 && !ue_table[0].tokens);
    assert(ueHashSearch(9) == 1 && ueHashSearch(17) == 2 && ue_table[1].tokens == 123);
    removeEntrybyUeIp(1);
    ue_table[0].ue_ip = 25; assert(ueHashInsert(25, 0) && ueHashSearch(25) == 0);

    struct rte_mempool pool = {0}; g_shaper_entry_pool = &pool;
    struct rte_mbuf packets[3] = {0};
    struct shaper_entry entries[] = {{.pkt = &packets[0]}, {.pkt = &packets[1]}, {.pkt = &packets[2]}};
    entries[0].next = &entries[1];
    g_ue_shaper[0].flows[0].head = &entries[0]; g_ue_shaper[0].queued_pkts = 2;
    g_ue_shaper[1].flows[0].head = &entries[2]; g_ue_shaper[1].queued_pkts = 1;
    g_shaper_active_ue_bitmap[0] = 3;
    upf_u_shaper_forget_ue(0);
    assert(packets[0].freed && packets[1].freed && !packets[2].freed && pool.returned == 2);
    assert(!g_ue_shaper[0].queued_pkts && g_ue_shaper[1].queued_pkts == 1);
    assert(g_shaper_active_ue_bitmap[0] == 2);
    upf_u_shaper_forget_ue(0); assert(pool.returned == 2);
    upf_u_shaper_forget_ue(-1); upf_u_shaper_forget_ue(MAX_UE);
    upf_u_shaper_cleanup();
    assert(packets[2].freed && pool.returned == 3 && pool.freed && !g_shaper_entry_pool);
    upf_u_shaper_cleanup(); assert(pool.freed == 1);
    puts("PASS: owned steering deletion, stale generations, UE hash collisions and buffered-packet cleanup");
}
