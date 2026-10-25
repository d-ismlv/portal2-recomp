# Chemins rapides du code généré

Trois coûts dominaient le code traduit, hors shims hôtes : les contrôles
mémoire logiciels (10 à 14 instructions par accès), le passage de chaque appel
par `rt_call` → `rt_dispatch` → `rt_lookup`, et la relecture des registres
invités après chaque écriture en mémoire invitée.

## Ce qui a changé

- **Registres** : les fonctions générées reçoivent `CPU *restrict c` et copient
  la base mémoire dans une locale `mem_` (macro `RT_MEM`, voir `rt.h`). Les
  fonctions du runtime qui reçoivent le CPU sont déclarées `noescape`.
  Invariants : le CPU vit hors de la mémoire invitée, et aucun runtime ne garde
  le pointeur après son retour (`shim_cpu` et `once_cpu` sont restaurés).
- **Appels** : un `call`/`jmp` vers une adresse fixe traduite devient
  `rt_call_fn`/`rt_jump_fn` (appel C direct). `link_direct` (lift.py) rabat
  sur `rt_call`/`rt_dispatch` toute cible non traduite, et sur `rt_call_import`
  (route du shim déjà résolue) les imports. Les appels et sauts indirects
  passent par `rt_call_any`/`rt_jump_any` (`rt_lookup` en ligne). Le protocole
  des transferts non locaux est inchangé : l'appelant compare `c->eip` à son
  adresse de retour. Chaque chemin rapide cède le verrou invité par `rt_poll`
  (test en ligne de `rt_guest_waiting`).
- **Mémoire** : `RT_FAST_MEMORY` supprime le contrôle logiciel par accès. La
  protection est alors celle des pages hôtes de 16 Kio (union des droits des
  quatre pages Linux de 4 Kio, `vm.c`). Une page de garde de 64 Kio après les
  4 Go absorbe les accès qui débordent.

## Options de build (`tools/build_game.py`)

| Option | Effet | Usage |
|---|---|---|
| (défaut) | contrôle mémoire au grain de 4 Kio, appels directs | développement |
| `--fast-memory` | `-DRT_FAST_MEMORY` | mesures, une fois le jeu validé |
| `--traced-calls` | `-DRT_TRACED_CALLS` : tout appel par `rt_call` | pile invitée complète dans les fautes, `P2_WATCH_CALL` |

`make test` reste en mode contrôlé : `tests/vm_checks.h` vérifie le grain de
4 Kio et échoue avec `RT_FAST_MEMORY`, comme attendu.

## Mesurer

```sh
python3 tools/bench.py --runs 5 --config base= --config fast=-DRT_FAST_MEMORY
```

Le banc (`tests/bench.c`) exécute de vraies fonctions de `libtier0` et
`libvstdlib`, générées comme le jeu (`generate_sharded`) : codec binaire,
UTF-8 ↔ UTF-32, table de symboles de `KeyValuesSystem`. Les sommes de contrôle
doivent être identiques entre configurations. `P2_PROFILE=fichier` (profileur
intégré) fonctionne aussi sur le banc ; passer assez de tours (`… 2400`) pour
dépasser l'intervalle d'écriture de 5 s.

Mesures du 25 octobre 2026 (M3 Pro, `-O1`, médiane de 5) :

| | codec | unicode | keyvals |
|---|---|---|---|
| départ (3a8440a) | 36,0 ms | 87,1 ms | 124,3 ms |
| branche, mémoire contrôlée | 21,8 ms | 89,2 ms | 113,6 ms |
| branche, `--fast-memory` | 22,8 ms | 88,7 ms | 103,7 ms |

`unicode` tourne presque entièrement dans iconv hôte, et `keyvals` en bonne
partie dans `strcmp`/`strcasecmp` hôtes : ce banc borne mal le code généré
seul. Le jeu complet compile en un exécutable de 354 Mo (contre 703 Mo avant).

## Limites connues

- Hors `--traced-calls`, `frames[]` (pile invitée des diagnostics) ne voit plus
  les appels directs ni indirects résolus, et `P2_WATCH_CALL` ne voit que les
  appels passant encore par `rt_call`.
- Les chemins rapides supposent l'exécution invitée sérialisée par le verrou
  (`scheduler.c`) ; rien ici ne rend le code généré sûr en parallèle.
