// Semantics of instructions added after the full-game coverage audit. The
// guest code is real i386 machine code (tests/lifter_programs.s), lifted by
// tools/lift.py; expected values come from the architecture manuals, not from
// the runtime helpers under test.
#include "checks.h"
#include <math.h>

uint32_t lifter_entry(const char *name);
#define IN 0x20018000u
#define OUT 0x20019000u

static CPU run_named(const char *name) {
    uint32_t entry = lifter_entry(name);
    assert(entry);
    CPU c; rt_cpu_reset(&c); c.esp = 0x20010000;
    rt_call(&c, entry, RT_RETURN_SENTINEL);
    assert(c.esp == 0x20010000 && c.eip == RT_RETURN_SENTINEL);
    return c;
}
static void clear(void) { memset(GPTR(IN), 0, 0x2000); }
static float f32(uint32_t a) { return RF(a); }
static uint32_t float_bits(float v) { uint32_t b; memcpy(&b, &v, 4); return b; }
static void put_floats(uint32_t a, float x, float y, float z, float w) {
    WF(a, x); WF(a + 4, y); WF(a + 8, z); WF(a + 12, w);
}
static void expect_floats(uint32_t a, float x, float y, float z, float w) {
    assert(f32(a) == x && f32(a + 4) == y && f32(a + 8) == z && f32(a + 12) == w);
}

RT_CHECK(lifter_bit_tests) {
    clear();
    W32(IN + 0xc, 0); W32(IN + 0x10, 0x8); W32(IN + 0x14, 0x100);
    run_named("lf_bits");
    assert(R8(OUT) == 1 && R8(OUT + 1) == 0 && R8(OUT + 2) == 1);
    assert(R32(OUT + 4) == 0x80000003u && R32(OUT + 8) == 0x80000002u && R32(OUT + 0xc) == 0x80000012u);
    assert(R32(IN + 0xc) == 0x80000000u && R8(OUT + 3) == 0);       // bit string reaches the previous dword
    assert(R32(IN + 0x14) == 0 && R8(OUT + 0x10) == 1);              // and the following one
    assert(R32(IN + 0x10) == 0x8 && R8(OUT + 0x11) == 1);            // immediate index is modulo 32
}

RT_CHECK(lifter_narrow_integer) {
    clear();
    CPU c = run_named("lf_narrow");
    assert(R32(OUT + 0x24) == 0xaaaa0f00u && R8(OUT + 0x20) == 1);  // MUL r8, CF=OF=AH!=0
    assert(R32(OUT + 0x28) == 0x0000fffau && R8(OUT + 0x21) == 0);  // IMUL r8 fits in AL
    assert(R32(OUT + 0x2c) == 0x1111020eu);                         // DIV r8: AH=rem, AL=quot
    assert(R32(OUT + 0x30) == 0xfffffffdu && (R32(OUT + 0x34) & 0xffff) == 0xffff);  // IDIV r16
    assert(R32(OUT + 0x38) == 0x1234ff80u);                         // CBW
    assert(R8(OUT + 0x3c) == 1 && R8(OUT + 0x3d) == 1 && R8(OUT + 0x3e) == 1 && R8(OUT + 0x3f) == 1);
    assert(R32(OUT + 0x40) == 0x9700u);                             // LAHF: SF AF PF 1 CF
    (void)c;
}

RT_CHECK(lifter_sse_shuffles) {
    clear();
    put_floats(IN + 0x100, 1, 2, 3, 4); put_floats(IN + 0x110, 5, 6, 7, 8);
    put_floats(IN + 0x120, -1, 2, -3, 0);
    run_named("lf_sse");
    expect_floats(OUT + 0x100, 4, 3, 6, 5);
    expect_floats(OUT + 0x110, 1, 5, 2, 6);
    expect_floats(OUT + 0x120, 3, 7, 4, 8);
    expect_floats(OUT + 0x130, 7, 8, 5, 6);
    assert(f32(OUT + 0x140) == 5 && f32(OUT + 0x144) == 6 && f32(OUT + 0x148) == 7 && f32(OUT + 0x14c) == 8);
    assert(R32(OUT + 0x150) == 5);
    float in[4] = {-1, 2, -3, 0}, value[4] = {1, 2, 3, 4};
    for (int i = 0; i < 4; ++i) assert(R32(OUT + 0x160 + 4 * i) == (~float_bits(in[i]) & float_bits(value[i])));
    expect_floats(OUT + 0x170, 5, 6, 7, 8);
}

RT_CHECK(lifter_sse_math) {
    clear();
    WF(IN + 0x200, 16); WD(IN + 0x208, 2.25); WF(IN + 0x210, NAN); WF(IN + 0x214, 3);
    WD(IN + 0x218, -0.0); WD(IN + 0x220, 0.0); put_floats(IN + 0x230, 4, 16, 0.25f, 1);
    run_named("lf_sse_math");
    assert(f32(OUT + 0x200) == 4 && RD(OUT + 0x208) == 1.5);
    assert(f32(OUT + 0x210) == 3);                    // MINSS: NaN first operand -> second operand
    assert(isnan(f32(OUT + 0x214)));                  // MAXSS: NaN second operand -> NaN
    double zero = RD(OUT + 0x218); assert(zero == 0 && !signbit(zero));  // equal -> second (+0)
    expect_floats(OUT + 0x220, 2, 4, 0.5f, 1);
    float rcp[4] = {0.25f, 0.0625f, 4, 1}, rsqrt[4] = {0.5f, 0.25f, 2, 1};
    for (int i = 0; i < 4; ++i) {                     // ISA bound: relative error <= 1.5 * 2^-12
        assert(fabsf(f32(OUT + 0x230 + 4 * i) - rcp[i]) <= 1.5f * 0x1p-12f * rcp[i]);
        assert(fabsf(f32(OUT + 0x240 + 4 * i) - rsqrt[i]) <= 1.5f * 0x1p-12f * rsqrt[i]);
    }
}

RT_CHECK(lifter_sse2_integer) {
    clear();
    uint32_t a[4] = {1, 0xffffffffu, 0x00010002u, 0x80000000u}, b[4] = {1, 2, 0x00030004u, 0x7fffffffu};
    memcpy(GPTR(IN + 0x300), a, 16); memcpy(GPTR(IN + 0x310), b, 16);
    run_named("lf_int");
    uint32_t shuffled[4] = {a[3], a[2], a[1], a[0]};
    assert(!memcmp(GPTR(OUT + 0x300), shuffled, 16));
    uint16_t aw[8], bw[8]; memcpy(aw, a, 16); memcpy(bw, b, 16);
    uint16_t unpacked[8] = {aw[0], bw[0], aw[1], bw[1], aw[2], bw[2], aw[3], bw[3]};
    assert(!memcmp(GPTR(OUT + 0x310), unpacked, 16));
    for (int i = 0; i < 4; ++i) {
        assert(R32(OUT + 0x320 + 4 * i) == a[i] + b[i]);
        assert(R32(OUT + 0x330 + 4 * i) == (a[i] == b[i] ? 0xffffffffu : 0));
    }
    for (int i = 0; i < 8; ++i) {
        assert(R16(OUT + 0x340 + 2 * i) == ((int16_t)aw[i] > (int16_t)bw[i] ? 0xffff : 0));
        assert(R16(OUT + 0x380 + 2 * i) == (uint16_t)((int16_t)aw[i] * (int16_t)bw[i]));
        assert(R16(OUT + 0x390 + 2 * i) == (uint16_t)(((int16_t)aw[i] * (int16_t)bw[i]) >> 16));
    }
    assert(R64(OUT + 0x350) == (uint64_t)a[0] * b[0] && R64(OUT + 0x358) == (uint64_t)a[2] * b[2]);
    uint16_t packed[8] = {1, 0xffff, 0x7fff, 0x8000, 1, 2, 0x7fff, 0x7fff};
    assert(!memcmp(GPTR(OUT + 0x360), packed, 16));
    uint8_t bytes[16] = {0}; memcpy(bytes, GPTR(IN + 0x300) + 5, 11);
    assert(!memcmp(GPTR(OUT + 0x370), bytes, 16));
    assert(R64(OUT + 0x3a0) == UINT64_MAX && R64(OUT + 0x3a8) == UINT64_MAX);
}

RT_CHECK(lifter_mmx) {
    clear();
    W64(IN + 0x400, UINT64_C(0x0000800000010001)); W64(IN + 0x408, 4);
    CPU c = run_named("lf_mmx");
    assert(R64(OUT + 0x400) == UINT64_C(0x0000800080008001));   // PADDW wraps per lane
    assert(R64(OUT + 0x408) == UINT64_C(0x0000080000001000));   // PSRAD by 4
    assert(R64(OUT + 0x410) == UINT64_C(0x7fff800008001000));   // PACKSSDW saturates
    assert(R32(OUT + 0x418) == 0x08001000u);
    assert(R64(OUT + 0x420) == UINT64_C(0x0000800080008001) && R64(OUT + 0x428) == 0);
    assert(f32(OUT + 0x430) == (float)(int32_t)0x80008001 && f32(OUT + 0x434) == 32768.0f);
    assert(f32(OUT + 0x438) == 0 && f32(OUT + 0x43c) == 0);     // upper lanes kept
    assert(c.ftags == 0 && c.ftop == 0);                          // EMMS
}

RT_CHECK(lifter_x87_compare) {
    clear();
    WD(IN + 0x500, 1); WD(IN + 0x508, 2); WD(IN + 0x510, NAN);
    CPU c = run_named("lf_x87_compare");
    assert(R8(OUT + 0x500) == 1 && R8(OUT + 0x501) == 0);
    assert(RD(OUT + 0x508) == 1);                                 // FCMOVNBE taken
    assert(R8(OUT + 0x502) == 1 && R8(OUT + 0x503) == 1 && R8(OUT + 0x504) == 1);  // unordered
    assert(RD(OUT + 0x510) == 2);                                 // FCMOVB not taken
    assert(c.ftop == 0 && c.ftags == 0);
}

RT_CHECK(lifter_x87_math) {
    clear();
    WD(IN + 0x600, -2.75); W32(IN + 0x608, 5); WD(IN + 0x610, 1); WD(IN + 0x618, 8);
    WD(IN + 0x620, 0.5); WD(IN + 0x628, 3); WD(IN + 0x630, -11);
    W64(IN + 0x640, UINT64_C(0xc000000000000000)); W16(IN + 0x648, 0x3fff);
    CPU c = run_named("lf_x87_math");
    assert(R16(OUT + 0x600) == 0x037f);
    assert(RD(OUT + 0x608) == -2);                                // truncating FRNDINT
    assert(RD(OUT + 0x610) == 5.0 / -15.0);
    assert(RD(OUT + 0x618) == atan2(1, 1) && RD(OUT + 0x620) == 3);
    assert(RD(OUT + 0x628) == sin(0.5));
    assert(RD(OUT + 0x630) == 1 && RD(OUT + 0x638) == tan(0.5));
    assert(RD(OUT + 0x640) == -2);                                // FPREM keeps the dividend's sign
    assert((R32(OUT + 0x648) & 0xffff) == 0x7200);                // TOP=6, C3=Q0, C1=Q1, C0=Q2=0, C2=0
    assert(R64(OUT + 0x650) == UINT64_C(0xc000000000000000) && R16(OUT + 0x658) == 0x3fff);
    assert(RD(OUT + 0x660) == log10(2.0));
    assert(c.ftop == 0 && c.fcw == 0x037f);
}

RT_CHECK(lifter_extended_conversions) {
    double cases[] = {0.0, -0.0, 1.0, -3.5, 1e-310, 1e300, INFINITY, -INFINITY};
    for (unsigned i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        rt_store_extended(OUT + 0x700, cases[i]);
        double back = rt_load_extended(OUT + 0x700);
        assert(back == cases[i] && signbit(back) == signbit(cases[i]));
    }
    rt_store_extended(OUT + 0x700, NAN);
    assert(isnan(rt_load_extended(OUT + 0x700)));
    W64(OUT + 0x700, 1); W16(OUT + 0x708, 0x3fff + 63);             // unnormal: 1 * 2^(63-63)
    assert(rt_load_extended(OUT + 0x700) == 1.0);
}
