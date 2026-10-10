# Manager software classification baseline

Branch `sw-lb-upf`, based on `opensource` at `fa54111`. Manager learns sessions
from packets and assigns workers round-robin. UPF-C only gains a session/TEID
log; it does not tell manager about sessions or choose workers. SMF is unchanged.
All workers run the existing UPF pipeline against shared PFCP session/rule state.
There is no hardware worker classification or automatic worker spawning.

```text
N3/N6 shared ports → manager RX: parse + learned lookup → pinned worker RX ring
                  → PDR/FAR + QoS + GTP/L2 processing → worker TX ring
                  → manager TX → N6/N3 shared ports
```

## Dispatch and scope

- Config contains N3/N6 ports/IPs and `worker INSTANCE_ID` lines, in round-robin
  order. No UE IPs or TEIDs are configured. Old `session` lines are rejected.
- A new UE's first data packet selects the next configured instance; with the
  example this is `14 → 15 → 16 → 17 → 14…`. This is **first-packet order**, not
  UE attachment order. Existing sessions stay pinned; packets do not rotate.
- UL on N3: validate outer IPv4/UDP/2152 and the GTPv1-U T-PDU base header and
  lengths, then hash the **UL TEID**. On a TEID miss only, walk any extensions
  and read the inner source UE IPv4 to join the UL/DL keys. Subsequent UL packets
  use TEID alone, with no extension or inner IP/L4 parsing in manager.
- DL on N6: hash destination UE IPv4. If DL arrives first, learn its UE-to-worker
  binding; the first UL supplies the TEID and keeps that same worker. The worker
  still obtains the gNB's DL TEID from the installed FAR.
- Two append-only open-addressed tables hold UL TEID → worker and DL UE IP →
  worker. The single manager RX thread is the only writer; publication to worker
  drain handling uses release/acquire, with no packet-path lock. Maximum 1024
  observed UEs and 32 workers. One stable UL TEID per UE is supported.
- Per-worker counts represent observed UE bindings, not confirmed/active PFCP
  sessions. Manager has no session-deletion information. There is no timeout,
  eviction or migration: reset the deployment before changing/reusing UE IPs or
  TEIDs. A second TEID for an already learned UE is dropped. Valid-looking
  traffic without a PFCP session can consume a binding; UPF-U's PDR lookup still
  decides whether that traffic may be forwarded. Keep experiment traffic controlled.
- All workers use **service ID 1**, with distinct explicit **instance IDs**.
  Manager dispatch bypasses ONVM's RSS-based service selection. Mapped IDs are
  reserved against automatic assignment and unrelated NFs.
- Malformed discovery packets and invalid outer headers drop in manager; when
  the table is full, new sessions drop and existing bindings continue working.
  Other ports retain the ordinary service-chain path. ARP and local ICMP do not
  consume round-robin slots. Local ICMP goes to the lowest configured instance;
  that local ping does **not** measure full UPF forwarding.
- Supported data traffic: untagged, unfragmented IPv4 UDP/TCP/ICMP in contiguous
  mbufs, including IPv4 options and GTP extension chains. IPv6, VLAN-tagged
  input, outer IP fragments, GTP control messages and segmented mbufs are rejected
  by manager. Inner UL fragments remain outside the supported experiment scope.
  NAT is rejected because N6 destination UE IP is the DL dispatch key.

The manager does only ownership classification. The worker still performs PDR
classification, FAR actions, GTP decapsulation/encapsulation, QER policing,
downlink shaping, session buffering, and Ethernet construction. Rules must be
installed through the real SMF/UPF-C; packet learning does not create PFCP sessions.

## Worker coordination and MAC handling

The original UPF-C publishes one shared classifier snapshot and sends updates
to service 1. Each worker now polls that publication at burst boundaries,
including when idle. The lowest configured instance sends the original GC ACK only
after **all configured workers** have stopped using older snapshots. A missing
worker holds back reclamation. Shared classifier lookup remains read-only;
meter/shaper state remains process-local, with each UE pinned to one process.

BUFF→FORW notifications still arrive through service 1. The receiving worker
relays a drain bit to the learned session owner, preserving the
buffer ring's single consumer even when no new packet arrives for that UE.

Source MACs are copied from the manager's port information once at startup.
There is no per-packet NIC query or shared memzone lookup. ARP packets are copied
to each worker so all private neighbor caches learn; only the lowest instance
answers requests for the shared UPF IP. Data packets are not copied. Optional
`n3_peer_mac` / `n6_peer_mac` fix each port's destination MAC, useful with a
DPDK generator or `rxonly` sink that cannot answer ARP. These options assume
one next hop per port. Use the same MAC policy in both architectures.

Start workers before attaching UEs. Keep workers and mappings fixed until the
run ends; worker restart/reuse requires a fresh deployment. Stop traffic before
stopping workers. This branch does not add session migration, concurrent
session deletion safety, or a general PFCP reclamation redesign. Measure steady
sessions after setup/modification has completed; inherited UPF-C mutable-rule
and deletion behavior is outside this static baseline.

## Build and run

For one-command CN startup, use the [software-LB launcher](../../scripts/sw-lb/README.md).
It creates matching run-local configs and starts manager, UPF-C, explicit workers
and control NFs, with per-process tmux windows/logs. The manual commands below
remain useful for individual components.

Build on the existing **Linux DPDK testbed**, using the repository's Meson setup:

```bash
./scripts/build.sh             # initial setup, existing project dependencies
# For an already configured build:
ninja -C build
```

Rebuild and restart manager and UPF processes together. Packet learning changes
the shared LB memory layout (now `UPF_SW_LB_V3`); do not mix old and new binaries.
Only Meson is supported here; the repository's legacy DPDK Makefiles are not
maintained by this change.
Use the original single-endpoint SMF/UPF-C configuration. Do not use the SR-IOV
scaling launcher or the worker-specific N3 endpoint allocation workflow.

1. Use one shared N3 port and one shared N6 port. On the current CN these are
   existing VF0 devices `0000:06:10.0` and `0000:06:10.1`; keep their PFs under
   Linux for N2. All workers share this pair, with no hardware UE-to-worker
   steering. Remove leftover experiment filters directing traffic to other VFs.
   Check EAL's PCI-to-port enumeration and use this pair in secondary allowlists.
2. Copy/edit [map.example.conf](map.example.conf) and
   [upf_u.example.yaml](upf_u.example.yaml). Their N3/N6 ports and IPs must match.
   N3 must also match the endpoint already advertised by your original control
   plane. Every worker uses the same YAML. List just the instances you will
   start, for example `worker 14` through `worker 17`. TEIDs and UE IPs are learned
   while running; no restart or config edit is needed to add a new UE.
3. Run the manager from the repository root. Replace PCI addresses if needed:

```bash
sudo ./build/onvm/onvm_mgr/onvm_mgr \
  -l 0-2 -n 4 --proc-type=primary \
  -a 0000:06:10.0 -a 0000:06:10.1 -- \
  -p 3 -n 0xFFFF8 -s stdout --upf-lb docs/sw-lb-upf/map.example.conf
```

Here `-p 3` enables ports 0/1. The first `-n 4` is EAL memory channels; the
second `-n 0xFFFF8` is ONVM's allowed NF core mask. Packet learning requires the
default single manager RX thread. Keep thread/core counts identical across comparisons.
The wrapper `scripts/start.sh` does not forward `--upf-lb`; use the binary.

Port initialization uses the same VF capability handling as the working SR-IOV
branch: disable RSS for a single RX queue (or no supported RSS hashes), restrict
offloads to device capabilities, use matching port/TX-queue offloads, and set a
1500-byte IP MTU. Startup logs each port's PCI address, driver, queue counts,
RSS mode and offloads; a failed promiscuous-mode request is reported. These
settings remove an initialization difference between baselines; they do not
establish the cause of a remote zero-RX failure without a testbed rerun.

4. Start the original control NFs as usual, and start the four workers below
   in separate terminals. Reserve core 7 for UPF-C and avoid overlapping the
   other control NF cores. These commands replace the original single UPF-U:

```bash
sudo ./build/5gc/l25gc_upf_u -l 3 -n 4 --proc-type=secondary -- -r 1 -n 14 -m -- docs/sw-lb-upf/upf_u.example.yaml
sudo ./build/5gc/l25gc_upf_u -l 4 -n 4 --proc-type=secondary -- -r 1 -n 15 -m -- docs/sw-lb-upf/upf_u.example.yaml
sudo ./build/5gc/l25gc_upf_u -l 5 -n 4 --proc-type=secondary -- -r 1 -n 16 -m -- docs/sw-lb-upf/upf_u.example.yaml
sudo ./build/5gc/l25gc_upf_u -l 6 -n 4 --proc-type=secondary -- -r 1 -n 17 -m -- docs/sw-lb-upf/upf_u.example.yaml
```

Use the same EAL file prefix/base address as the manager if your installation
requires explicit values. Do not enable shared-core sleeping. Wait for all
four instances to be `NF_RUNNING`, then attach UEs. No separate original UPF-U
may run on service 1. To test N workers, list exactly N instance IDs in the
config and start those N workers. Keep total session count and traffic distribution
the same across the software and hardware cases.

5. Send low-rate UL and DL for each UE separately, in the desired round-robin
   order. Manager logs the learned UE, UL TEID, instance and session counts;
   confirm that worker's RX counter advances and the destination receives the
   transformed packet. Check UL decapsulation, DL outer destination and TEID,
   and UE payload/sequence numbers. Then test concurrent UEs and full RTT.
   Warm ARP first or set real peer MACs on both architectures. An `rxonly` DN
   cannot produce RTT replies; use an IP/UDP echo responder with return routing
   for the UE subnet via the UPF's N6 address.

UPF-C logs successful session establishment at info level, for example:

```text
[PFCP] Session established: SEID=1 UE=10.60.0.1 UL_TEID=7 (0x00000007)
```

After that UE sends traffic, manager logs its independent assignment:

```text
UPF LB learned: UE=10.60.0.1 UL_TEID=7 (0x00000007) instance=14 worker_sessions=1 total_sessions=1
```

If DL arrives first, the first manager log says `UL_TEID=pending first UL`; its
next log supplies the learned TEID without incrementing the session count again.
Warm all test sessions before measuring steady-state throughput/RTT. A custom
generator must still use the SMF-allocated TEID to match the real worker PDR;
use the UPF-C log for this. All streams target the same N3 IP and VF MAC.

Classifier rejects are counted in the leader NF's `rx_drop`; absent/full worker
ring drops are counted against that worker. ARP copies and local ICMP add NF
packets, so use generator/receiver **data packet** counters for loss accounting.
Also collect NIC `imissed`, `ierrors`, `rx_nombuf` and TX errors: packets lost
before manager polling never appear in NF counters.

## Recommended experiment

Use two main figures, plus evidence locating the bottleneck:

| Measurement | Procedure | Interpretation |
| --- | --- | --- |
| RTT versus offered load | UE/generator → GTP UL → full UPF → DN echo → full UPF DL → UE/generator; plot p50/p95/p99 and probe loss | Exposes queueing as the manager approaches saturation. Use identical absolute loads, including low load; report probe timeouts, not just surviving replies. |
| Maximum lossless throughput versus workers | 1–4 workers, UL and DL separately; binary-search aggregate offered Mpps | Shows scaling and its ceiling. Use small packets to avoid the link masking CPU limits. |
| Manager and worker cost/backlog | `perf stat`/`perf record`, NIC counters, worker RX ring occupancy at matched loads | Distinguishes software parsing/lookup from worker processing, ring contention, NIC loss, or TX limits. |

RTT includes both UPF directions, links, and the responder. It is not isolated
classification latency, and RTT/2 is not a measured one-way delay. Use a low-rate
timestamped UDP/ICMP probe alongside controlled traffic, timestamp on the same
generator clock, and keep the responder unchanged. Do not use ping to the
UPF's own IP. Report offered and achieved load; a DPDK busy-poll core showing
100% CPU utilization alone does not identify saturation. Profile matched runs
separately from the final latency trials if profiling changes results.

For throughput, search with short trials, then validate the candidate rate for
at least 60 seconds and repeat it (suggested: five trials, report the range).
Require end-to-end sent/received data counts to match after queues drain;
sequence verification also catches duplication/reordering. This follows the
lossless-rate definition and final-duration guidance in
[RFC 2544 §§24, 26.1](https://www.rfc-editor.org/rfc/rfc2544.html#section-26.1).
Label it “zero observed loss over N seconds,” and report rate-search resolution.
Keep warm-up outside the measured interval and disallow steadily growing queues.

Use 64/256/1024-byte **inner IP packets**, including their IP header. State the
actual encapsulation and wire sizes. For 64 inner bytes, the minimum N3 frame
is 114 bytes excluding FCS; with an 8-byte optional/PDU extension it is 122.
Including FCS, preamble/SFD and IFG adds 24 bytes, giving about 9.06 or 8.56 Mpps
on 10 Gbps. The 256-byte inner case with that extension is about 3.70 Mpps.
An observed line-rate plateau is not evidence of classification overhead.

Keep hardware, NUMA placement, CPU frequency, cores, session count, packet
mix, burst size, queue sizes, QER rates and logging constant. This branch uses
4096-entry NF RX rings (4095 usable), matching the SR-IOV branch, with the
existing 65536-entry TX rings and 32-packet bursts. Source-MAC caching and the
disabled-TRACE argument guard are included to avoid penalizing software
dispatch with already-known worker overhead. Set QoS ceilings above the offered
test rates while retaining the actual QoS code path.

One shared VF pair versus per-worker VF pairs, and one shared classifier versus
worker-specific classifiers, remain architectural differences. The comparison measures the
architectures; manager profiles support the narrower classification-cost claim.
The software design may show similar throughput if the workers or physical link
bottleneck first. Report that outcome; do not add artificial work to force a gap.

## Local validation

```bash
python3 tests/sw_lb/run.py
git diff --check
```

ASan/UBSan tests compile the real parser/map implementation and production
manager dispatch/flush and worker coordination bodies, mocking DPDK services.
They cover production port setup with single-queue/no-capability RSS handling,
offload capability limits, matching queue configuration and startup failures;
first-packet learning with IPv4 options and GTP optional/extensions,
round-robin wraparound/counts, UL-first and DL-first affinity, TEID-only warm UL
selection independent of inner headers, both hash tables at capacity,
all truncation offsets, malformed discovery packets, duplicate worker config,
random malformed packets, batch/full/stopped queues,
ARP copy ownership, allocation failure,
MAC validation/caching/static-peer selection and ARP fallback,
normal ONVM fallback with/without `FLOW_LOOKUP`, all-worker GC ACK and ACK retry,
and owner-only drain. They do not replace a Linux Meson build, real PFCP session
test, NIC I/O test or measured RTT/throughput result. This development host lacks
Meson and installed DPDK, so those testbed checks remain to be run.
