// Runtime commun au code recompilé : état du CPU invité, mémoire 32 bits, indicateurs.
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef union {
    uint8_t b[16]; uint16_t w[8]; uint32_t d[4]; int32_t i[4];
    uint64_t q[2]; float f[4]; double g[2];
} XMM;

// Indicateurs persistants : CALL/RET et les thunks PIC ne les modifient pas.
// pf stocke un octet dont la parité représente PF.
typedef struct { uint32_t cf, zf, sf, of, pf, af; } FL;

typedef struct CPU {
    FL flags;
    uint32_t eip;
    uint32_t gs_base;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t df;          // indicateur de direction (0/1)
    uint32_t eflags_sys;  // bits écrits par popfd et non modélisés (ID, AC…)
    uint32_t ftop;        // sommet de la pile x87
    double st[8];
    uint16_t fcw, fsw;
    uint8_t ftags;
    uint32_t mxcsr;
    XMM xmm[8];
    uint32_t tid;         // index du thread invité
} CPU;

extern uint8_t *M;   // base hôte de l'espace d'adressage invité (4 Go réservés)

typedef void (*gfn)(CPU *);
void rt_dispatch(CPU *c, uint32_t target);                       // appelle le code invité à target
__attribute__((noreturn)) void rt_trap(CPU *c, uint32_t pc, const char *why);
void rt_div32(CPU *c, uint32_t divisor, int is_signed, uint32_t pc);
double rt_sse_round(CPU *c, double value);
double rt_x87_round(CPU *c, double value);
void rt_cpu_reset(CPU *c);
void rt_fxsave(CPU *c, uint32_t address);
void rt_ldmxcsr(CPU *c, uint32_t value);
void rt_cpuid(CPU *c);
uint64_t rt_rdtsc(void);
uint32_t rt_rand32(void);

#define GPTR(a) ((void *)(M + (uint32_t)(a)))

#define RT_VM_BEGIN 0x40000000u
#define RT_VM_END 0x70000000u
extern uint8_t rt_vm_permissions[(RT_VM_END - RT_VM_BEGIN) / 4096];
__attribute__((noreturn)) void rt_memory_trap(uint32_t address, uint64_t length, int mode);
static inline void rt_check_memory(uint32_t address, unsigned size, int mode) {
    if (address > UINT32_MAX - (size - 1)) rt_memory_trap(address, size, mode);
    uint32_t last = address + size - 1;
    if (address >= RT_VM_BEGIN && address < RT_VM_END &&
        (rt_vm_permissions[(address - RT_VM_BEGIN) >> 12] & mode) != mode)
        rt_memory_trap(address, size, mode);
    if ((last >> 12) != (address >> 12) && last >= RT_VM_BEGIN && last < RT_VM_END &&
        (rt_vm_permissions[(last - RT_VM_BEGIN) >> 12] & mode) != mode)
        rt_memory_trap(address, size, mode);
}
void rt_check_span(uint32_t address, uint64_t length, int mode);
static inline uint8_t  R8 (uint32_t a) { rt_check_memory(a, 1, 1); return M[a]; }
static inline uint16_t R16(uint32_t a) { rt_check_memory(a, 2, 1); uint16_t v; memcpy(&v, M + a, 2); return v; }
static inline uint32_t R32(uint32_t a) { rt_check_memory(a, 4, 1); uint32_t v; memcpy(&v, M + a, 4); return v; }
static inline uint64_t R64(uint32_t a) { rt_check_memory(a, 8, 1); uint64_t v; memcpy(&v, M + a, 8); return v; }
static inline float    RF (uint32_t a) { rt_check_memory(a, 4, 1); float v; memcpy(&v, M + a, 4); return v; }
static inline double   RD (uint32_t a) { rt_check_memory(a, 8, 1); double v; memcpy(&v, M + a, 8); return v; }
static inline XMM      RX (uint32_t a) { rt_check_memory(a, 16, 1); XMM v; memcpy(&v, M + a, 16); return v; }
static inline void W8 (uint32_t a, uint32_t v) { rt_check_memory(a, 1, 2); M[a] = (uint8_t)v; }
static inline void W16(uint32_t a, uint32_t v) { rt_check_memory(a, 2, 2); uint16_t x = (uint16_t)v; memcpy(M + a, &x, 2); }
static inline void W32(uint32_t a, uint32_t v) { rt_check_memory(a, 4, 2); memcpy(M + a, &v, 4); }
static inline void W64(uint32_t a, uint64_t v) { rt_check_memory(a, 8, 2); memcpy(M + a, &v, 8); }
static inline void WF (uint32_t a, float v)    { rt_check_memory(a, 4, 2); memcpy(M + a, &v, 4); }
static inline void WD (uint32_t a, double v)   { rt_check_memory(a, 8, 2); memcpy(M + a, &v, 8); }
static inline void WX (uint32_t a, XMM v)      { rt_check_memory(a, 16, 2); memcpy(M + a, &v, 16); }

static inline void PUSH(CPU *c, uint32_t v) { c->esp -= 4; W32(c->esp, v); }
static inline uint32_t POP(CPU *c) { uint32_t v = R32(c->esp); c->esp += 4; return v; }

#define MSK(n) ((n) == 32 ? 0xffffffffu : ((1u << (n)) - 1))
#define SGN(n) (1u << ((n) - 1))
static inline uint32_t sx(uint32_t v, int n) { return n == 32 ? v : (uint32_t)((int32_t)(v << (32 - n)) >> (32 - n)); }

static inline void szp(FL *f, uint32_t r, int n) { r &= MSK(n); f->zf = r == 0; f->sf = (r >> (n - 1)) & 1; f->pf = r & 0xff; }
static inline uint32_t f_add(FL *f, uint32_t a, uint32_t b, int n) {
    a &= MSK(n); b &= MSK(n); uint64_t r = (uint64_t)a + b; uint32_t rr = (uint32_t)r & MSK(n);
    f->cf = (uint32_t)(r >> n) & 1; f->of = (((a ^ rr) & (b ^ rr)) >> (n - 1)) & 1; f->af = ((a ^ b ^ rr) >> 4) & 1; szp(f, rr, n); return rr;
}
static inline uint32_t f_adc(FL *f, uint32_t a, uint32_t b, int n) {
    a &= MSK(n); b &= MSK(n); uint64_t r = (uint64_t)a + b + f->cf; uint32_t rr = (uint32_t)r & MSK(n);
    f->cf = (uint32_t)(r >> n) & 1; f->of = (((a ^ rr) & (b ^ rr)) >> (n - 1)) & 1; f->af = ((a ^ b ^ rr) >> 4) & 1; szp(f, rr, n); return rr;
}
static inline uint32_t f_sub(FL *f, uint32_t a, uint32_t b, int n) {
    a &= MSK(n); b &= MSK(n); uint32_t rr = (a - b) & MSK(n);
    f->cf = a < b; f->of = (((a ^ b) & (a ^ rr)) >> (n - 1)) & 1; f->af = ((a ^ b ^ rr) >> 4) & 1; szp(f, rr, n); return rr;
}
static inline uint32_t f_sbb(FL *f, uint32_t a, uint32_t b, int n) {
    a &= MSK(n); b &= MSK(n); uint32_t ci = f->cf; uint32_t rr = (a - b - ci) & MSK(n);
    f->cf = (uint64_t)a < (uint64_t)b + ci; f->of = (((a ^ b) & (a ^ rr)) >> (n - 1)) & 1; f->af = ((a ^ b ^ rr) >> 4) & 1; szp(f, rr, n); return rr;
}
static inline uint32_t f_logic(FL *f, uint32_t r, int n) { f->cf = 0; f->of = 0; szp(f, r, n); return r & MSK(n); }
static inline uint32_t f_inc(FL *f, uint32_t a, int n) { uint32_t r = (a + 1) & MSK(n); f->of = r == SGN(n); f->af = ((a ^ 1u ^ r) >> 4) & 1; szp(f, r, n); return r; }
static inline uint32_t f_dec(FL *f, uint32_t a, int n) { a &= MSK(n); uint32_t r = (a - 1) & MSK(n); f->of = a == SGN(n); f->af = ((a ^ 1u ^ r) >> 4) & 1; szp(f, r, n); return r; }
static inline uint32_t f_neg(FL *f, uint32_t a, int n) { a &= MSK(n); uint32_t r = (0 - a) & MSK(n); f->cf = a != 0; f->of = a == SGN(n); f->af = ((a ^ r) >> 4) & 1; szp(f, r, n); return r; }
static inline uint32_t f_shl(FL *f, uint32_t a, uint32_t cnt, int n) {
    cnt &= 31; a &= MSK(n); if (!cnt) return a;
    uint64_t r = (uint64_t)a << cnt; uint32_t rr = (uint32_t)r & MSK(n);
    f->cf = cnt <= (uint32_t)n ? (uint32_t)(r >> n) & 1 : 0; f->of = ((rr >> (n - 1)) & 1) ^ f->cf; szp(f, rr, n); return rr;
}
static inline uint32_t f_shr(FL *f, uint32_t a, uint32_t cnt, int n) {
    cnt &= 31; a &= MSK(n); if (!cnt) return a;
    uint32_t rr = cnt >= 32 ? 0 : a >> cnt; f->cf = (uint32_t)(((uint64_t)a >> (cnt - 1)) & 1); f->of = (a >> (n - 1)) & 1; szp(f, rr, n); return rr;
}
static inline uint32_t f_sar(FL *f, uint32_t a, uint32_t cnt, int n) {
    cnt &= 31; int32_t s = (int32_t)sx(a & MSK(n), n); if (!cnt) return a & MSK(n);
    uint32_t rr = (uint32_t)(s >> cnt) & MSK(n); f->cf = (uint32_t)(s >> (cnt - 1)) & 1; f->of = 0; szp(f, rr, n); return rr;
}
static inline uint32_t f_rotate(FL *f, uint32_t value, uint32_t count, int n, int kind) {
    value &= MSK(n);
    uint32_t masked = count & 31;
    if (!masked) return value;
    unsigned bits = (unsigned)n + (kind >= 2);
    unsigned amount = masked % bits;
    uint64_t ring = value | (kind >= 2 ? (uint64_t)f->cf << n : 0);
    uint64_t mask = (UINT64_C(1) << bits) - 1;
    if (amount) {
        if (kind == 0 || kind == 2) ring = ((ring << amount) | (ring >> (bits - amount))) & mask;
        else ring = ((ring >> amount) | (ring << (bits - amount))) & mask;
    }
    uint32_t result = (uint32_t)ring & MSK(n);
    if (kind >= 2) f->cf = (uint32_t)(ring >> n) & 1;
    else f->cf = kind == 0 ? result & 1 : result >> (n - 1);
    if (masked == 1) f->of = kind == 0 || kind == 2 ? (result >> (n - 1)) ^ f->cf : ((result >> (n - 1)) ^ (result >> (n - 2))) & 1;
    return result;
}
static inline uint32_t f_imul(FL *f, uint32_t a, uint32_t b, int n) {
    int64_t r = (int64_t)(int32_t)sx(a, n) * (int64_t)(int32_t)sx(b, n); uint32_t rr = (uint32_t)r & MSK(n);
    f->cf = f->of = r != (int64_t)(int32_t)sx(rr, n); szp(f, rr, n); return rr;
}
static inline int parity(uint32_t v) { return !__builtin_parity(v & 0xff); }

enum { CC_O, CC_NO, CC_B, CC_AE, CC_E, CC_NE, CC_BE, CC_A, CC_S, CC_NS, CC_P, CC_NP, CC_L, CC_GE, CC_LE, CC_G };
static inline int cond(const FL *f, int cc) {
    switch (cc) {
    case CC_O: return f->of; case CC_NO: return !f->of;
    case CC_B: return f->cf; case CC_AE: return !f->cf;
    case CC_E: return f->zf; case CC_NE: return !f->zf;
    case CC_BE: return f->cf | f->zf; case CC_A: return !(f->cf | f->zf);
    case CC_S: return f->sf; case CC_NS: return !f->sf;
    case CC_P: return parity(f->pf); case CC_NP: return !parity(f->pf);
    case CC_L: return f->sf != f->of; case CC_GE: return f->sf == f->of;
    case CC_LE: return f->zf || f->sf != f->of; default: return !f->zf && f->sf == f->of;
    }
}
// Comparaison scalaire SSE (comisd/ucomisd) : ZF, PF, CF ; OF=SF=0.
static inline void f_comi(FL *f, double a, double b) {
    f->of = f->sf = 0;
    if (a != a || b != b) { f->zf = 1; f->cf = 1; f->pf = 0; }      // non ordonné : PF=1
    else { f->zf = a == b; f->cf = a < b; f->pf = 1; }               // PF=0
}
static inline uint32_t get_eflags(CPU *c, const FL *f) {
    return f->cf | (parity(f->pf) << 2) | (f->af << 4) | (f->zf << 6) | (f->sf << 7) | (c->df << 10) | (f->of << 11) | 0x202 | c->eflags_sys;
}
static inline void set_eflags(CPU *c, FL *f, uint32_t v) {
    f->cf = v & 1; f->af = (v >> 4) & 1; f->pf = (v >> 2) & 1 ? 0 : 1; f->zf = (v >> 6) & 1; f->sf = (v >> 7) & 1;
    c->df = (v >> 10) & 1; f->of = (v >> 11) & 1; c->eflags_sys = v & 0x00240000u;  // ID (21) et AC (18)
}

// Pile x87, modélisée en double (pas de 80 bits : aucun accès tbyte dans ces modules).
#define ST(i) (c->st[(c->ftop + (i)) & 7])
static inline void fpush(CPU *c, double v) { c->ftop = (c->ftop - 1) & 7; c->st[c->ftop] = v; c->ftags |= 1u << c->ftop; }
static inline double fpop(CPU *c) { double v = c->st[c->ftop]; c->ftags &= ~(1u << c->ftop); c->ftop = (c->ftop + 1) & 7; return v; }

static inline int32_t cvt_trunc32(double v) { return (v != v || v >= 2147483648.0 || v < -2147483648.0) ? INT32_MIN : (int32_t)v; }
static inline int64_t cvt_trunc64(double v) { return (v != v || v >= 9223372036854775808.0 || v < -9223372036854775808.0) ? INT64_MIN : (int64_t)v; }
static inline int32_t cvt_round32(double v) { return cvt_trunc32(nearbyint(v)); }

// Chargement et pont d'appels invités.
int rt_init(void);
void rt_destroy(void);
int rt_map(uint32_t addr, uint64_t size);
uint32_t rt_mmap(CPU *c, uint32_t hint, uint32_t length, int protection, uint32_t flags, int fd, uint64_t offset);
int rt_munmap(uint32_t address, uint32_t length);
int rt_mprotect(uint32_t address, uint32_t length, int protection);
int rt_madvise(uint32_t address, uint32_t length, int advice);
void rt_vm_destroy(void);
int rt_load_segment(const char *directory, const char *name, uint32_t offset,
                    uint32_t addr, uint32_t filesz, uint32_t memsz);
int rt_init_import_data(const char *name, uint32_t address, uint32_t size);
int rt_time_data_import(const char *name, uint32_t address);  // time.c
int rt_load_images(const char *directory);  // généré par lift.py
uint32_t rt_export(const char *name);       // 0 si export non recompilé
gfn rt_lookup(uint32_t target);
uint32_t rt_atomic32(CPU *c, uint32_t address, uint32_t value, int operation);
uint32_t rt_exchange32(CPU *c, uint32_t address, uint32_t value);
uint32_t rt_compare_exchange32(CPU *c, uint32_t address, uint32_t expected, uint32_t value);
uint64_t rt_compare_exchange64(CPU *c, uint32_t address, uint64_t expected, uint64_t value);
void rt_return(CPU *c);
void rt_call(CPU *c, uint32_t target, uint32_t expected_return);
const char *rt_import_name(uint32_t target);
int rt_shim(CPU *c, const char *name);
CPU *rt_shim_cpu(void);   // CPU du shim en cours sur ce thread, NULL hors shim
void *rt_span(CPU *c, uint32_t address, uint64_t length);
void *rt_write_span(CPU *c, uint32_t address, uint64_t length);
uint32_t rt_string_length(CPU *c, uint32_t address);
int rt_register_tls(const char *directory, const char *name, uint32_t id, uint32_t offset, uint32_t filesz, uint32_t memsz, uint32_t alignment);
uint32_t rt_thread_pointer(CPU *c);
uint32_t rt_tls_address(CPU *c, uint32_t module, uint32_t offset);
void rt_tls_destroy(void);
int rt_loader_shim(CPU *c, const char *name);
void rt_modules_destroy(void);
int rt_scan_shim(CPU *c, const char *name);
int rt_libc_shim(CPU *c, const char *name);
void rt_libc_destroy(void);
uint32_t rt_standard_stream(int index);
void *rt_stream_file(CPU *c, uint32_t handle);
int rt_format_shim(CPU *c, const char *name);
int rt_pthread_shim(CPU *c, const char *name);
void rt_pthreads_destroy(void);
uint32_t rt_alloc(uint32_t size);
uint32_t rt_realloc(uint32_t ptr, uint32_t size);
int rt_free(uint32_t ptr);
void rt_shims_destroy(void);
size_t rt_atexit_count(void);
int rt_atexit_get(size_t index, uint32_t *function, uint32_t *arg, uint32_t *dso);
#define RT_RETURN_SENTINEL 0xfffffff0u

int rt_paths_init(const char *game, const char *linux, const char *user);
const char *rt_read_path(const char *path);
const char *rt_write_path(const char *path, int preserve_existing);
const char *rt_program_path(void);
int rt_stat_shim(CPU *c, const char *name);
void rt_set_errno(CPU *c, int value);
void rt_save_errno(CPU *c);

void rt_guest_enter(void);
void rt_guest_leave(void);
unsigned rt_guest_suspend(void);
void rt_guest_resume(unsigned saved);
void rt_guest_poll(void);
int rt_thread_shim(CPU *c, const char *name);
void rt_thread_destructors(CPU *c);
void rt_threads_destroy(void);

int rt_futex(CPU *c,uint32_t address,uint32_t operation,uint32_t value,uint32_t timeout);
// Fournisseurs de shims supplémentaires : chaque fichier runtime s'enregistre
// lui-même, sans modifier shims.c. Un fournisseur renvoie 0 s'il ne connaît
// pas le nom. Déclarer aussi les noms sur une ligne `// SHIMS: a b c` du
// fichier pour que tools/lift.py les accepte.
// Fonctions natives obtenues à l'exécution (SDL_GL_GetProcAddress,
// alGetProcAddress, dlsym sur une bibliothèque hôte) : une adresse invitée
// stable par nom ; un appel invité vers elle passe par rt_shim(c, nom) comme
// un import statique. Renvoie 0 si la table est pleine.
#define RT_DYNAMIC_IMPORT_BASE 0xf8000000u
#define RT_DYNAMIC_IMPORT_STRIDE 16u
#define RT_DYNAMIC_IMPORT_LIMIT 0x40000u
uint32_t rt_dynamic_import(const char *name);
const char *rt_dynamic_import_name(uint32_t address);
// Handles opaques pour les pointeurs hôtes (SDL_Window*, contexte GL, FT_Face...).
// Le code invité ne doit jamais les déréférencer : la plage 0xe8000000 n'est
// pas projetée, un accès invité y est une faute. `kind` est un identifiant non
// nul choisi par chaque pont (par ex. un FOURCC) ; get/free vérifient le type.
#define RT_HANDLE_BASE 0xe8000000u  // 0xe0000000 : données importées (elfprep SHIMDATA_BASE)
#define RT_HANDLE_STRIDE 16u
#define RT_HANDLE_LIMIT 0x800000u
uint32_t rt_handle_new(void *host, uint32_t kind);
void *rt_handle_get(uint32_t handle, uint32_t kind);
uint32_t rt_handle_find(const void *host, uint32_t kind);
void *rt_handle_free(uint32_t handle, uint32_t kind);
// Adresse invitée d'une fonction GL hôte, 0 si non prise en charge (gl.c).
uint32_t rt_gl_proc_address(const char *name);
// Valeur de retour flottante i386 (float/double/long double) : dans ST(0).
static inline void rt_return_st0(CPU *c, double value) { fpush(c, value); }
typedef int (*rt_shim_provider)(CPU *c, const char *name);
void rt_register_shim_provider(rt_shim_provider provider);
#define RT_SHIM_PROVIDER(fn) \
    __attribute__((constructor)) static void rt_register_##fn(void) { rt_register_shim_provider(fn); }
void rt_futex_destroy(void);
