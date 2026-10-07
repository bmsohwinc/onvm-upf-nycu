/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UPF_SCALING_H
#define UPF_SCALING_H
#include "pfcp_xact.h"
#include "onvm_nflib.h"
int UpfScalingInit(void);
/* Takes ownership of the parsed message buffer only on success. */
int UpfScalingEnqueue(Bufblk *message, PfcpXact *xact);
struct _UpfSession;
int UpfScalingDelete(Bufblk *message, PfcpXact *xact, struct _UpfSession *session);
int UpfControlLoop(struct onvm_nf_local_ctx *ctx);
void UpfScalingStop(void);
#endif
