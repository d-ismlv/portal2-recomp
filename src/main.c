#include "modules.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static uint32_t guest_string(const char *value) {
    size_t length = strlen(value) + 1;
    uint32_t address = rt_alloc((uint32_t)length);
    if (!address) { fputs("guest argument allocation failed\n", stderr); exit(2); }
    memcpy(GPTR(address), value, length);
    return address;
}
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);   // journal lisible même si le processus meurt
    const char *images = NULL, *game = NULL, *linux = NULL, *user = NULL;
    int first = 1;
    while (first < argc) {
        if (!strcmp(argv[first], "--images") && first + 1 < argc) { images = argv[first + 1]; first += 2; }
        else if (!strcmp(argv[first], "--game-dir") && first + 1 < argc) { game = argv[first + 1]; first += 2; }
        else if (!strcmp(argv[first], "--linux-root") && first + 1 < argc) { linux = argv[first + 1]; first += 2; }
        else if (!strcmp(argv[first], "--user-dir") && first + 1 < argc) { user = argv[first + 1]; first += 2; }
        else if (!strcmp(argv[first], "--")) { ++first; break; }
        else break;
    }
    if (!images || !game) { fprintf(stderr, "usage: %s --images /absolute/prep --game-dir /path/to/Portal2 -- [game arguments]\n", argv[0]); return 2; }
    if (rt_init() || rt_load_images(images) || rt_map(0x70000000, 0x800000)) {
        fputs("native runtime initialization failed\n", stderr); return 2;
    }
    if (linux && user && rt_paths_init(game, linux, user)) { perror("game paths"); rt_destroy(); return 2; }
    if (chdir(game)) { perror(game); rt_destroy(); return 2; }
    CPU cpu; rt_cpu_reset(&cpu); cpu.esp = 0x70800000;
    uint32_t count = (uint32_t)(argc - first + 1), vector = rt_alloc((count + 1) * 4);
    if (!vector) { rt_destroy(); return 2; }
    W32(vector, guest_string("portal2_linux"));
    for (uint32_t i = 1; i < count; ++i) W32(vector + i * 4, guest_string(argv[first + (int)i - 1]));
    W32(vector + count * 4, 0);
    {   // /proc/self/cmdline : arguments invités séparés par des NUL
        size_t total = 0;
        for (uint32_t i = 0; i < count; ++i) total += strlen((const char *)GPTR(R32(vector + i * 4))) + 1;
        char *joined = malloc(total ? total : 1), *at = joined;
        for (uint32_t i = 0; joined && i < count; ++i) {
            size_t n = strlen((const char *)GPTR(R32(vector + i * 4))) + 1;
            memcpy(at, GPTR(R32(vector + i * 4)), n); at += n;
        }
        if (joined) { rt_procfs_set_cmdline(joined, total); free(joined); }
    }
    rt_loader_set_arguments(count, vector, 0);
    const GuestModule *program = rt_find_module("portal2_linux");
    uint32_t entry = program ? rt_module_symbol(program, "main") : 0;
    if (!entry) { fputs("real portal2_linux main is absent\n", stderr); rt_destroy(); return 2; }
    if (rt_initialize_module(&cpu, program)) { rt_destroy(); return 2; }
    uint32_t args[] = {count, vector, 0};
    fprintf(stderr, "entering recompiled portal2_linux main at %#x\n", entry);
    int result = (int32_t)rt_invoke(&cpu, entry, args, 3);
    fprintf(stderr, "portal2_linux main returned %d\n", result);
    rt_destroy();
    return result;
}
