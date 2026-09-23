# Dynamic UPF-U scale-out

Scope: place new sessions using each ready worker's current RX queue length.
Multiple sessions may share a worker; each session receives its own UL TEID.
No periodic sampling, cooldown, migration, session deletion or scale-down.
All VFs are initialized before runtime activation. The target is Intel
X520/82599 with a Linux DN running iperf.

## Implementation phases

1. **Configuration and registry (this change):** parse resource slots and
   policy, validate manager resources, publish an immutable configuration with
   all runtime entries INACTIVE.
2. **Manager activation:** acknowledged updates to polling and port-to-worker
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

## Phase 1 behavior

The [scaling example](../../5gc/upf_c/config/upfcfg.scaling.example.yaml) defines
`configuration.scaling`. It is mutually exclusive with `upf_u_workers`.
The existing static configuration and packet path continue to work.
**The new configuration only publishes inactive slots in this phase:** it
does not spawn processes, change polling or install filters. Establishment
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
other processes will report readiness through acknowledged messages. No
process-private pointers, process handles or new telemetry counters are shared.
Existing `UpfSession` ownership and classifier structures are unchanged here.

Rebuild/restart manager and UPF-C together before using the scaling config.
Changing a published configuration requires restarting the manager and NFs.

## Validation

[Worker-slot tests](../../tests/worker_slots/README.md) cover the parser and
registry, including rejected configurations and publication ownership.
Local ASan/UBSan checks passed with mock DPDK and YAML document APIs; the example
YAML syntax was checked with the system YAML parser. An extracted-function
harness also checked manager resource validation and static/dynamic exclusion.
The documented libyaml-linked test command and full Linux/DPDK build still need
the development dependencies; hardware steering requires the testbed.
