// SDL sur iOS : tailles en pixels pour le jeu (voir sdl_ios.h).
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Échelle points -> pixels : celle de l'écran sur appareil. Sur simulateur
// (rendu OpenGL ES logiciel, 0,5 image/s en 2622×1206), 1 par défaut.
// Sur appareil, 2 par défaut (voir plus bas). P2_IOS_RENDER_SCALE=1, =native
// ou une valeur entre les deux (1,5…) : moins de pixels, agrandis par l'écran
// (la vue GL prend cette échelle : rt_ios_gl_create_context, gl_ios.m).
float rt_ios_render_scale(void) {
    static float scale;
    if (scale > 0) return scale;
    const char *e = getenv("P2_IOS_RENDER_SCALE");
    int native_requested = e && !strcmp(e, "native");
    float requested = e && *e && !native_requested ? (float)atof(e) : 0;
    if (e && *e && !native_requested && requested <= 1.0f) return scale = 1.0f;
#if TARGET_OS_SIMULATOR
    if (!native_requested && requested <= 1.0f) return scale = 1.0f;
#endif
    __block CGFloat native = 1;
    if ([NSThread isMainThread]) native = UIScreen.mainScreen.nativeScale;
    else dispatch_sync(dispatch_get_main_queue(), ^{ native = UIScreen.mainScreen.nativeScale; });
    if (native <= 1) return scale = 1.0f;
    // Par défaut 2 (iPhone 15 Pro Max : 1864×860 au lieu de 2796×1290) : sans
    // plafond d'écran, 154 i/s contre 90 en natif dans sp_a2_laser_stairs ; le
    // GPU limite (mesuré par tools/ios_bench.py).
    if (!requested && !native_requested) requested = 2.0f;
    return scale = requested > 1.0f && requested < native ? requested : (float)native;
}
static int up(int v) { return (int)lroundf((float)v * rt_ios_render_scale()); }
static int down(int v) { return (int)lroundf((float)v / rt_ios_render_scale()); }

void rt_ios_touch_controls_attach(SDL_Window *window);
void rt_ios_game_state_poll(void);
SDL_Window *rt_ios_create_window(const char *title, int x, int y, int w, int h, Uint32 flags) {
    if (rt_ios_render_scale() > 1) flags |= SDL_WINDOW_ALLOW_HIGHDPI;
    SDL_Window *window = SDL_CreateWindow(title, x, y, down(w), down(h), flags);
    rt_ios_touch_controls_attach(window);   // commandes tactiles (touch_controls.m)
    return window;
}
void rt_ios_get_window_size(SDL_Window *window, int *w, int *h) {
    int pw = 0, ph = 0;
    SDL_GetWindowSize(window, &pw, &ph);
    if (w) *w = up(pw);
    if (h) *h = up(ph);
}
void rt_ios_set_window_size(SDL_Window *window, int w, int h) { SDL_SetWindowSize(window, down(w), down(h)); }
static void mode_up(SDL_DisplayMode *mode) { mode->w = up(mode->w); mode->h = up(mode->h); }
int rt_ios_set_window_display_mode(SDL_Window *window, const SDL_DisplayMode *mode) {
    if (!mode) return SDL_SetWindowDisplayMode(window, NULL);
    SDL_DisplayMode m = *mode; m.w = down(m.w); m.h = down(m.h);
    return SDL_SetWindowDisplayMode(window, &m);
}
int rt_ios_get_window_display_mode(SDL_Window *window, SDL_DisplayMode *mode) {
    int r = SDL_GetWindowDisplayMode(window, mode); if (!r && mode) mode_up(mode); return r;
}
int rt_ios_get_current_display_mode(int display, SDL_DisplayMode *mode) {
    int r = SDL_GetCurrentDisplayMode(display, mode); if (!r && mode) mode_up(mode); return r;
}
int rt_ios_get_desktop_display_mode(int display, SDL_DisplayMode *mode) {
    int r = SDL_GetDesktopDisplayMode(display, mode); if (!r && mode) mode_up(mode); return r;
}
int rt_ios_get_display_bounds(int display, SDL_Rect *rect) {
    int r = SDL_GetDisplayBounds(display, rect);
    if (!r && rect) { rect->x = up(rect->x); rect->y = up(rect->y); rect->w = up(rect->w); rect->h = up(rect->h); }
    return r;
}
Uint32 rt_ios_get_mouse_state(int *x, int *y) {
    int px = 0, py = 0;
    Uint32 buttons = SDL_GetMouseState(&px, &py);
    if (x) *x = up(px);
    if (y) *y = up(py);
    return buttons;
}
void rt_ios_warp_mouse_in_window(SDL_Window *window, int x, int y) { SDL_WarpMouseInWindow(window, down(x), down(y)); }

// Positions absolues et tailles de fenêtre en pixels ; les déplacements
// relatifs (visée) gardent leur échelle, pour ne pas tripler la sensibilité.
int rt_ios_poll_event(SDL_Event *event) {
    rt_ios_game_state_poll();   // menus/chargement ou partie (game_state.c), pour les commandes tactiles
    int got = SDL_PollEvent(event);
    if (!got || !event || rt_ios_render_scale() <= 1) return got;
    switch (event->type) {
    case SDL_MOUSEMOTION: event->motion.x = up(event->motion.x); event->motion.y = up(event->motion.y); break;
    case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
        event->button.x = up(event->button.x); event->button.y = up(event->button.y); break;
    case SDL_WINDOWEVENT:
        if (event->window.event == SDL_WINDOWEVENT_RESIZED || event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            event->window.data1 = up(event->window.data1); event->window.data2 = up(event->window.data2);
        }
        break;
    default: break;
    }
    return got;
}
#endif
