#!/usr/bin/env python3
"""Build the real Linux launcher plus foundation constructors as native ARM64.

Additional module/indirect roots can be persisted in config/game-roots.json.
Generated code is AOT C, with traps for instructions/imports not yet ported.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import lift


def roots(layout):
    result = ['portal2_linux:symbol:main']
    for module in layout['modules']:
        if module['name'] in ('libtier0.so', 'libvstdlib.so', 'portal2_linux'):
            if module['init']: result.append(f'{module["name"]}:init:dt')
            result += [f'{module["name"]}:init:{i}' for i in range(len(module['init_array']))]
    extra = Path('config/game-roots.json')
    if extra.exists(): result += json.loads(extra.read_text())
    return list(dict.fromkeys(result))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--layout', type=Path, default=Path('build/game/prep/layout.json'))
    parser.add_argument('--out', type=Path, default=Path('build/portal2-native'))
    parser.add_argument('--opt', default='1', choices=['0', '1', '2', '3'])
    args = parser.parse_args()
    layout = json.loads(args.layout.read_text())
    try:
        code = lift.generate(layout, roots(layout), 'objdump', True, Path('build/disasm-cache'))
    except lift.Unsupported as exc:
        parser.exit(1, f'game lift: {exc}\n')
    source = args.layout.parent.parent / 'game.c'
    source.write_text(code)
    runtime = sorted(str(p) for p in Path('runtime').glob('*.c'))
    command = ['clang', f'-O{args.opt}', '-g', '-Wall', '-Wextra', '-Werror', '-Iruntime',
               *runtime, str(source), 'src/main.c', '-o', str(args.out)]
    subprocess.run(command, check=True)


if __name__ == '__main__':
    main()
