#!/usr/bin/env python3
"""Charge des .so ELF i386 à des adresses fixes et applique toutes leurs relocations hors ligne.

Sortie, pour chaque module : une copie de l'ELF dont les octets sont relocalisés (code et données),
et un JSON de métadonnées (base, segments, init, exports, imports, points d'entrée connus).
Le désassemblage se fait ensuite sur la copie relocalisée, avec --adjust-vma=base.
"""
import json, os, struct, sys

THUNK_BASE = 0xF0000000   # une adresse par fonction importée de la libc
THUNK_STRIDE = 0x10
SHIMDATA_BASE = 0xE0000000  # objets de données importés (stderr, ...)
# Pont Steam (runtime/steam.c) : les exports Steam des libsteam_api*.so invités deviennent des
# imports hôtes ; leur libsupc++ embarqué (__cxa_*, typeinfo), utilisé par tout le jeu, reste invité.
HOST_BRIDGED = ('SteamAPI_', 'SteamInternal_', 'SteamGameServer_')

R_386_NONE, R_386_32, R_386_PC32 = 0, 1, 2
R_386_GLOB_DAT, R_386_JMP_SLOT, R_386_RELATIVE = 6, 7, 8
R_386_TLS_DTPMOD32, R_386_TLS_DTPOFF32 = 35, 36


class Elf:
    def __init__(self, path, base, modid):
        self.path, self.base, self.modid = path, base, modid
        self.name = os.path.basename(path)
        self.data = bytearray(open(path, 'rb').read())
        d = self.data
        assert d[:4] == b'\x7fELF' and d[4] == 1, 'ELF32 attendu'
        (self.e_type, self.e_machine, _, self.e_entry, self.e_phoff, self.e_shoff, _, _,
         self.e_phentsize, self.e_phnum, self.e_shentsize, self.e_shnum, self.e_shstrndx) = \
            struct.unpack_from('<HHIIIIIHHHHHH', d, 16)
        assert self.e_machine == 3
        self.phdrs = [struct.unpack_from('<8I', d, self.e_phoff + i * self.e_phentsize) for i in range(self.e_phnum)]
        self.shdrs = [struct.unpack_from('<10I', d, self.e_shoff + i * self.e_shentsize) for i in range(self.e_shnum)]
        shstr = self.shdrs[self.e_shstrndx]
        self.secs = {}
        for s in self.shdrs:
            n = self.cstr(shstr[4] + s[0])
            self.secs[n] = dict(name=n, type=s[1], flags=s[2], addr=s[3], off=s[4], size=s[5], link=s[6], entsize=s[9])
        self.loads = [p for p in self.phdrs if p[0] == 1]
        self.tls = next((p for p in self.phdrs if p[0] == 7), None)
        self.parse_dynamic()

    def cstr(self, off):
        e = self.data.index(b'\0', off)
        return self.data[off:e].decode()

    def v2o(self, va):
        for p in self.loads:
            if p[2] <= va < p[2] + p[4]:
                return p[1] + va - p[2]
        return None

    def u32(self, va):
        return struct.unpack_from('<I', self.data, self.v2o(va))[0]

    def put32(self, va, v):
        o = self.v2o(va)
        assert o is not None, hex(va)
        struct.pack_into('<I', self.data, o, v & 0xffffffff)

    def parse_dynamic(self):
        dyn = next(p for p in self.phdrs if p[0] == 2)
        self.dyn = {}
        self.needed = []
        o = dyn[1]
        while True:
            tag, val = struct.unpack_from('<iI', self.data, o)
            o += 8
            if tag == 0:
                break
            if tag == 1:
                self.needed.append(val)
            else:
                self.dyn.setdefault(tag, val)
        strtab = self.v2o(self.dyn[5])
        self.needed = [self.cstr(strtab + n) for n in self.needed]
        ds = self.secs['.dynsym']
        dstr = self.secs['.dynstr']['off']
        self.dynsyms = []
        for i in range(ds['size'] // 16):
            nm, val, size, info, other, shndx = struct.unpack_from('<IIIBBH', self.data, ds['off'] + i * 16)
            self.dynsyms.append(dict(name=self.cstr(dstr + nm), value=val, size=size,
                                     bind=info >> 4, type=info & 15, shndx=shndx, visibility=other & 3))
        self.symtab = []
        if '.symtab' in self.secs:
            st = self.secs['.symtab']
            sstr = self.shdrs[st['link']][4]
            for i in range(st['size'] // 16):
                nm, val, size, info, other, shndx = struct.unpack_from('<IIIBBH', self.data, st['off'] + i * 16)
                self.symtab.append(dict(name=self.cstr(sstr + nm), value=val, size=size,
                                        bind=info >> 4, type=info & 15, shndx=shndx, visibility=other & 3))

    def rels(self):
        out = []
        for tag_addr, tag_size in ((17, 18), (23, 2)):  # DT_REL/DT_RELSZ, DT_JMPREL/DT_PLTRELSZ
            if tag_addr in self.dyn:
                o = self.v2o(self.dyn[tag_addr])
                for i in range(self.dyn[tag_size] // 8):
                    off, info = struct.unpack_from('<II', self.data, o + i * 8)
                    out.append((off, info & 0xff, info >> 8))
        return out

    def exports(self):
        if not hasattr(self, '_export_cache'):
            self._export_cache = {s['name']: s for s in self.dynsyms if s['shndx'] != 0 and s['name'] and s['bind'] in (1, 2) and s['visibility'] in (0, 3)}
        return self._export_cache


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('mods', nargs='+', help='chemin.so@0xBASE, dans l\'ordre de chargement')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    mods = []
    for i, spec in enumerate(a.mods):
        p, b = spec.split('@')
        mods.append(Elf(p, int(b, 16), i + 1))

    definitions = {}
    for module in mods:
        for name, symbol in module.exports().items():
            definitions.setdefault(name, (module, symbol))

    imports = {}      # nom -> adresse du thunk
    data_imports = {}
    order = []

    def thunk(name):
        if name not in imports:
            imports[name] = THUNK_BASE + THUNK_STRIDE * len(order)
            order.append(name)
        return imports[name]

    def lookup(name, weak, kind=0, size=0):
        definition = definitions.get(name)
        if definition and definition[0].name.startswith('libsteam_api') and name.startswith(HOST_BRIDGED):
            definition = None
        if definition:
            module, symbol = definition
            value = symbol['value'] if symbol['shndx'] == 0xfff1 else module.base + symbol['value']
            return value, module.name
        if kind == 1 or name in ('stdin', 'stdout', 'stderr', 'environ', '__environ'):
            if name not in data_imports:
                address = SHIMDATA_BASE + sum((item['size'] + 15) & ~15 for item in data_imports.values())
                data_imports[name] = dict(name=name, addr=address, size=max(size, 4))
            else:
                if size > data_imports[name]['size']:
                    raise SystemExit(f'imported object {name}: inconsistent sizes')
            return data_imports[name]['addr'], 'host-data' 
        if weak and (name.startswith('_ITM_') or name.startswith('_ZGTt') or name == '__gmon_start__'):
            return 0, None
        return thunk(name), 'libc'

    meta_all = dict(imports=None, modules=[])
    for m in mods:
        tls_sym_off = {}
        n_by_type = {}
        for off, typ, si in m.rels():
            n_by_type[typ] = n_by_type.get(typ, 0) + 1
            if typ == R_386_NONE:
                continue
            P = m.base + off
            A = m.u32(off)
            sym = m.dynsyms[si] if si else None
            if typ == R_386_RELATIVE:
                m.put32(off, m.base + A)
                continue
            if typ in (R_386_TLS_DTPMOD32, R_386_TLS_DTPOFF32):
                owner, tls_symbol = m, sym
                if sym and sym['bind'] and sym.get('visibility', 0) not in (1, 2, 3):
                    definition = definitions.get(sym['name'])
                    if definition:
                        owner, tls_symbol = definition
                    elif sym['shndx'] == 0:
                        raise SystemExit(f'{m.name}: unresolved external TLS symbol {sym["name"]}')
                if typ == R_386_TLS_DTPMOD32:
                    m.put32(off, owner.modid)
                else:
                    if not tls_symbol or tls_symbol['type'] != 6:
                        raise SystemExit(f'{m.name}: invalid TLS relocation symbol')
                    m.put32(off, A + tls_symbol['value'])
                continue
            symbolic = 16 in m.dyn or (m.dyn.get(30, 0) & 2)
            local = sym['bind'] == 0 or (sym['shndx'] != 0 and (symbolic or sym.get('visibility', 0) in (1, 2, 3)))
            if local:
                S = sym['value'] if sym['shndx'] == 0xfff1 else m.base + sym['value']
            else:
                S, _ = lookup(sym['name'], sym['bind'] == 2, sym['type'], sym['size'])
            if typ == R_386_32:
                m.put32(off, S + A)
            elif typ == R_386_PC32:
                m.put32(off, S + A - P)
            elif typ in (R_386_GLOB_DAT, R_386_JMP_SLOT):
                m.put32(off, S)
            else:
                raise SystemExit(f'{m.name}: relocation {typ} non gérée à {off:#x}')
        img = os.path.join(a.out, m.name)
        open(img, 'wb').write(m.data)

        text = m.secs['.text']
        entries = set()
        names = {}
        for s in m.symtab + m.dynsyms:
            if s['type'] == 2 and s['shndx'] != 0 and s['value']:
                names.setdefault(m.base + s['value'], s['name'])
                if not s['name'].endswith('.cold') and '.cold.' not in s['name']:
                    entries.add(m.base + s['value'])
        for tag in (12, 13):  # DT_INIT, DT_FINI
            if tag in m.dyn:
                entries.add(m.base + m.dyn[tag])
        init_array = []
        if 25 in m.dyn:
            init_array = [m.u32(m.dyn[25] + 4 * i) for i in range(m.dyn[27] // 4)]
        fini_array = []
        if 26 in m.dyn:
            fini_array = [m.u32(m.dyn[26] + 4 * i) for i in range(m.dyn[28] // 4)]
        entries.update(x for x in init_array + fini_array if x not in (0, 0xffffffff))
        # pointeurs vers .text présents dans les données (vtables, tables de fonctions)
        tlo, thi = m.base + text['addr'], m.base + text['addr'] + text['size']
        ptrs = set()
        for off, typ, si in m.rels():
            if typ in (R_386_RELATIVE, R_386_32):
                v = m.u32(off)
                in_text = text['addr'] <= off < text['addr'] + text['size']
                if tlo <= v < thi and not in_text:
                    ptrs.add(v)
        exports = {n: m.base + s['value'] for n, s in m.exports().items()}
        md = dict(name=m.name, modid=m.modid, base=m.base, image=img,
                  gotplt=m.base + m.dyn.get(3, 0), entry=m.base + m.e_entry,
                  loads=[dict(off=p[1], vaddr=m.base + p[2], filesz=p[4], memsz=p[5], flags=p[6]) for p in m.loads],
                  tls=(dict(off=m.tls[1], filesz=m.tls[4], memsz=m.tls[5], align=m.tls[7]) if m.tls else None),
                  init=(m.base + m.dyn[12]) if 12 in m.dyn else 0,
                  fini=(m.base + m.dyn[13]) if 13 in m.dyn else 0,
                  init_array=init_array, fini_array=fini_array,
                  text=[tlo, thi],
                  plt=[m.base + m.secs['.plt']['addr'], m.base + m.secs['.plt']['addr'] + m.secs['.plt']['size']] if '.plt' in m.secs else None,
                  entries=sorted(entries), data_ptrs=sorted(ptrs),
                  names={hex(k): v for k, v in names.items()},
                  exports=exports, needed=m.needed, rel_counts=n_by_type)
        meta_all['modules'].append(md)
        print(f'{m.name}: base {m.base:#x}, {len(entries)} entrées, {len(ptrs)} pointeurs de code en données, relocs {n_by_type}')
    meta_all['imports'] = [dict(name=n, addr=imports[n]) for n in order]
    meta_all['data_imports'] = list(data_imports.values())
    json.dump(meta_all, open(os.path.join(a.out, 'layout.json'), 'w'), indent=1)
    print(f'{len(order)} imports libc')


if __name__ == '__main__':
    main()
