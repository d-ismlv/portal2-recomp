# Audit OpenGL de `libtogl.so` (Linux i386) face à l'OpenGL d'Apple

Sources : désassemblage de `bin/linux32/libtogl.so` (constructeur
`COpenGLEntryPoints` 0x3b050–0x3d820, appelé par `GetOpenGLEntryPoints`) et de
`launcher.so` ; mesures sur ce Mac (M3 Pro, macOS 27.0.1) avec CGL et SDL
(sdl2-compat de /opt/homebrew). Méthode reproductible : chaque appel à
`VoidFnPtrLookup(nom, &okay, requis, repli)` (0x3abd0) donne le nom, le
drapeau `okay` (= extension ou groupe « OpenGL ») et le caractère requis.

## Comment togl obtient GL

- `GetOpenGLEntryPoints(lookup)` reçoit le rappel du launcher (0x36960 dans
  `launcher.so`), qui appelle `SDL_GL_GetProcAddress(nom)`. Aucun import GL
  statique. **194 noms** sont demandés (+ `glXGetCurrentDisplay` et
  `glXQueryExtensionsString`, voir plus bas).
- Version : `glGetString(GL_VERSION)` analysé en `maj.min.patch` ; il faut
  **≥ 2.0.0**, sinon message « PROBLEM: You appear to have OpenGL… » et
  échec.
- Extensions : `strstr` sur `glGetString(GL_EXTENSIONS)` (jamais
  `glGetStringi`), avec repli GLX si le nom manque. Certaines acceptent aussi
  une version (ARB_sync ≥ 3.2, ARB_framebuffer_object ≥ 3.0).
- Fatal : si **une seule** des 124 fonctions du groupe « OpenGL » renvoie
  NULL, `Error("Missing basic required OpenGL functionality.")`. Les 70
  fonctions d'extension ne font qu'éteindre le drapeau de leur extension.
- Contexte : le launcher ne fixe que RGBA8, double tampon, profondeur 24,
  stencil 8, accélération (et `CONTEXT_FLAGS=DEBUG` sous option). **Aucune
  version ni profil demandés** → sur macOS, SDL crée un contexte **legacy
  2.1** (mesuré : « 2.1 Metal - 91.7 », GLSL 1.20).
- Shaders GLSL générés en `#version 120` + `GL_ARB_shader_texture_lod`.
  `#version 330 compatibility` n'est choisi que pour les shaders marqués
  0x1000 (chemin bindless, inaccessible sans `GL_NV_bindless_texture` —
  déduction, non vérifiée à l'exécution).

## Profils Apple

| | legacy 2.1 | core 3.2/4.1 |
|---|---|---|
| `glGetString(GL_EXTENSIONS)` | oui | **NULL + GL_INVALID_ENUM** |
| immédiat/fixe (`glBegin`, `glAlphaFunc`, `glPushAttrib`, `glClipPlane`, client state) | oui | non |
| ARB_vertex/fragment_program, objets ARB_shader_objects | oui | non |
| GLSL 1.20 | oui | non (≥ 1.40/1.50) |
| sampler objects, timer query ARB, VAO ARB, copy_buffer, map_buffer_range | non | oui |

Le profil core est **exclu** : togl utilise massivement le pipeline fixe, les
programmes ARB, `GL_EXTENSIONS` et GLSL 1.20. Il faut le profil legacy, comme
le portage Mac historique.

## Extensions vérifiées par togl, sur legacy 2.1 (Metal, M3 Pro)

Présentes : EXT/ARB_framebuffer_object, EXT_framebuffer_blit,
EXT_framebuffer_multisample(+_blit_scaled), **APPLE_fence, ARB_sync**,
EXT_draw_buffers2, EXT_bindable_uniform, APPLE_flush_buffer_range,
ARB_vertex_buffer_object, ARB_occlusion_query, APPLE_texture_range,
APPLE_client_storage, ARB/EXT_vertex_array_bgra, **EXT_texture_compression_s3tc
+ dxt1**, **EXT_texture_sRGB_decode**, ARB_draw_elements_base_vertex,
EXT_gpu_program_parameters, EXT_timer_query, ARB_shader_texture_lod.

Absentes (toutes facultatives) : NV_fence, ARB_map_buffer_range,
ARB_debug_output, GREMEDY_string_marker, EXT_direct_state_access,
NV_bindless_texture, AMD_pinned_memory, NVX_gpu_memory_info, ATI_meminfo,
ANGLE_dxt3/5, GLX_EXT_swap_control_tear.

Les trois vérifications bloquantes de togl (fence NV/ARB/APPLE ; S3TC ;
sRGB_decode) **passent** sur legacy 2.1.

## Fonctions : présence et comportement mesurés

- `dlsym(OpenGL.framework)` (ce que fait SDL) trouve **176/194** noms. Les 18
  absents sont tous facultatifs : 5 `*FenceNV`, `glStringMarkerGREMEDY`,
  `glDebugMessageCallbackARB`/`ControlARB`, `glBindMultiTextureEXT`, 9
  fonctions NV_bindless. `glX*` : absents (togl n'y recourt que si une
  extension manque dans `GL_EXTENSIONS` ; il affiche alors « Could not find
  required OpenGL entry point 'glXGetCurrentDisplay' » sans échouer).
- `SDL_GL_GetProcAddress` sous macOS renvoie un pointeur même pour des
  fonctions 3.x (mesuré pour `glGenSamplers`). Le groupe requis est donc
  « présent », mais dans un contexte legacy ces fonctions répondent
  **GL_INVALID_OPERATION** sans effet (mesuré) :
  - `glGenSamplers/DeleteSamplers/BindSampler/SamplerParameter*` —
    `glGenSamplers` est appelé 512 fois (une boucle de 0x550 à 0x1d50, pas 0xc) à l'init du contexte (0x4528b) ; les
    noms restent 0. `glBindSampler`/paramètres ne servent que si
    `-gl_enablesamplerobjects` (drapeau 0x5c du contexte, désactivé par
    défaut). **Non bloquant** ; l'erreur GL résiduelle n'est lue que par
    l'unique `glGetError` de diagnostic (0x4127c, sous drapeau debug).
  - `glGenVertexArrays/Delete/Bind` : chargés mais jamais appelés
    directement (aucun site `call *0x378/0x37c/0x380`).
  - `glCopyBufferSubData` : un seul site, cible `GL_PINNED_MEMORY_AMD`
    (0x9160), donc inaccessible sans AMD_pinned_memory.
  - `glQueryCounter` : seulement dans `GLMBeginPIXEvent`/télémétrie (drapeau
    0x4d5240). `glGetQueryObjectui64v` fonctionne sur legacy (EXT_timer_query).
  - `glMapBufferRange`/`glFlushMappedBufferRange` : facultatifs ; togl
    retombe sur `glMapBufferARB` + APPLE_flush_buffer_range.
- Les autres requis (programmes ARB, `glProgramEnvParameters4fvEXT`,
  `glDrawRangeElementsBaseVertex`, `glColorMaskIndexedEXT`…) sont couverts
  par les extensions legacy listées plus haut.

Méthode d'usage : recherche des appels indirects `call *OFF(%reg)` sur
l'offset de chaque pointeur dans la structure `gGL` ; c'est une borne
inférieure (un chargement puis `call *%reg` échappe à la recherche).
Les conclusions « non appelé » ci-dessus ont été relues à la main.

## Conclusion

**Rien de bloquant côté OpenGL Apple pour l'initialisation de togl**, à
condition d'un contexte legacy 2.1 (comportement par défaut de SDL) et d'un
`rt_gl_proc_address` qui, comme SDL sur macOS, renvoie une adresse pour
tout symbole exporté par OpenGL.framework (sinon `glGenSamplers` etc.
manquants ⇒ erreur fatale). Risques restants, non vérifiables avant rendu
réel : bugs du pilote legacy Metal (2.1 n'est plus maintenu depuis 10.14),
performances (pas de map_buffer_range, recopie des tampons mappés par le
pont), et chemins GLSL/ARB précis générés par togl.

## Points du pont qui en découlent

- Pointeurs clients : togl appelle `glVertexAttribPointer` **sans VBO lié**
  avec un tableau invité statique (0x40f75) ; le pont lit la liaison
  courante et traduit seulement dans ce cas. Idem indices/PBO.
- `GLhandleARB` vaut `void *` sur macOS et `unsigned int` sur Linux ; les
  valeurs mesurées sont de petits entiers (1, 3).
- Option alternative non retenue : `libdxvk_d3d9.so` + `shaderapivk.so`
  (`-vulkan`) existent dans ce dépôt, mais demanderaient Vulkan (MoltenVK,
  nouvelle dépendance).
