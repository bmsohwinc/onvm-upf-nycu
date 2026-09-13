# Scripts

This directory contains various scripts to run and configure different
portions of openNetVM

## Ports reserved for direct NF I/O

`start.sh -D DIRECT_PORTMASK` reserves ports for direct NF RX/TX. The manager
initializes one RX queue and one TX queue (both queue 0) on each reserved port,
but does not receive or transmit packets on it. Other ports keep the existing
manager/ring path. The default mask is `0`.

The direct mask must be a subset of `-k PORTMASK`. For four VF devices assigned
DPDK port IDs 0–3:

```bash
ONVM_ALLOW_LIST="<N3_VF1_PCI> <N6_VF1_PCI> <N3_VF2_PCI> <N6_VF2_PCI>" \
    ./scripts/start.sh -k f -D f -n 0xFFF8 -s stdout
```

Check the manager startup log for the actual device-to-port-ID mapping. Each
reserved port must have exactly one direct-I/O NF polling/transmitting queue 0;
a ring-based NF cannot use these ports through the manager.

`ONVM_DIRECT_PORT_MASK=f` also supplies the mask, allowing an existing wrapper
script to enable it without adding a `-D` argument. An explicit `-D` overrides
the environment variable. The manager publishes the immutable `uint64_t` mask
in `MProc_direct_port_mask` for direct-I/O NFs to check at startup.

## UPF-U direct VF I/O

Set `configuration.dataplane.direct_io: true` in the UPF-U YAML. The existing
`ports.n3_port` and `ports.n6_port` select its two reserved VF ports; both use
RX/TX queue 0. Use a separate config per worker, with disjoint port pairs and
the corresponding N3/N6 IPs. The default `false` keeps the existing ring path.

The manager and both secondary UPF-U processes must receive the same PCI
allowlist. The YAML selects which two ports each worker actually polls.
For example, after starting the manager with the four-VF allowlist:

```bash
# Use the same device list as manager, in each worker's shell.
export ONVM_ALLOW_LIST="<N3_VF1_PCI> <N6_VF1_PCI> <N3_VF2_PCI> <N6_VF2_PCI>"
allow_args=()
for dev in $ONVM_ALLOW_LIST; do
    allow_args+=(--allow "$dev")
done

# Worker 1; run worker 2 in another shell with -l 5, -r 15 and its own YAML.
sudo ./build/5gc/l25gc_upf_u -l 3 -n 4 --proc-type=secondary \
    "${allow_args[@]}" -- -r 14 -m -- /path/to/upf_u_1.yaml
```

`-m` is the ONVM manual-core flag: it uses the EAL core specified by `-l`.
Choose distinct, manager-enabled NF cores and unused service IDs. Existing
wrappers that split arguments on the substring `--` cannot safely forward
`--allow`; the direct binary invocation above avoids that parsing issue.

One bounded burst is read from each VF per event-loop iteration. Normal TX,
ARP replies, and shaper/session-buffer drains transmit on the assigned VF;
partial TX bursts free unsent packets. NF control messages still use ONVM
message queues. Each VF must have only one worker using its queues, and its
PMD must support RX/TX in a DPDK secondary process.

## UPF-C worker ownership

Add this optional list under `configuration` in the UPF-C YAML, using the actual
VF port IDs and worker N3 addresses:

```yaml
upf_u_workers:
  - {service_id: 14, n3_ip: 192.0.2.11, n3_port: 0, n6_port: 1}
  - {service_id: 15, n3_ip: 192.0.2.12, n3_port: 2, n6_port: 3}
```

Run UPF-C as service 2, and exactly one UPF-U per listed service. Each worker's
`direct_io`, N3 IP and port pair must match its UPF-U YAML. All listed VF ports
must be reserved by the manager. Start both workers before connecting the UEs.

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
single-UPF-U configuration and service 1 notifications.

The complete two-UE demo still requires UPF-side TEID allocation, Created PDR
responses, and the matching SMF endpoint/QER changes. PFCP currently uses
SMF-supplied TEIDs; the configured `n3_ip` records/checks worker ownership.
Returning that worker's N3 endpoint to SMF remains the next protocol step.

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
