"""SSE/x87 lowering used by the constructor milestone (x87 modeled in double)."""
import re


def lower(op, args, nxt, Operand, Unsupported):
    def xmm(text):
        m = re.fullmatch(r'xmm([0-7])', text)
        return int(m[1]) if m else None

    def operand(text):
        index = xmm(text)
        if index is not None: return f'c->xmm[{index}]'
        value = Operand(text)
        if value.width == 64: return f'(XMM){{.q={{{value.read()},0}}}}'
        if value.width == 32: return f'(XMM){{.d={{{value.read()},0,0,0}}}}'
        return value.read()

    def scalar(text, size):
        index = xmm(text)
        if index is not None:
            return f'c->xmm[{index}].{"g" if size == 64 else "f"}[0]'
        mem = Operand(text)
        if mem.addr is None or mem.width != size:
            raise Unsupported('scalar floating operand')
        return f'{"RD" if size == 64 else "RF"}({mem.addr})'

    def st(text):
        if text == 'st':
            return 'ST(0)'
        m = re.fullmatch(r'st\(([0-7])\)', text)
        if not m:
            raise Unsupported(f'x87 register {text!r}')
        return f'ST({m[1]})'

    parts = [x.strip() for x in args.split(',')] if args else []
    code = None
    if op in ('movd', 'movq') and len(parts) == 2 and any(xmm(p) is not None for p in parts):
        dst, src = parts; width = 32 if op == 'movd' else 64
        field = 'd' if width == 32 else 'q'
        value = f'c->xmm[{xmm(src)}].{field}[0]' if xmm(src) is not None else Operand(src).read()
        if xmm(dst) is not None:
            code = f'{{ uint64_t value = {value}; c->xmm[{xmm(dst)}] = (XMM){{0}}; c->xmm[{xmm(dst)}].{field}[0] = value; }}'
        else:
            code = Operand(dst).write(value)
    elif op in ('movaps', 'movups', 'movapd', 'movupd', 'movdqa', 'movdqu') and len(parts) == 2:
        dst, src = parts
        if xmm(dst) is not None:
            code = f'c->xmm[{xmm(dst)}] = {operand(src)};'
        else:
            mem = Operand(dst)
            if mem.width != 128 or xmm(src) is None:
                raise Unsupported('packed move operands')
            code = mem.write(operand(src))
        if op in ('movaps', 'movapd', 'movdqa'):
            for text in parts:
                if xmm(text) is None:
                    address = Operand(text).addr
                    code = f'if (({address}) & 15) rt_trap(c, c->eip, "unaligned SSE move"); ' + code
    elif op in ('movsd', 'movss') and len(parts) == 2 and any(xmm(p) is not None for p in parts):
        dst, src = parts; size = 64 if op == 'movsd' else 32
        field = 'q' if size == 64 else 'd'
        value = f'c->xmm[{xmm(src)}].{field}[0]' if xmm(src) is not None else Operand(src).read()
        if xmm(dst) is not None:
            code = f'{{ uint64_t value = {value}; '
            if xmm(src) is None:
                code += f'c->xmm[{xmm(dst)}] = (XMM){{0}}; '
            code += f'c->xmm[{xmm(dst)}].{field}[0] = value; }}'
        else:
            code = Operand(dst).write(value)
    elif op in ('pxor', 'xorps', 'xorpd', 'pand', 'pandn', 'por', 'andps', 'andpd', 'orps', 'orpd') and len(parts) == 2:
        dst, src = parts
        if xmm(dst) is None:
            raise Unsupported('packed destination')
        operator = '^' if op in ('pxor', 'xorps', 'xorpd') else '|' if op in ('por', 'orps', 'orpd') else '&'
        negate = '~' if op == 'pandn' else ''
        code = (f'{{ XMM lhs = {operand(dst)}, rhs = {operand(src)}; '
                f'for (unsigned lane = 0; lane < 2; ++lane) '
                f'c->xmm[{xmm(dst)}].q[lane] = {negate}lhs.q[lane] {operator} rhs.q[lane]; }}')
    elif op in ('paddq', 'psubq') and len(parts) == 2 and xmm(parts[0]) is not None:
        dst, src = parts
        operator = '+' if op == 'paddq' else '-'
        code = (f'{{ XMM lhs = {operand(dst)}, rhs = {operand(src)}; '
                f'for (unsigned lane = 0; lane < 2; ++lane) c->xmm[{xmm(dst)}].q[lane] = lhs.q[lane] {operator} rhs.q[lane]; }}')
    elif op in ('psrlw','psrld','psrlq','psllw','pslld','psllq','psraw','psrad') and len(parts)==2 and xmm(parts[0]) is not None:
        dst, source = parts
        width={'w':16,'d':32,'q':64}[op[-1]]; field={16:'w',32:'d',64:'q'}[width]
        try: immediate=int(source,0)
        except ValueError: immediate=None
        count=f'{immediate}u' if immediate is not None else f'({operand(source)}).q[0]'
        arithmetic=op.startswith('psra'); left=op.startswith('psll')
        shifted=f'(int{width}_t)value.{field}[lane] >> count' if arithmetic else f'value.{field}[lane] {"<<" if left else ">>"} count'
        saturated=f'0u - (uint{width}_t)((value.{field}[lane] >> {width-1}) & 1)' if arithmetic else '0'
        code=(f'{{ XMM value={operand(dst)}; uint64_t count={count}; '
              f'for(unsigned lane=0;lane<{128//width};++lane) c->xmm[{xmm(dst)}].{field}[lane] = '
              f'count >= {width} ? {saturated} : {shifted}; }}')
    elif op == 'punpckldq' and len(parts) == 2 and xmm(parts[0]) is not None:
        dst, src = parts
        code = (f'{{ XMM lhs = {operand(dst)}, rhs = {operand(src)}; '
                f'c->xmm[{xmm(dst)}] = (XMM){{.d = {{lhs.d[0], rhs.d[0], lhs.d[1], rhs.d[1]}}}}; }}')
    elif op in ('addpd','subpd','mulpd','divpd','addps','subps','mulps','divps','minpd','maxpd','minps','maxps') and len(parts)==2 and xmm(parts[0]) is not None:
        dst,source=parts; field='g' if op.endswith('pd') else 'f'; lanes=2 if field=='g' else 4
        if op[:3] in ('min','max'):
            comparison='<' if op.startswith('min') else '>'
            value=f'lhs.{field}[lane] {comparison} rhs.{field}[lane] ? lhs.{field}[lane] : rhs.{field}[lane]'
        else:
            operator={'add':'+','sub':'-','mul':'*','div':'/'}[op[:3]]
            value=f'lhs.{field}[lane] {operator} rhs.{field}[lane]'
        code=(f'{{ XMM lhs={operand(dst)}, rhs={operand(source)}; '
              f'for(unsigned lane=0;lane<{lanes};++lane) c->xmm[{xmm(dst)}].{field}[lane] = {value}; }}')
    elif re.fullmatch(r'cmp(eq|lt|le|unord|neq|nlt|nle|ord)(pd|ps|sd|ss)',op) and len(parts)==2 and xmm(parts[0]) is not None:
        dst,source=parts; match=re.fullmatch(r'cmp(eq|lt|le|unord|neq|nlt|nle|ord)(pd|ps|sd|ss)',op)
        predicate,suffix=match.groups(); wide=suffix in ('pd','sd'); scalar_op=suffix in ('sd','ss')
        field,mask=('g','q') if wide else ('f','d'); lanes=1 if scalar_op else 2 if wide else 4
        rhs=f'(XMM){{.{field}={{{scalar(source,64 if wide else 32)}}}}}' if scalar_op else operand(source)
        compare={'eq':'a==b','lt':'a<b','le':'a<=b','unord':'isnan(a)||isnan(b)',
                 'neq':'a!=b','nlt':'!(a<b)','nle':'!(a<=b)','ord':'!isnan(a)&&!isnan(b)'}[predicate]
        full='UINT64_MAX' if wide else 'UINT32_MAX'
        code=(f'{{ XMM lhs={operand(dst)}, rhs={rhs}; for(unsigned lane=0;lane<{lanes};++lane) {{ '
              f'double a=lhs.{field}[lane], b=rhs.{field}[lane]; c->xmm[{xmm(dst)}].{mask}[lane]=({compare}) ? {full} : 0; }} }}')
    elif op in ('cvttpd2dq','cvtpd2dq','cvttps2dq','cvtps2dq') and len(parts)==2 and xmm(parts[0]) is not None:
        dst,source=parts; field='g' if 'pd' in op else 'f'; lanes=2 if field=='g' else 4
        value=f'value.{field}[lane]' if op.startswith('cvtt') else f'rt_sse_round(c,value.{field}[lane])'
        code=(f'{{ XMM value={operand(source)}; c->xmm[{xmm(dst)}]=(XMM){{0}}; '
              f'for(unsigned lane=0;lane<{lanes};++lane) c->xmm[{xmm(dst)}].i[lane]=cvt_trunc32({value}); }}')
    elif op in ('cvtdq2pd','cvtdq2ps') and len(parts)==2 and xmm(parts[0]) is not None:
        dst,source=parts; field='g' if op.endswith('pd') else 'f'; lanes=2 if field=='g' else 4
        code=(f'{{ XMM value={operand(source)}; '
              f'for(unsigned lane=0;lane<{lanes};++lane) c->xmm[{xmm(dst)}].{field}[lane]=value.i[lane]; }}')
    elif op in ('addsd', 'subsd', 'mulsd', 'divsd', 'addss', 'subss', 'mulss', 'divss') and len(parts) == 2:
        dst, src = parts
        if xmm(dst) is None:
            raise Unsupported('scalar SSE destination')
        size = 64 if op.endswith('sd') else 32
        operator = {'add': '+', 'sub': '-', 'mul': '*', 'div': '/'}[op[:3]]
        code = f'{scalar(dst, size)} = {scalar(dst, size)} {operator} {scalar(src, size)};'
    elif op in ('ucomisd', 'comisd', 'ucomiss', 'comiss') and len(parts) == 2:
        size = 64 if op.endswith('sd') else 32
        code = f'f_comi(&f, {scalar(parts[0], size)}, {scalar(parts[1], size)});'
    elif op in ('cvtsi2sd', 'cvtsi2ss') and len(parts) == 2 and xmm(parts[0]) is not None:
        size = 64 if op.endswith('sd') else 32
        code = f'{scalar(parts[0], size)} = (int32_t){Operand(parts[1]).read()};'
    elif op in ('cvtss2sd', 'cvtsd2ss') and len(parts) == 2 and xmm(parts[0]) is not None:
        size = 64 if op.endswith('sd') else 32
        code = f'{scalar(parts[0], size)} = {scalar(parts[1], 96 - size)};'
    elif op in ('cvttsd2si', 'cvtsd2si', 'cvttss2si', 'cvtss2si') and len(parts) == 2:
        size = 64 if 'sd' in op else 32
        function = 'cvt_trunc32' if op.startswith('cvtt') else 'cvt_round32'
        code = Operand(parts[0]).write(f'{function}({scalar(parts[1], size)})')
    elif op in ('fld1', 'fldz', 'fldpi', 'fldln2', 'fldl2e') and not parts:
        value = {'fld1': '1.0', 'fldz': '0.0', 'fldpi': '3.14159265358979323846',
                 'fldln2': '0.69314718055994530942', 'fldl2e': '1.44269504088896340736'}[op]
        code = f'fpush(c, {value});'
    elif op in ('fld', 'fild') and len(parts) == 1:
        if parts[0].startswith('st'):
            value = st(parts[0])
        elif op == 'fild':
            mem = Operand(parts[0])
            value = f'(int{mem.width}_t){mem.read()}'
        else:
            mem = Operand(parts[0]); value = scalar(parts[0], mem.width)
        code = f'{{ double value = {value}; fpush(c, value); }}'
    elif op in ('fist', 'fistp', 'fisttp') and len(parts) == 1:
        mem = Operand(parts[0])
        if mem.addr is None or mem.width not in (16, 32, 64):
            raise Unsupported('x87 integer store format')
        value = 'ST(0)' if op == 'fisttp' else 'rt_x87_round(c, ST(0))'
        converter = 'cvt_trunc64' if mem.width == 64 else 'cvt_trunc32'
        code = mem.write(f'{converter}({value})')
        if op != 'fist': code += ' fpop(c);'
    elif op in ('fst', 'fstp') and len(parts) == 1:
        if parts[0].startswith('st'):
            code = f'{st(parts[0])} = ST(0);'
        else:
            mem = Operand(parts[0])
            if mem.width not in (32, 64) or mem.addr is None:
                raise Unsupported('x87 store format')
            code = f'{"WD" if mem.width == 64 else "WF"}({mem.addr}, ST(0));'
        if op == 'fstp': code += ' fpop(c);'
    elif op in ('fadd', 'fsub', 'fsubr', 'fmul', 'fdiv', 'fdivr',
                'faddp', 'fsubp', 'fsubrp', 'fmulp', 'fdivp', 'fdivrp'):
        pop = op.endswith('p'); operation = op[:-1] if pop else op
        if len(parts) == 2:
            dst, src = st(parts[0]), st(parts[1])
        elif len(parts) == 1:
            dst = 'ST(0)'
            if parts[0].startswith('st'): src = st(parts[0])
            else:
                mem = Operand(parts[0]); src = scalar(parts[0], mem.width)
        elif not parts and pop:
            dst, src = 'ST(1)', 'ST(0)'
        else:
            raise Unsupported('x87 arithmetic form')
        operator = {'fadd': '+', 'fsub': '-', 'fsubr': '-', 'fmul': '*', 'fdiv': '/', 'fdivr': '/'}[operation]
        lhs, rhs = (src, dst) if operation.endswith('r') else (dst, src)
        code = f'{dst} = {lhs} {operator} {rhs};'
        if pop: code += ' fpop(c);'
    elif op == 'fxch' and len(parts) <= 1:
        other = st(parts[0]) if parts else 'ST(1)'
        code = f'{{ double value = ST(0); ST(0) = {other}; {other} = value; }}'
    elif op in ('fchs', 'fabs', 'fsqrt') and not parts:
        value = {'fchs': '-ST(0)', 'fabs': 'fabs(ST(0))', 'fsqrt': 'sqrt(ST(0))'}[op]
        code = f'ST(0) = {value};'
    if code is None:
        return None
    return code + f' goto L{nxt:x};', [nxt]
