// Aiguillage des imports : mêmes résultats qu'une recherche par nom, sans la
// refaire à chaque appel (cache par adresse cible), y compris entre threads.
#include "checks.h"
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static _Atomic unsigned bench_a_calls, bench_b_calls;
static int bench_provider_a(CPU *c, const char *name) {
    if (strcmp(name, "rt_bench_import_a") && strcmp(name, "rt_bench_import_a2")) return 0;
    c->eax = 0xa; ++bench_a_calls; return 1;
}
RT_SHIM_PROVIDER(bench_provider_a)
static int bench_provider_b(CPU *c, const char *name) {
    // Accepte aussi le nom de A : l'ordre d'enregistrement doit primer.
    if (!strcmp(name, "rt_bench_import_b") || !strcmp(name, "rt_bench_import_b2")) {
        // Comme un shim bloquant : relâche le verrou invité, d'autres threads
        // aiguillent pendant ce temps.
        unsigned saved = rt_guest_suspend(); sched_yield(); rt_guest_resume(saved);
        c->eax = 0xb; ++bench_b_calls; return 1;
    }
    if (!strcmp(name, "rt_bench_import_a")) { c->eax = 0xbad; return 1; }
    return 0;
}
RT_SHIM_PROVIDER(bench_provider_b)

static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static void call_import(CPU *c, uint32_t address, uint32_t expected) {
    c->eax = 0;
    rt_call(c, address, RT_RETURN_SENTINEL);
    assert(c->eax == expected && c->esp == 0x20010000);
}

RT_CHECK(import_dispatch_benchmark) {
    uint32_t a = rt_dynamic_import("rt_bench_import_a");
    CPU c; rt_cpu_reset(&c); c.esp = 0x20010000;
    enum { N = 200000 };
    call_import(&c, a, 0xa);
    double start = now();
    for (unsigned i = 0; i < N; ++i) call_import(&c, a, 0xa);
    double elapsed = now() - start;
    assert(bench_a_calls == N + 1);
    fprintf(stderr, "import dispatch: %.1f ns/call (dernier fournisseur enregistré, %u appels)\n",
            elapsed * 1e9 / N, (unsigned)N);
}

RT_CHECK(import_dispatch_respects_provider_order) {
    uint32_t a = rt_dynamic_import("rt_bench_import_a"), b = rt_dynamic_import("rt_bench_import_b");
    CPU c; rt_cpu_reset(&c); c.esp = 0x20010000;
    for (int i = 0; i < 3; ++i) { call_import(&c, a, 0xa); call_import(&c, b, 0xb); }
    // Le chemin par nom (rt_shim) reste disponible et donne le même fournisseur.
    c.eax = 0; assert(rt_shim(&c, "rt_bench_import_a") && c.eax == 0xa);
    c.eax = 0; assert(rt_shim(&c, "rt_bench_import_b") && c.eax == 0xb);
}

typedef struct { uint32_t address, expected, stack; } Caller;
static void *concurrent_caller(void *arg) {
    const Caller *job = arg;
    CPU c; rt_cpu_reset(&c);
    for (int i = 0; i < 20000; ++i) {
        c.esp = job->stack; c.eax = 0;      // une pile invitée par thread
        rt_call(&c, job->address, RT_RETURN_SENTINEL);
        if (c.esp != job->stack || c.eax != job->expected) abort();
    }
    return NULL;
}

RT_CHECK(import_dispatch_is_thread_safe) {
    // Noms neufs : le cache se remplit pendant que les threads s'exécutent.
    unsigned before_a = bench_a_calls, before_b = bench_b_calls;
    Caller jobs[8];
    for (int i = 0; i < 8; ++i)
        jobs[i] = (Caller){rt_dynamic_import(i & 1 ? "rt_bench_import_b2" : "rt_bench_import_a2"),
                           i & 1 ? 0xbu : 0xau, 0x20018000u - 0x400u * (uint32_t)i};
    pthread_t threads[8];
    for (int i = 0; i < 8; ++i) assert(!pthread_create(&threads[i], NULL, concurrent_caller, &jobs[i]));
    for (int i = 0; i < 8; ++i) assert(!pthread_join(threads[i], NULL));
    assert(bench_a_calls - before_a == 4 * 20000 && bench_b_calls - before_b == 4 * 20000);
}

RT_CHECK(unknown_import_still_traps) {
    uint32_t unknown = rt_dynamic_import("rt_no_provider_knows_this");
    for (int round = 0; round < 1; ++round) {
        fflush(NULL);
        pid_t pid = fork();
        assert(pid >= 0);
        if (!pid) {
            int devnull = open("/dev/null", 1);
            if (devnull >= 0) dup2(devnull, 2);
            CPU c; rt_cpu_reset(&c); c.esp = 0x20010000;
            rt_call(&c, unknown, RT_RETURN_SENTINEL);
            _exit(0);                                    // ne doit jamais revenir
        }
        int status = 0;
        assert(waitpid(pid, &status, 0) == pid);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    }
}

// Résolveur optionnel : le second appel passe par le handler direct.
static unsigned resolved_provider_calls, resolved_handler_calls;
static int resolved_provider(CPU *c, const char *name) {
    if (strcmp(name, "rt_resolved_import")) return 0;
    ++resolved_provider_calls; c->eax = 0x51; return 1;
}
static int resolved_handler(CPU *c, const char *name, void *data) {
    assert(!strcmp(name, "rt_resolved_import") && data == &resolved_handler_calls);
    ++*(unsigned *)data; c->eax = 0x52; return 1;
}
static int resolve_import(const char *name, RtShimRoute *route) {
    if (strcmp(name, "rt_resolved_import")) return 0;
    route->handler = resolved_handler; route->data = &resolved_handler_calls; return 1;
}
RT_SHIM_PROVIDER_RESOLVER(resolved_provider, resolve_import)

RT_CHECK(import_dispatch_uses_resolved_handler) {
    uint32_t a = rt_dynamic_import("rt_resolved_import");
    CPU c; rt_cpu_reset(&c); c.esp = 0x20010000;
    call_import(&c, a, 0x51);
    call_import(&c, a, 0x52); call_import(&c, a, 0x52);
    assert(resolved_provider_calls == 1 && resolved_handler_calls == 2);
}

// Un fournisseur qui refuse après avoir accepté (hors contrat) ne fait pas
// piéger : la chaîne complète est reprise et trouve le suivant.
static int flaky_enabled = 1;
static int flaky_first(CPU *c, const char *name) {
    if (!flaky_enabled || strcmp(name, "rt_flaky_import")) return 0;
    c->eax = 1; return 1;
}
RT_SHIM_PROVIDER(flaky_first)
static int flaky_second(CPU *c, const char *name) {
    if (strcmp(name, "rt_flaky_import")) return 0;
    c->eax = 2; return 1;
}
RT_SHIM_PROVIDER(flaky_second)

RT_CHECK(import_dispatch_falls_back_to_full_chain) {
    uint32_t a = rt_dynamic_import("rt_flaky_import");
    CPU c; rt_cpu_reset(&c); c.esp = 0x20010000;
    call_import(&c, a, 1);
    flaky_enabled = 0;
    call_import(&c, a, 2); call_import(&c, a, 2);
    flaky_enabled = 1;
}
