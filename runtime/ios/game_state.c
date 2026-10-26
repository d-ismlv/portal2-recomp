// État du jeu pour les commandes tactiles : interface du jeu (menus, pause,
// écrans de chargement) affichée ou non.
//
// Aucun appel SDL ne le dit sur iOS : le moteur ne capture jamais la souris et
// appelle SDL_ShowCursor(1) à chaque image, en partie comme au menu. On demande
// donc au moteur : IEngineVGui (« VEngineVGui001 », obtenu par CreateInterface
// d'engine.so), méthode IsGameUIVisible, 4e entrée de sa table virtuelle (deux
// destructeurs, GetPanel, IsGameUIVisible ; vérifié dans le code traduit :
// m_pGameUI ? m_pGameUI->IsGameUIActive() : false).
//
// Appelé depuis SDL_PollEvent (sdl_ios.m), sur le thread principal du jeu,
// verrou invité tenu, au plus dix fois par seconde.
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include "modules.h"
#include "rt.h"

static uint32_t engine_vgui;      // objet IEngineVGui invité (0 : pas encore)
static int ui_visible = 1;        // menus au démarrage
static uint64_t next_query, next_lookup;

int rt_ios_game_ui_visible(void) { return ui_visible; }

void rt_ios_game_state_poll(void) {
    uint64_t now = SDL_GetTicks64();
    if (now < next_query) return;
    next_query = now + 100;
    CPU *c = rt_foreign_thread_attach();   // thread invité courant : son CPU
    if (!c) return;
    if (!engine_vgui) {
        if (now < next_lookup) return;
        next_lookup = now + 1000;   // engine.so pas encore initialisé : réessayer
        const GuestModule *engine = rt_find_module("engine.so");
        uint32_t create = engine ? rt_module_symbol(engine, "CreateInterface") : 0;
        static uint32_t name;
        if (!name && (name = rt_alloc(sizeof("VEngineVGui001")))) memcpy(GPTR(name), "VEngineVGui001", sizeof("VEngineVGui001"));
        if (!create || !name) return;
        uint32_t args[2] = {name, 0};
        engine_vgui = rt_invoke(c, create, args, 2);
        if (engine_vgui) fprintf(stderr, "[ios] état du jeu : IEngineVGui à %#x\n", engine_vgui);
        if (!engine_vgui) return;
    }
    uint32_t method = R32(R32(engine_vgui) + 3 * 4);
    int visible = (rt_invoke(c, method, &engine_vgui, 1) & 0xff) != 0;
    if (visible != ui_visible) fprintf(stderr, "[ios] interface du jeu %s\n", visible ? "affichée" : "masquée");
    ui_visible = visible;
}
#endif
