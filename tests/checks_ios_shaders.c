// Réécriture GLSL ES 1.00 -> 3.00 du port iOS (runtime/ios/essl3.c, C
// portable, testé ici sur l'hôte).
#include "checks.h"
#include <string.h>
#include "../runtime/ios/essl3.c"

static int before(const char *text, const char *first, const char *second) {
    const char *a = strstr(text, first), *b = strstr(text, second);
    return a && b && a < b;
}

// Sortie écrite dans une fonction placée avant main (shaders de pipeline fixe
// de gl4es) : déclarée en tête, sinon « undeclared identifier ».
RT_CHECK(essl3_output_declared_before_helper_functions) {
    const char *fs = "#version 100\nprecision highp float;\nvarying vec4 oT0;\n"
                     "void _gl4es_main()\n{\ngl_FragData[0] = oT0;\n}\n"
                     "void main() {\n _gl4es_main();\n}\n";
    char *out = rt_essl1_to_essl3(fs, 1, 0);
    assert(out);
    assert(before(out, "out vec4 gl4es_FragData[4];", "void _gl4es_main"));
    free(out);
}

// gl_FragColor et gl_FragData dans le même shader : une seule sortie.
RT_CHECK(essl3_frag_color_and_data_share_one_output) {
    const char *fs = "#version 100\nprecision highp float;\nvarying vec4 oT0;\n"
                     "void main() {\ngl_FragData[0] = oT0;\nif (oT0.x > 0.5) gl_FragColor = oT0;\n}\n";
    char *out = rt_essl1_to_essl3(fs, 1, 0);
    assert(out);
    assert(!strstr(out, "gl4es_FragColor"));
    assert(strstr(out, "if (oT0.x > 0.5) gl4es_FragData[0] = oT0;"));
    free(out);
}

// Émulation sRGB/bordure : appels d'échantillonnage enveloppés avec leur
// unité, main renommé et encodage final conditionnel.
RT_CHECK(essl3_emulation_wraps_sampling_and_output) {
    const char *fs = "#version 100\nprecision highp float;\nuniform sampler2D sampler1;\n"
                     "uniform sampler2DShadow sampler3;\nuniform samplerCube sampler5;\nvarying vec4 t;\n"
                     "void main()\n{\nvec4 r = texture2D( sampler1, t.xy );\n"
                     "r += shadow2D(sampler3, t.xyz) + textureCube(sampler5, t.xyz);\ngl_FragColor = r;\n}\n";
    char *out = rt_essl1_to_essl3(fs, 1, 1);
    assert(out);
    assert(strstr(out, "p2_texture(1,  sampler1, t.xy )"));
    assert(strstr(out, "p2_gl4es_shadow2D(3, sampler3, t.xyz)"));
    assert(strstr(out, "p2_texture(5, sampler5, t.xyz)"));
    assert(strstr(out, "void p2_main()"));
    assert(strstr(out, "if (((p2_srgb_write >> 0) & 1) != 0) gl4es_FragColor.rgb = p2_srgb_encode(gl4es_FragColor.rgb);"));
    assert(!strstr(out, "p2_srgb_write >> 1"));   // gl_FragColor : une seule sortie
    free(out);
    // gl_FragData : chaque sortie encodée selon son bit.
    const char *mrt = "#version 100\nprecision highp float;\nvarying vec4 t;\n"
                      "void main()\n{\ngl_FragData[0] = t;\ngl_FragData[1] = t.wzyx;\n}\n";
    char *m = rt_essl1_to_essl3(mrt, 1, 1);
    assert(m && strstr(m, "if (((p2_srgb_write >> 1) & 1) != 0) gl4es_FragData[1].rgb = p2_srgb_encode(gl4es_FragData[1].rgb);"));
    free(m);
    out = rt_essl1_to_essl3(fs, 1, 0);   // sans émulation : appels directs
    assert(out && !strstr(out, "p2_") && strstr(out, "texture( sampler1, t.xy )"));
    free(out);
}

// Attributs vN de togl : couleurs BGRA permutées dans le nuanceur selon le bit
// N de p2_bgra, sans copie CPU (gl4es les lit alors dans le tampon).
RT_CHECK(essl3_vertex_attributes_swizzle_bgra_by_mask) {
    const char *vs = "#version 100\nattribute vec4 v0; \nattribute vec4 v3;\nvarying vec4 c;\n"
                     "void main() {\nc = v3 * v0.x;\ngl_Position = v0;\n}\n";
    char *out = rt_essl1_to_essl3(vs, 0, 1);
    assert(out);
    assert(strstr(out, "uniform int p2_bgra;\nin vec4 v0;"));   // uniforme déclarée avant les fonctions
    assert(strstr(out, "vec4 p2_v3() { return ((p2_bgra >> 3) & 1) != 0 ? v3.zyxw : v3; }\n#define v3 p2_v3()\n"));
    assert(strstr(out, "#define v0 p2_v0()"));
    assert(strstr(out, "c = v3 * v0.x;"));   // usages laissés au préprocesseur
    free(out);
    const char *plain = "#version 100\nattribute vec2 aPosition;\nvoid main() {\ngl_Position = vec4(aPosition, 0.0, 1.0);\n}\n";
    out = rt_essl1_to_essl3(plain, 0, 1);
    assert(out && !strstr(out, "p2_bgra"));   // sans attribut vN : inchangé
    free(out);
}

// Sortie déclarée mais jamais écrite (bufferclearobeystencil_vs20 sans
// couleur, où gl4es déclare _gl4es_FrontColor) : l'éditeur de liens d'ES
// d'Apple refuse le programme si le nuanceur de fragments la lit, et togl ne
// trace alors jamais l'effacement de profondeur des portails. Valeur donnée au
// début de main ; les sorties écrites restent intactes.
RT_CHECK(essl3_unwritten_vertex_outputs_are_initialized) {
    const char *vs = "#version 100\nvarying lowp vec4 _gl4es_FrontColor;\ncentroid varying vec4 oT1;\n"
                     "varying vec4 oT0;\nvarying vec2 uv;\nvarying vec4 arr[2];\nattribute vec4 v0;\n"
                     "void main()\n{\noT0 = v0;\ngl_Position = v0;\n}\n";
    char *out = rt_essl1_to_essl3(vs, 0, 1);
    assert(out);
    assert(strstr(out, "void main()\n{\n_gl4es_FrontColor = vec4(1.0);\noT1 = vec4(0.0);\nuv = vec2(0.0);\n"));
    assert(!strstr(out, "oT0 = vec4(0.0)"));    // écrite par le nuanceur
    assert(!strstr(out, "arr = "));             // tableau laissé tel quel
    free(out);
    // Nuanceur de fragments : ses entrées ne sont pas touchées.
    const char *fs = "#version 100\nprecision highp float;\nvarying lowp vec4 _gl4es_FrontColor;\n"
                     "void main()\n{\ngl_FragColor = vec4(1.0);\n}\n";
    out = rt_essl1_to_essl3(fs, 1, 0);
    assert(out && !strstr(out, "_gl4es_FrontColor = "));
    free(out);
}
