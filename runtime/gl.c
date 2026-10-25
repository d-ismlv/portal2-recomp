// Pont OpenGL : libtogl obtient ses fonctions GL par SDL_GL_GetProcAddress,
// qui délègue à rt_gl_proc_address. Chaque fonction prise en charge reçoit
// une adresse invitée (rt_dynamic_import) ; l'appel invité arrive dans
// gl_shim, qui exécute le thunk de runtime/gl_generated.c (généré par
// tools/gen_gl.py) ou une fonction manuelle ci-dessous.
//
// Comme SDL sous macOS, une adresse est rendue pour tout symbole exporté par
// OpenGL.framework, même si le contexte courant (legacy 2.1) ne le prend pas
// en charge : l'appel répond alors GL_INVALID_OPERATION, comme sur un vrai
// Mac. Voir docs/gl-audit.md.
//
// Pointeurs invités : la mémoire invitée est projetée dans l'espace hôte
// (M + adresse), donc un pointeur invité validé devient GPTR(adresse) et
// reste valide après l'appel (tableaux de sommets clients, client storage).
// Seuls les pointeurs hôtes renvoyés par GL (glMapBuffer, glGetString,
// GLsync) demandent une copie ou un handle.
#include "rt.h"
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>

typedef int (*rt_gl_thunk)(CPU *c, void *fn);
typedef struct { const char *name; rt_gl_thunk thunk; } RtGlEntry;
extern const RtGlEntry rt_gl_entries[];
extern const unsigned rt_gl_entry_count;

#define GL_SYNC_KIND 0x474c5359u  // 'GLSY'
// ARB_map_buffer_range (gl3.h, absent de glext.h).
#define GL_MAP_READ_BIT 0x0001
#define GL_MAP_WRITE_BIT 0x0002
#define GL_MAP_INVALIDATE_RANGE_BIT 0x0004
#define GL_MAP_INVALIDATE_BUFFER_BIT 0x0008
#define GL_MAP_FLUSH_EXPLICIT_BIT 0x0010

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void *library;
static void **hosts;  // pointeur hôte par entrée, résolu à la demande

static void open_library(void) {
    library = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY | RTLD_LOCAL);
    hosts = calloc(rt_gl_entry_count, sizeof(*hosts));
}

static int compare_entry(const void *key, const void *entry) {
    return strcmp(key, ((const RtGlEntry *)entry)->name);
}
static const RtGlEntry *find_entry(const char *name) {
    if (!name || name[0] != 'g' || name[1] != 'l') return NULL;
    return bsearch(name, rt_gl_entries, rt_gl_entry_count, sizeof(*rt_gl_entries), compare_entry);
}
static void *host_function(const RtGlEntry *entry) {
    pthread_once(&once, open_library);
    if (!library || !hosts) return NULL;
    size_t index = (size_t)(entry - rt_gl_entries);
    void *fn = __atomic_load_n(&hosts[index], __ATOMIC_ACQUIRE);
    if (!fn) {
        fn = dlsym(library, entry->name);
        __atomic_store_n(&hosts[index], fn, __ATOMIC_RELEASE);
    }
    return fn;
}

uint32_t rt_gl_proc_address(const char *name) {
    const RtGlEntry *entry = find_entry(name);
    return entry && host_function(entry) ? rt_dynamic_import(name) : 0;
}

static int gl_shim(CPU *c, const char *name) {
    const RtGlEntry *entry = find_entry(name);
    if (!entry) return 0;
    void *fn = host_function(entry);
    if (!fn) rt_trap(c, c->eip, "fonction GL absente de OpenGL.framework");
    // Sans contexte courant, le GL d'Apple déréférence un contexte nul.
    if (!CGLGetCurrentContext()) rt_trap(c, c->eip, "appel GL sans contexte courant");
    return entry->thunk(c, fn);
}
RT_SHIM_PROVIDER(gl_shim)

// ---- Arguments i386 cdecl ----
uint32_t rt_gl_u32(CPU *c, unsigned slot) { return R32(c->esp + 4 + 4 * slot); }
float rt_gl_f32(CPU *c, unsigned slot) { return RF(c->esp + 4 + 4 * slot); }
double rt_gl_f64(CPU *c, unsigned slot) { return RD(c->esp + 4 + 4 * slot); }
uint64_t rt_gl_u64(CPU *c, unsigned slot) { return R64(c->esp + 4 + 4 * slot); }
uint64_t rt_gl_n(int64_t n) { return n < 0 ? 0 : (uint64_t)n; }
void *rt_gl_handle_arg(uint32_t handle) { return (void *)(uintptr_t)handle; }
uint32_t rt_gl_handle_ret(CPU *c, void *handle) {
    // GLhandleARB vaut void * sur macOS, unsigned int sur Linux.
    if ((uintptr_t)handle > UINT32_MAX) rt_trap(c, c->eip, "GLhandleARB hôte hors 32 bits");
    return (uint32_t)(uintptr_t)handle;
}
uint32_t rt_gl_iptr_ret(CPU *c, intptr_t value) {
    if (value < INT32_MIN || value > INT32_MAX) rt_trap(c, c->eip, "GLintptr hôte hors 32 bits");
    return (uint32_t)(int32_t)value;
}

// ---- Pointeurs invités ----
void *rt_gl_in(CPU *c, uint32_t ptr, uint64_t size, int nullable) {
    if (!ptr) {
        if (nullable || !size) return NULL;
        rt_trap(c, c->eip, "pointeur GL nul");
    }
    return rt_span(c, ptr, size);
}
void *rt_gl_out(CPU *c, uint32_t ptr, uint64_t size, int nullable) {
    if (!ptr) {
        if (nullable || !size) return NULL;
        rt_trap(c, c->eip, "pointeur GL de sortie nul");
    }
    return rt_write_span(c, ptr, size);
}
const char *rt_gl_str(CPU *c, uint32_t ptr) { rt_string_length(c, ptr); return GPTR(ptr); }

static GLint integer(GLenum pname) { GLint v = 0; glGetIntegerv(pname, &v); return v; }

// Pointeur de tableau de sommets : décalage dans le VBO lié, sinon tableau
// client en mémoire invitée (lu par GL au moment du tracé, taille inconnue ici).
const void *rt_gl_array(CPU *c, uint32_t ptr) {
    (void)c;
    if (integer(GL_ARRAY_BUFFER_BINDING)) return (const void *)(uintptr_t)ptr;
    return ptr ? GPTR(ptr) : NULL;
}

const void *rt_gl_indices(CPU *c, uint32_t ptr, int32_t count, uint32_t type) {
    if (integer(GL_ELEMENT_ARRAY_BUFFER_BINDING)) return (const void *)(uintptr_t)ptr;
    uint64_t unit = type == GL_UNSIGNED_BYTE ? 1 : type == GL_UNSIGNED_SHORT ? 2 : type == GL_UNSIGNED_INT ? 4 : 0;
    if (!unit) return ptr ? GPTR(ptr) : NULL;  // type invalide : GL refuse sans lire
    return rt_gl_in(c, ptr, rt_gl_n(count) * unit, 0);
}

// Octets par pixel d'un couple format/type (0 : inconnu du pont).
static uint64_t pixel_bytes(uint32_t format, uint32_t type) {
    switch (type) {
    case GL_UNSIGNED_BYTE_3_3_2: case GL_UNSIGNED_BYTE_2_3_3_REV: return 1;
    case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_5_6_5_REV:
    case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_4_4_4_4_REV:
    case GL_UNSIGNED_SHORT_5_5_5_1: case GL_UNSIGNED_SHORT_1_5_5_5_REV:
    case GL_UNSIGNED_SHORT_8_8_APPLE: case GL_UNSIGNED_SHORT_8_8_REV_APPLE: return 2;
    case GL_UNSIGNED_INT_8_8_8_8: case GL_UNSIGNED_INT_8_8_8_8_REV:
    case GL_UNSIGNED_INT_10_10_10_2: case GL_UNSIGNED_INT_2_10_10_10_REV:
    case GL_UNSIGNED_INT_24_8_EXT: case GL_UNSIGNED_INT_10F_11F_11F_REV_EXT:
    case GL_UNSIGNED_INT_5_9_9_9_REV_EXT: return 4;
    case GL_FLOAT_32_UNSIGNED_INT_24_8_REV: return 8;
    }
    uint64_t size;
    switch (type) {
    case GL_UNSIGNED_BYTE: case GL_BYTE: size = 1; break;
    case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT_ARB: size = 2; break;
    case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: size = 4; break;
    default: return 0;
    }
    switch (format) {
    case GL_RED: case GL_GREEN: case GL_BLUE: case GL_ALPHA: case GL_LUMINANCE:
    case GL_DEPTH_COMPONENT: case GL_STENCIL_INDEX: case GL_COLOR_INDEX: return size;
    case GL_LUMINANCE_ALPHA: case GL_RG: return 2 * size;
    case GL_RGB: case GL_BGR: return 3 * size;
    case GL_RGBA: case GL_BGRA: case GL_ABGR_EXT: return 4 * size;
    }
    return 0;
}

typedef struct { GLenum row_length, skip_rows, skip_pixels, alignment, image_height, skip_images, row_bytes, binding; } PixelStore;
static const PixelStore unpack_store = {GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_ROWS, GL_UNPACK_SKIP_PIXELS, GL_UNPACK_ALIGNMENT,
                                        GL_UNPACK_IMAGE_HEIGHT, GL_UNPACK_SKIP_IMAGES, GL_UNPACK_ROW_BYTES_APPLE, GL_PIXEL_UNPACK_BUFFER_BINDING};
static const PixelStore pack_store = {GL_PACK_ROW_LENGTH, GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS, GL_PACK_ALIGNMENT,
                                      GL_PACK_IMAGE_HEIGHT, GL_PACK_SKIP_IMAGES, GL_PACK_ROW_BYTES_APPLE, GL_PIXEL_PACK_BUFFER_BINDING};

// Taille lue/écrite par GL pour une image w×h×d (d = 0 : image 2D, sans
// SKIP_IMAGES ni IMAGE_HEIGHT), selon l'état de stockage des pixels.
static uint64_t image_bytes(CPU *c, const PixelStore *s, int32_t w, int32_t h, int32_t d, uint32_t format, uint32_t type) {
    int is3d = d != 0;
    if (!is3d) d = 1;
    if (w <= 0 || h <= 0 || d <= 0) return 0;
    uint64_t pixel = pixel_bytes(format, type);
    if (!pixel) rt_trap(c, c->eip, "format/type de pixels GL non pris en charge par le pont");
    uint64_t row_length = integer(s->row_length) > 0 ? (uint64_t)integer(s->row_length) : (uint64_t)w;
    uint64_t alignment = integer(s->alignment) > 0 ? (uint64_t)integer(s->alignment) : 1;
    uint64_t row = (row_length * pixel + alignment - 1) / alignment * alignment;
    if (integer(s->row_bytes) > 0) row = (uint64_t)integer(s->row_bytes);  // APPLE_row_bytes
    uint64_t rows = integer(s->image_height) > 0 && is3d ? (uint64_t)integer(s->image_height) : (uint64_t)h;
    uint64_t image = rows * row;
    uint64_t start = (uint64_t)(integer(s->skip_rows) > 0 ? integer(s->skip_rows) : 0) * row +
                     (uint64_t)(integer(s->skip_pixels) > 0 ? integer(s->skip_pixels) : 0) * pixel;
    if (is3d) start += (uint64_t)(integer(s->skip_images) > 0 ? integer(s->skip_images) : 0) * image;
    return start + (uint64_t)(d - 1) * image + (uint64_t)(h - 1) * row + (uint64_t)w * pixel;
}

const void *rt_gl_unpack(CPU *c, uint32_t ptr, int32_t w, int32_t h, int32_t d, uint32_t format, uint32_t type, int nullable) {
    if (integer(GL_PIXEL_UNPACK_BUFFER_BINDING)) return (const void *)(uintptr_t)ptr;
    if (!ptr && nullable) return NULL;
    return rt_gl_in(c, ptr, image_bytes(c, &unpack_store, w, h, d, format, type), 0);
}
const void *rt_gl_unpack_compressed(CPU *c, uint32_t ptr, int32_t size) {
    if (integer(GL_PIXEL_UNPACK_BUFFER_BINDING)) return (const void *)(uintptr_t)ptr;
    return rt_gl_in(c, ptr, rt_gl_n(size), 1);
}
static GLint level_parameter(uint32_t target, int32_t level, GLenum pname) {
    GLint v = 0; glGetTexLevelParameteriv(target, level, pname, &v); return v;
}
void *rt_gl_pack_tex(CPU *c, uint32_t ptr, uint32_t target, int32_t level, uint32_t format, uint32_t type) {
    if (integer(GL_PIXEL_PACK_BUFFER_BINDING)) return (void *)(uintptr_t)ptr;
    int is3d = target == GL_TEXTURE_3D || target == GL_TEXTURE_2D_ARRAY_EXT || target == GL_TEXTURE_1D_ARRAY_EXT;
    int32_t w = level_parameter(target, level, GL_TEXTURE_WIDTH);
    int32_t h = level_parameter(target, level, GL_TEXTURE_HEIGHT);
    int32_t d = is3d ? level_parameter(target, level, GL_TEXTURE_DEPTH) : 0;
    if (!w) return ptr ? GPTR(ptr) : NULL;  // cible ou niveau invalide : GL refuse sans écrire
    return rt_gl_out(c, ptr, image_bytes(c, &pack_store, w, h ? h : 1, d, format, type), 0);
}
void *rt_gl_pack_compressed(CPU *c, uint32_t ptr, uint32_t target, int32_t level) {
    if (integer(GL_PIXEL_PACK_BUFFER_BINDING)) return (void *)(uintptr_t)ptr;
    return rt_gl_out(c, ptr, rt_gl_n(level_parameter(target, level, GL_TEXTURE_COMPRESSED_IMAGE_SIZE)), 0);
}

// Nombre de valeurs écrites par glGet*v (GL 2.1 et extensions de togl).
uint32_t rt_gl_get_count(uint32_t pname) {
    switch (pname) {
    case GL_MODELVIEW_MATRIX: case GL_PROJECTION_MATRIX: case GL_TEXTURE_MATRIX: case GL_COLOR_MATRIX:
    case GL_TRANSPOSE_MODELVIEW_MATRIX: case GL_TRANSPOSE_PROJECTION_MATRIX:
    case GL_TRANSPOSE_TEXTURE_MATRIX: case GL_TRANSPOSE_COLOR_MATRIX: return 16;
    case GL_VIEWPORT: case GL_SCISSOR_BOX: case GL_COLOR_CLEAR_VALUE: case GL_COLOR_WRITEMASK:
    case GL_BLEND_COLOR: case GL_CURRENT_COLOR: case GL_CURRENT_TEXTURE_COORDS:
    case GL_CURRENT_RASTER_POSITION: case GL_CURRENT_RASTER_COLOR: case GL_CURRENT_RASTER_SECONDARY_COLOR:
    case GL_CURRENT_RASTER_TEXTURE_COORDS: case GL_CURRENT_SECONDARY_COLOR: case GL_FOG_COLOR:
    case GL_LIGHT_MODEL_AMBIENT: case GL_ACCUM_CLEAR_VALUE: case GL_MAP2_GRID_DOMAIN: return 4;
    case GL_CURRENT_NORMAL: return 3;
    case GL_DEPTH_RANGE: case GL_MAX_VIEWPORT_DIMS: case GL_POLYGON_MODE: case GL_LINE_WIDTH_RANGE:
    case GL_POINT_SIZE_RANGE: case GL_ALIASED_LINE_WIDTH_RANGE: case GL_ALIASED_POINT_SIZE_RANGE:
    case GL_MAP1_GRID_DOMAIN: case GL_MAP2_GRID_SEGMENTS: return 2;
    case GL_COMPRESSED_TEXTURE_FORMATS: {
        GLint n = integer(GL_NUM_COMPRESSED_TEXTURE_FORMATS);
        return n > 0 ? (uint32_t)n : 0;
    }
    }
    return 1;
}
uint32_t rt_gl_texparam_count(uint32_t pname) { return pname == GL_TEXTURE_BORDER_COLOR ? 4 : 1; }

// ---- glGetString : copie stable en mémoire invitée ----
typedef struct GlString { uint32_t name, guest; char *text; struct GlString *next; } GlString;
static GlString *strings;

int rt_gl_manual_glGetString(CPU *c, void *fn) {
    uint32_t name = rt_gl_u32(c, 0);
    const char *text = (const char *)((const GLubyte *(*)(GLenum))fn)(name);
    c->eax = 0;
    if (!text) return 1;
    pthread_mutex_lock(&lock);
    GlString *s = strings;
    while (s && (s->name != name || strcmp(s->text, text))) s = s->next;
    if (!s) {
        size_t length = strlen(text);
        s = calloc(1, sizeof(*s));
        if (s) s->text = strdup(text);
        if (s && s->text && length < UINT32_MAX) s->guest = rt_alloc((uint32_t)length + 1);
        if (!s || !s->text || !s->guest) { pthread_mutex_unlock(&lock); rt_trap(c, c->eip, "glGetString : mémoire invitée épuisée"); }
        memcpy(GPTR(s->guest), text, length + 1);
        s->name = name; s->next = strings; strings = s;
    }
    c->eax = s->guest;  // jamais libéré : le pointeur reste valide comme sous Linux
    pthread_mutex_unlock(&lock);
    return 1;
}

// ---- Tampons mappés : miroir invité ----
// Le pointeur hôte de glMapBuffer ne peut pas être donné à l'invité. Chaque
// tampon reçoit un miroir en mémoire invitée (réutilisé d'un mappage à
// l'autre) : contenu recopié au mappage, renvoyé vers l'hôte à chaque
// glFlushMappedBufferRange(APPLE) et, sauf vidage explicite, au démappage.
// Limites : coût d'une copie complète par mappage ; écritures invitées non
// visibles par GL avant flush/démap (togl n'en dépend pas : APPLE_flush_buffer_range).
typedef struct Mirror {
    CGLShareGroupObj share; uint32_t name;
    uint32_t guest, capacity;
    void *host; uint64_t length;
    int mapped, writable, explicit_flush;
    int flush_on_unmap;  // état APPLE_flush_buffer_range du tampon (vrai par défaut)
    struct Mirror *next;
} Mirror;
static Mirror *mirrors;

static GLenum binding_of(uint32_t target) {
    switch (target) {
    case GL_ARRAY_BUFFER: return GL_ARRAY_BUFFER_BINDING;
    case GL_ELEMENT_ARRAY_BUFFER: return GL_ELEMENT_ARRAY_BUFFER_BINDING;
    case GL_PIXEL_PACK_BUFFER: return GL_PIXEL_PACK_BUFFER_BINDING;
    case GL_PIXEL_UNPACK_BUFFER: return GL_PIXEL_UNPACK_BUFFER_BINDING;
    case GL_UNIFORM_BUFFER_EXT: return GL_UNIFORM_BUFFER_BINDING_EXT;
    case GL_TRANSFORM_FEEDBACK_BUFFER_EXT: return GL_TRANSFORM_FEEDBACK_BUFFER_BINDING_EXT;
    case 0x8F36: case 0x8F37: return target;  // COPY_READ/WRITE_BUFFER (GL 3.1)
    }
    return 0;
}
static CGLShareGroupObj current_share(void) { return CGLGetShareGroup(CGLGetCurrentContext()); }
static Mirror *find_mirror(CGLShareGroupObj share, uint32_t name, int create) {
    for (Mirror *m = mirrors; m; m = m->next) if (m->share == share && m->name == name) return m;
    if (!create) return NULL;
    Mirror *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->share = share; m->name = name; m->flush_on_unmap = 1; m->next = mirrors; mirrors = m;
    return m;
}
static uint32_t bound_buffer(uint32_t target) {
    GLenum binding = binding_of(target);
    return binding ? (uint32_t)integer(binding) : 0;
}
static int host_still_mapped(uint32_t target) {
    GLint mapped = 0; glGetBufferParameteriv(target, GL_BUFFER_MAPPED, &mapped); return mapped;
}

// Mappage hôte réussi de [offset, offset+length) : miroir invité.
static uint32_t mirror_map(CPU *c, uint32_t target, void *host, uint64_t length, int copy_in, int writable, int explicit_flush) {
    uint32_t name = bound_buffer(target);
    Mirror *m = name ? find_mirror(current_share(), name, 1) : NULL;
    if (!m || length > UINT32_MAX) { glUnmapBufferARB(target); rt_trap(c, c->eip, "glMapBuffer : tampon hôte non suivi par le pont"); }
    if (m->capacity < length || !m->guest) {
        rt_free(m->guest);
        m->guest = rt_alloc((uint32_t)(length ? length : 1)); m->capacity = m->guest ? (uint32_t)length : 0;
        if (!m->guest) { glUnmapBufferARB(target); rt_trap(c, c->eip, "glMapBuffer : mémoire invitée épuisée"); }
    }
    if (copy_in && length) memcpy(GPTR(m->guest), host, length);
    m->host = host; m->length = length; m->mapped = 1;
    m->writable = writable; m->explicit_flush = explicit_flush;
    return m->guest;
}
static Mirror *mapped_mirror(uint32_t target) {
    uint32_t name = bound_buffer(target);
    Mirror *m = name ? find_mirror(current_share(), name, 0) : NULL;
    if (m && m->mapped && !host_still_mapped(target)) m->mapped = 0;  // démappé implicitement (glBufferData…)
    return m && m->mapped ? m : NULL;
}

int rt_gl_manual_glMapBufferARB(CPU *c, void *fn) {
    uint32_t target = rt_gl_u32(c, 0), access = rt_gl_u32(c, 1);
    void *host = ((void *(*)(GLenum, GLenum))fn)(target, access);
    c->eax = 0;
    if (!host) return 1;
    GLint size = 0; glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
    Mirror *known = find_mirror(current_share(), bound_buffer(target), 0);
    int explicit_flush = known && !known->flush_on_unmap;
    c->eax = mirror_map(c, target, host, (uint64_t)(size > 0 ? size : 0), 1, access != GL_READ_ONLY, explicit_flush);
    return 1;
}

int rt_gl_manual_glMapBufferRange(CPU *c, void *fn) {
    uint32_t target = rt_gl_u32(c, 0), access = rt_gl_u32(c, 3);
    intptr_t offset = (int32_t)rt_gl_u32(c, 1), length = (int32_t)rt_gl_u32(c, 2);
    void *host = ((void *(*)(GLenum, intptr_t, intptr_t, GLbitfield))fn)(target, offset, length, access);
    c->eax = 0;
    if (!host) return 1;
    int invalidate = (access & (GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT)) && !(access & GL_MAP_READ_BIT);
    c->eax = mirror_map(c, target, host, (uint64_t)(length > 0 ? length : 0), !invalidate,
                        (access & GL_MAP_WRITE_BIT) != 0, (access & GL_MAP_FLUSH_EXPLICIT_BIT) != 0);
    return 1;
}

int rt_gl_manual_glUnmapBuffer(CPU *c, void *fn) {
    uint32_t target = rt_gl_u32(c, 0);
    Mirror *m = mapped_mirror(target);
    if (m) {
        if (m->writable && !m->explicit_flush && m->length) memcpy(m->host, GPTR(m->guest), m->length);
        m->mapped = 0; m->host = NULL;
    }
    c->eax = (uint8_t)((GLboolean (*)(GLenum))fn)(target);
    return 1;
}

static int flush_range(CPU *c, void *fn) {
    uint32_t target = rt_gl_u32(c, 0);
    intptr_t offset = (int32_t)rt_gl_u32(c, 1), size = (int32_t)rt_gl_u32(c, 2);
    Mirror *m = mapped_mirror(target);
    if (m && m->writable && offset >= 0 && size >= 0 && (uint64_t)offset + (uint64_t)size <= m->length)
        memcpy((char *)m->host + offset, (char *)GPTR(m->guest) + offset, (size_t)size);
    ((void (*)(GLenum, intptr_t, intptr_t))fn)(target, offset, size);
    return 1;
}
int rt_gl_manual_glBufferParameteriAPPLE(CPU *c, void *fn) {
    uint32_t target = rt_gl_u32(c, 0), pname = rt_gl_u32(c, 1);
    int32_t value = (int32_t)rt_gl_u32(c, 2);
    ((void (*)(GLenum, GLenum, GLint))fn)(target, pname, value);
    uint32_t name = bound_buffer(target);
    // Seul un appel valide modifie l'état : relire la valeur retenue par GL.
    if (name && pname == GL_BUFFER_FLUSHING_UNMAP_APPLE) {
        Mirror *m = find_mirror(current_share(), name, 1);
        GLint current = GL_TRUE; glGetBufferParameteriv(target, GL_BUFFER_FLUSHING_UNMAP_APPLE, &current);
        if (m) m->flush_on_unmap = current != GL_FALSE;
    }
    return 1;
}
int rt_gl_manual_glFlushMappedBufferRangeAPPLE(CPU *c, void *fn) { return flush_range(c, fn); }
int rt_gl_manual_glFlushMappedBufferRange(CPU *c, void *fn) { return flush_range(c, fn); }

int rt_gl_manual_glDeleteBuffersARB(CPU *c, void *fn) {
    int32_t n = (int32_t)rt_gl_u32(c, 0);
    uint32_t list = rt_gl_u32(c, 1);
    const GLuint *names = rt_gl_in(c, list, rt_gl_n(n) * 4, 0);
    CGLShareGroupObj share = current_share();
    for (int32_t i = 0; i < n; ++i) {
        uint32_t name = R32(list + 4 * (uint32_t)i);
        for (Mirror **p = &mirrors; *p; p = &(*p)->next) {
            if ((*p)->share != share || (*p)->name != name || !name) continue;
            Mirror *dead = *p; *p = dead->next;
            rt_free(dead->guest); free(dead);  // GL démappe implicitement un tampon supprimé
            break;
        }
    }
    ((void (*)(GLsizei, const GLuint *))fn)(n, names);
    return 1;
}

// ---- Sources de shaders : tableau de char * invités ----
int rt_gl_manual_glShaderSourceARB(CPU *c, void *fn) {
    void *shader = rt_gl_handle_arg(rt_gl_u32(c, 0));
    int32_t count = (int32_t)rt_gl_u32(c, 1);
    typedef void (*F)(void *, GLsizei, const char *const *, const GLint *);
    if (count <= 0) { ((F)fn)(shader, count, NULL, NULL); return 1; }  // GL valide count
    uint32_t list = rt_gl_u32(c, 2), length_list = rt_gl_u32(c, 3);
    rt_gl_in(c, list, (uint64_t)count * 4, 0);
    const GLint *lengths = rt_gl_in(c, length_list, (uint64_t)count * 4, 1);
    const char **host = calloc((size_t)count, sizeof(*host));
    if (!host) rt_trap(c, c->eip, "glShaderSource : mémoire hôte épuisée");
    for (int32_t i = 0; i < count; ++i) {
        uint32_t text = R32(list + 4 * (uint32_t)i);
        int32_t length = lengths ? (int32_t)R32(length_list + 4 * (uint32_t)i) : -1;
        host[i] = length >= 0 ? rt_gl_in(c, text, (uint64_t)length, 0) : rt_gl_str(c, text);
    }
    ((F)fn)(shader, count, host, lengths);
    free(host);
    return 1;
}

// ---- GLsync : pointeur hôte → handle ----
static void *sync_arg(CPU *c, uint32_t handle) {
    if (!handle) return NULL;  // GL signale lui-même l'objet invalide
    void *sync = rt_handle_get(handle, GL_SYNC_KIND);
    if (!sync) rt_trap(c, c->eip, "GLsync invalide");
    return sync;
}
int rt_gl_manual_glFenceSync(CPU *c, void *fn) {
    void *sync = ((void *(*)(GLenum, GLbitfield))fn)(rt_gl_u32(c, 0), rt_gl_u32(c, 1));
    c->eax = sync ? rt_handle_new(sync, GL_SYNC_KIND) : 0;
    if (sync && !c->eax) { glDeleteSync(sync); rt_trap(c, c->eip, "table de handles pleine (GLsync)"); }
    return 1;
}
int rt_gl_manual_glClientWaitSync(CPU *c, void *fn) {
    void *sync = sync_arg(c, rt_gl_u32(c, 0));
    GLbitfield flags = rt_gl_u32(c, 1);
    uint64_t timeout = rt_gl_u64(c, 2);
    unsigned saved = rt_guest_suspend();  // attente bloquante : laisser tourner les autres threads
    GLenum result = ((GLenum (*)(void *, GLbitfield, uint64_t))fn)(sync, flags, timeout);
    rt_guest_resume(saved);
    c->eax = result;
    return 1;
}
int rt_gl_manual_glWaitSync(CPU *c, void *fn) {
    ((void (*)(void *, GLbitfield, uint64_t))fn)(sync_arg(c, rt_gl_u32(c, 0)), rt_gl_u32(c, 1), rt_gl_u64(c, 2));
    return 1;
}
int rt_gl_manual_glDeleteSync(CPU *c, void *fn) {
    uint32_t handle = rt_gl_u32(c, 0);
    void *sync = sync_arg(c, handle);
    if (sync) rt_handle_free(handle, GL_SYNC_KIND);
    ((void (*)(void *))fn)(sync);
    return 1;
}
int rt_gl_manual_glGetSynciv(CPU *c, void *fn) {
    void *sync = sync_arg(c, rt_gl_u32(c, 0));
    uint32_t pname = rt_gl_u32(c, 1);
    int32_t size = (int32_t)rt_gl_u32(c, 2);
    GLsizei *length = rt_gl_out(c, rt_gl_u32(c, 3), 4, 1);
    GLint *values = rt_gl_out(c, rt_gl_u32(c, 4), rt_gl_n(size) * 4, 0);
    ((void (*)(void *, GLenum, GLsizei, GLsizei *, GLint *))fn)(sync, pname, size, length, values);
    return 1;
}

// APPLE_texture_range : le pointeur rendu est celui fourni par l'invité.
int rt_gl_manual_glGetTexParameterPointervAPPLE(CPU *c, void *fn) {
    uint32_t target = rt_gl_u32(c, 0), pname = rt_gl_u32(c, 1), out = rt_gl_u32(c, 2);
    rt_gl_out(c, out, 4, 0);
    void *value = NULL;
    ((void (*)(GLenum, GLenum, void **))fn)(target, pname, &value);
    uintptr_t p = (uintptr_t)value, base = (uintptr_t)M;
    if (p && (p < base || p - base > UINT32_MAX)) rt_trap(c, c->eip, "pointeur GL hôte hors de la mémoire invitée");
    W32(out, p ? (uint32_t)(p - base) : 0);
    return 1;
}
