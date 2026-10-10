#!/usr/bin/env python3
"""Shared-port software-LB CN launcher. Requires Python 3.8+, PyYAML and tmux."""
import argparse
import ipaddress
import json
import os
from pathlib import Path
import pwd
import re
import shlex
import shutil
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[2]
SOCKET = "upf-sw-lb"
NFS = ("nrf", "udr", "udm", "ausf", "nssf", "pcf", "chf", "amf", "smf")
MASK = "0xFFFF8"


def expand(value, home):
    if isinstance(value, str):
        return value.replace("${LAB_HOME}", home).replace("${UPF_REPO}", str(REPO))
    if isinstance(value, dict):
        return {key: expand(item, home) for key, item in value.items()}
    if isinstance(value, list):
        return [expand(item, home) for item in value]
    return value


def quote(argv):
    return shlex.join([str(arg) for arg in argv])


def prepare(config, directory):
    """Validate every input and build run-local configs/commands without writes."""
    import yaml

    c = config
    for key in ("core_repo", "nf_bin", "nf_config_dir", "upfc_config", "results_root"):
        if not Path(c[key]).is_absolute():
            raise ValueError(f"{key} must be an absolute path (or use ${{LAB_HOME}} / ${{UPF_REPO}})")

    def read_yaml(path):
        try:
            return yaml.safe_load(path.read_text())
        except yaml.YAMLError as exc:
            raise ValueError(f"invalid YAML in {path}: {exc}") from exc

    workers = c["workers"]
    if not 1 <= len(workers) <= 4:
        raise ValueError("configure 1–4 workers on cores 3–6")
    cores, instances = {0, 1, 2, 7}, {2}
    for w in workers:
        if (type(w["core"]) is not int or w["core"] not in range(3, 7) or
                type(w["instance"]) is not int or not 1 <= w["instance"] < 128 or
                w["core"] in cores or w["instance"] in instances):
            raise ValueError("workers need unique cores 3–6 and unique instance IDs 1–127 (excluding 2)")
        cores.add(w["core"])
        instances.add(w["instance"])
    pci = [c["n3_pci"], c["n6_pci"]]
    if len(set(pci)) != 2 or any(not re.fullmatch(r"[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]", p) for p in pci):
        raise ValueError("N3/N6 must specify two distinct PCI addresses")
    for key in ("n3_ip", "n6_ip"):
        addr = ipaddress.IPv4Address(c[key])
        if addr.is_unspecified or addr.is_multicast:
            raise ValueError(f"invalid {key}: {addr}")
    for key in ("n3_peer_mac", "n6_peer_mac"):
        mac = c.get(key)
        if mac is not None and (not re.fullmatch(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", mac) or
                                int(mac[:2], 16) & 1 or int(mac.replace(":", ""), 16) == 0):
            raise ValueError(f"{key} must be a unicast MAC or null for ARP")

    core_repo = Path(c["core_repo"])
    if not core_repo.is_dir():
        raise ValueError(f"missing core_repo: {core_repo}")
    cfg = directory / "config"
    files = {}
    upfc = read_yaml(Path(c["upfc_config"]))
    conf = upfc["configuration"]
    if "scaling" in conf:
        raise ValueError("upfc_config must be the original single-UPF YAML, without scaling")
    conf["gtpu"] = [{"addr": c["n3_ip"]}]
    conf["dataplane_ports"] = {"access": 0, "core": 1, "sgi": 1}
    conf["debugLevel"] = "info"  # Includes successful session/TEID logging.
    files["upfcfg.yaml"] = yaml.safe_dump(upfc, sort_keys=False)
    upfu = read_yaml(REPO / "docs/sw-lb-upf/upf_u.example.yaml")
    dp = upfu["configuration"]["dataplane"]
    dp.update(upf_n3_ip=c["n3_ip"], upf_n6_ip=c["n6_ip"])
    for key in ("n3_peer_mac", "n6_peer_mac"):
        if c.get(key):
            dp[key] = c[key]
    files["upf_u.yaml"] = yaml.safe_dump(upfu, sort_keys=False)
    files["map.conf"] = (f'n3 0 {c["n3_ip"]}\nn6 1 {c["n6_ip"]}\n' +
                         "".join(f'worker {w["instance"]}\n' for w in workers))
    eal = ["-n", "4", "--proc-type=secondary", "--file-prefix=rte"]
    allow = ["-a", pci[0], "-a", pci[1]]
    jobs = []

    def job(name, argv, cwd, env=None, identity=None):
        jobs.append(dict(name=name, argv=list(map(str, argv)), cwd=str(cwd),
                         env=env or {}, identity=identity))

    job("manager", [REPO / "build/onvm/onvm_mgr/onvm_mgr", "-l", "0-2", "-n", "4",
                    "--proc-type=primary", "--file-prefix=rte", "--base-virtaddr=0x7f000000000",
                    *allow, "--", "-p", "3", "-n", MASK, "-s", "stdout",
                    "--upf-lb", cfg / "map.conf"], REPO)
    job("upfc", [REPO / "build/5gc/l25gc_upf_c", "-l", "7", *eal, "--no-pci",
                 "--", "-r", "2", "-n", "2", "-m", "--", "-f", cfg / "upfcfg.yaml"],
        REPO, identity=[2, 2, 7])
    for w in workers:
        job(f'upfu-{w["instance"]}', [REPO / "build/5gc/l25gc_upf_u", "-l", w["core"],
            *eal, *allow, "--", "-r", "1", "-n", w["instance"], "-m", "--", cfg / "upf_u.yaml"],
            REPO, identity=[w["instance"], 1, w["core"]])
    env = {"CORE_REPO": str(core_repo), "UPF_REPO": str(REPO), "ONVM_NF_JSON": str(cfg / "nf") + "/"}
    for key, filename in (("ONVMPOLLER_IPID_YAML", "ipid.yaml"),
                          ("ONVMPOLLER_NFIP_YAML", "NFip.yaml"), ("ONVMPOLLER_IPID_TXT", "ipid.txt")):
        files[filename] = (core_repo / "onvm_nf_configs" / filename).read_text()
        env[key] = str(cfg / filename)
    for nf in NFS:
        data = json.loads((Path(c["nf_config_dir"]) / f"{nf}.json").read_text())
        core = c["control_cores"][nf]
        iid, sid = data["onvm"]["instanceid"], data["onvm"]["serviceid"]
        if (type(core) is not int or core not in range(3, 20) or core in cores or
                type(iid) is not int or iid in instances or not 1 <= iid < 128 or
                type(sid) is not int or not 3 <= sid < 32):
            raise ValueError(f"{nf}: conflicting/invalid core, instance or service ID in source JSON")
        cores.add(core)
        instances.add(iid)
        data["dpdk"].update(corelist=str(core), memory_channels=4, portmask=3, allowlist=pci)
        files[f"nf/{nf}.json"] = json.dumps(data, indent=2) + "\n"
        job(nf, ["taskset", "-c", core, Path(c["nf_bin"]) / nf], core_repo,
            dict(env, NF_NAME=nf), [iid, sid, core])
    for path in [Path(j["argv"][0]) for j in jobs if j["name"] not in NFS] + [Path(c["nf_bin"]) / nf for nf in NFS]:
        if not path.is_file() or not os.access(path, os.X_OK):
            raise ValueError(f"missing/non-executable binary: {path}; build the UPF targets or correct nf_bin for control NFs")
    return files, jobs


class Launcher:
    def __init__(self, config, run):
        self.config = config
        self.directory = Path(config["results_root"]) / run / "cn"
        self.session = "sw-lb-" + run

    def tmux(self, *args, check=True):
        return subprocess.run(["tmux", "-L", SOCKET, "-f", "/dev/null", *args],
                              check=check, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def exists(self):
        return self.tmux("has-session", "-t", "=" + self.session, check=False).returncode == 0

    def panes(self):
        result = self.tmux("list-panes", "-s", "-t", "=" + self.session,
                           "-F", "#{window_name}\t#{pane_id}\t#{pane_dead}")
        return {name: (pane, dead == "1") for name, pane, dead in
                (line.split("\t") for line in result.stdout.splitlines())}

    def healthy(self, names):
        panes = self.panes()
        for name in names:
            if name not in panes or panes[name][1]:
                raise RuntimeError(f"{name} exited; inspect {self.directory / (name + '.log')}")

    def launch(self, job):
        name = job["name"]
        script = self.directory / f"{name}.sh"
        gate = self.directory / f"{name}.go"
        argv = ["env", *[f"{k}={v}" for k, v in job["env"].items()], *job["argv"]]
        script.write_text("#!/bin/bash\nset -euo pipefail\nwhile [[ ! -e " + shlex.quote(str(gate)) +
                          " ]]; do sleep 0.05; done\ncd " + shlex.quote(job["cwd"]) + "\nexec " + quote(argv) + "\n")
        pane = self.tmux("new-window", "-d", "-P", "-F", "#{pane_id}", "-t", self.session + ":",
                         "-n", name, quote(["bash", script])).stdout.strip()
        self.tmux("pipe-pane", "-O", "-t", pane, "cat >> " + shlex.quote(str(self.directory / f"{name}.log")))
        gate.touch()
        print(f"Started {name}", flush=True)

    def wait(self, pattern, names, timeout=90):
        deadline = time.monotonic() + timeout
        path = self.directory / "manager.log"
        while time.monotonic() < deadline:
            self.healthy(names)
            if path.exists() and re.search(pattern, path.read_text(errors="replace")):
                return
            time.sleep(0.25)
        raise RuntimeError(f"registration/startup timeout: {names[-1]}; inspect manager.log and its NF log. "
                           "Check the actual IID/SID/core, and rebuild X-IO/control NFs if ONVM libraries differ.")

    def start(self):
        files, jobs = prepare(self.config, self.directory)
        if self.directory.exists() or self.exists():
            raise ValueError("results/session already exist; use a fresh run ID")
        # The shared EAL prefix is fixed to rte, including JSON-based control NFs.
        for process in ("onvm_mgr", "l25gc_upf_c", "l25gc_upf_u", *NFS):
            if subprocess.run(["pgrep", "-x", process], stdout=subprocess.DEVNULL).returncode == 0:
                raise ValueError(f"{process} already running; stop the previous deployment first")
        self.directory.mkdir(parents=True)
        for name, contents in files.items():
            path = self.directory / "config" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents)
        (self.directory / "resolved.json").write_text(json.dumps(self.config, indent=2) + "\n")
        (self.directory / "commands.json").write_text(json.dumps(jobs, indent=2) + "\n")
        subprocess.run(["systemctl", "start", "mongod"], check=True)
        self.tmux("new-session", "-d", "-s", self.session, "-n", "control", "bash --noprofile --norc")
        self.tmux("set-option", "-w", "-g", "remain-on-exit", "on")
        self.tmux("set-option", "-w", "-g", "automatic-rename", "off")
        names = []
        try:
            for j in jobs:
                self.launch(j)
                names.append(j["name"])
                if j["name"] == "manager":
                    self.wait("Running RX thread for RX queue", names)
                else:
                    iid, sid, core = j["identity"]
                    self.wait(rf"\b{iid}\s*/\s*{sid}\s*/\s*{core}\s+", names)
                if j["name"] in NFS:
                    time.sleep(2)
                    self.healthy(names)
        except (Exception, KeyboardInterrupt):
            print("Startup failed; stopping this run's processes. Logs and panes are retained.", file=sys.stderr)
            self.stop()
            raise
        print(f"All NFs registered on expected cores. Logs: {self.directory}\n"
              "Now start gNB/UEs; verify PDU establishment, then warm each UE with traffic.")

    def stop(self):
        if not self.exists():
            print("No tmux session for this run.")
            return
        panes = self.panes()
        order = [*reversed(NFS), "upfc", *sorted(n for n in panes if n.startswith("upfu-")), "manager"]
        for name in order:
            if name not in panes or panes[name][1]:
                continue
            self.tmux("send-keys", "-t", panes[name][0], "C-c")
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline and not self.panes()[name][1]:
                time.sleep(0.2)
            if not self.panes()[name][1]:
                raise RuntimeError(f"{name} did not stop; inspect its pane. Remaining processes left running.")
            print(f"Stopped {name}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config", type=Path)
    parser.add_argument("run", help="fresh run ID, e.g. sw01")
    parser.add_argument("action", choices=("check", "start", "status", "attach", "stop"))
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}", args.run):
        parser.error("invalid run ID")
    if args.action != "check" and os.geteuid() != 0:
        parser.error("run via sudo -E bash scripts/sw-lb/cn.sh CONFIG RUN ACTION")
    try:
        home = pwd.getpwnam(os.environ.get("SUDO_USER") or pwd.getpwuid(os.getuid()).pw_name).pw_dir
        config = expand(json.loads(args.config.read_text()), home)
        lab = Launcher(config, args.run)
        if args.action in ("check", "start"):
            for command in ("tmux", "taskset", "systemctl", "pgrep", "bash"):
                if not shutil.which(command):
                    raise ValueError(f"missing command: {command}")
        if args.action == "check":
            _, jobs = prepare(config, lab.directory)
            for j in jobs:
                print(j["name"] + ": " + quote(j["argv"]))
            print(f"Input checks passed; no processes started. Logs will be in {lab.directory}")
        elif args.action == "attach":
            os.execvp("tmux", ["tmux", "-L", SOCKET, "attach", "-t", "=" + lab.session])
        elif args.action == "status":
            if not lab.exists():
                raise ValueError("no tmux session for this run")
            for name, (_, dead) in lab.panes().items():
                print(f"{name:12} {'exited' if dead else 'running'}")
            print(f"Logs: {lab.directory}")
        else:
            getattr(lab, args.action)()
    except (OSError, ValueError, KeyError, TypeError, RuntimeError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"ERROR: {exc}\n")
    except ImportError:
        parser.exit(1, "ERROR: install PyYAML: sudo apt-get install python3-yaml\n")
    except KeyboardInterrupt:
        parser.exit(130, "Interrupted.\n")


if __name__ == "__main__":
    main()
