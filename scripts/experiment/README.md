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

The example reproduces your supplied MAC/IP/TEID/rate/size values. **Check the
CN PCI allowlist and portmask:** your supplied manager command names two devices
and defaults to mask `3`. Two dynamic workers require their configured N3/N6
ports (four distinct DPDK ports). Populate the actual allowlist and corresponding
mask; do not guess PCI addresses or infer DPDK port IDs from VF indices. Set
`cn.slots` to cover all configured slots that may appear in the UPF-C log.

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
