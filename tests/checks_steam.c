// Pont Steam. Sans client : mandataires invités, table générée, enregistrement
// des callbacks, et marshalling i386 à travers les vraies fonctions flat de la
// libsteam_api.dylib officielle (si présente) vers des objets hôtes de test.
// Avec P2_STEAM_TESTS=1 et le client Steam lancé : SteamAPI_Init réel
// (SteamAppId=620), appels par vtable invitée, CSteamID par pointeur caché,
// callback et résultat d'appel pompés. Aucun identifiant n'est affiché.
#include "checks.h"
#include "steam.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define STACK 0x2001c000u
#define SCRATCH 0x2001d000u
#define OBJECTS 0x2001e000u

static const SteamMethodInfo *method(const char *name) {
    for (size_t i = 0; i < steam_method_count; ++i)
        if (!strcmp(steam_methods[i].name, name)) return &steam_methods[i];
    assert(!"Steam method missing from the generated table");
    return NULL;
}
static uint32_t guest_string(uint32_t at, const char *s) { memcpy(GPTR(at), s, strlen(s) + 1); return at; }

// Appel invité cdecl de l'emplacement `name` sur le mandataire `proxy` ; sret placé avant this.
static CPU call_method(uint32_t proxy, const char *name, uint32_t sret, unsigned count, const uint32_t *args) {
    const SteamMethodInfo *m = method(name);
    CPU c; rt_cpu_reset(&c); c.esp = STACK;
    for (unsigned i = count; i; --i) PUSH(&c, args[i - 1]);
    PUSH(&c, proxy);
    if (m->sret) PUSH(&c, sret);
    uint32_t before = c.esp;
    rt_call(&c, R32(R32(proxy) + 4 * m->slot), RT_RETURN_SENTINEL);
    assert(c.eip == RT_RETURN_SENTINEL);
    assert(c.esp == before + (m->sret ? 4u : 0u));   // `ret 4` : l'appelé dépile le pointeur caché
    return c;
}
static uint32_t call_import(const char *name, unsigned count, const uint32_t *args) {
    CPU c; rt_cpu_reset(&c); c.esp = STACK;
    for (unsigned i = count; i; --i) PUSH(&c, args[i - 1]);
    PUSH(&c, RT_RETURN_SENTINEL);
    assert(rt_shim(&c, name) == 1);
    return c.eax;
}

// ---------------------------------------------------------------- fonctions invitées simulées
static unsigned context_calls, run_calls, result_calls;
static uint32_t context_arg, run_object, result_object, result_failed;
static uint64_t result_call;
static uint8_t run_data[64], result_data[64];
static int test_guest(CPU *c, const char *name) {
    if (strncmp(name, "__steamtest_", 12)) return 0;
    uint32_t a0 = R32(c->esp + 4), a1 = R32(c->esp + 8);
    if (!strcmp(name, "__steamtest_context")) { ++context_calls; context_arg = a0; }
    else if (!strcmp(name, "__steamtest_size")) c->eax = R32(a0 + 12);   // taille attendue rangée après m_iCallback
    else if (!strcmp(name, "__steamtest_run")) {
        ++run_calls; run_object = a0; memcpy(run_data, GPTR(a1), R32(a0 + 12) < 64 ? R32(a0 + 12) : 64);
    } else if (!strcmp(name, "__steamtest_result")) {
        ++result_calls; result_object = a0; memcpy(result_data, GPTR(a1), R32(a0 + 12) < 64 ? R32(a0 + 12) : 64);
        result_failed = R8(c->esp + 12); result_call = R64(c->esp + 16);
    } else return 0;
    return 1;
}
RT_SHIM_PROVIDER(test_guest)

// Objet CCallbackBase invité : vptr, flags @4, m_iCallback @8 ; taille attendue @12 (test).
static uint32_t guest_callback(uint32_t at, uint8_t flags, uint32_t size) {
    uint32_t vtable = at + 64;
    W32(vtable, rt_dynamic_import("__steamtest_run"));
    W32(vtable + 4, rt_dynamic_import("__steamtest_result"));
    W32(vtable + 8, rt_dynamic_import("__steamtest_size"));
    W32(at, vtable); W32(at + 4, flags); W32(at + 8, 0); W32(at + 12, size);
    return at;
}

RT_CHECK(steam_generated_table_is_consistent) {
    for (size_t i = 1; i < steam_method_count; ++i) assert(strcmp(steam_methods[i - 1].name, steam_methods[i].name) < 0);
    for (size_t i = 0; i < steam_method_count; ++i) {
        const SteamMethodInfo *m = &steam_methods[i];
        assert(m->iface < steam_interface_count && m->slot < steam_interfaces[m->iface].slots);
        assert(!!m->fn != !!m->why);
        size_t length = strlen(steam_interfaces[m->iface].version);
        assert(!strncmp(m->name, steam_interfaces[m->iface].version, length) && !strncmp(m->name + length, "::", 2));
        for (size_t j = 0; j < i; ++j) assert(steam_methods[j].iface != m->iface || steam_methods[j].slot != m->slot);
    }
    // Les 25 versions servies par la dylib, parmi les 27 demandées par Portal 2.
    assert(steam_interface_count == 25);
    assert(method("SteamUser021::GetSteamID")->slot == 2 && method("SteamUser021::GetSteamID")->sret);
    assert(!method("SteamUser021::BLoggedOn")->sret && method("SteamUser021::BLoggedOn")->slot == 1);
}

RT_CHECK(steam_ids_are_masked_in_logs) {
    char text[] = "Caching Steam ID:  76561190000000001 [x] 765611900000000012 17656119000000000";
    steam_mask_ids(text, strlen(text));
    assert(!strcmp(text, "Caching Steam ID:  XXXXXXXXXXXXXXXXX [x] 765611900000000012 17656119000000000"));
}

RT_CHECK(steam_proxy_layout) {
    CPU cpu; rt_cpu_reset(&cpu);
    int host, other;
    uint32_t proxy = steam_proxy(&cpu, &host, "SteamUser021");
    assert(proxy && steam_proxy(&cpu, &host, "SteamUser021") == proxy);   // même objet hôte, même mandataire
    assert(steam_proxy(&cpu, &other, "SteamUser021") != proxy && !steam_proxy(&cpu, NULL, "SteamUser021"));
    assert(steam_proxy_host(proxy) == &host);
    uint32_t vtable = R32(proxy);
    assert(vtable == steam_vtable(&cpu, "SteamUser021"));
    assert(!strcmp(rt_dynamic_import_name(R32(vtable + 4 * 2)), "SteamUser021::GetSteamID"));
    assert(!strcmp(rt_dynamic_import_name(R32(vtable + 4 * 1)), "SteamUser021::BLoggedOn"));
    // ISteamClient : emplacements sans fonction flat (méthodes DEPRECATED) nommés par numéro.
    uint32_t client = steam_vtable(&cpu, "SteamClient020");
    int gaps = 0;
    for (uint32_t s = 0; s < steam_interfaces[0].slots; ++s) gaps += !strncmp(rt_dynamic_import_name(R32(client + 4 * s)), "SteamClient020::slot", 20);
    assert(gaps > 0);
    // Interface absente du SDK 1.53a : mandataire opaque, tous les emplacements piègent.
    uint32_t opaque = steam_proxy(&cpu, &other, "SteamGameCoordinator001");
    assert(opaque && !strcmp(rt_dynamic_import_name(R32(R32(opaque))), "SteamGameCoordinator001::slot0"));
}

RT_CHECK(steam_callback_registration) {
    uint32_t object = guest_callback(OBJECTS, 0, 20);
    uint32_t args[] = {object, 1101};
    call_import("SteamAPI_RegisterCallback", 2, args);
    assert(R8(object + 4) == 1 && R32(object + 8) == 1101);
    call_import("SteamAPI_UnregisterCallback", 1, args);
    assert(R8(object + 4) == 0 && R32(object + 8) == 1101);
    W8(object + 4, 2);   // k_ECallbackFlagsGameServer posé par CCallback avant l'inscription
    call_import("SteamAPI_RegisterCallback", 2, args);
    assert(R8(object + 4) == 3);
    call_import("SteamAPI_UnregisterCallback", 1, args);
    uint32_t result[] = {object, 0x89abcdefu, 0x01234567u};
    call_import("SteamAPI_RegisterCallResult", 3, result);
    call_import("SteamAPI_UnregisterCallResult", 3, result);
    // Breakpad : sans effet, ne doit rien installer.
    uint32_t none[6] = {0};
    call_import("SteamAPI_UseBreakpadCrashHandler", 6, none);
    call_import("SteamAPI_SetMiniDumpComment", 1, none);
    call_import("SteamAPI_WriteMiniDump", 3, none);
}

// ---------------------------------------------------------------- marshalling via la dylib officielle
// Objets hôtes de test : la dylib appelle leur vtable exactement comme celle du client.
typedef struct { void **vtable; } Fake;
static void *user_vtable[64], *stats_vtable[64], *friends_vtable[64], *client_vtable[64];
static Fake fake_user = {user_vtable}, fake_stats = {stats_vtable}, fake_friends = {friends_vtable}, fake_client = {client_vtable};
static uint64_t fake_steamid(Fake *self) { assert(self == &fake_user); return UINT64_C(0x0110000100abcdef); }
static bool fake_logged_on(Fake *self) { (void)self; return true; }
static bool fake_data_folder(Fake *self, char *buffer, int size) {
    (void)self; assert(size == 32); snprintf(buffer, (size_t)size, "/fake/userdata"); return true;
}
static bool fake_stat_int(Fake *self, const char *name, int32_t *out) {
    (void)self; assert(!strcmp(name, "portals")); *out = -42; return true;
}
static bool fake_stat_float(Fake *self, const char *name, float *out) {
    (void)self; assert(!strcmp(name, "speed")); *out = 1.5f; return true;
}
static uint64_t fake_percentages(Fake *self) { (void)self; return UINT64_C(0x1122334455667788); }
static const char *fake_persona(Fake *self) { (void)self; return "Chell"; }
static const char *fake_friend_persona(Fake *self, uint64_t id) {
    (void)self; return id == UINT64_C(0x0110000100000007) ? "Wheatley" : "?";
}
static void *fake_get_user(Fake *self, int32_t user, int32_t pipe, const char *version) {
    (void)self; assert(user == 3 && pipe == 4 && !strcmp(version, "SteamUser021")); return &fake_user;
}

RT_CHECK(steam_marshalling_through_official_flat_api) {
    if (!steam_dylib_available()) { printf("skip steam marshalling: %s not loadable\n", steam_dylib_path()); return; }
    user_vtable[method("SteamUser021::GetSteamID")->slot] = (void *)fake_steamid;
    user_vtable[method("SteamUser021::BLoggedOn")->slot] = (void *)fake_logged_on;
    user_vtable[method("SteamUser021::GetUserDataFolder")->slot] = (void *)fake_data_folder;
    stats_vtable[method("STEAMUSERSTATS_INTERFACE_VERSION012::GetStatInt32")->slot] = (void *)fake_stat_int;
    stats_vtable[method("STEAMUSERSTATS_INTERFACE_VERSION012::GetStatFloat")->slot] = (void *)fake_stat_float;
    stats_vtable[method("STEAMUSERSTATS_INTERFACE_VERSION012::RequestGlobalAchievementPercentages")->slot] = (void *)fake_percentages;
    friends_vtable[method("SteamFriends017::GetPersonaName")->slot] = (void *)fake_persona;
    friends_vtable[method("SteamFriends017::GetFriendPersonaName")->slot] = (void *)fake_friend_persona;
    client_vtable[method("SteamClient020::GetISteamUser")->slot] = (void *)fake_get_user;
    CPU cpu; rt_cpu_reset(&cpu);
    uint32_t user = steam_proxy(&cpu, &fake_user, "SteamUser021");
    uint32_t stats = steam_proxy(&cpu, &fake_stats, "STEAMUSERSTATS_INTERFACE_VERSION012");
    uint32_t friends = steam_proxy(&cpu, &fake_friends, "SteamFriends017");
    uint32_t client = steam_proxy(&cpu, &fake_client, "SteamClient020");

    W64(SCRATCH, 0);   // CSteamID par pointeur caché, `ret 4`
    CPU c = call_method(user, "SteamUser021::GetSteamID", SCRATCH, 0, NULL);
    assert(c.eax == SCRATCH && R64(SCRATCH) == UINT64_C(0x0110000100abcdef));
    c = call_method(user, "SteamUser021::BLoggedOn", 0, 0, NULL);
    assert(c.eax == 1);
    uint32_t folder[] = {SCRATCH + 0x100, 32};
    c = call_method(user, "SteamUser021::GetUserDataFolder", 0, 2, folder);
    assert(c.eax == 1 && !strcmp(GPTR(SCRATCH + 0x100), "/fake/userdata"));
    uint32_t stat[] = {guest_string(SCRATCH + 0x200, "portals"), SCRATCH + 0x300};
    c = call_method(stats, "STEAMUSERSTATS_INTERFACE_VERSION012::GetStatInt32", 0, 2, stat);
    assert(c.eax == 1 && (int32_t)R32(SCRATCH + 0x300) == -42);
    uint32_t speed[] = {guest_string(SCRATCH + 0x200, "speed"), SCRATCH + 0x300};
    c = call_method(stats, "STEAMUSERSTATS_INTERFACE_VERSION012::GetStatFloat", 0, 2, speed);
    assert(c.eax == 1 && RF(SCRATCH + 0x300) == 1.5f);
    c = call_method(stats, "STEAMUSERSTATS_INTERFACE_VERSION012::RequestGlobalAchievementPercentages", 0, 0, NULL);
    assert(c.eax == 0x55667788u && c.edx == 0x11223344u);   // SteamAPICall_t dans edx:eax
    c = call_method(friends, "SteamFriends017::GetPersonaName", 0, 0, NULL);
    assert(c.eax && !strcmp(GPTR(c.eax), "Chell"));
    uint32_t persona = c.eax;
    assert(call_method(friends, "SteamFriends017::GetPersonaName", 0, 0, NULL).eax == persona);   // copie internée
    uint32_t friend[] = {0x00000007u, 0x01100001u};   // CSteamID par valeur : 8 octets sur la pile
    c = call_method(friends, "SteamFriends017::GetFriendPersonaName", 0, 2, friend);
    assert(!strcmp(GPTR(c.eax), "Wheatley"));
    uint32_t get_user[] = {3, 4, guest_string(SCRATCH + 0x200, "SteamUser021")};
    c = call_method(client, "SteamClient020::GetISteamUser", 0, 3, get_user);
    assert(c.eax == user);   // interface rendue : même mandataire invité
}

RT_CHECK(steam_context_init_caches_like_the_dylib) {
    if (!steam_dylib_available()) { printf("skip steam context: dylib not loadable\n"); return; }
    uint32_t data = OBJECTS + 0x200;
    W32(data, rt_dynamic_import("__steamtest_context")); W32(data + 4, 0); W32(data + 8, 0);
    uint32_t args[] = {data};
    unsigned before = context_calls;
    assert(call_import("SteamInternal_ContextInit", 1, args) == data + 8);
    assert(call_import("SteamInternal_ContextInit", 1, args) == data + 8);
    // Le compteur de la dylib décide : au plus un appel tant qu'aucun Init/Shutdown n'a lieu.
    assert(context_calls - before <= 1 && (context_calls == before || context_arg == data + 8));
}

// ---------------------------------------------------------------- vrai client (P2_STEAM_TESTS=1)
static void pump_until(unsigned *counter, unsigned target) {
    for (int i = 0; i < 200 && *counter < target; ++i) {
        call_import("SteamAPI_RunCallbacks", 0, NULL);
        nanosleep(&(struct timespec){0, 50 * 1000 * 1000}, NULL);
    }
}

RT_CHECK(steam_real_client) {
    const char *enabled = getenv("P2_STEAM_TESTS");
    if (!enabled || strcmp(enabled, "1")) { printf("skip steam client: set P2_STEAM_TESTS=1\n"); return; }
    if (!steam_dylib_available()) { printf("skip steam client: %s not loadable\n", steam_dylib_path()); return; }
    CPU cpu; rt_cpu_reset(&cpu);
    if (!((bool (*)(void))steam_flat(&cpu, "SteamAPI_IsSteamRunning"))()) { printf("skip steam client: Steam is not running\n"); return; }
    setenv("SteamAppId", "620", 1); setenv("SteamGameId", "620", 1);
    assert(call_import("SteamAPI_InitSafe", 0, NULL) == 1);
    uint32_t hUser = call_import("SteamAPI_GetHSteamUser", 0, NULL);
    assert(hUser);
    uint32_t find[] = {hUser, guest_string(SCRATCH + 0x400, "SteamUser021")};
    uint32_t user = call_import("SteamInternal_FindOrCreateUserInterface", 2, find);
    assert(user && R32(user) == steam_vtable(&cpu, "SteamUser021"));
    assert(call_method(user, "SteamUser021::BLoggedOn", 0, 0, NULL).eax == 1);
    W64(SCRATCH, 0);
    CPU c = call_method(user, "SteamUser021::GetSteamID", SCRATCH, 0, NULL);
    uint64_t id = R64(SCRATCH);
    assert(c.eax == SCRATCH && (id >> 56) == 1 && ((id >> 52) & 15) == 1 && (uint32_t)id);   // univers public, compte individuel
    find[1] = guest_string(SCRATCH + 0x400, "SteamUtils010");
    uint32_t utils = call_import("SteamInternal_FindOrCreateUserInterface", 2, find);
    assert(call_method(utils, "SteamUtils010::GetAppID", 0, 0, NULL).eax == 620);
    find[1] = guest_string(SCRATCH + 0x400, "STEAMAPPS_INTERFACE_VERSION008");
    uint32_t apps = call_import("SteamInternal_FindOrCreateUserInterface", 2, find);
    uint32_t portal2[] = {620};
    assert(call_method(apps, "STEAMAPPS_INTERFACE_VERSION008::BIsSubscribedApp", 0, 1, portal2).eax == 1);
    // Callback : UserStatsReceived_t (1101), 20 octets en pack(4).
    find[1] = guest_string(SCRATCH + 0x400, "STEAMUSERSTATS_INTERFACE_VERSION012");
    uint32_t stats = call_import("SteamInternal_FindOrCreateUserInterface", 2, find);
    uint32_t received = guest_callback(OBJECTS + 0x400, 0, 20);
    uint32_t reg[] = {received, 1101};
    call_import("SteamAPI_RegisterCallback", 2, reg);
    unsigned runs = run_calls;
    assert(call_method(stats, "STEAMUSERSTATS_INTERFACE_VERSION012::RequestCurrentStats", 0, 0, NULL).eax == 1);
    pump_until(&run_calls, runs + 1);
    assert(run_calls > runs && run_object == received);
    uint64_t game; memcpy(&game, run_data, 8);
    assert(game == 620);
    call_import("SteamAPI_UnregisterCallback", 1, reg);
    // Résultat d'appel : GlobalAchievementPercentagesReady_t (1110), 12 octets.
    c = call_method(stats, "STEAMUSERSTATS_INTERFACE_VERSION012::RequestGlobalAchievementPercentages", 0, 0, NULL);
    uint64_t call = ((uint64_t)c.edx << 32) | c.eax;
    assert(call);
    uint32_t pending = guest_callback(OBJECTS + 0x600, 0, 12);
    uint32_t set[] = {pending, (uint32_t)call, (uint32_t)(call >> 32)};
    call_import("SteamAPI_RegisterCallResult", 3, set);
    unsigned results = result_calls;
    pump_until(&result_calls, results + 1);
    assert(result_calls > results && result_object == pending && result_call == call && !result_failed);
    memcpy(&game, result_data, 8);
    assert(game == 620);
    call_import("SteamAPI_Shutdown", 0, NULL);
    printf("steam client: init, vtable calls, CSteamID sret, callback and call result verified\n");
}
