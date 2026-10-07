# Moving-average scaling and session-free downscaling: review handoff

Written 2026-10-07. This records the decisions, implementation, rationale and
validation from the conversation that followed `conversation-context.md`.
Use this as the entry point when reviewing the large downscaling patch later.
The earlier context contains the forwarding experiments and paper-related
history; this change does not establish new throughput results.

## Conversation and decisions

1. The starting policy scaled up after three consecutive high queue readings.
   Workers stayed alive after load subsided. The user requested analysis and a
   plan for moving averages and downscaling before implementation, and wanted
   to understand both mechanisms.
2. For the first downscaling version, the user chose **session-free workers**:
   let established sessions finish naturally. Migrating or forcibly expiring
   silent sessions was excluded.
3. The user asked why PFCP deletion needed work if a UE could be stopped manually.
   The existing dynamic path explicitly rejected deletion and retained the
   session. UE release must cause SMF to send PFCP deletion, and UPF-C must
   actually process it before a worker can become session-free. UPF-C does not
   originate a session deletion merely because traffic is low. Killing a UE or
   stopping pkt-gen is not proof that normal session release completed.
4. The user requested a hold/timeout to avoid repeated worker spawning/stopping,
   and accepted the other lifecycle safeguards.
5. Moving averages were implemented first, with configurable window size, then
   committed as `03067d8` (`Use configurable moving averages for UPF scale-out`).
6. The user requested exact downscaling steps/files for review, then authorized
   implementation with concise code. The implementation also needed normal
   session deletion, worker-local cleanup and safe manager teardown.
7. After seeing the large diff, the user specifically questioned the shaper and
   trTCM changes. Their purpose is session-state cleanup; the metering algorithm
   and shaping policy were not redesigned. Details are below. The assistant
   acknowledged that combining these dependencies made the patch harder to
   review and should have explained the dependency more clearly.
8. The user authorized committing the implementation and requested this new
   document so the changes can be reviewed clearly later.

## Moving average: behavior and limits

Each READY worker has its own sliding window of NF RX ring occupancy readings:

```text
average[worker] = sum(last W readings for that worker) / W
scale up when every READY average > rx_queue_threshold
```

The window must be full. Comparisons use integer sums against `threshold * W`,
preserving fractional averages without floating-point decisions. Window size 1
uses the latest reading. Workers are not averaged together: an idle worker must
remain visible as available capacity.

Compared with three consecutive readings, averaging tolerates individual low
samples. For W=3 and threshold 40, `80, 0, 80` averages 53.33 and qualifies;
the consecutive policy would reset at zero. It does **not** guarantee fewer
spawns: `0, 0, 150` also qualifies. Larger windows change responsiveness and
retain older load longer. These are experimental parameters, not calibrated
capacity measurements or a forecasting model.

Default W=10 at 10 ms is a nominal 100 ms window; first-to-last sample spacing
is 90 ms. Detection delay also depends on existing readings and event-loop
timing. Missing READY queues, gaps of at least two sampling intervals, changed
READY membership or worker identity reset history. Missing readings are not
zero-filled. Startup/shutdown and control errors also reset history. Sampling
continues at `max_workers`, allowing downscaling at that limit.

New sessions still select the least-queued READY worker from a fresh snapshot;
ties select the first slot. Admission does not itself spawn a worker. Existing
sessions remain pinned, so adding capacity benefits subsequent admissions.

## Downscaling policy

The scaler selects the highest-numbered eligible READY slot when:

- READY count is above `min_workers`.
- Every READY moving average is at or below `scale_down_queue_threshold`.
- The candidate has zero sessions, including zero pending/uncertain cleanup.
- No uncompleted admission is waiting.

Eligibility must persist for `scale_down_hold_ms`. New admissions, a changed
candidate, loss of eligibility, missing samples and lifecycle/window resets
cancel the hold. Each subsequent stop needs a fresh window/hold. Only one start
or stop proceeds at a time.

An empty spare stays alive while another worker is busy. A worker with a silent
but established session stays alive regardless of its queue average. There is
no session migration, idle-session expiry or automatic recovery of FAILED slots.

Session counts are owned by UPF-C's existing event loop. Allocation increments
the count; confirmed cleanup decrements it. Failed admission rollback also
decrements only after safe reclamation. Uncertain cleanup remains counted.

## Normal PFCP deletion: why more than a counter decrement

```text
SMF deletion request
  → mark session deletion_pending; reject concurrent modification
  → remove owned N6 UE filter and wait for manager ACK
  → withdraw classifier rules and wait for all registered readers
  → ask owner worker to purge UE shaper/meter state; wait for ACK
  → free session rules, maps and per-session buffer ring; decrement count
  → cache/send accepted PFCP deletion response
```

The parsed request and PFCP transaction remain pending while this progresses;
UPF-C does not block its event loop. Deletion has a 60-second deadline. Failed
or timed-out cleanup returns rejection and conservatively retains the session
resources/count; partial cleanup may already have made the session inactive.
Such a session needs operator investigation/restart rather than unsafe reuse.

The dispatcher now lets PFCP transaction replay happen before rejecting a
missing SEID. This matters after successful deletion: the session is gone, but
a retransmitted request must receive the cached success. A new transaction for
a genuinely missing session receives session-context-not-found. Response build/
cache failure can be retried without freeing resources twice.

Cleanup mailboxes carry worker generation, session index, SEID and classifier
version. ACKs cannot be overwritten before consumption. Old CLEAR_AND_DRAIN
events also carry the SEID, preventing a delayed event from draining a different
session that later reuses the same index.

## Why shaper and trTCM files changed

**Shaper:** a deleted session can still have queued packets in its worker. The
existing whole-worker cleanup loop was extracted into
`upf_u_shaper_forget_ue(index)`, which frees one UE's queued packets/entries,
clears its flow lists and active bit, and leaves other UEs intact. Whole-worker
cleanup calls this helper and now frees its shaper-entry mempool on exit.
Much of the shaper diff is the old loop moved into this reusable helper.

**trTCM source:** this file also owns the UE lookup table and UE token buckets.
`removeEntrybyUeIp()` clears a deleted UE's entry/bucket state and rebuilds the
lookup table so deleting an open-addressing collision-chain entry cannot hide
other UEs. This allows entries to be reclaimed while the worker keeps serving
other sessions. Metering rates, color decisions and token-update algorithms
were not changed; shared flow-meter entries are not blindly removed per UE.

The owner worker runs this cleanup at its packet-loop boundary and ACKs it.
UPF-C waits for that ACK before reclaiming the session. A zero session count
must mean cleanup completed, not simply that a deletion request arrived.

## Worker stop and reuse

```text
READY → STOPPING
  → remove owned N3 worker filter; wait for steering ACK
  → disable both VF polling routes; wait for RX-boundary ACK
  → wait for empty NF RX/TX rings
  → SIGTERM worker process group
  → confirm successful child exit with waitpid
  → wait for manager core/service release
  → INACTIVE
```

STOPPING excludes the worker from new admissions. Classifier reader membership
is removed only after process exit. Successful reuse increments the generation,
clears old registration/ACK fields and follows the normal startup sequence.
Before re-enabling a previously used VF route, manager RX discards stale VF
packets with a bounded drain. Configured VFs/ports remain allocated.

Stop timeout or failed cleanup marks the slot FAILED; it is not reused.
Termination has a forced-kill fallback. Outstanding mailbox ACKs must still be
consumed, even after timeout, before those mailboxes can be reused. A missing
ACK can therefore block lifecycle progress rather than permit unsafe reuse.

Manager N3 deletion refuses a slot with remaining session filters. Filter
removal checks ownership/generation and kernel rule contents. ixgbe adds
`FLOW_EXT` during readback even for zero-valued extensions; comparison accounts
for that while still checking extension contents. See the
[driver implementation](https://github.com/torvalds/linux/blob/v6.8/drivers/net/ethernet/intel/ixgbe/ixgbe_ethtool.c#L2380-L2442).

Review also found that manager TX could use NF rings while lifecycle cleanup
freed them. A TX read/write lock now protects complete TX passes; NF lifecycle
changes acquire the write side. RX retains its existing synchronization. TX
threads can still run concurrently. Performance impact requires testbed measurement.

## Configuration and deployment

Set these in the actual YAML referenced by `cn.upfc_config`, under
`configuration.scaling`:

```yaml
min_workers: 1
max_workers: 2                 # Choose within the configured slot count.
rx_queue_threshold: 40
queue_sample_interval_ms: 10
queue_window_samples: 10
scale_down_queue_threshold: 10
scale_down_hold_ms: 30000
worker_stop_timeout_ms: 5000
```

Policy defaults are shown above; omitted `max_workers` defaults to slot count.
The window range is 1..1024. The lower threshold can be zero but must be strictly
below the scale-up threshold. Both new timeout values must be positive. The
obsolete `queue_consecutive_samples` key is rejected with a migration message.
The hold precedes the stop; the stop timeout bounds cleanup/exit after STOPPING.

Moving averages introduced registry ABI 6; this implementation uses **ABI 7**
and changes the shared session layout. Rebuild manager, UPF-C, UPF-U and affected
linked control NFs together, then restart the deployment. On CN, use the existing
build procedure (`ninja -C build` for the configured UPF build). Do not attach
mixed binaries to a live deployment. `cn.queue_threshold` only affects qcheck's
display; it does not configure the scaler.

## File map and recommended review order

Paths below are relative to the repository root. Review by responsibility;
the total diff includes tests and documentation as well as runtime changes.

| Review group | Files | What to inspect |
| --- | --- | --- |
| 1. Policy and accounting | `5gc/upf_c/upf_scaling.c`, `.h` | Moving averages, hold cancellation, session counts, pending request reuse, start/stop state machines. |
| 2. Shared protocol/config | `onvm/upf/upf_worker.h`, `.c`; `onvm/upf/upf_context.h`; `5gc/upf_c/upf_worker_config.c`, `upf_config.c`; `config/upfcfg.scaling.example.yaml` under UPF-C | ABI 7, STOPPING, cleanup mailbox ownership, deletion flag, defaults and validation. |
| 3. PFCP deletion | `5gc/upf_c/n4_dispatcher.c`, `n4_onvm_pfcp_handler.c`, `.h`, `n4_onvm_pfcp_build.c`, `.h` | Replay before missing-session handling, deferred deletion, causes, cached response and modification rejection. |
| 4. Worker cleanup | `5gc/upf_u/upf_u.c`, `upf_u_shaper.c`, `.h`, `upf_u_trtcm.c`, `.h` | Owner-thread cleanup/ACK, SEID checks, UE queues and token-state reclamation. |
| 5. Manager teardown | `onvm/onvm_mgr/onvm_upf_steer.c`, `onvm_upf.c`, `.h`, `main.c` | N3 deletion, ownership checks, disable ACK, stale VF drain, TX ring lifetime synchronization. |
| 6. Observation | `scripts/qcheck.py`, `tests/test_qcheck.py` | STOPPING/INACTIVE/failure parsing and changed NF instance on respawn. |
| 7. Verification/docs | `tests/scale_down/`, `tests/proactive_scaling/`, `tests/worker_polling/`, `tests/worker_slots/`; UPF-C README; `docs/sriov-upf/dynamic-scaling.md`; experiment README | Mocks versus integration coverage, deployment steps and experimental limitations. |

Start with [the policy README](../../5gc/upf_c/README.md), then
[the scaler](../../5gc/upf_c/upf_scaling.c). See
[dynamic deployment](dynamic-scaling.md) and
[test scope](../../tests/scale_down/README.md) for operational details.

## Validation recorded in this conversation

Passed locally:

- `python3 tests/proactive_scaling/run.py`: production sampler/placement with
  mock time/queues, ASan/UBSan; full/fresh windows and READY-only admission.
- `python3 tests/scale_down/run.py`: production lifecycle, cleanup and filter
  functions with mocked dependencies, ASan/UBSan. Covers holds, minimum workers,
  busy-peer spare retention, deletion/reader/owner ACK ordering, rollback counts,
  PFCP replay after session removal, failures, stop ordering and generation reuse.
  Also covers hash collisions, shaper packet reclamation and ixgbe readback.
- `python3 tests/worker_polling/run.py`: both FLOW_LOOKUP variants, ASan/UBSan;
  polling ACKs, stale VF packets, actual extracted RX/TX loops, teardown locking
  and concurrent mailbox updates.
- `python3 -m unittest discover -s tests -p test_qcheck.py`: all 10 tests passed.
- `git diff --check`: passed before the commit preparation.

The YAML tests were extended for the new defaults, overrides and invalid
threshold/timeouts, but compilation is blocked locally by missing `yaml.h`.
No dependency installation was completed. The full Linux/DPDK build, actual
PFCP exchange, process lifecycle and NIC behavior have **not** been validated
for this patch on CN. Mock tests do not demonstrate throughput or absence of
hardware packet loss. The shaper/filter test runners extract production code;
they do not compile every complete production translation unit.

## Testbed checks for the later review

1. Build all affected binaries consistently and run the YAML parser tests where
   libyaml development files are installed. Start a fresh run with clean PF rules.
2. Drive scale-out; record `Scale-out trigger`, `Spawned`, READY and admission
   logs. Verify actual slot/IP/TEID rather than inferring placement from UE order.
3. Stop only traffic while retaining a session. Its worker must remain alive.
4. Release the PDU session through normal signaling. Confirm SMF PFCP deletion,
   accepted response and `Deleted SEID=... slot=... sessions=...` in UPF-C.
5. Keep an empty spare while a peer is busy; verify no scale-down. Lower all
   READY averages and verify hold start, STOPPING and INACTIVE, preserving minimum.
6. Raise load or admit a session during the hold; verify cancellation. With
   several empty workers, verify one stop at a time and a fresh hold per stop.
7. Scale out again into the stopped slot. Verify a new generation/registration,
   correct filters/polling and forwarding; repeat release/stop/restart cycles.
8. Inspect resource/filter cleanup and PFCP retry behavior. In controlled failure
   tests, verify uncertain cleanup stays occupied/FAILED and is never reused.
9. Re-measure forwarding throughput and manager CPU cost after TX synchronization.
   Retain logs, config, PF rules and counters; do not infer performance from mocks.

Remaining design limits are intentional: existing-session load is not migrated;
low queue occupancy does not imply zero sessions; failed cleanup has conservative
retention rather than automatic repair. Threshold/window/hold tuning and any
future session migration are separate work.
