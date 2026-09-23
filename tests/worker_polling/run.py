#!/usr/bin/env python3
"""Compile actual routing/packet code with mock DPDK; extract only the RX entrypoint."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
main = (root / 'onvm/onvm_mgr/main.c').read_text()
match = re.search(r'static int\nrx_thread_main\(.*?\n}', main, re.S)
assert match, 'RX entrypoint not found'
with tempfile.TemporaryDirectory(prefix='upf-polling-') as directory:
    build = Path(directory)
    (build / 'rx_loop.inc').write_text(match.group(0))
    for defines in ([], ['-DFLOW_LOOKUP']):
        binary = build / 'test_worker_polling'
        subprocess.run([
            os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
            '-pthread', '-fsanitize=address,undefined', *defines,
            '-I' + str(build), '-Itests/worker_polling/stubs',
            '-Itests/worker_slots/stubs', '-Ionvm/upf',
            'tests/worker_polling/test_worker_polling.c', '-o', str(binary)
        ], cwd=root, check=True)
        subprocess.run([str(binary)], check=True)
        subprocess.run([str(binary), 'invalid-config'], check=True)
