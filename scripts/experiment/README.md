# Three-node experiment launchers

One launcher per node, one tmux window/log per process. Python 3 (3.8+), Bash,
tmux, and the existing Linux testbed are required. These scripts do not configure
VFs, bind NICs, change NF algorithms, or connect to other nodes. Use the same run
ID on all three nodes. The launchers run through `sudo -E` so tmux and its panes
share root privileges without separate password prompts for each process.

Copy this directory to each node. On CN, retain the instrumentation-enabled UPF
build and `scripts/qcheck.py` / `scripts/nf_timing.py` in the UPF repository.

## Configuration: do this first

```sh
cp scripts/experiment/config.example.json scripts/experiment/lab.json
```

Edit `lab.json` on each node. `${LAB_HOME}` expands to the invoking user's home
(from `SUDO_USER`), not root's home. Verify repositories, binary locations and
the actual **dynamic scaling** UPF-C YAML. The source-tree `upfcfg.yaml` is not
currently a dynamic worker configuration. Set `max_workers: 2` for this test.

UPF-C uses ONVM manual core assignment (`-m`) to retain the core selected by
`cn.upfc_eal` (`-l 7` in the example). Reserve that core for UPF-C: it must be
enabled by `manager_nf_coremask` and absent from worker slots and control NF
core assignments. EAL `-l` alone does not prevent ONVM from choosing another
core, potentially occupying worker slot 0's core before slot validation.

The example manager NF mask `0xFFFF8` enables cores 3–19, covering workers on
3–6, UPF-C on 7, and the reference control-NF assignments through AUSF on 16
and CHF on 17. The old `0xFFF8` mask enabled only 3–15 and rejected those two
NFs. Update `cn.manager_nf_coremask` and `cn.upfc_eal` in existing `lab.json`
files too; changing the example does not update them.

All control NFs run from `${core_repo}/bin-sriov/<nf>` with `${core_repo}` as
their working directory, matching the reference `run_nf` function. The custom
SMF source lives in `${smf_repo}`, but its build output must be
`${core_repo}/bin-sriov/smf`. The launcher does not compile binaries;
`smf_repo` and `xio_repo` are exported as environment variables, not used to
select alternative executables. After SMF source changes, rebuild on CN using
the existing configured build environment, with SMF stopped:

```bash
export CORE_REPO="$HOME/L25GC-plus"
export SMF_REPO="$HOME/smf-sriov"
export XIO_REPO="$HOME/xio-scaling"
(
    set -e
    cd "$SMF_REPO"
    go mod edit -replace "github.com/nycu-ucr/onvmpoller=$XIO_REPO"
    CGO_LDFLAGS_ALLOW='-Wl,.*' CGO_CFLAGS_ALLOW='^-mrtm$' \
        CGO_ENABLED=1 go build -a -o "$CORE_REPO/bin-sriov/smf" ./cmd
)
```

If ONVM static libraries or X-IO changed, follow the reference build procedure
to rebuild the libraries and all nine control NFs before launching.

The example reproduces your supplied MAC/IP/TEID/rate/size values. CN allows all
eight VFs `0000:06:10.0` through `0000:06:10.7`, with hexadecimal port mask `ff`
(DPDK ports 0–7). Even PCI functions are N3; odd functions are N6. With matching
DPDK enumeration, the worker port pairs are:

| Slot | N3 PCI function / DPDK port | N6 PCI function / DPDK port |
| --- | --- | --- |
| 0 | `0000:06:10.0` / 0 | `0000:06:10.1` / 1 |
| 1 | `0000:06:10.2` / 2 | `0000:06:10.3` / 3 |
| 2 | `0000:06:10.4` / 4 | `0000:06:10.5` / 5 |
| 3 | `0000:06:10.6` / 6 | `0000:06:10.7` / 7 |

Verify the actual manager PCI-to-port mapping, then set `n3_port` / `n6_port`
in your UPF-C `scaling.worker_slots` accordingly. `n3_vf` / `n6_vf` are indices
within their respective PFs, not PCI function numbers or DPDK port IDs; preserve
the correct PF-relative indices. The generic UPF-C example uses a different
port ordering and must be adapted. `cn.slots: 4` controls qcheck monitoring only;
it does not create workers or change UPF-C's `max_workers`.

If you already copied `lab.json`, update its `cn.manager_allow`,
`cn.manager_portmask`, and `cn.slots` too. Updating the example does not change
existing configurations. Restart manager and UPF-C with a fresh run ID after
changing port configuration. The old allowlist included `0000:06:00.1`, which
is not one of these VFs; a log showing only port 0 cannot satisfy an N3/N6 pair.

Check UE IPs, TEIDs and N3 destinations against actual admitted sessions. The
scripts deliberately do not infer them from UE order or modify live sessions.
One clean deployment usually assigns 0x1001/0x1002, but failed admissions can
consume TEIDs. The example tests **64-byte inner IPv4 packets**, not the earlier
256-byte experiment; change `ue.inner_size` explicitly if reproducing that run.

Results default to `/dev/shm/upf-experiments/<run>/<node>` (tmpfs limits timing
CSV write stalls). Use a fresh run ID; existing results are never truncated by a
new start. **Copy results to persistent storage before reboot.** Keep settings
fixed during a run; the resolved configuration and exact launch commands are
saved alongside logs. No local shell startup file is sourced by tmux panes.

## 1. Start CN and DN

First stop any old deployment using your existing cleanup procedure. The
launchers do not invoke `stop_cn.sh`: that helper globally matches processes and
drops MongoDB collections. `cn.sh stop` below only signals this run's panes and
does not clear the database. The manager wrapper/start script retains its usual
hugepage cleanup, so do not use it alongside another ONVM deployment.

On CN, from the UPF repository:

```sh
sudo -E bash scripts/experiment/cn.sh scripts/experiment/lab.json run01 start
```

This starts MongoDB, manager, UPF-C, control NFs and qcheck. It waits for manager
RX startup and the first UPF-U READY; control NFs retain your two-second launch
spacing and are checked for early exit, not full application health. It starts
qcheck immediately and uses **this run's** `upfc.log` for worker discovery.
The UPF-C binary is launched directly with configurable EAL arguments, because
the existing wrapper's inner `sudo` can discard `ONVM_NF_TIMING_DIR`.

On DN (commands assume this directory was copied under `scripts/experiment`):

```sh
sudo -E bash scripts/experiment/dn.sh scripts/experiment/lab.json run01 start
```

This launches interactive testpmd, waits for its prompt, and sends `start`.

## 2. Establish sessions on different workers

On UE:

```sh
sudo -E bash scripts/experiment/ue.sh scripts/experiment/lab.json run01 start
sudo -E bash scripts/experiment/ue.sh scripts/experiment/lab.json run01 ue1
sudo -E bash scripts/experiment/ue.sh scripts/experiment/lab.json run01 load1
```

`start` launches gNB and waits for NG setup. `ue1` waits for successful PDU
establishment; edit the configurable readiness patterns if your UERANSIM logs
use different text. `load1` starts the 120-second setup generator. While worker
1's RX backlog is above threshold in CN's qcheck window:

```sh
sudo -E bash scripts/experiment/ue.sh scripts/experiment/lab.json run01 ue2
```

**Checkpoint on CN:** verify distinct `Admitted SEID ... slot=... TEID=...`
entries and both workers READY. UE2 establishment alone does not prove placement
on worker 2. If placement is wrong, correct setup and use a clean run. If setup
traffic ends before admission, use a longer `setup_duration_s` on the next run.

```sh
sudo -E bash scripts/experiment/ue.sh scripts/experiment/lab.json run01 stop-traffic
```

Leave UE1/UE2/gNB running. Wait until qcheck shows drained RX/TX queues and stable
packet counts. Verify the configured generator session parameters before measurement.

## 3. Measure automatically

Before measurement, synchronize CN/UE clocks using your normal time service.
For example, inspect `timedatectl show -p NTPSynchronized --value` and your NTP
service's offset report on both nodes. UE writes phase markers using its wall
clock; CN timestamps its counters. If clock alignment is uncertain, mark phases
on CN with `nf_timing.py mark` instead and inspect actual ingress transitions.

On DN, with setup traffic stopped/drained:

```sh
sudo -E bash scripts/experiment/dn.sh scripts/experiment/lab.json run01 begin
```

`begin` sends `stop`, `show port stats all`, `clear port stats all`, `start` and
keeps the output in `sink.log`. It starts a fresh **whole-experiment** accounting
window. Do not reset sink counters between the three measured phases.

On UE:

```sh
sudo -E bash scripts/experiment/ue.sh scripts/experiment/lab.json run01 measure
```

A `sequence` tmux window runs UE1 alone → both → UE1 alone, 30 seconds per phase
with three-second settling gaps. It automatically starts/stops generators and
writes `phases.csv`, including transition markers. UE1's generator runs
continuously through all three phases. UE sessions stay established. Generators
receive Ctrl-C so they print final totals; their configured duration is a safety
bound, not the phase timer. Inspect `status` and `sequence.log` for completion;
failure writes `ABORTED` and stops the measurement generators.

After the sequence finishes and CN queues/port counts drain, on DN:

```sh
sudo -E bash scripts/experiment/dn.sh scripts/experiment/lab.json run01 finish
```

This sends `stop` and `show port stats all`, preserving the final sink counts.
Those counts cover settling/transition traffic as well as steady phases. Compare
them with **whole measured generator runs**, not with trimmed phase rates.

Copy UE's `run01/ue/phases.csv` to CN, for example `/tmp/run01-phases.csv`, using
your usual file-transfer method. Then on CN:

```sh
sudo -E bash scripts/experiment/cn.sh scripts/experiment/lab.json run01 report \
  --phases /tmp/run01-phases.csv
```

This runs `nf_timing.py report`, producing interval and phase summaries. Automatic
reporting requires a completed `END` marker and refuses aborted runs. Retain the
raw CN/UE/DN directories, including generator and sink logs, for packet accounting.
Only one measured sequence is supported per run ID; use fresh runs for repeats.

## View and stop

Use the corresponding node launcher with `attach`, `status`, or `stop`:

```sh
sudo -E bash scripts/experiment/cn.sh scripts/experiment/lab.json run01 attach
sudo -E bash scripts/experiment/ue.sh scripts/experiment/lab.json run01 status
sudo -E bash scripts/experiment/dn.sh scripts/experiment/lab.json run01 stop
```

tmux: **Ctrl-b w** selects a window, **Ctrl-b d** detaches. Each process remains
visible after exit and has its own log. `stop-traffic` on UE aborts a running
sequence and stops generators while keeping UE/gNB sessions. Full `stop` on each
node stops its experiment processes; on CN it stops UPF-C before manager. It does
not force-kill hung processes or stop MongoDB. Inspect any reported timeout.

After stopping capture, archive each node's run directory, for example:

```sh
mkdir -p "$HOME/experiment-results"
cp -r /dev/shm/upf-experiments/run01 "$HOME/experiment-results/"
```

Real tmux/DPDK startup and testpmd prompt handling require validation on the nodes.
