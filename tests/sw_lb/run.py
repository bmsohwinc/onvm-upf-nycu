#!/usr/bin/env python3
"""ASan/UBSan checks of classification, L2, dispatch and worker coordination.

Compile production function bodies with mocked DPDK/ONVM services. These are
local logic/ownership checks, not a Linux build or an end-to-end UPF test.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
FLAGS = [os.environ.get("CC", "clang"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
         "-Wall", "-Wextra", "-Werror", "-g", "-O1",
         "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
         "-I", str(ROOT / "onvm/upf"), "-I", str(HERE)]


def body(path):
    # Replace only includes; compile production function bodies verbatim.
    return re.sub(r"^#include[^\n]*\n", "", (ROOT / path).read_text(), flags=re.M)


def function(code, name):
    match = re.search(r"(?:static )?(?:void|int)\n" + name + r"\(.*?\n\}", code, re.S)
    assert match, name
    return match.group(0)


with tempfile.TemporaryDirectory(prefix="upf-sw-lb-") as tmp:
    binary = str(Path(tmp) / "test_parser")
    subprocess.run(FLAGS + [
        str(ROOT / "onvm/upf/upf_sw_lb.c"), str(HERE / "test_parser.c"),
        "-o", binary,
    ], check=True)
    subprocess.run([binary], check=True)
    tmp = Path(tmp)
    l2_source = (HERE / "test_l2.c").read_text().replace(
        "/* SOURCE_UNDER_TEST */",
        function(body("5gc/upf_u/upf_u_config.c"), "parse_mac") + "\n" +
        function(body("5gc/upf_u/upf_u_arp.c"), "cached_mac") + "\n" +
        function(body("5gc/upf_u/upf_u_arp.c"), "attach_l2_or_arp"))
    (tmp / "test_l2.c").write_text(l2_source)
    subprocess.run(FLAGS + [str(tmp / "test_l2.c"), "-o", str(tmp / "test_l2")], check=True)
    subprocess.run([str(tmp / "test_l2")], check=True)
    sources = []
    for name, path in [("manager", "onvm/onvm_mgr/onvm_upf_lb.c"),
                       ("rx", "onvm/onvm_mgr/onvm_pkt.c"),
                       ("worker", "5gc/upf_u/upf_u_lb.c")]:
        target = tmp / f"{name}.c"
        code = '#include "stubs.h"\n' + body(path)
        if name == "worker":
            code += "\nvoid test_worker_select(unsigned id) { instance = id; }\n"
        target.write_text(code)
        sources.append(str(target))
    common = body("onvm/onvm_nflib/onvm_pkt_common.c")
    functions = []
    for name in ["onvm_pkt_flush_all_nfs", "onvm_pkt_flush_nf_queue"]:
        match = re.search(r"void\n" + name + r"\([^\n]*\) \{.*?\n\}", common, re.S)
        assert match, name
        functions.append(match.group(0))
    flush = tmp / "flush.c"
    flush.write_text('#include "stubs.h"\n' + "\n".join(functions))
    config = tmp / "map.conf"
    config.write_text("n3 0 10.10.2.11\nn6 1 10.10.3.11\n"
                      "worker 14\nworker 15\n")
    for flow_lookup in [False, True]:
        binary = str(tmp / f"test_runtime_{flow_lookup}")
        subprocess.run(FLAGS + (["-DFLOW_LOOKUP"] if flow_lookup else []) + sources +
                       [str(flush), str(ROOT / "onvm/upf/upf_sw_lb.c"),
                        str(HERE / "test_runtime.c"), "-o", binary], check=True)
        subprocess.run([binary, str(config)], check=True)
