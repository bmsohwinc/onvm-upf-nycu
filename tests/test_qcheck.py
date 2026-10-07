"""Portable telemetry/log replay checks; no DPDK or live socket required.

Run: python3 -m unittest discover -s tests -p 'test_qcheck.py' -v
"""

import contextlib
import csv
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


SPEC = importlib.util.spec_from_file_location("qcheck", Path(__file__).resolve().parents[1] / "scripts/qcheck.py")
qcheck = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(qcheck)


class Telemetry:
    def __init__(self):
        self.command = None
        self.sample = 0
        self.commands = []
        self.supported = ["/ring/info", "/mempool/info", "/ethdev/list", "/ethdev/stats"]

    def __enter__(self):
        return self

    def __exit__(self, *args):
        pass

    def settimeout(self, value):
        pass

    def connect(self, path):
        pass

    def sendall(self, command):
        self.command = command.decode()
        self.commands.append(self.command)

    def recv(self, size):
        if self.command is None:
            return b'{"max_output_len": 16384}'
        endpoint = self.command.split(",")[0]
        if endpoint == "/":
            value = self.supported
        elif endpoint == "/ethdev/list":
            value = [0]
        elif endpoint == "/ring/info":
            # Worker appears late; later RX data goes missing, rather than zero.
            if "_17_" in self.command and self.sample == 2:
                value = None
            else:
                value = {"used_count": 1200 if "_RX" in self.command else 0,
                         "capacity": 4095 if "_RX" in self.command else 65535}
        elif endpoint == "/mempool/info":
            value = {"size": 32767, "populated_size": 32767,
                     "common_pool_count": 20000, "total_cache_count": 1000}
        else:
            value = {"ipackets": [100, 150, 20][self.sample], "opackets": 20,
                     "ibytes": [51200, 76800, 10240][self.sample], "obytes": 10240,
                     "rx_nombuf": 0, "imissed": 0, "ierrors": 0}
            # oerrors deliberately unsupported/missing.
        return json.dumps({endpoint: value}).encode()


class QcheckTests(unittest.TestCase):
    def capture(self, args, telemetry, step=None):
        output, errors = io.StringIO(), io.StringIO()

        def advance(seconds):
            telemetry.sample += 1
            if step:
                step(telemetry.sample)

        with patch.object(qcheck.sys, "argv", ["qcheck.py", *args]), \
                patch.object(qcheck.socket, "socket", return_value=telemetry), \
                patch.object(qcheck.time, "sleep", side_effect=advance), \
                contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            qcheck.main()
        return list(csv.DictReader(io.StringIO(output.getvalue()))), errors.getvalue()

    def test_dynamic_workers_and_counter_reset(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "upfc.log"
            log.write_text("Spawned UPF-U slot=0 pid=123 service=14 core=3\n"
                           "UPF-U slot=0 instance=2 READY\n")

            def spawn(sample):
                if sample == 1:
                    with log.open("a") as stream:
                        stream.write("Spawned UPF-U slot=1 pid=456 service=15 core=4\n"
                                     "UPF-U slot=1 instance=17 READY\n")

            rows, warnings = self.capture(["--upfc-log", str(log), "--count", "3"], Telemetry(), spawn)
        self.assertEqual(len(rows), 3)
        self.assertEqual(rows[0]["slot1_state"], "unseen")
        self.assertEqual(rows[0]["slot1_rx"], "")
        self.assertEqual(rows[1]["slot1_instance"], "17")
        self.assertEqual(rows[1]["slot1_service"], "15")
        self.assertEqual(rows[1]["slot1_rx"], "1200")
        self.assertEqual(rows[1]["slot1_rx_capacity"], "4095")
        self.assertEqual(rows[1]["log_ready_workers"], "2")
        self.assertEqual(rows[1]["rx_sum"], "2400")
        self.assertEqual(rows[1]["rx_min"], "1200")
        self.assertEqual(rows[1]["slot1_rx_ge_threshold"], "1")
        self.assertEqual(rows[1]["all_rx_ge_threshold"], "1")
        self.assertEqual(rows[0]["slot1_rx_ge_threshold"], "")
        self.assertEqual(rows[1]["pool_in_use"], "11767")
        self.assertEqual(rows[0]["p0_rx_pps"], "")
        self.assertEqual(rows[1]["p0_ipackets_delta"], "50")
        self.assertGreater(int(rows[1]["p0_rx_bps"]), 0)
        self.assertEqual(rows[2]["p0_ipackets_delta"], "")
        self.assertEqual(rows[2]["p0_rx_pps"], "")
        self.assertEqual(rows[2]["p0_oerrors"], "")
        self.assertEqual(rows[2]["slot1_rx"], "")
        self.assertEqual(rows[2]["rx_sum"], "")
        self.assertEqual(rows[2]["all_rx_ge_threshold"], "")
        self.assertEqual(rows[2]["sampled_rx_count"], "1")
        self.assertIn("Unavailable: /ring/info,MProc_Client_17_RX", warnings)

    def test_explicit_instances(self):
        rows, _ = self.capture(["2", "9", "--count", "1"], Telemetry())
        self.assertEqual(rows[0]["nf9_rx"], "1200")
        self.assertEqual(rows[0]["nf2_tx"], "0")
        self.assertEqual(rows[0]["log_ready_workers"], "")

    def test_watch_keeps_csv_clean_and_reports_affinity(self):
        rows, events = self.capture(["2", "--watch", "--threshold", "1200", "--count", "2"], Telemetry())
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[0]["rx_threshold"], "1200")
        self.assertIn("allowed_cpus=", events)
        self.assertIn("RX_HIGH nf2 instance=2 rx=1200 >= threshold=1200", events)
        self.assertEqual(events.count("RX_HIGH"), 1)
        self.assertEqual(events.count("ALL_SAMPLED_HIGH workers="), 1)
        self.assertIn("STATUS nf2(nf2): rx=1200 tx=0 peak=1200 HIGH", events)

    def test_threshold_transitions_late_worker_missing_data_and_peak(self):
        monitor = qcheck.QueueMonitor(["slot0", "slot1"], 1024, 1)

        def sample(elapsed, first, second, state="ready"):
            row = {"elapsed_s": elapsed, "time_ns": 1790402819000000000 + int(elapsed * 1e9),
                   "slot0_state": "ready", "slot0_instance": 2, "slot0_tx": 0,
                   "slot1_state": state, "slot1_instance": 9 if state == "ready" else "", "slot1_tx": 0}
            values = []
            for label, rx in (("slot0", first), ("slot1", second)):
                if row[f"{label}_state"] == "ready":
                    values.append(rx)
                    if rx is not None:
                        row[f"{label}_rx"] = rx
                        row[f"{label}_rx_ge_threshold"] = int(rx >= 1024)
            if all(v is not None for v in values):
                row["all_rx_ge_threshold"] = int(min(values) >= 1024)
            monitor.update(row)

        output = io.StringIO()
        with contextlib.redirect_stderr(output):
            sample(0, 0, None, "unseen")
            sample(.1, 1024, None, "unseen")  # Equality triggers, inactive slot excluded.
            sample(.2, 1500, None, "unseen")  # No repeated HIGH event.
            sample(.3, 1000, 1200)  # New worker high, old worker low: not all high.
            sample(1.1, 0, None)  # Missing expected worker data is not a low reading.
            sample(1.2, 0, 0)
        events = output.getvalue()
        self.assertEqual(events.count("RX_HIGH slot0"), 1)
        self.assertEqual(events.count("RX_HIGH slot1"), 1)
        self.assertEqual(events.count("ALL_SAMPLED_HIGH workers="), 1)
        self.assertIn("rx=1024 >= threshold=1024", events)
        self.assertIn("RX_LOW slot0 instance=2 rx=1000", events)
        self.assertIn("RX_UNKNOWN slot1 instance=9", events)
        self.assertNotIn("RX_LOW slot1", events)
        self.assertIn("rx=0 tx=0 peak=1500 below", events)
        self.assertIn("slot1(nf9): RX unavailable", events)
        self.assertIn("+1.100s", events)

    def test_missing_telemetry_fails_before_csv(self):
        telemetry = Telemetry()
        telemetry.supported.remove("/ring/info")
        with self.assertRaisesRegex(RuntimeError, "commands unavailable: /ring/info"):
            self.capture(["2", "--count", "1"], telemetry)

    def test_log_late_creation_partial_line_and_exit(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "upfc.log"
            log = qcheck.WorkerLog(path, 4)
            log.update()
            self.assertEqual(log.workers[0]["state"], "unseen")
            path.write_text("Spawned UPF-U slot=0 pid=123 service=14 core=3\n"
                            "UPF-U slot=0 instance=2 RE")
            log.update()
            self.assertEqual(log.workers[0]["state"], "starting")
            with path.open("a") as stream:
                stream.write("ADY\n")
            log.update()
            self.assertEqual(log.workers[0]["instance"], 2)
            with path.open("a") as stream:
                stream.write("UPF-U slot 0 exited; existing sessions are not reassigned\n")
            log.update()
            self.assertEqual(log.workers[0]["state"], "failed")

    def test_planned_stop_restart_and_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "upfc.log"
            log = qcheck.WorkerLog(path, 2)
            events = [
                ("Spawned UPF-U slot=1 pid=123 service=15", "starting", None),
                ("UPF-U slot=1 instance=8 READY", "ready", 8),
                ("UPF-U slot=1 STOPPING sessions=0", "stopping", 8),
                ("UPF-U slot=1 INACTIVE stop_ms=2.0", "inactive", None),
                ("Spawned UPF-U slot=1 pid=124 service=15", "starting", None),
                ("UPF-U slot=1 instance=9 READY", "ready", 9),
                ("UPF-U slot=1 stop failed: error=-5", "failed", 9),
                ("UPF-U slot 1 stop timed out; slot will not be reused", "failed", 9),
            ]
            for line, state, instance in events:
                with path.open("a") as stream:
                    stream.write(line + "\n")
                log.update()
                self.assertEqual(log.workers[1]["state"], state)
                self.assertEqual(log.workers[1].get("instance"), instance)
                self.assertEqual(log.workers[1]["service"], 15)

    def test_log_truncation_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "upfc.log"
            path.write_text("UPF-U slot=0 instance=2 READY\n")
            log = qcheck.WorkerLog(path, 4)
            log.update()
            path.write_text("")
            with self.assertRaisesRegex(RuntimeError, "rotated/truncated"):
                log.update()

    def test_slot_overflow_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "upfc.log"
            path.write_text("UPF-U slot=4 instance=2 READY\n")
            with self.assertRaisesRegex(RuntimeError, "increase --slots"):
                qcheck.WorkerLog(path, 4).update()

    def test_invalid_arguments(self):
        for args in ([], ["2", "2"], ["2", "--interval", "nan"],
                     ["--upfc-log", "upfc.log", "--slots", "0"],
                     ["2", "--upfc-log", "upfc.log"], ["2", "--threshold", "0"],
                     ["2", "--status-interval", "nan"]):
            with self.subTest(args=args), self.assertRaises(SystemExit):
                self.capture(args, Telemetry())


if __name__ == "__main__":
    unittest.main()
