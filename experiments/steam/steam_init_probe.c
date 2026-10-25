// Sonde native ARM64 : SteamAPI_Init contre le vrai client Steam Mac.
// Lit seulement l'état (connexion, AppID, possession) ; n'affiche ni SteamID
// ni pseudonyme. Aucun contournement : c'est le client Steam qui décide.
// Usage : SteamAppId=620 ./steam_init_probe /chemin/vers/libsteam_api.dylib
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SYM(type, name) __typeof__(type) *name = (__typeof__(type) *)dlsym(lib, #name); \
    if (!name) { fprintf(stderr, "symbole absent : %s\n", #name); return 2; }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage : %s libsteam_api.dylib\n", argv[0]); return 2; }
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "dlopen : %s\n", dlerror()); return 2; }
    SYM(bool(void), SteamAPI_Init)
    SYM(void(void), SteamAPI_Shutdown)
    SYM(void *(void), SteamAPI_SteamUser_v021)
    SYM(void *(void), SteamAPI_SteamApps_v008)
    SYM(void *(void), SteamAPI_SteamUtils_v010)
    SYM(bool(void *), SteamAPI_ISteamUser_BLoggedOn)
    SYM(uint64_t(void *), SteamAPI_ISteamUser_GetSteamID)
    SYM(uint32_t(void *), SteamAPI_ISteamUtils_GetAppID)
    SYM(bool(void *, uint32_t), SteamAPI_ISteamApps_BIsSubscribedApp)
    SYM(const char *(void *), SteamAPI_ISteamApps_GetCurrentGameLanguage)

    printf("SteamAppId=%s\n", getenv("SteamAppId") ? getenv("SteamAppId") : "(absent)");
    bool ok = SteamAPI_Init();
    printf("SteamAPI_Init=%d\n", ok);
    if (!ok) return 1;
    void *user = SteamAPI_SteamUser_v021(), *apps = SteamAPI_SteamApps_v008(),
         *utils = SteamAPI_SteamUtils_v010();
    printf("ISteamUser021=%s ISteamApps008=%s ISteamUtils010=%s\n",
           user ? "ok" : "NULL", apps ? "ok" : "NULL", utils ? "ok" : "NULL");
    if (user) printf("BLoggedOn=%d SteamID_valide=%d\n", SteamAPI_ISteamUser_BLoggedOn(user),
                     SteamAPI_ISteamUser_GetSteamID(user) != 0);
    if (utils) printf("GetAppID=%u\n", SteamAPI_ISteamUtils_GetAppID(utils));
    if (apps) printf("BIsSubscribedApp(620)=%d langue=%s\n",
                     SteamAPI_ISteamApps_BIsSubscribedApp(apps, 620),
                     SteamAPI_ISteamApps_GetCurrentGameLanguage(apps));
    SteamAPI_Shutdown();
    return 0;
}
