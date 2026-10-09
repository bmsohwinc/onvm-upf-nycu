# Manager software classification baseline

Branch `sw-lb-upf`, based on `opensource` at `fa54111`. No SR-IOV setup,
UPF-C source changes, SMF changes, or automatic worker scaling. All workers run
the existing UPF pipeline against the original shared PFCP session/rule state.

```text
N3/N6 physical port → manager RX: parse + static lookup → pinned worker RX ring
                  → PDR/FAR + QoS + GTP/L2 processing → worker TX ring
                  → manager TX → N6/N3 physical port
```

## Dispatch and scope

- UL on N3: validate outer IPv4/UDP/2152 and the GTPv1-U T-PDU base header and
  lengths, then hash the **UL TEID** to select the worker. Manager does not
  traverse GTP extensions or parse the inner IP/L4 headers; UPF-U does that
  during normal packet processing.
- DL on N6: hash destination UE IPv4 to the same worker. The **UL TEID** belongs
  in the map; the worker gets the gNB's **DL TEID** from the installed FAR.
- Each `session UL_TEID UE_IPV4 NF_INSTANCE_ID` entry populates two separate
  open-addressed tables: UL TEID → worker and DL UE IP → the same worker.
  They are immutable during a run and have no packet-path lock.
  Maximum 1024 unique UE/TEID pairs and 32 workers. Duplicate UE IPs or UL
  TEIDs are rejected. One UL tunnel per UE is supported by this experiment.
- All workers use **service ID 1**, with distinct explicit **instance IDs**.
  Manager dispatch bypasses ONVM's RSS-based service selection. Mapped IDs are
  reserved against automatic assignment and unrelated NFs.
- Unmapped traffic and invalid outer headers on N3/N6 drop in manager. UL inner
  headers and GTP extensions are left to the worker's existing parser and PDR
  lookup. Other ports retain the ordinary service-chain path. Local ICMP to the
  UPF endpoint goes to the lowest mapped
  instance. That local ping does **not** measure full UPF forwarding.
- Supported data traffic: untagged, unfragmented IPv4 UDP/TCP/ICMP in contiguous
  mbufs, including IPv4 options and GTP extension chains. IPv6, VLAN-tagged
  input, outer IP fragments, GTP control messages and segmented mbufs are rejected
  by manager. Inner UL fragments remain outside the supported experiment scope.
  NAT is rejected because N6 destination UE IP is the static dispatch key.

The manager does only ownership classification. The worker still performs PDR
classification, FAR actions, GTP decapsulation/encapsulation, QER policing,
downlink shaping, session buffering, and Ethernet construction. Rules must be
installed through the real SMF/UPF-C; the static map does not create sessions.

## Worker coordination and MAC handling

The original UPF-C publishes one shared classifier snapshot and sends updates
to service 1. Each worker now polls that publication at burst boundaries,
including when idle. The lowest mapped instance sends the original GC ACK only
after **all configured workers** have stopped using older snapshots. A missing
worker holds back reclamation. Shared classifier lookup remains read-only;
meter/shaper state remains process-local, with each UE pinned to one process.

BUFF→FORW notifications still arrive through service 1. The receiving worker
relays a drain bit to the statically mapped session owner, preserving the
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

Build on the existing **Linux DPDK testbed**, using the repository's Meson setup:

```bash
./scripts/build.sh             # initial setup, existing project dependencies
# For an already configured build:
ninja -C build
```

Rebuild and restart manager and NFs together. The TEID lookup table changes the
shared LB memory layout (now `UPF_SW_LB_V2`); do not mix old and new binaries.
Only Meson is supported here; the repository's legacy DPDK Makefiles are not
maintained by this change.
Use the original single-endpoint SMF/UPF-C configuration. Do not use the SR-IOV
scaling launcher or the worker-specific N3 endpoint allocation workflow.

1. Bind the two **data-plane physical ports** to the normal DPDK driver. Keep
   management/control-plane interfaces separate. This baseline uses no VFs or
   hardware worker-steering rules. Check EAL's PCI-to-port enumeration.
2. Copy/edit [map.example.conf](map.example.conf) and
   [upf_u.example.yaml](upf_u.example.yaml). Their N3/N6 ports and IPs must match.
   N3 must also match the endpoint already advertised by your original control
   plane. Every worker uses the same YAML. Get actual UL TEID/UE pairs from
   the original SMF/PFCP logs; the example `0x1001`–`0x1004` values are placeholders.
   Prepare the static values before starting the measured deployment; changes
   require a restart and verification of the new session allocations.
3. Run the manager from the repository root. Replace PCI addresses if needed:

```bash
sudo ./build/onvm/onvm_mgr/onvm_mgr \
  -l 0-2 -n 4 --proc-type=primary \
  -a 0000:06:00.0 -a 0000:06:00.1 -- \
  -p 3 -n 0xFFFF8 -s stdout --upf-lb docs/sw-lb-upf/map.example.conf
```

Here `-p 3` enables ports 0/1. The first `-n 4` is EAL memory channels; the
second `-n 0xFFFF8` is ONVM's allowed NF core mask. There is one manager RX
thread by default. Keep manager thread/core counts identical across comparisons.
The wrapper `scripts/start.sh` does not forward `--upf-lb`; use the binary.

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
four instances to be `NF_RUNNING`, then attach UEs and verify their actual
TEIDs and addresses against the map. No separate original UPF-U may run on
service 1. To test N workers, map all test sessions onto exactly N instance IDs
and start those N workers. Keep total session count and traffic distribution
the same across the software and hardware cases.

5. First send low-rate UL and DL for each UE separately. Confirm exactly its
   mapped worker's RX counter advances and the destination receives the
   transformed packet. Check UL decapsulation, DL outer destination and TEID,
   and UE payload/sequence numbers. Then test concurrent UEs and full RTT.
   Warm ARP first or set real peer MACs on both architectures. An `rxonly` DN
   cannot produce RTT replies; use an IP/UDP echo responder with return routing
   for the UE subnet via the UPF's N6 address.

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

PF versus VF I/O and one shared classifier versus worker-specific classifiers
remain architectural differences. The end-to-end comparison measures the
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
They cover IPv4 options, packets with GTP optional/extensions, TEID-only UL
selection independent of inner headers, UDP/TCP/ICMP UL/DL affinity,
both hash tables at capacity, all truncation offsets, malformed outer lengths,
duplicate config, random malformed packets, batch/full/stopped queues,
ARP copy ownership, allocation failure,
MAC validation/caching/static-peer selection and ARP fallback,
normal ONVM fallback with/without `FLOW_LOOKUP`, all-worker GC ACK and ACK retry,
and owner-only drain. They do not replace a Linux Meson build, real PFCP session
test, NIC I/O test or measured RTT/throughput result. This development host lacks
Meson and installed DPDK, so those testbed checks remain to be run.
