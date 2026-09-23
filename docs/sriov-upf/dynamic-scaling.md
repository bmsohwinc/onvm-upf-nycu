# Dynamic UPF-U scale-out

Scope: new IPv4 sessions, multiple sessions per worker, and one UL TEID per
session. UPF-C reads current worker RX-ring occupancy at admission. There is no
periodic queue sampling, cooldown, session-count threshold, migration or
scale-down. The target is Intel X520/82599 and a Linux DN running iperf.

## Implementation phases

1. Configuration and shared worker registry: committed in `6677bf0`.
2. Acknowledged manager polling/dispatch updates: committed in `06baf8b`.
3. UPF-C admission/spawning and classifier reader registration: implemented.
4. Manager PF filters and deferred PFCP completion: implemented.
5. Linux build and end-to-end verification on the testbed: remaining.

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
| `rx_queue_threshold` | Packets; default 1024. A new worker is requested when every ready worker is at or above it. |
| `teid_first`, `teid_last` | Global, monotonically allocated per-session UL TEID range; defaults `0x1001` through `0xffffffff`. No reuse on failed attempts. |
| `worker_binary` | Absolute executable path to `l25gc_upf_u` on the CN. |
| `file_prefix` | Same EAL namespace as manager and UPF-C; default `rte`. |
| `startup_timeout_ms` | Worker registration/classifier startup deadline; default 30000. This is not a spawning cooldown. |

Remove `dn_route_helper`, `dn_host` and `dn_interface` from older configurations;
these keys are no longer accepted. DN routing is configured manually, once per
UE subnet, and is not part of the worker configuration or admission ACKs.

The NF RX ring is sized at **65536 entries (65535 usable)**; 32 is the packet
burst size. The shared dataplane pool also limits occupancy (`NUM_MBUFS=32767`).
The configured threshold must be below both limits. Admission picks the least
queued ready worker below threshold; otherwise it starts one unused slot or
waits for the worker already starting. At `max_workers`, it uses the least
queued ready worker. A brief lock conflict defers the admission read; queues
are not subsequently resampled for an already selected session.

## Setup

1. Rebuild manager and all NFs together. The registry ABI is now **4**, and the
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
4. On the gNB, preconfigure each pool N3 IP's neighbor entry to its N3 VF MAC,
   using the guide's variables and actual interface, for example:

   ```sh
   sudo ip neigh replace "$N3_IP1" lladdr "$N3_MAC1" nud permanent dev "$RAN_LINK"
   sudo ip neigh replace "$N3_IP2" lladdr "$N3_MAC2" nud permanent dev "$RAN_LINK"
   ```

   These entries retain the existing N3 setup. N6 uses the shared PF gateway
   below; its destination-IP filters select the worker VF.
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
   user's earlier pkt-gen/VF experiment on the same machine and NIC; full UPF
   traffic remains to be verified. DN needs no per-worker routes or neighbors.
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
reclamation. The shared NF lock protects ring lifetime during admission reads.

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

Phases 1–2 had local mock-based checks recorded in the change history. For
phases 3–4 no tests were added or run; their local compiler checks covered the
configuration parser and registry. The manual-DN-routing change has shared-header
and YAML syntax checks plus diff whitespace review, with no tests added.
A full Linux/DPDK build and end-to-end UPF traffic still require the testbed;
Meson and the Linux/DPDK build dependencies are unavailable locally.
