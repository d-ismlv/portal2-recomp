"""Petite DLL PE32 de test : tests/pe_plugin.s assemblé en COFF par clang,
puis lié ici comme le ferait link.exe (sections à leur RVA, table .reloc pour
chaque adresse absolue, exportations des symboles globaux de .text)."""
from pathlib import Path
import struct
import subprocess

IMAGE_BASE = 0x10000000
SECTION_ALIGNMENT, FILE_ALIGNMENT, HEADERS = 0x1000, 0x200, 0x400
DIR32, REL32 = 6, 0x14


def _align(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


def parse_coff(data):
    _, section_count, _, symbol_offset, symbol_count, optional_size, _ = struct.unpack_from('<HHIIIHH', data, 0)
    strings = symbol_offset + 18 * symbol_count

    def name(raw):
        if raw[:4] == b'\0\0\0\0':
            start = strings + struct.unpack_from('<I', raw, 4)[0]
            return data[start:data.index(b'\0', start)].decode()
        return raw.rstrip(b'\0').decode()
    sections = []
    for i in range(section_count):
        o = 20 + optional_size + 40 * i
        size, raw, relocs, count = (struct.unpack_from('<I', data, o + 16)[0], struct.unpack_from('<I', data, o + 20)[0],
                                    struct.unpack_from('<I', data, o + 24)[0], struct.unpack_from('<H', data, o + 32)[0])
        sections.append(dict(name=name(data[o:o + 8]), data=bytearray(data[raw:raw + size]) if raw else bytearray(size),
                             relocations=[struct.unpack_from('<IIH', data, relocs + 10 * j) for j in range(count)]))
    symbols, i = [], 0
    while i < symbol_count:
        o = symbol_offset + 18 * i
        value, section, _, storage, aux = struct.unpack_from('<IhHBB', data, o + 8)
        symbols.append(dict(name=name(data[o:o + 8]), value=value, section=section, storage=storage))
        symbols += [None] * aux
        i += 1 + aux
    return sections, symbols


def link_dll(obj, name='testpe.asi'):
    """(octets de la DLL, {symbole: RVA}) depuis un objet COFF i386."""
    sections, symbols = parse_coff(Path(obj).read_bytes())
    layout = [s for s in sections if s['name'] in ('.text', '.data')]
    rva = {}
    next_rva = SECTION_ALIGNMENT
    for s in layout:
        rva[s['name']] = next_rva
        next_rva += _align(max(len(s['data']), 1), SECTION_ALIGNMENT)
    address = {}
    for symbol in symbols:
        if symbol and symbol['section'] > 0 and sections[symbol['section'] - 1]['name'] in rva:
            address[symbol['name']] = rva[sections[symbol['section'] - 1]['name']] + symbol['value']
    base_relocations = []
    for s in layout:
        for offset, symbol_index, kind in s['relocations']:
            symbol = symbols[symbol_index]
            target = rva[sections[symbol['section'] - 1]['name']] + symbol['value']
            place = rva[s['name']] + offset
            current = struct.unpack_from('<I', s['data'], offset)[0]
            if kind == DIR32:
                struct.pack_into('<I', s['data'], offset, (current + IMAGE_BASE + target) & 0xffffffff)
                base_relocations.append(place)
            elif kind == REL32:
                struct.pack_into('<I', s['data'], offset, (current + target - place - 4) & 0xffffffff)
            else:
                raise ValueError(f'COFF relocation {kind:#x}')
    # Exportations : symboles globaux de .text, par nom.
    exported = sorted((s['name'], address[s['name']]) for s in symbols
                      if s and s['storage'] == 2 and s['section'] > 0 and sections[s['section'] - 1]['name'] == '.text')
    edata_rva = next_rva
    names_offset = 40 + 10 * len(exported)
    strings = bytearray(name.encode() + b'\0')
    name_rvas = []
    for export, _ in exported:
        name_rvas.append(edata_rva + names_offset + len(strings))
        strings += export.encode() + b'\0'
    edata = struct.pack('<IIHHIIIIIII', 0, 0, 0, 0, edata_rva + names_offset, 1, len(exported), len(exported),
                        edata_rva + 40, edata_rva + 40 + 4 * len(exported), edata_rva + 40 + 8 * len(exported))
    edata += b''.join(struct.pack('<I', target) for _, target in exported)
    edata += b''.join(struct.pack('<I', r) for r in name_rvas)
    edata += b''.join(struct.pack('<H', i) for i in range(len(exported)))
    edata += strings
    next_rva += _align(len(edata), SECTION_ALIGNMENT)
    reloc_rva = next_rva
    reloc = bytearray()
    for page in sorted({r & ~0xfff for r in base_relocations}):
        entries = [(3 << 12) | (r & 0xfff) for r in sorted(base_relocations) if r & ~0xfff == page]
        if len(entries) % 2:
            entries.append(0)
        reloc += struct.pack('<II', page, 8 + 2 * len(entries)) + b''.join(struct.pack('<H', e) for e in entries)
    next_rva += _align(len(reloc), SECTION_ALIGNMENT)
    image_size = next_rva
    out_sections = [('.text', rva['.text'], layout[0]['data'], 0x60000020),
                    ('.data', rva['.data'], layout[1]['data'], 0xc0000040),
                    ('.edata', edata_rva, bytes(edata), 0x40000040),
                    ('.reloc', reloc_rva, bytes(reloc), 0x42000040)]
    directories = [(0, 0)] * 16
    directories[0] = (edata_rva, len(edata))
    directories[5] = (reloc_rva, len(reloc))
    optional = struct.pack('<HBBIIIIIIIIIHHHHHHIIIIHHIIIIII', 0x10b, 9, 0, _align(len(layout[0]['data']), FILE_ALIGNMENT),
                           0, 0, 0, rva['.text'], rva['.data'], IMAGE_BASE, SECTION_ALIGNMENT, FILE_ALIGNMENT,
                           5, 0, 0, 0, 5, 0, 0, image_size, HEADERS, 0, 2, 0,
                           0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
    optional += b''.join(struct.pack('<II', *d) for d in directories)
    headers = bytearray(b'MZ' + bytes(0x3a) + struct.pack('<I', 0x40))
    headers += b'PE\0\0' + struct.pack('<HHIIIHH', 0x14c, len(out_sections), 0, 0, 0, len(optional), 0x2102) + optional
    raw = HEADERS
    body = bytearray()
    for section_name, section_rva, data, flags in out_sections:
        raw_size = _align(len(data), FILE_ALIGNMENT)
        headers += struct.pack('<8sIIIIIIHHI', section_name.encode(), len(data), section_rva, raw_size, raw,
                               0, 0, 0, 0, flags)
        body += bytes(data) + bytes(raw_size - len(data))
        raw += raw_size
    assert len(headers) <= HEADERS
    return bytes(headers) + bytes(HEADERS - len(headers)) + bytes(body), address


def build(directory, source=Path(__file__).with_name('pe_plugin.s')):
    """Assemble et lie la DLL de test dans `directory` ; renvoie (chemin, symboles)."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    obj = directory / 'pe_plugin.obj'
    subprocess.run(['clang', '-target', 'i386-pc-windows-msvc', '-c', str(source), '-o', str(obj)], check=True)
    data, symbols = link_dll(obj)
    path = directory / 'testpe.asi'
    path.write_bytes(data)
    return path, symbols
