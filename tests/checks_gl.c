// Pont OpenGL : appels invités i386 vers le GL hôte, dans un contexte CGL
// legacy hors écran (celui que SDL crée sous macOS pour togl).
#include "checks.h"
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define STACK 0x20010000u
#define DATA 0x20012000u  // 0x20012000-0x20020000 : données des tests GL

static void current_context(void) {
    static CGLContextObj context;
    if (!context) {
        CGLPixelFormatAttribute attributes[] = {kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_Legacy, 0};
        CGLPixelFormatObj format = NULL; GLint count = 0;
        assert(CGLChoosePixelFormat(attributes, &format, &count) == kCGLNoError && format);
        assert(CGLCreateContext(format, NULL, &context) == kCGLNoError);
        CGLDestroyPixelFormat(format);
    }
    assert(CGLSetCurrentContext(context) == kCGLNoError);
}

static CPU last;
// Appel invité cdecl : n mots de 32 bits empilés (un double ou un GLuint64 en prend deux).
static uint32_t gl(const char *name, unsigned n, ...) {
    uint32_t fn = rt_gl_proc_address(name);
    if (!fn) { fprintf(stderr, "rt_gl_proc_address(%s) = 0\n", name); abort(); }
    va_list list; va_start(list, n);
    for (unsigned i = 0; i < n; ++i) W32(STACK + 4 * i, va_arg(list, uint32_t));
    va_end(list);
    rt_cpu_reset(&last); last.esp = STACK;
    rt_call(&last, fn, RT_RETURN_SENTINEL);
    assert(last.esp == STACK);
    return last.eax;
}
static uint32_t f32(float v) { uint32_t u; memcpy(&u, &v, 4); return u; }
static uint32_t lo(double v) { uint64_t u; memcpy(&u, &v, 8); return (uint32_t)u; }
static uint32_t hi(double v) { uint64_t u; memcpy(&u, &v, 8); return (uint32_t)(u >> 32); }

RT_CHECK(gl_proc_address_matches_macos_sdl) {
    uint32_t clear = rt_gl_proc_address("glClearColor");
    assert(clear >= RT_DYNAMIC_IMPORT_BASE && clear == rt_gl_proc_address("glClearColor"));
    assert(!strcmp(rt_dynamic_import_name(clear), "glClearColor"));
    // Symbole exporté mais inutilisable en legacy : adresse rendue, comme SDL.
    assert(rt_gl_proc_address("glGenSamplers"));
    // Absents d'OpenGL.framework, ou inconnus du pont.
    assert(!rt_gl_proc_address("glDebugMessageCallbackARB") && !rt_gl_proc_address("glGenFencesNV"));
    assert(!rt_gl_proc_address("glXGetCurrentDisplay") && !rt_gl_proc_address("glob") && !rt_gl_proc_address(NULL));
}

RT_CHECK(gl_get_string_is_a_stable_guest_copy) {
    current_context();
    uint32_t version = gl("glGetString", 1, GL_VERSION);
    assert(version && !strcmp(GPTR(version), (const char *)glGetString(GL_VERSION)) && R8(version) >= '2');
    assert(gl("glGetString", 1, GL_VERSION) == version);
    uint32_t extensions = gl("glGetString", 1, GL_EXTENSIONS);
    assert(extensions && strstr(GPTR(extensions), "GL_APPLE_fence") && strstr(GPTR(extensions), "GL_EXT_texture_sRGB_decode"));
    assert(!gl("glGetString", 1, 0x1234u) && gl("glGetError", 0) == GL_INVALID_ENUM);
}

RT_CHECK(gl_scalar_float_and_double_arguments) {
    current_context();
    gl("glClearColor", 4, f32(0.25f), f32(0.5f), f32(0.75f), f32(1.0f));
    gl("glGetFloatv", 2, GL_COLOR_CLEAR_VALUE, DATA);
    assert(RF(DATA) == 0.25f && RF(DATA + 4) == 0.5f && RF(DATA + 8) == 0.75f && RF(DATA + 12) == 1.0f);
    gl("glClearDepth", 2, lo(0.375), hi(0.375));
    gl("glDepthRange", 4, lo(0.125), hi(0.125), lo(0.875), hi(0.875));
    gl("glGetDoublev", 2, GL_DEPTH_CLEAR_VALUE, DATA);
    assert(RD(DATA) == 0.375);
    gl("glGetDoublev", 2, GL_DEPTH_RANGE, DATA);
    assert(RD(DATA) == 0.125 && RD(DATA + 8) == 0.875);
    gl("glViewport", 4, 1, 2, 30, 40);
    gl("glGetIntegerv", 2, GL_VIEWPORT, DATA);
    assert(R32(DATA) == 1 && R32(DATA + 4) == 2 && R32(DATA + 8) == 30 && R32(DATA + 12) == 40);
    gl("glEnable", 1, GL_BLEND);
    assert(gl("glIsEnabled", 1, GL_BLEND) == 1);
    gl("glDisable", 1, GL_BLEND);
    assert(gl("glIsEnabled", 1, GL_BLEND) == 0 && gl("glGetError", 0) == GL_NO_ERROR);
}

RT_CHECK(gl_texture_upload_and_readback_use_pixel_store) {
    current_context();
    gl("glGenTextures", 2, 1, DATA);
    uint32_t texture = R32(DATA);
    assert(texture && gl("glIsTexture", 1, texture) == 0);  // nom réservé, pas encore lié
    gl("glBindTexture", 2, GL_TEXTURE_2D, texture);
    static const uint8_t rgb[18] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18};
    memcpy(GPTR(DATA + 0x100), rgb, sizeof(rgb));
    gl("glPixelStorei", 2, GL_UNPACK_ALIGNMENT, 1);  // 3×2 RGB serrés : 18 octets
    gl("glTexImage2D", 9, GL_TEXTURE_2D, 0, GL_RGBA8, 3, 2, 0, GL_RGB, GL_UNSIGNED_BYTE, DATA + 0x100);
    assert(gl("glIsTexture", 1, texture) == 1);
    gl("glGetTexLevelParameteriv", 4, GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, DATA);
    assert(R32(DATA) == 3);
    memset(GPTR(DATA + 0x200), 0xee, 64);
    gl("glGetTexImage", 5, GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, DATA + 0x200);
    for (unsigned i = 0; i < 6; ++i)
        assert(R8(DATA + 0x200 + 4 * i) == rgb[3 * i] && R8(DATA + 0x200 + 4 * i + 2) == rgb[3 * i + 2] &&
               R8(DATA + 0x200 + 4 * i + 3) == 255);
    assert(R8(DATA + 0x200 + 24) == 0xee);  // rien au-delà de 3×2×4
    // Relecture RGB avec PACK_ALIGNMENT 4 : lignes de 9 octets bourrées à 12.
    memset(GPTR(DATA + 0x200), 0xee, 64);
    gl("glPixelStorei", 2, GL_PACK_ALIGNMENT, 4);
    gl("glGetTexImage", 5, GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, DATA + 0x200);
    assert(!memcmp(GPTR(DATA + 0x200), rgb, 9) && !memcmp(GPTR(DATA + 0x200 + 12), rgb + 9, 9));
    assert(R8(DATA + 0x200 + 21) == 0xee);
    gl("glTexSubImage2D", 9, GL_TEXTURE_2D, 0, 1, 1, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, DATA + 0x100);
    gl("glGetTexImage", 5, GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, DATA + 0x200);
    assert(!memcmp(GPTR(DATA + 0x200 + 12 + 3), rgb, 3));
    // UNPACK_ROW_LENGTH/SKIP_* : le pixel lu est à (1×4 + 2)×3 = 18 octets.
    gl("glPixelStorei", 2, GL_UNPACK_ROW_LENGTH, 4);
    gl("glPixelStorei", 2, GL_UNPACK_SKIP_ROWS, 1);
    gl("glPixelStorei", 2, GL_UNPACK_SKIP_PIXELS, 2);
    W8(DATA + 0x100 + 18, 0x81); W8(DATA + 0x100 + 19, 0x82); W8(DATA + 0x100 + 20, 0x83);
    gl("glTexSubImage2D", 9, GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, DATA + 0x100);
    gl("glPixelStorei", 2, GL_UNPACK_ROW_LENGTH, 0);
    gl("glPixelStorei", 2, GL_UNPACK_SKIP_ROWS, 0);
    gl("glPixelStorei", 2, GL_UNPACK_SKIP_PIXELS, 0);
    gl("glGetTexImage", 5, GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, DATA + 0x200);
    assert(R8(DATA + 0x200) == 0x81 && R8(DATA + 0x201) == 0x82 && R8(DATA + 0x202) == 0x83);
    W32(DATA, texture);
    gl("glDeleteTextures", 2, 1, DATA);
    assert(gl("glIsTexture", 1, texture) == 0 && gl("glGetError", 0) == GL_NO_ERROR);
    gl("glPixelStorei", 2, GL_UNPACK_ALIGNMENT, 4);
}

static void host_contents(uint8_t *out, unsigned size) {
    glGetBufferSubData(GL_ARRAY_BUFFER, 0, size, out);
}

RT_CHECK(gl_map_buffer_uses_a_guest_mirror) {
    current_context();
    gl("glGenBuffersARB", 2, 1, DATA);
    uint32_t buffer = R32(DATA);
    gl("glBindBufferARB", 2, GL_ARRAY_BUFFER, buffer);
    for (unsigned i = 0; i < 32; ++i) W8(DATA + 0x100 + i, i);
    gl("glBufferDataARB", 4, GL_ARRAY_BUFFER, 32, DATA + 0x100, GL_STREAM_DRAW);
    uint8_t host[32];

    // WRITE_ONLY : les octets non écrits gardent leur valeur.
    uint32_t p = gl("glMapBufferARB", 2, GL_ARRAY_BUFFER, GL_WRITE_ONLY);
    assert(p && p < RT_HANDLE_BASE && R8(p + 31) == 31);
    W32(p + 4, 0xa1a2a3a4);
    assert(gl("glUnmapBuffer", 1, GL_ARRAY_BUFFER) == 1);
    host_contents(host, 32);
    assert(host[3] == 3 && host[4] == 0xa4 && host[7] == 0xa1 && host[8] == 8 && host[31] == 31);

    // APPLE_flush_buffer_range explicite : seule la plage vidée est transmise.
    gl("glBufferParameteriAPPLE", 3, GL_ARRAY_BUFFER, GL_BUFFER_FLUSHING_UNMAP_APPLE, GL_FALSE);
    p = gl("glMapBufferARB", 2, GL_ARRAY_BUFFER, GL_WRITE_ONLY);
    assert(p && R32(p + 4) == 0xa1a2a3a4);
    W8(p + 10, 0x55); W8(p + 20, 0x66);
    gl("glFlushMappedBufferRangeAPPLE", 3, GL_ARRAY_BUFFER, 8, 4);
    assert(gl("glUnmapBuffer", 1, GL_ARRAY_BUFFER) == 1);
    host_contents(host, 32);
    assert(host[10] == 0x55 && host[20] == 20);
    gl("glBufferParameteriAPPLE", 3, GL_ARRAY_BUFFER, GL_BUFFER_FLUSHING_UNMAP_APPLE, GL_TRUE);

    // READ_ONLY : contenu visible, rien n'est recopié vers l'hôte.
    p = gl("glMapBufferARB", 2, GL_ARRAY_BUFFER, GL_READ_ONLY);
    assert(p && R8(p + 10) == 0x55 && R8(p + 20) == 20);
    W8(p + 0, 0x77);
    assert(gl("glUnmapBuffer", 1, GL_ARRAY_BUFFER) == 1);
    host_contents(host, 32);
    assert(host[0] == 0);

    // glBufferData démappe implicitement : pas d'écriture via un pointeur hôte périmé.
    p = gl("glMapBufferARB", 2, GL_ARRAY_BUFFER, GL_READ_WRITE);
    assert(p);
    gl("glBufferDataARB", 4, GL_ARRAY_BUFFER, 64, 0, GL_STREAM_DRAW);
    gl("glUnmapBuffer", 1, GL_ARRAY_BUFFER);  // Apple renvoie GL_TRUE ici, mais signale l'erreur
    assert(gl("glGetError", 0) == GL_INVALID_OPERATION);

    // Sans tampon lié : erreur GL, pointeur nul.
    gl("glBindBufferARB", 2, GL_ARRAY_BUFFER, 0);
    assert(gl("glMapBufferARB", 2, GL_ARRAY_BUFFER, GL_WRITE_ONLY) == 0 && gl("glGetError", 0) == GL_INVALID_OPERATION);
    gl("glDeleteBuffersARB", 2, 1, DATA);
    assert(!glIsBuffer(buffer) && gl("glGetError", 0) == GL_NO_ERROR);
}

RT_CHECK(gl_shader_source_takes_guest_string_arrays) {
    current_context();
    uint32_t shader = gl("glCreateShaderObjectARB", 1, GL_FRAGMENT_SHADER_ARB);
    assert(shader && shader < 0x10000);
    const char *first = "void main() { gl_FragColor = vec4(1.0); }XXXX";  // longueur explicite, sans NUL
    const char *second = "\n// fin\n";
    memcpy(GPTR(DATA + 0x100), first, strlen(first));
    memcpy(GPTR(DATA + 0x200), second, strlen(second) + 1);
    uint32_t length = (uint32_t)(strlen(first) - 4);
    W32(DATA, DATA + 0x100); W32(DATA + 4, DATA + 0x200);
    W32(DATA + 8, length); W32(DATA + 12, 0xffffffffu);
    gl("glShaderSourceARB", 4, shader, 2, DATA, DATA + 8);
    gl("glGetObjectParameterivARB", 3, shader, GL_OBJECT_SHADER_SOURCE_LENGTH_ARB, DATA + 16);
    assert(R32(DATA + 16) == length + strlen(second) + 1);
    gl("glCompileShaderARB", 1, shader);
    gl("glGetObjectParameterivARB", 3, shader, GL_OBJECT_COMPILE_STATUS_ARB, DATA + 16);
    assert(R32(DATA + 16) == 1);
    uint32_t program = gl("glCreateProgramObjectARB", 0);
    gl("glAttachObjectARB", 2, program, shader);
    gl("glLinkProgramARB", 1, program);
    gl("glGetObjectParameterivARB", 3, program, GL_OBJECT_LINK_STATUS_ARB, DATA + 16);
    assert(R32(DATA + 16) == 1);
    W32(DATA + 20, 0xdeadbeef);
    gl("glGetInfoLogARB", 4, program, 64, DATA + 20, DATA + 0x300);
    assert(R32(DATA + 20) < 64);
    gl("glDeleteObjectARB", 1, shader); gl("glDeleteObjectARB", 1, program);
    assert(gl("glGetError", 0) == GL_NO_ERROR);
}

RT_CHECK(gl_vertex_pointer_is_client_memory_or_vbo_offset) {
    current_context();
    gl("glBindBufferARB", 2, GL_ARRAY_BUFFER, 0);
    gl("glVertexAttribPointer", 6, 1, 3, GL_FLOAT, 0, 12, DATA + 0x400);
    void *pointer = NULL;
    glGetVertexAttribPointerv(1, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pointer);
    assert(pointer == GPTR(DATA + 0x400));
    GLuint buffer; glGenBuffers(1, &buffer); glBindBuffer(GL_ARRAY_BUFFER, buffer);
    gl("glVertexAttribPointer", 6, 1, 3, GL_FLOAT, 0, 12, 24);
    glGetVertexAttribPointerv(1, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pointer);
    assert(pointer == (void *)24);
    glBindBuffer(GL_ARRAY_BUFFER, 0); glDeleteBuffers(1, &buffer);
    assert(gl("glGetError", 0) == GL_NO_ERROR);
}

RT_CHECK(gl_sync_objects_are_guest_handles) {
    current_context();
    uint32_t sync = gl("glFenceSync", 2, GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    assert(sync >= RT_HANDLE_BASE);
    uint64_t timeout = 1000000000ull;
    uint32_t status = gl("glClientWaitSync", 4, sync, GL_SYNC_FLUSH_COMMANDS_BIT, (uint32_t)timeout, (uint32_t)(timeout >> 32));
    assert(status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED);
    W32(DATA, 0xdeadbeef);
    gl("glGetSynciv", 5, sync, GL_SYNC_STATUS, 1, DATA, DATA + 4);
    assert(R32(DATA) == 1 && R32(DATA + 4) == GL_SIGNALED);
    gl("glDeleteSync", 1, sync);
    assert(!rt_handle_get(sync, 0x474c5359u) && gl("glGetError", 0) == GL_NO_ERROR);
    gl("glDeleteSync", 1, 0);  // objet nul accepté par GL
}

RT_CHECK(gl_legacy_context_rejects_core_only_entry_points) {
    current_context();
    // Comportement réel d'un Mac : togl reçoit l'adresse, GL refuse l'appel.
    W32(DATA, 0x12345678);
    gl("glGenSamplers", 2, 1, DATA);
    assert(gl("glGetError", 0) == GL_INVALID_OPERATION && R32(DATA) == 0x12345678);
}

RT_CHECK(gl_texture_range_pointer_round_trips) {
    current_context();
    GLuint texture; glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, texture);
    gl("glTextureRangeAPPLE", 3, GL_TEXTURE_RECTANGLE_ARB, 256, DATA + 0x800);
    W32(DATA, 0);
    gl("glGetTexParameterPointervAPPLE", 3, GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_RANGE_POINTER_APPLE, DATA);
    assert(R32(DATA) == DATA + 0x800);
    glDeleteTextures(1, &texture);
    assert(gl("glGetError", 0) == GL_NO_ERROR);
}
