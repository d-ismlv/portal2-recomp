"""Greffon PE de test pour tests/checks_pe.c : tests/pe_plugin.s lié en DLL
(tests/pe_fixture.py) puis traduit par lift.generate_pe comme les .asi du jeu."""
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'tools'), str(ROOT / 'tests')]
import lift  # noqa: E402
import pe_fixture  # noqa: E402
import peprep  # noqa: E402

out = Path(sys.argv[1])
path, symbols = pe_fixture.build(out.parent / 'pe-fixture')
source, stats = lift.generate_pe(peprep.PeImage(path), 'testpe')
if stats['not_compiled']:
    raise SystemExit(f'test plugin not fully lifted: {stats["not_compiled"]}')
data = path.read_bytes()
code = [source,
        f'const uint32_t testpe_file_size = {len(data)}u;',
        f'const uint8_t testpe_file[{len(data)}] = {{']
code += [','.join(str(b) for b in data[i:i + 32]) + ',' for i in range(0, len(data), 32)]
code += ['};', 'uint32_t testpe_symbol(const char *name);', 'uint32_t testpe_symbol(const char *name) {']
code += [f'if (!strcmp(name, {json.dumps(name)})) return {rva:#x}u;' for name, rva in sorted(symbols.items())]
code += ['return 0; }', '']
out.write_text('\n'.join(code))
