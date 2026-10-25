// Explicit guest handles and i386 layouts: no native FILE/DIR/timeval pointer
// or structure is ever cast to the guest ABI.
#include "posix.h"
#include <dirent.h>
#include <pthread.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

typedef struct Stream {
    uint32_t handle;
    FILE *file;
    int directory;         // flux de répertoire : liste fusionnée figée à l'ouverture
    RtDirEntry *entries;
    size_t entry_count, position;
    uint32_t dirent, dirent64;
    int standard;
    struct Stream *next;
} Stream;
static Stream *streams;
static uint32_t next_handle = 0xd0000100u;
static _Thread_local uint32_t errno_address;
typedef struct Environment { char *name; uint32_t value; struct Environment *next; } Environment;
static Environment *environment;
static uint32_t environ_slots[4];
static size_t environ_slot_count;
static uint32_t argument(CPU *c, unsigned i) { return R32(c->esp + 4 + 4 * i); }

// errno Darwin et Linux coïncident de 1 à 34 (sauf EAGAIN/EDEADLK : 11/35 inversés).
static const struct { int darwin, linux_value; } errno_table[] = {
    {EDEADLK, 35}, {EAGAIN, 11}, {EINPROGRESS, 115}, {EALREADY, 114}, {ENOTSOCK, 88},
    {EDESTADDRREQ, 89}, {EMSGSIZE, 90}, {EPROTOTYPE, 91}, {ENOPROTOOPT, 92},
    {EPROTONOSUPPORT, 93}, {ESOCKTNOSUPPORT, 94}, {ENOTSUP, 95}, {EOPNOTSUPP, 95},
    {EPFNOSUPPORT, 96}, {EAFNOSUPPORT, 97}, {EADDRINUSE, 98}, {EADDRNOTAVAIL, 99},
    {ENETDOWN, 100}, {ENETUNREACH, 101}, {ENETRESET, 102}, {ECONNABORTED, 103},
    {ECONNRESET, 104}, {ENOBUFS, 105}, {EISCONN, 106}, {ENOTCONN, 107}, {ESHUTDOWN, 108},
    {ETOOMANYREFS, 109}, {ETIMEDOUT, 110}, {ECONNREFUSED, 111}, {ELOOP, 40},
    {ENAMETOOLONG, 36}, {EHOSTDOWN, 112}, {EHOSTUNREACH, 113}, {ENOTEMPTY, 39},
    {EUSERS, 87}, {EDQUOT, 122}, {ESTALE, 116}, {EREMOTE, 66}, {ENOLCK, 37}, {ENOSYS, 38},
    {EOVERFLOW, 75}, {ECANCELED, 125}, {EIDRM, 43}, {ENOMSG, 42}, {EILSEQ, 84},
    {ENOATTR, 61}, {EBADMSG, 74}, {EMULTIHOP, 72}, {ENODATA, 61}, {ENOLINK, 67},
    {ENOSR, 63}, {ENOSTR, 60}, {EPROTO, 71}, {ETIME, 62}, {ENOTRECOVERABLE, 131},
    {EOWNERDEAD, 130}, {EPROCLIM, 11}, {EBADEXEC, 8}, {EBADARCH, 8}, {EBADMACHO, 8},
    {ESHLIBVERS, 8}, {EFTYPE, 22}, {EAUTH, 13}, {ENEEDAUTH, 13}, {EPWROFF, 5}, {EDEVERR, 5},
    {EBADRPC, 5}, {ERPCMISMATCH, 5}, {EPROGUNAVAIL, 5}, {EPROGMISMATCH, 5}, {EPROCUNAVAIL, 5},
    {ENOPOLICY, 1}, {EQFULL, 105},
};
int rt_linux_errno(int value) {
    if (value == EDEADLK) return 35;
    if (value == EAGAIN) return 11;
    if (value > 0 && value <= 34) return value;
    for (size_t i = 0; i < sizeof(errno_table) / sizeof(*errno_table); ++i)
        if (errno_table[i].darwin == value) return errno_table[i].linux_value;
    return 5; // erreur Darwin sans équivalent Linux : EIO
}
int rt_darwin_errno(int value) {
    if (value == 11) return EAGAIN;
    if (value == 35) return EDEADLK;
    if (value > 0 && value <= 34) return value;
    for (size_t i = 0; i < sizeof(errno_table) / sizeof(*errno_table); ++i)
        if (errno_table[i].linux_value == value) return errno_table[i].darwin;
    return 0;
}
#define linux_errno rt_linux_errno
static uint32_t errno_slot(CPU *c) {
    if (!errno_address) {
        errno_address = rt_alloc(4);
        if (!errno_address) rt_trap(c, c->eip, "cannot allocate guest errno");
        W32(errno_address, 0);
    }
    return errno_address;
}
void rt_set_errno(CPU *c, int value) { W32(errno_slot(c), (uint32_t)value); }
void rt_save_errno(CPU *c) { rt_set_errno(c, linux_errno(errno)); }
#define save_errno rt_save_errno
static const char *string(CPU *c, uint32_t address) { rt_string_length(c, address); return GPTR(address); }
static Stream *lookup(CPU *c, uint32_t handle, int directory) {
    for (Stream *s = streams; s; s = s->next)
        if (s->handle == handle && (directory ? s->directory : s->file != NULL)) return s;
    rt_trap(c, c->eip, directory ? "unknown guest DIR handle" : "unknown guest FILE handle");
}
void *rt_stream_file(CPU *c, uint32_t handle) { return lookup(c, handle, 0)->file; }
static Stream *new_stream_record(void) {
    Stream *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->handle = next_handle; next_handle += 0x100;
    s->next = streams; streams = s;
    return s;
}
static uint32_t new_stream(FILE *file) {
    Stream *s = new_stream_record();
    if (!s) return 0;
    s->file = file;
    return s->handle;
}
uint32_t rt_stream_new(FILE *file) { return new_stream(file); }
FILE *rt_stream_take(CPU *c, uint32_t handle) {
    Stream *s = lookup(c, handle, 0);
    if (s->standard) rt_trap(c, c->eip, "standard stream cannot be detached");
    FILE *file = s->file; s->file = NULL;
    return file;
}

// Liste fusionnée d'un répertoire invité : copie utilisateur, dépôt Linux,
// copie du jeu. Un nom n'apparaît qu'une fois ; la première source gagne.
typedef struct { RtDirEntry *items; size_t count, capacity; uint32_t *table; size_t buckets; } Listing;
static uint32_t name_hash(const char *name) {
    uint32_t h = 2166136261u;
    while (*name) h = (h ^ (uint8_t)*name++) * 16777619u;
    return h;
}
static int listing_has(Listing *l, const char *name) {
    if (!l->buckets) return 0;
    for (size_t i = name_hash(name) & (l->buckets - 1);; i = (i + 1) & (l->buckets - 1)) {
        if (!l->table[i]) return 0;
        if (!strcmp(l->items[l->table[i] - 1].name, name)) return 1;
    }
}
static int listing_add(Listing *l, const struct dirent *entry) {
    if (l->count + 1 > l->buckets / 2) {
        size_t buckets = l->buckets ? l->buckets * 2 : 64;
        uint32_t *table = calloc(buckets, sizeof(*table));
        if (!table) return -1;
        for (size_t i = 0; i < l->count; ++i) {
            size_t j = name_hash(l->items[i].name) & (buckets - 1);
            while (table[j]) j = (j + 1) & (buckets - 1);
            table[j] = (uint32_t)i + 1;
        }
        free(l->table); l->table = table; l->buckets = buckets;
    }
    if (l->count == l->capacity) {
        size_t capacity = l->capacity ? l->capacity * 2 : 32;
        RtDirEntry *items = realloc(l->items, capacity * sizeof(*items));
        if (!items) return -1;
        l->items = items; l->capacity = capacity;
    }
    char *name = strdup(entry->d_name);
    if (!name) return -1;
    l->items[l->count] = (RtDirEntry){name, entry->d_ino, entry->d_type};
    size_t j = name_hash(name) & (l->buckets - 1);
    while (l->table[j]) j = (j + 1) & (l->buckets - 1);
    l->table[j] = (uint32_t)++l->count;
    return 0;
}
void rt_dir_entries_free(RtDirEntry *entries, size_t count) {
    for (size_t i = 0; i < count; ++i) free(entries[i].name);
    free(entries);
}
// Cache des listes de dossiers du jeu : le correcteur de casse de Source liste
// les dossiers composant par composant à chaque fichier absent (des dizaines
// de milliers de fois par minute). Invalidé à chaque écriture invitée
// (rt_paths_generation) ; les changements faits hors du jeu ne sont pas vus
// pendant la partie.
static int dir_entries_uncached(const char *path, RtDirEntry **entries, size_t *count);
typedef struct { char relative[PATH_MAX]; unsigned long generation; int error; RtDirEntry *entries; size_t count; } DirCache;
static DirCache dir_cache[64];
static unsigned dir_cache_next;
static pthread_mutex_t dir_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static RtDirEntry *copy_entries(const RtDirEntry *from, size_t count) {
    RtDirEntry *to = calloc(count ? count : 1, sizeof(*to));
    for (size_t i = 0; to && i < count; ++i) {
        to[i] = from[i];
        if (!(to[i].name = strdup(from[i].name))) { rt_dir_entries_free(to, i); return NULL; }
    }
    return to;
}
int rt_dir_entries(const char *path, RtDirEntry **entries, size_t *count) {
    char relative[PATH_MAX];
    if (rt_path_relative(path, relative) != 1) return dir_entries_uncached(path, entries, count);
    unsigned long now = rt_paths_generation();
    pthread_mutex_lock(&dir_cache_lock);
    for (unsigned i = 0; i < 64; ++i) {
        DirCache *e = &dir_cache[i];
        if (e->generation != now || strcmp(e->relative, relative)) continue;
        int error = e->error;
        RtDirEntry *copy = error ? NULL : copy_entries(e->entries, e->count);
        size_t n = e->count;
        pthread_mutex_unlock(&dir_cache_lock);
        if (error) { errno = error; return -1; }
        if (!copy) { errno = ENOMEM; return -1; }
        *entries = copy; *count = n;
        return 0;
    }
    pthread_mutex_unlock(&dir_cache_lock);
    int result = dir_entries_uncached(path, entries, count), error = result ? errno : 0;
    if (result && error != ENOENT && error != ENOTDIR) return result;
    RtDirEntry *stored = result ? NULL : copy_entries(*entries, *count);
    if (!result && !stored) return result;
    pthread_mutex_lock(&dir_cache_lock);
    DirCache *e = &dir_cache[dir_cache_next++ % 64];
    if (e->entries) rt_dir_entries_free(e->entries, e->count);
    snprintf(e->relative, sizeof(e->relative), "%s", relative);
    e->generation = now; e->error = error; e->entries = stored; e->count = result ? 0 : *count;
    pthread_mutex_unlock(&dir_cache_lock);
    if (result) errno = error;
    return result;
}
static int dir_entries_uncached(const char *path, RtDirEntry **entries, size_t *count) {
    char relative[PATH_MAX], sources[3][PATH_MAX];
    int inside = rt_path_relative(path, relative), source_count = 0;
    if (inside < 0) return -1;
    if (inside) {
        // Même règle que rt_read_path : le répertoire doit être visible.
        const char *visible = rt_read_path(path); struct stat info;
        if (!visible) return -1;
        if (stat(visible, &info)) return -1;
        if (!S_ISDIR(info.st_mode)) { errno = ENOTDIR; return -1; }
        if (rt_path_overlay(relative, sources[0])) return -1;
        source_count = 1;
        char lower[2][PATH_MAX]; int lower_count = rt_path_lower(relative, lower);
        for (int i = 0; i < lower_count; ++i) strcpy(sources[source_count++], lower[i]);
    } else {
        if (strlen(path) >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
        strcpy(sources[0], path); source_count = 1;
    }
    Listing l = {0};
    int opened = 0;
    for (int i = 0; i < source_count; ++i) {
        DIR *dir = opendir(sources[i]);
        if (!dir) {
            if (!inside || (errno != ENOENT && errno != ENOTDIR)) goto fail;
            continue;
        }
        ++opened;
        struct dirent *entry;
        for (errno = 0; (entry = readdir(dir)); errno = 0) {
            if (listing_has(&l, entry->d_name)) continue;
            // Les sources originales (i > 0) respectent les suppressions ;
            // la copie utilisateur cache seulement le répertoire des marques.
            if (inside && (i ? rt_path_hidden_name(relative, entry->d_name)
                             : !*relative && !strcmp(entry->d_name, RT_WHITEOUTS))) continue;
            if (inside && rt_path_is_game_source(sources[i]) && rt_path_windows_binary(entry->d_name)) continue;
            if (listing_add(&l, entry)) { closedir(dir); errno = ENOMEM; goto fail; }
        }
        int error = errno;
        closedir(dir);
        if (error) { errno = error; goto fail; }
    }
    if (!opened) { errno = ENOENT; goto fail; }
    free(l.table);
    *entries = l.items; *count = l.count;
    return 0;
fail:;
    int error = errno;
    free(l.table); rt_dir_entries_free(l.items, l.count);
    errno = error;
    return -1;
}
uint32_t rt_standard_stream(int index) {
    uint32_t handle = 0xd1000000u + (uint32_t)index * 0x100;
    for (Stream *s = streams; s; s = s->next) if (s->handle == handle) return handle;
    Stream *s = calloc(1, sizeof(*s));
    if (!s) return 0;
    s->handle = handle; s->file = index == 0 ? stdin : index == 1 ? stdout : stderr;
    s->standard = 1; s->next = streams; streams = s;
    return handle;
}
void rt_libc_destroy(void) {
    while (streams) {
        Stream *next = streams->next;
        if (streams->file && !streams->standard) fclose(streams->file);
        rt_dir_entries_free(streams->entries, streams->entry_count);
        free(streams); streams = next;
    }
    while (environment) { Environment *next = environment->next; free(environment->name); free(environment); environment = next; }
    next_handle = 0xd0000100u; errno_address = 0; environ_slot_count = 0;
}

static FILE *virtual_cpuinfo(void) {
    FILE *file = tmpfile();
    if (!file) return NULL;
    // Linux system view of the same one-core, 1 GHz virtual CPU as CPUID/RDTSC.
    fputs("processor\t: 0\nvendor_id\t: GenuineIntel\ncpu family\t: 6\nmodel\t\t: 15\n"
          "model name\t: Portal 2 AOT virtual CPU (Apple Silicon)\nstepping\t: 2\n"
          "cpu MHz\t\t: 1000.000\nphysical id\t: 0\nsiblings\t: 1\ncore id\t\t: 0\ncpu cores\t: 1\n"
          "flags\t\t: fpu tsc cx8 cmov mmx fxsr sse sse2\n\n", file);
    rewind(file);
    return file;
}

int rt_libc_shim(CPU *c, const char *name) {
    uint32_t a, b, n;
    if (!strcmp(name, "__errno_location")) c->eax = errno_slot(c);
    else if (!strcmp(name, "getenv")) {
        const char *key = string(c, argument(c, 0));
        const char *value = getenv(key);
        c->eax = 0;
        if (value) {
            Environment *item = environment;
            while (item && strcmp(item->name, key)) item = item->next;
            if (!item) {
                item = calloc(1, sizeof(*item));
                if (!item) return 1;
                item->name = strdup(key);
                if (!item->name) { free(item); return 1; }
                item->next = environment; environment = item;
            }
            size_t size = strlen(value) + 1;
            if (!item->value || strcmp(GPTR(item->value), value)) {
                uint32_t copy = size > UINT32_MAX ? 0 : rt_alloc((uint32_t)size);
                if (copy) { memcpy(GPTR(copy), value, size); item->value = copy; }
            }
            c->eax = item->value;
        }
    } else if (!strcmp(name, "gettimeofday")) {
        a = argument(c, 0); b = argument(c, 1);
        struct timeval tv; struct timezone tz;
        int result = gettimeofday(&tv, b ? &tz : NULL);
        if (!result && (tv.tv_sec > INT32_MAX || tv.tv_sec < INT32_MIN)) { errno = EOVERFLOW; result = -1; }
        if (!result && a) { rt_span(c, a, 8); W32(a, (uint32_t)tv.tv_sec); W32(a + 4, (uint32_t)tv.tv_usec); }
        if (!result && b) { rt_span(c, b, 8); W32(b, (uint32_t)tz.tz_minuteswest); W32(b + 4, (uint32_t)tz.tz_dsttime); }
        if (result) save_errno(c);
        c->eax = (uint32_t)result;
    } else if (!strcmp(name, "syscall")) {
        uint32_t number = argument(c, 0);
        if (number == 355) { // i386 SYS_getrandom
            a = argument(c, 1); n = argument(c, 2); b = argument(c, 3);
            if (b & ~7u) { errno = EINVAL; save_errno(c); c->eax = UINT32_MAX; }
            else { arc4random_buf(rt_write_span(c, a, n), n); c->eax = n; }
        } else if (number == 224) {
            c->eax = !c->tid || c->tid==1 ? (uint32_t)getpid() : 0x40000000u+c->tid;
        } else if (number == 240) {
            int result=rt_futex(c,argument(c,1),argument(c,2),argument(c,3),argument(c,4));
            if(result<0)save_errno(c);
            c->eax=(uint32_t)result;
        } else {
            fprintf(stderr, "unimplemented i386 syscall %u\n", number);
            rt_trap(c, c->eip, "unimplemented i386 syscall");
        }
    } else if (!strcmp(name, "clock_gettime")) {
        int id = (int32_t)argument(c, 0); b = argument(c, 1);
        clockid_t native;
        switch (id) {
        case 0: case 5: native = CLOCK_REALTIME; break;
        case 1: case 6: case 7: native = CLOCK_MONOTONIC; break;
        case 2: native = CLOCK_PROCESS_CPUTIME_ID; break;
        case 3: native = CLOCK_THREAD_CPUTIME_ID; break;
        case 4: native = CLOCK_MONOTONIC_RAW; break;
        default: errno = EINVAL; save_errno(c); c->eax = UINT32_MAX; return 1;
        }
        struct timespec value;
        int result = clock_gettime(native, &value);
        if (!result && (value.tv_sec > INT32_MAX || value.tv_sec < INT32_MIN)) { result = -1; errno = EOVERFLOW; }
        if (!result) { rt_span(c, b, 8); W32(b, (uint32_t)value.tv_sec); W32(b + 4, (uint32_t)value.tv_nsec); }
        else save_errno(c);
        c->eax = (uint32_t)result;
    } else if (!strcmp(name, "sysconf")) {
        int key = (int32_t)argument(c, 0); long result;
        switch (key) {
        case 2: result = sysconf(_SC_CLK_TCK); break;
        case 4: result = sysconf(_SC_OPEN_MAX); break;
        case 30: result = 4096; break; // virtual i386/Linux page size
        case 83: case 84: result = 1; break; // same topology as CPUID/proc
        case 85: case 86: {
            uint64_t memory = 0; size_t size = sizeof(memory);
            if (sysctlbyname("hw.memsize", &memory, &size, NULL, 0)) result = -1;
            else result = (long)((memory < UINT64_C(2147483648) ? memory : UINT64_C(2147483648)) / 4096);
            break;
        }
        default: errno = EINVAL; result = -1;
        }
        if (result == -1) save_errno(c);
        c->eax = (uint32_t)result;
    } else if (!strcmp(name, "mmap") || !strcmp(name, "mmap64")) {
        uint64_t offset = argument(c, 5);
        if (!strcmp(name, "mmap64")) offset |= (uint64_t)argument(c, 6) << 32;
        c->eax = rt_mmap(c, argument(c, 0), argument(c, 1), (int)argument(c, 2), argument(c, 3), (int32_t)argument(c, 4), offset);
        if (c->eax == UINT32_MAX) save_errno(c);
    } else if (!strcmp(name, "munmap") || !strcmp(name, "mprotect") || !strcmp(name, "madvise")) {
        a = argument(c, 0); b = argument(c, 1); int result;
        if (!strcmp(name, "munmap")) result = rt_munmap(a, b);
        else if (!strcmp(name, "mprotect")) result = rt_mprotect(a, b, (int)argument(c, 2));
        else result = rt_madvise(a, b, (int)argument(c, 2));
        if (result) save_errno(c);
        c->eax = (uint32_t)result;
    } else if (!strcmp(name, "usleep")) {
        uint32_t usec = argument(c, 0);
        struct timespec delay = {usec / 1000000u, (long)(usec % 1000000u) * 1000};
        unsigned saved=rt_guest_suspend();
        int result = nanosleep(&delay, NULL);
        int saved_errno=errno;rt_guest_resume(saved);errno=saved_errno;
        if (result) save_errno(c);
        c->eax = (uint32_t)result;
    } else if (!strcmp(name, "fopen") || !strcmp(name, "fopen64")) {
        const char *path = string(c, argument(c, 0)), *mode = string(c, argument(c, 1));
        FILE *file;
        if (!strcmp(path, "/proc/cpuinfo")) {
            if (strcmp(mode, "r") && strcmp(mode, "rb")) { errno = EACCES; file = NULL; }
            else file = virtual_cpuinfo();
        } else {
            int writing = mode[0]=='w' || mode[0]=='a' || strchr(mode,'+');
            const char *resolved=writing?rt_write_path(path,mode[0]!='w'):rt_read_path(path);
            file=resolved?fopen(resolved,mode):NULL;
        }
        if (!file) { save_errno(c); c->eax = 0; }
        else {
            c->eax = new_stream(file);
            if (!c->eax) { fclose(file); errno = ENOMEM; save_errno(c); }
        }
    } else if (!strcmp(name, "fclose")) {
        Stream *s = lookup(c, argument(c, 0), 0);
        int result = s->standard ? fflush(s->file) : fclose(s->file);
        s->file = NULL;
        if (result) save_errno(c);
        c->eax = (uint32_t)result;
    } else if (!strcmp(name, "fgets")) {
        a = argument(c, 0); n = argument(c, 1);
        Stream *s = lookup(c, argument(c, 2), 0);
        rt_write_span(c, a, n);
        char *result = fgets(GPTR(a), (int)n, s->file);
        if (!result && ferror(s->file)) save_errno(c);
        c->eax = result ? a : 0;
    } else if (!strcmp(name, "fputs")) {
        const char *value = string(c, argument(c, 0));
        Stream *s = lookup(c, argument(c, 1), 0);
        int result = fputs(value, s->file); if (result == EOF) save_errno(c);
        c->eax = (uint32_t)result;
    } else if (!strcmp(name, "fputc") || !strcmp(name, "putc")) {
        Stream *s = lookup(c, argument(c, 1), 0);
        int result = fputc((int)argument(c, 0), s->file); if (result == EOF) save_errno(c);
        c->eax = (uint32_t)result;
    } else if (!strcmp(name, "fgetc") || !strcmp(name, "getc")) {
        Stream *s = lookup(c, argument(c, 0), 0);
        int result = fgetc(s->file); if (result == EOF && ferror(s->file)) save_errno(c);
        c->eax = (uint32_t)result;
    } else if (!strcmp(name, "feof") || !strcmp(name, "ferror") || !strcmp(name, "clearerr") ||
               !strcmp(name, "fileno") || !strcmp(name, "fileno_unlocked") || !strcmp(name, "rewind")) {
        Stream *s=lookup(c,argument(c,0),0);
        if(!strcmp(name,"feof"))c->eax=(uint32_t)feof(s->file);
        else if(!strcmp(name,"ferror"))c->eax=(uint32_t)ferror(s->file);
        else if(!strcmp(name,"clearerr"))clearerr(s->file);
        else if(!strcmp(name,"rewind"))rewind(s->file);
        else {int result=fileno(s->file);if(result<0)save_errno(c);c->eax=(uint32_t)result;}
    } else if (!strcmp(name, "ftell") || !strcmp(name, "ftello") || !strcmp(name, "ftello64")) {
        off_t result=ftello(lookup(c,argument(c,0),0)->file);
        int wide=!strcmp(name,"ftello64");
        if(!wide && result>INT32_MAX){result=-1;errno=EOVERFLOW;}
        if(result<0)save_errno(c);
        c->eax=(uint32_t)result;if(wide)c->edx=(uint32_t)((uint64_t)result>>32);
    } else if (!strcmp(name, "fseek") || !strcmp(name, "fseeko") || !strcmp(name, "fseeko64")) {
        Stream *s=lookup(c,argument(c,0),0);
        int wide=!strcmp(name,"fseeko64"), whence=(int)argument(c,wide?3:2);
        int64_t offset=wide?(int64_t)(((uint64_t)argument(c,2)<<32)|argument(c,1)):(int32_t)argument(c,1);
        int result=fseeko(s->file,offset,whence);if(result)save_errno(c);c->eax=(uint32_t)result;
    } else if (!strcmp(name, "fflush")) {
        a = argument(c, 0);
        int result = fflush(a ? lookup(c, a, 0)->file : NULL); if (result) save_errno(c);
        c->eax = (uint32_t)result;
    } else if (!strcmp(name, "fread") || !strcmp(name, "fwrite")) {
        a = argument(c, 0); b = argument(c, 1); n = argument(c, 2);
        Stream *s = lookup(c, argument(c, 3), 0);
        void *buffer = !strcmp(name, "fread") ? rt_write_span(c, a, (uint64_t)b * n) : rt_span(c, a, (uint64_t)b * n);
        size_t count = !strcmp(name, "fread") ? fread(buffer, b, n, s->file) : fwrite(buffer, b, n, s->file);
        if (count < n && ferror(s->file)) save_errno(c);
        c->eax = (uint32_t)count;
    } else if (!strcmp(name, "open") || !strcmp(name, "open64") || !strcmp(name, "__open_2") || !strcmp(name, "__open64_2")) {
        const char *path = string(c, argument(c, 0)); uint32_t flags = argument(c, 1);
        int native = (int)(flags & 3);
        if (flags & 0x40) native |= O_CREAT;
        if (flags & 0x80) native |= O_EXCL;
        if (flags & 0x100) native |= O_NOCTTY;
        if (flags & 0x200) native |= O_TRUNC;
        if (flags & 0x400) native |= O_APPEND;
        if (flags & 0x800) native |= O_NONBLOCK;
        if (flags & 0x1000) native |= O_SYNC;
        if (flags & 0x10000) native |= O_DIRECTORY;
        if (flags & 0x20000) native |= O_NOFOLLOW;
        if (flags & 0x80000) native |= O_CLOEXEC;
        uint32_t supported = 3u|0x40|0x80|0x100|0x200|0x400|0x800|0x1000|0x8000|0x10000|0x20000|0x80000|0x100000;
        if (flags & ~supported) { errno = ENOTSUP; save_errno(c); c->eax = UINT32_MAX; return 1; }
        if ((flags & 0x40) && strstr(name, "_2")) rt_trap(c,c->eip,"invalid fortified open flags");
        mode_t mode = flags & 0x40 ? (mode_t)argument(c, 2) : 0;
        if((flags & 0xc0)==0xc0) {
            const char *existing=rt_read_path(path);
            if(!existing || !access(existing,F_OK)) {
                if(existing)errno=EEXIST;
                save_errno(c);c->eax=UINT32_MAX;return 1;
            }
        }
        const char *resolved=(flags & (3|0x40|0x200|0x400))?rt_write_path(path,!(flags & (0x80|0x200))):rt_read_path(path);
        int result = resolved?open(resolved, native, mode):-1; if (result < 0) save_errno(c);
        c->eax = (uint32_t)result;
    } else if (!strcmp(name, "chdir")) {
        const char *resolved=rt_read_path(string(c,argument(c,0)));
        int result=resolved?chdir(resolved):-1;
        if(result)save_errno(c);
        c->eax=(uint32_t)result;
    } else if (!strcmp(name, "getcwd")) {
        a=argument(c,0); n=argument(c,1);
        char *result=getcwd(NULL,0);
        if(!result){save_errno(c);c->eax=0;}
        else {
            size_t length=strlen(result)+1;
            if(!a){ n=n?n:(uint32_t)length; a=rt_alloc(n); if(!a)errno=ENOMEM; }
            if(!a || length>n){if(a)errno=ERANGE;save_errno(c);c->eax=0;}
            else {memcpy(rt_write_span(c,a,length),result,length);c->eax=a;}
            free(result);
        }
    } else if (!strcmp(name, "readlink")) {
        const char *path=string(c,argument(c,0)); a=argument(c,1); n=argument(c,2);
        ssize_t result;
        char own_exe[48]; snprintf(own_exe,sizeof(own_exe),"/proc/%d/exe",(int)getpid());
        if(!strcmp(path,"/proc/self/exe") || !strcmp(path,own_exe)) {
            const char *program=rt_program_path(); size_t count=strlen(program);if(count>n)count=n;
            memcpy(rt_write_span(c,a,count),program,count);result=(ssize_t)count;
        } else {const char *resolved=rt_read_path(path);result=resolved?readlink(resolved,rt_write_span(c,a,n),n):-1;}
        if(result<0)save_errno(c);
        c->eax=(uint32_t)result;
    } else if (!strcmp(name, "close")) {
        int result = close((int32_t)argument(c, 0)); if (result) save_errno(c);
        c->eax = (uint32_t)result;
    } else if (!strcmp(name, "read") || !strcmp(name, "write")) {
        int fd = (int32_t)argument(c, 0); a=argument(c,1); n=argument(c,2);
        if (n > 0x7ffff000u) n=0x7ffff000u;
        ssize_t result;
        if (!strcmp(name,"read")) result=read(fd,rt_write_span(c,a,n),n);
        else result=write(fd,rt_span(c,a,n),n);
        if (result < 0) save_errno(c);
        c->eax=(uint32_t)result;
    } else if (!strcmp(name, "lseek") || !strcmp(name, "lseek64")) {
        int fd=(int32_t)argument(c,0); int64_t offset=(int32_t)argument(c,1); int whence=(int)argument(c,2);
        int wide=!strcmp(name,"lseek64");
        if (wide) { offset=(int64_t)(((uint64_t)argument(c,2)<<32)|argument(c,1)); whence=(int)argument(c,3); }
        off_t result=lseek(fd,(off_t)offset,whence);
        if (!wide && result > INT32_MAX) { result=-1; errno=EOVERFLOW; }
        if (result < 0) save_errno(c);
        c->eax=(uint32_t)result; if (wide) c->edx=(uint32_t)((uint64_t)result>>32);
    } else if (!strcmp(name, "access")) {
        const char *path = string(c, argument(c, 0));
        int result;
        if (!strcmp(path, "/proc/cpuinfo")) { result = (argument(c, 1) & 3) ? -1 : 0; if (result) errno = EACCES; }
        else { const char *resolved=rt_read_path(path); result=resolved?access(resolved,(int)argument(c,1)):-1; }
        if (result) save_errno(c);
        c->eax = (uint32_t)result;
    } else if (!strcmp(name, "opendir")) {
        RtDirEntry *entries; size_t count;
        if (rt_dir_entries(string(c, argument(c, 0)), &entries, &count)) { save_errno(c); c->eax = 0; return 1; }
        Stream *s = new_stream_record();
        if (!s) { rt_dir_entries_free(entries, count); errno = ENOMEM; save_errno(c); c->eax = 0; return 1; }
        s->directory = 1; s->entries = entries; s->entry_count = count;
        c->eax = s->handle;
    } else if (!strcmp(name, "closedir")) {
        Stream *s = lookup(c, argument(c, 0), 1);
        rt_dir_entries_free(s->entries, s->entry_count);
        s->entries = NULL; s->entry_count = 0; s->directory = 0;
        if (s->dirent) rt_free(s->dirent);
        if (s->dirent64) rt_free(s->dirent64);
        s->dirent = s->dirent64 = 0;
        c->eax = 0;
    } else if (!strcmp(name, "readdir") || !strcmp(name, "readdir64")) {
        // glibc i386 : dirent {ino32, off32, reclen16, type8, name[256]} (268 octets),
        // dirent64 {ino64, off64, reclen16, type8, name[256]} (280 octets).
        Stream *s = lookup(c, argument(c, 0), 1);
        int wide = !strcmp(name, "readdir64");
        c->eax = 0;
        if (s->position >= s->entry_count) return 1; // fin : errno inchangé
        RtDirEntry *entry = &s->entries[s->position];
        size_t len = strlen(entry->name);
        if (len > 255) { errno = ENAMETOOLONG; save_errno(c); return 1; }
        if (!wide && entry->ino > UINT32_MAX) { ++s->position; errno = EOVERFLOW; save_errno(c); return 1; }
        uint32_t *slot = wide ? &s->dirent64 : &s->dirent, size = wide ? 280 : 268;
        if (!*slot) *slot = rt_alloc(size);
        if (!*slot) { errno = ENOMEM; save_errno(c); return 1; }
        uint32_t address = *slot, header = wide ? 19 : 11;
        memset(GPTR(address), 0, size);
        ++s->position;
        if (wide) { W64(address, entry->ino); W64(address + 8, s->position); }
        else { W32(address, (uint32_t)entry->ino); W32(address + 4, (uint32_t)s->position); }
        W16(address + header - 3, (uint32_t)((header + len + 1 + (wide ? 7 : 3)) & ~(size_t)(wide ? 7 : 3)));
        W8(address + header - 1, entry->type);
        memcpy(GPTR(address + header), entry->name, len + 1); c->eax = address;
    } else if (!strcmp(name, "strpbrk") || !strcmp(name, "strstr") || !strcmp(name, "strcasestr") || !strcmp(name, "strchr") || !strcmp(name, "strrchr")) {
        a = argument(c, 0); const char *source = string(c, a); const char *result;
        if (!strcmp(name, "strchr")) result = strchr(source, (int)argument(c, 1));
        else if (!strcmp(name, "strrchr")) result = strrchr(source, (int)argument(c, 1));
        else {
            const char *other = string(c, argument(c, 1));
            result = !strcmp(name, "strpbrk") ? strpbrk(source, other) : !strcmp(name, "strstr") ? strstr(source, other) : strcasestr(source, other);
        }
        c->eax = result ? a + (uint32_t)(result - source) : 0;
    } else if (!strcmp(name, "strcasecmp") || !strcmp(name, "strncasecmp")) {
        const char *lhs = string(c, argument(c, 0)), *rhs = string(c, argument(c, 1));
        c->eax = (uint32_t)(!strcmp(name, "strcasecmp") ? strcasecmp(lhs, rhs) : strncasecmp(lhs, rhs, argument(c, 2)));
    } else if (!strcmp(name, "strdup")) {
        a = argument(c, 0); n = rt_string_length(c, a);
        c->eax = rt_alloc(n + 1);
        if (c->eax) memcpy(GPTR(c->eax), GPTR(a), (size_t)n + 1);
        else { errno = ENOMEM; save_errno(c); }
    } else if (!strcmp(name, "strcat") || !strcmp(name, "strncat")) {
        a = argument(c, 0); b = argument(c, 1);
        uint32_t destination_length = rt_string_length(c, a), count = 0;
        uint32_t limit = !strcmp(name, "strcat") ? UINT32_MAX : argument(c, 2);
        while (count < limit) {
            rt_span(c, b, (uint64_t)count + 1);
            if (!R8(b + count)) break;
            ++count;
        }
        rt_write_span(c, a, (uint64_t)destination_length + count + 1);
        memmove(GPTR(a + destination_length), GPTR(b), count);
        W8(a + destination_length + count, 0); c->eax = a;
    } else if (!strcmp(name, "strtod")) {
        a = argument(c, 0); b = argument(c, 1);
        const char *source = string(c, a); char *end;
        errno = 0; double value = strtod(source, &end);
        if (b) W32(b, a + (uint32_t)(end - source));
        if (errno) save_errno(c);
        fpush(c, value);
    } else if (!strcmp(name, "strerror")) {
        int code = (int32_t)argument(c, 0), native = rt_darwin_errno(code);
        if (native) code = native; else if (code > 0) code = -1; // message « Unknown error »
        const char *message = strerror(code); size_t size = strlen(message) + 1;
        c->eax = rt_alloc((uint32_t)size);
        if (c->eax) memcpy(GPTR(c->eax), message, size);
    } else if (!strcmp(name, "tolower") || !strcmp(name, "toupper")) {
        int value = (int32_t)argument(c, 0);
        if (value < -1 || value > 255) rt_trap(c, c->eip, "invalid ctype input");
        c->eax = (uint32_t)(!strcmp(name, "tolower") ? tolower(value) : toupper(value));
    } else if (!strcmp(name, "getppid")) {
        c->eax = (uint32_t)getppid();
    } else if (!strcmp(name, "strtol") || !strcmp(name, "strtoul")) {
        a = argument(c, 0); b = argument(c, 1);
        const char *source = string(c, a); char *end;
        int base = (int32_t)argument(c, 2);
        if (base && (base < 2 || base > 36)) { errno = EINVAL; save_errno(c); c->eax = 0; return 1; }
        errno = 0;
        if (!strcmp(name, "strtol")) {
            long value = strtol(source, &end, base);
            if (value > INT32_MAX) { value = INT32_MAX; errno = ERANGE; }
            if (value < INT32_MIN) { value = INT32_MIN; errno = ERANGE; }
            c->eax = (uint32_t)value;
        } else {
            unsigned long value = strtoul(source, &end, base);
            const char *sign = source; while (isspace((unsigned char)*sign)) ++sign;
            unsigned long magnitude = *sign == '-' ? 0ul - value : value;
            if (errno == ERANGE || magnitude > UINT32_MAX) { c->eax = UINT32_MAX; errno = ERANGE; }
            else c->eax = (uint32_t)value;
        }
        if (b) { rt_span(c, b, 4); W32(b, a + (uint32_t)(end - source)); }
        if (errno) save_errno(c);
    } else return 0;
    return 1;
}

// Objets invités `environ` : reconstruits après chaque setenv, comme glibc
// qui modifie le tableau visible par le programme. Les anciennes chaînes ne
// sont pas libérées (glibc ne libère pas non plus les valeurs remplacées).
int rt_environ_sync(void) {
    if (!environ_slot_count) return 0;
    extern char **environ;
    size_t count = 0; while (environ[count]) ++count;
    uint32_t array = rt_alloc((uint32_t)((count + 1) * 4));
    if (!array) return -1;
    for (size_t i = 0; i < count; ++i) {
        size_t length = strlen(environ[i]) + 1;
        uint32_t copy = rt_alloc((uint32_t)length);
        if (!copy) return -1;
        memcpy(GPTR(copy), environ[i], length); W32(array + (uint32_t)i * 4, copy);
    }
    W32(array + (uint32_t)count * 4, 0);
    for (size_t i = 0; i < environ_slot_count; ++i) W32(environ_slots[i], array);
    return 0;
}
int rt_init_import_data(const char *name, uint32_t address, uint32_t size) {
    if (rt_map(address, size)) return -1;
    if (!strcmp(name, "stdin") || !strcmp(name, "stdout") || !strcmp(name, "stderr")) {
        int index = !strcmp(name, "stdin") ? 0 : !strcmp(name, "stdout") ? 1 : 2;
        uint32_t handle = rt_standard_stream(index);
        if (!handle) return -1;
        memset(GPTR(address), 0, size); W32(address, handle);
        return 0;
    }
    if (!strcmp(name, "environ") || !strcmp(name, "__environ") || !strcmp(name, "_environ")) {
        if (environ_slot_count == sizeof(environ_slots) / sizeof(*environ_slots)) return -1;
        environ_slots[environ_slot_count++] = address;
        return rt_environ_sync();
    }
    if (!strcmp(name, "timezone") || !strcmp(name, "daylight"))
        return rt_time_data_import(name, address);   // time.c : rafraîchi par tzset
    fprintf(stderr, "unimplemented imported object: %s\n", name);
    return -1;
}
