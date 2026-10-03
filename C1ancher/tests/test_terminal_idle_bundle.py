#!/usr/bin/env python3
"""Build-only embedding checks: no new update component or device installation."""
import argparse
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def symbols(path):
    text = subprocess.check_output(['readelf', '--dyn-syms', '-W', str(path)], text=True)
    result = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) >= 8 and fields[0].rstrip(':').isdigit():
            result[fields[7].split('@')[0]] = (int(fields[2]), fields[6])
    return result

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build')
    parser.add_argument('--bash', type=Path, default=Path('/bin/bash'))
    args = parser.parse_args()
    for arch in ('host', 'target'):
        module = args.build_dir / arch / 'terminal-idle.so'
        header = (args.build_dir / arch / 'terminal-idle-image.h').read_text()
        image = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', header))
        assert image == module.read_bytes(), f'{arch} generated image differs'
        dynamic = subprocess.check_output(['readelf', '-d', str(module)], text=True)
        assert '(NEEDED)' not in dynamic, f'{arch} unexpectedly requires a build-host library'
        print(f'{arch}: exact embedded ELF, {len(image)} bytes, no DT_NEEDED')
    # Validate actual host dynamic symbol sizes. This is NOT device evidence.
    # Compile against the host public header as well as checking ELF sizes.
    subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror',
                    '-include', 'readline/readline.h', '-fsyntax-only',
                    str(ROOT / 'src/services/terminal_idle_bash.c')], check=True)
    macros = subprocess.check_output(['cc', '-E', '-dM', '-include', 'readline/readline.h',
                                      '-x', 'c', '/dev/null'], text=True)
    version_match = re.search(r'^#define RL_READLINE_VERSION (\S+)', macros, re.M)
    assert version_match and int(version_match[1], 0) in (0x0801, 0x0802), version_match
    expected = {'RL_STATE_INITIALIZED': 2,
                'RL_STATE_TERMPREPPED': 4, 'RL_STATE_READCMD': 8, 'RL_STATE_OVERWRITE': 0x2000,
                'RL_STATE_TTYCSAVED': 0x40000, 'RL_STATE_CALLBACK': 0x80000,
                'RL_STATE_VICMDONCE': 0x400000}
    for name, value in expected.items():
        match = re.search(r'^#define ' + name + r' (\S+)', macros, re.M)
        assert match and int(match[1], 0) == value, name
    bash = symbols(args.bash)
    import ctypes
    assert bash['rl_readline_state'][0] == ctypes.sizeof(ctypes.c_ulong), bash['rl_readline_state']
    for name in ('rl_event_hook', 'rl_line_buffer', 'rl_instream', 'rl_executing_macro'):
        assert bash[name][0] == ctypes.sizeof(ctypes.c_void_p), name
    for name in ('rl_end', 'rl_point', 'rl_done', 'rl_pending_input', 'rl_readline_version',
                 'executing', 'executing_builtin', 'parse_and_execute_level'):
        assert bash[name][0] == ctypes.sizeof(ctypes.c_int), name
    for name in ('get_current_prompt_level', '_rl_pushed_input_available', 'rl_set_keyboard_input_timeout'):
        assert name in bash and bash[name][1] != 'UND', name
    print(f'{args.bash}: host dynamic Bash/Readline symbol sizes match declarations')
    target = args.build_dir / 'C1ancher'
    if target.exists():
        assert (args.build_dir / 'target/terminal-idle.so').read_bytes() in target.read_bytes()
        print('C1ancher: complete target module present inside executable')
    # Old protocol remains exactly the four existing entries.
    protocol = (ROOT / 'src/update/protocol.c').read_text()
    assert re.search(r'#define C1_UPDATE_COMPONENT_COUNT\s+4U', (ROOT / 'src/update/update.h').read_text())
    assert 'line_number != 14U' in protocol
    assert 'terminal-idle' not in protocol and 'terminal_idle' not in protocol
    print('update protocol has no terminal helper entry; release remains four components')

if __name__ == '__main__':
    main()
