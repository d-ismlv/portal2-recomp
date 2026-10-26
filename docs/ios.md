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
| `P2_TOUCH_TEST_CLICK=s,x,y` | clic gauche en (x, y) points, s secondes après le premier menu |
| `P2_TOUCH_TEST_MOVE=0.5,1` (+ `_SECONDS`, `_DELAY`, `_ANGLE`, `P2_TOUCH_TEST_WASD`) | inclinaisons imposées au stick de déplacement |
| `P2_IOS_RENDER_SCALE=1/2/native` | échelle de rendu (points → pixels) ; `tools/ios_bench.py` compare antialiasing × échelle |
| `P2_GL_STREAM_UNSYNC=0`, `P2_IOS_BGRA=0`, `P2_IOS_SRGB=0` | désactivent les chemins rapides ou l'émulation |
| `P2_IOS_SHADER_DUMP=<dossier>`, `P2_IOS_GL_TRACE=1`, `P2_IOS_DRAW_LOG=<image>` | nuanceurs convertis, compilation, dessins d'une image |

Console du jeu : `-condebug` l'écrit dans
`Documents/portal2-userdata/portal2/console.log`.

## Performance (sp_a2_laser_intro, iPhone 15 Pro Max, natif 2796×1290)

| Étape | i/s |
|---|---|
| départ (-O1 contrôlé) | ~5 |
| plus de réédition de nuanceur sous plan de découpe (gl4es) | 7-15 |
| `--release` + optimisations de la session Mac | ~11 |
| `glBufferSubData` non synchronisé | ~19 |
| couleurs BGRA dans le tampon, permutées par le nuanceur | ~47 |

Leçons : le code du jeu traduit pèse peu ; le coût est dans le pilote ES
d'Apple, qui attend le GPU sur toute écriture dans un tampon en vol et
recopie tous les sommets dès qu'un attribut vient de la mémoire client.
Profiler avec `P2_PROFILE_RAW=1` avant de deviner.

## Pièges propres à iOS

- Système de fichiers sensible à la casse : le moteur écrit et lit en
  minuscules (`portal2/save/…`), `runtime/paths.c` et
  `runtime/ios/case_path.c` reprennent la casse réelle.
- SDL ne voit jamais le jeu « capturer » la souris : l'état menu/jeu vient du
  moteur (`IEngineVGui::IsGameUIVisible`, `runtime/ios/game_state.c`).
- Manette tactile : vue UIKit posée sur la fenêtre et dessinée de force (la
  boucle de SDL ne valide jamais les transactions UIKit).
