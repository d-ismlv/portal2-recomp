#!/usr/bin/env python3
"""Emit host metadata for prepared guest modules; it contains no guest code."""
import argparse
import json
from pathlib import Path


def generate(layout):
    lines = ['#include "modules.h"']
    for i, module in enumerate(layout['modules']):
        values = ', '.join(f'{address:#x}u' for address in module['init_array']) or '0'
        lines.append(f'static const uint32_t init_{i}[] = {{{values}}};')
        lines.append(f'static const GuestSymbol symbols_{i}[] = {{')
        symbols = dict(module['exports'])
        # main/_start can be present only in the ordinary symbol table.
        for address, name in module['names'].items():
            if name in ('main', '_start'): symbols.setdefault(name, int(address, 16))
        for name, address in sorted(symbols.items()):
            lines.append(f'{{{json.dumps(name)}, {address:#x}u}},')
        lines.append('{NULL, 0}};')
        needed = ', '.join(json.dumps(name) for name in module['needed']) or 'NULL'
        lines.append(f'static const char *const needed_{i}[] = {{{needed}}};')
    lines.append('const GuestModule rt_modules[] = {')
    for i, module in enumerate(layout['modules']):
        lines.append(f'{{{json.dumps(module["name"])}, {module["modid"]}u, {module["base"]:#x}u, '
                     f'{module.get("entry",0):#x}u, {module["init"]:#x}u, {module["fini"]:#x}u, '
                     f'init_{i}, {len(module["init_array"])}u, symbols_{i}, '
                     f'sizeof(symbols_{i}) / sizeof(*symbols_{i}) - 1, needed_{i}, {len(module["needed"])}u}},')
    lines.extend(['};', 'const size_t rt_module_count = sizeof(rt_modules) / sizeof(*rt_modules);', ''])
    return '\n'.join(lines)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--layout', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(generate(json.loads(args.layout.read_text())))
