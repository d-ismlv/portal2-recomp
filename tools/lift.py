#!/usr/bin/env python3
"""Strict integer i386 -> C AOT spike. Unsupported reachable instructions are errors.

The generated C contains one native function per requested export and direct C
branches for guest control flow. It never fetches or interprets guest opcodes.
Direct calls are followed transitively; indirect calls/tail jumps use a checked
dispatch table. SSE and x87 are outside this milestone.
"""
import argparse
import json
import hashlib
import shutil
from pathlib import Path
import re
import subprocess
import float_lift
import registry

REGS = {r: (r, 32, 0) for r in 'eax ecx edx ebx esp ebp esi edi'.split()}
for full, low, high, word in [('eax', 'al', 'ah', 'ax'), ('ebx', 'bl', 'bh', 'bx'),
                              ('ecx', 'cl', 'ch', 'cx'), ('edx', 'dl', 'dh', 'dx')]:
    REGS.update({low: (full, 8, 0), high: (full, 8, 8), word: (full, 16, 0)})
REGS.update({r[1:]: (r, 16, 0) for r in ['esp', 'ebp', 'esi', 'edi']})
CC = dict(o='O', no='NO', b='B', c='B', nae='B', ae='AE', nb='AE', nc='AE',
          e='E', z='E', ne='NE', nz='NE', be='BE', na='BE', a='A', nbe='A',
          s='S', ns='NS', p='P', pe='P', np='NP', po='NP', l='L', nge='L',
          ge='GE', nl='GE', le='LE', ng='LE', g='G', nle='G')


class Unsupported(ValueError):
    pass


def address(expr):
    """Accept only sums/differences of registers, scaled registers and constants."""
    tokens = re.findall(r'0x[0-9a-f]+|[a-z]+|[0-9]+|[-+*]', expr)
    if ''.join(tokens) != re.sub(r'\s+', '', expr):
        raise Unsupported(f'address expression {expr!r}')
    parts = re.split(r'(?=[+-])', ''.join(tokens))
    out = []
    for part in filter(None, parts):
        sign = '-' if part.startswith('-') else '+'
        factors = part.lstrip('+-').split('*')
        values = []
        if len(factors) > 2:
            raise Unsupported(expr)
        for atom in factors:
            if atom in REGS and REGS[atom][1] == 32:
                values.append('c->' + atom)
            elif atom == 'eiz':
                values.append('0u')
            else:
                try:
                    values.append(f'{int(atom, 0):#x}u')
                except ValueError:
                    raise Unsupported(expr) from None
        out.append(sign + '*'.join(values))
    if not out:
        raise Unsupported(expr)
    return '(uint32_t)(0u' + ''.join(out) + ')'


class Operand:
    def __init__(self, text):
        self.text = text
        self.reg = REGS.get(text)
        self.addr = None
        self.width = self.reg[1] if self.reg else None
        m = re.fullmatch(r'(?:(byte|word|dword|qword|xmmword) ptr )?(?:(gs|fs):)?\[(.*)\]', text)
        if m:
            if m[2] == 'fs': raise Unsupported('FS segment is not implemented')
            self.effective_addr = address(m[3])
            self.addr = f'(uint32_t)(rt_thread_pointer(c) + {self.effective_addr})' if m[2] == 'gs' else self.effective_addr
            self.width = {None: None, 'byte': 8, 'word': 16, 'dword': 32, 'qword': 64, 'xmmword': 128}[m[1]]
        self.imm = None
        if not self.reg and self.addr is None:
            try:
                self.imm = int(text, 0) & 0xffffffff
            except ValueError:
                raise Unsupported(f'operand {text!r}') from None

    def read(self):
        if self.reg:
            full, width, shift = self.reg
            return f'((c->{full} >> {shift}) & {((1 << width) - 1):#x}u)'
        if self.addr is not None and self.width:
            return f'{"RX" if self.width == 128 else "R" + str(self.width)}({self.addr})'
        if self.imm is not None:
            return f'{self.imm:#x}u'
        raise Unsupported(self.text)

    def write(self, value):
        if self.reg:
            full, width, shift = self.reg
            mask = ((1 << width) - 1) << shift
            if width == 32:
                return f'c->{full} = (uint32_t)({value});'
            return (f'c->{full} = (c->{full} & {~mask & 0xffffffff:#x}u) | '
                    f'(((uint32_t)({value}) << {shift}) & {mask:#x}u);')
        if self.addr is not None and self.width:
            return f'{"WX" if self.width == 128 else "W" + str(self.width)}({self.addr}, {value});'
        raise Unsupported(f'destination {self.text!r}')


def disassemble(image, base, objdump):
    result = subprocess.run([objdump, '-d', '--x86-asm-syntax=intel',
                             f'--adjust-vma={base:#x}', str(image)],
                            check=True, capture_output=True, text=True)
    insns = {}
    for line in result.stdout.splitlines():
        m = re.match(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2}\s+)+)\s*([a-z][a-z0-9]*)\s*(.*?)\s*$', line)
        if m:
            pc = int(m[1], 16)
            size = len(m[2].split())
            args = re.sub(r'\s*<.*$', '', m[4]).split('#', 1)[0].strip()
            insns[pc] = (pc + size, m[3], args)
    # LLVM sometimes emits LOCK as a separate disassembly line. It belongs to
    # the following instruction; the byte after LOCK is not a second entry.
    for pc, (nxt, op, args) in list(insns.items()):
        if op == 'lock' and not args and nxt in insns:
            end, following, operands = insns.pop(nxt)
            insns[pc] = (end, 'lock_' + following, operands)
    return insns


def string_instruction(op, args, nxt):
    repeat = None
    if op in ('rep', 'repe', 'repz', 'repne', 'repnz'):
        repeat = op
        fields = args.split(None, 1)
        if not fields:
            raise Unsupported('empty string prefix')
        op, args = fields[0], fields[1] if len(fields) > 1 else ''
    if op not in ('movsb', 'movsw', 'movsd', 'stosb', 'stosw', 'stosd',
                  'lodsb', 'lodsw', 'lodsd', 'scasb', 'scasw', 'scasd',
                  'cmpsb', 'cmpsw', 'cmpsd'):
        return None
    if 'xmm' in args:  # SSE MOVSD/CMPSD are distinct opcodes
        return None
    if any(segment in args for segment in ('gs:', 'fs:', 'cs:', 'ss:')):
        raise Unsupported('non-flat string segment')
    width = {'b': 8, 'w': 16, 'd': 32}[op[-1]]
    size = width // 8
    kind = op[:-1]
    body = []
    if kind == 'movs':
        body.append(f'a = R{width}(c->esi); W{width}(c->edi, a);')
    elif kind == 'stos':
        body.append(f'W{width}(c->edi, c->eax);')
    elif kind == 'lods':
        body.append(f'a = R{width}(c->esi); ' + Operand({8: 'al', 16: 'ax', 32: 'eax'}[width]).write('a'))
    elif kind == 'scas':
        body.append(f'f_sub(&f, c->eax, R{width}(c->edi), {width});')
    elif kind == 'cmps':
        body.append(f'f_sub(&f, R{width}(c->esi), R{width}(c->edi), {width});')
    step = f'(c->df ? (0u - {size}u) : {size}u)'
    if kind in ('movs', 'lods', 'cmps'):
        body.append(f'c->esi += {step};')
    if kind in ('movs', 'stos', 'scas', 'cmps'):
        body.append(f'c->edi += {step};')
    if repeat:
        body.append('--c->ecx;')
        if kind in ('cmps', 'scas'):
            body.append('if (' + ('f.zf' if repeat in ('repne', 'repnz') else '!f.zf') + ') break;')
        text = 'while (c->ecx) { ' + ' '.join(body) + ' }'
    else:
        text = ' '.join(body)
    return text + f' goto L{nxt:x};', [nxt]


def translate(pc, insn, entries=frozenset(), callees=None):
    if callees is None:
        callees = set()
    nxt, op, args = insn
    end = f'goto L{nxt:x};'
    if op in ('int3', 'ud2', 'hlt'):
        return f'rt_trap(c, {pc:#x}u, "{op}");', []
    string = string_instruction(op, args, nxt)
    if string is not None:
        return string
    if op == 'ret':
        adjustment = int(args, 0) if args else 0
        if not 0 <= adjustment <= 65535:
            raise Unsupported('ret stack adjustment')
        return f'c->flags = f; rt_return(c); c->esp += {adjustment}u; return;', []
    if op == 'call':
        target = Operand(args)
        if target.imm == nxt:
            return f'PUSH(c, {nxt:#x}u); {end}', [nxt]
        if target.imm is not None:
            callees.add(target.imm)
        elif target.width != 32:
            raise Unsupported('call operand width')
        return (f'a = {target.read()}; c->flags = f; rt_call(c, a, {nxt:#x}u); '
                f'f = c->flags; {end}'), [nxt]
    if op == 'jmp' or (op.startswith('j') and op[1:] in CC):
        target = Operand(args)
        if target.imm is None:
            if op != 'jmp' or target.width != 32:
                raise Unsupported('indirect control flow')
            return f'a = {target.read()}; c->flags = f; rt_dispatch(c, a); return;', []
        if target.imm in entries:
            callees.add(target.imm)
            branch = f'c->flags = f; rt_dispatch(c, {target.imm:#x}u); return;'
            successors = []
        else:
            branch = ('rt_guest_poll(); ' if target.imm <= pc else '') + f'goto L{target.imm:x};'
            successors = [target.imm]
        if op == 'jmp':
            return branch, successors
        return f'if (cond(&f, CC_{CC[op[1:]]})) {{ {branch} }} {end}', successors + [nxt]
    floating = float_lift.lower(op, args, nxt, Operand, Unsupported)
    if floating is not None:
        return floating
    operands = [Operand(x.strip()) for x in args.split(',')] if args else []
    if op in ('shl', 'shr', 'sar') and len(operands) == 1:
        operands.append(Operand('1'))
    if op == 'pushfd' and not operands:
        code = 'PUSH(c, get_eflags(c, &f));'
    elif op == 'popfd' and not operands:
        code = 'a = POP(c); set_eflags(c, &f, a);'
    elif op == 'cpuid' and not operands:
        code = 'rt_cpuid(c);'
    elif op == 'rdtsc' and not operands:
        code = '{ uint64_t ticks = rt_rdtsc(); c->eax = (uint32_t)ticks; c->edx = (uint32_t)(ticks >> 32); }'
    elif op == 'cdq' and not operands:
        code = 'c->edx = 0u - (c->eax >> 31);'
    elif op == 'cwde' and not operands:
        code = 'c->eax = sx(c->eax & 0xffffu, 16);'
    elif op == 'fxsave' and len(operands) == 1 and operands[0].addr:
        code = f'rt_fxsave(c, {operands[0].addr});'
    elif op == 'ldmxcsr' and len(operands) == 1:
        code = f'rt_ldmxcsr(c, {operands[0].read()});'
    elif op == 'stmxcsr' and len(operands) == 1:
        code = operands[0].write('c->mxcsr')
    elif op == 'pause' and not operands:
        code = 'rt_guest_poll(); __atomic_signal_fence(__ATOMIC_SEQ_CST);'
    elif op in ('cld', 'std') and not operands:
        code = f'c->df = {int(op == "std")}u;'
    elif op == 'leave' and not operands:
        code = 'c->esp = c->ebp; c->ebp = POP(c);'
    elif op == 'nop' or (op == 'lea' and operands[0].reg and operands[1].addr):
        code = '' if op == 'nop' else operands[0].write(operands[1].effective_addr)
    elif op in ('mov', 'movzx', 'movsx') and len(operands) == 2:
        dst, src = operands
        value = src.read()
        if op == 'movsx':
            if not src.width:
                raise Unsupported('movsx width')
            value = f'sx({value}, {src.width})'
        code = dst.write(value)
    elif op == 'push' and len(operands) == 1:
        src = operands[0]
        if src.width == 16:
            code = f'a = {src.read()}; c->esp -= 2; W16(c->esp, a);'
        else:
            code = f'a = {src.read()}; PUSH(c, a);'
    elif op == 'pop' and len(operands) == 1 and operands[0].reg:
        dst = operands[0]
        code = ('a = R16(c->esp); c->esp += 2; ' if dst.width == 16 else 'a = POP(c); ') + dst.write('a')
    elif op in ('add', 'adc', 'sub', 'sbb', 'cmp', 'and', 'or', 'xor', 'test', 'shl', 'shr', 'sar') and len(operands) == 2:
        dst, src = operands
        n = dst.width
        if n is None:
            raise Unsupported('missing operand width')
        code = f'a = {dst.read()}; b = {src.read()}; '
        if op in ('and', 'or', 'xor', 'test'):
            operator = {'and': '&', 'test': '&', 'or': '|', 'xor': '^'}[op]
            value = f'f_logic(&f, a {operator} b, {n})'
        else:
            value = f'f_{"sub" if op == "cmp" else op}(&f, a, b, {n})'
        code += f'r = {value}; '
        if op not in ('cmp', 'test'):
            code += dst.write('r')
    elif op in ('rol', 'ror', 'rcl', 'rcr') and len(operands) in (1, 2):
        dst = operands[0]; count = operands[1].read() if len(operands) == 2 else '1u'
        kind = {'rol': 0, 'ror': 1, 'rcl': 2, 'rcr': 3}[op]
        code = f'a = {dst.read()}; b = {count}; r = f_rotate(&f, a, b, {dst.width}, {kind}); ' + dst.write('r')
    elif op == 'bswap' and len(operands) == 1 and operands[0].width == 32:
        code = operands[0].write(f'__builtin_bswap32({operands[0].read()})')
    elif op in ('not', 'neg', 'inc', 'dec') and len(operands) == 1:
        dst = operands[0]
        value = '~a' if op == 'not' else f'f_{op}(&f, a, {dst.width})'
        code = f'a = {dst.read()}; r = {value}; ' + dst.write('r')
    elif op in ('lock_add', 'lock_sub', 'lock_and', 'lock_or', 'lock_xor', 'lock_adc', 'lock_sbb', 'lock_inc', 'lock_dec'):
        operation = op[5:]
        dst = operands[0]
        if dst.addr is None or dst.width != 32 or len(operands) != (1 if operation in ('inc','dec') else 2):
            raise Unsupported('atomic arithmetic operand form')
        value = '1u' if operation in ('inc', 'dec') else operands[1].read()
        kind = {'add': 0, 'adc': 0, 'inc': 0, 'sub': 1, 'sbb': 1, 'dec': 1, 'and': 2, 'or': 3, 'xor': 4}[operation]
        increment = 'b + f.cf' if operation in ('adc', 'sbb') else 'b'
        code = f'b = {value}; a = rt_atomic32(c, {dst.addr}, {increment}, {kind}); '
        if operation in ('and', 'or', 'xor'):
            operator = {'and': '&', 'or': '|', 'xor': '^'}[operation]
            code += f'f_logic(&f, a {operator} b, 32);'
        elif operation in ('inc', 'dec'):
            code += f'f_{operation}(&f, a, 32);'
        else:
            code += f'f_{operation}(&f, a, b, 32);'
    elif op in ('xadd', 'lock_xadd') and len(operands) == 2:
        dst, src = operands
        if dst.addr is None or dst.width != 32 or not src.reg or src.width != 32:
            raise Unsupported('xadd operand form')
        code = f'b = {src.read()}; a = rt_atomic32(c, {dst.addr}, b, 0); f_add(&f, a, b, 32); ' + src.write('a')
    elif op == 'xchg' and len(operands) == 2:
        dst, src = operands
        if dst.addr is not None and dst.width == 32 and src.reg and src.width == 32:
            code = f'a = {src.read()}; r = rt_exchange32(c, {dst.addr}, a); ' + src.write('r')
        elif dst.reg and src.reg and dst.width == src.width:
            code = f'a = {dst.read()}; b = {src.read()}; ' + dst.write('b') + ' ' + src.write('a')
        else:
            raise Unsupported('xchg operand form')
    elif op in ('cmpxchg8b', 'lock_cmpxchg8b') and len(operands) == 1:
        dst = operands[0]
        if dst.addr is None or dst.width != 64:
            raise Unsupported('cmpxchg8b operand form')
        code = (f'{{ uint64_t expected = ((uint64_t)c->edx << 32) | c->eax; '
                f'uint64_t observed = rt_compare_exchange64(c, {dst.addr}, expected, ((uint64_t)c->ecx << 32) | c->ebx); '
                'f.zf = observed == expected; if (!f.zf) { c->eax = (uint32_t)observed; c->edx = (uint32_t)(observed >> 32); } }')
    elif op in ('cmpxchg', 'lock_cmpxchg') and len(operands) == 2:
        dst, src = operands
        if dst.addr is None or dst.width != 32 or not src.reg or src.width != 32:
            raise Unsupported('cmpxchg operand form')
        code = (f'a = c->eax; b = {src.read()}; r = rt_compare_exchange32(c, {dst.addr}, a, b); '
                'f_sub(&f, a, r, 32); if (a != r) c->eax = r;')
    elif op.startswith('cmov') and op[4:] in CC and len(operands) == 2:
        dst, src = operands
        code = f'a = {src.read()}; if (cond(&f, CC_{CC[op[4:]]})) {{ {dst.write("a")} }}'
    elif op.startswith('set') and op[3:] in CC and len(operands) == 1:
        code = operands[0].write(f'cond(&f, CC_{CC[op[3:]]})')
    elif op in ('div', 'idiv') and len(operands) == 1 and operands[0].width == 32:
        code = f'rt_div32(c, {operands[0].read()}, {int(op == "idiv")}, {pc:#x}u);'
    elif op == 'mul' and len(operands) == 1 and operands[0].width == 32:
        code = (f'{{ uint64_t value = (uint64_t)c->eax * {operands[0].read()}; '
                'c->eax = (uint32_t)value; c->edx = (uint32_t)(value >> 32); f.cf = f.of = c->edx != 0; }')
    elif op in ('bsr', 'bsf') and len(operands) == 2 and operands[0].width == 32:
        dst, src = operands
        value = '31u - (uint32_t)__builtin_clz(a)' if op == 'bsr' else '(uint32_t)__builtin_ctz(a)'
        code = f'a = {src.read()}; f.zf = a == 0; if (a) {{ {dst.write(value)} }}'
    elif op in ('shld', 'shrd') and len(operands) == 3 and operands[0].width == 32:
        dst, src, count = operands
        value = '(a << r) | (b >> (32 - r))' if op == 'shld' else '(a >> r) | (b << (32 - r))'
        carry = '(a >> (32 - r)) & 1' if op == 'shld' else '(a >> (r - 1)) & 1'
        code = (f'a = {dst.read()}; b = {src.read()}; r = ({count.read()}) & 31; '
                f'if (r) {{ f.cf = {carry}; uint32_t result = {value}; '
                f'f.of = ((a ^ result) >> 31) & 1; szp(&f, result, 32); {dst.write("result")} }}')
    elif op == 'imul' and len(operands) in (2, 3):
        dst = operands[0]
        lhs, rhs = (dst, operands[1]) if len(operands) == 2 else operands[1:]
        code = f'a = {lhs.read()}; b = {rhs.read()}; r = f_imul(&f, a, b, {dst.width}); ' + dst.write('r')
    elif op == 'imul' and len(operands) == 1 and operands[0].width == 32:
        code = (f'product = (int64_t)(int32_t)c->eax * (int64_t)(int32_t){operands[0].read()}; '
                'c->eax = (uint32_t)product; c->edx = (uint32_t)((uint64_t)product >> 32); '
                'f.cf = f.of = product != (int64_t)(int32_t)c->eax;')
    else:
        raise Unsupported(f'{op} {args}')
    return code + ' ' + end, [nxt]


def lift(entry, insns, entries=frozenset(), callees=None):
    pending = [entry]
    translated = {}
    while pending:
        pc = pending.pop()
        if pc in translated:
            continue
        if pc not in insns:
            raise Unsupported(f'no instruction at {pc:#x}')
        try:
            code, successors = translate(pc, insns[pc], entries - {entry}, callees)
        except (Unsupported, IndexError) as exc:
            raise Unsupported(f'{pc:#x}: {exc}') from exc
        translated[pc] = code
        pending.extend(successors)
    lines = [f'static void fn_{entry:x}(CPU *c) {{',
             '    FL f = c->flags; uint32_t a, b, r; int64_t product;',
             '    (void)f; (void)a; (void)b; (void)r; (void)product;', f'    goto L{entry:x};']
    lines.extend(f'L{pc:x}: {code}' for pc, code in sorted(translated.items()))
    lines.append('}')
    return '\n'.join(lines), len(translated)


# Keep this list synchronized with runtime/shims.c. Unknown imports fail closed.
# New runtime files declare their names on `// SHIMS: a b c` lines instead (see declared_shims).
_CORE_SHIMS = frozenset('pthread_create pthread_join pthread_attr_init pthread_attr_setstacksize pthread_attr_destroy pthread_getschedparam pthread_setschedparam pthread_exit pthread_equal pthread_cond_wait pthread_cond_timedwait '
                  'feof ferror clearerr fileno fileno_unlocked ftell ftello ftello64 fseek fseeko fseeko64 rewind strrchr '
                  'memset memcpy memmove memcmp strlen strcpy stpcpy strncpy strcmp '
                  'malloc calloc realloc free malloc_usable_size __cxa_atexit abort puts snprintf vsnprintf pthread_self '
                  'pthread_mutex_init pthread_mutex_lock pthread_mutex_unlock '
                  'pthread_mutex_trylock pthread_mutex_destroy pthread_mutexattr_init '
                  'pthread_mutexattr_settype pthread_mutexattr_destroy __errno_location getenv '
                  '___tls_get_addr __tls_get_addr gettimeofday usleep fopen fopen64 fclose fgets fread fwrite access opendir closedir '
                  'readdir strpbrk strstr strcasestr strchr strcasecmp strncasecmp strdup tolower toupper getppid strtol strtoul __isoc99_sscanf sscanf strcat strncat strtod strerror pthread_key_create __pthread_key_create pthread_key_delete pthread_setspecific pthread_getspecific pthread_once pthread_cond_init pthread_cond_destroy pthread_cond_signal pthread_cond_broadcast dlopen dlsym dlerror clock_gettime sysconf mmap mmap64 munmap mprotect madvise syscall fputs fputc putc fgetc getc fflush printf fprintf vprintf vfprintf open open64 __open_2 __open64_2 close read write lseek lseek64 chdir getcwd readlink stat stat64 lstat lstat64 fstat fstat64 __xstat __xstat64 __lxstat __lxstat64 __fxstat __fxstat64'.split())


def declared_shims(runtime_dir=Path(__file__).resolve().parent.parent / 'runtime'):
    """Names declared by runtime/*.c on lines starting with `// SHIMS:`."""
    names = set()
    for path in sorted(runtime_dir.glob('*.c')):
        for line in path.read_text().splitlines():
            if line.startswith('// SHIMS:'):
                names.update(line[len('// SHIMS:'):].split())
    return names


SHIMS = _CORE_SHIMS | frozenset(declared_shims())

def resolve_export(layout, spec):
    if ':symbol:' in spec:
        name, symbol = spec.split(':symbol:')
        mod = next((m for m in layout['modules'] if m['name'] == name), None)
        matches = [int(address, 16) for address, value in mod['names'].items() if value == symbol] if mod else []
        if len(matches) != 1:
            raise Unsupported(f'expected one function symbol for {spec!r}, found {len(matches)}')
        return matches[0]
    if ':code:' in spec:
        name, offset = spec.split(':code:')
        mod = next((m for m in layout['modules'] if m['name'] == name), None)
        try:
            address = mod['base'] + int(offset, 0) if mod else -1
        except ValueError:
            raise Unsupported(f'invalid code offset {spec!r}') from None
        if not mod or not any(s['flags'] & 1 and s['vaddr'] <= address < s['vaddr'] + s['filesz']
                              for s in mod['loads']):
            raise Unsupported(f'code root outside executable segments: {spec!r}')
        return address
    if ':init:' in spec:
        name, index = spec.split(':init:')
        mod = next((m for m in layout['modules'] if m['name'] == name), None)
        if mod is None:
            raise Unsupported(f'unknown module {name!r}')
        if index == 'dt':
            return mod['init']
        try:
            return mod['init_array'][int(index)]
        except (ValueError, IndexError):
            raise Unsupported(f'unknown constructor {spec!r}') from None
    matches = [m for m in layout['modules'] if spec in m['exports']]
    if len(matches) != 1:
        raise Unsupported(f'export {spec!r}: expected one definition, found {len(matches)}')
    return matches[0]['exports'][spec]


def generate(layout, exports, objdump, allow_unimplemented=False, disasm_cache=None):
    code = ['/* Generated by tools/lift.py. Do not edit. */', '#include "rt.h"']
    selected = [(name, resolve_export(layout, name)) for name in exports]
    insns = {}
    loaded = set()
    def ensure_module(entry):
        for mod in layout['modules']:
            if any(s['flags'] & 1 and s['vaddr'] <= entry < s['vaddr'] + s['filesz'] for s in mod['loads']):
                if mod['name'] not in loaded:
                    decoded = None
                    cache_path = None
                    if disasm_cache is not None:
                        tool = Path(shutil.which(objdump) or objdump).resolve()
                        identity = (f'parser-v1:{mod["base"]}:{tool}:'
                                    f'{tool.stat().st_mtime_ns}:{tool.stat().st_size}').encode()
                        digest = hashlib.sha256(identity + Path(mod['image']).read_bytes()).hexdigest()
                        cache_path = Path(disasm_cache) / (digest + '.json')
                        if cache_path.exists():
                            decoded = {int(pc): tuple(value) for pc,value in json.loads(cache_path.read_text()).items()}
                    if decoded is None:
                        decoded = disassemble(mod['image'], mod['base'], objdump)
                        if cache_path is not None:
                            cache_path.parent.mkdir(parents=True, exist_ok=True)
                            cache_path.write_text(json.dumps(decoded))
                    insns.update(decoded)
                    loaded.add(mod['name'])
                return
        raise Unsupported(f'entry {entry:#x} lies outside executable segments')
    imports = {item['addr']: item['name'] for item in layout['imports']}
    entries = {e for m in layout['modules'] for e in m['entries']}
    entries.update(entry for _, entry in selected)
    entries.update(imports)
    pending = [entry for _, entry in selected]
    compiled = set()
    used_imports = set()
    unimplemented = set()
    total = 0
    while pending:
        entry = pending.pop()
        if entry in compiled or entry in used_imports:
            continue
        if entry in imports:
            name = imports[entry]
            if name not in SHIMS:
                if not allow_unimplemented:
                    raise Unsupported(f'import {name!r} at {entry:#x} has no shim')
                unimplemented.add(name)
            used_imports.add(entry)
            continue
        ensure_module(entry)
        callees = set()
        body, count = lift(entry, insns, entries, callees)
        code.append(body)
        compiled.add(entry)
        total += count
        pending.extend(sorted(callees))
    print(f'{len(selected)} roots, {len(compiled)} native functions, '
          f'{total} instructions, {len(used_imports)} direct imports')
    if unimplemented:
        print('Runtime traps for unimplemented imports: ' + ', '.join(sorted(unimplemented)))
    code.append('gfn rt_lookup(uint32_t target) { switch (target) {')
    for entry in sorted(compiled):
        code.append(f'case {entry:#x}u: return fn_{entry:x};')
    code.extend(['default: return NULL; } }', 'uint32_t rt_export(const char *name) {'])
    for name, entry in selected:
        code.append(f'if (!strcmp(name, {json.dumps(name)})) return {entry:#x}u;')
    code.extend(['return 0; }', 'const char *rt_import_name(uint32_t target) { switch (target) {'])
    for addr, name in sorted(imports.items()):
        code.append(f'case {addr:#x}u: return {json.dumps(name)};')
    code.extend(['default: return NULL; } }', 'int rt_load_images(const char *directory) {'])
    for mod in layout['modules']:
        for seg in mod['loads']:
            code.append(f'if (rt_load_segment(directory, {json.dumps(mod["name"])}, '
                        f'{seg["off"]}u, {seg["vaddr"]:#x}u, {seg["filesz"]}u, {seg["memsz"]}u)) return -1;')
        tls = mod.get('tls')
        if tls:
            code.append(f'if (rt_register_tls(directory, {json.dumps(mod["name"])}, {mod["modid"]}u, '
                        f'{tls["off"]}u, {tls["filesz"]}u, {tls["memsz"]}u, {tls["align"]}u)) return -1;')
    for item in layout.get('data_imports', []):
        code.append(f'if (rt_init_import_data({json.dumps(item["name"])}, {item["addr"]:#x}u, {item["size"]}u)) return -1;')
    code.extend(['return 0; }', ''])
    code.append(registry.generate(layout))
    return '\n'.join(code)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--layout', required=True, type=Path)
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--objdump', default='objdump')
    ap.add_argument('--allow-unimplemented-imports', action='store_true',
                    help='emit runtime traps for missing shims instead of rejecting direct imports')
    ap.add_argument('exports', nargs='+')
    args = ap.parse_args()
    try:
        code = generate(json.loads(args.layout.read_text()), args.exports, args.objdump, args.allow_unimplemented_imports)
    except Unsupported as exc:
        ap.exit(1, f'lift: {exc}\n')
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(code)


if __name__ == '__main__':
    main()
