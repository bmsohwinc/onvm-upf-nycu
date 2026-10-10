# Software-LB CN launcher

Run on the CN node from the **sw-lb-upf checkout**. Starts MongoDB, manager,
UPF-C, all configured UPF-U workers, the nine control NFs, and qcheck on a separate
core. Each process has a tmux window and log, like the SR-IOV experiment launcher.

## First use

```bash
cd "$HOME/onvm-upf-scaling"
sudo apt-get install tmux python3-yaml   # only if missing
cp scripts/sw-lb/config.example.json scripts/sw-lb/config.json
```

Edit `scripts/sw-lb/config.json`:

- `nf_bin`: actual directory containing all nine built control NFs. Defaults to
  your working `~/L25GC-plus/bin-sriov`; the name does not select a protocol mode.
  The script does not rebuild SMF/X-IO or establish binary compatibility.
- `nf_config_dir`: source ONVM JSONs, normally `~/L25GC-plus/onvm_nf_configs`.
  The launcher preserves their service/instance IDs and other fields, but sets
  core assignments and the shared two-VF allowlist in **run-local copies**.
- `upfc_config`: original single-UPF YAML. The default is this checkout's
  `5gc/upf_c/config/upfcfg.yaml`. Use your existing `$HOME/sw-lb-config/upfcfg.yaml`
  if it contains required local PFCP/DNN settings. Scaling configs are rejected.
- `n3_peer_mac` / `n6_peer_mac`: the remote next-hop MACs you supplied. Set either
  to JSON `null` to use ARP on that port. These are destination MACs, not CN MACs.
- `workers`: defaults to instances 14–17 on cores 3–6. Keep just the entries
  needed for a 1/2/3-worker experiment; the launcher generates the matching map.
- `monitor_core`: qcheck CPU, default **19**, separate from manager and all NFs.
- `queue_interval_s`: sampling interval, default **0.2 seconds**, matching the
  SR-IOV launcher. `queue_threshold`: RX occupancy for status events, default
  **40 packets**; it does not change forwarding or trigger scaling. Existing
  configs can omit these three fields and use the defaults.

`${LAB_HOME}` resolves to the invoking user's home, even under sudo.
`${UPF_REPO}` resolves to the checkout containing this script.

The fixed CPU layout is manager 0–2, UPF-C 7, workers 3–6, control NFs as listed
in `control_cores`, qcheck 19. Service 1 is reserved for workers and service/instance 2 for
UPF-C. Duplicate cores/instance IDs and missing executables fail before startup.
The EAL prefix is `rte`, matching the JSON-based control-NF integration.

## Start

Stop the previous CN deployment first. The launcher refuses an existing manager,
UPF or control-NF process. Use a fresh run ID each time:

```bash
# Read-only: validate inputs/binaries and print the planned commands.
bash scripts/sw-lb/cn.sh scripts/sw-lb/config.json sw01 check

sudo -E bash scripts/sw-lb/cn.sh scripts/sw-lb/config.json sw01 start
```

All inputs are checked before starting processes. Run-local files are saved in
`/dev/shm/upf-experiments/sw01/cn/config/`:

- `map.conf`: shared N3/N6 ports/IPs and ordered worker IDs, without TEIDs/UE IPs.
- `upf_u.yaml`: identical config for every worker, NAT disabled, optional peer MACs.
- `upfcfg.yaml`: copy of the original config with shared N3 address, ports 0/1,
  and info logging for TEIDs. PFCP and DNN settings are preserved.
- `nf/*.json` and IP/ID mapping files for the control NFs.

The source configs are not edited. Exact commands, resolved launcher config and
per-process shell scripts are retained beside `manager.log`, `upfc.log`,
`upfu-14.log` … `upfu-17.log`, and `smf.log` etc. Logs are captured by tmux, so
there is no user-shell redirection into a root-owned log directory.

Manager polls only `06:10.0` (N3/port 0) and `06:10.1` (N6/port 1), with port
mask `3`. All workers share that pair. This script does not configure/bind NICs,
install NIC classification rules, clean hugepages, or launch gNB/UE/DN processes.
Keep PFs in Linux for N2 and remove old experiment steering to other VF pairs
using your existing NIC setup procedure. Confirm PCI-to-port enumeration in EAL.

Startup waits for manager RX and for each NF's **expected instance/service/core**
to appear in manager stats, then starts qcheck and requires valid RX/TX queue
samples for every configured worker. Controls retain two-second launch spacing.
A timeout or early exit stops this run's processes, keeping logs/panes for diagnosis.
Registration is not a PFCP/SBI/session health check. If a control NF appears on
the wrong core, check the X-IO build's ONVM manual-core-assignment fix; `taskset`
alone does not stop ONVM from reassigning its polling core.

The launcher uses your existing `CORE_REPO/config` SMF/AMF/etc. configs. Keep
SMF's single UPF endpoint at `10.10.2.11` (or your configured `n3_ip`) with
matching PFCP/DNN settings. Rebuild manager/UPFs together after source changes;
rebuild control NFs too when their statically linked ONVM/X-IO ABI changes.

After startup, start gNB and attach your UEs. Send low-rate traffic from each UE
in order to learn round-robin ownership, then run traffic concurrently. Attaching
four UEs alone does not assign manager workers; **first data packets do**.

```bash
tail -f /dev/shm/upf-experiments/sw01/cn/manager.log
grep 'Session established' /dev/shm/upf-experiments/sw01/cn/upfc.log
grep 'UPF LB learned' /dev/shm/upf-experiments/sw01/cn/manager.log
```

## Queue capture

qcheck starts automatically with `start` and stops with `stop`. It reuses the
SR-IOV telemetry sampler in `scripts/qcheck.py`, selecting the configured worker
**instance IDs** and shared ports **0 (N3)** / **1 (N6)**. It requires the manager's
DPDK telemetry socket `/var/run/dpdk/rte/dpdk_telemetry.v2` and its `/ring/info`,
`/mempool/info`, and `/ethdev/stats` endpoints. No dataplane rebuild is needed for
this launcher change if your existing DPDK build provides these endpoints.

Files under `/dev/shm/upf-experiments/sw01/cn/`:

- `queues.csv`: timestamped samples, flushed after every row. `nf14_rx` /
  `nf14_tx` are worker 14's ONVM input/output ring occupancies in packets, with
  corresponding capacity columns; likewise for each other configured worker.
  RX measures packets waiting for the worker; TX measures packets waiting for
  manager forwarding. These rings combine UL and DL traffic. Ports 0/1 provide
  aggregate NIC packet rates and error/drop counters, not per-worker rates.
- `qcheck.log`: CPU affinity, RX threshold transitions, and one-second status
  summaries. `peak` is the largest **sampled** RX occupancy since the previous
  summary. The CSV retains occupancies both below and above the threshold.

`time_ns` is Unix wall-clock nanoseconds; `elapsed_s` is monotonic capture time.
Record the start/end time and offered rate of each traffic phase to match queue
samples to RTT measurements. SR-IOV's dynamic mode labels columns by `slotN`;
SW-LB's fixed mode uses `nf<instance>`, matching `config/map.conf` and `resolved.json`.

Sampling is sequential, not an atomic snapshot. Missing readings are blank,
never assumed zero; `sample_ms` and `interval_overrun` expose slow sampling.
Short queue bursts between samples can be missed, and these are **software ring
occupancies**, not hardware RX/TX queue depths or packets already dequeued into
processing batches. Empty samples therefore do not establish absence of queueing.
Use the same interval for both designs; smaller intervals increase telemetry work
in the manager as well as in the pinned qcheck client.

```bash
tail -f /dev/shm/upf-experiments/sw01/cn/qcheck.log
```

## Inspect and stop

```bash
sudo -E bash scripts/sw-lb/cn.sh scripts/sw-lb/config.json sw01 status
sudo -E bash scripts/sw-lb/cn.sh scripts/sw-lb/config.json sw01 attach
# Stop traffic/UEs on the other nodes before stopping CN.
sudo -E bash scripts/sw-lb/cn.sh scripts/sw-lb/config.json sw01 stop
```

Tmux: **Ctrl-b w** selects a window; **Ctrl-b d** detaches. `status` reports
process liveness, not session health. The idle `control` window remains alive.
`stop` sends Ctrl-C only to this run's panes: qcheck, controls, UPF-C, workers,
manager last. It leaves MongoDB running, retains results, and never clears subscriber
data or force-kills hung processes. A stop timeout leaves remaining processes
running for inspection. Copy `/dev/shm` results to persistent storage before reboot.

## Validation

```bash
python3 tests/test_sw_lb_launcher.py
python3 tests/test_qcheck.py
bash -n scripts/sw-lb/cn.sh
```

Tests use temporary configs/binaries and mocked process control. Real tmux,
DPDK registration, PFCP establishment and traffic still require the Linux testbed.
