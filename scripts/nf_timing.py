#!/usr/bin/env python3
"""Mark experiment phases and summarize the optional per-UPF-U timing CSVs."""

import argparse
import bisect
import csv
from collections import defaultdict
from pathlib import Path
import time


def read_csv(path):
    # A running worker may still be writing its last row. Ignore only that fragment.
    with path.open() as stream:
        lines = stream.readlines()
    if lines and not lines[-1].endswith("\n"):
        lines.pop()
    return list(csv.DictReader(lines))


def ratio(numerator, denominator):
    return numerator / denominator if denominator else ""


def metrics(rows):
    total = lambda field: sum(float(row[field]) for row in rows)
    packets, bursts = total("packets"), total("bursts")
    sampled = total("sampled_packets")
    result = {
        "seconds": total("interval_s"),
        "dequeue_kpps": ratio(packets, total("interval_s") * 1000),
        "packets_per_burst": ratio(packets, bursts),
        "full_burst_pct": ratio(total("full_bursts") * 100, bursts),
        "empty_poll_pct": ratio((total("polls") - bursts) * 100, total("polls")),
        "rx_sample_mean": ratio(total("rx_sum"), total("queue_samples")),
        "rx_sample_max": max(int(row["rx_max"]) for row in rows),
        "tx_sample_mean": ratio(total("tx_sum"), total("queue_samples")),
        "tx_sample_max": max(int(row["tx_max"]) for row in rows),
        "timed_bursts": int(total("samples")),
        "timed_packets": int(sampled),
    }
    for stage in ("dequeue", "handler", "tx", "callback"):
        result[f"{stage}_ns_per_packet"] = ratio(total(f"{stage}_ns"), sampled)
    result["handler_burst_max_ns"] = max(float(row["handler_max_ns"]) for row in rows)
    result["previous_write_max_us"] = max(float(row["previous_write_ns"]) for row in rows) / 1000
    for field in ("nf_rx", "nf_rx_drop", "nf_tx_drop", "nf_action_drop"):
        values = [row[f"{field}_delta"] for row in rows]
        result[f"{field}_delta"] = sum(values) if all(v != "" for v in values) else ""
    return result


def write_csv(path, rows):
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def report(directory):
    marker_path = directory / "phases.csv"
    markers = read_csv(marker_path) if marker_path.exists() else []
    boundaries = [int(row["time_ns"]) for row in markers]
    if any(a >= b for a, b in zip(boundaries, boundaries[1:])):
        raise ValueError("phase timestamps must strictly increase")
    if markers and len(markers) < 2:
        raise ValueError("mark the end of the phase before reporting")
    groups = defaultdict(list)
    intervals = []
    paths = sorted(directory.glob("nf-timing-*.csv"))
    if not paths:
        raise ValueError("no nf-timing-*.csv files found")
    for path in paths:
        previous = None
        for row in read_csv(path):
            if None in row or any(value is None for value in row.values()):
                raise ValueError(f"malformed CSV row in {path}")
            end = int(row["time_ns"])
            duration = float(row["interval_s"])
            if not end or duration <= 0:
                raise ValueError(f"invalid timestamp or interval in {path}")
            if previous and end <= int(previous["time_ns"]):
                raise ValueError(f"wall clock moved backwards in {path}")
            for field in ("nf_rx", "nf_rx_drop", "nf_tx_drop", "nf_action_drop"):
                current = int(row[f"{field}_total"])
                old = int(previous[f"{field}_total"]) if previous else None
                row[f"{field}_delta"] = current - old if old is not None and current >= old else ""
            previous = row
            start = end - round(duration * 1e9)
            phase = "all"
            if markers:
                index = bisect.bisect_right(boundaries, start) - 1
                # Exclude intervals crossing a marker and the open final phase.
                if index < 0 or index + 1 >= len(markers) or end > boundaries[index + 1]:
                    continue
                phase = markers[index]["phase"]
            identity = {key: row[key] for key in ("pid", "tid", "instance", "service", "core")}
            intervals.append({"time_ns": end, "phase": phase, **identity, **metrics([row])})
            groups[(path.name, phase)].append(row)
    if not intervals:
        raise ValueError("no complete timing intervals inside the marked phases")
    summaries = []
    for (source, phase), rows in groups.items():
        identity = {key: rows[0][key] for key in ("pid", "tid", "instance", "service", "core")}
        summaries.append({"source": source, "phase": phase, **identity, **metrics(rows)})
    write_csv(directory / "timing-intervals.csv", sorted(intervals, key=lambda row: row["time_ns"]))
    write_csv(directory / "timing-summary.csv", summaries)
    print(f"Wrote {directory / 'timing-intervals.csv'}")
    print(f"Wrote {directory / 'timing-summary.csv'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    mark = commands.add_parser("mark", help="mark a phase start on the CN host")
    mark.add_argument("directory", type=Path)
    mark.add_argument("phase", help="e.g. UE1_ONLY, BOTH, UE1_AGAIN, END")
    summarize = commands.add_parser("report", help="write interval and phase summary CSVs")
    summarize.add_argument("directory", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "mark":
            path = args.directory / "phases.csv"
            header = not path.exists() or path.stat().st_size == 0
            now = time.time_ns()
            with path.open("a", newline="") as stream:
                writer = csv.writer(stream)
                if header:
                    writer.writerow(["time_ns", "phase"])
                writer.writerow([now, args.phase])
            print(f"{now} {args.phase}")
        else:
            report(args.directory)
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"nf_timing: {error}\n")


if __name__ == "__main__":
    main()
