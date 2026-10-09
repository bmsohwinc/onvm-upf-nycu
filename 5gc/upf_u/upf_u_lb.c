#include <rte_memzone.h>
#include "upf_sw_lb.h"
#include "upf_cls_ctrl.h"
#include "upf_context.h"
#include "upf_sess_buf.h"
#include "upf_events.h"
#include "upf_u_config.h"
#include "upf_u_lb.h"

static struct upf_sw_lb *lb;
static unsigned instance;
static uint32_t last_ack;

int upf_u_lb_enabled(void) { return lb != NULL; }
int upf_u_lb_arp_responder(void) { return !lb || instance == lb->leader; }

int
upf_u_lb_init(struct onvm_nf *nf) {
    const struct rte_memzone *mz = rte_memzone_lookup(UPF_SW_LB_MZ);
    if (!mz) return 0;
    if (mz->len < sizeof(*lb)) return -1;
    lb = mz->addr;
    instance = nf->instance_id;
    if (instance >= UPF_SW_LB_WORKERS || !lb->workers[instance] || nf->service_id != UPF_U_SERVICE_ID ||
        g_nat_enabled || g_n3_port != lb->n3_port || g_n6_port != lb->n6_port ||
        rte_be_to_cpu_32(g_n3_ip_be) != lb->n3_ip || rte_be_to_cpu_32(g_n6_ip_be) != lb->n6_ip) {
        UTLT_Error("UPF LB: require mapped instance, service 1, matching N3/N6 ports/IPs, NAT disabled");
        return -1;
    }
    /* Restarting a worker loses its private meters/shaper/ARP state. Require a
     * clean experiment restart instead of silently reusing stale state. */
    if (__atomic_exchange_n(&lb->state[instance].started, 1, __ATOMIC_ACQ_REL)) {
        UTLT_Error("UPF LB: instance %u already started; restart the deployment", instance);
        return -1;
    }
    UTLT_Warning("UPF LB: instance=%u, workers=%u, ACK/ARP leader=%u",
                 instance, lb->worker_count, lb->leader);
    return 0;
}

void
upf_u_lb_poll(void **snapshot, uint32_t *version, struct onvm_nf *nf,
              uint32_t (*drain)(int, uint32_t, struct onvm_nf *)) {
    if (!lb) return;
    struct upf_sw_lb_worker *state = &lb->state[instance];
    uint32_t v1 = __atomic_load_n(&g_upf_cls_ctrl->version, __ATOMIC_ACQUIRE);
    if (!(v1 & 1) && v1 != *version) {
        void *p = __atomic_load_n(&g_upf_cls_ctrl->active, __ATOMIC_ACQUIRE);
        uint32_t v2 = __atomic_load_n(&g_upf_cls_ctrl->version, __ATOMIC_ACQUIRE);
        if (p && v1 == v2) {
            *snapshot = p;
            *version = v2;
            /* No old classifier/PDR reference survives the packet burst. */
            __atomic_store_n(&state->version, v2, __ATOMIC_RELEASE);
        }
    }
    if (instance == lb->leader && *version != last_ack && upf_sw_lb_all_seen(lb, *version)) {
        if (UpfSendEvt1(UPF_C_SERVICE_ID, EVT_CLS_GC_ACK, *version) == 0)
            last_ack = *version;
    }
    if (__atomic_load_n(&state->drain_pending, __ATOMIC_ACQUIRE) &&
        __atomic_exchange_n(&state->drain_pending, 0, __ATOMIC_ACQ_REL)) {
        for (unsigned w = 0; w < UPF_SW_LB_DRAIN_WORDS; w++) {
            uint64_t bits = __atomic_exchange_n(&state->drain[w], 0, __ATOMIC_ACQ_REL);
            while (bits) {
                unsigned bit = __builtin_ctzll(bits);
                unsigned index = w * 64 + bit;
                g_sess_buf[index].is_buffering = 0;
                drain(index, UINT32_MAX, nf);
                bits &= bits - 1;
            }
        }
    }
}

void
upf_u_lb_relay_drain(int session_index) {
    if (session_index < 0 || session_index >= SESS_BUF_MAX_USERS) return;
    /* UPF-C still addresses service 1. Resolve this rare control event to the
     * same owner as packets, preserving each session ring's single consumer. */
    for (unsigned i = 0; i < UPF_SW_LB_CAPACITY; i++) {
        const struct upf_sw_lb_route *r = &lb->routes[i];
        if (!r->instance) continue;
        const UpfSession *s = UpfSessionFindByUeIP(r->ue_ip);
        if (!s || s->index != session_index) continue;
        struct upf_sw_lb_worker *state = &lb->state[r->instance];
        __atomic_fetch_or(&state->drain[session_index / 64], UINT64_C(1) << (session_index % 64), __ATOMIC_RELEASE);
        __atomic_store_n(&state->drain_pending, 1, __ATOMIC_RELEASE);
        return;
    }
    UTLT_Warning("UPF LB: no mapped owner for drain session index %d", session_index);
}
