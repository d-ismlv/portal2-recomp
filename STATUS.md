# État — 26 octobre 2026, nuit (reprise Claude)

**Le jeu est jouable** (testé par l'utilisateur : menus, clavier/souris, son,
voix, musique, début de partie, retour au menu, Cmd-Q). Performances
mesurées en jeu (sp_a1_intro1, focus simulé, fenêtre cachée, M3 Pro) :

| build | 1280×720 | « 2560×1440 » sans Retina (en fait 1512×982) | 2560×1440 Retina |
|---|---|---|---|
| `-O1` mémoire contrôlée (25 oct. soir) | ~117 fps | — | — |
| `--release` (`-O2 --fast-memory`), 26 oct. | 235-255 fps | ~263-280 fps | ~215-230 fps (menu) |

Attention : sans mode Retina, macOS réduit une fenêtre plus grande que
l'écran (1512×982 points sur ce MacBook) ; les anciennes mesures
« 2560×1440 » rendaient donc 1512×982 pixels. Le mode Retina rend vraiment
2560×1440 (2,5 fois plus de pixels) dans une fenêtre de 1280×720 points.

Gains de la nuit du 25 au 26 (mesurés avec `tools/bench_game.py`, moyenne de
la seconde moitié de plusieurs parties) : `--release` (×1,7), QoS interactive
(+15 %), caches de fichiers (access/fopen des absents, parents de la racine,
flux libc indexés), et surtout ajouts `glBufferSubData` non synchronisés dans
un stockage neuf (+30 %, relu trois fois par Codex, `P2_GL_STREAM_UNSYNC=0`
pour désactiver). Essais écartés, mesures à l'appui : annoncer 2 ou 4 CPU
(plus lent), relâcher le verrou invité pendant les appels GL (plus lent),
forcer l'usage des tampons, `gl_bufmode`.

MSAA (26 oct., sp_a1_intro6, 2560×1440 Retina, machine chargée par le
simulateur iOS) : togl fait 6 résolutions MSAA par image (3 réduites vers la
texture de réfraction 1024², 2 pleine taille, 1 vers l'écran), chacune après
de nouveaux dessins dans la surface MSAA : aucune n'est redondante. MSAA 4x :
13,4 ms GPU par image ; sans MSAA : 7,0 ms (47 → 75 fps sous charge). En
Retina, baisser l'antialiasing dans le menu Vidéo est le levier principal.
Les lancements de diagnostic (sans `--show`) utilisent maintenant une copie
des données utilisateur (`../work/portal2-userdata-diag`).

Profil CPU fiable (26 oct., `/usr/bin/sample` sur le jeu en régime établi ;
le profileur SIGPROF intégré surpondère les appels système sous macOS) :
thread principal ≈ 50 % code du jeu traduit, 16 % runtime, 11 % pilote
GL/Metal, 8 % présentation (requêtes WindowServer, peut-être dues à la
fenêtre cachée de diagnostic). Gains du jour, sans MSAA en 2560×1440 (limité
CPU), A/B alterné : routes directes des imports chauds (`P2_IMPORT_COUNTS`
pour les compter) 120,9 → 125,6 fps ; cache de cible par site d'appel
indirect + `rt_return` en macro 117,4 → 124,4 fps (machine chargée). Avec
MSAA 4x en 1440p le GPU limite (~13 ms/image) : ces gains n'y changent rien.
Pistes suivantes : ThinLTO/PGO, accès TLS en ligne, présentation en fenêtre
visible à mesurer.

Grandes salles (26 oct. soir, 2560×1440 Retina, fenêtre cachée) :
sp_a2_laser_over_goo et sp_a2_column_blocker ≈ 95 fps machine calme sans
MSAA (GPU ~9 ms/image, donc plafond GPU ~110 fps en 1440p), ~50 fps quand
un autre jeu et une compilation tournent, ~38-40 fps avec MSAA 4x en plus :
c'était la situation de l'utilisateur (30 fps). Les mesures n'ont de sens que
machine calme (vérifier `uptime`). Essai écarté : regrouper les fonctions
chaudes (`P2_PROFILE_WALL=1 P2_PROFILE_ORDER=…`, `tools/hot_order.py`,
`build_game.py --order-file`) : pas de gain visible (78,7 → 78,9 fps).
Préparé, pas encore exécuté : PGO (`build_game.py --pgo-generate` puis
`--pgo-use`, profil écrit toutes les 20 s ; demande ~5 Go de disque).
Comparer deux binaires : `bench_game.py --binary build/<copie>` (la copie doit
rester dans `build/`, le chemin de libsteam_api est relatif à l'exécutable).

Plein écran : la souris vise ~0,5 cm trop haut d'après l'utilisateur ; la zone
sûre sous l'encoche fait 32 points sur ce MacBook. Le journal écrit maintenant
la géométrie réelle (`[sdl] géométrie …`) à la création, aux changements de
taille et de plein écran : un lancement en plein écran donnera les chiffres.
Le SIGBUS de fn_17eabcc0 (3 fois le 26 oct.) survient à la fermeture du jeu
(CPortal_Player, écouteur d'inventaire GC orphelin probable), pas en jeu.

Le thread principal attend surtout le pilote OpenGL d'Apple (soumissions
Metal sur glBufferSubData de tampons en vol, compilation GLSL à la volée,
mise en cache par togl dans `update/glshaders.cfg`). La carte graphique a de
la marge : la cadence varie peu avec la résolution.

Lancer (depuis ce dossier, client Steam ouvert) :

```sh
python3 tools/build_game.py --release  # pour jouer : -O2 sans contrôle mémoire (≈ 10 min à froid)
python3 tools/build_game.py            # diagnostic : -O1, mémoire contrôlée (≈ 20 min à froid)
SteamAppId=620 python3 tools/run_game.py --show --timeout 3600
```

Pour jouer sans terminal : `python3 tools/make_app.py` crée
`~/Applications/Portal 2 Natif.app` (lien vers `build/portal2-native`, chemins
de `config/local.json`, Steam démarré au besoin, sans `-w/-h` : résolution et
mode viennent du menu Vidéo, enregistrés dans `update/cfg/video.txt`).
Journal : `~/Library/Logs/Portal2Natif.log`. À refaire seulement si les
chemins ou arguments changent ; une recompilation suffit sinon.

Retina (actif par défaut, `P2_RETINA=0` pour revenir à l'ancien
comportement) : l'écran est annoncé en pixels (3024×1964), donc le menu
Vidéo propose aussi les grandes résolutions de la table du jeu (jusqu'à
2560×1600). Une résolution qui tient à l'écran en points donne une fenêtre
normale (rendu ×1) ; une plus grande donne une fenêtre Retina de moitié en
points (2560×1440 → fenêtre de 1280×720 points, rendu natif). Bascule à chaud
de `wantsBestResolutionOpenGLSurface`. Plein écran non vérifié en Retina.
F3 affiche/masque un compteur d'images hôte (`P2_FPS=1` : visible dès le
départ).

Diagnostics : `P2_GPU_TIMING=1` (temps GPU par image, requêtes
GL_TIME_ELAPSED), `P2_BLIT_TRACE=<n>` (300 copies glBlitFramebuffer à partir
de la n-ième, avec les dessins faits dans la surface source depuis la copie
précédente), `P2_FRAME_LOG=1` (cadence), `P2_FRAME_DUMP=/chemin.ppm`
(capture du tampon arrière), `P2_WATCH_CALL=0x<adresse>` (arguments d'un
appel invité), fautes hôtes traduites en adresse invitée + pile invitée.
Sans `--show`, la fenêtre est cachée et la synchro verticale désactivée.

Corrections décisives de la soirée : tas invité O(1) (l'ancien était
quadratique), tables de switch non coupées en fonctions, portées de symboles
façon ld.so (libprotobuf de client.so/server.so), /proc générés depuis
l'hôte, binaires Windows de la copie du jeu invisibles (Miles chargeait un
.asi PE), GLX minimal, événements SDL3 filtrés, Steam GameCoordinator.

## Greffons Miles PE (voix MP3) — branche feat/pe-plugins

libMiles charge lui-même les DLL Windows `bin/linux32/*.asi` (mssmp3 décode les
voix MP3). Elles sont traduites d'avance (`tools/peprep.py`,
`lift.generate_pe`, appelés par `build_game.py` → `build/game/gen/pe`) en C
relogeable indexé par RVA ; `runtime/pe.c` reconnaît l'image au premier
transfert vers elle (octets de .text identiques au fichier, relocations
comprises : données vers la vue écrivable de Miles, valeurs vers .text vers la
vue exécutable), puis aiguille. Seuls les .asi traduits et identiques au
fichier restent visibles (`rt_path_windows_binary`). Diagnostic :
`P2_PE_TRACE=1` (fonctions de greffon appelées). Vérifié : plus de « Can't
create mixer », décodage de `vo/announcer/openingexercise01.wav` identique à
afconvert (corrélation 0,9999997). En fenêtre cachée, la carte ne déclenche
pas la réplique d'ouverture et la sortie reste muette : à écouter en vrai.

---

# Point de reprise — 25 octobre 2026

Travail mis en pause à la demande de l’utilisateur après la validation de la
passerelle de threads. **Le jeu ne se lance pas encore : aucun menu ni aucune
fenêtre de jeu n’a été validé.**

## Dernier démarrage réel

Le Mach-O ARM64 appelle le vrai `portal2_linux main`, initialise les modules
`libmimalloc`, `libtier0`, `libvstdlib`, `libsteam_api`, `libtogl`, `launcher` et
`filesystem_stdio`, puis exécute les premiers threads du moteur. Le dernier
arrêt concret est l’import **`SDL_SetHint`**, appelé par `launcher.so`.
Les traces sont dans `build/game/launch.log` et `build/game/discovery.log`.

Les nombres de fonctions et instructions générées mesurent la compilation,
pas la couverture d’exécution. Des méthodes voisines de tables virtuelles sont
compilées comme racines statiques même avant leur première exécution.

## Lot terminé avant la pause

- Copie `stpcpy`, fichiers stdio, positions 32/64 bits et détection EOF/erreur.
- Traduction explicite des structures Linux i386 `stat`/`stat64`.
- Accès aux ressources originales et au dépôt Linux ; écritures de configuration
  sous `../work/portal2-userdata`, sans modifier la copie originale.
- Threads Darwin avec état CPU, pile invitée protégée, TCB, TLS et errno séparés.
  Création, jonction, attribut de taille de pile et paramètres d’ordonnancement.
- Mutex/conditions, attente temporisée, initialisation unique et destructeurs TLS.
- Sérialisation des instructions invitées sur un processeur logique. Les appels
  et branches arrière cèdent périodiquement la main ; les attentes bloquantes
  libèrent le verrou d’exécution. Ce choix privilégie la cohérence sur ARM.
- Futex Linux WAIT/WAKE, y compris variante PRIVATE, avec attente réelle,
  réveils limités, comparaison de valeur et délai d’expiration.

`make test sanitize` passe : oracle codec (513 tailles), ABI/CPU/mémoire,
comparaisons/conversions SSE, fichiers, puis trois threads natifs. Les tests de
threads couvrent boucle AOT concurrente, réveil de condition, jonction, isolation
errno/TCB, héritage MXCSR, pthread_once, destructeurs TLS et futex WAIT/WAKE.
Le binaire de jeu est reconstruit séparément avec `--opt 0` pour le diagnostic.

La revue Claude a été demandée mais **n’a pas pu avoir lieu** : l’organisation
interdit actuellement l’accès par abonnement à Claude Code. Ce n’est pas une
revue réussie ; les vérifications disponibles sont les tests locaux et UBSan.

## Prochaine étape, après autorisation de reprise

Implémenter une passerelle typée vers SDL2 natif ARM64. La bibliothèque et les
en-têtes sont déjà présents : `/opt/homebrew/lib/libSDL2.dylib` et
`/opt/homebrew/include/SDL2`. Aucun nouveau téléchargement Steam n’est nécessaire
pour cette étape. Ne jamais tronquer les pointeurs Darwin 64 bits vers les
adresses invitées 32 bits : utiliser un registre de handles et traduire les
structures SDL, événements et callbacks. Le rendu OpenGL et Steam restent à
traiter ensuite. `steamclient.dylib` présent dans Steam possède aussi une tranche
ARM64, mais aucune passerelle Steam native n’est encore implémentée.

Commandes de reprise, depuis ce dossier :

```sh
make test
python3 tools/discover_game.py --steps 20 --timeout 30
```

`discover_game.py` s’arrête sur tout import/instruction manquant ; il ne traite
ni une sortie normale ni une expiration de délai comme une preuve de jeu lancé.
La configuration locale non versionnée est dans `config/local.json` ; les racines
indirectes découvertes sont dans `config/game-roots.json`.

## Limites à conserver explicites

La destruction du runtime refuse les workers encore actifs pour éviter de
libérer leur mémoire en cours d’exécution. Arrêt coordonné des workers, threads
détachés, attributs de conditions, autres opérations futex, certains appels
Linux et finalisation des modules restent incomplets. Le x87 est modélisé en
précision double. Les lectures de répertoires ne fusionnent pas encore les trois
sources de fichiers. Aucun contournement des imports manquants ne doit servir
à déclarer un lancement réussi.

## Conventions pour les ponts natifs (ajoutées le 25 octobre, reprise Claude)

- **Nouveaux imports** : un fichier `runtime/<domaine>.c` par domaine. Il déclare
  ses noms sur des lignes `// SHIMS: nom1 nom2 ...` (lues par `tools/lift.py`)
  et s'enregistre avec `RT_SHIM_PROVIDER(fonction)` ; la fonction renvoie 0 pour
  un nom inconnu. Ne plus allonger la liste `_CORE_SHIMS` de `lift.py` ni la
  chaîne de `rt_shim` dans `shims.c`.
- **Adresses dynamiques** (`SDL_GL_GetProcAddress`, `alGetProcAddress`, `dlsym`
  vers une bibliothèque hôte) : `rt_dynamic_import(nom)` renvoie une adresse
  invitée stable (plage `0xf8000000`) ; un appel invité y aboutit dans
  `rt_shim(c, nom)` comme un import statique.
- **Pointeurs hôtes** : `rt_handle_new/get/find/free` (plage non projetée
  `0xe0000000`, typée par un identifiant). Une structure que le code invité lit
  directement (SDL_Surface, FT_Face, SDL_Event...) doit être recopiée en mémoire
  invitée au format i386.
- **Retour flottant** : `rt_return_st0(c, valeur)`. **Rappel invité** depuis
  l'hôte : `rt_invoke` (`modules.h`).
- **Rappel invité depuis un thread hôte étranger** (thread audio SDL, CoreAudio) :
  `CPU *c = rt_foreign_thread_attach()` (idempotent : pile protégée, TCB, TLS,
  errno propres), puis `rt_guest_enter(); rt_invoke(c, …); rt_guest_leave();`.
  Détachement explicite ou automatique à la sortie du thread. Tout appel hôte qui
  peut attendre ce thread (verrou de périphérique SDL, jonction) se fait verrou
  invité relâché (`rt_guest_suspend/resume`), sinon interblocage.
- **Tests** : `tests/checks_<domaine>.c` avec `RT_CHECK(nom) { ... }`, liés
  automatiquement dans `abi-smoke` (et sa variante UBSan).
- **Bibliothèques hôtes** : `HOST_CFLAGS`/`HOST_LIBS` du Makefile, recopiés dans
  `tools/build_game.py` (SDL2, FreeType, Fontconfig, iconv, OpenGL, OpenAL).
- Attention : `make clean` efface aussi `build/game/prep` ; le régénérer avec
  `python3 tools/gameprep.py` (quelques secondes).
