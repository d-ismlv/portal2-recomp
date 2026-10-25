# Steam pour Portal 2 recompilé — enquête (25 octobre 2026)

**Conclusion.** Le jeu ne démarre pas sans Steam : il faut parler au vrai client.
C'est faisable proprement avec la **`libsteam_api.dylib` officielle (Valve, ARM64)**
déjà présente localement. Elle expose exactement les versions d'interfaces que
Portal 2 demande. Un `SteamAPI_Init` natif avec `SteamAppId=620` **réussit** contre le
client Mac en cours d'exécution, et le client confirme la possession
(`BIsSubscribedApp(620)=1`). Ne reste qu'un pont i386 → ARM64 à écrire.

## 1. Initialisation Steam dans le build Linux

- Modules liés à `libsteam_api.so` (`NEEDED`) : `engine`, `inputsystem`, `localize`,
  `launcher`, `dedicated`, `client`, `matchmaking`, `server`. Ils importent en
  tout **25 fonctions** : `SteamAPI_InitSafe`, `SteamAPI_RunCallbacks`,
  `SteamAPI_(Un)RegisterCallback/CallResult`, `SteamAPI_GetHSteamUser/Pipe`,
  `SteamInternal_ContextInit`, `SteamInternal_FindOrCreateUserInterface`,
  `SteamInternal_CreateInterface`, `SteamAPI_RestartAppIfNecessary` (dans `server`
  seulement), `SteamGameServer_*`, breakpad… `launcher.so` n'importe rien de Steam.
- Il s'agit de l'API moderne des accesseurs `SteamInternal_ContextInit`, version
  SDK **1.53a**. `engine.so` (fonction `0x3c6bb0`, appelée depuis `0x3c5e30`, qui
  appelle `SteamAPI_InitSafe` sans tester le résultat) remplit un contexte de
  23 interfaces. Il renvoie `false` dès qu'une seule manque : `SteamClient020`,
  `SteamUser021`, `SteamFriends017`, `SteamUtils010`, `SteamMatchMaking009`,
  `SteamMatchGameSearch001`, `SteamMatchMakingServers002`, `USERSTATS012`, `APPS008`,
  `SteamNetworking006`, `REMOTESTORAGE016`, `SCREENSHOTS003`, `HTTP003`,
  `SteamController008`, `UGC016`, `APPLIST001`, `MUSIC001`, `MUSICREMOTE001`,
  `HTMLSURFACE_005`, `INVENTORY_V003`, `VIDEO_V002`, `PARENTALSETTINGS001`,
  `SteamInput006`. `client`/`server` demandent aussi `SteamGameServer014`,
  `SteamGameServerStats001`, `SteamGameCoordinator001` et `SteamGameStats001`.
- **Échec = arrêt du jeu.** Dans `SV_InitGameDLL` (`engine.so` `0x55cc4c`), si le
  champ `+0x1c` du contexte (`STEAMAPPS_INTERFACE_VERSION008`) est nul, le jeu appelle
  `Error("Steam is not running. You must start Steam in order to play this game.")`.
  `server.so` (`0x8048b2`) appelle `SteamAPI_RestartAppIfNecessary(GetAppID())` et
  fait échouer `DLLInit` si l'appel renvoie vrai. Aucune option `-nosteam` n'existe ;
  `-insecure` ne concerne que VAC. **Il n'y a donc pas de mode dégradé solo.**
- `libsteam_api.so` invité : `getenv("HOME")`, `"%s/.steam/%s"` (chaîne
  `sdk`/`32` reconstruite sur la pile, donc `~/.steam/sdk32/steamclient.so`), puis
  repli sur un `steamclient.so` local. Il lit aussi `steam.pid`, `steam_appid.txt` et
  `SteamAppId`/`SteamGameId`, et utilise `dlopen`/`dlmopen`/`kill`.

## 2. Notre runtime aujourd'hui

`rt_loader_shim` (`runtime/modules.c`) ne résout `dlopen` que vers les modules
invités recompilés, par nom de base. `steamclient.so` n'en fait pas partie :
`dlopen` renvoie `NULL` (« guest module unavailable »). `kill` et `dlmopen` ne sont
pas encore shimmés. Le chemin Steam n'a jamais été exécuté : le lancement s'arrête
avant, sur `SDL_SetHint`.

## 3. Côté Mac natif : ce qui existe

| Fichier | Architectures | Constat |
|---|---|---|
| `Steam.AppBundle/.../steamclient.dylib` | x86_64 + **arm64**, signé Valve | 38 exports C (`CreateInterface`, `Steam_BGetCallback`…). Sert encore toutes les anciennes versions (`SteamClient006…023`, `SteamUser004…023`) et les **27/27** chaînes demandées par le jeu. |
| `work/downloads/steamworks/steam_api.bundle/.../libsteam_api.dylib` (Steamworks.NET 20.1.0 = SDK **1.53a**) | i386 + x86_64 + **arm64**, signé Valve | Exporte les **25/25** fonctions importées par le jeu, plus l'API « flat » C (`SteamAPI_ISteamUser_GetSteamID`…) et `SteamAPI_ManualDispatch_*`. Ses accesseurs `SteamAPI_SteamXxx_vNNN` correspondent **exactement** aux versions du jeu. |

Aucun Portal 2 Mac installé (`steamapps/common/Portal 2` ne contient qu'une sauvegarde).

## 4. Essai natif (`experiments/steam/steam_init_probe.c`)

Le client Steam Mac **tournait déjà** (`steam_osx`, `ipcserver`) ; je ne l'ai pas lancé.

```
sans SteamAppId : Loaded '.../steamclient.dylib' OK. → "SteamAPI_Init() failed; no appID found." → 0
SteamAppId=620 : SteamAPI_Init=1, ISteamUser021/Apps008/Utils010 ok,
                 BLoggedOn=1, GetAppID=620, BIsSubscribedApp(620)=1, langue=english
```

La sonde n'affiche ni SteamID ni pseudo. Effet visible : pendant environ 1 s, le
profil a pu afficher « en jeu : Portal 2 ». **Non testé** : client arrêté ou mode
hors ligne. Les chaînes `create pipe failed` et `connect to global user failed`
montrent que l'init exige un client lancé et connecté.

## 5. Options et recommandation

**A — recommandée : remplacer `libsteam_api.so` invité par un module hôte**
`runtime/steam.c` qui charge la dylib officielle. Il comprend :
1. Les 25 imports, transmis tels quels. Exception : `UseBreakpad*`/`WriteMiniDump`
   restent sans effet, journalisés, pour ne pas laisser breakpad installer ses
   gestionnaires de signaux par-dessus les nôtres.
2. Des objets mandataires invités : une vtable par interface, faite de
   `rt_dynamic_import("steam:ISteamUser021:N")`. L'hôte associe chaque slot N à la
   fonction flat correspondante. La table vient de l'ordre de déclaration du SDK
   1.53a (les fichiers autogénérés de Steamworks.NET, MIT, sont dans le même paquet).
   Slots implémentés à la demande ; un slot inconnu provoque un `rt_trap`.
3. Les callbacks par `SteamAPI_ManualDispatch_*` : registre côté hôte des
   `CCallbackBase` invités, copie du `pubParam` en mémoire invitée, puis
   `rt_invoke` de `Run`. L'alignement est de 4 octets sur Linux i386 comme sur
   macOS, donc les structures sans pointeur se copient octet pour octet.

Pièges ABI : sur i386, `CSteamID` est renvoyé par pointeur caché (`ret 4`) et passé
sur la pile (8 octets) ; `bool` tient sur 8 bits ; les `const char*` renvoyés
doivent être recopiés dans un cache invité.

Volume : les accesseurs relevés statiquement donnent au moins 22 couples
(interface, slot) distincts. Les appels faits via le contexte de `client`/`server`
échappent à cette heuristique ; il faut compter **80 à 150 méthodes** réellement
utilisées, sur environ 550 dans ces interfaces. **Effort estimé** : 4 à 7 jours
pour le démarrage et le solo (init, user/apps/utils/friends, succès `UserStats`,
`RemoteStorage` pour le cloud, callbacks). Ajouter 3 à 5 jours pour la coop, le
workshop et le navigateur de serveurs : ces fonctions passent des objets réponse
C++ invités au client, ce qui demande des mandataires inverses. Risques : signatures
mal typées (prévoir des tests par slot), callbacks contenant des pointeurs, et
ré-entrance pendant `RunCallbacks` sous le verrou d'exécution invité.

**B — garder `libsteam_api.so` invité et lui faire charger un pont « steamclient ».**
Il faut le même marshalling des vtables, mais vers les objets C++ ARM64 natifs
(sans API flat). S'y ajoutent les particularités Linux à émuler (`~/.steam/sdk32`,
`steam.pid` + `kill`, `dlmopen`, `/proc/self/cmdline`) et le protocole bas niveau
`Steam_BGetCallback`. Plus long (+30 à 50 %) et plus fragile, sans bénéfice.

**Exclu** : renvoyer de fausses interfaces ou forcer `BIsSubscribed`. Ce serait un
émulateur Steam, interdit par les règles du projet.

## 6. Décisions (validées par l'utilisateur)

Option A ; dylib officielle chargée par chemin, jamais versionnée ; essais avec
`SteamAppId=620` acceptés ; client Steam lancé et connecté par l'utilisateur ;
solo d'abord, coop/workshop/navigateur de serveurs ensuite.

## 7. Implémentation (branche `feat/steam-bridge`)

**Fichiers.** `runtime/steam.c` (imports, mandataires, callbacks), `runtime/steam.h`,
`runtime/steam_generated.c` (généré par `tools/gen_steam.py`, ne pas modifier),
`tests/checks_steam.c`, `tools/steam_callsites.py` (mesure statique).

**Chemin de la dylib** : variable `P2_STEAM_API_DYLIB`, sinon clé `steam_api_dylib` de
`config/local.json` (lue relativement à l'exécutable : `<exe>/../config/local.json`), sinon
`../work/downloads/steamworks/steam_api.bundle/Contents/MacOS/libsteam_api.dylib`.
Dylib introuvable au premier appel Steam : `rt_trap` explicite (pas de faux `false`).
`SteamAPI_InitSafe` pose `SteamAppId`/`SteamGameId` depuis la clé `steam_appid` de
`config/local.json` s'ils sont absents de l'environnement.

**Résolution des imports** (`tools/elfprep.py`, 5 lignes). `libsteam_api.so` invité **ne peut
pas** être retiré : il embarque le libsupc++ (`__cxa_guard_*`, `__cxa_throw`,
`__gxx_personality_v0`, vtables `typeinfo`) auquel se lient presque tous les modules. Seuls ses
exports `SteamAPI_*`, `SteamInternal_*`, `SteamGameServer_*` deviennent des imports hôtes. Sans
cela, `libsteam_api_linux.so` (ancien SDK que personne ne charge) capterait ces noms. Ses
références internes (`SteamAPI_Init`, `SteamAPI_IsSteamRunning`…) deviennent aussi des imports :
sans shim, elles piègent si son code devait un jour les atteindre. Adresses des modules inchangées.

**Imports (25).** Transmis à la dylib, sauf :
- `SteamAPI_RunCallbacks` / `SteamGameServer_RunCallbacks` : pompe `SteamAPI_ManualDispatch_*`
  (activé après un `InitSafe`/`GameServer_Init` réussi), sans ré-entrance ;
- `SteamAPI_(Un)RegisterCallback/CallResult` : registre hôte des objets `CCallbackBase` invités ;
  `RegisterCallback` pose `k_ECallbackFlagsRegistered` et `m_iCallback` comme la dylib ;
- `SteamInternal_ContextInit` : un double hôte par bloc invité ; quand la vraie dylib appelle
  son `pFn` (compteur d'init changé), on appelle le `pFn` invité sur le contexte invité ;
- breakpad (`UseBreakpadCrashHandler`, `SetMiniDumpComment`, `WriteMiniDump`) : sans effet,
  journalisé une fois, pour ne pas installer les gestionnaires de signaux de breakpad.

**Interfaces.** Mandataire invité = `[vtable invitée][handle hôte]` ; chaque emplacement est
`rt_dynamic_import("SteamUser021::GetSteamID")`. Emplacements : désassemblage ARM64 des fonctions
flat de la dylib (`ldr x8,[x0]; ldr xN,[x8,#off]; br xN` → off/8, ABI Itanium identique en i386
GCC) ; N donne aussi le nombre d'arguments entiers, comparé à la signature. Signatures :
`NativeMethods.cs` de Steamworks.NET 20.1.0 (MIT) + type de retour des enveloppes C#
(CSteamID/CGameID par valeur → pointeur caché et `ret 4`, chaînes → copie invitée internée,
`GetISteam*` → mandataire). `ISteamController` (absent de Steamworks.NET) reprend les
signatures homonymes d'`ISteamInput` (`Legacy_*`, `Init`/`RunFrame` sans argument), contrôlées
par le nombre d'arguments. Tampons : pointeur suivi d'une taille (`cub*`, `cch*`…) ou tableaux de
taille documentée (`STEAM_INPUT_MAX_*`). **721/772 méthodes** des 25 interfaces sont traduites ;
les autres et les emplacements sans fonction flat (méthodes `DEPRECATED`) piègent avec leur nom,
leur emplacement et la raison. `SteamGameCoordinator001` et `SteamGameStats001` (hors SDK) : si le
client en rend un, mandataire opaque dont tous les emplacements piègent.

Méthodes piégées notables : `ISteamMatchmakingServers` (16/17, objets réponse C++ invités —
navigateur de serveurs), `SetWarningMessageHook` (pointeur de fonction invité), une partie
d'`ISteamUGC`/`ISteamInventory`/`ISteamHTMLSurface` (tableaux de chaînes, structures),
`ISteamNetworking::GetP2PSessionState`, `ISteamUser::GetAvailableVoice`.

**Callbacks.** `GetNextCallback` → copie du `pubParam` en mémoire invitée → `Run(param)`
(emplacement 0) ou, pour `SteamAPICallCompleted_t`, `GetAPICallResult` puis
`Run(param, bIOFailure, hCall)` (emplacement 1). Avant chaque appel, `GetCallbackSizeBytes`
(emplacement 2) de l'objet invité doit égaler la taille hôte, sinon `rt_trap` : protège contre
une disposition divergente (pointeurs). Ordre des emplacements vérifié statiquement : 32 vtables
`[code, code, mov eax,imm; ret]` dans `engine.so`.

**Confidentialité.** `steamclient.dylib` écrit le SteamID64 sur stderr pendant l'init : stderr
est capturé pendant `InitSafe`/`GameServer_Init` et tout SteamID64 est masqué (`XXXX…`).

**Mesure.** `tools/steam_callsites.py` (borne basse : les contextes membres d'objets échappent
à l'heuristique) trouve 13 couples (interface, méthode) — tous traduits. La mesure dynamique
attend que le jeu dépasse l'arrêt SDL.

**Tests** (`make test`, `make sanitize`) : table générée, mandataires, inscription des
callbacks, masquage, et marshalling i386 complet à travers les vraies fonctions flat de la dylib
vers des objets hôtes de test (CSteamID `ret 4`, `uint64` en edx:eax, chaînes, sorties,
`float`, interface rendue). `P2_STEAM_TESTS=1 make test` (client lancé, sinon « skip ») :
`SteamAPI_InitSafe` réel avec `SteamAppId=620`, `BLoggedOn`, `GetSteamID` (validé sans
l'afficher), `GetAppID`=620, `BIsSubscribedApp(620)`, callback `UserStatsReceived_t` (20 octets)
et résultat d'appel `GlobalAchievementPercentagesReady_t`, puis `SteamAPI_Shutdown`.

**Reste.** Solo : mesurer dynamiquement dès que le jeu dépasse SDL ; traduire les méthodes qui
piègent alors. Les appels Steam bloquants (IPC) gardent le verrou d'exécution invité.
Coop/workshop/serveurs : mandataires inverses pour `ISteamMatchmaking*Response`, tableaux de
chaînes UGC, `SetWarningMessageHook` (rappel invité).
