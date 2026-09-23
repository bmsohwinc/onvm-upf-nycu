# Manager worker-polling tests

Run from the repository root; only Python 3, a C compiler and pthreads are needed:

```sh
python3 tests/worker_polling/run.py
```

The harness compiles the production registry, manager routing, RX packet and
common packet code with mock DPDK/ONVM structures. It extracts the actual
`rx_thread_main` function to check which ports reach `rte_eth_rx_burst`.
Both default-chain and `FLOW_LOOKUP` builds run under ASan/UBSan.

Coverage: configuration installation, inactive VF skipping, activation ACK
ordering, exact-instance dispatch, ordinary ports, legacy mapping, full-ring
drops, unchanged OUT/TX behavior, invalid registration/core/service/rings,
rollback, duplicate/stale generations, NF removal and sequence exhaustion.
A concurrent producer/manager test performs 3,000 mailbox updates with both
success and error results. Invalid manager configuration runs in a fresh
process to verify failure without installing a partial map.

The mutex substitute checks the lock contract; it does not measure DPDK
spinlock performance. EAL multi-process behavior, full manager lifecycle/TX
concurrency, and real VF traffic still require the Linux/DPDK testbed.
