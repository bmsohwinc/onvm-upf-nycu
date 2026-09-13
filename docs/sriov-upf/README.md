# Two UPF-U workers on Intel 82599 SR-IOV

This guide extends a working three-node L25GC deployment: node1 runs UE/gNB,
node2 runs ONVM and the control NFs, and node3 runs the existing DN reflector.
Use Bash for the commands below. Replace the example addresses and interface
names with your Fabric allocation.

Each worker directly polls queue 0 of one N3 VF and one N6 VF. The manager
initializes all four VFs but skips their packet RX/TX. UPF-C assigns sessions
in worker-list order and returns that worker's static N3 IP and uplink TEID.
Control NFs continue using their existing ONVM transport.

| Session | Worker service | N3 VF | N6 VF | N3 IP (example) | UL TEID |
| --- | --- | --- | --- | --- | --- |
| First: UE1, `10.60.0.1` | 14 | N3 PF VF0 | N6 PF VF0 | `192.0.2.11` | `0x1001` (4097) |
| Second: UE2, `10.60.0.2` | 15 | N3 PF VF1 | N6 PF VF1 | `192.0.2.12` | `0x1002` (4098) |

Use one IPv4 PDU session and one default UL/DL PDR pair per UE, with NAT
disabled. Connect UE1 before UE2. Worker selection uses arrival order, not a
configured UE-IP allowlist; the preinstalled NIC rules must match that order.
Assignments are not reused. Restart the full application between experiments.

## 1. Record the testbed values

On node2, create an environment file for the following terminals:

```bash
mkdir -p "$HOME/upf-sriov-demo"
cat > "$HOME/upf-sriov-demo/env.sh" <<'EOF'
export UPF_REPO="$HOME/L25GC-plus/NFs/onvm-upf"
export DEMO_DIR="$HOME/upf-sriov-demo"
export N3_PF=ens1f0
export N6_PF=ens1f1
export N3_IP1=192.0.2.11
export N3_IP2=192.0.2.12
export N6_IP1=198.51.100.11
export N6_IP2=198.51.100.12
export GNB_N3_IP=192.0.2.1
export DN_IP=198.51.100.1
export UE1_IP=10.60.0.1
export UE2_IP=10.60.0.2
export N3_VF0_MAC=02:00:00:03:00:01
export N3_VF1_MAC=02:00:00:03:00:02
export N6_VF0_MAC=02:00:00:06:00:01
export N6_VF1_MAC=02:00:00:06:00:02
EOF
# Edit these values before continuing.
source "$HOME/upf-sriov-demo/env.sh"
```

Use distinct, reachable N3 addresses and distinct N6 addresses for the workers.
These addresses belong to UPF-U's packet/ARP handling; do not also assign them
to node2's Linux PF interfaces. Keep AMF/N2 and management addressing as in the
working baseline. Allocate the two static UE IPs through your existing
subscriber/UE setup; `dnn_list.cidr` alone does not assign them.

The commands below use manager cores 0–2, UPF-U1 core 3, UPF-C core 4 and
UPF-U2 core 14, leaving cores 5–13 for the baseline control-NF launch script.
Check `lscpu -e` and the other NFs' actual bindings for conflicts; the example
mask assumes logical CPUs 0–15 are available.
Services 14 and 15 must be unused, with exactly one worker per service/VF pair.

## 2. Build ONVM/UPF and apply the one SMF change

On node2, retain the working L25GC build environment and installed DPDK
24.07, including the ixgbe PMD. For a fresh environment, use
[the dependency/build instructions](../../MANUAL_INSTALL.md); this experiment
uses `vfio-pci` for VFs instead of the legacy PF/`igb_uio` binding steps.
`ethtool`, `iproute2`, `pciutils` and `bc` are also needed.

```bash
cd "$UPF_REPO"
pkg-config --modversion libdpdk
# Activate the existing Meson/Python environment here if your baseline uses one.
if [ -d build/meson-private ]; then
    meson setup --reconfigure build
else
    meson setup build
fi
ninja -C build onvm/logger/liblogger.a
ninja -C build
sudo ldconfig
```

Rebuild manager, UPF-C and UPF-U together: the shared session layout changed.
Do not run old UPF binaries against the new manager/session state.

The companion SMF change is the single commit `9d572b9`, based on `7ee2243`.
Its exact diff is included as [smf-n3-allocation.patch](smf-n3-allocation.patch)
so deployment does not depend on the local `old_ref/` checkout.
From a clean checkout of the matching deployed SMF baseline:

```bash
cd "$HOME/L25GC-plus/NFs/smf"
git status --short
git apply --check "$UPF_REPO/docs/sriov-upf/smf-n3-allocation.patch"
git am "$UPF_REPO/docs/sriov-upf/smf-n3-allocation.patch"
CGO_ENABLED=1 go build -o "$HOME/L25GC-plus/bin/smf" ./cmd
```

Skip `git am` if that SMF change is already applied. Keep the baseline X-IO
replacement, CGO include/link settings and ONVM poller configuration; a plain
upstream SMF environment does not provide this deployment's control transport.

The patch records UPF-C's FTUP capability, requests `CH=1`, consumes the returned
Created PDR and uses its N3 IP/TEID in the N2 setup sent to the gNB. It retains
SMF's local TEID allocator bookkeeping and the gNB-assigned downlink TEID.
Allocation mode advertises one uplink tunnel; no NR-DC setup is required.

Keep **one UPF-C node** in `smfcfg.yaml`, with its existing N4 address and DNN.
Keep the existing valid N3 `interfaces.endpoints` configuration; the patched
SMF uses the returned per-session endpoint for N2 instead of that common
placeholder. Do not add two UPF nodes to SMF. Your AMF `ngapIpList` update
still sets the real node2 N2 IP; no AMF code change is needed.

## 3. Create and bind four VFs on node2

Stop the UEs and all existing ONVM NFs/manager before reconfiguring the NIC.
Both physical functions must be available on node2 with SR-IOV and IOMMU
enabled. In a Fabric VM, PF passthrough must also expose SR-IOV configuration
and usable VF IOMMU groups to the guest.

```bash
ethtool -i "$N3_PF"
ethtool -i "$N6_PF"
cat "/sys/class/net/$N3_PF/device/sriov_totalvfs"
cat "/sys/class/net/$N6_PF/device/sriov_totalvfs"
cat /proc/cmdline
```

Both PF drivers should be `ixgbe`, and each PF must support at least two VFs.
If IOMMU support is missing, enable it in the host/VM setup and boot settings
before continuing (Intel hosts commonly use `intel_iommu=on`).

On a fresh setup with `sriov_numvfs` currently zero:

```bash
echo 2 | sudo tee "/sys/class/net/$N3_PF/device/sriov_numvfs"
echo 2 | sudo tee "/sys/class/net/$N6_PF/device/sriov_numvfs"

sudo ip link set dev "$N3_PF" vf 0 mac "$N3_VF0_MAC"
sudo ip link set dev "$N3_PF" vf 1 mac "$N3_VF1_MAC"
sudo ip link set dev "$N6_PF" vf 0 mac "$N6_VF0_MAC"
sudo ip link set dev "$N6_PF" vf 1 mac "$N6_VF1_MAC"
sudo ip link set dev "$N3_PF" up
sudo ip link set dev "$N6_PF" up

N3_VF0_BDF=$(basename "$(readlink -f "/sys/class/net/$N3_PF/device/virtfn0")")
N3_VF1_BDF=$(basename "$(readlink -f "/sys/class/net/$N3_PF/device/virtfn1")")
N6_VF0_BDF=$(basename "$(readlink -f "/sys/class/net/$N6_PF/device/virtfn0")")
N6_VF1_BDF=$(basename "$(readlink -f "/sys/class/net/$N6_PF/device/virtfn1")")
export ONVM_ALLOW_LIST="$N3_VF0_BDF $N6_VF0_BDF $N3_VF1_BDF $N6_VF1_BDF"
for dev in $ONVM_ALLOW_LIST; do
    test -d "/sys/bus/pci/devices/$dev/iommu_group" || { echo "No IOMMU group: $dev"; exit 1; }
done

sudo modprobe vfio-pci
sudo python3 "$UPF_REPO/subprojects/dpdk/usertools/dpdk-devbind.py" \
    --bind=vfio-pci $ONVM_ALLOW_LIST
sudo python3 "$UPF_REPO/subprojects/dpdk/usertools/dpdk-devbind.py" --status
printf '\nexport ONVM_ALLOW_LIST="%s"\n' "$ONVM_ALLOW_LIST" >> "$DEMO_DIR/env.sh"
```

If there are already two VFs per PF, reuse them and skip writing
`sriov_numvfs`. Check the four BDFs before binding; bind only those VFs.
Inspect `ip -d link show "$N3_PF"` and its N6 equivalent for assigned MACs and
VLANs. Match VF VLAN configuration to the actual Fabric link; the examples
assume untagged Ethernet as seen by the application.

The PFs stay on Linux `ixgbe` to service VF mailbox requests and install
filters. `vfio-pci` exposes the VFs to DPDK's ixgbe VF PMD. VF BDF numbering
can interleave between the two ports; derive it through `virtfnN`, as above.
[DPDK 24.07 Intel VF guide](https://doc.dpdk.org/guides-24.07/nics/intel_vf.html)

## 4. Install the static NIC rules and arrange MAC delivery

Install these rules while the applications are stopped:

```bash
sudo ethtool -K "$N3_PF" ntuple on
sudo ethtool -N "$N3_PF" flow-type udp4 dst-ip "$N3_IP1" dst-port 2152 vf 0 queue 0 loc 0
sudo ethtool -N "$N3_PF" flow-type udp4 dst-ip "$N3_IP2" dst-port 2152 vf 1 queue 0 loc 1
sudo ethtool -K "$N6_PF" ntuple on
sudo ethtool -N "$N6_PF" flow-type ip4 dst-ip "$UE1_IP" vf 0 queue 0 loc 0
sudo ethtool -N "$N6_PF" flow-type ip4 dst-ip "$UE2_IP" vf 1 queue 0 loc 1
sudo ethtool -n "$N3_PF"
sudo ethtool -n "$N6_PF"
```

Use free rule locations if 0/1 are already occupied. Filters on a PF must use
compatible matching masks; inspect existing filters before adding these.
For older ethtool syntax, replace `vf 0 queue 0` with `action 4294967296`, and
`vf 1 queue 0` with `action 8589934592`. Do not use `user-def` as the destination
VF selector. Distinct N3 destination IPs avoid needing GTP/TEID parsing in
the NIC. [Intel ixgbe filter syntax](https://raw.githubusercontent.com/intel/ethernet-linux-ixgbe/main/README),
[Linux v6.8 ixgbe filter implementation](https://raw.githubusercontent.com/torvalds/linux/v6.8/drivers/net/ethernet/intel/ixgbe/ixgbe_ethtool.c)

**82599 Flow Director does not override the VF selected by Ethernet routing.**
Frames must also have the intended VF's destination MAC (or equivalent correct
VLAN/pool delivery). Sending every packet to the PF MAC and adding only the IP
rules above is insufficient. [Intel 82599 datasheet, sections 7.1.2.2 and 7.1.2.7.1](https://www.mouser.com/pdfdocs/82599datasheet.pdf)

On **node1**, use the real gNB N3 interface and the same N3 IP/MAC values:

```bash
# Set these values on node1; N3_LINK may be inside the gNB's network namespace.
N3_LINK=ens1f0
N3_IP1=192.0.2.11
N3_IP2=192.0.2.12
N3_VF0_MAC=02:00:00:03:00:01
N3_VF1_MAC=02:00:00:03:00:02
sudo ip route replace "$N3_IP1/32" dev "$N3_LINK"
sudo ip route replace "$N3_IP2/32" dev "$N3_LINK"
sudo ip neigh replace "$N3_IP1" lladdr "$N3_VF0_MAC" nud permanent dev "$N3_LINK"
sudo ip neigh replace "$N3_IP2" lladdr "$N3_VF1_MAC" nud permanent dev "$N3_LINK"
```

These commands assume the Fabric N3 service provides an L2 path between gNB
and the VFs. If a router is between them, the final hop needs the VF mappings;
node1's neighbor entries cannot control Ethernet addresses across that router.

On **node3**, retain the DN reflector that swaps Ethernet and IP source/
destination addresses. Uplink packets leave with the worker's N6 VF source
MAC; swapping it into the return destination sends the reply to that same VF.
A normal routed DN sender instead needs UE `/32` routes/neighbors toward the
corresponding N6 VF MAC on the final L2 hop.

**ARP is still required for outgoing UPF-U traffic.** With NAT off, UPF-U ARPs
directly for the inner DN destination IP and the gNB N3 IP. Both must be L2
reachable or answered by proxy ARP. Ensure the gNB and DN endpoint answer ARP
and that ARP replies can reach each VF. Node2 Linux routes or `ip neigh`
entries do not populate UPF-U's private ARP cache. An IPv4-only DN reflector
must have an ARP responder available on its dataplane link.

## 5. Start the manager and obtain DPDK port IDs

Retain your baseline hugepage allocation and shared-memory setup. For a fresh
dedicated node, a starting allocation is 1024 2-MB pages; increase it if the
manager cannot allocate its pools, and allocate on the NIC's NUMA node:

```bash
echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
sudo mkdir -p /mnt/huge
mountpoint -q /mnt/huge || sudo mount -t hugetlbfs nodev /mnt/huge
# Retain the baseline's fixed-address multiprocess setup.
sudo sysctl -w kernel.randomize_va_space=0

source "$HOME/upf-sriov-demo/env.sh"
cd "$UPF_REPO"
./scripts/start.sh -k f -D f -n 0xFFF8 -r 32 -s stdout
```

`ONVM_ALLOW_LIST` now contains the **four VF BDFs**, not the original two PFs.
`-k f` initializes DPDK ports 0–3; `-D f` reserves all four for direct I/O.
If using your external manager wrapper, pass `-k f -a "$ONVM_ALLOW_LIST"`
and export `ONVM_DIRECT_PORT_MASK=f` before invoking it.

Stop all previous ONVM processes first: `start.sh` removes old `rtemap_*`
hugepage files. Keep manager/NFs in the same DPDK shared-memory namespace
(default file prefix `rte`). Do not change PF queues, ntuple settings or VF
configuration while the manager/workers run.

Record each startup line `Port N (PCI_BDF): direct NF I/O`, along with
`Rx rings 1` and `Tx rings 1`. **Allowlist order does not establish port ID
order.** In another node2 terminal, substitute the IDs from those lines:

```bash
source "$HOME/upf-sriov-demo/env.sh"
# EXAMPLE ONLY: N3 VF0=0, N3 VF1=1, N6 VF0=2, N6 VF1=3.
cat >> "$DEMO_DIR/env.sh" <<'EOF'
export N3_PORT1=0
export N6_PORT1=2
export N3_PORT2=1
export N6_PORT2=3
EOF
source "$DEMO_DIR/env.sh"
```

## 6. Write the worker and UPF-C configs

On node2, after setting the actual port IDs:

```bash
for worker in 1 2; do
    if [ "$worker" = 1 ]; then
        n3_ip=$N3_IP1; n6_ip=$N6_IP1; n3_port=$N3_PORT1; n6_port=$N6_PORT1
    else
        n3_ip=$N3_IP2; n6_ip=$N6_IP2; n3_port=$N3_PORT2; n6_port=$N6_PORT2
    fi
    cat > "$DEMO_DIR/upf_u_$worker.yaml" <<EOF
info:
  version: 1.0.0
  description: UPF-U worker $worker
configuration:
  log_level: info
  dataplane:
    direct_io: true
    upf_n3_ip: "$n3_ip"
    upf_n6_ip: "$n6_ip"
    ports: {n3_port: $n3_port, n6_port: $n6_port}
  nat:
    enable: false
    an_peer_n3_ip: "$GNB_N3_IP"
    dn_peer_n6_ip: "$DN_IP"
    public_ip: "$n6_ip"
    port_min: 10000
    port_max: 60000
EOF
done

cat > "$DEMO_DIR/upfcfg.yaml" <<EOF
info:
  version: 1.0.0
  description: Two-worker UPF-C
configuration:
  debugLevel: info
  ReportCaller: false
  pfcp:
    - addr: 127.0.0.8
  gtpu:
    - addr: "$N3_IP1"
  dataplane_ports: {access: $N3_PORT1, core: $N6_PORT1, sgi: $N6_PORT1}
  upf_u_workers:
    - {service_id: 14, n3_ip: "$N3_IP1", ul_teid: 0x1001, n3_port: $N3_PORT1, n6_port: $N6_PORT1}
    - {service_id: 15, n3_ip: "$N3_IP2", ul_teid: 0x1002, n3_port: $N3_PORT2, n6_port: $N6_PORT2}
  dnn_list:
    - dnn: internet
      cidr: 10.60.0.0/24
EOF
```

Retain your baseline N4 address, DNN and UE subnet if they differ from this
example. `gtpu.addr` is the common fallback; worker-mode N3 allocation uses
the per-worker `n3_ip`. UPF-C's worker IP/ports must exactly match each UPF-U
config. Each static UL TEID must be nonzero and unique.

## 7. Start UPF-C, both workers and the control NFs

Run this preparation in **each** UPF-C/UPF-U terminal:

```bash
source "$HOME/upf-sriov-demo/env.sh"
cd "$UPF_REPO"
allow_args=()
for dev in $ONVM_ALLOW_LIST; do allow_args+=(--allow "$dev"); done
```

Start UPF-C first, then one command per worker terminal:

```bash
# UPF-C terminal
sudo ./build/5gc/l25gc_upf_c -l 4 -n 4 --proc-type=secondary \
    "${allow_args[@]}" -- -r 2 -m -- -f "$DEMO_DIR/upfcfg.yaml"
```

```bash
# UPF-U1 terminal
sudo ./build/5gc/l25gc_upf_u -l 3 -n 4 --proc-type=secondary \
    "${allow_args[@]}" -- -r 14 -m -- "$DEMO_DIR/upf_u_1.yaml"
```

```bash
# UPF-U2 terminal
sudo ./build/5gc/l25gc_upf_u -l 14 -n 4 --proc-type=secondary \
    "${allow_args[@]}" -- -r 15 -m -- "$DEMO_DIR/upf_u_2.yaml"
```

UPF-U takes a **positional YAML path**; UPF-C uses `-f`. ONVM `-m` is a
manual-core flag selecting the EAL `-l` core, not a mask. The direct binary
commands avoid the external wrappers' `awk -F "--"` parsing of `--allow`.
Each worker must report its intended N3/N6 ports and `RX/TX queue 0`.
The two workers must use the same DPDK allowlist as the manager.

Start the remaining control NFs using the working L25GC scripts/environment,
including the rebuilt SMF. Retain the ONVM poller mappings, `NF_NAME`, X-IO
configuration and core bindings. SMF keeps its existing service ID and UPF-C
route; it does not need routing entries for services 14/15. If a control NF's
EAL configuration probes PCI devices, keep its device list consistent with
the new VF setup. Its control ring transport remains unchanged.

SMF uses its normal `-c`/`-u` Go options, not the C NFs' EAL separators. Its
default configs are relative to the L25GC root. Use the existing SMF launch
script from that directory, ensuring it launches the rebuilt binary once.
Wait for PFCP association and both workers to be running before attaching UEs;
UPF-C's static assignment does not check worker readiness.

## 8. Connect and verify the two sessions

1. Start the ARP-capable DN endpoint/reflector on node3 and the gNB on node1.
   Connect only UE1. Check that it receives the expected static UE IP.
2. UPF-C should log `UE=<UE1_IP> -> UPF-U service=14` and
   `Allocated N3 F-TEID: service=14 TEID=4097 IP=<N3_IP1>`.
   Wait for session establishment and the initial gNB downlink FAR update.
3. Connect UE2. Expect service 15, TEID 4098 and N3 IP2. A third new session
   is rejected because there is no unused worker.
4. Send a low-rate, small UDP stream from UE1 to the existing DN reflector.
   Allow initial ARP resolution; initial packets may drop. Verify return
   traffic before repeating for UE2, then send both streams together.
5. Capture N3 on the gNB side and N6 on the DN side. VF devices bound to
   `vfio-pci` are not Linux capture interfaces on node2. Example on node1:
   `sudo tcpdump -ni "$N3_LINK" -s 0 -w /tmp/two-upfu-n3.pcap 'udp port 2152 or arp'`.

Expected packets:

| Direction | UE1 | UE2 |
| --- | --- | --- |
| N3 uplink | Destination N3 IP1, TEID `0x1001`, destination N3 VF0 MAC | Destination N3 IP2, TEID `0x1002`, destination N3 VF1 MAC |
| N6 uplink | Plain IPv4, source UE1 IP, source N6 VF0 MAC | Plain IPv4, source UE2 IP, source N6 VF1 MAC |
| N6 return | Destination UE1 IP and N6 VF0 MAC | Destination UE2 IP and N6 VF1 MAC |
| N3 downlink | Source N3 IP1, destination gNB, **gNB-assigned DL TEID** | Source N3 IP2, destination gNB, **gNB-assigned DL TEID** |

The downlink TEID need not equal `0x1001`/`0x1002`. UPF-C returns the static
TEID for uplink; the gNB chooses its downlink receive TEID.

ONVM's port counters include direct worker RX/TX, so increasing manager
statistics do not imply its threads carried these packets. Check the startup
reservation/worker logs, packet MAC/IP/TEID values and per-worker counters.
With only UE1 sending, sustained data traffic should increment worker 14;
worker 15 can still receive broadcast ARP. Repeat with only UE2 sending.
Start with small packets (for example 64-byte UDP payloads); leave room for
GTP encapsulation within the link MTU before increasing sizes/rates.

## 9. Common failures and repeat runs

| Symptom | Check |
| --- | --- |
| No VF IOMMU group or VF bind fails | PF passthrough, SR-IOV exposure and host/guest IOMMU setup. |
| VF initialization or secondary attach fails | PFs still on `ixgbe` and up; same DPDK build, allowlist, hugepage mount and namespace; no old manager. |
| Direct-I/O startup rejects a port | Actual BDF-to-port mapping, manager `-k f -D f`, exactly one RX/TX queue per VF. |
| NIC rule rejected | Installed ixgbe/ethtool support, occupied filter locations, matching mask conflicts and valid VF index. Inspect `ethtool -n`. |
| N3 packets arrive at PF but worker gets no traffic | Destination VF MAC/last-hop neighbor mapping, VF VLAN and matching destination N3 IP; IP filters cannot override L2 routing. |
| Worker receives packets but no data exits | ARP for the real gNB/DN address, worker config/ownership match, classifier installation and packet size. Node2 kernel routes do not supply the UPF gateway. |
| PFCP rejects an explicit F-TEID | Rebuilt patched SMF is running and completed a fresh FTUP association. |
| UE2 reaches worker 1 | Static UE IPs, UE attach order, N3 endpoint in N2, and both N6 MAC/IP filters. |

For another run, stop the UE processes, all control NFs and both workers, then
stop the manager. Keep the VF setup/rules if unchanged; restart from step 5
using the same port mapping only after confirming the manager logs. Do not
attempt to reuse a freed worker assignment in this experiment.

## Verification boundary

Local checks covered direct-I/O dispatch, worker ownership, YAML rejection
cases, PFCP retry/allocation, C↔Go PFCP encoding and SMF N2 endpoint encoding.
A focused ASan/UBSan regression installed two sessions with separate UL/DL
PDRs, FARs and default/AMBR QERs, then applied their initial gNB modifications.
Runtime DPDK/hash/transport pieces were mocked in these checks.

The full Linux build, PF/VF initialization, hardware rules and three-node
traffic test still need to run on the testbed. The guide's ixgbe commands are
based on the cited driver documentation/source, not a measured 82599 run.
