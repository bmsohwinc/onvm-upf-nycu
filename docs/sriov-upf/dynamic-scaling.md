# Dynamic UPF-U scale-out

Scope: place new sessions using each ready worker's current RX queue length.
Multiple sessions may share a worker; each session receives its own UL TEID.
No periodic sampling, cooldown, migration, session deletion or scale-down.
All VFs are initialized before runtime activation. The target is Intel
X520/82599 with a Linux DN running iperf.

## Implementation phases

1. **Configuration and registry (complete):** parse resource slots and
   policy, validate manager resources, publish an immutable configuration with
   all runtime entries INACTIVE.
2. **Manager activation (implemented):** acknowledged updates to polling and port-to-worker
   dispatch; NIC device initialization stays at startup.
3. **UPF-C admission and spawning:** inspect RX queues on establishment,
   launch/register a worker when needed, synchronize joining classifier
   readers, allocate per-session TEIDs, and handle pending establishments/retries
   without blocking the ONVM event loop.
4. **Steering and establishment completion:** manager installs PF filters on
   UPF-C's request; establish the DN's UE /32 return route and await completion
   before returning the endpoint to SMF.
5. **End-to-end checks:** verify failure/retry behavior, classifier lifetime,
   multiple sessions per worker and load-triggered scale-out on the testbed.

Each phase is reviewed and approved before its commit and the next phase.

## Configuration behavior

The [scaling example](../../5gc/upf_c/config/upfcfg.scaling.example.yaml) defines
`configuration.scaling`. It is mutually exclusive with `upf_u_workers`.
The existing static configuration and packet path continue to work.
**The new configuration publishes inactive slots:** once the manager installs
the map, it skips those slots' VF pairs. It does not spawn processes or install filters. Establishment
requests in this mode return NO_RESOURCES_AVAILABLE, preventing accidental
fallback to legacy allocation before dynamic admission is implemented.

The slot ID is its zero-based array index. Each slot specifies a distinct
service/core, N3/N6 port pair, and N3/N6 IP pair. `n3_vf` and `n6_vf` are indices
on the named Linux PFs; they are different identifiers from DPDK port IDs.
Match ports against manager startup logs. MACs remain in existing shared port
information. PF-to-VF device identity and filter capabilities will be validated
by the steering phase; this phase checks configured port availability only.

`min_workers` defaults to 1, `max_workers` to the number of slots, and
`rx_queue_threshold` to 1024 packets. The threshold must be positive and below
both the NF ring size and the shared dataplane mbuf count. `teid_first` defaults
to `0x1001`, `teid_last` to `0xffffffff`; this range will be used for sessions,
not assigned as one TEID per worker. `worker_binary` must be an absolute path;
executable availability will be checked when launching is implemented.

## Shared ownership

The manager creates `UPF_WORKERS` with an ABI version and service-ID limit.
UPF-C attaches, validates the complete configuration, then publishes it once
with release/acquire ordering. Validation rejects unavailable/occupied cores
and services, uninitialized ports, duplicate slot resources, and invalid
worker/TEID/threshold bounds. Startup validation does not reserve ONVM cores or
services; admission must check availability again before spawning.

`UpfWorkerRegistry.config` holds value-only configuration;
`runtime[slot]` holds state, generation and the assigned NF instance ID.
States are INACTIVE, STARTING, READY and FAILED. UPF-C will own transitions;
manager polling ACKs are separate from these states. No
process-private pointers, process handles or new telemetry counters are shared.
Existing `UpfSession` ownership and classifier structures are unchanged here.

Rebuild/restart manager and NFs together before using the scaling config;
phase 2 changes the registry ABI to version 2.
Changing a published configuration requires restarting the manager and NFs.

## Phase 2: polling and dispatch

Manager initializes/starts all configured NIC ports once. Its RX thread checks
the shared configuration before each polling pass; installation replaces the
entire legacy service map and marks every pool slot inactive. Non-pool ports
use the ordinary service chain. Without a dynamic configuration, legacy
polling/routing remains active. An out-of-range legacy service drops ingress
instead of preventing manager startup with a smaller dynamic service range.

UPF-C's phase 3 event loop will use this nonblocking sequence:

1. Wait for `UpfWorkerPollingStatus() == 1` (zero is pending; negative is an error).
2. After worker registration and classifier synchronization, submit
   `UpfWorkerPollRequest({slot, instance_id, generation, enable=1}, &sequence)`.
   Only one request may be outstanding; another submission returns `-EBUSY`.
3. Poll `UpfWorkerPollResult(sequence, &result)`; `-EINPROGRESS` means pending.
   Completion returns zero, with `result` holding zero or a negative errno.
   A successful ACK confirms the polling/dispatch update, not NIC filters,
   classifier readiness or session admission.

The mailbox uses release/acquire sequence counters. At an RX-pass boundary,
manager verifies the exact NF is running, has both rings, matches the slot's
service/core, and is the service's only registered instance. One route entry
controls both VF ports, so the ACK follows installation of both mappings.
Packets bypass service hashing and go through the existing batching/full-ring
drop path to that instance's RX ring. Worker TX remains unchanged.

`enable=0` disables both ports for startup rollback; it does not stop the NF or
remove filters. Generation starts at one and increases for each start attempt.
Repeated activation of the same active tuple and repeated disable are
idempotent; reactivation after disable/removal requires a newer generation.
An active slot cannot be rebound without first disabling it. Never overwrite
a pending request after a timeout: wait for its ACK, then roll back if needed.

The implementation requires the existing single manager RX thread. A lock
serializes each RX pass with manager NF registration/removal; STOP removes the
route before freeing RX rings, and an instance-ID reuse cannot revive the old
mapping. This can briefly pause polling during NF lifecycle operations. Port
initialization arrays, devices and TX queues are not changed at activation.

## Validation

[Worker-slot tests](../../tests/worker_slots/README.md) cover the parser and
registry, including rejected configurations and publication ownership.
Local ASan/UBSan checks passed with mock DPDK and YAML document APIs; the example
YAML syntax was checked with the system YAML parser. An extracted-function
harness also checked manager resource validation and static/dynamic exclusion.
The documented libyaml-linked test command and full Linux/DPDK build still need
the development dependencies; hardware steering requires the testbed.

[Manager polling tests](../../tests/worker_polling/README.md) run without DPDK
or libyaml. ASan/UBSan checks cover the actual RX loop and routing code,
success/error ACKs, rejected registrations, stale requests, rollback, legacy
and TX behavior, and concurrent mailbox use. Real VF traffic and full manager
lifecycle concurrency remain testbed checks.
