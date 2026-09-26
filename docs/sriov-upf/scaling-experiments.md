# Evaluation: queue evolution and lossless uplink UDP capacity

Two separate experiments: (A) admission-driven growth from one to four UPF-Us;
(B) maximum measured UDP goodput with exactly four sessions, comparing four
UPF-Us against one UPF-U in L25GC+. These are testbed procedures, not results.

## Which queue and why

The current uplink path is:

```text
N3 VF hardware RX → manager RX → UPF-U NF RX ring → UPF-U processing
                 → UPF-U NF TX ring → manager TX → N6 VF hardware TX → DN
```

Plot each UPF-U's **NF RX ring** (`MProc_Client_<instance>_RX`). This is exactly
the queue read by `select_worker()` in `5gc/upf_c/upf_scaling.c`. Its occupancy
measures pending UPF work, not CPU utilization or total offered load. N3 and N6
traffic share that worker's RX ring; use predominantly uplink traffic here.
The associated TX ring diagnoses downstream manager/TX congestion.

| Observation | Interpretation to investigate |
| --- | --- |
| Persistent NF RX backlog | UPF processing cannot keep up with delivered traffic. |
| Persistent NF TX backlog | Manager TX, NIC egress or downstream path bottleneck. |
| NF RX empty, ingress rate plateaus | Generator/gNB, manager RX or NIC may be limiting delivery. |
| `rx_nombuf` increases | RX mbuf allocation failures; inspect the shared pool. |
| `imissed` increases | Hardware RX drops due to unavailable buffers, where the PMD supports this counter. |

VF descriptor occupancy is a secondary diagnostic for the NIC-to-manager path;
it is not a substitute for pending UPF work. DPDK's
[`/ethdev/rx_queue`](https://github.com/DPDK/dpdk/blob/v24.11/lib/ethdev/rte_ethdev_telemetry.c)
describes queue configuration, not a portable live queue-length signal. DPDK warns that
unsupported [ethdev statistics](https://doc.dpdk.org/api-24.11/structrte__eth__stats.html)
can read zero. Check supported PF/VF xstats and manager NF RX/TX drop counters;
end-to-end UDP accounting decides whether the trial is lossless.

### Queue sizing

This tree now uses `NF_RX_QUEUE_RINGSIZE=4096`: **4095 usable RX entries** for
each NF. `NF_QUEUE_RINGSIZE=65536` still sizes TX rings. The 1024 threshold is
about 25% of RX capacity. See [DPDK ring capacity](https://doc.dpdk.org/api-24.11/rte__ring_8h.html).
Rebuild/restart the manager and NFs together; verify `*_rx_capacity=4095` in
the capture. This changes all NF RX rings, including control NFs, not just UPF-U.

The dataplane pool contains 32767 mbufs, not a fundamental DPDK maximum. It is
shared across NIC RX descriptors, NF RX/TX rings, in-flight bursts, caches and
other buffered traffic. Four full RX rings use 16380 pointer slots; this does
not reserve mbufs or guarantee that enough remain. With one 512-descriptor RX
queue on each of eight VFs, roughly 4096 mbufs are also posted to NIC RX queues,
including initialized inactive VFs. Use actual negotiated descriptor counts.
Multi-segment packets can consume multiple mbufs. The separate control-plane
pool does not increase dataplane capacity.

4K/1K is a reasonable **starting experiment**, not a calibrated scaling policy.
At threshold, remaining RX headroom is 3071 packets: at 1 Mpps excess arrival
rate, that is about 3.1 ms. Worker startup is not guaranteed to finish in that
time, and old sessions never move. Reducing ring size bounds backlog; it does
not ensure lossless scaling or higher throughput. Keep sizes identical in
controlled one/four-worker comparisons and record the vanilla baseline's sizes.

## Preparation and capture

1. Follow [dynamic-scaling.md](dynamic-scaling.md) for VF/PF setup, filters,
   routing and clean startup. Rebuild with `ninja -C build` on CN. Restart the
   whole deployment between configurations and remove stale session/filter
   state using the established cleanup procedure.
2. The supplied scaling example has **two slots**. Extend it to four unique
   core/service/N3/N6-IP/VF/port sets and set `min_workers: 1`, `max_workers: 4`,
   `rx_queue_threshold: 1024`, `debugLevel: info`. Initialize all eight VFs in
   manager before startup; only UPF-C launches the workers. Verify actual
   BDF-to-port mappings. Slot, VF index, service ID and instance ID are different.
3. Record commit, DPDK/iperf versions, NIC/driver, MTU, offloads, queue/pool
   sizes, CPU/NUMA/SMT placement and QER/session-AMBR limits. Reserve dedicated
   worker cores; keep manager, control NFs, generator and DN off those cores
   and their SMT siblings. Hold manager thread counts fixed across cases.
4. Use a **new UPF-C log for each run**, capturing stdout and stderr. Keep INFO
   logging enabled. Start queue capture after manager, before UE admission;
   the UPF-C log may appear later. Use its existing launch command with
   `> run-A/upfc.log 2>&1` (never append an earlier run).

```sh
mkdir -p run-A
sudo python3 scripts/qcheck.py --upfc-log run-A/upfc.log --slots 4 \
    --interval 0.1 > run-A/queues.csv 2> run-A/qcheck.log
```

Run capture in its own terminal, preferably on a spare core. It discovers
slot-to-instance mappings from existing Spawned/READY/exit logs. Columns
`slot0_rx` through `slot3_rx` stay fixed as workers appear. Pre-READY and failed
workers have blank queue cells; an idle ready worker has a measured zero.
`log_ready_workers` is log-derived, not a health probe; `sampled_rx_count`
reports how many expected RX queues returned values. Stop/restart capture if
the deployment restarts. Capture cannot distinguish an old log from a new run.

`rx_min` matters to the all-workers threshold; `rx_sum` is only a summary.
Both are blank if any expected queue is unavailable. `elapsed_s` is monotonic;
`time_ns` aligns with same-host events. Record admission/load changes and
actual established-session counts with timestamps; synchronize hosts or record
clock offsets. `sample_ms` measures sequential read time; `interval_overrun`
flags a telemetry read exceeding the interval. Check actual `elapsed_s` gaps
as well. Start at 100 ms; try 10–20 ms only if collection overhead is small,
using a spare core and an instrumentation-on/off capacity comparison.

Port columns retain raw packet/byte/error counters and deltas; rates use actual
elapsed time. Counter rollback produces a blank delta. Byte rates are NIC
accounting, not UDP payload goodput. All enabled ports are collected by default;
`--ports ...` narrows them. Select the manager's telemetry socket with `--socket`
when its file prefix differs from `rte`. For fixed instance IDs, the previous
mode remains available: `sudo python3 scripts/qcheck.py 2 9 --interval 0.1`.

Also retain manager statistics with NF RX/TX drops and port TX drops, supported
PF/VF xstats before/after, per-UE sender/receiver reports, and UE→slot→instance
mapping. `qcheck` does not export ONVM software-drop counters. Check generator,
gNB and DN threads with `pidstat -t -u`/`mpstat`; 100% busy-poll CPU alone is
not evidence of UPF saturation.

## A. Queue length versus time during 1 → 4 worker growth

Admission is the trigger. `select_worker()` starts another worker only when a
**new session arrives and every ready worker has RX occupancy ≥1024** (subject
to startup and worker limits). It samples once, chooses the least queued worker
when below threshold, and never migrates existing sessions. Creating four idle
UEs first can put all four sessions on slot 0. `min_workers: 4` also does not
guarantee distribution: equal queues select the first slot.

Use four independent UE PDU sessions and one long-running uplink UDP stream per
established session. Keep payload size at 512 bytes initially; no reverse `-R`
traffic or parallel `-P` streams. Use four DN server processes on ports 5201–5204.
Example for one UE; substitute the DN and actual UE TUN address:

```sh
# DN (separate terminal/process for each port)
iperf3 -s -B <DN_IP> -p 5201
# UE host; -b is requested UDP payload bitrate, not measured delivered rate
iperf3 -c <DN_IP> -p 5201 -B <UE1_IP> -u -b 500M -l 512 \
    -t 300 -i 1 --get-server-output -J > ue1-setup.json
```

Select initial rates through a short pilot. For example, step 100, 200, 400,
800 Mbit/s per active UE until queues build; these are offered loads, not
expected capacity. Restart a stream at each rate step if necessary; keep its
PDU session established. Mark the resulting traffic gaps.

| Stage | Sessions / workers | Action and evidence |
| --- | --- | --- |
| Idle, 10 s | 0 / 1 READY | Capture idle baseline; slots 1–3 remain blank. |
| UE1, ≥20 s per rate | 1 / 1 | Admit UE1; ramp its UDP load until slot 0 RX builds. |
| UE2 admission | 1→2 / 1→2 expected | Keep UE1 load running; admit UE2 while slot 0 is ≥1024. Verify `spawn_threshold`, READY and UE2's `Admitted` slot. |
| UE2 load, ≥20 s per rate | 2 / 2 | Start UE2 traffic; raise both streams until both worker RX rings build. |
| UE3 admission/load | 2→3 / 2→3 expected | Admit UE3 under that load, verify its placement, then load all three workers. |
| UE4 admission/load | 3→4 / 3→4 expected | Admit UE4 while all three RX rings meet threshold; start UE4 traffic and hold ≥30 s. |
| Drain, 10 s | 4 / 4 | Stop UDP traffic, retain sessions; observe queue drain. No scale-down is implemented. |

Inspect exact decision samples, not just the periodic trace:

```sh
rg 'Admission (queue|decision)|Spawned UPF-U|READY|Admitted SEID' run-A/upfc.log
```

`xid`/`xact` correlate decisions with admission. `spawn_threshold` means spawn
was requested; READY and `Admitted` establish completion. If an admission
selects `below_threshold`, record that outcome. To obtain the intended
four-session/four-worker run, restart and adjust the pilot timing/load; do not
silently add extra sessions or relabel a misplacement. Admit sessions serially:
simultaneous requests can share the same starting worker.

If high offered load never produces NF RX backlog, diagnose generator/gNB,
manager RX, QoS and hardware drops before lowering the threshold. NIC-only
congestion cannot validate this scaling trigger. A lower threshold belongs in
a separately labeled sensitivity run. Threshold stress may lose packets;
this experiment tests queue/placement behavior, not lossless transition.
An overloaded old worker can stay backlogged after another worker starts.

Plot four RX traces against `elapsed_s`, a horizontal 1024 threshold and a
4095 capacity line. Leave missing cells as gaps, never replace them with zero.
Add aligned panels for **established UE count**, **READY worker count**, and
requested/actual offered load, plus markers for admissions and READY events.
Retain raw samples; periodic sampling can miss a decision-time spike. Do not
claim peaks or trigger timing more precise than the sampling/log timestamps.
Repeat three clean runs, report all scale events and failures.

Minimal raw queue plot (requires Matplotlib; event/load panels come from the
recorded experiment timeline):

```python
import csv
import matplotlib.pyplot as plt

with open("run-A/queues.csv") as f:
    rows = list(csv.DictReader(f))
t = [float(r["elapsed_s"]) for r in rows]
for slot in range(4):
    q = [float(r[f"slot{slot}_rx"] or "nan") for r in rows]
    plt.plot(t, q, label=f"UPF-U {slot + 1}")
plt.axhline(1024, color="black", linestyle="--", label="Admission threshold")
plt.axhline(4095, color="grey", linestyle=":", label="RX capacity")
plt.xlabel("Time since capture start (s)")
plt.ylabel("NF RX occupancy (packets)")
plt.legend()
plt.tight_layout()
plt.savefig("run-A/queues.pdf")
```

## B. Highest measured zero-loss uplink UDP throughput

Primary plot: **four sessions / one L25GC+ UPF-U** versus **four sessions / four
scaled UPF-Us**. Also measure this fork with `min_workers=max_workers=1` as a
control to distinguish scaling gains from other fork/NIC-path differences.
Name the exact vanilla L25GC+ revision and settings. If its ring sizes/path
differ, disclose that or add a matched-ring baseline; do not attribute every
difference to worker count.

### Establish and freeze placement

- Four-worker case: complete A through four admitted sessions on four distinct
  slots. Stop the setup streams, keep sessions, let RX/TX queues drain and
  mbuf availability recover. Confirm all workers remain READY. No migration or
  new admissions during measurement. Setup losses are retained separately.
- One-worker cases: establish the same four sessions on the single worker.
  Use the appropriate single-worker configuration for vanilla L25GC+; this
  fork's `scaling` options are not assumed to exist upstream. Avoid the legacy
  `upf_u_workers` list: it is a one-session-per-worker allocation mode.
- Hold generator, gNB, DN, payload, MTU/offloads, QER/AMBR, affinity, manager
  thread count and traffic split constant. Four worker cores versus one is
  intentional; report manager cores too. Confirm low-rate bidirectional
  reachability and warm ARP before measurement.

### Rate search and pass criterion

1. Use **four simultaneous streams with equal per-UE rates**. Aggregate offered
   rate `R` means `R/4` per stream, not `R` per stream. Use the same 512-byte
   payload in all cases. Optional 128/1200-byte runs are separate data series.
2. Warm with a separate 10-second low-rate run. Each measured trial sends for
   at least 60 seconds, then allows a 5-second drain. Start all four clients
   together on the UE host, each with its own `-B <UE_IP>` and DN server port.
   Save sender and DN receiver interval/final reports. Do not use `-O` to hide
   measurement loss. Verify the actual common traffic overlap is ≥60 seconds;
   use a longer send duration if launch/connection skew would shorten it.
3. Start at a rate known to pass; increase aggregate rate in 20% steps until a
   loss occurs. Bisect the last pass/fail bracket until width ≤1% of the passing
   rate (or ≤1 Mbit/s). Repeat boundary trials if results are inconsistent;
   loss versus requested rate need not be perfectly monotonic.
4. A passing trial requires **zero lost datagrams on every UE**, valid completed
   sender/receiver reports, and matching sent/received unique packet counts
   after drain. Check tail loss and sequence accounting, not only rounded
   `0.00%` output. Offered rate must actually be attained (predeclare tolerance,
   e.g. ±2% per stream), with no increasing software/hardware drops attributable
   to the test or unexplained counter resets. Missing reports are invalid,
   never a pass. Reconcile duplication/reordering accounting if reported;
   unresolved sequence accounting makes the result inconclusive.
5. Confirm the highest passing rate in **five 60-second trials**, all passing;
   otherwise lower the rate and reconfirm. Then validate for 300 seconds.
   Recheck the next higher bracket endpoint and record its loss. If the
   generator/link reaches its limit before a failure, report an achieved lower
   bound on UPF capacity, not a measured UPF maximum.

For B, run one server per port with `-1 -J`, restarting it before every trial
(and after the separate warm-up). Save receiver JSON independently:

```sh
iperf3 -s -1 -J -B <DN_IP> -p 5201 > trial-dn-ue1.json
```

One client command per UE, launched together (check the installed
[iperf3 options](https://software.es.net/iperf/invoking.html)):

```sh
iperf3 -c <DN_IP> -p 5201 -B <UE1_IP> -u -b <PER_UE_RATE> -l 512 \
    -t 65 -i 1 --get-server-output -J > trial-ue1.json
```

Use receiver `end.sum_received` for loss/bytes and sender `end.sum_sent` for
sent counts, where supported by the installed version; do not use the ambiguous
legacy UDP `end.sum` as received goodput. Require equal sent/received payload
bytes as an additional tail-loss check. Verify these fields on the low-rate
pilot ([iperf3 implementation](https://github.com/esnet/iperf/blob/3.16/src/iperf_api.c)).
Waiting between trials drains the dataplane; it does not extend an iperf
receiver that has already closed. Packets missing from its report fail the trial.

Use at least a common 60-second window inside the overlapping receiver
intervals for all four streams. Preserve absolute interval alignment using
synchronized timestamps. Apply the zero-loss criterion to the **whole trial**,
including edges; the common window controls throughput aggregation. If interval
reports cannot identify that window, fix capture/alignment before comparing;
do not sum averages from unequal or partially overlapping runs.

For a common interval `T`, compute receiver goodput as
`8 × sum(received UDP payload bytes in T) / T / 1e6` Mbit/s. Report aggregate
received pps as well. Port byte counters include different encapsulation and
must not replace DN payload goodput. At 512 bytes, 500 Mbit/s of payload is
approximately 122070 pps before IP/UDP/GTP/Ethernet overhead.

### Results and figure

For each case/repeat retain: four-session placement, per-UE requested and
actual sender rates, receiver bytes/packets/loss, interval bounds, queue/pool
capture, software/PF/VF drop deltas, and pass/fail. Alternate case order across
repetitions to reduce thermal/time-order bias. Preserve failed trials too.

Plot two primary bars (one-UPF L25GC+, four-UPF scaled system), with the matched
one-worker fork as an optional third bar. Height is median aggregate received
goodput across the five confirmations at that case's highest verified passing
rate; show min–max whiskers. Report offered pass/fail bracket, validation
duration, payload size, pps and CPU budget in the caption/table. A finite test
supports **zero observed loss for this workload/duration**, not universal
losslessness. Report speedup as four-worker / one-worker goodput, identifying
which one-worker baseline was used.

If four workers do not improve capacity, compare actual N3 arrival rates,
per-worker RX/TX queues, pool pressure, manager drops and generator/DN profiles.
This is a valid result when a shared component is the bottleneck.

## Local checks and testbed acceptance

```sh
python3 -m unittest discover -s tests -p 'test_qcheck.py' -v
```

Replay checks cover late workers, stable columns, missing telemetry, partial
log lines, worker exits, counter resets and invalid arguments. They do not
validate DPDK multiprocessing, VF counters or real throughput. On CN require:
full Linux build; telemetry RX capacity 4095; four distinct READY/admitted
workers; low-rate four-session reachability; and valid sender/receiver reports
before collecting the final figures.
