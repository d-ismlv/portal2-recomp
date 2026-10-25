// Independent arithmetic ground truth uses widened values, signed ranges and
// low-nibble sums/borrows rather than duplicating the runtime flag formulas.
static void check_arithmetic_flags(void) {
    for (unsigned a = 0; a < 256; ++a) for (unsigned b = 0; b < 256; ++b) {
        for (unsigned carry = 0; carry < 2; ++carry) {
            FL f = {0}; f.cf = carry;
            unsigned sum = a + b + carry;
            int signed_sum = (int8_t)a + (int8_t)b + (int)carry;
            uint32_t result = f_adc(&f, a, b, 8);
            assert(result == (sum & 255) && f.cf == (sum > 255));
            assert(f.of == (signed_sum < -128 || signed_sum > 127));
            assert(f.af == (((a & 15) + (b & 15) + carry) > 15));
            assert(f.zf == (result == 0) && f.sf == (result >> 7));
            f.cf = carry;
            int difference = (int)a - (int)b - (int)carry;
            int signed_difference = (int8_t)a - (int8_t)b - (int)carry;
            result = f_sbb(&f, a, b, 8);
            assert(result == ((uint32_t)difference & 255) && f.cf == (difference < 0));
            assert(f.of == (signed_difference < -128 || signed_difference > 127));
            assert(f.af == ((a & 15) < (b & 15) + carry));
        }
    }
}

static void check_cpu_runtime(void) {
    CPU c; rt_cpu_reset(&c);
    assert(c.fcw == 0x37f && c.mxcsr == 0x1f80 && get_eflags(&c, &c.flags) == 0x202);
    set_eflags(&c, &c.flags, 0x00240cd7);
    assert((get_eflags(&c, &c.flags) & 0x00240cd7) == 0x00240cd7);
    c.eax = 0; rt_cpuid(&c);
    assert(c.eax == 1 && c.ebx == 0x756e6547 && c.edx == 0x49656e69 && c.ecx == 0x6c65746e);
    c.eax = 1; rt_cpuid(&c);
    assert(c.edx & (1u << 26));
    uint64_t t1 = rt_rdtsc(), t2 = rt_rdtsc(); assert(t2 >= t1);
    c.eax = 0; c.edx = 1; rt_div32(&c, 3, 0, 0);
    assert(c.eax == 1431655765u && c.edx == 1);
    c.eax = (uint32_t)-17; c.edx = UINT32_MAX; rt_div32(&c, 5, 1, 0);
    assert((int32_t)c.eax == -3 && (int32_t)c.edx == -2);
    rt_cpu_reset(&c); fpush(&c, -1.5);
    c.xmm[6].q[0] = 0x1122334455667788;
    c.xmm[6].q[1] = 0x8877665544332211;
    rt_fxsave(&c, 0x20006000);
    assert(R16(0x20006000) == 0x37f && (R16(0x20006002) >> 11) == 7);
    assert(R8(0x20006004) == 0x80 && R32(0x20006018) == 0x1f80);
    assert(R32(0x2000601c) == 0xffff);
    assert(R64(0x20006020) == UINT64_C(0xc000000000000000) && R16(0x20006028) == 0xbfff);
    assert(R64(0x200060a0 + 16 * 6) == 0x1122334455667788);
    assert(R64(0x200060a8 + 16 * 6) == 0x8877665544332211);
    rt_ldmxcsr(&c, 0x9fc0); rt_fxsave(&c, 0x20006000);
    assert(R32(0x20006018) == 0x9fc0);
    assert(fpop(&c) == -1.5 && c.ftags == 0 && c.ftop == 0);
}

static void check_rotations(void) {
    for (unsigned value=0; value<256; ++value) for (unsigned count=0; count<32; ++count)
    for (unsigned carry=0; carry<2; ++carry) for (int kind=0; kind<4; ++kind) {
        FL flags={.cf=carry,.zf=1,.sf=1,.pf=0x35,.af=1};
        unsigned expected=value, expected_carry=carry;
        for (unsigned step=0; step<count; ++step) {
            unsigned next;
            if (kind==0 || kind==2) {
                next=expected>>7;
                expected=((expected<<1)|(kind==0 ? next : expected_carry))&255;
            } else {
                next=expected&1;
                expected=(expected>>1)|((kind==1 ? next : expected_carry)<<7);
            }
            expected_carry=next;
        }
        assert(f_rotate(&flags,value,count,8,kind)==expected);
        assert(flags.cf==expected_carry && flags.zf==1 && flags.sf==1 && flags.pf==0x35 && flags.af==1);
        if (count==1) assert(flags.of == (kind==0||kind==2 ? (expected>>7)^expected_carry : ((expected>>7)^(expected>>6))&1));
    }
}
