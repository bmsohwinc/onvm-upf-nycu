#pragma once
#include "upf_sw_lb.h"

struct queue_mgr;
struct rte_mbuf;
extern struct upf_sw_lb *onvm_upf_lb;
extern const char *onvm_upf_lb_path;
void onvm_upf_lb_init(void);
/* Returns one when it consumes the packet, zero for the normal ONVM path. */
int onvm_upf_lb_dispatch(struct queue_mgr *mgr, struct rte_mbuf *pkt);
