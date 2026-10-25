// Sonde du port iOS : valide la chaîne SDL2 + OpenGL ES + gl4es (OpenGL 2.1
// de bureau, pipeline fixe compris), la réservation de 4 Go de l'espace
// invité et la lecture des données du jeu. Résultats sur la sortie standard.
#include <SDL.h>
#include <dlfcn.h>
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
    // 3. Fenêtre et contexte OpenGL ES 2 par SDL, puis gl4es par-dessus.
    if (SDL_Init(SDL_INIT_VIDEO)) { printf("probe: SDL_Init failed: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_Window *window = SDL_CreateWindow("probe", 0, 0, 640, 480, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
    SDL_GLContext context = window ? SDL_GL_CreateContext(window) : NULL;
    if (!context) { printf("probe: GL context failed: %s\n", SDL_GetError()); return 1; }
    SDL_GL_GetDrawableSize(window, &fb_w, &fb_h);
    gles = dlopen("/System/Library/Frameworks/OpenGLES.framework/OpenGLES", RTLD_NOW | RTLD_LOCAL);
    set_getprocaddress(gles_proc);
    set_getmainfbsize(fb_size);
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
