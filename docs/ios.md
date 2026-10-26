# Port iOS

Branche `feat/ios-spike`. Le jeu tourne sur iPhone 15 Pro Max (iOS 27) :
menus, partie, changement de niveau, voix, manette tactile.

## Chaîne de rendu

togl (D3D9 → OpenGL 2.1) → `runtime/gl.c` → `runtime/ios/gl_ios.m` → gl4es
(statique, `ios/deps/gl4es-ios.patch`) → OpenGL ES 3 (OpenGLES.framework).

- `runtime/ios/essl3.c` réécrit chaque nuanceur GLSL ES 1.00 de gl4es en
  ES 3.00 : émulation sRGB (décodage par unité, encodage par sortie),
  bordures de texture, et permutation des couleurs BGRA des attributs `vN`.
- `runtime/ios/srgb.c` suit l'état sRGB/bordure entre togl et gl4es et pose
  les uniformes avant chaque dessin.
- `gl_ios.m`, à la frontière gl4es → ES : formats dimensionnés d'ES 3,
  textures 16 bits en demi-flottants, écritures `glBufferSubData` non
  synchronisées (`glMapBufferRange` + `GL_MAP_UNSYNCHRONIZED_BIT`).

## Construire et lancer

```sh
ios/deps/build.sh device                          # SDL2, FreeType, gl4es (une fois, ou après un changement du correctif)
python3 tools/build_ios.py --target device --release   # pour jouer : -O2 sans contrôle mémoire
tools/ios_device.sh install                       # installe sur l'iPhone branché
tools/ios_device.sh data                          # copie les données du jeu (~11,5 Go, une fois)
tools/ios_device.sh run 120 VAR=valeur…           # lance, attend, rapatrie Documents/game.log
```

Simulateur : `python3 tools/build_ios.py` puis `tools/run_ios_sim.sh <secondes>`
(`P2_SIM_DEVICE=<UDID>` pour un autre simulateur). Le simulateur rend en
logiciel et interpole mal les triangles découpés au plan proche
(`ios/probe/clip_test.c`) : ses artefacts n'existent pas sur l'appareil.

Signature (compte gratuit) : profils de 7 jours obtenus par
`xcodebuild -allowProvisioningUpdates` sur `ios/signing/project.yml`, puis
`ios/signing/sign.py` (appelé par `build_ios.py`). Le lancement échoue si
l'iPhone est verrouillé.

`Documents/launch.args` dans le conteneur de l'app ajoute des arguments
(un par ligne), par exemple `+map` puis `sp_a2_laser_intro`.

## Diagnostics

| Variable | Effet |
|---|---|
| `P2_LOG_FILE=Documents/game.log` | journal (relatif au conteneur) |
| `P2_FRAME_LOG=1` | cadence toutes les 60 images |
| `P2_PROFILE=Documents/profile.txt` (+ `P2_PROFILE_DELAY=60`, `P2_PROFILE_RAW=1`) | profileur intégré ; avec `RAW`, `tools/ios_symbolize.py` résout les fonctions du pilote par les symboles de l'appareil (Xcode, iOS DeviceSupport) |
| `P2_TRACE_FILES=<texte>` | fopen/open/unlink/rename dont le chemin contient le texte |
| `P2_TOUCH_CONTROLS=0/1` | manette tactile désactivée / toujours affichée |
| `P2_TOUCH_ACCEL=2.5` (+ `_LOW=300`, `_HIGH=1500`, `_SMOOTH=0.5`, `_LOG=1`) | accélération du trackpad : gain 1 sous `_LOW` pt/s, montée progressive jusqu'au multiplicateur `P2_TOUCH_ACCEL` à `_HIGH` pt/s ; vitesse lissée (poids `_SMOOTH`) ; `=1` la coupe, `_LOG` journalise vitesse et gain |
| `P2_BACKTAP=0` | coupe les tapes au dos (actives par défaut si le capteur existe ; voir « Tapes au dos ») |
| `P2_BACKTAP_LOG=1` | journalise chaque candidat (dès la moitié des seuils) et le bruit de fond toutes les 5 s |
| `P2_TOUCH_TEST_CLICK=s,x,y` | clic gauche en (x, y) points, s secondes après le premier menu |
| `P2_TOUCH_TEST_MOVE=0.5,1` (+ `_SECONDS`, `_DELAY`, `_ANGLE`, `P2_TOUCH_TEST_WASD`) | inclinaisons imposées au stick de déplacement |
| `P2_IOS_RENDER_SCALE=1/2/native` | échelle de rendu (points → pixels) ; `tools/ios_bench.py` compare antialiasing × échelle |
| `P2_GL_STREAM_UNSYNC=0`, `P2_IOS_BGRA=0`, `P2_IOS_SRGB=0` | désactivent les chemins rapides ou l'émulation |
| `P2_IOS_SHADER_DUMP=<dossier>`, `P2_IOS_GL_TRACE=1`, `P2_IOS_DRAW_LOG=<image>` | nuanceurs convertis, compilation, dessins d'une image |

Console du jeu : `-condebug` l'écrit dans
`Documents/portal2-userdata/portal2/console.log`.

## Tapes au dos

Tenu en paysage, un index qui tape le dos près du bord gauche tire le portail
bleu, près du bord droit l'orange (`runtime/ios/back_tap.c`, testé par
`tests/checks_back_tap.c` ; CoreMotion dans `touch_controls.m`, file dédiée,
sans autorisation). Candidat : |accélération utilisateur| ≥ seuil et variation
d'un échantillon au suivant ≥ seuil de secousse ; jugé 100 ms plus tard.
Côté : signe de la rotation brève autour de l'axe x de l'appareil (vertical en
paysage), rapporté à l'orientation de l'écran (gravité à défaut) ; repli
« latéral » (tape sur la tranche) par le signe de l'accélération selon y.
Rejets : menu ou surcouche masquée (jusqu'à 80 ms avant le pic), toucher qui commence ou finit à ±80 ms,
agitation ou gravité qui change avant le pic (hors des 300 ms qui suivent le candidat d'avant, 80 ms d'historique au moins), téléphone posé à plat écran vers le haut (gravité z < -0,95). Clic :
appui, relâchement après 50 ms et au moins deux images. Valeurs par défaut
calibrées le 26/10/2026 sur iPhone 15 Pro Max (`P2_BACKTAP_LOG=1`) : 30 tapes au
dos sur 30 du bon côté dans les deux paysages ; la main fait vibrer le
téléphone 50 à 100 ms après la tape, d'où la limite de durée relâchée ;
CoreMotion livre 100 Hz au plus. Les tapes sur la tranche n'ont pas été
essayées. Les tapes fortes culminent 30 à 40 ms après leur début et font vibrer
le téléphone plus de 150 ms : critère de montée abandonné, vibration exclue sur
300 ms. Limite connue : le premier à-coup d'un geste brusque depuis le repos
ressemble à une tape (l'écarter retarderait chaque tir).

Enregistrement brut pour régler les seuils sur le Mac :
`P2_BACKTAP_RAW=Documents/backtap.raw` (échantillons et touchers), puis
`cc -O2 -o build/backtap-replay tools/backtap_replay.c -lm` et
`build/backtap-replay backtap.raw` (mêmes variables `P2_BACKTAP_*`). `P2_TOUCH_CONTROLS=0` les coupe aussi.

| Variable | Défaut | Rôle |
|---|---|---|
| `P2_BACKTAP_ACCEL` | 0.10 g | pic minimal d'accélération |
| `P2_BACKTAP_JERK` | 6 g/s | secousse minimale |
| `P2_BACKTAP_ROT` | 0.08 rad/s | rotation minimale (axe x) pour décider le côté |
| `P2_BACKTAP_REFRACTORY` | 0.25 s | rien après un candidat |
| `P2_BACKTAP_TOUCH_GUARD` | 0.08 s | rien autour d'un début ou d'une fin de toucher |
| `P2_BACKTAP_FLAT` | -0.95 g | gravité z sous ce seuil : téléphone à plat, ignoré (`-2` : jamais) |
| `P2_BACKTAP_WIDTH` | 0.12 s | durée maximale du pic (au-dessus de 35 % de son maximum ; mesurée sur 100 ms, donc inactive par défaut) |
| `P2_BACKTAP_AGITATION` | 0.05 g | moyenne quadratique maximale dans les 300 ms d'avant |
| `P2_BACKTAP_GRAVITY` | 0.12 g | variation maximale de la gravité dans les 200 ms d'avant |
| `P2_BACKTAP_INVERT` | 0 | `1` : gauche et droite permutées |
| `P2_BACKTAP_LATERAL` | 1 | tape sur la tranche : `-1` sens inversé, `0` ignorée |
| `P2_BACKTAP_ZSIGN` | 0 | `±1` : exige ce signe de a_z au départ du pic (dos contre écran) |
| `P2_BACKTAP_HOLD` | 0.05 s | durée de l'appui |
| `P2_BACKTAP_HZ` | 200 | fréquence demandée à CoreMotion (100 Hz obtenus sur iPhone 15 Pro Max) |

Journal : `[ios] tape <s> : pic, (x y z), secousse, rotation x y z, montée (délai du maximum),
durée, avant (agitation), gravité (variation, puis vecteur), toucher (écart en
ms), orientation -> côté ou raison du rejet`. Constats : tape au dos a_z < 0
au pic, tape à l'écran a_z > 0.

## Performance (sp_a2_laser_intro, iPhone 15 Pro Max, natif 2796×1290)

| Étape | i/s |
|---|---|
| départ (-O1 contrôlé) | ~5 |
| plus de réédition de nuanceur sous plan de découpe (gl4es) | 7-15 |
| `--release` + optimisations de la session Mac | ~11 |
| `glBufferSubData` non synchronisé | ~19 |
| couleurs BGRA dans le tampon, permutées par le nuanceur | ~47 |

Sans plafond d'écran (`tools/ios_bench.py`, `P2_IOS_NO_PRESENT=1`),
sp_a2_laser_stairs au point d'arrivée : natif 2796×1290 90 i/s (MSAA 4×) ou
94 (sans) ; échelle 2 (1864×860) 154 ; 1,5 178. Le GPU limite : échelle 2 par
défaut sur appareil. Vue lourde de la même salle (setpos -188.94 -396.92
-31.97) : ~117 i/s soutenus à l'échelle 2 avec MSAA 4× (~150 avant que le
téléphone chauffe). Pour jouer : 60 Hz par défaut, réguliers ;
`P2_IOS_MAX_FPS=120` demande 120 Hz (cadence alors entre 60 et 120). L'écran ProMotion arrondit la cadence affichée (120, 60,
40…) : une image de plus de 8,3 ms s'affiche à 60.

Leçons : le code du jeu traduit pèse peu ; le coût est dans le pilote ES
d'Apple, qui attend le GPU sur toute écriture dans un tampon en vol et
recopie tous les sommets dès qu'un attribut vient de la mémoire client.
Profiler avec `P2_PROFILE_RAW=1` avant de deviner.

## Pièges propres à iOS

- L'éditeur de liens d'ES refuse un programme dont le nuanceur de fragments
  lit une sortie que celui de sommets n'écrit pas (le GL de bureau l'accepte) :
  les dessins sautés cassaient la vue à travers les portails et figeaient le
  menu Options. `essl3.c` initialise ces sorties.

- Système de fichiers sensible à la casse : le moteur écrit et lit en
  minuscules (`portal2/save/…`), `runtime/paths.c` et
  `runtime/ios/case_path.c` reprennent la casse réelle.
- SDL ne voit jamais le jeu « capturer » la souris : l'état menu/jeu vient du
  moteur (`IEngineVGui::IsGameUIVisible`, `runtime/ios/game_state.c`).
- Manette tactile : vue UIKit posée sur la fenêtre et dessinée de force (la
  boucle de SDL ne valide jamais les transactions UIKit).
