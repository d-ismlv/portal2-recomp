import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('table_roots', Path(__file__).resolve().parents[1] / 'tools/table_roots.py')
tables = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tables)


class TableRootsTests(unittest.TestCase):
    def test_adjacent_known_code_stops_at_noncode_and_unknown_entries(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / 'module.so'
            image.write_bytes(struct.pack('<9I', 0x1010, 0, 0x1010, 0x1020, 0x1030, 0x1040, 0, 0x1050, 0))
            module = {'name': 'module.so', 'base': 0x1000, 'image': str(image),
                      'entries': [0x1010, 0x1020, 0x1040, 0x1050, 0x2000],
                      'loads': [{'flags': 5, 'vaddr': 0x1000, 'off': 36, 'filesz': 0x100},
                                {'flags': 6, 'vaddr': 0x2000, 'off': 0, 'filesz': 36}]}
            self.assertEqual(tables.adjacent_roots({'modules': [module]}, 0x1020),
                             ['module.so:code:0x10', 'module.so:code:0x20'])
            self.assertEqual(tables.adjacent_roots({'modules': [module]}, 0x2000), [])


if __name__ == '__main__':
    unittest.main()
