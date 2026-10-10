/* Manager learns per-UE ownership from packets; no PFCP notification. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define UPF_SW_LB_MZ "UPF_SW_LB_V3"
#define UPF_SW_LB_CAPACITY 2048 /* at most 1024 sessions, <= 50% occupied */
#define UPF_SW_LB_MAX_WORKERS 32
#define UPF_SW_LB_WORKERS 128  /* ONVM instance IDs, zero reserved */
#define UPF_SW_LB_DRAIN_WORDS 16 /* 1024 session-buffer indices */
#define UPF_SW_LB_DROP (-1)
#define UPF_SW_LB_ARP (-2)

struct upf_sw_lb_route {
    uint32_t ue_ip; /* host byte order */
    uint32_t teid;
    uint16_t instance;
};

struct upf_sw_lb_worker {
    uint32_t version; /* release after the worker stops using older snapshots */
    uint32_t started;
    uint32_t drain_pending;
    uint64_t drain[UPF_SW_LB_DRAIN_WORDS];
} __attribute__((aligned(64)));

struct upf_sw_lb {
    uint16_t n3_port, n6_port;
    uint32_t n3_ip, n6_ip; /* host byte order */
    uint16_t worker_count, leader, route_count;
    uint16_t teid_count, next_worker;
    uint16_t worker_order[UPF_SW_LB_MAX_WORKERS];
    uint16_t worker_sessions[UPF_SW_LB_WORKERS]; /* observed UE bindings, not PFCP active sessions */
    uint8_t workers[UPF_SW_LB_WORKERS];
    struct upf_sw_lb_route last_learned; /* manager-only, for setup logging */
    struct upf_sw_lb_route routes[UPF_SW_LB_CAPACITY]; /* indexed by UE IP (DL) */
    struct upf_sw_lb_route teid_routes[UPF_SW_LB_CAPACITY]; /* indexed by UL TEID */
    struct upf_sw_lb_worker state[UPF_SW_LB_WORKERS];
};

/* Startup only: N3/N6 settings and worker order remain fixed during a run. */
int upf_sw_lb_load(struct upf_sw_lb *lb, const char *path);
/* Routes are append-only. The single manager RX thread is their only writer;
 * workers acquire instance before reading a published route's immutable UE IP. */
const struct upf_sw_lb_route *upf_sw_lb_lookup(const struct upf_sw_lb *lb, uint32_t ue_ip);
const struct upf_sw_lb_route *upf_sw_lb_lookup_teid(const struct upf_sw_lb *lb, uint32_t teid);
/* >0: instance ID, 0: other port (normal ONVM), negative: drop / ARP fanout. */
int upf_sw_lb_classify(struct upf_sw_lb *lb, uint16_t port,
                       const uint8_t *packet, size_t length);
/* Called only at a worker's packet-burst boundary. */
int upf_sw_lb_all_seen(const struct upf_sw_lb *lb, uint32_t version);
