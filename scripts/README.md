# Scripts

This directory contains various scripts to run and configure different
portions of openNetVM

## Queue capture for dynamic scaling

Follow the current UPF-C INFO log to collect four stable slot columns while
workers start gradually (instance IDs are learned from READY events):

```sh
sudo python3 scripts/qcheck.py --upfc-log run-A/upfc.log --slots 4 \
    --interval 0.1 > run-A/queues.csv
```

Start manager first; use a fresh log/capture for each deployment run. Queue
cells stay blank until a worker is READY. Fixed instance IDs still work:
`sudo python3 scripts/qcheck.py 2 9 --ports 0 1 2 3`. The script also captures
shared mbuf availability, port packet/byte/error counters and their deltas.
See the [two-experiment test plan](../docs/sriov-upf/scaling-experiments.md)
for queue interpretation, startup/load sequencing, plotting and the four-session
lossless UDP comparison.

## Manager VF I/O and UPF-U rings

The manager polls all enabled ports and transmits packets returned through NF
TX rings. UPF-U uses its normal RX/TX rings, including ARP and deferred output.
There is no direct-I/O option or port reservation mask.

The temporary `onvm_port_to_upf_service` array in
[`onvm_pkt.c`](../onvm/onvm_mgr/onvm_pkt.c) maps ingress DPDK port IDs to worker
service IDs: ports **0/2 → 14**, **1/3 → 15**. This assumes N3 VFs are ports 0/1
and N6 VFs are ports 2/3. Check the manager's BDF-to-port startup log, edit the
array to match the actual ports/services, then rebuild/restart the manager.
Zero entries retain normal service-chain dispatch; clear the four defaults
for a legacy deployment. Run exactly one UPF-U per mapped service. Packets
for an absent worker are dropped by normal NF enqueue handling.
UPF-C does not update this manager-local map yet.

```bash
ONVM_ALLOW_LIST="<N3_VF0_PCI> <N3_VF1_PCI> <N6_VF0_PCI> <N6_VF1_PCI>" \
    ./scripts/start.sh -k f -n 0xFFF8 -r 32 -s stdout
```

Each port gets one RX queue per manager RX thread and one TX queue per manager
TX thread. The default three manager cores use one RX and one TX queue per
port. The manager rejects queue counts above a device's capabilities.

Keep the existing per-worker N3/N6 IP and port settings for packet processing,
ARP and session ownership. Manager and secondary NFs use the same PCI allowlist
and DPDK namespace so their port IDs and shared memory agree.

```bash
export ONVM_ALLOW_LIST="<N3_VF0_PCI> <N3_VF1_PCI> <N6_VF0_PCI> <N6_VF1_PCI>"
allow_args=()
for dev in $ONVM_ALLOW_LIST; do
    allow_args+=(--allow "$dev")
done

# Worker 1; run worker 2 in another shell with -l 14, -r 15 and its own YAML.
sudo ./build/5gc/l25gc_upf_u -l 3 -n 4 --proc-type=secondary \
    "${allow_args[@]}" -- -r 14 -m -- /path/to/upf_u_1.yaml
```

`-m` selects the EAL core specified by `-l`. Choose distinct, manager-enabled
NF cores and unused service IDs. Invoke the binary as above when wrappers
cannot safely forward `--allow`.

## UPF-C worker ownership

Add this optional list under `configuration` in the UPF-C YAML, using the actual
VF port IDs and worker N3 addresses:

```yaml
upf_u_workers:
  - {service_id: 14, n3_ip: 192.0.2.11, ul_teid: 0x1001, n3_port: 0, n6_port: 2}
  - {service_id: 15, n3_ip: 192.0.2.12, ul_teid: 0x1002, n3_port: 1, n6_port: 3}
```

Run UPF-C as service 2, and exactly one UPF-U per listed service. Each worker's
N3 IP and port pair must match its UPF-U YAML, and the service/port mapping
must match the manager array. All listed VF ports must be enabled by the
manager. Start both workers before connecting the UEs.

The first new session takes the first worker; the second takes the second.
UPF-C copies ownership into the shared session and uses that worker's ports
when preparing PDRs. Buffer-drain events go only to the owner; classifier
updates reach every configured worker. Old classifier snapshots and retired
PDRs remain allocated until every worker has acknowledged a newer snapshot.
UPF-U rejects packets and drain events belonging to another worker.

PFCP retransmissions replay the cached response before session allocation.
Duplicate UE IPs/TEIDs cannot replace existing mappings, and a new session
cannot consume a worker beyond the configured list. Assignments are not reused;
restart all NFs between demo runs. Omitting the list preserves the existing
single-UPF-U configuration and service 1 notifications; also clear the
manager map to restore service-chain ingress.

`ul_teid` is required for each worker and accepts decimal or `0x` hexadecimal
values in 1–4294967295. TEIDs must be distinct across workers because the shared
uplink map uses TEID as its key. These static values can be used in the NIC
rules installed before startup; UPF-C does not install those rules.

With a worker list, UPF-C advertises FTUP in PFCP Association Setup Response.
SMF must request an IPv4 N3 F-TEID with `CH=1`. UPF-C resolves that request to
the selected worker's `ul_teid` and `n3_ip`, installs the resolved PDR, and
returns the same pair in Created PDR with its uplink PDR ID. The uplink PDR
is identified by Source Interface rather than its position in the message.
This demo supports one N3 F-TEID per session; CH allocation is handled during
establishment. The gNB-assigned downlink TEID in FAR updates remains unchanged.
The CH/Created PDR exchange follows [TS 29.244, section 8.2.3](https://www.etsi.org/deliver/etsi_ts/129200_129299/129244/14.01.00_60/ts_129244v140100p.pdf).

Worker mode rejects explicit SMF-selected F-TEIDs instead of overriding an
endpoint that SMF may already have advertised. Invalid F-TEID allocation requests
and exhausted worker capacity receive a PFCP rejection cached for retries.
Without a worker list, FTUP stays absent and explicit SMF-selected F-TEIDs
continue to work.

The matching SMF change is a separate commit, `9d572b9`, based on the L25GC
SMF baseline `7ee2243`. Apply it to the deployed SMF and rebuild it. It records
FTUP, requests CH, and consumes Created PDR before reporting establishment
success. N2 setup uses the resolved session endpoint and advertises only one
uplink tunnel in allocation mode. SMF's own TEID allocator bookkeeping and
the gNB downlink TEID remain independent of the UPF allocation.

Use one default UL/DL PDR pair per UE for this experiment. Additional PCC
paths and NR-DC are outside this allocation model. The baseline SMF already
creates QERs per session; UPF-C now installs only the supplied QERs and no
longer borrows QER ID 1 from the first session.

Rebuild the manager, UPF-C and both UPF-Us together, then restart them: the
shared `UpfSession` layout changed. Generic ONVM NF structures and other NFs'
ring paths are unchanged.

## Licensing

```
# BSD LICENSE
#
# Copyright(c)
#          2015-2017 George Washington University
#          2015-2017 University of California Riverside
#          2010-2014 Intel Corporation.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#
# Redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer.
# Redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in
# the documentation and/or other materials provided with the
# distribution.
# The name of the author may not be used to endorse or promote
# products derived from this software without specific prior
# written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
