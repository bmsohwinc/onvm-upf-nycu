#define TRACE_MODULE _upf_context

#include "upf_context.h"
#include "upf_sess_buf.h"

#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <netinet/in.h>
#include <net/if.h>

#include <rte_byteorder.h>
#include <rte_memzone.h>
#include <rte_malloc.h>

#include "utlt_debug.h"
#include "utlt_pool.h"
#include "utlt_index.h"
#include "utlt_hash.h"
#include "utlt_network.h"
#include "utlt_netheader.h"

#include "pfcp_message.h"
#include "pfcp_types.h"
#include "pfcp_xact.h"

#include "updk/env.h"
#include "updk/init.h"
#include "updk/rule.h"

#include "upf_cls_ctrl.h"

/* // for logging

#include <inttypes.h>
#include <rte_hexdump.h> */


#define MAX_NUM_OF_SUBNET       16

static UpfContext self;
static _Bool upfContextInitialized = 0;
static uint64_t g_sessionIdPool = 1;

upf_cls_ctrl_t *g_upf_cls_ctrl = NULL;

list_t *g_all_pdr_list = NULL;


void UpfPDRGlobalInit(void) {
    if (!g_all_pdr_list) {
        g_all_pdr_list = list_new();
    }
}

void UpfPDRGlobalAdd(UpfPDR *pdr) {
    if (!pdr) {
        return;
    }
    if (!g_all_pdr_list) {
        g_all_pdr_list = list_new();
    }
    list_rpush(g_all_pdr_list, list_node_new(pdr));
}

void UpfPDRGlobalRemove(UpfPDR *pdr) {
    if (!g_all_pdr_list || !pdr) {
        return;
    }
    list_iterator_t *it = list_iterator_new(g_all_pdr_list, LIST_HEAD);
    for (list_node_t *n; (n = list_iterator_next(it)); ) {
        if ((UpfPDR *)n->val == pdr) { 
            list_remove(g_all_pdr_list, n);
            break;
        }
    }
    list_iterator_destroy(it);
}


int UpfClsCtrlInit(void) {
    const struct rte_memzone *mz = rte_memzone_lookup(MZ_UPF_CLS_CTRL);
    if (!mz) {
        mz = rte_memzone_reserve_aligned(
            MZ_UPF_CLS_CTRL, sizeof(upf_cls_ctrl_t),
            SOCKET_ID_ANY, RTE_MEMZONE_2MB, RTE_CACHE_LINE_SIZE);
        if (!mz) return -1;

        /* We are the creator: initialize to a stable, empty state.
           version must be EVEN (stable). 0 is perfect. */
        upf_cls_ctrl_t *ctrl = (upf_cls_ctrl_t *)mz->addr;
        __atomic_store_n(&ctrl->active,  NULL, __ATOMIC_RELEASE);
        __atomic_store_n(&ctrl->version, 0u,   __ATOMIC_RELEASE);
    }

    g_upf_cls_ctrl = (upf_cls_ctrl_t *)mz->addr;

    // logging block
    /* UTLT_Info("CLS_CTRL mapped: slot=%p iova=%" PRIu64 " active=%p ver=%u",
          (void*)g_upf_cls_ctrl,
          (uint64_t)rte_mem_virt2iova(g_upf_cls_ctrl),
          (void*)g_upf_cls_ctrl->active,
          g_upf_cls_ctrl->version); */

    return 0;
}

UpfContext *Self() {
    return &self;
}

Status UpfContextInit() {
    UTLT_Assert(upfContextInitialized == 0, return STATUS_ERROR,
                "UPF context has been initialized!");

    memset(&self, 0, sizeof(UpfContext));

    // TODO : Add GTPv1 init here
    self.envParams = AllocEnvParams();
    UTLT_Assert(self.envParams, return STATUS_ERROR,
        "EnvParams alloc failed");

    self.upSock.fd = -1;
    SockSetEpollMode(&self.upSock, EPOLLIN);

    // TODO : Add PFCP init here
    ListHeadInit(&self.pfcpIPList);

    ListHeadInit(&self.ranS1uList);
    ListHeadInit(&self.upfN4List);
    ListHeadInit(&self.dnnList);

    self.recoveryTime = htonl(time((time_t *)NULL));

    // Set Default Value
    self.gtpDevNamePrefix = "upfgtp";
    // defined in utlt_3gpptypes instead of GTP_V1_PORT defined in GTP_PATH;
    self.gtpv1Port = GTPV1_U_UDP_PORT;
    self.pfcpPort = PFCP_UDP_PORT;
    self.accessPort = 0;
    self.corePort   = 1;
    self.sgiPort    = 1;  // SGi follows CORE by convention
    strcpy(self.envParams->virtualDevice->deviceID, self.gtpDevNamePrefix);

    // Init Resource
    UpfSessionPoolInit();
    UeIpToUpfSessionMapInit();
    TeidToUpfSessionMapInit();

    PfcpNodeInit(); // init pfcp node for upfN4List (it will used pfcp node)
    TimerListInit(&self.timerServiceList);

    upfContextInitialized = 1;

    return STATUS_OK;
}

// TODO : Need to Remove List Members iterativelyatively
Status UpfContextTerminate() {
    UTLT_Assert(upfContextInitialized == 1, return STATUS_ERROR,
                "UPF context has been terminated!");

    Status status = STATUS_OK;

    // Terminate resource
    // TODO(vivek)
    // IndexTerminate(&upfSessionPool);

    PfcpRemoveAllNodes(&self.upfN4List);
    PfcpNodeTerminate();

    SockNodeListFree(&self.pfcpIPList);
    FreeVirtualDevice(self.envParams->virtualDevice);

    upfContextInitialized = 0;

    return status;
}

Status UpfPDRDeregisterToSessionByID(UpfSession *session, uint16_t id) {
    UTLT_Assert(session, return STATUS_ERROR, "session not found error");
    UTLT_Assert(session->pdr_list, return STATUS_ERROR, "PDR list not initialized");

    list_node_t *node = NULL;
    list_iterator_t *it;
    it = list_iterator_new(session->pdr_list, LIST_HEAD);
    while ((node = list_iterator_next(it))) {
        UpfPDR *pdr_i = (UpfPDR *) node->val;
        if (pdr_i->pdrId == id) {
            break;
        }
    }
    list_iterator_destroy(it);
    UTLT_Assert(node, return STATUS_ERROR, "PDR ID[%u] does NOT exist in UPF Context", id);
    list_remove(session->pdr_list, node);
    return STATUS_OK;
}


UpfDeregResult UpfPDRDeregisterToSessionByIDEx(UpfSession *session, uint16_t id) {
    
    UpfDeregResult  res = { 
        .status = STATUS_ERROR,
        .pdr = NULL
    };

    UTLT_Assert(session, return res, "session not found");
    UTLT_Assert(session->pdr_list, return res, "PDR list not initialized");

    list_node_t *node = NULL;
    list_iterator_t *it = list_iterator_new(session->pdr_list, LIST_HEAD);
    
    while ((node = list_iterator_next(it))) {
        UpfPDR *p = (UpfPDR *)node->val;
        if (p->pdrId == id) {
            res.pdr = p;      // stash it
            break;
        }
    }
    list_iterator_destroy(it);

    UTLT_Assert(node, return res, "PDR ID[%u] does NOT exist", id);
    list_remove(session->pdr_list, node);
    res.status = STATUS_OK;
    return res;
}



Status UpfFARDeregisterToSessionByID(UpfSession *session, uint16_t id) {
    UTLT_Assert(session, return STATUS_ERROR, "session not found error");
    UTLT_Assert(session->far_list, return STATUS_ERROR, "FAR list not initialized");

    list_node_t *node = NULL;
    list_iterator_t *it;
    it = list_iterator_new(session->far_list, LIST_HEAD);
    while ((node = list_iterator_next(it))) {
        UpfFAR *far = (UpfFAR *) node->val;
        if (far->farId == id) {
            break;
        }
    }
    UTLT_Assert(node, return STATUS_ERROR, "FAR ID[%u] does NOT exist in UPF Context", id);
    list_remove(session->far_list, node);
    return STATUS_OK;
}

Status UpfQERDeregisterToSessionByID(UpfSession *session, uint16_t id) {
    UTLT_Assert(session, return STATUS_ERROR, "session not found error");
    UTLT_Assert(session->qer_list, return STATUS_ERROR, "QER list not initialized");

    list_node_t *node = NULL;
    list_iterator_t *it;
    it = list_iterator_new(session->qer_list, LIST_HEAD);
    while ((node = list_iterator_next(it))) {
        UpfQER *qer = (UpfQER *) node->val;
        if (qer->qerId == id) {
            break;
        }
    }
    UTLT_Assert(node, return STATUS_ERROR, "QER ID[%u] does NOT exist in UPF Context", id);
    list_remove(session->qer_list, node);
    return STATUS_OK;
}

Status UpfPDRRegisterToSession(UpfSession *session, UpfPDR *pdr) {
    UTLT_Assert(session, return STATUS_ERROR, "session not found error");
    UTLT_Assert(session->pdr_list, return STATUS_ERROR, "PDR list not initialized");

    list_rpush(session->pdr_list, list_node_new(pdr));
    return STATUS_OK;
}

Status UpfFARRegisterToSession(UpfSession *session, UpfFAR * far) {
    UTLT_Assert(session, return STATUS_ERROR, "session not found error");
    UTLT_Assert(session->far_list, return STATUS_ERROR, "FAR list not initialized");

    list_rpush(session->far_list, list_node_new(far));
    return STATUS_OK;
}

Status UpfQERRegisterToSession(UpfSession *session, UpfQER *qer){
    UTLT_Assert(session, return STATUS_ERROR, "session not found error");
    UTLT_Assert(session->qer_list, return STATUS_ERROR, "QER list not initialized");

    list_rpush(session->qer_list, list_node_new(qer));
    return STATUS_OK;
}

UpfPDR *UpfPDRFindByID(UpfSession *session, uint16_t id) {
    UTLT_Assert(session, return NULL, "session not found error");
    UTLT_Assert(session->pdr_list, return NULL, "PDR list not initialized");

    list_node_t *node;
    list_iterator_t *it;
    it = list_iterator_new(session->pdr_list, LIST_HEAD);
    while ((node = list_iterator_next(it))) {
        UpfPDR *pdr = (UpfPDR *) node->val;
        if (pdr->pdrId == id) {
            list_iterator_destroy(it);
            return pdr;
        }
    }
    list_iterator_destroy(it);
    return NULL;
}

UpfFAR *UpfFARFindByID(UpfSession *session, uint16_t id) {
    UTLT_Assert(session, return NULL, "session not found error");
    UTLT_Assert(session->far_list, return NULL, "FAR list not initialized");

    list_node_t *node;
    list_iterator_t *it;
    it = list_iterator_new(session->far_list, LIST_HEAD);
    while ((node = list_iterator_next(it))) {
        UpfFAR *far = (UpfFAR *) node->val;
        if (far->farId == id) {
            list_iterator_destroy(it);
            return far;
        }
    }
    list_iterator_destroy(it);
    return NULL;
}

UpfQER *UpfQERFindByID(UpfSession *session, uint16_t id){
    UTLT_Assert(session, return NULL, "session not found error");
    UTLT_Assert(session->qer_list, return NULL, "QER list not initialized");

    list_node_t *node;
    list_iterator_t *it;
    it = list_iterator_new(session->qer_list, LIST_HEAD);
    while ((node = list_iterator_next(it))) {
        UpfQER *qer = (UpfQER *) node->val;
        if (qer->qerId == id) {
            list_iterator_destroy(it);
            return qer;
        }
    }
    list_iterator_destroy(it);
    return NULL;
}

UpfSession *UpfSessionAdd(PfcpUeIpAddr *ueIp,
                          PfcpFTeid *teid,
                          uint8_t *dnn,
                          uint16_t dnn_len,
                          uint8_t pdnType) {
    UTLT_Assert(teid, return NULL, "teid is null");
    UTLT_Assert(ueIp, return NULL, "ueIp");
    UpfSession *session = NULL;

    UTLT_Assert(pdnType == PFCP_PDN_TYPE_IPV4 && ueIp->v4 && !ueIp->v6,
                return NULL, "Only IPv4 sessions are supported");
    UTLT_Assert(!teid->ch && teid->v4 && !teid->v6, return NULL,
                "UpfSessionAdd requires a resolved IPv4 F-TEID");
    UTLT_Assert(dnn && dnn_len && dnn_len <= MAX_DNN_LEN, return NULL, "Invalid DNN");
    uint32_t ue_key = rte_be_to_cpu_32(ueIp->addr4.s_addr);
    uint32_t teid_key = rte_be_to_cpu_32(teid->teid);
    UTLT_Assert(!UpfSessionFindByUeIP(ue_key) && !UpfSessionFindByTeid(teid_key),
                return NULL, "UE IP or TEID is already assigned; existing session is unchanged");
    UTLT_Assert(!Self()->workerCount || Self()->nextWorker < Self()->workerCount,
                return NULL, "No unused UPF-U worker; restart all NFs to reset demo assignments");

    session = UpfSessionAlloc(g_sessionIdPool);
    // UTLT_Debug("Session return from UpfSessionAlloc: %p\n", session);
    UTLT_Assert(session, return NULL, "session alloc error");

    memcpy(session->pdn.dnn, dnn, dnn_len);
    session->pdn.dnn[dnn_len] = '\0';

    session->pdr_list = list_new();
    session->far_list = list_new();
    session->qer_list = list_new();
    if (!session->pdr_list || !session->far_list || !session->qer_list)
        goto fail;
    // DumpUpfSession();
    //use to check srr flag
    session->srr_flag = false;

    session->teid = teid_key;
    session->pdn.paa.pdnType = pdnType;
    session->ueIpv4.addr4.s_addr = ue_key;
    if (Self()->workerCount) {
        session->worker = Self()->workers[Self()->nextWorker];
    }

    if (InsertTEIDtoSessionMap(teid_key, session) != STATUS_OK)
        goto fail;
    if (InsertUEIPtoSessionMap(ue_key, session) != STATUS_OK) {
        TeidToUpfSessionMapFree(teid_key);
        goto fail;
    }

    if (Self()->workerCount) {
        Self()->nextWorker++;
        UTLT_Info("Session SEID=%lu UE=%s -> UPF-U service=%u N3 port=%u N6 port=%u",
                    session->upfSeid, inet_ntoa(ueIp->addr4), session->worker.service_id,
                    session->worker.n3_port, session->worker.n6_port);
    }

    /* Create per-session DL buffer ring (eager: before any packets arrive) */
    if (g_sess_buf && UpfSessBufRingCreate(session->index) < 0) {
        UTLT_Warning("SessBuf ring create failed for session index %d", session->index);
    }

    g_sessionIdPool++;
    return session;

fail:
    /* These lists are still empty. Remove only maps inserted by this attempt. */
    if (session->pdr_list) list_destroy(session->pdr_list);
    if (session->far_list) list_destroy(session->far_list);
    if (session->qer_list) list_destroy(session->qer_list);
    UpfSessionFree(session);
    return NULL;
}

Status UpfSessionRemove(UpfSession *session) {
    UTLT_Assert(session, return STATUS_ERROR, "session error");

    /* Destroy per-session DL buffer ring before freeing the session */
    if (g_sess_buf) {
        UpfSessBufRingDestroy(session->index);
    }

    if (!session->far_list) {
        list_destroy(session->far_list);
    }

    if (!session->pdr_list) {
        list_destroy(session->pdr_list);
    }
    UeIpToUpfSessionMapFree(session->ueIpv4.addr4.s_addr);
    TeidToUpfSessionMapFree(session->teid);
    UpfSessionFree(session);
    return STATUS_OK;
}

UpfSession *UpfSessionAddByMessage(PfcpMessage *message, uint8_t *cause) {
    /* Slot configuration is available before dynamic placement is wired in.
     * Never let an inactive pool fall through to legacy session allocation.
     */
    if (Self()->scaling.slot_count) {
        *cause = PFCP_CAUSE_NO_RESOURCES_AVAILABLE;
        return NULL;
    }
    PFCPSessionEstablishmentRequest *request = &message->pFCPSessionEstablishmentRequest;
    *cause = PFCP_CAUSE_MANDATORY_IE_MISSING;
    UTLT_Assert(request->nodeID.presence && request->cPFSEID.presence &&
                request->pDNType.presence && request->createFAR[0].presence,
                return NULL, "Missing session establishment IE");
    *cause = PFCP_CAUSE_INVALID_LENGTH;
    UTLT_Assert(request->cPFSEID.value && request->cPFSEID.len >= PFCP_F_SEID_HDR_LEN &&
                request->pDNType.value && request->pDNType.len == 1,
                return NULL, "Incomplete session identifiers");

    /* Find the N3 PDR by Source Interface, independent of Create PDR order.
     * The demo has one uplink tunnel per session and one Created PDR response. */
    CreatePDR *uplink = NULL;
    unsigned fteid_count = 0;
    for (unsigned i = 0; i < sizeof(request->createPDR) / sizeof(request->createPDR[0]); i++) {
        CreatePDR *pdr = &request->createPDR[i];
        if (!pdr->presence || !pdr->pDI.presence) continue;
        PDI *pdi = &pdr->pDI;
        if (pdi->localFTEID.presence) fteid_count++;
        if (!uplink && pdi->localFTEID.presence && pdi->sourceInterface.presence &&
            pdi->sourceInterface.value && pdi->sourceInterface.len == 1 &&
            (*(uint8_t *)pdi->sourceInterface.value & 0x0f) == 0)
            uplink = pdr;
    }
    *cause = PFCP_CAUSE_MANDATORY_IE_MISSING;
    UTLT_Assert(uplink && uplink->pDRID.presence && uplink->pDI.uEIPAddress.presence &&
                uplink->pDI.networkInstance.presence, return NULL, "Missing uplink PDR identifiers");
    PDI *pdi = &uplink->pDI;
    FTEID *ie = &pdi->localFTEID;
    *cause = PFCP_CAUSE_INVALID_LENGTH;
    UTLT_Assert(uplink->pDRID.value && uplink->pDRID.len == sizeof(uint16_t) &&
                pdi->uEIPAddress.value && pdi->uEIPAddress.len >= 5 &&
                pdi->networkInstance.value && pdi->networkInstance.len &&
                pdi->networkInstance.len <= MAX_DNN_LEN && ie->value && ie->len >= 1,
                return NULL, "Incomplete uplink PDR identifiers");

    /* A CH request contains only flags (and optionally Choose ID), not a TEID. */
    PfcpFTeid *input = ie->value;
    bool choose = input->ch;
    PfcpFTeid resolved = {0};
    *cause = PFCP_CAUSE_INVALID_F_TEID_ALLOCATION_OPTION;
    UTLT_Assert(input->v4 && !input->v6 && choose == (Self()->workerCount != 0),
                return NULL, "Worker mode requires CH=1 IPv4; legacy mode requires an explicit IPv4 F-TEID");
    if (choose) {
        UTLT_Assert(fteid_count == 1, return NULL, "Worker demo supports one N3 F-TEID per session");
        *cause = PFCP_CAUSE_INVALID_LENGTH;
        UTLT_Assert(ie->len == 1 + input->chid, return NULL, "Invalid CH F-TEID length");
        *cause = PFCP_CAUSE_NO_RESOURCES_AVAILABLE;
        UTLT_Assert(Self()->nextWorker < Self()->workerCount, return NULL, "No unused UPF-U worker");
        const UpfWorker *worker = &Self()->workers[Self()->nextWorker];
        resolved.v4 = 1;
        resolved.teid = rte_cpu_to_be_32(worker->ul_teid);
        resolved.addr4 = worker->n3_addr;
    } else {
        UTLT_Assert(!input->chid, return NULL, "Choose ID requires CH=1");
        *cause = PFCP_CAUSE_INVALID_LENGTH;
        UTLT_Assert(ie->len == PFCP_F_TEID_IPV4_LEN, return NULL, "Invalid IPv4 F-TEID length");
        memcpy(&resolved, input, PFCP_F_TEID_IPV4_LEN);
    }

    /* Reserve replacement storage before assigning a session. The PFCP parser
     * owns IE values; expanding the original one-byte CH buffer is unsafe. */
    void *replacement = NULL;
    if (choose) {
        *cause = PFCP_CAUSE_NO_RESOURCES_AVAILABLE;
        replacement = UTLT_Malloc(PFCP_F_TEID_IPV4_LEN);
        UTLT_Assert(replacement, return NULL, "F-TEID allocation failed");
        memcpy(replacement, &resolved, PFCP_F_TEID_IPV4_LEN);
    }

    *cause = PFCP_CAUSE_REQUEST_REJECTED;
    UpfSession *session = UpfSessionAdd(pdi->uEIPAddress.value, &resolved,
                            pdi->networkInstance.value, pdi->networkInstance.len,
                            *(uint8_t *)request->pDNType.value);
    if (!session) {
        if (replacement) UTLT_Free(replacement);
        return NULL;
    }
    session->smfSeid = rte_be_to_cpu_64(((PfcpFSeid *)request->cPFSEID.value)->seid);
    uint16_t pdr_id;
    memcpy(&pdr_id, uplink->pDRID.value, sizeof(pdr_id));
    session->uplink_pdr_id = ntohs(pdr_id);
    session->uplink_teid_allocated = choose;
    if (choose) {
        UTLT_Free(ie->value);
        ie->value = replacement;
        ie->len = PFCP_F_TEID_IPV4_LEN;
        UTLT_Info("Allocated N3 F-TEID: service=%u TEID=%u IP=%s PDR=%u",
                    session->worker.service_id, session->teid,
                    inet_ntoa(session->worker.n3_addr), session->uplink_pdr_id);
    }
    *cause = PFCP_CAUSE_REQUEST_ACCEPTED;
    return session;
}
