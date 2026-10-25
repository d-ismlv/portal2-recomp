// OpenGL hôte sur iOS : contexte ES 2 par SDL, gl4es par-dessus. Voir gl_ios.h.
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#define GLES_SILENCE_DEPRECATION 1
#import <OpenGLES/EAGL.h>
#include <OpenGLES/ES2/gl.h>
#include <SDL.h>
#include <SDL_syswm.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "gl4esinit.h"

void gl4es_pre_swap(void);
void gl4es_post_swap(void);

static void *gles;
static GLuint system_framebuffer;   // FBO de la vue SDL : le « 0 » d'iOS
static int drawable_width, drawable_height;
static void (*real_bind_framebuffer)(GLenum, GLuint);
static void (*real_get_integerv)(GLenum, GLint *);

// Sur iOS il n'y a pas de framebuffer par défaut : SDL dessine dans son FBO.
// togl lie 0 pour le tampon arrière ; gl4es transmet tel quel, d'où cette
// traduction au niveau des fonctions ES qu'il appelle.
static void bind_framebuffer(GLenum target, GLuint framebuffer) {
    real_bind_framebuffer(target, framebuffer ? framebuffer : system_framebuffer);
}
static void get_integerv(GLenum name, GLint *values) {
    real_get_integerv(name, values);
    if (name == GL_FRAMEBUFFER_BINDING && (GLuint)values[0] == system_framebuffer) values[0] = 0;
}
static void *gles_function(const char *name) {
    if (!strcmp(name, "glBindFramebuffer")) return (void *)bind_framebuffer;
    if (!strcmp(name, "glGetIntegerv")) return (void *)get_integerv;
    return dlsym(gles, name);
}
static void main_framebuffer_size(int *width, int *height) { *width = drawable_width; *height = drawable_height; }

static void load_extras(void);
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void start_gl4es(void) {
    gles = dlopen("/System/Library/Frameworks/OpenGLES.framework/OpenGLES", RTLD_NOW | RTLD_LOCAL);
    real_bind_framebuffer = dlsym(gles, "glBindFramebuffer");
    real_get_integerv = dlsym(gles, "glGetIntegerv");
    set_getprocaddress(gles_function);
    set_getmainfbsize(main_framebuffer_size);
    initialize_gl4es();
    load_extras();
}

// ---- ajouts à gl4es pour togl ----
// togl exige des fences (NV, ARB ou APPLE), S3TC et EXT_texture_sRGB_decode.
// gl4es n'a ni fences ni sRGB_decode : ARB_sync est servi par APPLE_sync
// d'OpenGL ES (mêmes constantes), après vidage des commandes que gl4es tient
// encore en tampon ; sRGB_decode est annoncé seulement (le paramètre de
// texture est ignoré : gamma approximatif, rendu correct par ailleurs).
typedef struct __GLsync *SyncObject;
static SyncObject (*es_fence_sync)(GLenum, GLbitfield);
static GLenum (*es_client_wait_sync)(SyncObject, GLbitfield, uint64_t);
static void (*es_wait_sync)(SyncObject, GLbitfield, uint64_t);
static void (*es_delete_sync)(SyncObject);
static void (*es_get_synciv)(SyncObject, GLenum, GLsizei, GLsizei *, GLint *);
static void (*gl4es_flush_fn)(void);
static const GLubyte *(*gl4es_get_string)(GLenum);
static void *es_symbol(const char *name) {
    char apple[96];
    snprintf(apple, sizeof(apple), "%sAPPLE", name);
    void *fn = dlsym(gles, apple);
    return fn ? fn : dlsym(gles, name);
}
static void load_extras(void) {
    es_fence_sync = es_symbol("glFenceSync");
    es_client_wait_sync = es_symbol("glClientWaitSync");
    es_wait_sync = es_symbol("glWaitSync");
    es_delete_sync = es_symbol("glDeleteSync");
    es_get_synciv = es_symbol("glGetSynciv");
    gl4es_flush_fn = gl4es_GetProcAddress("glFlush");
    gl4es_get_string = gl4es_GetProcAddress("glGetString");
    if (!es_fence_sync) fprintf(stderr, "[gl-ios] OpenGL ES sans APPLE_sync : fences indisponibles\n");
}
static SyncObject fence_sync(GLenum condition, GLbitfield flags) {
    gl4es_flush_fn();   // la fence doit suivre les tracés que gl4es diffère
    return es_fence_sync(condition, flags);
}
static GLenum client_wait_sync(SyncObject sync, GLbitfield flags, uint64_t timeout) {
    return es_client_wait_sync(sync, flags, timeout);
}
static void wait_sync(SyncObject sync, GLbitfield flags, uint64_t timeout) { es_wait_sync(sync, flags, timeout); }
static void delete_sync(SyncObject sync) { if (sync) es_delete_sync(sync); }
static void get_synciv(SyncObject sync, GLenum name, GLsizei size, GLsizei *length, GLint *values) {
    es_get_synciv(sync, name, size, length, values);
}
static const GLubyte *get_string(GLenum name) {
    static char *extensions;
    const GLubyte *s = gl4es_get_string(name);
    if (name != GL_EXTENSIONS || !s) return s;
    if (!extensions && asprintf(&extensions, "%s%s%s", s, es_fence_sync ? " GL_ARB_sync" : "",
                                " GL_EXT_texture_sRGB_decode") < 0) return s;
    return (const GLubyte *)extensions;
}
// Références directes de gl.c, que gl4es n'exporte pas sous ces noms.
void gl4es_glDeleteSync(SyncObject sync) { delete_sync(sync); }
GLboolean gl4es_glUnmapBuffer(GLenum target);
GLboolean gl4es_glUnmapBufferARB(GLenum target) { return gl4es_glUnmapBuffer(target); }

void *rt_host_gl_open(void) { return (void *)1; }   // gl4es est lié statiquement
void *rt_host_gl_symbol(void *library, const char *name) {
    (void)library;
    static const struct { const char *name; void *fn; } extras[] = {
        {"glFenceSync", (void *)fence_sync}, {"glClientWaitSync", (void *)client_wait_sync},
        {"glWaitSync", (void *)wait_sync}, {"glDeleteSync", (void *)delete_sync},
        {"glGetSynciv", (void *)get_synciv}, {"glGetString", (void *)get_string},
    };
    for (size_t i = 0; i < sizeof(extras) / sizeof(*extras); ++i) {
        if (strcmp(name, extras[i].name)) continue;
        if (!gl4es_get_string) return NULL;   // avant le premier contexte : rien
        int sync = strstr(name, "Sync") != NULL;
        return sync && !es_fence_sync ? NULL : extras[i].fn;
    }
    return gl4es_GetProcAddress(name);
}

void *rt_ios_gl_current_context(void) { return (__bridge void *)[EAGLContext currentContext]; }
void *rt_ios_gl_share_group(void *context) {
    return context ? (__bridge void *)((__bridge EAGLContext *)context).sharegroup : NULL;
}

SDL_GLContext rt_ios_gl_create_context(SDL_Window *window) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GLContext context = SDL_GL_CreateContext(window);
    if (!context) return NULL;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(window, &info) && info.subsystem == SDL_SYSWM_UIKIT)
        system_framebuffer = info.info.uikit.framebuffer;
    SDL_GL_GetDrawableSize(window, &drawable_width, &drawable_height);
    pthread_once(&once, start_gl4es);
    fprintf(stderr, "[gl-ios] contexte ES 2, FBO système %u, %dx%d\n", system_framebuffer,
            drawable_width, drawable_height);
    return context;
}

void rt_ios_gl_swap_window(SDL_Window *window) {
    gl4es_pre_swap();
    SDL_GL_SwapWindow(window);
    gl4es_post_swap();
}
#endif
