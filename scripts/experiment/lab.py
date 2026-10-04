#!/usr/bin/env python3
"""Local tmux experiment launchers. No SSH, hardware setup, or global process kills."""
import argparse
import csv
import json
import os
from pathlib import Path
import pwd
import re
import shlex
import shutil
import subprocess
import time

SOCKET = "upf-experiment"


def expand(value, home):
    if isinstance(value, str):
        return value.replace("${LAB_HOME}", home)
    if isinstance(value, list):
        return [expand(item, home) for item in value]
    if isinstance(value, dict):
        return {key: expand(item, home) for key, item in value.items()}
    return value


def command(argv):
    return shlex.join([str(arg) for arg in argv])


class Lab:
    def __init__(self, args):
        self.node, self.run = args.node, args.run
        home = pwd.getpwnam(os.environ.get("SUDO_USER") or pwd.getpwuid(os.getuid()).pw_name).pw_dir
        self.config = expand(json.loads(args.config.read_text()), home)
        self.c = self.config[self.node]
        self.directory = Path(self.config["results_root"]) / self.run / self.node
        self.session = f"exp-{self.node}-{self.run}"

    def tmux(self, *argv, check=True):
        return subprocess.run(["tmux", "-L", SOCKET, "-f", "/dev/null", *argv],
                              check=check, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def exists(self):
        return self.tmux("has-session", "-t", "=" + self.session, check=False).returncode == 0

    def create(self):
        if self.exists() or self.directory.exists():
            raise ValueError("session/results already exist; use a fresh run ID")
        self.directory.mkdir(parents=True)
        (self.directory / "config.json").write_text(json.dumps(self.config, indent=2) + "\n")
        self.tmux("new-session", "-d", "-s", self.session, "-n", "control", "bash --noprofile --norc")
        self.tmux("set-option", "-w", "-g", "remain-on-exit", "on")
        self.tmux("set-option", "-w", "-g", "automatic-rename", "off")
        print(f"Created {self.session}; logs: {self.directory}", flush=True)

    def panes(self):
        result = self.tmux("list-panes", "-s", "-t", "=" + self.session,
                           "-F", "#{window_name}\t#{pane_id}\t#{pane_dead}")
        return {name: (pane, dead == "1") for name, pane, dead in
                (line.split("\t") for line in result.stdout.splitlines())}

    def alive(self, name):
        return name in self.panes() and not self.panes()[name][1]

    def start_job(self, name, argv, cwd, env=None, stdout=None):
        if name in self.panes():
            raise ValueError(f"window {name} already exists; inspect it or use a fresh run")
        if not Path(cwd).is_dir():
            raise ValueError(f"missing working directory: {cwd}")
        if env:
            argv = ["env", *[f"{key}={value}" for key, value in env.items()], *argv]
        script = self.directory / f"{name}.sh"
        gate = self.directory / f"{name}.go"
        body = ("#!/bin/bash\nset -euo pipefail\nwhile [[ ! -e " + shlex.quote(str(gate)) +
                " ]]; do sleep 0.05; done\ncd " + shlex.quote(str(cwd)) + "\nexec " + command(argv))
        if stdout:
            body += " > " + shlex.quote(str(self.directory / stdout))
        script.write_text(body + "\n")
        pane = self.tmux("new-window", "-d", "-P", "-F", "#{pane_id}", "-t", self.session + ":",
                         "-n", name, command(["bash", script])).stdout.strip()
        # Attach capture before starting the real process, preserving its terminal/stdin.
        self.tmux("pipe-pane", "-O", "-t", pane,
                  "cat >> " + shlex.quote(str(self.directory / f"{name}.log")))
        gate.touch()
        self.event("launch:" + name)

    def wait_pattern(self, name, pattern, timeout=60, offset=0):
        deadline = time.monotonic() + timeout
        path = self.directory / f"{name}.log"
        while time.monotonic() < deadline:
            text = path.read_text(errors="replace")[offset:] if path.exists() else ""
            if re.search(pattern, text):
                return
            if not self.alive(name):
                raise RuntimeError(f"{name} exited; inspect {path}")
            time.sleep(0.2)
        raise RuntimeError(f"timeout waiting for {name}: {pattern}; inspect {path}")

    def send(self, name, text):
        if not self.alive(name):
            raise RuntimeError(f"{name} is not running")
        pane = self.panes()[name][0]
        self.tmux("send-keys", "-t", pane, "-l", text)
        self.tmux("send-keys", "-t", pane, "Enter")

    def interrupt(self, name):
        if not self.alive(name):
            return
        self.tmux("send-keys", "-t", self.panes()[name][0], "C-c")
        deadline = time.monotonic() + 30
        while self.alive(name) and time.monotonic() < deadline:
            time.sleep(0.2)
        if self.alive(name):
            raise RuntimeError(f"{name} did not exit; inspect its pane (no forced kill performed)")
        self.event("stopped:" + name)

    def event(self, label, filename="events.csv"):
        path = self.directory / filename
        fresh = not path.exists()
        with path.open("a", newline="") as stream:
            writer = csv.writer(stream)
            if fresh:
                writer.writerow(["time_ns", "phase"])
            writer.writerow([time.time_ns(), label])
        print(label, flush=True)

    def start_cn(self):
        if subprocess.run(["pgrep", "-x", "onvm_mgr"], stdout=subprocess.DEVNULL).returncode == 0:
            raise RuntimeError("manager already running; stop the old deployment before start")
        p, c = self.config, self.c
        # Validate paths/configs before starting a partially configured deployment.
        for path in [c["upfc_config"], f'{p["upf_repo"]}/build/5gc/l25gc_upf_c',
                     f'{p["core_repo"]}/scripts/run/run_onvm_mgr.sh']:
            if not Path(path).is_file():
                raise ValueError(f"missing file: {path}")
        cores = {}
        for nf in c["control_nfs"]:
            cores[nf] = str(json.loads((Path(c["nf_config_dir"]) / f"{nf}.json").read_text())["dpdk"]["corelist"])
            if not (Path(p["core_repo"]) / "bin-sriov" / nf).is_file():
                raise ValueError(f"missing control NF binary: {nf}")
        self.create()
        subprocess.run(["systemctl", "start", "mongod"], check=True)
        self.start_job("manager", ["bash", f'{p["core_repo"]}/scripts/run/run_onvm_mgr.sh',
            "-p", p["upf_repo"], "-a", " ".join(c["manager_allow"]),
            "-k", c["manager_portmask"], "-n", c["manager_nf_coremask"]], p["core_repo"])
        self.wait_pattern("manager", "Running RX thread for RX queue")
        env = {"ONVM_NF_TIMING_DIR": str(self.directory) if c["timing"] else ""}
        self.start_job("upfc", [f'{p["upf_repo"]}/build/5gc/l25gc_upf_c', *c["upfc_eal"],
            "--", "-r", "2", "-m", "--", "-f", c["upfc_config"]], p["upf_repo"], env)
        self.wait_pattern("upfc", r"UPF-U slot=0 instance=\d+ READY", timeout=90)
        env = {key.upper(): p[key] for key in ("core_repo", "upf_repo", "smf_repo", "xio_repo")}
        env["ONVM_NF_JSON"] = c["nf_config_dir"] + "/"
        for key, filename in [("ONVMPOLLER_IPID_YAML", "ipid.yaml"),
                              ("ONVMPOLLER_NFIP_YAML", "NFip.yaml"), ("ONVMPOLLER_IPID_TXT", "ipid.txt")]:
            env[key] = f'{p["core_repo"]}/onvm_nf_configs/{filename}'
        for nf in c["control_nfs"]:
            self.start_job(nf, ["taskset", "-c", cores[nf], f'{p["core_repo"]}/bin-sriov/{nf}'],
                           p["core_repo"], dict(env, NF_NAME=nf))
            time.sleep(c["control_start_gap_s"])
            if not self.alive(nf):
                raise RuntimeError(f"{nf} exited; inspect its log")
        self.start_job("qcheck", ["taskset", "-c", c["monitor_core"], "python3",
            f'{p["upf_repo"]}/scripts/qcheck.py', "--upfc-log", str(self.directory / "upfc.log"),
            "--slots", str(c["slots"]), "--interval", str(c["queue_interval_s"]), "--watch",
            "--threshold", str(c["queue_threshold"])], p["upf_repo"], stdout="queues.csv")

    def start_ue(self):
        self.create()
        self.start_job("gnb", ["./build/nr-gnb", "-c", self.c["gnb_config"]], self.config["ueransim_repo"])
        self.wait_pattern("gnb", self.c["gnb_ready"])

    def connect(self, index):
        name = f"ue{index + 1}"
        self.start_job(name, ["./build/nr-ue", "-c", self.c["streams"][index]["config"]], self.config["ueransim_repo"])
        self.wait_pattern(name, self.c["ue_ready"], timeout=120)

    def generator(self, index, name, duration):
        c, s = self.c, self.c["streams"][index]
        if not self.alive(f"ue{index + 1}"):
            raise ValueError(f"connect ue{index + 1} before starting its generator")
        if any(self.alive(n) for n in self.panes() if n != name and
               n in (("setup1", "traffic1") if index == 0 else ("traffic2",))):
            raise ValueError("a generator already owns this UE's VF; stop it first")
        args = ["./build/examples/dpdk-gtpgen", "-l", s["core"], "-a", s["pci"],
                "--file-prefix=gtp-ue" + str(index + 1), "--"]
        for flag, key in [("src-mac", "src_mac"), ("dst-mac", "dst_mac"), ("n3-dst-ip", "n3_dst_ip"),
                          ("teid", "teid"), ("ue-ip", "ue_ip"), ("rate", "rate")]:
            args += ["--" + flag, str(s[key])]
        args += ["--n3-src-ip", c["n3_src_ip"], "--dn-ip", c["dn_ip"], "--qfi", str(c["qfi"]),
                 "--size", str(c["inner_size"]), "--duration", str(duration)]
        self.start_job(name, args, self.config["dpdk_repo"])
        self.wait_pattern(name, r"gtpgen: port=.*rate=")

    def hold(self, seconds, names):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            for name in names:
                if not self.alive(name):
                    raise RuntimeError(f"{name} exited during measurement")
            time.sleep(min(0.5, max(0, deadline - time.monotonic())))

    def measure(self):
        if (self.directory / "phases.csv").exists():
            raise ValueError("this run already has measurement markers; use a fresh run ID")
        for name in ("gnb", "ue1", "ue2"):
            if not self.alive(name):
                raise ValueError(f"{name} must be running before measurement")
        if any(self.alive(name) for name in self.panes() if name.startswith(("setup", "traffic"))):
            raise ValueError("stop setup traffic and drain queues first")
        phase, settle = self.c["phase_s"], self.c["settle_s"]
        if phase <= 0 or settle < 0:
            raise ValueError("phase_s must be positive; settle_s must be nonnegative")
        duration = int(3 * phase + 3 * settle + 180)
        try:
            self.generator(0, "traffic1", duration)
            self.hold(settle, ["traffic1"])
            self.event("UE1_ONLY", "phases.csv")
            self.hold(phase, ["traffic1"])
            self.event("TRANSITION_1", "phases.csv")
            self.generator(1, "traffic2", duration)
            self.hold(settle, ["traffic1", "traffic2"])
            self.event("BOTH", "phases.csv")
            self.hold(phase, ["traffic1", "traffic2"])
            self.event("TRANSITION_2", "phases.csv")
            self.interrupt("traffic2")
            self.hold(settle, ["traffic1"])
            self.event("UE1_AGAIN", "phases.csv")
            self.hold(phase, ["traffic1"])
            self.event("END", "phases.csv")
        except BaseException:
            self.event("ABORTED", "phases.csv")
            raise
        finally:
            errors = []
            for name in ("traffic2", "traffic1"):
                try:
                    self.interrupt(name)
                except (RuntimeError, subprocess.CalledProcessError) as error:
                    errors.append(str(error))
            if errors:
                self.event("ABORTED", "phases.csv")
                raise RuntimeError("generator cleanup failed: " + "; ".join(errors))

    def sink_command(self, text):
        path = self.directory / "sink.log"
        offset = len(path.read_text(errors="replace")) if path.exists() else 0
        self.send("sink", text)
        # Require a new prompt after a newline, not a readline redraw while typing.
        self.wait_pattern("sink", r"\r?\ntestpmd>", offset=offset)
        reply = path.read_text(errors="replace")[offset:]
        if re.search(r"Bad arguments|Command not found|Invalid command", reply, re.I):
            raise RuntimeError(f"testpmd rejected {text!r}; inspect {path}")
        self.event("sink:" + text)

    def start_dn(self):
        self.create()
        self.start_job("sink", ["./build/app/dpdk-testpmd", *self.c["eal"], "--", *self.c["args"]], self.config["dpdk_repo"])
        self.wait_pattern("sink", "testpmd>")
        self.sink_command("start")

    def stop(self):
        if self.node == "dn" and self.alive("sink"):
            self.sink_command("stop")
            self.sink_command("show port stats all")
            self.send("sink", "quit")
            return
        names = list(self.panes())
        # Stop generators before UEs; stop UPF-C/workers before the manager.
        names.sort(key=lambda name: (name == "manager", name != "sequence",
                                     not name.startswith(("setup", "traffic")), name != "upfc"))
        for name in names:
            if name != "control":
                self.interrupt(name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("node", choices=["cn", "ue", "dn"])
    parser.add_argument("config", type=Path)
    parser.add_argument("run", help="shared run ID, e.g. run01")
    parser.add_argument("action", choices=["start", "attach", "status", "stop", "ue1", "ue2", "load1",
                                           "stop-traffic", "measure", "_measure", "begin", "finish", "report"])
    parser.add_argument("--phases", type=Path, help="CN report: UE phases.csv copied from this run")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_-]+", args.run):
        parser.error("run ID must contain only letters, digits, underscores or hyphens")
    if os.geteuid() != 0:
        parser.error("run via sudo -E bash <node>.sh ... (one root tmux server, no per-pane sudo prompts)")
    try:
        lab = Lab(args)
        if args.action == "start":
            getattr(lab, "start_" + args.node)()
        else:
            if not lab.exists():
                raise ValueError("no session for this node/run; start it first")
            if args.action == "attach":
                os.execvp("tmux", ["tmux", "-L", SOCKET, "attach", "-t", "=" + lab.session])
            elif args.action == "status":
                print(lab.tmux("list-panes", "-s", "-t", "=" + lab.session, "-F",
                               "#{window_name}: dead=#{pane_dead} exit=#{pane_dead_status} pid=#{pane_pid}").stdout)
            elif args.action == "stop":
                lab.stop()
            elif args.node == "ue" and args.action in ("ue1", "ue2"):
                lab.connect(int(args.action[-1]) - 1)
            elif args.node == "ue" and args.action == "load1":
                lab.generator(0, "setup1", lab.c["setup_duration_s"])
            elif args.node == "ue" and args.action == "stop-traffic":
                lab.interrupt("sequence")
                for name in lab.panes():
                    if name.startswith(("setup", "traffic")):
                        lab.interrupt(name)
            elif args.node == "ue" and args.action == "measure":
                lab.start_job("sequence", ["python3", str(Path(__file__).resolve()), "ue",
                              str(args.config.resolve()), args.run, "_measure"], lab.directory)
            elif args.node == "ue" and args.action == "_measure":
                lab.measure()
            elif args.node == "dn" and args.action in ("begin", "finish"):
                lab.sink_command("stop")
                lab.sink_command("show port stats all")
                if args.action == "begin":
                    lab.sink_command("clear port stats all")
                    lab.sink_command("start")
            elif args.node == "cn" and args.action == "report":
                source = args.phases or lab.directory / "phases.csv"
                with source.open() as stream:
                    phases = list(csv.DictReader(stream))
                if not phases or phases[-1]["phase"] != "END" or any(p["phase"] == "ABORTED" for p in phases):
                    raise ValueError("measurement did not finish normally; inspect events and raw data")
                if args.phases:
                    destination = lab.directory / "phases.csv"
                    if destination.exists():
                        raise ValueError("CN phases.csv already exists; inspect it before replacing")
                    shutil.copyfile(args.phases, destination)
                subprocess.run(["python3", str(Path(lab.config["upf_repo"]) / "scripts/nf_timing.py"),
                                "report", str(lab.directory)], check=True)
            else:
                raise ValueError(f"{args.action} is not an action for {args.node}")
        print(f"Results: {lab.directory}")
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.CalledProcessError) as error:
        detail = error.stderr if isinstance(error, subprocess.CalledProcessError) else ""
        parser.exit(1, f"experiment: {error}\n{detail or ''}")


if __name__ == "__main__":
    main()
