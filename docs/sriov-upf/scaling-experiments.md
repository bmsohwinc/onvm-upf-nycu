# Measuring admission-driven scale-out

These are manual testbed experiments, not automated tests. First establish
where throughput is limited; adding UPF-U cores only helps when UPF-U processing
is the bottleneck. One UE's TCP throughput is not a per-UE capacity guarantee.
The existing manager RX/TX threads, gNB, NICs and DN remain shared resources.

## 1. Record the admission decision

After transferring the updated `5gc/upf_c/upf_scaling.c` to CN:

```sh
cd "$HOME/onvm-upf-scaling"
ninja -C build 5gc/l25gc_upf_c
```

Restart the deployment using the existing shutdown/filter-cleanup/startup
procedure; do not attach a replacement UPF-C to the current session state.
Keep `debugLevel: info`, `min_workers: 1`, `max_workers: 4`, and initially
`rx_queue_threshold: 1024`. Capture UPF-C stdout/stderr in a file, then:

```sh
rg 'Admission (queue|decision)|Spawned UPF-U|READY|Admitted SEID' /path/to/upfc.log
```

Illustrative output, not a measured result:

```text
Admission queue: xid=42 xact=7 slot=0 instance=2 service=14 rx=1400 capacity=65535 threshold=1024
Admission decision: xid=42 xact=7 ready=1 best_slot=0 min_rx=1400 selected_slot=1 reason=spawn_threshold spawn_error=0
...
UPF-U slot=1 instance=14 READY
Admitted SEID=3 slot=1 TEID=4099 xid=42 xact=7
```

`xid` and `xact` connect the decision to the admitted session. Instance IDs are
assigned by manager; slot 1 need not have instance ID 14. Queue logs contain
the actual values used by the decision, copied before logging; counts across
workers are successive reads, not an atomic snapshot. Logging happens outside
the NF lock and adds no packet-path or periodic control-loop logging.

| Reason | Meaning |
| --- | --- |
| `below_threshold` | At least one ready worker was below threshold; choose the least queued. |
| `spawn_threshold` | Every ready worker met/exceeded threshold; spawn another worker. |
| `spawn_no_ready` | Spawn because there was no valid ready worker. |
| `wait_starting` | No ready worker was below threshold; use the worker already starting. |
| `worker_limit` | Process limit or available slot pool exhausted; use the least queued ready worker if one exists. |
| `spawn_failed` | Startup could not be initiated; inspect the negative errno and chosen fallback. |

With `ready=0`, `best_slot=-1` and `min_rx=0` mean no ready queue was sampled.
`spawn_threshold` confirms process launch, while READY and Admitted confirm
startup and session admission completed. The subsequent traffic must also work.

Creating all UEs before traffic normally places them on slot 0. Ties pick the
first slot, so `min_workers: 2` alone does not distribute idle sessions. Starting
more iperf flows within an existing PDU session does not trigger admission;
existing sessions never migrate. TCP backoff and instantaneous sampling can
both prevent the threshold from being observed even during a throughput plateau.

## 2. Observe queues and CPU threads before changing the threshold

On CN, sample the current worker's RX and TX rings for 30 seconds. Set `iid` to
its **instance ID**, not service ID or slot. This works with the testbed's DPDK
24.11 telemetry and does not require rebuilding or restarting applications.

```sh
sudo python3 - <<'PY' > worker-queues.csv
import json, socket, time
iid = 2
with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as s:
    s.settimeout(5)
    s.connect('/var/run/dpdk/rte/dpdk_telemetry.v2')
    s.recv(65536)
    print('time_ns,instance,rx,tx,rx_capacity', flush=True)
    for _ in range(300):
        counts = []
        for direction in ('RX', 'TX'):
            s.sendall(f'/ring/info,MProc_Client_{iid}_{direction}'.encode())
            info = json.loads(s.recv(65536))['/ring/info']
            counts.append(info['used_count'])
            if direction == 'RX':
                capacity = info['capacity']
        print(time.time_ns(), iid, *counts, capacity, sep=',', flush=True)
        time.sleep(0.1)
PY
```

These samples can miss sub-100-ms bursts; admission logs are authoritative
about what triggered placement. DPDK documents the fields in its
[ring telemetry implementation](https://github.com/DPDK/dpdk/blob/v24.11/lib/ring/rte_ring.c).

While traffic runs, use these commands on node0 and CN (`pidstat`/`mpstat` need
the `sysstat` package). Use `pgrep` output to substitute real PIDs below:

```sh
pgrep -af 'nr-gnb|nr-ue|iperf3|onvm_mgr|l25gc_upf_u'
pidstat -t -u -C 'nr-gnb|nr-ue|iperf3|onvm_mgr|l25gc_upf_u' 1 30
mpstat -P ALL 1 30
taskset -apc <PID>
```

Check DN's iperf CPU usage too. An unpinned process can have one saturated
thread even when most cores are idle. Keep gNB, UE and iperf processes off
each other's busy cores when repeating measurements; inspect node0's topology
before choosing affinities. Reserve CN cores 0–2 for manager, 3–6 for workers,
7 for UPF-C, and keep control NFs off worker cores and their SMT siblings.

DPDK polling threads use CPU while idle, so 100% CPU alone does not establish
a UPF bottleneck. If needed, profile worker and manager separately:

```sh
sudo perf record -F 99 -g -p <UPFU_PID> -o /tmp/upfu.perf -- sleep 15
sudo perf report --stdio -i /tmp/upfu.perf
```

Distinguish packet-processing work from empty-ring polling. Also check subscriber
session-AMBR/QER MBR settings; hold them identical across runs and ensure a QoS
cap is not the plateau being measured. UPF-C's debug log reports `QER MBR`, and
UPF-U logs installed downlink `AMBR`, `GBR`, and `MBR` where applicable.

## 3. Use controlled offered load, then admit another UE

On DN, retain one server process per UE/port, as in the current setup. Start
missing servers in separate terminals:

```sh
iperf3 -s -B 10.10.3.2 -p 5201
iperf3 -s -B 10.10.3.2 -p 5202
iperf3 -s -B 10.10.3.2 -p 5203
```

Initially register UE1 only. On node0, substitute its actual TUN address:

```sh
UE1_IP=10.60.0.1
iperf3 -c 10.10.3.2 -p 5201 -B "$UE1_IP" \
    -u -b 500M -l 512 -t 120 -i 1 --get-server-output -J > ue1-500M.json
```

Repeat at 200M, 500M, 1G and higher as the generator permits, keeping `-l 512`
fixed. `-b` is requested payload bitrate, not a guarantee that packets reach
CN; compare sender output, manager port-0 RX packet deltas and DN received
bitrate/loss. At 500 Mbit/s, 512-byte UDP payloads request about 122,070 pps;
Ethernet/IP/UDP/GTP overhead consumes additional bandwidth. See the
[iperf3 options](https://software.es.net/iperf/invoking.html).

While UE1 traffic and queue observation are running, register a fresh UE2
using its own provisioned IMSI. Read its admission log and actual TUN address,
then launch its traffic to port 5202. Repeat with UE3 while earlier traffic
continues. This separates the session-admission trigger from the subsequent
traffic increase. Verify additional worker rows, READY/Admitted logs and
traffic on ports 2/3, then 4/5, when those slots are selected.

If 1024 is never observed, establish the bottleneck before tuning it. For a
separate sensitivity experiment, restart with threshold 32, then compare
decision logs; this may trigger on short bursts and is not a calibrated load
threshold. Even 32 cannot trigger when the sampled queue is empty. Do not
claim a sustained overload from one queue sample or a performance gain from
a lower threshold alone.

| Observation under rising offered load | Next diagnosis |
| --- | --- |
| CN N3 RX plateaus; worker RX/TX remain nearly empty | Profile generator/gNB and manager RX; inspect N3 PF/VF hardware drops. |
| Worker RX backlog persists; packet-processing work dominates its profile | UPF-U is a candidate bottleneck; admit a new session and inspect the decision. |
| Worker TX backlog persists | Inspect manager TX, N6 NIC and DN before attributing the limit to UPF-U. |
| More workers carry traffic, but aggregate throughput stays flat | Look for a shared manager, generator, NIC, DN or QoS limit. |

Check hardware `/ethdev/xstats` before/after load on the active ports, especially
missed/error/mbuf-allocation counters. Manager/UPF drop counters alone cannot
exclude loss in the generator, NIC, kernel or elsewhere on the path.

## 4. Compare capacity with the same workload

Run a fixed-one-worker baseline using this fork (`min_workers: 1`,
`max_workers: 1`), then dynamic mode (`min_workers: 1`, `max_workers: 4`).
Use the same timed UE-admission/load sequence, packet sizes, offered rates,
QoS, affinity, MTU, offloads and generator; repeat each run. Add vanilla L25GC+
as a separate baseline under the same conditions to separate scaling gains
from other implementation differences. Clean up session/filter state between runs.

Report receiver goodput, UDP loss, actual ingress pps, admitted slot per UE,
decision-time queue lengths, READY worker count and cores reserved for dataplane
work. Include manager's three cores in both CPU budgets; one worker becoming
two changes that budget from four to five cores. Keep UPF-C/control NF costs
explicit and fixed. Use the same wall-clock overlap across flows: do not sum
averages from the current unequal 20- and 30-second tests.

After functional startup checks, run separate TCP and downlink (`-R`) experiments.
An overloaded old session remains on its original worker, so new-session
placement cannot promise balanced workers or linear throughput scaling. This
implementation demonstrates admission-driven scale-out; it does not shrink the
worker pool when traffic falls. If the generator cannot saturate one worker,
use a faster GTP-U traffic source reproducing the admitted sessions' actual
N3 endpoints/TEIDs and UE addresses, and report it as a separate experiment.
