// Fichiers /proc Linux lus par le jeu, générés à chaque ouverture depuis
// l'état réel de l'hôte (mémoire, swap, processus) et le profil CPU invité
// de cpu.c (CPUID, horloge virtuelle de 1 GHz). Ils sont écrits dans un
// dossier temporaire privé et ouverts par le chemin qu'on renvoie à fopen/open.
// Les autres chemins /proc renvoient ENOENT et sont signalés une fois.
#include "rt.h"
#include <errno.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char directory[1024];
static char *cmdline; static size_t cmdline_size;

void rt_procfs_set_cmdline(const char *bytes, size_t size) {
    pthread_mutex_lock(&lock);
    free(cmdline);
    cmdline = malloc(size ? size : 1);
    if (cmdline) { memcpy(cmdline, bytes, size); cmdline_size = size; } else cmdline_size = 0;
    pthread_mutex_unlock(&lock);
}

static uint64_t sysctl_u64(const char *name) {
    uint64_t value = 0; size_t size = sizeof(value);
    if (sysctlbyname(name, &value, &size, NULL, 0)) return 0;
    if (size == 4) { uint32_t v32; memcpy(&v32, &value, 4); return v32; }
    return value;
}

static int write_meminfo(FILE *out) {
    uint64_t total = sysctl_u64("hw.memsize");
    vm_size_t page = 0; host_page_size(mach_host_self(), &page);
    vm_statistics64_data_t vm; mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &count) != KERN_SUCCESS) return -1;
    struct xsw_usage swap = {0}; size_t swap_size = sizeof(swap);
    sysctlbyname("vm.swapusage", &swap, &swap_size, NULL, 0);
    uint64_t kb = 1024, free_kb = (uint64_t)vm.free_count * page / kb;
    uint64_t inactive_kb = (uint64_t)vm.inactive_count * page / kb;
    uint64_t active_kb = (uint64_t)vm.active_count * page / kb;
    uint64_t purgeable_kb = (uint64_t)vm.purgeable_count * page / kb;
    uint64_t cached_kb = (uint64_t)vm.external_page_count * page / kb;
    fprintf(out, "MemTotal:       %8llu kB\n", (unsigned long long)(total / kb));
    fprintf(out, "MemFree:        %8llu kB\n", (unsigned long long)free_kb);
    fprintf(out, "MemAvailable:   %8llu kB\n", (unsigned long long)(free_kb + inactive_kb + purgeable_kb));
    fprintf(out, "Buffers:        %8llu kB\n", 0ULL);
    fprintf(out, "Cached:         %8llu kB\n", (unsigned long long)cached_kb);
    fprintf(out, "SwapCached:     %8llu kB\n", 0ULL);
    fprintf(out, "Active:         %8llu kB\n", (unsigned long long)active_kb);
    fprintf(out, "Inactive:       %8llu kB\n", (unsigned long long)inactive_kb);
    fprintf(out, "SwapTotal:      %8llu kB\n", (unsigned long long)(swap.xsu_total / kb));
    fprintf(out, "SwapFree:       %8llu kB\n", (unsigned long long)(swap.xsu_avail / kb));
    return 0;
}

// Cohérent avec rt_cpuid (cpu.c) : un processeur logique, famille 6,
// modèle 15, révision 2, fpu tsc cx8 cmov mmx fxsr sse sse2, TSC à 1 GHz.
static int write_cpuinfo(FILE *out) {
    uint64_t l2 = sysctl_u64("hw.perflevel0.l2cachesize");
    if (!l2) l2 = sysctl_u64("hw.l2cachesize");
    fprintf(out,
            "processor\t: 0\n"
            "vendor_id\t: GenuineIntel\n"
            "cpu family\t: 6\n"
            "model\t\t: 15\n"
            "model name\t: Portal 2 AOT virtual CPU (Apple Silicon)\n"
            "stepping\t: 2\n"
            "cpu MHz\t\t: 1000.000\n"
            "cache size\t: %llu KB\n"
            "physical id\t: 0\n"
            "siblings\t: 1\n"
            "core id\t\t: 0\n"
            "cpu cores\t: 1\n"
            "fpu\t\t: yes\n"
            "fpu_exception\t: yes\n"
            "cpuid level\t: 1\n"
            "wp\t\t: yes\n"
            "flags\t\t: fpu tsc cx8 cmov mmx fxsr sse sse2\n"
            "bogomips\t: 2000.00\n"
            "clflush size\t: 64\n"
            "cache_alignment\t: 64\n"
            "address sizes\t: 32 bits physical, 32 bits virtual\n"
            "power management:\n\n",
            (unsigned long long)(l2 / 1024));
    return 0;
}

static int write_statm(FILE *out) {
    mach_task_basic_info_data_t info; mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) != KERN_SUCCESS) return -1;
    // Pages Linux de 4 Kio ; la taille virtuelle reflète l'espace invité de 4 Gio.
    unsigned long long resident = info.resident_size / 4096, size = (UINT64_C(1) << 32) / 4096;
    fprintf(out, "%llu %llu 0 0 0 %llu 0\n", size, resident, resident);
    return 0;
}

static int write_cmdline(FILE *out) {
    pthread_mutex_lock(&lock);
    size_t written = cmdline ? fwrite(cmdline, 1, cmdline_size, out) : 0;
    int ok = cmdline && written == cmdline_size;
    pthread_mutex_unlock(&lock);
    return ok ? 0 : -1;
}

static int write_overcommit(FILE *out) { return fputs("0\n", out) < 0 ? -1 : 0; }

typedef struct { const char *path, *file; int (*writer)(FILE *); } ProcFile;
static const ProcFile files[] = {
    {"/proc/meminfo", "meminfo", write_meminfo},
    {"/proc/cpuinfo", "cpuinfo", write_cpuinfo},
    {"/proc/self/statm", "self_statm", write_statm},
    {"/proc/self/cmdline", "self_cmdline", write_cmdline},
    {"/proc/sys/vm/overcommit_memory", "overcommit_memory", write_overcommit},
};

int rt_is_proc_path(const char *path) {
    return path && !strncmp(path, "/proc", 5) && (!path[5] || path[5] == '/');
}

// Chemin hôte d'un fichier /proc généré, ou NULL (errno) s'il n'est pas émulé.
const char *rt_proc_path(const char *path) {
    char self[64];
    snprintf(self, sizeof(self), "/proc/%d/", (int)getpid());
    char normalized[1024];
    if (!strncmp(path, self, strlen(self)))   // /proc/<notre pid>/x == /proc/self/x
        snprintf(normalized, sizeof(normalized), "/proc/self/%s", path + strlen(self));
    else snprintf(normalized, sizeof(normalized), "%s", path);
    for (size_t i = 0; i < sizeof(files) / sizeof(*files); ++i) {
        if (strcmp(normalized, files[i].path)) continue;
        pthread_mutex_lock(&lock);
        if (!directory[0]) {
            const char *tmp = getenv("TMPDIR");
            snprintf(directory, sizeof(directory), "%s/portal2-procfs-XXXXXX", tmp && *tmp ? tmp : "/tmp");
            if (!mkdtemp(directory)) { directory[0] = 0; pthread_mutex_unlock(&lock); return NULL; }
        }
        pthread_mutex_unlock(&lock);
        static _Thread_local char result[1100];
        snprintf(result, sizeof(result), "%s/%s", directory, files[i].file);
        char temporary[1120];
        snprintf(temporary, sizeof(temporary), "%s.%p", result, (void *)&result);
        FILE *out = fopen(temporary, "wb");
        if (!out) return NULL;
        int failed = files[i].writer(out);
        if (fclose(out) || failed || rename(temporary, result)) { unlink(temporary); errno = EIO; return NULL; }
        return result;
    }
    static pthread_mutex_t report_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&report_lock);
    fprintf(stderr, "[procfs] %s is not emulated (ENOENT)\n", path);
    pthread_mutex_unlock(&report_lock);
    errno = ENOENT;
    return NULL;
}
