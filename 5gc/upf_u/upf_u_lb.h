#pragma once
#include <stdint.h>
struct onvm_nf;
int upf_u_lb_init(struct onvm_nf *nf);
int upf_u_lb_enabled(void);
int upf_u_lb_arp_responder(void);
void upf_u_lb_poll(void **snapshot, uint32_t *version, struct onvm_nf *nf,
                   uint32_t (*drain)(int, uint32_t, struct onvm_nf *));
void upf_u_lb_relay_drain(int session_index);
