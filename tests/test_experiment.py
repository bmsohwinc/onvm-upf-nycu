"""Launcher argument/sequence tests; never start tmux, sudo, NFs or NICs."""
import importlib.util
import json
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("experiment_lab", ROOT / "scripts/experiment/lab.py")
lab_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lab_module)


class ExperimentTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="experiment-test-")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.config = json.loads((ROOT / "scripts/experiment/config.example.json").read_text())
        self.config["results_root"] = str(self.directory)
        self.path = self.directory / "config.json"
        self.path.write_text(json.dumps(self.config))
        self.lab = lab_module.Lab(SimpleNamespace(node="ue", run="run01", config=self.path))
        self.lab.directory.mkdir(parents=True)

    def test_generator_matches_user_parameters(self):
        lab = self.lab
        jobs = []
        lab.panes = lambda: {"ue1": ("%1", False), "ue2": ("%2", False)}
        lab.start_job = lambda *args: jobs.append(args)
        lab.wait_pattern = lambda *args: None
        lab.generator(0, "setup1", 120)
        lab.generator(1, "traffic2", 60)
        first, second = jobs[0][1], jobs[1][1]
        for args, pci, teid, rate, duration in [(first, "0000:06:10.0", "0x1001", "1000000", "120"),
                                               (second, "0000:06:10.2", "0x1002", "985000", "60")]:
            for flag, expected in [("-a", pci), ("--teid", teid), ("--rate", rate),
                                   ("--duration", duration), ("--size", "64")]:
                self.assertEqual(args[args.index(flag) + 1], expected)
        lab.panes = lambda: {"ue1": ("%1", False), "setup1": ("%2", False)}
        with self.assertRaisesRegex(ValueError, "already owns"):
            lab.generator(0, "traffic1", 120)

    def sequence(self, fail=False):
        lab = self.lab
        live = {"ue1", "ue2", "gnb"}
        events, generators, stopped = [], [], []
        lab.panes = lambda: {name: ("%1", False) for name in live}
        def generate(index, name, duration):
            live.add(name)
            generators.append((index, name, duration))
        def interrupt(name):
            live.discard(name)
            stopped.append(name)
        def hold(seconds, names):
            self.assertTrue(set(names) <= live)
            if fail and events and events[-1] == "BOTH":
                raise RuntimeError("generator failed")
        lab.generator, lab.interrupt, lab.hold = generate, interrupt, hold
        lab.event = lambda label, filename="events.csv": events.append(label)
        if fail:
            with self.assertRaisesRegex(RuntimeError, "generator failed"):
                lab.measure()
            self.assertEqual(events[-1], "ABORTED")
        else:
            lab.measure()
            self.assertEqual(events, ["UE1_ONLY", "TRANSITION_1", "BOTH", "TRANSITION_2", "UE1_AGAIN", "END"])
        self.assertEqual([job[0] for job in generators], [0, 1])
        self.assertEqual(live, {"ue1", "ue2", "gnb"})
        self.assertIn("traffic1", stopped)
        self.assertIn("traffic2", stopped)

    def test_sequence_keeps_sessions(self):
        self.sequence()

    def test_failed_sequence_stops_generators(self):
        self.sequence(fail=True)

    def test_cleanup_attempts_both_generators(self):
        lab = self.lab
        live = {"gnb", "ue1", "ue2"}
        events, attempts = [], []
        lab.panes = lambda: {name: ("%1", False) for name in live}
        lab.generator = lambda index, name, duration: live.add(name)
        lab.hold = lambda *args: None
        lab.event = lambda label, filename="events.csv": events.append(label)
        def interrupt(name):
            attempts.append(name)
            if name == "traffic2":
                raise RuntimeError("stuck traffic2")
            live.discard(name)
        lab.interrupt = interrupt
        with self.assertRaisesRegex(RuntimeError, "cleanup failed"):
            lab.measure()
        self.assertIn("traffic1", attempts)
        self.assertNotIn("traffic1", live)
        self.assertEqual(events[-1], "ABORTED")

    def test_generated_script_preserves_arguments_and_env(self):
        lab = self.lab
        lab.directory = self.directory / "logs space'$literal"
        lab.directory.mkdir()
        calls = []
        lab.panes = lambda: {}
        def tmux(*args, **kwargs):
            calls.append(args)
            return SimpleNamespace(stdout="%99\n", returncode=0)
        lab.tmux = tmux
        script_args = ["a b", "literal'quote", "$(not-a-command)", "`not-a-command`"]
        code = "import json,os,sys; print(json.dumps([os.environ['TEST_VALUE'],sys.argv[1:]]))"
        lab.start_job("probe", [sys.executable, "-c", code, *script_args], self.directory,
                      {"TEST_VALUE": "literal $value 'quoted'"}, stdout="probe.csv")
        script = lab.directory / "probe.sh"
        subprocess.run(["bash", "-n", str(script)], check=True)
        subprocess.run(["bash", str(script)], check=True)
        observed = json.loads((lab.directory / "probe.csv").read_text())
        self.assertEqual(observed, ["literal $value 'quoted'", script_args])
        self.assertEqual(calls[0][0], "new-window")
        self.assertEqual(calls[1][0], "pipe-pane")
        self.assertEqual(shlex.split(calls[0][-1]), ["bash", str(script)])


if __name__ == "__main__":
    unittest.main()
