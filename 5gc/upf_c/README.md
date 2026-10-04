# Proactive UPF-U scale-out

This branch moves worker creation from session arrival to a periodic queue check
in UPF-C's existing event loop. No new thread or forecasting model is used.

## Policy

- Start `min_workers` using the existing startup sequence.
- Every `queue_sample_interval_ms`, read each READY worker's NF RX ring.
- If **all** sampled READY queues are **strictly above** `rx_queue_threshold`
  for `queue_consecutive_samples` consecutive rounds, start one unused slot.
- A queue at/below threshold, an unavailable sample, or no READY workers resets
  the count. Sampling for scale-out pauses during startup and at `max_workers`.
- New sessions select the least-queued READY worker, even above threshold or
  while another worker starts. Equal queues choose the first slot. With no READY
  worker, admission waits within the existing request timeout.

An idle newly started worker prevents further scale-out until it also becomes
loaded. Existing sessions stay on their assigned worker. There is no scale-down
or migration; adding capacity benefits subsequent sessions.

## Configuration and deployment

In the **actual YAML referenced by `cn.upfc_config`**, set these fields under
`configuration.scaling`, keeping your existing slots, paths and network settings:

```yaml
min_workers: 1
max_workers: 2               # Or up to the number of configured slots.
rx_queue_threshold: 40
queue_sample_interval_ms: 10
queue_consecutive_samples: 3
```

The last three values are also the parser defaults. Existing explicit thresholds
(e.g. 1024) remain in effect until edited. Three samples at 10 ms give nominal
20–30 ms detection after backlog rises; event-loop delays can extend this.
40 packets is an experimental trigger, not a measured capacity guarantee.

The added configuration fields change the shared registry to **ABI 5**. Rebuild
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
| `Scale-out trigger` | Last qualifying sample: READY count, minimum RX backlog, threshold, sample count and interval. |
| `Spawned UPF-U ... spawn_ms` | Process-creation time; excludes worker initialization. |
| `UPF-U ... READY ready_ms` | Full startup through registration, classifier ACK, N3 steering and manager polling ACK. **Use this as worker creation-to-ready time.** |
| `Admission received ... xid ... xact` | PFCP request entered UPF-C admission, after parsing. |
| `Admission decision` | Queue snapshots and selected READY slot; arrival never spawns. |
| `Admitted ... attach_ms` | Session installation through N6 steering ACK and classifier ACK on its worker. |
| `worker_wait_ms`, `response_ms`, `admission_ms` | Placement/wait, response-function time, and total admission time. |

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

## Local verification

`python3 tests/proactive_scaling/run.py` compiles the production sampler and
placement code with mock queues/time and ASan/UBSan. It checks sampling intervals,
consecutive overload/reset, startup/worker limits and READY-only admission.
The policy and manager-polling mock suites pass locally with ASan/UBSan.
The [worker-slot tests](../../tests/worker_slots/README.md) cover YAML defaults,
overrides and validation, but could not run locally because `yaml.h` is missing.
Full build/startup/PFCP/NIC validation still needs the CN testbed.
