#!/usr/bin/env python3
"""Sample NF rings, shared packet buffers, and port counters as CSV.

Dynamic: sudo python3 scripts/qcheck.py --upfc-log upfc.log --slots 4 > qcheck.csv
Fixed IDs: sudo python3 scripts/qcheck.py 2 9 --ports 0 1 2 3 > qcheck.csv
Samples are sequential, not an atomic snapshot. Blank cells mean unavailable
data (or no previous sample for rates/deltas), never an assumed zero.
Add --watch for readable status and threshold events on stderr; stdout stays CSV.
"""

import argparse
import csv
import json
import math
import os
import re
import socket
import sys
import time


class WorkerLog:
    """Follow this run's INFO log; slot IDs are stable, NF instance IDs are not."""

    spawn = re.compile(r"Spawned UPF-U slot=(\d+) pid=\d+ service=(\d+)")
    ready = re.compile(r"UPF-U slot=(\d+) instance=(\d+) READY")
    lifecycle = re.compile(r"UPF-U slot=(\d+) (STOPPING|INACTIVE)")
    failed = re.compile(r"UPF-U slot[ =](\d+) (?:exited|startup failed|stop failed|stop timed out)")

    def __init__(self, path, slots):
        self.path = path
        self.workers = [{"state": "unseen"} for _ in range(slots)]
        self.offset = 0
        self.identity = None
        self.pending = b""

    def update(self):
        try:
            stream = open(self.path, "rb")
        except FileNotFoundError:
            if self.identity is not None:
                raise RuntimeError("UPF-C log disappeared; start a new capture for a new run")
            return  # Capture may start before UPF-C creates its log.
        with stream:
            stat = os.fstat(stream.fileno())
            identity = (stat.st_dev, stat.st_ino)
            if self.identity is not None and (identity != self.identity or stat.st_size < self.offset):
                raise RuntimeError("UPF-C log rotated/truncated; start a new capture for a new run")
            self.identity = identity
            stream.seek(self.offset)
            data = self.pending + stream.read()
            self.offset = stream.tell()
        lines = data.split(b"\n")
        self.pending = lines.pop()  # Do not consume a partially written event.
        for raw in lines:
            line = raw.decode("utf-8", errors="replace")
            match = (self.spawn.search(line) or self.ready.search(line) or
                     self.lifecycle.search(line) or self.failed.search(line))
            if not match:
                continue
            slot = int(match[1])
            if slot >= len(self.workers):
                raise RuntimeError(f"log contains slot {slot}; increase --slots")
            if match.re is self.spawn:
                self.workers[slot] = {"state": "starting", "service": int(match[2])}
            elif match.re is self.ready:
                self.workers[slot].update(state="ready", instance=int(match[2]))
            elif match.re is self.lifecycle:
                self.workers[slot]["state"] = match[2].lower()
                if match[2] == "INACTIVE":
                    self.workers[slot].pop("instance", None)
            else:
                self.workers[slot]["state"] = "failed"


COUNTERS = ("ipackets", "opackets", "ibytes", "obytes", "rx_nombuf", "imissed", "ierrors", "oerrors")
RATES = {"ipackets": ("rx_pps", 1), "opackets": ("tx_pps", 1),
         "ibytes": ("rx_bps", 8), "obytes": ("tx_bps", 8)}


class QueueMonitor:
    """Human-readable sampled observations, not controller scaling decisions."""

    def __init__(self, labels, threshold, status_interval):
        self.labels = labels
        self.threshold = threshold
        self.status_interval = status_interval
        self.previous = {}
        self.peaks = {}
        self.all_high = None
        self.next_status = 0

    def update(self, row):
        elapsed = row["elapsed_s"]
        stamp = time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(row["time_ns"] / 1e9))
        stamp += f".{row['time_ns'] // 1_000_000 % 1000:03d}Z +{elapsed:.3f}s"

        def emit(message):
            print(f"[{stamp}] {message}", file=sys.stderr, flush=True)

        summary = []
        active = []
        for label in self.labels:
            state, iid = row[f"{label}_state"], row[f"{label}_instance"]
            identity = (state, iid)
            old_identity, old_high = self.previous.get(label, (None, None))
            if identity != old_identity:
                emit(f"WORKER {label} state={state} instance={iid if iid != '' else '-'}")
                old_high = None
                self.peaks.pop(label, None)
            rx = row.get(f"{label}_rx")
            high = row.get(f"{label}_rx_ge_threshold")
            if state in ("ready", "explicit"):
                active.append(high)
                if isinstance(rx, int):
                    self.peaks[label] = max(rx, self.peaks.get(label, rx))
                    if high != old_high:
                        if high:
                            emit(f"RX_HIGH {label} instance={iid} rx={rx} >= threshold={self.threshold}"
                                 + (" (first valid observation)" if old_high is None else ""))
                        elif old_high is not None:
                            emit(f"RX_LOW {label} instance={iid} rx={rx} < threshold={self.threshold}")
                    tx = row.get(f"{label}_tx", "?")
                    summary.append(f"{label}(nf{iid}): rx={rx} tx={tx} peak={self.peaks[label]} "
                                   f"{'HIGH' if high else 'below'}")
                else:
                    if old_high is not None or identity != old_identity:
                        emit(f"RX_UNKNOWN {label} instance={iid}; telemetry unavailable")
                    summary.append(f"{label}(nf{iid}): RX unavailable")
            else:
                summary.append(f"{label}: {state}")
            self.previous[label] = (identity, high)

        all_high = row.get("all_rx_ge_threshold")
        if all_high == 1 and self.all_high != 1:
            emit(f"ALL_SAMPLED_HIGH workers={len(active)} threshold={self.threshold}; "
                 "UPF-C must resample on a NEW session; this is not a spawn event")
        elif self.all_high == 1 and all_high != 1:
            emit("ALL_SAMPLED_HIGH ended: " +
                 ("at least one RX below threshold" if all_high == 0 else "no complete valid sample"))
        self.all_high = all_high
        if elapsed >= self.next_status:
            emit("STATUS " + " | ".join(summary))
            self.peaks.clear()
            self.next_status = elapsed + self.status_interval


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("instances", type=int, nargs="*", help="explicit NF instance IDs (not services)")
    parser.add_argument("--upfc-log", help="follow worker lifecycle events in this run's UPF-C INFO log")
    parser.add_argument("--slots", type=int, default=4, help="fixed slot columns in log mode (default: 4)")
    parser.add_argument("--ports", type=int, nargs="+", help="default: all DPDK ports")
    parser.add_argument("--interval", type=float, default=0.1)
    parser.add_argument("--watch", action="store_true", help="print threshold events/status to stderr")
    parser.add_argument("--threshold", type=int, default=1024,
                        help="monitor threshold; must match UPF-C YAML (default: 1024); does not change UPF-C")
    parser.add_argument("--status-interval", type=float, default=1,
                        help="seconds between --watch summaries (default: 1)")
    parser.add_argument("--count", type=int, default=0, help="samples; 0 means until Ctrl-C")
    parser.add_argument("--socket", default="/var/run/dpdk/rte/dpdk_telemetry.v2")
    parser.add_argument("--pool", default="MProc_pktmbuf_pool")
    args = parser.parse_args()
    if not math.isfinite(args.interval) or args.interval <= 0 or args.count < 0:
        parser.error("interval must be positive and finite; count must be nonnegative")
    if args.threshold <= 0 or not math.isfinite(args.status_interval) or args.status_interval <= 0:
        parser.error("threshold and status interval must be positive and finite")
    if bool(args.instances) == bool(args.upfc_log):
        parser.error("supply instance IDs OR --upfc-log (no assumed instance IDs)")
    if not 1 <= args.slots <= 32:
        parser.error("slots must be between 1 and 32")
    if any(i < 0 for i in args.instances + (args.ports or [])):
        parser.error("instance and port IDs must be nonnegative")
    if len(set(args.instances)) != len(args.instances) or len(set(args.ports or [])) != len(args.ports or []):
        parser.error("duplicate instance or port IDs")
    worker_log = WorkerLog(args.upfc_log, args.slots) if args.upfc_log else None
    labels = ([f"slot{i}" for i in range(args.slots)] if worker_log else
              [f"nf{i}" for i in args.instances])
    monitor = QueueMonitor(labels, args.threshold, args.status_interval) if args.watch else None
    if monitor:
        affinity = (','.join(map(str, sorted(os.sched_getaffinity(0))))
                    if hasattr(os, "sched_getaffinity") else "unavailable")
        print(f"qcheck: pid={os.getpid()} allowed_cpus={affinity} monitor_threshold={args.threshold} "
              f"sample_interval={args.interval}s; CSV=stdout, watch=stderr; "
              "threshold must match UPF-C YAML; peak=max sampled RX since previous STATUS", file=sys.stderr, flush=True)

    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as sock:
        sock.settimeout(5)
        sock.connect(args.socket)
        greeting = json.loads(sock.recv(65536))
        if not isinstance(greeting, dict):
            raise RuntimeError("invalid telemetry greeting")
        reply_size = max(65536, int(greeting.get("max_output_len", 65536)))
        warned = set()

        def query(command, expected=dict):
            sock.sendall(command.encode())
            reply = sock.recv(reply_size)
            if not reply:
                raise RuntimeError("telemetry socket closed")
            envelope = json.loads(reply)
            result = envelope.get(command.split(",", 1)[0]) if isinstance(envelope, dict) else None
            if not isinstance(result, expected) or not result:
                if command not in warned:
                    print(f"Unavailable: {command}; recording blank cells", file=sys.stderr)
                    warned.add(command)
                return {}
            return result

        commands = query("/", list)
        required = {"/ring/info", "/mempool/info", "/ethdev/stats"}
        if args.ports is None:
            required.add("/ethdev/list")
        missing = required.difference(commands)
        if missing:
            raise RuntimeError(f"telemetry commands unavailable: {', '.join(sorted(missing))}")
        ports = args.ports
        if ports is None:
            ports = query("/ethdev/list", list)
            if not isinstance(ports, list):
                raise RuntimeError("cannot list ports; specify --ports")

        fields = ["time_ns", "elapsed_s", "sample_ms", "interval_overrun", "rx_threshold", "all_rx_ge_threshold"]
        for label in labels:
            fields += [f"{label}_{key}" for key in
                       ("state", "instance", "service", "rx", "tx", "rx_capacity", "tx_capacity", "rx_ge_threshold")]
        fields += ["log_ready_workers", "sampled_rx_count", "rx_sum", "rx_min", "rx_max",
                   "pool_size", "pool_common_free", "pool_cached_free",
                   "pool_free_total", "pool_in_use"]
        for port in ports:
            fields += [f"p{port}_{key}" for key in ("rx_pps", "tx_pps", "rx_bps", "tx_bps")]
            for key in COUNTERS:
                fields += [f"p{port}_{key}", f"p{port}_{key}_delta"]
        writer = csv.DictWriter(sys.stdout, fieldnames=fields)
        writer.writeheader()
        sys.stdout.flush()
        previous = {}
        sample = 0
        origin = time.monotonic()
        overrun_warned = False
        while not args.count or sample < args.count:
            start = time.monotonic()
            row = {"time_ns": time.time_ns(), "elapsed_s": round(start - origin, 6),
                   "rx_threshold": args.threshold}
            if worker_log:
                worker_log.update()
                workers = worker_log.workers
                row["log_ready_workers"] = sum(w["state"] == "ready" for w in workers)
            else:
                workers = [{"state": "explicit", "instance": iid} for iid in args.instances]
            rx_counts = []
            for label, worker in zip(labels, workers):
                for key in ("state", "instance", "service"):
                    row[f"{label}_{key}"] = worker.get(key, "")
                if worker["state"] not in ("ready", "explicit"):
                    continue
                iid = worker["instance"]
                for direction in ("RX", "TX"):
                    info = query(f"/ring/info,MProc_Client_{iid}_{direction}")
                    row[f"{label}_{direction.lower()}"] = info.get("used_count", "")
                    row[f"{label}_{direction.lower()}_capacity"] = info.get("capacity", "")
                    if direction == "RX":
                        rx_counts.append(info.get("used_count"))
                        if isinstance(info.get("used_count"), int):
                            row[f"{label}_rx_ge_threshold"] = int(info["used_count"] >= args.threshold)
            row["sampled_rx_count"] = sum(isinstance(n, int) for n in rx_counts)
            if rx_counts and all(isinstance(n, int) for n in rx_counts):
                row["rx_sum"] = sum(rx_counts)
                row["rx_min"] = min(rx_counts)
                row["rx_max"] = max(rx_counts)
                row["all_rx_ge_threshold"] = int(min(rx_counts) >= args.threshold)

            pool = query(f"/mempool/info,{args.pool}")
            row["pool_size"] = pool.get("size", "")
            row["pool_common_free"] = pool.get("common_pool_count", "")
            row["pool_cached_free"] = pool.get("total_cache_count", "")
            if all(isinstance(pool.get(k), int) for k in
                   ("populated_size", "common_pool_count", "total_cache_count")):
                free = pool["common_pool_count"] + pool["total_cache_count"]
                if 0 <= free <= pool["populated_size"]:
                    row["pool_free_total"] = free
                    row["pool_in_use"] = pool["populated_size"] - free

            for port in ports:
                stats = query(f"/ethdev/stats,{port}")
                now = time.monotonic()
                for key in COUNTERS:
                    row[f"p{port}_{key}"] = stats.get(key, "")
                if port in previous:
                    then, old = previous[port]
                    elapsed = now - then
                    for key in COUNTERS:
                        value, before = stats.get(key), old.get(key)
                        if isinstance(value, int) and isinstance(before, int) and value >= before:
                            delta = value - before
                            row[f"p{port}_{key}_delta"] = delta
                            if key in RATES and elapsed > 0:
                                output, multiplier = RATES[key]
                                row[f"p{port}_{output}"] = round(delta * multiplier / elapsed)
                previous[port] = (now, stats)
            duration = time.monotonic() - start
            row["sample_ms"] = round(duration * 1000, 3)
            row["interval_overrun"] = int(duration > args.interval)
            if duration > args.interval and not overrun_warned:
                print("qcheck: sampling exceeds --interval; increase it or select fewer --ports", file=sys.stderr)
                overrun_warned = True
            writer.writerow(row)
            sys.stdout.flush()
            if monitor:
                monitor.update(row)
            sample += 1
            if not args.count or sample < args.count:
                time.sleep(max(0, args.interval - (time.monotonic() - start)))


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"qcheck: {exc}", file=sys.stderr)
        sys.exit(1)
