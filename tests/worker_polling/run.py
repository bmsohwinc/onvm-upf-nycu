#!/usr/bin/env python3
"""Compile actual routing/packet code and RX/TX entrypoints with mock DPDK."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
main = (root / 'onvm/onvm_mgr/main.c').read_text()
with tempfile.TemporaryDirectory(prefix='upf-polling-') as directory:
    build = Path(directory)
    for direction in ('rx', 'tx'):
        match = re.search(r'static int\n' + direction + r'_thread_main\(.*?\n}', main, re.S)
        assert match, direction + ' entrypoint not found'
        (build / (direction + '_loop.inc')).write_text(match.group(0))
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
