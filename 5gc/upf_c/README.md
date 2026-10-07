# Dynamic UPF-U scaling

This branch moves worker creation from session arrival to a periodic queue check
in UPF-C's existing event loop. No new thread or forecasting model is used.

## Policy

- Start `min_workers` using the existing startup sequence.
- Every `queue_sample_interval_ms`, read each READY worker's NF RX ring.
- Keep the last `queue_window_samples` readings **per worker**. Once the window
  is full, start one unused slot if **every READY worker's average RX backlog**
  is **strictly above** `rx_queue_threshold`.
- Each new reading replaces the oldest; a low/zero reading stays in the average.
  An unavailable READY queue, a missed sampling interval (at least twice the
  configured interval between reads), or a changed READY set/worker identity
  resets all windows. Missing readings are never replaced with zeros.
- Sampling pauses and history resets during startup, shutdown or control errors. Sampling
  continues at `max_workers`, but cannot start another worker. A spawn attempt
  resets history, including when spawning is deferred or fails.
- New sessions select the least-queued READY worker, even above threshold or
  while another worker starts. Equal queues choose the first slot. With no READY
  worker, admission waits within the existing request timeout.

An idle newly started worker prevents further scale-out until it also becomes
loaded. Existing sessions stay on their assigned worker; adding capacity benefits
subsequent sessions.

Scale-down uses the same averages. If all READY averages are at or below
`scale_down_queue_threshold`, keep at least `min_workers` and select the highest
READY slot with zero sessions. Pending admissions block the decision; pending
or uncertain session cleanup still counts as occupied. These conditions must
hold continuously for `scale_down_hold_ms`. A busy peer keeps an idle spare alive.
Missing samples, new admissions, candidate changes and lifecycle changes reset
the hold. Only one worker starts or stops at a time; each stop needs a fresh hold.

Sessions end through SMF's normal PFCP deletion. UPF-C removes the N6 rule,
withdraws classifier rules, waits for readers and the owner's buffered-packet/
meter cleanup ACK, then frees the session and caches the deletion response.
Stopping traffic alone does not delete a session. There is no migration or forced
session expiry.

A worker transitions `READY → STOPPING → INACTIVE`: delete its N3 rule, disable
both VF polling routes, drain NF queues, request graceful exit, and wait for the
manager to release its core/service. Only then can the slot restart with a new
generation. Timeout/cleanup failures leave it FAILED and unavailable for reuse.

## Configuration and deployment

In the **actual YAML referenced by `cn.upfc_config`**, set these fields under
`configuration.scaling`, keeping your existing slots, paths and network settings:

```yaml
min_workers: 1
max_workers: 2               # Or up to the number of configured slots.
rx_queue_threshold: 40
queue_sample_interval_ms: 10
queue_window_samples: 10     # Number of samples, 1..1024.
scale_down_queue_threshold: 10 # Must be below rx_queue_threshold; zero is valid.
scale_down_hold_ms: 30000
worker_stop_timeout_ms: 5000
```

The policy values shown are also the parser defaults. Hold and stop timeouts
must be positive. Replace the old
`queue_consecutive_samples` key in existing YAML; it is rejected with a migration
message. Existing explicit thresholds (e.g. 1024) remain in effect until edited.
Ten samples at 10 ms give a nominal 100 ms window; first-to-last sample spacing
is 90 ms. This is not a fixed detection delay: crossing depends on the values
already in the window, new backlog magnitude and event-loop timing. The threshold
40 and window size 10 are experimental starting values, not calibrated capacity.

For each worker, `average = sum(last W readings) / W`. The comparison uses
`sum > threshold * W`, preserving fractional averages. Averaging across workers
would hide an idle worker behind a busy one, so it is not used. For example, with
W=3 and threshold 40, `80, 0, 80` qualifies (53.33), whereas the old consecutive
rule would reset at zero. A large isolated spike such as `0, 0, 150` also qualifies
(50); averaging does not guarantee fewer burst-triggered spawns. Window size 1
uses the latest sample directly.

The shared registry is now **ABI 7**, with a changed session layout. Rebuild
manager, UPF-C and UPF-U together (`ninja -C build` on CN), rebuild control NFs
linked to the changed ONVM libraries using your existing build procedure, and
restart the deployment. Use a fresh experiment run and the usual PF filter
cleanup. See [dynamic setup](../../docs/sriov-upf/dynamic-scaling.md).

`cn.queue_threshold` in `lab.json` only controls qcheck's display; set it to 40
for consistency. `cn.queue_interval_s` controls that observer, not UPF-C's policy.

## Logs and measurements

Info-level logs provide:

| Marker/field | Meaning |
| --- | --- |
| `Scale-out trigger` | READY count, `min_rx_avg` (minimum of per-worker window averages), threshold, `window_samples` and interval. |
| `Spawned UPF-U ... spawn_ms` | Process-creation time; excludes worker initialization. |
| `UPF-U ... READY ready_ms` | Full startup through registration, classifier ACK, N3 steering and manager polling ACK. **Use this as worker creation-to-ready time.** |
| `Admission received ... xid ... xact` | PFCP request entered UPF-C admission, after parsing. |
| `Admission decision` | Queue snapshots and selected READY slot; arrival never spawns. |
| `Admitted ... attach_ms` | Session installation through N6 steering ACK and classifier ACK on its worker. |
| `worker_wait_ms`, `response_ms`, `admission_ms` | Placement/wait, response-function time, and total admission time. |
| `Deleted SEID=... slot=... sessions=...` | Session resources reclaimed after all cleanup ACKs. |
| `Scale-down hold started/cancelled` | Eligibility timer and resets. |
| `UPF-U ... STOPPING` / `INACTIVE stop_ms` | Shutdown began / process and manager resources released. |

Durations use a monotonic clock and are available without packet timing enabled.
The [timing guide](../../docs/sriov-upf/nf-timing.md#measurements-and-limits)
defines the boundaries. Wall-clock log timestamps locate these events on a timeline.

## Quick experiment

1. Start CN, DN and gNB; attach UE1 and send setup traffic to its assigned endpoint.
2. On CN, watch `upfc.log` for `Scale-out trigger`, `Spawned UPF-U slot=1`, then
   `UPF-U slot=1 ... READY ready_ms=...`. No UE2 request is needed to trigger this.
3. Keep UE1 traffic running and attach UE2 **after slot 1 is READY**. Verify its
   `Admitted` slot/IP/TEID, then send UE2 traffic using that endpoint.
4. Compare `ready_ms` and per-session `attach_ms`. For a combined timeline, use
   UPF-C's request/attachment logs and UE `events.csv` generator-launch markers
   with synchronized CN/UE clocks. Launch time is not the first packet arrival;
   the code does not timestamp the first data packet.

An early UE2 request may still choose worker 0. Idle queues can also tie; always
verify placement. Scale-out does not remove an existing worker's backlog or a
10 Gbps link bottleneck. Follow the [launcher steps](../../scripts/experiment/README.md).

For scale-down, release the UE's PDU session and verify `Deleted SEID` in UPF-C.
Keep every READY worker's average below the lower threshold for the hold duration;
expect STOPPING, then INACTIVE, without going below `min_workers`. Repeat scale-out
to check slot reuse. Keep a silent but established session in a separate trial:
its worker must stay alive.

## Local verification

`python3 tests/proactive_scaling/run.py` compiles the production sampler and
placement code with mock queues/time and ASan/UBSan. It checks full-window warmup,
rolling eviction, per-worker averages, strict/fractional thresholds, configurable
window bounds, missing/stale samples, worker changes, startup/process limits and
READY-only admission. The [worker-slot tests](../../tests/worker_slots/README.md)
cover YAML defaults, overrides, bounds, obsolete-key rejection and registry ABI
validation; they require libyaml development files. Full build/startup/PFCP/NIC
validation still needs the CN testbed.

`python3 tests/scale_down/run.py` checks deletion/cleanup ACK ordering, response
caching, timeouts, holds, spare retention, graceful stops and generation reuse
with mock processes and ASan/UBSan. `python3 tests/worker_polling/run.py` checks
polling activation/disable and stale VF packet cleanup. qcheck follows planned
stops and respawns; its unit tests run with
`python3 -m unittest discover -s tests -p test_qcheck.py`.
