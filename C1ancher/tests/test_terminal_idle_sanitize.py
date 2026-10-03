#!/usr/bin/env python3
"""Instrument terminal/supervisor AND the actual Bash-loaded helper on the host.

Run as root inside a disposable PID/mount namespace; never touches a device.
Leak checks are disabled because the system Bash itself is not instrumented and
retains process-lifetime allocations. This does not establish target Bash ABI.
"""
import os
from pathlib import Path
import signal
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/terminal-idle-sanitize'
BUILD.mkdir(parents=True, exist_ok=True)
flags = ['-std=c11', '-D_GNU_SOURCE', '-D_POSIX_C_SOURCE=200809L', '-O1', '-g',
         '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-fsanitize=address,undefined',
         '-fno-omit-frame-pointer', '-Isrc']

def run(args, **kwargs):
    subprocess.run(args, cwd=ROOT, check=True, **kwargs)

run(['cc', *flags, '-fPIC', '-shared', 'src/services/terminal_idle_bash.c',
     '-o', str(BUILD / 'helper.so')])
run(['python3', 'scripts/embed-terminal-idle.py', str(BUILD / 'helper.so'), str(BUILD / 'image.h')])
blob = '-DC1_TERMINAL_IDLE_BLOB_HEADER="' + str(BUILD / 'image.h') + '"'
for name, sources in (
    ('lifecycle', ['tests/test_terminal_lifecycle.c', 'src/services/terminal.c',
                   'src/platform/liveness.c', 'src/platform/app_lease.c']),
    ('faults', ['tests/test_terminal_idle_faults.c', 'src/platform/liveness.c', 'src/platform/app_lease.c']),
):
    run(['cc', *flags, '-no-pie', blob, *sources, '-o', str(BUILD / name)])
    env = dict(os.environ)
    env['LD_PRELOAD'] = subprocess.check_output(['cc', '-print-file-name=libasan.so'], text=True).strip()
    env['ASAN_OPTIONS'] = 'detect_leaks=0:abort_on_error=1'
    env['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGPIPE):
        signal.signal(sig, signal.SIG_DFL)
    run([str(BUILD / name)], env=env)
print('ASan+UBSan passed: host terminal/supervisor and instrumented module loaded into system Bash; leak checks excluded')
