#!/usr/bin/env python3
"""Validate shared-port launch configuration and run-scoped process handling."""
import copy
import csv
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import yaml

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("sw_lb_cn", ROOT / "scripts/sw-lb/cn.py")
cn = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cn)


class LauncherTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="sw-lb test ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        repo = self.root / "upf repo"
        for rel in ("docs/sw-lb-upf/upf_u.example.yaml", "5gc/upf_c/config/upfcfg.yaml", "scripts/qcheck.py"):
            dest = repo / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / rel, dest)
        self.repo_patch = patch.object(cn, "REPO", repo)
        self.repo_patch.start()
        self.addCleanup(self.repo_patch.stop)
        source = json.loads((ROOT / "scripts/sw-lb/config.example.json").read_text())
        self.config = cn.expand(source, str(self.root))
        self.config["results_root"] = str(self.root / "results")
        core = Path(self.config["core_repo"])
        base = core / "onvm_nf_configs"
        base.mkdir(parents=True)
        self.original = {}
        for i, nf in enumerate(cn.NFS, 3):
            # Distinct source IDs; stale eight-VF/core config must be replaced.
            data = {"dpdk": {"corelist": "99", "allowlist": ["old-vf"], "memory_channels": 1},
                    "onvm": {"serviceid": i, "instanceid": i, "output": "stdout"}}
            path = base / f"{nf}.json"
            path.write_text(json.dumps(data))
            self.original[path] = path.read_bytes()
        for name in ("ipid.yaml", "NFip.yaml", "ipid.txt"):
            (base / name).write_text("preserved mapping\n")
        paths = [repo / "build/onvm/onvm_mgr/onvm_mgr", repo / "build/5gc/l25gc_upf_c",
                 repo / "build/5gc/l25gc_upf_u", *[Path(self.config["nf_bin"]) / n for n in cn.NFS]]
        for path in paths:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("#!/bin/sh\nexit 0\n")
            path.chmod(0o755)
        self.directory = self.root / "results/sw01/cn"

    def test_consistent_shared_pair_and_preserved_control_identity(self):
        files, jobs = cn.prepare(self.config, self.directory)
        self.assertFalse(self.directory.exists())  # check is read-only
        self.assertEqual(len(jobs), 16)
        self.assertEqual(files["map.conf"], "n3 0 10.10.2.11\nn6 1 10.10.3.11\n"
                         "worker 14\nworker 15\nworker 16\nworker 17\n")
        mgr = jobs[0]["argv"]
        self.assertEqual(mgr[mgr.index("-p") + 1], "3")
        self.assertEqual(mgr.count("-a"), 2)
        self.assertNotIn("start.sh", " ".join(mgr))
        for job in jobs[2:6]:
            self.assertEqual(job["argv"].count("-a"), 2)
            self.assertIn("-m", job["argv"])
            self.assertIn("--file-prefix=rte", job["argv"])
            self.assertEqual(job["argv"][-1], str(self.directory / "config/upf_u.yaml"))
        upfc = yaml.safe_load(files["upfcfg.yaml"])["configuration"]
        self.assertEqual(upfc["gtpu"], [{"addr": "10.10.2.11"}])
        self.assertEqual(upfc["pfcp"], [{"addr": "127.0.0.8"}])
        self.assertEqual(upfc["dnn_list"][0]["cidr"], "10.60.0.0/24")
        upfu = yaml.safe_load(files["upf_u.yaml"])["configuration"]
        self.assertFalse(upfu["nat"]["enable"])
        self.assertEqual(upfu["dataplane"]["n6_peer_mac"], "90:e2:ba:b3:ba:99")
        for job in jobs[6:-1]:
            nf = job["name"]
            data = json.loads(files[f"nf/{nf}.json"])
            self.assertEqual(data["onvm"], json.loads(self.original[Path(self.config["nf_config_dir"]) / f"{nf}.json"])["onvm"])
            self.assertEqual(data["dpdk"]["allowlist"], ["0000:06:10.0", "0000:06:10.1"])
            self.assertEqual(data["dpdk"]["corelist"], str(self.config["control_cores"][nf]))
            self.assertEqual(job["env"]["ONVM_NF_JSON"], str(self.directory / "config/nf") + "/")
            self.assertEqual(job["cwd"], self.config["core_repo"])
        monitor = jobs[-1]
        self.assertEqual(monitor["name"], "qcheck")
        self.assertEqual(monitor["argv"], ["taskset", "-c", "19", sys.executable,
                         str(cn.REPO / "scripts/qcheck.py"), "14", "15", "16", "17",
                         "--ports", "0", "1", "--interval", "0.2", "--watch", "--threshold", "40"])
        self.assertEqual(monitor["stdout"], "queues.csv")
        for path, original in self.original.items():
            self.assertEqual(path.read_bytes(), original)

    def test_one_worker_and_dynamic_arp(self):
        self.config["workers"] = self.config["workers"][:1]
        self.config["n3_peer_mac"] = self.config["n6_peer_mac"] = None
        files, jobs = cn.prepare(self.config, self.directory)
        self.assertEqual(len(jobs), 13)
        self.assertEqual(files["map.conf"].count("worker"), 1)
        dp = yaml.safe_load(files["upf_u.yaml"])["configuration"]["dataplane"]
        self.assertNotIn("n3_peer_mac", dp)
        self.assertNotIn("n6_peer_mac", dp)
        args = jobs[-1]["argv"]
        self.assertEqual(args[5:args.index("--ports")], ["14"])

    def test_monitor_defaults_for_existing_configs_and_custom_settings(self):
        for key in cn.QCHECK_DEFAULTS:
            self.config.pop(key)
        lab = cn.Launcher(self.config, "sw01")
        _, jobs = cn.prepare(self.config, self.directory)
        self.assertEqual(lab.config["monitor_core"], 19)
        self.assertEqual(jobs[-1]["argv"][2], "19")
        self.config.update(monitor_core=18, queue_interval_s=0.05, queue_threshold=80)
        self.config["workers"][0]["instance"] = 30
        _, jobs = cn.prepare(self.config, self.directory)
        args = jobs[-1]["argv"]
        self.assertEqual(args[2], "18")
        self.assertEqual(args[5:9], ["30", "15", "16", "17"])
        self.assertEqual(args[args.index("--interval") + 1], "0.05")
        self.assertEqual(args[args.index("--threshold") + 1], "80")

    def test_invalid_or_conflicting_placement_fails_before_start(self):
        variants = []
        for key, value in (("nf_bin", str(self.root / "bin-does-not-exist")),
                           ("n6_pci", self.config["n3_pci"]),
                           ("n3_peer_mac", "ff:ff:ff:ff:ff:ff"),
                           ("results_root", "relative"),
                           ("monitor_core", 0), ("monitor_core", 3), ("monitor_core", 8),
                           ("monitor_core", -1), ("monitor_core", True),
                           ("queue_interval_s", 0), ("queue_interval_s", float("nan")),
                           ("queue_interval_s", float("inf")), ("queue_interval_s", True),
                           ("queue_threshold", 0), ("queue_threshold", 1.5)):
            config = copy.deepcopy(self.config)
            config[key] = value
            variants.append(config)
        config = copy.deepcopy(self.config)
        config["workers"][1]["instance"] = 14
        variants.append(config)
        config = copy.deepcopy(self.config)
        config["control_cores"]["smf"] = 3
        variants.append(config)
        for config in variants:
            with self.subTest(config=config), self.assertRaises(ValueError):
                cn.prepare(config, self.directory)
        self.assertFalse(self.directory.exists())

    def test_source_worker_id_collision_and_scaling_config_rejected(self):
        path = Path(self.config["nf_config_dir"]) / "smf.json"
        data = json.loads(path.read_text())
        data["onvm"]["instanceid"] = 14
        path.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, "smf"):
            cn.prepare(self.config, self.directory)
        path.write_bytes(self.original[path])
        upfc = Path(self.config["upfc_config"])
        data = yaml.safe_load(upfc.read_text())
        data["configuration"]["scaling"] = {"enabled": True}
        upfc.write_text(yaml.safe_dump(data))
        with self.assertRaisesRegex(ValueError, "without scaling"):
            cn.prepare(self.config, self.directory)

    def test_old_process_refused_before_creating_results(self):
        lab = cn.Launcher(self.config, "sw01")
        with patch.object(lab, "exists", return_value=False), patch.object(cn.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)):
            with self.assertRaisesRegex(ValueError, "already running"):
                lab.start()
        self.assertFalse(self.directory.exists())

    def test_start_failure_stops_run_and_preserves_evidence(self):
        lab = cn.Launcher(self.config, "sw01")
        with patch.object(lab, "exists", return_value=False), patch.object(lab, "tmux"), \
                patch.object(lab, "launch") as launch, patch.object(lab, "wait", side_effect=RuntimeError("manager failed")), \
                patch.object(lab, "stop") as stop, patch.object(cn.subprocess, "run", return_value=subprocess.CompletedProcess([], 1)):
            with self.assertRaisesRegex(RuntimeError, "manager failed"):
                lab.start()
        self.assertEqual(launch.call_args.args[0]["name"], "manager")
        stop.assert_called_once()
        self.assertTrue((self.directory / "commands.json").is_file())
        self.assertTrue((self.directory / "config/nf/smf.json").is_file())

    def test_qcheck_starts_last_and_startup_failure_stops_run(self):
        lab = cn.Launcher(self.config, "sw01")
        with patch.object(lab, "exists", return_value=False), patch.object(lab, "tmux"), \
                patch.object(lab, "launch") as launch, patch.object(lab, "wait") as wait, \
                patch.object(lab, "healthy"), patch.object(cn.time, "sleep"), \
                patch.object(lab, "wait_queues", side_effect=RuntimeError("no queue data")) as queues, \
                patch.object(lab, "stop") as stop, \
                patch.object(cn.subprocess, "run", return_value=subprocess.CompletedProcess([], 1)):
            with self.assertRaisesRegex(RuntimeError, "no queue data"):
                lab.start()
        self.assertEqual(launch.call_args_list[-1].args[0]["name"], "qcheck")
        self.assertEqual(wait.call_count, 15)  # qcheck is not an NF registration.
        self.assertEqual(queues.call_args.args[0][-1], "qcheck")
        stop.assert_called_once()

    def test_queue_readiness_requires_all_workers_rx_and_tx(self):
        self.directory.mkdir(parents=True)
        path = self.directory / "queues.csv"
        fields = [f"nf{i}_{d}" for i in range(14, 18) for d in ("rx", "tx")]
        with path.open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=fields)
            writer.writeheader()
            writer.writerow({field: 0 for field in fields if field != "nf17_tx"})
        lab = cn.Launcher(self.config, "sw01")
        with patch.object(lab, "healthy"), patch.object(cn.time, "sleep"), \
                patch.object(cn.time, "monotonic", side_effect=[0, 0, 11]):
            with self.assertRaisesRegex(RuntimeError, "missing worker RX/TX"):
                lab.wait_queues(["qcheck"])
        with path.open("a", newline="") as stream:
            csv.DictWriter(stream, fieldnames=fields).writerow(dict.fromkeys(fields, 0))
        with patch.object(lab, "healthy") as healthy:
            lab.wait_queues(["manager", "qcheck"])
        healthy.assert_called_once_with(["manager", "qcheck"])
        with patch.object(lab, "healthy", side_effect=RuntimeError("qcheck exited")):
            with self.assertRaisesRegex(RuntimeError, "qcheck exited"):
                lab.wait_queues(["qcheck"])

    def test_stop_targets_only_own_panes_manager_last(self):
        lab = cn.Launcher(self.config, "sw01")
        panes = {name: (f"%{i}", False) for i, name in enumerate(("manager", "upfc", "upfu-14", "smf", "control", "qcheck"))}
        stopped = []

        def tmux(*args):
            self.assertEqual(args[0], "send-keys")
            self.assertEqual(args[-1], "C-c")
            name = next(n for n, (p, _) in panes.items() if p == args[2])
            stopped.append(name)
            panes[name] = (args[2], True)

        with patch.object(lab, "exists", return_value=True), patch.object(lab, "panes", side_effect=lambda: dict(panes)), patch.object(lab, "tmux", side_effect=tmux):
            lab.stop()
        self.assertEqual(stopped, ["qcheck", "smf", "upfc", "upfu-14", "manager"])
        self.assertFalse(panes["control"][1])

    def test_launch_script_quotes_paths_and_installs_capture_before_release(self):
        self.directory.mkdir(parents=True)
        lab = cn.Launcher(self.config, "sw01")
        job = {"name": "smf", "argv": ["/tmp/a path/$(unsafe)", "a'b"],
               "cwd": str(self.root), "env": {"NF_NAME": "smf"}}
        calls = []

        def tmux(*args):
            self.assertFalse((self.directory / "smf.go").exists())
            calls.append(args[0])
            return subprocess.CompletedProcess([], 0, stdout="%42\n")

        with patch.object(lab, "tmux", side_effect=tmux):
            lab.launch(job)
        self.assertEqual(calls, ["new-window", "pipe-pane"])
        script = self.directory / "smf.sh"
        subprocess.run(["bash", "-n", str(script)], check=True)
        self.assertIn("'/tmp/a path/$(unsafe)'", script.read_text())
        self.assertTrue((self.directory / "smf.go").exists())

    def test_queue_csv_is_separate_from_status_and_paths_are_quoted(self):
        self.directory.mkdir(parents=True)
        lab = cn.Launcher(self.config, "sw01")
        output = "queue $(unsafe) 'samples.csv"
        job = {"name": "qcheck", "argv": [sys.executable, "-c",
               "import sys; print('time_ns,nf14_rx'); print('status', file=sys.stderr)"],
               "cwd": str(self.root), "env": {}, "stdout": output}
        with patch.object(lab, "tmux", return_value=subprocess.CompletedProcess([], 0, stdout="%42\n")):
            lab.launch(job)
        result = subprocess.run(["bash", str(self.directory / "qcheck.sh")],
                                check=True, text=True, capture_output=True)
        self.assertEqual(result.stdout, "")
        self.assertEqual(result.stderr, "status\n")
        self.assertEqual((self.directory / output).read_text(), "time_ns,nf14_rx\n")


if __name__ == "__main__":
    unittest.main()
