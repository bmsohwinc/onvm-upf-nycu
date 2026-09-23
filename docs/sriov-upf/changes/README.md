# UPF scaling: implementation and commit summary

This document covers all seven scaling commits in `onvm-upf-nycu`, from
baseline `fa54111` through `6b91f82`, and the single companion SMF commit
`9d572b9` on baseline `7ee2243`. See the [deployment guide](../README.md)
for commands, configs and the three-node test procedure.

The current model has one UPF-C assigning sessions to configured UPF-U
workers. The manager polls active N3/N6 VFs, dispatches packets to each
worker's RX ring by ingress port ID, and transmits packets from NF TX rings.
UPF-U uses rings for all packet I/O. Static mode uses a manager-local service
map; dynamic mode uses the configured slot pool and acknowledged poll updates.

Dynamic scale-out is implemented through
[phases 1–4](../dynamic-scaling.md). The opt-in configuration defines a pool of
precreated VF pairs and dedicated cores. UPF-C starts workers on demand from
current RX-ring occupancy, assigns multiple sessions per worker with distinct
TEIDs, and waits for classifier, polling and NIC acknowledgments before
completing PFCP establishment. The static packet path below remains available.

## Manual DN routing

- Remove manager SSH/helper execution, remote route acknowledgments and the
  `scripts/upf-dn-route` helper. Manager still installs local N3/N6 NIC filters
  requested by UPF-C; failed admission rolls back its owned N6 filter.
- Remove `dn_route_helper`, `dn_host` and `dn_interface` from the configuration
  and shared registry. Delete those keys from existing YAML. Registry ABI is
  now 4; rebuild and restart manager and all NFs together.
- Configure one DN route for the UE subnet via the CN's kernel-owned N6 PF IP.
  DN ARPs for the PF MAC; NIC destination-IP filters steer return packets to
  the assigned VF. The user's earlier pkt-gen experiment verified this steering
  on the same machine/NIC. Remove old per-UE routes that override the subnet route.

## Phases 3–4: dynamic admission and steering

The entries below record the original implementation; manual DN routing above
supersedes its helper and remote-route behavior.

- Add `upf_scaling.c`: event-loop startup/admission state, bounded pending PFCP
  requests, monotonic TEIDs, child reaping and failure rollback. Requests already
  selected for a worker are not resampled or moved. At the worker limit, new
  sessions use the least queued ready worker.
- Register classifier readers before spawn, ACK by slot/generation, and remove
  membership only after process exit. Inactive slots do not delay reclamation.
  UPF-U reads shared slot configuration and MACs, so spawning needs no custom YAML
  or PCI polling. Pending sessions remain blocked until success is cached.
- Add manager `onvm_upf_steer.c`: validate kernel ixgbe PFs and VF/DPDK identity,
  install per-worker N3 UDP/2152 and per-UE N6 IPv4 filters through ethtool ioctls,
  and acknowledge the DN helper's route installation. Devices/VFs are initialized
  beforehand; PF configuration is not reset at runtime.
- Add `scripts/upf-dn-route`, installed on CN and DN at the same absolute path.
  It establishes a UE /32 route through the owner's N6 IP/MAC over SSH, and uses
  protocol 242 to distinguish routes eligible for failed-establishment rollback.
- Move dynamic-mode PFCP timers into the UPF-C event loop and retain pending transactions for
  retransmission handling. A shared lock protects NF-ring lifetime during queue
  reads. Manager handles control requests separately from statistics timing.
- Reject dynamic-mode session deletion; teardown and migration remain outside
  this phase. Failed-establishment cleanup reserves resources if DN state is uncertain.
- Bump registry ABI to 3 and update the shared session/PFCP structures: rebuild
  and restart manager and all NFs together. See the dynamic guide for configuration,
  failure limits, prerequisites and remaining Linux/hardware verification.

No tests were added for these phases, per request. Phase 5 is testbed verification.

## Static packet path: manager polling and ring transport

This revision supersedes the direct-I/O behavior in commits `8b3df26` and
`bb36814`. Sections 1–8 below are the historical commit record.

- `onvm_port_to_upf_service` in [onvm_pkt.c](../../../onvm/onvm_mgr/onvm_pkt.c)
  maps ports 0/2 to service 14 and ports 1/3 to service 15, matching the deployment
  guide's example port IDs. Edit/rebuild for different IDs. Zero entries use
  normal service-chain dispatch; clear the defaults for legacy deployments.
  A mapped service must have exactly one UPF-U. Missing workers follow the
  existing NF enqueue drop path; mapped packets do not fall back to another NF.
- Manager RX polls every enabled port. Mapped packets bypass flow/default-chain
  lookup and retain their ingress port. Manager TX uses the ordinary OUT path.
  Queue counts follow manager threads; device queue limits are checked, RSS is
  disabled for one RX queue, and offloads are limited to device capabilities.
- Remove `onvm_direct_io.c/.h`, the NF enable API/hooks, `dataplane.direct_io`,
  `-D`/`--direct-port-mask`, `ONVM_DIRECT_PORT_MASK` and its shared memzone.
  Existing N3/N6 IP/port configuration remains necessary for packet processing.
- Worker ownership no longer requires direct I/O. UPF-C validates distinct,
  manager-enabled ports using the existing shared port information. Its worker
  list must agree with the manager map; no map update protocol is added here.
- Normal NF TX now excludes mbufs retained by a packet handler, including
  shaper/session buffering. ARP replies and deferred packets use the existing
  NF TX ring, avoiding early TX/free and later duplicate submission.
- The deployment and script guides now use manager polling and ring transport.
  Session allocation, PFCP/SMF endpoint handling and classifier ACK behavior
  remain as documented below.

Local verification of this revision: ASan/UBSan passed on extracted packet
functions with mocked DPDK rings/NIC TX, covering dispatch, service-chain
fallback, absent/full worker rings, retained mbufs, deferred output and partial
TX. Flow lookup and NF-side TX handling variants were exercised. Shell syntax
and diff whitespace checks passed. Full Linux compilation and VF traffic
remain testbed checks; Meson/DPDK were unavailable locally.

## Commit map

| Repository | Commit | Change |
| --- | --- | --- |
| Main | `8b3df26` | Reserve VF ports and exclude them from manager packet RX/TX. |
| Main | `bb36814` | Enable UPF-U direct RX/TX on both assigned VFs. |
| Main | `a078b03` | Assign sessions to workers and make shared rules/events work with multiple readers. |
| Main | `38ae2bd` | Allocate each worker's static N3 endpoint through PFCP; repair required codec behavior. |
| SMF | `9d572b9` | Request UPF allocation and advertise the returned session endpoint to the gNB. |
| Main | `f6c30bd` | Keep QERs within their own sessions. |
| Main | `b1fb986` | Correct PDR/FAR registration status and indexed PDR updates. |
| Main | `6b91f82` | Add the Intel 82599 deployment guide and portable SMF patch. |

Use `git show <commit>` in the appropriate repository to inspect an exact
diff. The historical sections below explain those commits; later commits complete
the intermediate worker/TEID support introduced by earlier ones.

## 1. Reserve VF ports in the manager — `8b3df26`

**Reason.** UPF-U and the manager must not poll or transmit through the same
queue. The manager still needs to configure and start each VF because it is
the DPDK primary process. Reservation separates device initialization from
packet processing without disabling the manager's existing paths globally.

| Changed files | Context and implementation |
| --- | --- |
| [scripts/start.sh](../../../scripts/start.sh) | Adds `-D DIRECT_PORTMASK`, forwards `--direct-port-mask`, and accepts `ONVM_DIRECT_PORT_MASK` as its default. Fixes hexadecimal mask handling so `f` can select four VF ports. |
| [onvm_args.c](../../../onvm/onvm_mgr/onvm_args.c), [onvm_init.h](../../../onvm/onvm_mgr/onvm_init.h) | Define/parse `onvm_direct_port_mask`; reject reservations outside the enabled port set. Default mask zero preserves existing behavior. |
| [onvm_init.c](../../../onvm/onvm_mgr/onvm_init.c) | `init()` initializes port state and publishes the mask. `init_port()` configures exactly one RX queue and one TX queue on reserved ports, disables RSS there, limits offloads to device capabilities, and logs PCI-device-to-port-ID mappings. |
| [onvm_common.h](../../../onvm/onvm_nflib/onvm_common.h) | Defines `MZ_DIRECT_PORT_MASK` (`MProc_direct_port_mask`), a separate shared `uint64_t` memzone. Existing generic NF and port structure layouts are preserved. |
| [main.c](../../../onvm/onvm_mgr/main.c) | `rx_thread_main()` skips reserved ports before `rte_eth_rx_burst()`. The thread remains available for other ports. |
| [onvm_pkt_common.c](../../../onvm/onvm_nflib/onvm_pkt_common.c), [onvm_pkt_common.h](../../../onvm/onvm_nflib/onvm_pkt_common.h) | Add mask helpers. Manager enqueue/flush paths reject reserved destinations, including stale buffered packets, so they cannot transmit on a worker's VF. |
| [scripts/README.md](../../../scripts/README.md) | Documents reservation masks, one queue pair per VF and the startup port mapping. |

**Benefit.** The manager can prepare all four VFs while leaving their queues
exclusively to the two workers. Ports outside the reservation keep normal
manager/ring forwarding.

## 2. Direct UPF-U RX/TX — `bb36814`

**Reason.** Replacing only `rte_ring_dequeue_burst()` would cover neither both
traffic directions nor all transmit paths. Ordinary packets, ARP replies and
deferred buffered/shaped packets all need to leave through the owning VF.
The shared NF event loop must retain control-message and callback processing.

| Changed files | Context and implementation |
| --- | --- |
| [upf_u_config.c](../../../5gc/upf_u/upf_u_config.c), [upf_u_config.h](../../../5gc/upf_u/upf_u_config.h), [upf_u.yaml](../../../5gc/upf_u/config/upf_u.yaml) | Add optional `configuration.dataplane.direct_io`, default false, and validate the N3/N6 port IDs and distinct pair. Existing per-worker N3/N6 IP fields are reused. |
| [upf_u.c](../../../5gc/upf_u/upf_u.c) | `main()` explicitly enables direct I/O after parsing the worker's configuration. An invalid/unreserved pair fails startup. Other NF applications do not call this enable API. |
| [onvm_direct_io.c](../../../onvm/onvm_nflib/onvm_direct_io.c), [onvm_direct_io.h](../../../onvm/onvm_nflib/onvm_direct_io.h) | New direct-I/O implementation. `onvm_nflib_enable_direct_io()` verifies reserved, initialized ports with one RX/TX queue each. `onvm_direct_io_rx()` alternates the two queue-0 polls and initializes ONVM packet metadata. `onvm_direct_io_tx()` groups output by the assigned port and uses `rte_eth_tx_burst()`. |
| [onvm_nflib.c](../../../onvm/onvm_nflib/onvm_nflib.c), [onvm_nflib.h](../../../onvm/onvm_nflib/onvm_nflib.h) | Expose the enable API and select direct RX only for the opted-in NF. The main loop polls one bounded burst from each VF per iteration, continues messages/callbacks, and avoids sleeping based on an unused RX ring. `onvm_nflib_return_pkt_bulk()` also selects direct TX. |
| [onvm_pkt_common.c](../../../onvm/onvm_nflib/onvm_pkt_common.c) | Route both normal TX batches and accumulated NF TX buffers through direct TX when enabled. Ring behavior remains the default. |
| [upf_u_arp.c](../../../5gc/upf_u/upf_u_arp.c) | Log saved ARP address values after packet submission; avoid dereferencing an mbuf after TX may have consumed/freed it. |
| [onvm_nflib/meson.build](../../../onvm/onvm_nflib/meson.build) | Compile and expose the new direct-I/O implementation/header. |
| [scripts/README.md](../../../scripts/README.md) | Document per-worker configs, matching primary/secondary allowlists and direct binary launch arguments. |

Direct-I/O selection and its port pair are process-local. Packet handlers can
still retain mbufs; retained packets are excluded from immediate TX. Partial
NIC transmissions free the unsent mbufs and update drop counters. An invalid
direct output action/destination is dropped rather than handed to the manager.

**Benefit.** Both directions bypass the manager's packet rings, including ARP
and deferred output. The existing packet processing and control interface are
reused. This removes the queue handoffs; a throughput/latency improvement still
needs measurement on the testbed.

## 3. Session ownership and shared rules — `a078b03`

**Reason.** Multiple UPF-Us need consistent ownership across session lookup,
PDR preparation, QoS state and buffer-drain events. They also read shared
classifier snapshots: an ACK from worker 1 alone cannot establish that worker
2 has stopped using an old snapshot.

| Changed files | Context and implementation |
| --- | --- |
| [upf_config.c](../../../5gc/upf_c/upf_config.c), [upfcfg.yaml](../../../5gc/upf_c/config/upfcfg.yaml) | Add ordered `upf_u_workers` entries and validate distinct service IDs, N3 IPs and disjoint reserved VF pairs. The static `ul_teid` field is added by the next commit. |
| [upf_context.h](../../../onvm/upf/upf_context.h) | Add `UpfWorker`, the configured worker array/count/cursor in `UpfContext`, and a value copy of the owner in shared `UpfSession`. No session points into UPF-C's private configuration. |
| [upf_context.c](../../../onvm/upf/upf_context.c) | `UpfSessionAdd()` assigns the next unused worker, initializes per-session rule lists, rejects duplicate UE-IP/TEID mappings, and cleans up a failed partial allocation. The cursor advances only after the session maps are inserted. |
| [onvm/upf/upf.c](../../../onvm/upf/upf.c) | Initialize new session slots. UE-IP and TEID lookups now use the session index stored as hash data, instead of treating each independent hash table's slot number as a session index. Correct the UE-IP deletion hash as well. |
| [n4_onvm_pfcp_handler.c](../../../5gc/upf_c/n4_onvm_pfcp_handler.c) | `UpfPdrPrecompileSdf()` uses the owner's ports. FAR transitions from buffering to forwarding notify that owner. `UpfClsRebuildAndPublish()` retains every retired snapshot and notifies all workers; `UpfClsOnAckFree()` frees snapshots/PDRs only up to the minimum acknowledged version across workers. |
| [n4_onvm_pfcp_handler.h](../../../5gc/upf_c/n4_onvm_pfcp_handler.h), [n4_onvm_pfcp_path.c](../../../5gc/upf_c/n4_onvm_pfcp_path.c) | Pass the worker service ID with classifier ACKs to the reclamation handler, and release consumed ACK event objects. |
| [upf_events.h](../../../onvm/utlt/upf_events.h) | Add `UpfSendEvt2()` for event payloads containing both classifier version and worker service ID. |
| [upf_u.c](../../../5gc/upf_u/upf_u.c) | `UpfSessionIsLocal()` checks service, VF pair and N3 IP before configuring QoS or processing/draining a session. `UpfClsMaybeFlipAndAck()` observes publications even when idle or notifications are lost, and retries ACKs that could not be enqueued. |
| [n4_dispatcher.c](../../../5gc/upf_c/n4_dispatcher.c), [pfcp_xact.c](../../../onvm/pfcp/pfcp_xact.c) | Check/replay PFCP transactions before allocating a worker. Keep cached transactions alive on `STATUS_EAGAIN`, so repeated retries do not allocate more sessions or consume the next worker. |
| [5gc/upf_c/upf.c](../../../5gc/upf_c/upf.c) | Exit when `UpfInit()` fails instead of entering the event loop with incomplete configuration/state. |
| [scripts/README.md](../../../scripts/README.md) | Document assignment order, worker startup requirements, shared-layout rebuilds and restart-between-runs behavior. |

**Benefit.** UE1 and UE2 get separate owners, correct session/rule lookups and
worker-local packet/QoS handling. One worker cannot prematurely free shared
rules still being read by another. PFCP retransmission does not consume the
limited worker list.

This is a fixed assignment model: one new session per configured worker in
arrival order, with no assignment reuse or worker-health selection. The same
code iterates the configured list; the current deployment exercises two.

## 4. UPF-C allocates the N3 endpoint — `38ae2bd`

**Reason.** Direct VF I/O alone cannot give the gNB different N3 destinations.
UPF-C must return the chosen worker's endpoint during session establishment,
and the installed uplink rule must agree with that endpoint. Static allocation
allows the experiment's NIC rules to be prepared before application startup.

| Changed files | Context and implementation |
| --- | --- |
| [upf_config.c](../../../5gc/upf_c/upf_config.c), [upfcfg.yaml](../../../5gc/upf_c/config/upfcfg.yaml) | Require a nonzero, distinct `ul_teid` for each worker; accept decimal or `0x` notation. Uniqueness is necessary because the shared uplink session map is keyed by TEID. |
| [upf_context.h](../../../onvm/upf/upf_context.h) | Extend `UpfWorker` with the static UL TEID and `UpfSession` with the uplink PDR ID and allocation flag used to construct the response. |
| [upf_context.c](../../../onvm/upf/upf_context.c) | `UpfSessionAddByMessage()` locates the uplink PDR by Source Interface instead of assuming array position zero. Resolve its IPv4 `CH=1` F-TEID to the next worker's configured TEID/IP. Allocate correctly sized replacement IE storage before replacing the short CH request value. Return a PFCP cause when allocation fails. |
| [n4_onvm_pfcp_build.c](../../../5gc/upf_c/n4_onvm_pfcp_build.c) | Advertise FTUP in Association Setup Response when workers are configured. Add Created PDR containing the matching PDR ID and allocated N3 F-TEID to successful establishment responses. Also correct the IPv6-address argument and check the result when encoding UPF F-SEID. |
| [n4_onvm_pfcp_handler.c](../../../5gc/upf_c/n4_onvm_pfcp_handler.c) | `CheckSessionFTeid()` requires a resolved endpoint before rule conversion and rejects worker-mode PDR creations/updates that contradict the session's allocated TEID/IP. |
| [n4_dispatcher.c](../../../5gc/upf_c/n4_dispatcher.c) | Build and cache rejection responses for invalid allocation requests or exhausted worker capacity, preserving transaction retry behavior. |
| [pfcp_message.c](../../../onvm/pfcp/pfcp_message.c) | Correct grouped-IE length/bounds handling and parse-error propagation. Access packed presence values safely and recompute the group-length destination after buffer growth, avoiding writes through a stale buffer pointer. These fixes support the actual C/Go allocation exchange. |
| [scripts/README.md](../../../scripts/README.md) | Document static TEIDs, FTUP/CH/Created PDR, the single-tunnel scope and required SMF integration. |

FTUP advertises the UPF's F-TEID allocation capability; CH asks it to choose
the endpoint. Worker mode requires this exchange and rejects an explicit
SMF-selected endpoint. Without a worker list, UPF-C keeps the legacy explicit
F-TEID behavior and does not advertise FTUP.

**Benefit.** The worker's shared session, uplink classifier rule and PFCP
response agree on one static TEID/N3 IP pair. SMF can convey that pair to the
gNB while continuing to communicate with one UPF-C.

## 5. SMF consumes and advertises that endpoint — `9d572b9`

**Reason.** The deployed SMF baseline allocated its own uplink TEID and used
the common configured N3 interface address for N2 setup. It needed to request
UPF allocation and consume Created PDR before reporting session success;
otherwise the gNB would send traffic to an endpoint different from the worker's.

These are the **only seven changed files in the SMF repository**. Links below
refer to the local `old_ref/smf` checkout. That checkout is not a runtime
dependency or tracked content of the main repo. The exact transferable diff
is [smf-n3-allocation.patch](../smf-n3-allocation.patch).

| Changed SMF file | Context and implementation |
| --- | --- |
| [internal/context/upf.go](../../../old_ref/smf/internal/context/upf.go) | Store the associated UPF's FTUP capability in its existing context. |
| [internal/sbi/processor/association.go](../../../old_ref/smf/internal/sbi/processor/association.go) | Read FTUP from the accepted Association Setup Response. |
| [internal/context/datapath.go](../../../old_ref/smf/internal/context/datapath.go) | `ActivateTunnelAndPDR()` sets the N3 uplink PDR's F-TEID to IPv4 `CH=1` when the UPF supports allocation. |
| [internal/context/n3_endpoint.go](../../../old_ref/smf/internal/context/n3_endpoint.go) | New `ApplyCreatedN3PDR()` matches the returned PDR ID to this session's pending N3 allocation, validates the endpoint, copies its IP bytes and updates the existing PDR/tunnel. `N3Endpoint()` returns that resolved endpoint, or the configured endpoint in legacy mode. |
| [internal/sbi/processor/datapath.go](../../../old_ref/smf/internal/sbi/processor/datapath.go) | `establishPfcpSession()` consumes Created PDR before signaling success; missing or invalid allocations cause establishment failure. |
| [internal/context/ngap_build.go](../../../old_ref/smf/internal/context/ngap_build.go) | Build N2 resource setup from the resolved session IP/TEID. Allocation mode advertises one uplink tunnel and omits the baseline's additional NR-DC tunnel advertisement. |
| [internal/context/n3_endpoint_test.go](../../../old_ref/smf/internal/context/n3_endpoint_test.go) | Add tests for distinct session endpoints/N2 encoding, invalid allocations, copied response data and legacy endpoint selection. |

**Benefit.** The gNB receives N3 IP1/TEID1 for UE1 and N3 IP2/TEID2 for UE2.
SMF's topology still contains one UPF-C; worker selection stays in UPF-C.

SMF's `LocalULTeid` remains its own allocator bookkeeping value. The allocated
N3 endpoint is stored in the PDR/uplink tunnel, avoiding release of a
UPF-assigned static ID through SMF's allocator. The gNB's downlink receive
TEID still arrives through the existing FAR update path. Existing per-session
QER generation, PFCP dependencies and the FTUP-absent path are retained.

## 6. Keep QERs session-local — `f6c30bd`

**Reason.** UPF-C had an empty-CreateQER fallback that borrowed QER ID 1 from
the first session. The current SMF already provides per-session QERs, so the
fallback could incorrectly share QoS rule ownership across the two workers.

| Changed files | Context and implementation |
| --- | --- |
| [n4_onvm_pfcp_handler.c](../../../5gc/upf_c/n4_onvm_pfcp_handler.c) | Install only present CreateQER entries and remove the first-session fallback. Compare QER registration explicitly against `STATUS_OK`. |
| [upf_context.c](../../../onvm/upf/upf_context.c) | `UpfQERRegisterToSession()` returns `STATUS_OK` after inserting the QER, instead of falling through a non-void function. |
| [scripts/README.md](../../../scripts/README.md) | Update the documented SMF integration to the final single commit and clarify that the baseline already supplies per-session QERs. |

**Benefit.** Each session uses its own supplied QERs, including default/QFI
and AMBR rules. Successful rule registration has a defined result. No extra
SMF QER-allocation change was needed.

## 7. Correct PDR/FAR registration and modification — `b1fb986`

**Reason.** PDR/FAR registration had the same missing-return/status-test
problem as QER registration. The modification loop also passed the whole
UpdatePDR array address instead of the current entry. Even a connect-once
experiment needs the initial modification that installs the gNB's downlink
forwarding information.

| Changed files | Context and implementation |
| --- | --- |
| [upf_context.c](../../../onvm/upf/upf_context.c) | `UpfPDRRegisterToSession()` and `UpfFARRegisterToSession()` explicitly return `STATUS_OK` after insertion. |
| [n4_onvm_pfcp_handler.c](../../../5gc/upf_c/n4_onvm_pfcp_handler.c) | Creation handlers compare registration results with `STATUS_OK`. The modification loop passes `&request->updatePDR[i]`. Remove an inactive `CHECK` block that attempted to re-register an existing PDR through an incorrect pointer. |

**Benefit.** Session rule installation no longer depends on undefined return
values, and each modification handles the intended PDR. Both sessions can
receive their independent initial gNB updates.

## 8. Deployment instructions and transferable SMF patch — `6b91f82`

**Reason.** The demo requires coordinated PCI allowlists, VF/DPDK port IDs,
worker YAML, core assignments, hardware steering and SMF deployment. Code
changes alone do not establish those relationships on the three nodes.

| Changed files | Context and implementation |
| --- | --- |
| [docs/sriov-upf/README.md](../README.md) | Full setup procedure: Intel 82599 PF/VF drivers, four-VF creation, static filters, ARP reachability, builds, configs, startup order, captures and troubleshooting. |
| [smf-n3-allocation.patch](../smf-n3-allocation.patch) | Exact `git format-patch` export of the one SMF commit. Lets the deployed SMF repository apply the change without copying or building `old_ref/`. |
| [README.md](../../../README.md) | Link the deployment guide from the repository entry point. |
| [scripts/README.md](../../../scripts/README.md) | Use core 14 in the second-worker example to avoid the baseline control-NF script's core-5 assignment. |

**Benefit.** The implementation has a reproducible deployment procedure and
a portable SMF change. Its original VF-MAC requirement is corrected by the
manual DN routing update above. The guide distinguishes software checks from
full UPF operation that still needs testbed verification.

## How the pieces work together

1. Manager initializes and polls four VF ports. Its local port-to-service map
   selects the UPF-U RX ring for both ports of each worker.
2. SMF associates with the single UPF-C, learns FTUP and requests an IPv4 N3
   endpoint using CH during session establishment.
3. UPF-C selects the next worker, copies ownership into the shared session,
   resolves its static endpoint, installs rules and returns Created PDR.
4. SMF puts the returned endpoint into N2 setup. The gNB sends that session's
   uplink traffic to the assigned N3 IP/TEID. Its own downlink TEID is installed
   by the subsequent PFCP modification.
5. The preconfigured NIC delivery sends uplink to the worker's N3 VF and
   return traffic to its N6 VF. The manager polls both and enqueues packets
   to that worker's RX ring. After UPF processing, packets leave through the
   worker's TX ring and the manager transmits on the selected output port.
6. UPF-C sends session drain events to the owner, but classifier publications
   reach every worker. Reclamation waits for all readers' acknowledgements.

## Scope, compatibility and checks

- The model is one static IPv4 session per worker, tested locally with two
  workers and one default UL/DL PDR pair per UE. NAT is disabled for the demo.
  Worker assignment is not reused; restart all NFs/manager between runs.
- Dynamic NIC rule installation, worker health/load balancing, session
  migration/reconnect, additional PCC paths and NR-DC support are outside this
  implementation. There is no UPF-LB in the data path. `old_ref/5gc` was used
  only for reference and was not modified.
- Generic NF/port shared layouts and other NFs' ring paths are preserved.
  `UpfSession` did change: rebuild and restart manager, UPF-C and UPF-U together.
  The existing shared session/classifier infrastructure is reused.
- Local checks covered direct dispatch and mbuf ownership, configuration
  rejection, multi-worker ownership/classifier ACKs, PFCP retries, C/Go PFCP
  interoperability and N2 endpoint encoding. A focused ASan/UBSan run installed
  two sessions with separate PDRs/FARs/QERs and applied their initial gNB
  modifications. DPDK/runtime pieces were mocked where required.
- Deployment-guide Bash/config generation and application of the SMF patch
  were checked. Full Linux compilation, manager VF RX/TX, hardware
  steering and three-node traffic/performance remain testbed checks. Temporary
  harness files were not committed; the SMF endpoint tests are in its commit.
