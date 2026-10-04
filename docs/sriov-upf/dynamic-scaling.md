# Dynamic UPF-U scale-out

Scope: new IPv4 sessions, multiple sessions per worker, and one UL TEID per
session. UPF-C periodically samples worker RX rings to start workers ahead of
new sessions; admissions select the least-queued READY worker. See the
[proactive scaling README](../../5gc/upf_c/README.md) for the policy, configuration
and timing logs. There is no forecasting, migration or scale-down.
The target is Intel X520/82599 and a Linux DN running iperf.

## Implementation phases

1. Configuration and shared worker registry: committed in `6677bf0`.
2. Acknowledged manager polling/dispatch updates: committed in `06baf8b`.
3. UPF-C admission/spawning and classifier reader registration: implemented.
4. Manager PF filters and deferred PFCP completion: implemented.
5. Baseline testbed verification: the user reported two workers forwarding
   1.45 Mpps each without drops after the MAC/logger fixes. The periodic
   proactive policy still needs testbed verification.

Phases 3–4 were implemented together. No tests were added for these phases,
as requested.

## Configuration

Use [upfcfg.scaling.example.yaml](../../5gc/upf_c/config/upfcfg.scaling.example.yaml).
`configuration.scaling` and the legacy `upf_u_workers` list are mutually
exclusive. The static configuration continues to use its original packet path.

| Setting | Meaning |
| --- | --- |
| `worker_slots` | Preallocated resources. The zero-based array index is the slot ID. |
| `service_id`, `core` | Unique ONVM service and dedicated CPU core per worker; service 2 is UPF-C. |
| `n3_port`, `n6_port` | Manager's DPDK port IDs, obtained from its startup PCI mapping. |
| `n3_pf`, `n6_pf` | Linux PF interface names; both remain bound to `ixgbe`. |
| `n3_vf`, `n6_vf` | Zero-based VF indices under those PFs, independent of DPDK port IDs. |
| `n3_ip`, `n6_ip` | Distinct, fixed IPv4 addresses for the slot. All its sessions share these IPs. |
| `n3_peer_ip`, `n6_peer_ip` | gNB and DN next-hop IPv4 addresses; dynamic workers run without NAT. |
| `min_workers`, `max_workers` | Initial worker count and process limit; defaults 1 and the slot count. |
| `rx_queue_threshold` | Packets; default 40. All READY queues must be strictly above it for consecutive sample rounds. |
| `queue_sample_interval_ms` | Sampling interval; default 10 ms. |
| `queue_consecutive_samples` | Consecutive qualifying rounds before starting one worker; default 3. |
| `teid_first`, `teid_last` | Global, monotonically allocated per-session UL TEID range; defaults `0x1001` through `0xffffffff`. No reuse on failed attempts. |
| `worker_binary` | Absolute executable path to `l25gc_upf_u` on the CN. |
| `file_prefix` | Same EAL namespace as manager and UPF-C; default `rte`. |
| `startup_timeout_ms` | Worker registration/classifier startup deadline; default 30000. This is not a spawning cooldown. |

Remove `dn_route_helper`, `dn_host` and `dn_interface` from older configurations;
these keys are no longer accepted. DN routing is configured manually, once per
UE subnet, and is not part of the worker configuration or admission ACKs.

The NF RX ring is sized at **4096 entries (4095 usable)**; TX rings remain at
65536 entries (65535 usable), and 32 is the packet burst size. The shared
dataplane pool also limits occupancy (`NUM_MBUFS=32767`). The configured
threshold must be below usable RX capacity and the mbuf count. Periodic sampling
starts one unused slot when all READY workers remain above threshold for the
configured number of samples. Only one worker starts at a time, up to
`max_workers`. Admission always selects the least-queued READY worker and never
spawns. If none is READY, it waits within the request timeout. A brief lock
conflict defers the admission read; queues are not subsequently resampled for
an already selected session.

## Setup

1. Rebuild manager and all NFs together. The registry ABI is now **5**, and the
   shared session/PFCP layouts changed. Restart the whole deployment between
   configurations; attaching a replacement UPF-C to a live run is unsupported.
2. Follow the [PF/VF preparation](README.md) for every configured slot. Create
   all VFs before manager startup, include all their BDFs in `ONVM_ALLOW_LIST`,
   enable all their manager ports and reserve distinct NF cores. Keep the
   manager's shared-core sleep mode disabled so idle workers can ACK classifiers.
3. With applications stopped, enable `ntuple` on both PFs and remove previous
   demo flow rules. Inspect with `ethtool -n`; dynamic startup requires both
   filter tables to be empty. Do not install the guide's static N3/N6 filters.
   The manager checks `ixgbe`, VF-to-port PCI identity, ntuple mode and capacity;
   it never creates VFs, enables ntuple or resets devices at runtime.
4. N3 neighbors can be learned through ARP when VF RX and UPF-U's ARP handling
   work; permanent gNB neighbor entries are not required. Apply VF VLAN/trust
   settings before starting manager, and use VF MTU 1500 with standard-MTU PFs.
   A permanent neighbor can isolate ARP faults during diagnosis, but does not
   establish that dynamic resolution works. N6 uses the shared PF gateway below.
5. Assign an N6 IP to the CN's kernel-owned N6 PF, distinct from the worker
   IPs, and install one route for the UE subnet on DN. For the example addresses:

   ```sh
   # On CN; replace N6_PF with its Linux interface name.
   sudo ip address replace 192.168.3.1/24 dev "$N6_PF"
   sudo ip link set dev "$N6_PF" up

   # On DN; DN_LINK is its Linux N6 interface (192.168.3.2/24).
   sudo ip route replace 10.60.0.0/24 via 192.168.3.1 dev "$DN_LINK"
   ip route get 10.60.0.1
   ```

   DN resolves the PF IP through ARP. Return frames use the PF destination MAC
   and the UE destination IP; the NIC's per-UE rule selects the N6 VF before
   delivery to the manager. This PF-MAC/IP-filter path was verified in the
   user's earlier pkt-gen/VF experiment and subsequent UE-to-DN ping/iperf
   on the same machine and NIC. DN needs no per-worker routes or neighbors.
   Remove any old per-UE `/32` routes (including helper routes tagged with
   protocol 242), since they override the subnet route. The manager does not
   connect to DN, install routes, or validate this manual route.
6. Edit the example's paths and PF/VF/port/core/IP values.
   Start manager and other control NFs as in the guide, then start UPF-C with
   this config. For example, if core 8 is reserved for UPF-C:

   ```sh
   sudo ./build/5gc/l25gc_upf_c -l 8 -n 4 --proc-type=secondary --file-prefix rte --no-pci -- -r 2 -m -- -f 5gc/upf_c/config/upfcfg.scaling.example.yaml
   ```

   Do not launch UPF-U manually in dynamic mode. UPF-C starts it as an EAL
   secondary with the configured core/service and `--no-pci`; it obtains
   ports, IPs, next hops and MACs from shared configuration. No per-worker YAML
   is generated. Leave the existing SMF CH=1/Created-PDR endpoint handling enabled.
   Ensure SMF's PFCP retransmission window covers worker startup and NIC steering;
   retaining the UPF-C transaction cannot extend the SMF's own timeout.

## Runtime sequence

UPF-C's `user_actions` callback handles worker startup, child exit, manager ACKs,
pending establishments and PFCP timer expiry. Process launch uses `posix_spawn`;
child status uses `waitpid(WNOHANG)`. No session waits in a blocking loop.
Dynamic mode disables the separate PFCP timer thread; static mode retains it.
Manager services lifecycle and steering requests every millisecond, independently
of its statistics interval;
PF ioctls run outside the packet RX thread.

Before starting a worker, UPF-C registers its classifier-reader generation and
rechecks core/service availability. The worker attaches its slot, initializes
its dataplane, reports its instance and ACKs the current classifier. Manager
installs the N3 filter, then acknowledges activation of both VF ports at an RX
boundary. UPF-C marks the worker READY only after that ACK.

For an establishment, UPF-C retains the parsed PFCP request/transaction, selects
one worker, allocates a unique TEID and builds its session rules. Pending sessions
remain blocked from processing. Manager installs the N6 rule and acknowledges
the local ioctl result; UPF-C waits for that ACK and the owner's classifier ACK,
then caches the success response, enables session processing and sends the N3
IP/TEID to SMF. Retransmissions reuse the same transaction and endpoint.

| Rule | Match/action | Lifetime/location |
| --- | --- | --- |
| N3 PF | Outer destination = worker N3 IP, UDP destination port 2152 → worker N3 VF, queue 0 | One per started worker; location = slot ID |
| N6 PF | IPv4 destination = UE IP, all transport protocols → owner N6 VF, queue 0 | One per session; location = shared session index |
| DN (manual setup) | `UE_SUBNET via CN_N6_PF_IP dev DN_INTERFACE`; ARP resolves the PF MAC | One static route per UE subnet, independent of worker/session lifetime |

The ioctl encodes VF `v`, queue 0 as `(v + 1) << 32`; each PF uses one consistent
filter mask. The implementation follows the
[Linux ixgbe filter API](https://raw.githubusercontent.com/torvalds/linux/v6.8/drivers/net/ethernet/intel/ixgbe/ixgbe_ethtool.c).
N6 return traffic uses the shared PF MAC; the destination-IP filter selects the
VF queue. See the [82599 flow-bifurcation guide](https://doc.dpdk.org/guides-19.11/howto/flow_bifurcation.html#using-flow-bifurcation-on-ixgbe-in-linux).
No NIC TEID match is needed because each worker has its own N3 destination IP.

## Shared ownership and failure behavior

| Consumer | Shared data |
| --- | --- |
| SMF | Existing PFCP Created-PDR N3 IPv4/UL TEID and UPF F-SEID; no worker slot or port IDs. |
| UPF-U | Immutable slot configuration; shared session owner and pending flag; classifier pointer/version; its generation and ACK fields. |
| Manager | Immutable slot configuration; polling mailbox; steering mailbox with operation, slot, generation, session index and UE IPv4. |
| UPF-C only | Child PIDs, startup stages/deadlines, pending PFCP messages, TEID allocator and session placement decisions. |

UPF-C owns runtime state/generation/instance fields; each worker owns its
registration and classifier ACK fields. Reader membership is installed before
spawn and removed only after confirmed process exit. Inactive slots do not block
reclamation. The shared NF lock protects ring lifetime during admission and
periodic sampling reads.

There is one outstanding steering operation at a time, and its result must be
consumed before the next submission. Polling ACKs remain separate from NIC
ACKs. UPF-U continues to use RX/TX rings for all packets, including ARP.

Up to 64 establishments can wait concurrently, each with a deadline of
`startup_timeout_ms + 60000`. Worker or steering failures produce a PFCP rejection.
Submitted mailbox operations are consumed before rollback; they are never
overwritten on timeout. Failed-session rollback removes its N6 filter,
withdraws its PDRs, and waits for all readers before freeing resources.
If cleanup cannot be confirmed, the failed session stays inactive and retains
its UE/TEID/resources until restart. The manual DN route is never modified.

Failed worker slots stop being polled and are not reused; existing sessions are
not moved. Started N3 filters and admitted-session N6 filters persist until
operator cleanup with applications stopped. Before another run, clear old PF
rules. The static DN subnet route can be reused across runs.
The normal session-deletion/scale-down workflow is outside this implementation;
dynamic-mode deletion requests return a PFCP rejection and retain the session.

## Validation status

The proactive sampler/placement and manager-polling mock suites pass locally
with ASan/UBSan. YAML parser tests require libyaml headers, unavailable locally;
the full Linux/DPDK build and hardware behavior require CN. Earlier testbed
results cover the baseline forwarding path, not this periodic policy. Follow
the [proactive experiment steps](../../5gc/upf_c/README.md#quick-experiment)
to verify scale-out before session arrival and collect lifecycle timings.
