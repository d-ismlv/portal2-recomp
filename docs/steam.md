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

## 6. À demander à l'utilisateur

1. Valider l'option A et l'usage de la `libsteam_api.dylib` officielle de
   `work/downloads/steamworks`, chargée par chemin et jamais versionnée. Son chemin
   irait dans `config/local.json`.
2. Accepter que le jeu soit lancé avec `SteamAppId=620`. C'est le mécanisme
   documenté de Valve, et le client vérifie toujours la possession. Sinon
   `RestartAppIfNecessary` déclencherait `steam://run/620`.
3. Laisser le client Steam lancé et connecté pendant les essais, et accepter que le
   profil affiche « en jeu : Portal 2 ».
4. Périmètre voulu : solo seul (4 à 7 jours) ou aussi coop/workshop (+3 à 5 jours).
