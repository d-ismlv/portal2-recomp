// Infrastructure commune des ponts vers les bibliothèques natives :
// adresses invitées dynamiques (GL, OpenAL, dlsym) et handles opaques pour
// les pointeurs hôtes 64 bits, jamais tronqués vers une adresse invitée.
#include "rt.h"
#include <pthread.h>
#include <stdlib.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char **dynamic_names;
static uint32_t dynamic_count;

uint32_t rt_dynamic_import(const char *name) {
    if (!name || !*name) return 0;
    pthread_mutex_lock(&lock);
    uint32_t index = 0;
    while (index < dynamic_count && strcmp(dynamic_names[index], name)) ++index;
    if (index == dynamic_count) {
        if (dynamic_count == RT_DYNAMIC_IMPORT_LIMIT) { pthread_mutex_unlock(&lock); return 0; }
        char **grown = realloc(dynamic_names, (dynamic_count + 1) * sizeof(*grown));
        char *copy = strdup(name);
        if (!grown || !copy) { free(copy); if (grown) dynamic_names = grown; pthread_mutex_unlock(&lock); return 0; }
        dynamic_names = grown;
        dynamic_names[dynamic_count++] = copy;
    }
    pthread_mutex_unlock(&lock);
    return RT_DYNAMIC_IMPORT_BASE + index * RT_DYNAMIC_IMPORT_STRIDE;
}

const char *rt_dynamic_import_name(uint32_t address) {
    if (address < RT_DYNAMIC_IMPORT_BASE || (address - RT_DYNAMIC_IMPORT_BASE) % RT_DYNAMIC_IMPORT_STRIDE)
        return NULL;
    uint32_t index = (address - RT_DYNAMIC_IMPORT_BASE) / RT_DYNAMIC_IMPORT_STRIDE;
    pthread_mutex_lock(&lock);
    const char *name = index < dynamic_count ? dynamic_names[index] : NULL;
    pthread_mutex_unlock(&lock);
    return name;
}

typedef struct { void *host; uint32_t kind; } Handle;
static Handle *handles;
static uint32_t handle_capacity;

uint32_t rt_handle_new(void *host, uint32_t kind) {
    if (!host || !kind) return 0;
    pthread_mutex_lock(&lock);
    uint32_t index = 0;
    while (index < handle_capacity && handles[index].kind) ++index;
    if (index == handle_capacity) {
        uint32_t capacity = handle_capacity ? handle_capacity * 2 : 256;
        if (capacity > RT_HANDLE_LIMIT) { pthread_mutex_unlock(&lock); return 0; }
        Handle *grown = realloc(handles, capacity * sizeof(*grown));
        if (!grown) { pthread_mutex_unlock(&lock); return 0; }
        memset(grown + handle_capacity, 0, (capacity - handle_capacity) * sizeof(*grown));
        handles = grown; handle_capacity = capacity;
    }
    handles[index] = (Handle){host, kind};
    pthread_mutex_unlock(&lock);
    return RT_HANDLE_BASE + index * RT_HANDLE_STRIDE;
}

static Handle *slot(uint32_t handle) {
    if (handle < RT_HANDLE_BASE || (handle - RT_HANDLE_BASE) % RT_HANDLE_STRIDE) return NULL;
    uint32_t index = (handle - RT_HANDLE_BASE) / RT_HANDLE_STRIDE;
    return index < handle_capacity && handles[index].kind ? &handles[index] : NULL;
}

void *rt_handle_get(uint32_t handle, uint32_t kind) {
    pthread_mutex_lock(&lock);
    Handle *h = slot(handle);
    void *host = h && h->kind == kind ? h->host : NULL;
    pthread_mutex_unlock(&lock);
    return host;
}

uint32_t rt_handle_find(const void *host, uint32_t kind) {
    pthread_mutex_lock(&lock);
    uint32_t result = 0;
    for (uint32_t i = 0; i < handle_capacity && !result; ++i)
        if (handles[i].kind == kind && handles[i].host == host)
            result = RT_HANDLE_BASE + i * RT_HANDLE_STRIDE;
    pthread_mutex_unlock(&lock);
    return result;
}

void *rt_handle_free(uint32_t handle, uint32_t kind) {
    pthread_mutex_lock(&lock);
    Handle *h = slot(handle);
    void *host = NULL;
    if (h && h->kind == kind) { host = h->host; *h = (Handle){0}; }
    pthread_mutex_unlock(&lock);
    return host;
}
