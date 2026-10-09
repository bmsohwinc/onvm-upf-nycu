/* Only DPDK/ONVM services are mocked. The runner compiles production bodies. */
#pragma once
#include <assert.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "upf_sw_lb.h"
#define MAX_NFS 128
#define PACKET_READ_SIZE 32
#define RTE_CACHE_LINE_SIZE 64
#define SESS_BUF_MAX_USERS 1024
#define ONVM_NF_ACTION_TONF 2
#define UPF_U_SERVICE_ID 1
#define UPF_C_SERVICE_ID 2
#define EVT_CLS_GC_ACK 0x4202
#define UTLT_Error(...) ((void)0)
#define UTLT_Warning(...) ((void)0)
#define rte_be_to_cpu_32 ntohl
struct onvm_pkt_meta { uint16_t src, destination; uint8_t action, chain_index, flags; };
struct rte_mbuf {
    uint16_t port, nb_segs, data_len;
    struct onvm_pkt_meta meta;
    void *pool;
    uint8_t data[256];
};
struct packet_buf { uint16_t count; struct rte_mbuf *buffer[PACKET_READ_SIZE]; };
struct queue_mgr { struct packet_buf nf_rx_bufs[MAX_NFS]; };
struct rte_ring { unsigned count, capacity; struct rte_mbuf *packets[256]; };
struct onvm_nf {
    uint16_t instance_id, service_id;
    int status;
    char *tag;
    struct rte_ring *rx_q;
    struct { uint64_t rx, rx_drop, tx, tx_drop; } stats;
};
struct port_info { unsigned num_ports; uint16_t id[32]; };
struct onvm_configuration { int dynfield_offset; };
struct rte_memzone { void *addr; size_t len; };
struct onvm_service_chain { int unused; };
struct onvm_flow_entry { struct onvm_service_chain *sc; };
typedef struct { void *active; uint32_t version; } upf_cls_ctrl_t;
typedef struct { uint8_t is_buffering; } UpfSessBuf;
typedef struct { int index; uint32_t ue_ip; } UpfSession;
extern struct onvm_nf nfs[MAX_NFS];
extern struct port_info *ports;
extern struct onvm_configuration *onvm_config;
extern struct onvm_service_chain *default_chain;
extern int ONVM_NF_SHARE_CORES;
extern struct upf_sw_lb *onvm_upf_lb;
extern const char *onvm_upf_lb_path;
extern upf_cls_ctrl_t *g_upf_cls_ctrl;
extern UpfSessBuf *g_sess_buf;
extern uint8_t g_nat_enabled;
extern uint16_t g_n3_port, g_n6_port;
extern uint32_t g_n3_ip_be, g_n6_ip_be;
#define rte_pktmbuf_mtod(m, type) ((type)(m)->data)
static inline unsigned rte_pktmbuf_data_len(const struct rte_mbuf *m) { return m->data_len; }
static inline int rte_socket_id(void) { return 0; }
static inline int onvm_nf_is_valid(const struct onvm_nf *nf) { return nf->status == 1; }
static inline struct onvm_pkt_meta *onvm_get_pkt_meta(struct rte_mbuf *m, int off) { (void)off; return &m->meta; }
void rte_exit(int status, const char *fmt, ...) __attribute__((noreturn));
const struct rte_memzone *rte_memzone_reserve_aligned(const char *, size_t, int, int, unsigned);
const struct rte_memzone *rte_memzone_lookup(const char *);
void rte_pktmbuf_free(struct rte_mbuf *);
struct rte_mbuf *rte_pktmbuf_copy(const struct rte_mbuf *, void *, uint32_t, uint32_t);
int rte_ring_enqueue_bulk(struct rte_ring *, void **, unsigned, void *);
void onvm_pkt_drop(struct rte_mbuf *);
void onvm_pkt_flush_nf_queue(struct queue_mgr *, uint16_t, struct onvm_nf *);
void onvm_pkt_flush_all_nfs(struct queue_mgr *, struct onvm_nf *);
void onvm_pkt_flush_port_queue(struct queue_mgr *, uint16_t);
void onvm_pkt_enqueue_nf(struct queue_mgr *, uint16_t, struct rte_mbuf *, struct onvm_nf *);
uint8_t onvm_sc_next_action(struct onvm_service_chain *, struct rte_mbuf *, int);
uint16_t onvm_sc_next_destination(struct onvm_service_chain *, struct rte_mbuf *, int);
int onvm_flow_dir_get_pkt(struct rte_mbuf *, struct onvm_flow_entry **);
void onvm_upf_lb_init(void);
int onvm_upf_lb_dispatch(struct queue_mgr *, struct rte_mbuf *);
void onvm_pkt_process_rx_batch(struct queue_mgr *, struct rte_mbuf **, uint16_t);
int UpfSendEvt1(uint16_t, uint32_t, uintptr_t);
UpfSession *UpfSessionFindByUeIP(uint32_t);
int upf_u_lb_init(struct onvm_nf *);
int upf_u_lb_enabled(void);
int upf_u_lb_arp_responder(void);
void upf_u_lb_poll(void **, uint32_t *, struct onvm_nf *, uint32_t (*)(int, uint32_t, struct onvm_nf *));
void upf_u_lb_relay_drain(int);
void test_worker_select(unsigned);
