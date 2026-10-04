#!/usr/bin/env python3
"""Exercise the production queue sampler and placement policy with mock NFs/time."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "5gc/upf_c/upf_scaling.c").read_text()
live_count = re.search(r"static int live_count\(void\) \{.*?\n}", source, re.S)
assert live_count, "live_count not found"
policy = source[source.index("struct QueueSample {"):source.index("static void reap_workers")]
with tempfile.TemporaryDirectory(prefix="upf-proactive-") as directory:
    build = Path(directory)
    (build / "policy.inc").write_text(live_count.group(0) + "\n" + policy)
    binary = build / "test_proactive_scaling"
    subprocess.run([
        os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-I" + str(build), "-Ionvm/upf",
        "tests/proactive_scaling/test_proactive_scaling.c", "-o", str(binary)
    ], cwd=root, check=True)
    subprocess.run([str(binary)], check=True)
