/*
# Copyright 2025 University of California, Riverside and National Yang Ming Chiao Tung University
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0
*/

#ifndef __N4_PFCP_HANDLER_H__
#define __N4_PFCP_HANDLER_H__

#include "upf_context.h"
#include "pfcp_message.h"
#include "pfcp_xact.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

Status UpfN4HandleCreatePdr(UpfSession *session, CreatePDR *createPdr);
Status UpfN4HandleCreateFar(UpfSession *session, CreateFAR *createFar);
Status UpfN4HandleCreateQer(UpfSession *session, CreateQER *createQER);
Status UpfN4HandleUpdatePdr(UpfSession *session, UpdatePDR *updatePdr);
Status UpfN4HandleUpdateFar(UpfSession *session, UpdateFAR *updateFar);
Status UpfN4HandleUpdateQer(UpfSession *session, UpdateQER *updateQer);
Status UpfN4HandleRemovePdr(UpfSession *session, uint16_t nPDRID);
Status UpfN4HandleRemoveFar(UpfSession *session, uint32_t nFARID);
Status UpfN4HandleRemoveQer(UpfSession *session, uint32_t nQERID);
Status UpfN4HandleSessionEstablishmentRequest(
        UpfSession *session, PfcpXact *pfcpXact, PFCPSessionEstablishmentRequest *request);
Status UpfN4HandleSessionModificationRequest(
        UpfSession *session, PfcpXact *xact, PFCPSessionModificationRequest *request);
Status UpfN4HandleSessionDeletionRequest(UpfSession *session, PfcpXact *xact, PFCPSessionDeletionRequest *request);
Status UpfN4SendDeletionResponse(uint64_t smf_seid, PfcpXact *xact, uint8_t cause);
Status UpfN4HandleSessionReportResponse(
        UpfSession *session, PfcpXact *xact, PFCPSessionReportResponse *response);
Status UpfN4HandleAssociationSetupRequest(PfcpXact *xact, PFCPAssociationSetupRequest *request);
Status UpfN4HandleAssociationUpdateRequest(PfcpXact *xact, PFCPAssociationUpdateRequest *request);
Status UpfN4HandleAssociationReleaseRequest(PfcpXact *xact, PFCPAssociationReleaseRequest *request);
Status UpfN4HandleHeartbeatRequest(PfcpXact *xact, HeartbeatRequest *request);
Status UpfN4HandleHeartbeatResponse(PfcpXact *xact, HeartbeatResponse *response);
uint32_t UpfClsSafeVersion(void);
void UpfClsCollect(void);
bool UpfClsRebuildAndPublish(uint32_t *version);
uint8_t UpfN4InstallSessionRules(UpfSession *session, PFCPSessionEstablishmentRequest *request);
Status UpfN4SendEstablishmentResponse(UpfSession *session, PfcpXact *xact,
    PFCPSessionEstablishmentRequest *request, uint8_t cause);
int UpfN4AbortPendingSession(UpfSession *session, uint32_t *version);
void UpfN4FreePendingSession(UpfSession *session);
void UpfClsOnAckFree(uint32_t ver, uint16_t service_id);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __N4_PFCP_HANDLER_H__ */
