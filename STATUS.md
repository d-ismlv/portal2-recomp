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
