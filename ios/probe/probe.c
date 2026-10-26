// Sonde du port iOS : valide la chaîne SDL2 + OpenGL ES + gl4es (OpenGL 2.1
// de bureau, pipeline fixe compris), la réservation de 4 Go de l'espace
// invité et la lecture des données du jeu. Résultats sur la sortie standard.
#include <SDL.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "gl4esinit.h"

typedef void (*clear_color_fn)(float, float, float, float);
typedef void (*clear_fn)(unsigned);
typedef void (*begin_fn)(unsigned);
typedef void (*vertex2f_fn)(float, float);
typedef void (*color3f_fn)(float, float, float);
typedef void (*end_fn)(void);
typedef void (*read_pixels_fn)(int, int, int, int, unsigned, unsigned, void *);
typedef const unsigned char *(*get_string_fn)(unsigned);
typedef void (*viewport_fn)(int, int, int, int);

static void *gles;
static void *gles_proc(const char *name) { return dlsym(gles, name); }
static int fb_w, fb_h;
static void fb_size(int *w, int *h) { *w = fb_w; *h = fb_h; }

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    // 1. Espace invité : 4 Go + garde, sans droit (comme rt_init).
    void *space = mmap(NULL, (1ull << 32) + 65536, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    printf("probe: guest space %s\n", space == MAP_FAILED ? "FAILED" : "ok");
    // 2. Données du jeu, lues directement depuis le disque de l'hôte.
    const char *data = getenv("P2_PROBE_FILE");
    struct stat st;
    if (data) printf("probe: game file %s (%lld bytes)\n", stat(data, &st) ? "UNREADABLE" : "ok",
                     data && !stat(data, &st) ? (long long)st.st_size : -1ll);
    if (data) {
        FILE *f = fopen(data, "rb");
        unsigned char head[4] = {0};
        size_t n = f ? fread(head, 1, 4, f) : 0;
        printf("probe: fopen %s errno=%d, read %zu bytes %02x%02x%02x%02x\n", f ? "ok" : "FAILED", f ? 0 : errno, n,
               head[0], head[1], head[2], head[3]);
        if (f) fclose(f);
    }
    // 3. Fenêtre et contexte OpenGL ES 2 par SDL, puis gl4es par-dessus.
    if (SDL_Init(SDL_INIT_VIDEO)) { printf("probe: SDL_Init failed: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, getenv("P2_PROBE_ES2") ? 2 : 3);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_Window *window = SDL_CreateWindow("probe", 0, 0, 640, 480, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
    SDL_GLContext context = window ? SDL_GL_CreateContext(window) : NULL;
    if (!context) { printf("probe: GL context failed: %s\n", SDL_GetError()); return 1; }
    SDL_GL_GetDrawableSize(window, &fb_w, &fb_h);
    if (getenv("P2_PROBE_CLIP")) { void clip_test(int, int); clip_test(fb_w, fb_h); return 0; }
    gles = dlopen("/System/Library/Frameworks/OpenGLES.framework/OpenGLES", RTLD_NOW | RTLD_LOCAL);
    set_getprocaddress(gles_proc);
    set_getmainfbsize(fb_size);
    {   // extensions réelles d'OpenGL ES (avant gl4es)
        const unsigned char *(*es_string)(unsigned) = dlsym(gles, "glGetString");
        const char *e = (const char *)es_string(0x1F03);
        printf("probe: ES %s\nprobe: ES extensions %s\n", es_string(0x1F02), e ? e : "(null)");
    }
    initialize_gl4es();
    get_string_fn get_string = gl4es_GetProcAddress("glGetString");
    printf("probe: GL_VERSION %s | GL_RENDERER %s\n", get_string(0x1F02), get_string(0x1F01));
    const char *ext = (const char *)get_string(0x1F03);
    printf("probe: S3TC %s, ARB_vertex_program %s, ARB_fragment_program %s, FBO %s\n",
           strstr(ext, "GL_EXT_texture_compression_s3tc") ? "yes" : "no",
           strstr(ext, "GL_ARB_vertex_program") ? "yes" : "no",
           strstr(ext, "GL_ARB_fragment_program") ? "yes" : "no",
           strstr(ext, "GL_EXT_framebuffer_object") ? "yes" : "no");
    // 4. Pipeline fixe : un triangle rouge en mode immédiat sur fond bleu.
    ((viewport_fn)gl4es_GetProcAddress("glViewport"))(0, 0, fb_w, fb_h);
    ((clear_color_fn)gl4es_GetProcAddress("glClearColor"))(0, 0, 1, 1);
    ((clear_fn)gl4es_GetProcAddress("glClear"))(0x4000);
    begin_fn begin = gl4es_GetProcAddress("glBegin");
    vertex2f_fn vertex = gl4es_GetProcAddress("glVertex2f");
    color3f_fn color = gl4es_GetProcAddress("glColor3f");
    begin(0x0004);
    color(1, 0, 0); vertex(-1, -1); vertex(3, -1); vertex(-1, 3);
    ((end_fn)gl4es_GetProcAddress("glEnd"))();
    unsigned char pixel[4] = {0};
    ((read_pixels_fn)gl4es_GetProcAddress("glReadPixels"))(fb_w / 2, fb_h / 2, 1, 1, 0x1908, 0x1401, pixel);
    printf("probe: center pixel %u %u %u %u (expected 255 0 0 255)\n", pixel[0], pixel[1], pixel[2], pixel[3]);
    SDL_GL_SwapWindow(window);
    // P2_PROBE_HOLD=n : garder l'image n secondes (capture d'écran du simulateur).
    const char *hold = getenv("P2_PROBE_HOLD");
    for (int i = 0; hold && i < atoi(hold) * 30; ++i) {
        SDL_PumpEvents();
        ((clear_fn)gl4es_GetProcAddress("glClear"))(0x4000);
        begin(0x0004); color(1, 0, 0); vertex(-1, -1); vertex(3, -1); vertex(-1, 3);
        ((end_fn)gl4es_GetProcAddress("glEnd"))();
        SDL_GL_SwapWindow(window);
        SDL_Delay(33);
    }
    // 4b. Texture DXT1 (S3TC) entièrement rouge, plaquée en pipeline fixe.
    {
        typedef void (*gen_fn)(int, unsigned *);
        typedef void (*bind_fn)(unsigned, unsigned);
        typedef void (*ctex_fn)(unsigned, int, unsigned, int, int, int, int, const void *);
        typedef void (*param_fn)(unsigned, unsigned, int);
        typedef void (*enable_fn)(unsigned);
        typedef void (*texcoord_fn)(float, float);
        unsigned tex = 0;
        ((gen_fn)gl4es_GetProcAddress("glGenTextures"))(1, &tex);
        ((bind_fn)gl4es_GetProcAddress("glBindTexture"))(0x0DE1, tex);
        const unsigned char block[8] = {0x00, 0xF8, 0x1F, 0x00, 0, 0, 0, 0};   // couleur0 rouge, indices 0
        const char *fmt = getenv("P2_PROBE_DXT_FORMAT");   // ex. 0x8C4C (sRGB DXT1)
        unsigned format = fmt ? (unsigned)strtoul(fmt, NULL, 0) : 0x83F0;
        ((ctex_fn)gl4es_GetProcAddress("glCompressedTexImage2D"))(0x0DE1, 0, format, 4, 4, 0, 8, block);
        typedef unsigned (*geterr_fn)(void);
        printf("probe: format %#x, glGetError %#x\n", format, ((geterr_fn)gl4es_GetProcAddress("glGetError"))());
        ((param_fn)gl4es_GetProcAddress("glTexParameteri"))(0x0DE1, 0x2801, 0x2600);   // MIN_FILTER NEAREST
        ((param_fn)gl4es_GetProcAddress("glTexParameteri"))(0x0DE1, 0x2800, 0x2600);
        ((enable_fn)gl4es_GetProcAddress("glEnable"))(0x0DE1);
        ((clear_fn)gl4es_GetProcAddress("glClear"))(0x4000);
        texcoord_fn tc = gl4es_GetProcAddress("glTexCoord2f");
        begin(0x0004); color(1, 1, 1);
        tc(0, 0); vertex(-1, -1); tc(2, 0); vertex(3, -1); tc(0, 2); vertex(-1, 3);
        ((end_fn)gl4es_GetProcAddress("glEnd"))();
        ((read_pixels_fn)gl4es_GetProcAddress("glReadPixels"))(fb_w / 2, fb_h / 2, 1, 1, 0x1908, 0x1401, pixel);
        printf("probe: DXT1 pixel %u %u %u %u (expected 255 0 0 255)\n", pixel[0], pixel[1], pixel[2], pixel[3]);
        ((enable_fn)gl4es_GetProcAddress("glDisable"))(0x0DE1);
    }
    // 4c. Mipmaps DXT1 64x64 envoyés niveau par niveau (comme togl) : niveau 0
    // rouge, les autres verts ; à l'échelle 1:1 le filtrage doit lire du rouge.
    {
        typedef void (*gen_fn)(int, unsigned *);
        typedef void (*bind_fn)(unsigned, unsigned);
        typedef void (*ctex_fn)(unsigned, int, unsigned, int, int, int, int, const void *);
        typedef void (*param_fn)(unsigned, unsigned, int);
        typedef void (*enable_fn)(unsigned);
        typedef void (*texcoord_fn)(float, float);
        unsigned tex = 0;
        ((gen_fn)gl4es_GetProcAddress("glGenTextures"))(1, &tex);
        ((bind_fn)gl4es_GetProcAddress("glBindTexture"))(0x0DE1, tex);
        static unsigned char blocks[64 * 64 / 2];
        const char *fmt = getenv("P2_PROBE_DXT_FORMAT");
        unsigned format = fmt ? (unsigned)strtoul(fmt, NULL, 0) : 0x83F0;
        for (int level = 0, size = 64; size >= 1; ++level, size /= 2) {
            int bw = (size + 3) / 4, count = bw * bw;
            for (int b = 0; b < count; ++b) {
                unsigned char *k = blocks + b * 8;
                k[0] = level ? 0xE0 : 0x00; k[1] = level ? 0x07 : 0xF8;   // vert 0x07E0 / rouge 0xF800
                k[2] = 0x00; k[3] = 0x00; k[4] = k[5] = k[6] = k[7] = 0;
            }
            ((ctex_fn)gl4es_GetProcAddress("glCompressedTexImage2D"))(0x0DE1, level, format, size, size, 0, count * 8, blocks);
        }
        ((param_fn)gl4es_GetProcAddress("glTexParameteri"))(0x0DE1, 0x2801, 0x2703);   // LINEAR_MIPMAP_LINEAR
        ((param_fn)gl4es_GetProcAddress("glTexParameteri"))(0x0DE1, 0x2800, 0x2601);
        ((enable_fn)gl4es_GetProcAddress("glEnable"))(0x0DE1);
        ((clear_fn)gl4es_GetProcAddress("glClear"))(0x4000);
        texcoord_fn tc = gl4es_GetProcAddress("glTexCoord2f");
        float u = 2.0f * fb_w / 64.0f, v = 2.0f * fb_h / 64.0f;   // un texel par pixel
        begin(0x0004); color(1, 1, 1);
        tc(0, 0); vertex(-1, -1); tc(u, 0); vertex(3, -1); tc(0, v); vertex(-1, 3);
        ((end_fn)gl4es_GetProcAddress("glEnd"))();
        ((read_pixels_fn)gl4es_GetProcAddress("glReadPixels"))(fb_w / 2, fb_h / 2, 1, 1, 0x1908, 0x1401, pixel);
        printf("probe: mipmaps DXT format %#x pixel %u %u %u (expected 255 0 0)\n", format, pixel[0], pixel[1], pixel[2]);
        ((enable_fn)gl4es_GetProcAddress("glDisable"))(0x0DE1);
    }
    // 4d. Shader GLSL 1.20 (comme togl) : texture DXT1 rouge sur l'unité 3.
    {
        typedef unsigned (*create_shader_fn)(unsigned);
        typedef void (*source_fn)(unsigned, int, const char **, const int *);
        typedef void (*uint_fn)(unsigned);
        typedef unsigned (*create_program_fn)(void);
        typedef void (*attach_fn)(unsigned, unsigned);
        typedef int (*location_fn)(unsigned, const char *);
        typedef void (*uniform1i_fn)(int, int);
        typedef void (*geti_fn)(unsigned, unsigned, int *);
        typedef void (*gen_fn)(int, unsigned *);
        typedef void (*bind_fn)(unsigned, unsigned);
        typedef void (*ctex_fn)(unsigned, int, unsigned, int, int, int, int, const void *);
        typedef void (*param_fn)(unsigned, unsigned, int);
        typedef void (*texcoord_fn)(float, float);
        const char *vs = "#version 120\nvarying vec2 uv;\nvoid main() { gl_Position = gl_Vertex; uv = gl_MultiTexCoord0.xy; }\n";
        const char *fs = "#version 120\nuniform sampler2D tex;\nvarying vec2 uv;\nvoid main() { gl_FragColor = texture2D(tex, uv); }\n";
        create_shader_fn create = gl4es_GetProcAddress("glCreateShader");
        source_fn source = gl4es_GetProcAddress("glShaderSource");
        uint_fn compile = gl4es_GetProcAddress("glCompileShader");
        geti_fn shaderiv = gl4es_GetProcAddress("glGetShaderiv");
        unsigned v = create(0x8B31), f = create(0x8B30);
        source(v, 1, &vs, NULL); compile(v); source(f, 1, &fs, NULL); compile(f);
        int okv = 0, okf = 0; shaderiv(v, 0x8B81, &okv); shaderiv(f, 0x8B81, &okf);
        unsigned program = ((create_program_fn)gl4es_GetProcAddress("glCreateProgram"))();
        ((attach_fn)gl4es_GetProcAddress("glAttachShader"))(program, v);
        ((attach_fn)gl4es_GetProcAddress("glAttachShader"))(program, f);
        ((uint_fn)gl4es_GetProcAddress("glLinkProgram"))(program);
        int linked = 0; ((geti_fn)gl4es_GetProcAddress("glGetProgramiv"))(program, 0x8B82, &linked);
        ((uint_fn)gl4es_GetProcAddress("glUseProgram"))(program);
        ((uniform1i_fn)gl4es_GetProcAddress("glUniform1i"))(((location_fn)gl4es_GetProcAddress("glGetUniformLocation"))(program, "tex"), 3);
        unsigned tex = 0;
        ((uint_fn)gl4es_GetProcAddress("glActiveTexture"))(0x84C0 + 3);
        ((gen_fn)gl4es_GetProcAddress("glGenTextures"))(1, &tex);
        ((bind_fn)gl4es_GetProcAddress("glBindTexture"))(0x0DE1, tex);
        const unsigned char block[8] = {0x00, 0xF8, 0x1F, 0x00, 0, 0, 0, 0};
        ((ctex_fn)gl4es_GetProcAddress("glCompressedTexImage2D"))(0x0DE1, 0, 0x8C4C, 4, 4, 0, 8, block);
        ((param_fn)gl4es_GetProcAddress("glTexParameteri"))(0x0DE1, 0x2801, 0x2600);
        ((param_fn)gl4es_GetProcAddress("glTexParameteri"))(0x0DE1, 0x2800, 0x2600);
        ((uint_fn)gl4es_GetProcAddress("glActiveTexture"))(0x84C0);
        ((clear_fn)gl4es_GetProcAddress("glClear"))(0x4000);
        texcoord_fn tc = gl4es_GetProcAddress("glTexCoord2f");
        begin(0x0004); tc(0, 0); vertex(-1, -1); tc(2, 0); vertex(3, -1); tc(0, 2); vertex(-1, 3);
        ((end_fn)gl4es_GetProcAddress("glEnd"))();
        ((read_pixels_fn)gl4es_GetProcAddress("glReadPixels"))(fb_w / 2, fb_h / 2, 1, 1, 0x1908, 0x1401, pixel);
        printf("probe: GLSL compile %d/%d link %d, unit 3 pixel %u %u %u (expected 255 0 0)\n", okv, okf, linked,
               pixel[0], pixel[1], pixel[2]);
        ((uint_fn)gl4es_GetProcAddress("glUseProgram"))(0);
    }
    // 4e. Texture SRGB8 + BGR 128x128 (forme des lightmaps de togl) : pixel
    // (x, y) = B=x*2, G=y*2, R=64 ; relu au centre de texels connus.
    {
        typedef void (*gen_fn)(int, unsigned *);
        typedef void (*bind_fn)(unsigned, unsigned);
        typedef void (*tex_fn)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
        typedef void (*param_fn)(unsigned, unsigned, int);
        typedef void (*enable_fn)(unsigned);
        typedef void (*texcoord_fn)(float, float);
        typedef void (*uint_fn)(unsigned);
        static unsigned char img[128 * 128 * 3];
        for (int y = 0; y < 128; ++y) for (int x = 0; x < 128; ++x) {
            unsigned char *p = img + 3 * (y * 128 + x); p[0] = (unsigned char)(x * 2); p[1] = (unsigned char)(y * 2); p[2] = 64;
        }
        unsigned tex = 0;
        ((uint_fn)gl4es_GetProcAddress("glActiveTexture"))(0x84C0);
        ((gen_fn)gl4es_GetProcAddress("glGenTextures"))(1, &tex);
        ((bind_fn)gl4es_GetProcAddress("glBindTexture"))(0x0DE1, tex);
        const char *ifmt = getenv("P2_PROBE_LM_INTERNAL");
        ((uint_fn)gl4es_GetProcAddress("glPixelStorei") ? (void)0 : (void)0);
        typedef void (*store_fn)(unsigned, int);
        ((store_fn)gl4es_GetProcAddress("glPixelStorei"))(0x0CF5, 1);   // UNPACK_ALIGNMENT 1
        ((tex_fn)gl4es_GetProcAddress("glTexImage2D"))(0x0DE1, 0, ifmt ? (int)strtol(ifmt, NULL, 0) : 0x8C41, 128, 128, 0, 0x80E0, 0x1401, img);
        if (getenv("P2_PROBE_LM_RGBA16")) {   // page de lightmaps HDR : RGBA16, UNSIGNED_SHORT
            static unsigned short wide[128 * 128 * 4];
            for (int y = 0; y < 128; ++y) for (int x = 0; x < 128; ++x) {
                unsigned short *p = wide + 4 * (y * 128 + x);
                p[0] = 64 * 257; p[1] = (unsigned short)(y * 2 * 257); p[2] = (unsigned short)(x * 2 * 257); p[3] = 65535;
            }
            ((tex_fn)gl4es_GetProcAddress("glTexImage2D"))(0x0DE1, 0, 0x805B, 128, 128, 0, 0x1908, 0x1403, wide);
            if (getenv("P2_PROBE_LM_RGBA16_SUB")) {
                typedef void (*sub_fn)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
                ((sub_fn)gl4es_GetProcAddress("glTexSubImage2D"))(0x0DE1, 0, 0, 0, 128, 128, 0x1908, 0x1403, wide);
            }
        }
        if (getenv("P2_PROBE_LM_SUB")) {   // mise à jour en BGRA 8_8_8_8_REV, comme togl
            static unsigned char sub[128 * 128 * 4];
            for (int y = 0; y < 128; ++y) for (int x = 0; x < 128; ++x) {
                unsigned char *p = sub + 4 * (y * 128 + x); p[0] = (unsigned char)(x * 2); p[1] = (unsigned char)(y * 2); p[2] = 64; p[3] = 255;
            }
            typedef void (*sub_fn)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
            memset(img, 0, sizeof(img));
            ((tex_fn)gl4es_GetProcAddress("glTexImage2D"))(0x0DE1, 0, 0x8C41, 128, 128, 0, 0x80E0, 0x1401, img);
            ((sub_fn)gl4es_GetProcAddress("glTexSubImage2D"))(0x0DE1, 0, 0, 0, 128, 128, 0x80E1, 0x8367, sub);
        }
        ((param_fn)gl4es_GetProcAddress("glTexParameteri"))(0x0DE1, 0x2801, 0x2600);
        ((param_fn)gl4es_GetProcAddress("glTexParameteri"))(0x0DE1, 0x2800, 0x2600);
        ((enable_fn)gl4es_GetProcAddress("glEnable"))(0x0DE1);
        ((clear_fn)gl4es_GetProcAddress("glClear"))(0x4000);
        texcoord_fn tc = gl4es_GetProcAddress("glTexCoord2f");
        begin(0x0004); color(1, 1, 1); tc(0, 0); vertex(-1, -1); tc(2, 0); vertex(3, -1); tc(0, 2); vertex(-1, 3);
        ((end_fn)gl4es_GetProcAddress("glEnd"))();
        for (int k = 0; k < 3; ++k) {
            int px = fb_w * (1 + 2 * k) / 8, py = fb_h * (1 + 2 * k) / 8;
            ((read_pixels_fn)gl4es_GetProcAddress("glReadPixels"))(px, py, 1, 1, 0x1908, 0x1401, pixel);
            int tx = px * 128 / fb_w, ty = py * 128 / fb_h;
            printf("probe: lightmap texel (%d,%d) pixel %u %u %u (attendu linéaire ~ R=64 G=%d B=%d, avant décodage sRGB)\n",
                   tx, ty, pixel[0], pixel[1], pixel[2], ty * 2, tx * 2);
        }
        ((enable_fn)gl4es_GetProcAddress("glDisable"))(0x0DE1);
    }
    // 5. Fonctions GL demandées par togl (liste du pont, un nom par ligne).
    const char *names = getenv("P2_PROBE_GL_NAMES");
    FILE *list = names ? fopen(names, "r") : NULL;
    if (list) {
        char name[128]; int total = 0, absent = 0;
        while (fgets(name, sizeof(name), list)) {
            name[strcspn(name, "\r\n")] = 0;
            if (!name[0]) continue;
            ++total;
            if (!gl4es_GetProcAddress(name)) { printf("probe: absent %s\n", name); ++absent; }
        }
        fclose(list);
        printf("probe: %d/%d fonctions GL absentes de gl4es\n", absent, total);
    }
    printf("probe: done\n");
    fflush(stdout);
    SDL_Quit();
    return 0;
}
