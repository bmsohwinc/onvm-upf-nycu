# Scale-down checks

Run `python3 tests/scale_down/run.py` from the repository root with a C compiler.
The runner extracts production functions and builds with ASan/UBSan. It mocks
time, processes, DPDK resources and PF ioctls; it never signals real processes.

Coverage: continuous holds, minimum workers, busy-peer spare retention, pending
admissions, session accounting, classifier/owner cleanup ACKs, graceful stop
ordering, failed cleanup, generation reuse, PFCP replay after session removal,
UE hash collisions, shaper buffer reclamation and ixgbe filter readback.
On Linux, the filter test uses the system ethtool definitions; macOS uses the
equivalent fields needed by the builder. Dispatcher coverage extracts its
session/deletion routing prefix and uses the real transaction receive logic.

Also run `python3 tests/worker_polling/run.py` for manager RX/TX synchronization
and stale VF packet cleanup. Actual process startup/shutdown, PF filters and
throughput must be verified on CN; these tests do not replace a full DPDK build.
