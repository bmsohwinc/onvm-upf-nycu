#!/usr/bin/env python3
"""Compile production lifecycle code with mock time, processes, PFCP and queues."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
scaler = (root / "5gc/upf_c/upf_scaling.c").read_text()
handler = (root / "5gc/upf_c/n4_onvm_pfcp_handler.c").read_text()
builder = (root / "5gc/upf_c/n4_onvm_pfcp_build.c").read_text()
worker = (root / "5gc/upf_u/upf_u.c").read_text()
steering = (root / "onvm/onvm_mgr/onvm_upf_steer.c").read_text()
shaper = (root / "5gc/upf_u/upf_u_shaper.c").read_text()
shaper_header = (root / "5gc/upf_u/upf_u_shaper.h").read_text()
meter = (root / "5gc/upf_u/upf_u_trtcm.c").read_text()
dispatcher = (root / "5gc/upf_c/n4_dispatcher.c").read_text()
transactions = (root / "onvm/pfcp/pfcp_xact.c").read_text()
nflib = (root / "onvm/onvm_nflib/onvm_nflib.c").read_text()


def function(source, name):
    match = re.search(r"^(?:(?:static|inline)\s+)*(?:void|int|bool|Status)\s+" + name + r"\([^)]*\)\s*{", source, re.M)
    assert match, name
    depth = 1
    end = match.end()
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


with tempfile.TemporaryDirectory(prefix="upf-scale-down-") as directory:
    build = Path(directory)
    lifecycle = scaler[scaler.index("#define MAX_PENDING"):scaler.index("int UpfScalingInit")]
    lifecycle += scaler[scaler.index("int UpfScalingEnqueue"):scaler.index("int UpfControlLoop")]
    (build / "lifecycle.inc").write_text(lifecycle)
    (build / "response.inc").write_text(function(builder, "UpfN4BuildSessionDeletionResponse") + "\n" +
                                       function(handler, "UpfN4SendDeletionResponse"))
    (build / "cleanup.inc").write_text(function(worker, "cleanup_session"))
    (build / "spawn.inc").write_text(function(scaler, "spawn_worker"))
    (build / "signals.inc").write_text("\n".join(function(nflib, name) for name in (
        "onvm_nflib_handle_signal", "onvm_nflib_start_signal_handler", "onvm_nflib_run")))
    callback = nflib.index("                if (nf->function_table->user_actions != ONVM_NO_CALLBACK")
    (build / "callback.inc").write_text(nflib[callback:nflib.index("\n\n", callback)])
    (build / "receive.inc").write_text(function(transactions, "PfcpXactUpdateRx"))
    start = dispatcher.index("            if (pfcpMessage->header.seidP) {")
    end = dispatcher.index("                if (!session) {", dispatcher.index("UTLT_Assert(session ||", start))
    (build / "dispatch.inc").write_text(dispatcher[start:end] + "\n}\n")
    (build / "filter.inc").write_text(function(steering, "filter"))
    (build / "steering.inc").write_text(
        steering[steering.index("static uint32_t n3_generation"):steering.index("static int ethctl")] +
        function(steering, "apply"))
    (build / "purge.inc").write_text(
        shaper_header[shaper_header.index("enum upf_u_shaper_pkt_color"):shaper_header.index("int\nupf_u_shaper_init")] +
        shaper[shaper.index("#define SHAPER_SCAN_BUDGET"):shaper.index("static uint64_t g_shaper_queued")] +
        "\n".join(function(shaper, name) for name in (
            "shaper_free_entry", "shaper_clear_ue_active", "upf_u_shaper_forget_ue", "upf_u_shaper_cleanup")) +
        "\n" + "\n".join(function(meter, name) for name in (
            "ueHashFunc", "ueHashSlotInUse", "ueHashSetInUse", "ueHashInit",
            "ueHashSearch", "ueHashInsert", "removeEntrybyUeIp")))
    for name in ("test_scale_down", "test_cleanup", "test_filter", "test_signals"):
        binary = build / name
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-function", "-Wno-unused-variable", "-pthread", "-fsanitize=address,undefined",
            "-I" + str(build), "-Itests/worker_slots/stubs", "-Ionvm/upf",
            "tests/scale_down/" + name + ".c", "-o", str(binary)
        ], cwd=root, check=True)
        subprocess.run([str(binary)], check=True, timeout=20)
