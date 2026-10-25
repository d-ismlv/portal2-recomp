# Couverture du traducteur i386 → C (audit du 25 octobre 2026)

Commande (≈ 1 min, 11 cœurs ; le cache de désassemblage est dans `build/disasm-cache`) :

```sh
python3 tools/progress.py --all --compile-forms --layout build/game/prep/layout.json --out build/coverage.json
```

Racines : symboles, exports, `DT_INIT/FINI`, `init_array/fini_array` et pointeurs de code
en données (`data_ptrs`) de **tous** les modules préparés, suivis par appels directs.
Mode collecte : chaque instruction refusée est comptée sans arrêter la fonction
(`lift.scan`) ; `lift.lift`, utilisé pour générer, reste fail-closed.
`--compile-forms` compile (`clang -fsyntax-only -Werror`) un exemple traduit de chaque
forme distincte (mnémonique + forme d'opérandes) trouvée dans le jeu.

## Résultat

| | avant ce lot | après |
|---|---|---|
| fonctions découvertes | 115 431 | 115 745 |
| entièrement traduisibles | 110 706 (95,9 %) | 115 745 (100 %) |
| instructions | 9,31 M | 9,79 M (cases de switch incluses) |
| formes distinctes compilées | — | 586, toutes compilent |

Bloqueurs résolus, par nombre de fonctions touchées : `sqrtss` (1 506), « pas de
frontière d'instruction » (1 515 : sauts croisés entre modules mal comptés, et
octets nuls que objdump élidait — corrigé par `objdump -z`), `minss`/`maxss`,
`andnps`, `bt/bts/btr/btc`, `shufps`, `unpck*ps`, `movhps/movlps/movlhps/movhlps`,
`movmskps`, `rcpps/rsqrtps/sqrtps`, `fcomi/fucomi(p)`, `fcmovcc`, `fldcw/fnstcw/fnstsw`,
`fi{add,mul,sub,subr,div,divr}`, `frndint`, `fsin/fcos/fptan/fpatan/fyl2x/fprem`,
`fld/fstp tbyte`, MMX (`movq/movd mm`, `padd*/psub*/pmul*/pack*/punpck*/psra*`, `emms`),
SSE2 entier (`pshufd`, `pcmpeq/gt`, `pmuludq`, `packssdw`, `psrldq`, `punpck*`),
`mul/imul/div/idiv` 8 et 16 bits, `cbw/cwd`, `sahf/lahf`, `rep ret`, `prefetch*`.
Chaque forme a un test sémantique sur du vrai code machine (`tests/lifter_programs.s`,
assemblé par `clang -target i386-linux-gnu`) dans `tests/checks_lifter.c`.

## Sauts indirects

| catégorie (occurrences dans les fonctions découvertes) | nombre |
|---|---|
| tables de switch résolues statiquement | 5 441 |
| `jmp reg` restants (appels terminaux virtuels) | 6 169 |
| `jmp [mem]` restants (vtables, pointeurs) | 2 458 |
| stubs PLT | 1 377 |

Sur toutes les formes de table reconnaissables, 1 632 sont résolues, 1 reste (un
tableau de pointeurs de fonctions de studiorender, pas un switch). Une table résolue
devient un `switch` C de `goto` locaux, mais **la cible calculée par le code invité
est comparée** : toute autre valeur passe par `rt_dispatch` vérifié (piège si non
recompilée). Une table imprécise ne peut donc qu'ajouter des étiquettes mortes.

## Limites connues

- Statique : les cibles indirectes hors `data_ptrs` (vtables construites, callbacks)
  ne sont pas découvertes ; « traduisible » ne veut dire ni exécuté ni juste.
- x87 en double (pas 80 bits) ; `fld/fstp tbyte` convertissent. `rcpps/rsqrtps`
  sont exacts (dans la borne ISA 1,5·2⁻¹², pas bit à bit Intel). NaN par défaut ARM.
- MMX : registres `mm` distincts de la pile x87 (pas d'alias), TOP/tags mis à jour.
- Exceptions C++ : seuls des chemins d'erreur de libstdc++ (bad_alloc, bad_cast,
  length_error, gestionnaire verbose de terminate) lancent ; aucun `catch` dans le code
  du jeu. Un `throw` réel testé (libsteam_api_linux `std::__throw_bad_cast`) déroule la
  pile invitée via `dl_iterate_phdr`/`.eh_frame` traduits, ne trouve pas de gestionnaire
  et finit dans `abort`, comme sous Linux. Les landing pads ne sont pas des points de
  reprise : si un gestionnaire existait, le transfert remonterait jusqu'à une frontière
  native et piégerait explicitement.
