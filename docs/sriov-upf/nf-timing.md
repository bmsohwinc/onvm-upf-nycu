# UPF-U concurrency diagnostics

For per-node tmux startup and automated traffic phases, use the
[experiment launchers](../../scripts/experiment/README.md). The manual procedure
below remains available for individual commands.

The optional NFLIB instrumentation records timing and queue lengths without
changing packet handlers, routing, queue sizes, admission or scaling. It only
enables for NFs tagged `upf_u` when `ONVM_NF_TIMING_DIR` names an existing writable
directory. UPF-C passes its environment to spawned workers. No registry ABI
change is required; rebuild and restart the deployment to load the new NFLIB.

## Start capture

On CN, rebuild with `ninja -C build`. Create a **fresh** directory on tmpfs to
avoid disk writes from the polling thread. Example (adapt UPF-C core/config):

```sh
nf_diag_dir=/dev/shm/upf-debug-run1
mkdir "$nf_diag_dir"
sudo env ONVM_NF_TIMING_DIR="$nf_diag_dir" \
  ./build/5gc/l25gc_upf_c -l 8 -n 4 --proc-type=secondary \
  --file-prefix rte --no-pci -- -r 2 -m -- \
  -f 5gc/upf_c/config/upfcfg.scaling.example.yaml \
  > "$nf_diag_dir/upfc.log" 2>&1
```

The directory must exist before workers start. Each UPF-U writes its own
`nf-timing-<pid>-<instance>.csv`; the startup log prints the filename and polling
thread ID. UPF-C itself is not instrumented. For manually started workers, pass
the environment variable to each UPF-U instead. Unset/omit it for the baseline.
An output error disables diagnostics with a message and leaves the NF running.

In another CN terminal, set the same `nf_diag_dir`, then run the existing monitor:

```sh
nf_diag_dir=/dev/shm/upf-debug-run1
sudo python3 scripts/qcheck.py --upfc-log "$nf_diag_dir/upfc.log" --slots 2 \
  --interval 0.1 --watch --threshold 1024 \
  > "$nf_diag_dir/queues.csv" 2> "$nf_diag_dir/qcheck.log"
```

Use the normal spare-core affinity for collectors, avoiding worker/manager cores
and their SMT siblings. Enable only the collectors needed for a given trial.

## Fixed-session experiment

Set `max_workers: 2`. Admit UE1, load its worker above the queue threshold, wait
for proactive scale-out and slot 1 READY, then admit UE2 while UE1 stays loaded.
Verify different admitted slots and both READY. Stop setup traffic,
retain both sessions, and wait for RX/TX queues to drain. Prestarting two workers
alone does not guarantee separate session placement.

Keep processes, sessions, rules, packet size and UE1's offered rate fixed. Mark
phases **on CN**, after making the traffic change and letting its transition
settle; use about 30 seconds per phase. `mark` records a timestamp only and does
not start/stop traffic. Mark a transition before changing traffic so reports can
exclude it explicitly:

```sh
# Start UE1 traffic, let it settle, then:
python3 scripts/nf_timing.py mark "$nf_diag_dir" UE1_ONLY
# After ~30 s, before starting UE2 traffic:
python3 scripts/nf_timing.py mark "$nf_diag_dir" TRANSITION_1
# Start UE2 traffic; after settling:
python3 scripts/nf_timing.py mark "$nf_diag_dir" BOTH
# After ~30 s, before stopping UE2 traffic:
python3 scripts/nf_timing.py mark "$nf_diag_dir" TRANSITION_2
# Stop UE2 traffic only; after settling:
python3 scripts/nf_timing.py mark "$nf_diag_dir" UE1_AGAIN
# After ~30 s:
python3 scripts/nf_timing.py mark "$nf_diag_dir" END
python3 scripts/nf_timing.py report "$nf_diag_dir"
```

The report writes `timing-intervals.csv` and `timing-summary.csv`. Compare
worker 1's UE1_ONLY / BOTH / UE1_AGAIN rows. Transition phases are labeled
separately; ignore those for steady-state comparisons. Rows crossing any marker
are excluded. Without markers, reporting summarizes the entire recording.
Repeat with fresh directories; retain raw files. Copy tmpfs results to persistent
storage after the experiment, before reboot. Stop qcheck with Ctrl-C when done.

## Measurements and limits

UPF-C also appends lifecycle durations to its timestamped `Spawned UPF-U`,
`UPF-U ... READY`, and `Admitted SEID` log lines. These measurements are always
available at info log level and do not require `ONVM_NF_TIMING_DIR`:

| Field | Interval (milliseconds) |
| --- | --- |
| `spawn_ms` | Spawn-attribute setup through process creation and attribute cleanup; excludes worker initialization. |
| `ready_ms` | Same start through UPF-C marking the worker READY, including registration, classifier acknowledgement, steering and polling enablement. |
| `worker_wait_ms` | Entry to `UpfScalingEnqueue` through observing the selected worker READY; includes placement, event-loop scheduling, and any worker startup wait. |
| `attach_ms` | Start of session installation on that ready worker through observing successful steering and its classifier acknowledgement. |
| `response_ms` | From that attachment completion through return of the establishment-response function. |
| `admission_ms` | Admission enqueue through return of the establishment-response function; sum of the preceding three session intervals, subject to rounding. |

Durations use DPDK's monotonic timer cycles, not differences between wall-clock
log timestamps. Session timing starts after PFCP parsing/transaction processing;
it excludes SMF/UE signalling latency and does not confirm remote receipt of the
response. `response_rc` records the response function's status: a cached success
response can retain an admitted session even if its first transmission fails.
Rejected pending admissions and startup failures handled by `fail_start` log
`elapsed_ms` separately; do not treat those as successful attachment/readiness
measurements. Immediate rejection before admission enqueue is not timed.

```sh
grep -E 'Scale-out trigger|Spawned UPF-U|UPF-U slot=.*READY|Admission received|Admitted SEID|Admission failed|startup failed' upfc.log
```

- `packets` counts actual dequeues, including packets subsequently retained or
  dropped by the handler. It is not the handler's return-to-TX count. Interval
  counts also include polls, nonempty bursts and full bursts.
- One loop in 1,024 samples RX/TX occupancy before dequeue. Queue averages are
  **sample/loop weighted**, not time-weighted; maxima are sampled maxima. Keep
  qcheck's fixed-time observations for queue evolution. `rx_now`/`tx_now` are
  additional snapshots at reporting time.
- For sampled **nonempty** bursts: `dequeue_ns` covers dequeue; `handler_ns`
  covers the handler loop; `tx_ns` covers post-handler bookkeeping, TX processing
  and flushing; `callback_ns` covers messages and the user callback. Empty polls
  do not enter these stage totals. Means use sampled packet counts; blank means
  no samples. `handler_burst_max_ns` is a maximum for an entire sampled burst.
- Timings are approximate TSC-derived **elapsed nanoseconds**, including
  interrupts/preemption and instrumentation overhead, not hardware CPU cycles
  or end-to-end packet latency. Do not extrapolate sampled sums to CPU utilization.
- Rows appear approximately once per second, plus a final partial shutdown row.
  Raw NF RX/enqueue-drop/TX-drop/action-drop counters are cumulative asynchronous
  snapshots. Reports compute deltas; the first observation or a counter rollback
  produces blank, never an assumed zero. Do not reset counters mid-experiment.
- Diagnostics add local counters, sampled timestamp/queue reads and synchronous
  CSV writes. `previous_write_ns` measures the preceding row's write/flush pause;
  that pause is included in the following packet-rate interval. Compare an
  enabled run against an otherwise identical disabled run before interpreting
  small differences. tmpfs reduces but does not eliminate logging overhead.

If handler ns/packet increases, investigate processing/cache effects. If handler
time stays stable while dequeue/TX time increases, investigate handoffs. Elapsed
timing alone cannot distinguish preemption, frequency changes and cache stalls.
For that follow-up, use `perf stat -t <polling-tid>` and separate `perf record`
runs; the raw CSV provides the TID. No additional profiler is needed to produce
the timing/queue results in this patch.

The complete Linux/DPDK build and instrumentation overhead still require CN.
