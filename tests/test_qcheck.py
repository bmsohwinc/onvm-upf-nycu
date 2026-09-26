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
        self.assertEqual(rows[1]["pool_in_use"], "11767")
        self.assertEqual(rows[0]["p0_rx_pps"], "")
        self.assertEqual(rows[1]["p0_ipackets_delta"], "50")
        self.assertGreater(int(rows[1]["p0_rx_bps"]), 0)
        self.assertEqual(rows[2]["p0_ipackets_delta"], "")
        self.assertEqual(rows[2]["p0_rx_pps"], "")
        self.assertEqual(rows[2]["p0_oerrors"], "")
        self.assertEqual(rows[2]["slot1_rx"], "")
        self.assertEqual(rows[2]["rx_sum"], "")
        self.assertEqual(rows[2]["sampled_rx_count"], "1")
        self.assertIn("Unavailable: /ring/info,MProc_Client_17_RX", warnings)

    def test_explicit_instances(self):
        rows, _ = self.capture(["2", "9", "--count", "1"], Telemetry())
        self.assertEqual(rows[0]["nf9_rx"], "1200")
        self.assertEqual(rows[0]["nf2_tx"], "0")
        self.assertEqual(rows[0]["log_ready_workers"], "")

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
                     ["2", "--upfc-log", "upfc.log"]):
            with self.subTest(args=args), self.assertRaises(SystemExit):
                self.capture(args, Telemetry())


if __name__ == "__main__":
    unittest.main()
