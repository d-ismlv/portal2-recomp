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
static GLuint system_colorbuffer;   // son renderbuffer couleur, présenté par SDL
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
// gl4es détecte les capacités par la liste d'extensions, à la manière d'ES 2.
// Dans un contexte ES 3, celles devenues standard n'y figurent plus : il les
// croit absentes (profondeur 16 bits, stencil séparé que iOS refuse, RGBA4,
// textures redimensionnées en puissances de deux). On les lui rend.
static const GLubyte *(*real_get_string)(GLenum);
static const GLubyte *es_get_string(GLenum name) {
    static char *extensions;
    const GLubyte *s = real_get_string(name);
    if (name != GL_EXTENSIONS || !s) return s;
    const char *version = (const char *)real_get_string(GL_VERSION);
    if (!version || !strstr(version, "OpenGL ES 3")) return s;
    if (!extensions && asprintf(&extensions, "%s GL_OES_packed_depth_stencil GL_OES_depth24 GL_OES_depth_texture "
            "GL_OES_rgb8_rgba8 GL_OES_element_index_uint GL_OES_texture_npot GL_EXT_blend_minmax GL_EXT_texture_rg "
            "GL_OES_texture_half_float GL_OES_texture_float GL_OES_fbo_render_mipmap", s) < 0) return s;
    return (const GLubyte *)extensions;
}
static void *gles_function(const char *name) {
    if (!strcmp(name, "glGetString")) return (void *)es_get_string;
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
    real_get_string = dlsym(gles, "glGetString");
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
static GLenum (*gl4es_check_status)(GLenum);
static void (*gl4es_tex_parameteri)(GLenum, GLenum, GLint);
static void (*gl4es_tex_parameteriv)(GLenum, GLenum, const GLint *);
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
    gl4es_check_status = gl4es_GetProcAddress("glCheckFramebufferStatus");
    gl4es_tex_parameteri = gl4es_GetProcAddress("glTexParameteri");
    gl4es_tex_parameteriv = gl4es_GetProcAddress("glTexParameteriv");
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
// Diagnostic : un FBO refusé par OpenGL ES est décrit attachement par attachement.
static GLenum check_framebuffer_status(GLenum target) {
    GLenum status = gl4es_check_status(target);
    if (status == GL_FRAMEBUFFER_COMPLETE) return status;
    static int reported;
    if (reported++ < 8) {
        void (*attachment)(GLenum, GLenum, GLenum, GLint *) = dlsym(gles, "glGetFramebufferAttachmentParameteriv");
        void (*rb)(GLenum, GLenum, GLint *) = dlsym(gles, "glGetRenderbufferParameteriv");
        void (*bind_rb)(GLenum, GLuint) = dlsym(gles, "glBindRenderbuffer");
        GLint fbo = 0, previous_rb = 0;
        real_get_integerv(GL_FRAMEBUFFER_BINDING, &fbo);
        real_get_integerv(GL_RENDERBUFFER_BINDING, &previous_rb);
        fprintf(stderr, "[gl-ios] FBO %d incomplet : %#x\n", fbo, status);
        const struct { GLenum point; const char *name; } points[] = {
            {GL_COLOR_ATTACHMENT0, "couleur0"}, {GL_DEPTH_ATTACHMENT, "profondeur"}, {GL_STENCIL_ATTACHMENT, "stencil"}};
        for (size_t i = 0; i < 3; ++i) {
            GLint type = 0, name = 0, format = 0, width = 0, height = 0;
            attachment(GL_FRAMEBUFFER, points[i].point, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
            attachment(GL_FRAMEBUFFER, points[i].point, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
            if (type == GL_RENDERBUFFER) {
                bind_rb(GL_RENDERBUFFER, (GLuint)name);
                rb(GL_RENDERBUFFER, GL_RENDERBUFFER_INTERNAL_FORMAT, &format);
                rb(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &width);
                rb(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &height);
            }
            fprintf(stderr, "[gl-ios]   %s : %s %d format %#x %dx%d\n", points[i].name,
                    type == GL_TEXTURE ? "texture" : type == GL_RENDERBUFFER ? "renderbuffer" : type ? "?" : "aucun",
                    name, format, width, height);
        }
        bind_rb(GL_RENDERBUFFER, (GLuint)previous_rb);
    }
    return status;
}

// EXT_texture_sRGB_decode est annoncé à togl : son paramètre de texture est
// absorbé ici au lieu d'atteindre gl4es/OpenGL ES (GL_INVALID_ENUM).
#define TEXTURE_SRGB_DECODE 0x8A48
static void tex_parameteri(GLenum target, GLenum name, GLint value) {
    if (name != TEXTURE_SRGB_DECODE) gl4es_tex_parameteri(target, name, value);
}
static void tex_parameteriv(GLenum target, GLenum name, const GLint *values) {
    if (name != TEXTURE_SRGB_DECODE) gl4es_tex_parameteriv(target, name, values);
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
        {"glCheckFramebufferStatus", (void *)check_framebuffer_status},
        {"glTexParameteri", (void *)tex_parameteri}, {"glTexParameteriv", (void *)tex_parameteriv},
        {"glCheckFramebufferStatusEXT", (void *)check_framebuffer_status},
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
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);   // ES 3 : 16 unités de texture (togl en utilise 16)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GLContext context = SDL_GL_CreateContext(window);
    if (!context) return NULL;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(window, &info) && info.subsystem == SDL_SYSWM_UIKIT)
        system_framebuffer = info.info.uikit.framebuffer, system_colorbuffer = info.info.uikit.colorbuffer;
    SDL_GL_GetDrawableSize(window, &drawable_width, &drawable_height);
    pthread_once(&once, start_gl4es);
    fprintf(stderr, "[gl-ios] contexte ES 3, FBO système %u, %dx%d\n", system_framebuffer,
            drawable_width, drawable_height);
    return context;
}

// P2_IOS_PROBE_FRAMES=1 : toutes les 120 images, quelques pixels du FBO
// système juste avant la présentation (noir partout = rien n'y est dessiné).
static void probe_frame(void) {
    static int enabled = -1, frame;
    if (enabled < 0) { const char *e = getenv("P2_IOS_PROBE_FRAMES"); enabled = e && *e == '1'; }
    if (!enabled || ++frame % 120) return;
    void (*read)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *) = dlsym(gles, "glReadPixels");
    GLint bound = 0; real_get_integerv(GL_FRAMEBUFFER_BINDING, &bound);
    real_bind_framebuffer(GL_FRAMEBUFFER, system_framebuffer);
    unsigned char p[5][4] = {{0}};
    int xs[5] = {drawable_width / 2, drawable_width / 4, 3 * drawable_width / 4, drawable_width / 2, drawable_width / 2};
    int ys[5] = {drawable_height / 2, drawable_height / 2, drawable_height / 2, drawable_height / 4, 3 * drawable_height / 4};
    for (int i = 0; i < 5; ++i) read(xs[i], ys[i], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p[i]);
    real_bind_framebuffer(GL_FRAMEBUFFER, (GLuint)bound);
    fprintf(stderr, "[gl-ios] image %d, FBO lié %d : pixels %02x%02x%02x %02x%02x%02x %02x%02x%02x %02x%02x%02x %02x%02x%02x\n",
            frame, bound, p[0][0], p[0][1], p[0][2], p[1][0], p[1][1], p[1][2], p[2][0], p[2][1], p[2][2],
            p[3][0], p[3][1], p[3][2], p[4][0], p[4][1], p[4][2]);
}

void rt_ios_gl_swap_window(SDL_Window *window) {
    gl4es_pre_swap();
    probe_frame();
    // SDL présente le renderbuffer lié à GL_RENDERBUFFER en supposant que c'est
    // le sien ; le jeu y laisse souvent un renderbuffer de profondeur (image noire).
    static void (*bind_rb)(GLenum, GLuint);
    if (!bind_rb) bind_rb = dlsym(gles, "glBindRenderbuffer");
    GLint renderbuffer = 0;
    real_get_integerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
    bind_rb(GL_RENDERBUFFER, system_colorbuffer);
    SDL_GL_SwapWindow(window);
    bind_rb(GL_RENDERBUFFER, (GLuint)renderbuffer);
    gl4es_post_swap();
}
#endif
