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


if __name__ == '__main__':
    unittest.main()
