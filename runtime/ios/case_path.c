// Chemins insensibles à la casse sur iOS. Le correcteur de casse de Source met
// les chemins en minuscules ; macOS (APFS insensible) les accepte, iOS non (le
// bac à sable compare la casse, et un vrai iPhone a un système de fichiers
// sensible à la casse). paths.c canonicalise déjà la racine (jeu, dépôt Linux,
// données utilisateur) ; ici chaque composant qui suit est remplacé par le nom
// réel de l'entrée du répertoire, trouvé sans tenir compte de la casse.
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

// Cache des contenus de répertoires (chemin exact -> noms). Une entrée créée
// après coup avec une autre casse n'est pas vue : rare, et le nom tel quel est
// alors gardé (l'appel échoue comme il le ferait sans cache).
typedef struct Directory { char *path; char **names; size_t count; struct Directory *next; } Directory;
#define BUCKETS 4096u
static Directory *buckets[BUCKETS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned hash(const char *s) { unsigned h = 2166136261u; while (*s) h = (h ^ (unsigned char)*s++) * 16777619u; return h; }

static Directory *listing(const char *path) {
    unsigned b = hash(path) % BUCKETS;
    for (Directory *d = buckets[b]; d; d = d->next) if (!strcmp(d->path, path)) return d;
    DIR *dir = opendir(path);
    if (!dir) return NULL;
    Directory *d = calloc(1, sizeof(*d));
    if (!d) { closedir(dir); return NULL; }
    d->path = strdup(path);
    size_t capacity = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (d->count == capacity) {
            capacity = capacity ? capacity * 2 : 32;
            char **grown = realloc(d->names, capacity * sizeof(*grown));
            if (!grown) break;
            d->names = grown;
        }
        d->names[d->count++] = strdup(entry->d_name);
    }
    closedir(dir);
    d->next = buckets[b]; buckets[b] = d;
    return d;
}

static const char *match(const char *directory, const char *name) {
    pthread_mutex_lock(&lock);
    Directory *d = listing(directory);
    const char *found = NULL;
    for (size_t i = 0; d && i < d->count && !found; ++i) if (!strcasecmp(d->names[i], name)) found = d->names[i];
    pthread_mutex_unlock(&lock);
    return found;   // les noms du cache ne sont jamais libérés
}

// canonical : chemin absolu dont les `trusted` premiers octets sont exacts.
const char *rt_ios_case_path(const char *canonical, size_t trusted) {
    static _Thread_local char ring[4][PATH_MAX];
    static _Thread_local unsigned next;
    char *out = ring[next++ & 3];
    struct stat info;
    if (!lstat(canonical, &info)) return canonical;   // déjà exact
    size_t length = strlen(canonical);
    if (trusted > length || length >= PATH_MAX) return canonical;
    memcpy(out, canonical, trusted); out[trusted] = 0;
    const char *rest = canonical + trusted;
    int exact = 1;
    while (*rest) {
        while (*rest == '/') ++rest;
        if (!*rest) break;
        const char *end = strchr(rest, '/'); if (!end) end = rest + strlen(rest);
        char part[NAME_MAX + 1];
        size_t n = (size_t)(end - rest);
        if (n > NAME_MAX) return canonical;
        memcpy(part, rest, n); part[n] = 0;
        size_t used = strlen(out);
        if (used + 1 + n >= PATH_MAX) return canonical;
        const char *real = NULL;
        if (exact) {
            out[used] = '/'; strcpy(out + used + 1, part);
            if (lstat(out, &info)) { out[used] = 0; real = match(used ? out : "/", part); }
            else { rest = end; continue; }
        }
        if (!real) exact = 0;   // introuvable : la suite est gardée telle quelle
        out[used] = '/'; strcpy(out + used + 1, real ? real : part);
        rest = end;
    }
    return out;
}
#endif
