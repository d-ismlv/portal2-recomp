// Profileur intégré (diagnostic) : P2_PROFILE=/chemin.txt active un minuteur
// SIGPROF (1 ms de CPU). Chaque tic relève le compteur de programme hôte ;
// un thread écrit toutes les 5 s l'histogramme par symbole hôte (fonctions
// générées fn_<adresse invitée>, shims, bibliothèques). Rien n'est actif
// sans la variable d'environnement.
#include "rt.h"
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define SLOTS 65536u
typedef struct { uintptr_t pc, caller, caller2; int main; } Key;
static Key keys[SLOTS];
static uint32_t used[SLOTS];   // 0 libre, 1 en cours, 2 prêt (accès __atomic)
static _Atomic uint32_t hits[SLOTS];
static _Atomic uint64_t total, dropped;
static const char *output;

static void tick(int signal, siginfo_t *info, void *context) {
    (void)signal; (void)info;
    Key key = {0};
#if defined(__aarch64__)
    ucontext_t *uc = context;
    key.pc = (uintptr_t)uc->uc_mcontext->__ss.__pc;
    key.caller = (uintptr_t)uc->uc_mcontext->__ss.__lr;
    uintptr_t fp = (uintptr_t)uc->uc_mcontext->__ss.__fp;
    // Deuxième appelant : cadre courant [fp] = fp parent, [fp+8] = retour.
    if (fp && !(fp & 7)) {
        uintptr_t parent = ((uintptr_t *)fp)[0];
        if (parent > fp && parent - fp < (1u << 24) && !(parent & 7)) key.caller2 = ((uintptr_t *)parent)[1];
        else key.caller2 = ((uintptr_t *)fp)[1];
    }
#else
    (void)context;
#endif
    key.main = pthread_main_np();
    ++total;
    uint32_t h = (uint32_t)(((key.pc >> 2) ^ (key.caller * 31) ^ (key.caller2 * 7) ^ (uintptr_t)key.main) * 2654435761u) & (SLOTS - 1);
    for (unsigned probe = 0; probe < 64; ++probe, h = (h + 1) & (SLOTS - 1)) {
        uint32_t state = __atomic_load_n(&used[h], __ATOMIC_ACQUIRE);
        if (state == 2 && !memcmp(&keys[h], &key, sizeof(key))) { ++hits[h]; return; }
        if (state == 0) {
            uint32_t expected = 0;
            if (__atomic_compare_exchange_n(&used[h], &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                keys[h] = key; __atomic_store_n(&used[h], 2, __ATOMIC_RELEASE); ++hits[h]; return;
            }
        }
    }
    ++dropped;
}

static const char *symbol(uintptr_t pc) {
    Dl_info where;
    if (pc && dladdr((void *)pc, &where)) return where.dli_sname ? where.dli_sname : where.dli_fname;
    return "?";
}
typedef struct { const char *name; uint64_t count; } Entry;
static int by_count(const void *a, const void *b) {
    uint64_t x = ((const Entry *)a)->count, y = ((const Entry *)b)->count;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void dump_section(FILE *out, int main_only, int with_callers) {
    Entry *entries = calloc(SLOTS, sizeof(*entries));
    char (*names)[512] = calloc(SLOTS, 512);
    if (!entries || !names) { free(entries); free(names); return; }
    size_t n = 0; uint64_t section = 0;
    for (uint32_t i = 0; i < SLOTS; ++i) {
        if (__atomic_load_n(&used[i], __ATOMIC_ACQUIRE) != 2 || !hits[i]) continue;
        if (main_only && !keys[i].main) continue;
        section += hits[i];
        char name[512];
        if (with_callers) snprintf(name, sizeof(name), "%s <- %s <- %s", symbol(keys[i].pc), symbol(keys[i].caller), symbol(keys[i].caller2));
        else snprintf(name, sizeof(name), "%s", symbol(keys[i].pc));
        size_t k = 0;
        while (k < n && strcmp(names[k], name)) ++k;
        if (k == n) { snprintf(names[n], 512, "%s", name); entries[n++] = (Entry){names[k], 0}; }
        entries[k].count += hits[i];
    }
    qsort(entries, n, sizeof(*entries), by_count);
    fprintf(out, "== %s, %s : %llu échantillons\n", main_only ? "thread principal" : "tous threads",
            with_callers ? "avec appelants" : "par symbole", (unsigned long long)section);
    for (size_t k = 0; k < n && k < 60; ++k)
        fprintf(out, "%6.2f%% %s\n", 100.0 * (double)entries[k].count / (double)(section ? section : 1), entries[k].name);
    free(entries); free(names);
}

static void dump(void) {
    char temporary[1100];
    snprintf(temporary, sizeof(temporary), "%s.tmp", output);
    FILE *out = fopen(temporary, "w");
    if (!out) return;
    fprintf(out, "samples %llu (dropped %llu)\n", (unsigned long long)total, (unsigned long long)dropped);
    dump_section(out, 1, 0);
    dump_section(out, 1, 1);
    dump_section(out, 0, 0);
    dump_section(out, 0, 1);
    fclose(out);
    rename(temporary, output);
}

static void *writer(void *unused) {
    (void)unused;
    for (;;) { sleep(5); dump(); }
    return NULL;
}

// P2_PROFILE_WALL=1 : échantillonne le thread principal en temps réel (même
// bloqué) en lui envoyant SIGPROF toutes les millisecondes ; montre où il attend.
static pthread_t main_thread;
static void *wall_sampler(void *unused) {
    (void)unused;
    struct timespec period = {0, 1000000};
    for (;;) { nanosleep(&period, NULL); pthread_kill(main_thread, SIGPROF); }
    return NULL;
}

__attribute__((constructor)) static void start_profiler(void) {
    output = getenv("P2_PROFILE");
    if (!output || !*output) return;
    struct sigaction action = {0};
    action.sa_sigaction = tick;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(SIGPROF, &action, NULL);
    pthread_t thread;
    if (getenv("P2_PROFILE_WALL")) {
        main_thread = pthread_self();   // constructeur : thread principal
        pthread_create(&thread, NULL, wall_sampler, NULL);
        pthread_detach(thread);
    } else {
        struct itimerval timer = {{0, 1000}, {0, 1000}};
        setitimer(ITIMER_PROF, &timer, NULL);
    }
    pthread_create(&thread, NULL, writer, NULL);
    pthread_detach(thread);
}
