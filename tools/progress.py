#!/usr/bin/env python3
"""Conservative static constructor audit; translatable does not mean executed."""
import argparse
import json
from pathlib import Path
import lift


def audit(layout, objdump):
    insns = {}
    entries = {entry for mod in layout['modules'] for entry in mod['entries']}
    imports = {item['addr']: item['name'] for item in layout['imports']}
    entries.update(imports)
    for mod in layout['modules']:
        insns.update(lift.disassemble(mod['image'], mod['base'], objdump))
    records = []
    for mod in layout['modules']:
        roots = [('dt', mod['init'])] + [(str(i), addr) for i, addr in enumerate(mod['init_array'])]
        for index, root in roots:
            if not root or root == 0xffffffff:
                continue
            pending, seen, missing, errors = [root], set(), set(), set()
            count = 0
            while pending:
                entry = pending.pop()
                if entry in seen:
                    continue
                seen.add(entry)
                if entry in imports:
                    if imports[entry] not in lift.SHIMS:
                        missing.add(imports[entry])
                    continue
                dependencies = set()
                try:
                    _, instructions = lift.lift(entry, insns, entries, dependencies)
                    count += instructions
                except lift.Unsupported as exc:
                    errors.add(str(exc))
                pending.extend(sorted(dependencies))
            records.append(dict(root=f'{mod["name"]}:init:{index}', address=hex(root),
                                translatable=not errors, discovered_functions=len(seen),
                                translated_instructions=count, missing_shims=sorted(missing),
                                instruction_blockers=sorted(errors)))
    return dict(note='Static lower-bound audit: discovery stops at unsupported instructions; '
                     'indirect targets and runtime execution require separate validation.', constructors=records)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--layout', default='build/prep/layout.json', type=Path)
    ap.add_argument('--out', default='build/progress.json', type=Path)
    ap.add_argument('--objdump', default='objdump')
    args = ap.parse_args()
    result = audit(json.loads(args.layout.read_text()), args.objdump)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + '\n')
    for item in result['constructors']:
        status = 'C OK' if item['translatable'] else 'BLOCKED'
        print(f'{item["root"]}: {status}; {len(item["missing_shims"])} missing direct shims')
        for error in item['instruction_blockers']:
            print(f'  {error}')


if __name__ == '__main__':
    main()
