# Portal 2 — recompilation native macOS (prototype)

Ce prototype recompile les instructions i386 réelles des bibliothèques Linux
vers du C, puis vers des exécutables Mach-O ARM64 avec Apple Clang. Les accès
mémoire utilisent des adresses invitées 32 bits ; les branchements deviennent
des branchements C. Aucun interpréteur x86, Rosetta ou Wine n'intervient dans
les chemins testés. **Le jeu ne démarre pas encore.**

## Construire et vérifier

Prérequis : macOS 64 bits, outils de ligne de commande Xcode (`clang` et
`objdump` LLVM), Python 3 et bibliothèques Linux i386 de sa copie de Portal 2.
Aucune dépendance Python supplémentaire.

Depuis ce dossier :

```sh
make test
make sanitize
make progress
file build/tier0-smoke build/abi-smoke
```

`make test` exécute les tests Python, le banc utilisant les vrais binaires
et les petites fonctions synthétiques de vérification de l'ABI.
`make sanitize` produit deux exécutables distincts avec UBSan.
`make progress` écrit `build/progress.json`, un audit statique des constructeurs
avec les instructions et imports restant à traiter. Cet audit ne remplace pas
l'exécution : il s'arrête à la première instruction non prise en charge de
chaque fonction et ne découvre pas tous les appels indirects.

Les entrées par défaut sont `../work/portal2-linux/bin/libtier0.so` et
`libvstdlib.so`. Pour changer de copie :

```sh
make clean
make test INPUT_DIR=/chemin/vers/portal2/bin
```

Les images relocalisées et tous les fichiers générés restent dans `build/`,
ignoré par Git. Les bibliothèques originales ne sont pas modifiées. Les racines
indirectes et certaines attentes du banc correspondent à la version des
bibliothèques actuellement présentes ; une autre édition nécessite un nouvel
audit des adresses et des tables.

## État vérifié le 25 octobre 2026

Le graphe sélectionné contient **151 fonctions natives et 9 916 instructions**.
Ce nombre comprend des chemins d'erreur non exercés et ne mesure pas la
couverture d'exécution du moteur.

Le banc natif exécute `DT_INIT` et **les 14 entrées de `init_array` de
`libtier0.so`** : indices 0–13. Il vérifie notamment l'enregistrement de
sept canaux de journalisation et de onze destructeurs. Les entrées de détection CPU/horloge et de configuration SSE passent également,
avec un profil CPU invité explicite cohérent avec le compteur RDTSC virtuel. Les
constructeurs testés sont invoqués explicitement ; cela ne constitue pas un
chargement complet du module ni une validation des dépendances des autres
modules. Les destructeurs sont enregistrés, pas exécutés.

Autres vérifications :

- 513 tailles de données pour `EncodeBinaryToString` et `DecodeBinaryFromString`,
  comparées à un oracle de format indépendant, avec erreurs de capacité,
  chaînes invalides, limites de sortie et pointeurs optionnels.
- Allocateur Source réel : `Plat_Alloc`, `Plat_Realloc`, `Plat_Free`, via ses
  méthodes virtuelles recompilées. Installation et relecture de 40 entrées
  par `UnitTestInstallTestCase`, `UnitTestCount` et `GetUnitTest`, couvrant
  plusieurs agrandissements de la table.
- Shims mémoire, tas invité, conservation des allocations après échec de
  `realloc`, débordement de `calloc`, mutex récursifs et statiques, erreurs
  de mutex détruits et identité du thread.
- Formatage i386 `snprintf`/`vsnprintf` : tailles de `long` et `size_t` à 32 bits,
  entiers 64 bits, arguments de largeur/précision, double, `%n` et troncature.
- Appels directs, indirects et récursifs, appels terminaux, thunks PIC,
  indicateurs persistants et `RET imm16` ; opérations atomiques alignées,
  copies répétées avec DF dans les deux sens et répétition de longueur zéro.
- Premières opérations SSE/SSE2 et x87 : copies de registres, conservation et
  remise à zéro des voies hautes, calcul scalaire, conversions, comparaisons
  et pile flottante.

`Plat_GetMemPageSize` conserve le résultat original (4), distinct de la taille
de page hôte. Les tests vérifient le comportement du code invité.

## Démarrage du programme réel (travail en cours)

Les modules du dépôt Linux Steam 661 ont été récupérés avec le compte du
propriétaire et préparés localement. `config/local.json` (ignoré par Git)
désigne ces binaires et les données du jeu existantes. Les binaires du jeu
restent hors des sources versionnées.

```sh
python3 tools/gameprep.py
python3 tools/run_game.py --build
```

`build/portal2-native` est un Mach-O ARM64 qui initialise les vrais modules
Linux recompilés et appelle le vrai `main` de `portal2_linux`. Le dernier
lancement est consigné dans `build/game/launch.log`. Il entre dans `main` et
initialise `libtier0`, `libvstdlib`, `libsteam_api`, `libtogl`, `launcher`
et `filesystem_stdio`, mais **aucune fenêtre de jeu n'est encore validée**.
`config/game-roots.json` conserve les fonctions indirectes découvertes.
`tools/discover_game.py` peut ajouter des racines après un arrêt explicite
sur une cible ELF non recompilée ; il ne saute aucune instruction ou import
incomplet et ne considère pas une sortie normale comme un lancement réussi.

Les ajouts récents comprennent TLS et le TCB i386 via GS, les objets importés
stdio/environ, les horloges, les descripteurs et fichiers, le formatage
variadique, et des pages virtuelles Linux de 4 Kio avec vérification des
permissions même lorsqu'elles partagent une page Darwin de 16 Kio.
Les appels stat/stat64 traduisent explicitement les structures glibc i386.
Les lectures combinent les données existantes et le dépôt Linux ; les écritures
sous le dossier du jeu sont redirigées vers `../work/portal2-userdata`.
Les tests vérifient notamment que ce dossier ne masque pas les ressources
et que modifier une configuration ne modifie pas sa copie originale.
La création et la jonction de threads natifs, les conditions, les destructeurs
TLS et les futex WAIT/WAKE sont maintenant testés. Les instructions invitées
sont sérialisées sur un processeur logique avec des points de cession dans les
appels et les boucles. Le dernier démarrage atteint `SDL_SetHint` ; la passerelle
SDL native, le rendu, les interactions Steam et l’arrêt coordonné des workers
restent à terminer. Voir [STATUS.md](STATUS.md) pour le point de reprise.

## Architecture et limites

- `tools/elfprep.py` prépare les segments et relocations à adresses fixes.
- `tools/lift.py` découvre transitivement les appels directs des racines
  sélectionnées ; `tools/float_lift.py` abaisse les premières opérations
  flottantes. Les racines `module:init:index` désignent les constructeurs et
  `module:code:offset` les fonctions indirectes identifiées dans les tables.
- `runtime/rt.c` réserve 4 Gio sans permission d'exécution, charge les segments
  et gère les appels/retours. Les indicateurs traversent les appels dans CPU.
- `runtime/shims.c` fournit un tas invité de 1 Gio et les premiers ponts libc.
  Aucun pointeur d'allocation hôte n'est exposé au code invité.
- `runtime/pthreads.c` conserve les mutex et conditions Darwin hors de la mémoire
  invitée. `runtime/threads.c` gère les workers natifs et `runtime/scheduler.c`
  sérialise leur exécution invitée avec libération du verrou lors des attentes.
- `runtime/format.c` décode les arguments i386 explicitement, sans convertir
  un `va_list` invité en `va_list` ARM64.

Le Makefile autorise la génération de fonctions ayant des imports encore
incomplets, via `--allow-unimplemented-imports`. La liste est imprimée à chaque
génération ; **si un tel import est effectivement appelé, le runtime s'arrête
avec son nom et son adresse**. Il ne renvoie pas de résultat fictif. L'outil de
recompilation refuse par défaut ces imports sans cette option. Les instructions
non prises en charge restent des erreurs de génération. Les cibles indirectes
absentes du dispatch arrêtent également l'exécution.

Restent notamment à traiter : la passerelle SDL, le rendu, les interactions
Steam, le cycle complet de finalisation des modules, les exceptions C++ et
plusieurs interfaces système. x87 est modélisé en `double`, sans la précision
80 bits ; exceptions et cas dénormalisés flottants ne reproduisent pas encore
entièrement x86. Les opérations atomiques non alignées sont refusées et les
protections ELF finales restent incomplètes. L'arrêt du runtime refuse de
libérer la mémoire de workers encore actifs.

Le graphe de jeu construit au point de pause comprend 1 713 fonctions natives
et 142 433 instructions. Il inclut des méthodes de tables non encore exercées ;
ce nombre ne mesure pas la couverture du jeu. Les modules moteur, client et
serveur sont préparés mais leur démarrage complet n'est pas validé.

## Reprise

Voir [STATUS.md](STATUS.md) : le prochain arrêt réel est `SDL_SetHint`.
Les tests `make test sanitize` passent. Le binaire de diagnostic se reconstruit
avec `python3 tools/build_game.py --opt 0` et la découverte se poursuit avec
`python3 tools/discover_game.py --steps 20 --timeout 30`.

La consultation et les revues duo/Claude ont été tentées mais ont échoué :
l'organisation a désactivé l'accès Claude Code par abonnement. Aucune revue
indépendante réussie n'est revendiquée.
