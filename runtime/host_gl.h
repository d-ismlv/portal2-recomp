// OpenGL de l'hôte pour les ponts GL (gl.c, sdl_video.c) : OpenGL.framework
// legacy sur macOS, gl4es (OpenGL 2.1 de bureau sur OpenGL ES) sur iOS.
#pragma once
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include "ios/gl_ios.h"
#else
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <dlfcn.h>
static inline void *rt_host_gl_open(void) {
    return dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY | RTLD_LOCAL);
}
static inline void *rt_host_gl_symbol(void *library, const char *name) { return dlsym(library, name); }
#endif
