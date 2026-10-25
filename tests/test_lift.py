import importlib.util
from pathlib import Path
import unittest

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))

spec = importlib.util.spec_from_file_location('lift', Path(__file__).resolve().parents[1] / 'tools/lift.py')
lift = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lift)


class LifterTests(unittest.TestCase):
    def test_runtime_files_declare_shims(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / 'a.c').write_text('// SHIMS: SDL_Init SDL_Quit\nint x;\n// SHIMS: sinf\n')
            (Path(tmp) / 'b.h').write_text('// SHIMS: ignored_in_headers\n')
            self.assertEqual(lift.declared_shims(Path(tmp)), {'SDL_Init', 'SDL_Quit', 'sinf'})

    def test_reject_unsupported_reachable_instruction(self):
        with self.assertRaisesRegex(lift.Unsupported, '0x10.*rsm'):
            lift.lift(0x10, {0x10: (0x12, 'rsm', '')})

    def test_direct_call_records_dependency_and_return_site(self):
        calls = set()
        code, count = lift.lift(0x10, {0x10: (0x15, 'call', '0x30'),
                                      0x15: (0x16, 'ret', '')}, callees=calls)
        self.assertEqual(calls, {0x30})
        self.assertEqual(count, 2)
        self.assertIn('rt_call(c, a, 0x15u)', code)

    def test_indirect_jump_uses_checked_dispatch(self):
        code, count = lift.lift(0x10, {0x10: (0x12, 'jmp', 'eax')})
        self.assertEqual(count, 1)
        self.assertIn('rt_dispatch(c, a)', code)

    def test_reject_unknown_direct_import(self):
        layout = {'modules': [], 'imports': [{'name': 'not_supported', 'addr': 0x30}]}
        from unittest.mock import patch
        with patch.object(lift, 'resolve_export', return_value=0x30):
            with self.assertRaisesRegex(lift.Unsupported, 'has no shim'):
                lift.generate(layout, ['entry'], 'objdump')

    def test_reject_target_inside_instruction(self):
        with self.assertRaisesRegex(lift.Unsupported, 'no instruction at 0x11'):
            lift.lift(0x10, {0x10: (0x12, 'jmp', '0x11')})

    def test_only_reachable_code_is_required(self):
        code, count = lift.lift(0x10, {0x10: (0x11, 'ret', ''), 0x11: (0x13, 'ud2', '')})
        self.assertEqual(count, 1)
        self.assertIn('rt_return(c)', code)

    def test_lock_prefix_keeps_the_real_instruction_boundary(self):
        from unittest.mock import patch
        import subprocess
        output = '10: f0 \tlock\n11: 0f b1 16 \tcmpxchg dword ptr [esi], edx\n14: c3 \tret\n'
        result = subprocess.CompletedProcess([], 0, output, '')
        with patch.object(lift.subprocess, 'run', return_value=result):
            insns = lift.disassemble('fixture', 0, 'objdump')
        self.assertNotIn(0x11, insns)
        self.assertEqual(insns[0x10], (0x14, 'lock_cmpxchg', 'dword ptr [esi], edx'))

    def test_reject_segment_addressing(self):
        with self.assertRaises(lift.Unsupported):
            lift.Operand('dword ptr fs:[0x14]')

    def test_reject_malformed_address(self):
        for expr in ('eax/4', 'eax++4', 'eax*bad', 'eax;abort()', ''):
            with self.subTest(expr=expr), self.assertRaises(lift.Unsupported):
                lift.address(expr)

    def test_pic_jump_table_becomes_local_switch(self):
        import tempfile
        got, base = 0x10020000, 0x10000000
        insns = {
            0x10: (0x13, 'cmp', 'eax, 0x2'), 0x13: (0x15, 'ja', '0x40'),
            0x15: (0x1c, 'mov', 'ecx, dword ptr [ebx + 4*eax - 0x100]'),
            0x1c: (0x1e, 'add', 'ecx, ebx'), 0x1e: (0x20, 'jmp', 'ecx'),
            0x20: (0x21, 'ret', ''), 0x21: (0x22, 'ret', ''), 0x22: (0x23, 'ret', ''),
            0x40: (0x41, 'ret', '')}
        insns = {pc + base: (nxt + base, op, args) for pc, (nxt, op, args) in insns.items()}
        insns[base + 0x13] = (base + 0x15, 'ja', hex(base + 0x40))
        with tempfile.TemporaryDirectory() as tmp:
            image = bytearray(0x30000)
            table = got - 0x100 - base
            for i, target in enumerate((0x20, 0x21, 0x22, 0x20)):   # 4th entry is beyond the bound
                image[table + 4 * i:table + 4 * i + 4] = ((base + target - got) & 0xffffffff).to_bytes(4, 'little')
            path = Path(tmp) / 'image'; path.write_bytes(bytes(image))
            layout = {'modules': [{'name': 'm', 'image': str(path), 'gotplt': got,
                                   'loads': [dict(off=0, vaddr=base, filesz=0x30000, memsz=0x30000, flags=5)]}]}
            tables = lift.JumpTables(layout, insns)
            self.assertEqual(tables(base + 0x1e), [base + 0x20, base + 0x21, base + 0x22])
            code, count = lift.lift(base + 0x10, insns, tables=tables)
        self.assertIn(f'case {base + 0x21:#x}u: goto L{base + 0x21:x};', code)
        self.assertIn('default: c->flags = f; rt_dispatch(c, a); return;', code)
        self.assertEqual(count, 9)

    def test_jump_table_with_bad_declared_case_is_not_guessed(self):
        insns = {0x10: (0x13, 'cmp', 'eax, 0x1'), 0x13: (0x15, 'ja', '0x40'),
                 0x15: (0x1c, 'jmp', 'dword ptr [4*eax + 0x1000]'), 0x40: (0x41, 'ret', ''),
                 0x20: (0x21, 'ret', '')}
        tables = lift.JumpTables({'modules': []}, insns)
        tables.read32 = lambda address: {0x1000: 0x20, 0x1004: 0x25}.get(address)  # 0x25: not code
        self.assertIsNone(tables(0x15))
        code, _ = lift.lift(0x10, insns, tables=tables)
        self.assertNotIn('switch', code)

    def test_audit_scan_collects_every_rejection(self):
        insns = {0x10: (0x12, 'rsm', ''), 0x12: (0x14, 'vzeroall', ''), 0x14: (0x15, 'ret', '')}
        count, failures, indirect = lift.scan(0x10, insns)
        self.assertEqual(count, 3)
        self.assertEqual([op for _, op, _, _ in failures], ['rsm', 'vzeroall'])
        with self.assertRaises(lift.Unsupported):   # the generator itself stays fail-closed
            lift.lift(0x10, insns)


if __name__ == '__main__':
    unittest.main()
