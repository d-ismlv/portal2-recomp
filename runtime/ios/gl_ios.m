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

static pthread_once_t once = PTHREAD_ONCE_INIT;
static void start_gl4es(void) {
    gles = dlopen("/System/Library/Frameworks/OpenGLES.framework/OpenGLES", RTLD_NOW | RTLD_LOCAL);
    real_bind_framebuffer = dlsym(gles, "glBindFramebuffer");
    real_get_integerv = dlsym(gles, "glGetIntegerv");
    set_getprocaddress(gles_function);
    set_getmainfbsize(main_framebuffer_size);
    initialize_gl4es();
}

void *rt_host_gl_open(void) { return (void *)1; }   // gl4es est lié statiquement
void *rt_host_gl_symbol(void *library, const char *name) { (void)library; return gl4es_GetProcAddress(name); }

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
