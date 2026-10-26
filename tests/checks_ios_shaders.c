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
    assert(strstr(out, "if (p2_srgb_write > 0.5) gl4es_FragColor.rgb = p2_srgb_encode(gl4es_FragColor.rgb);"));
    free(out);
    out = rt_essl1_to_essl3(fs, 1, 0);   // sans émulation : appels directs
    assert(out && !strstr(out, "p2_") && strstr(out, "texture( sampler1, t.xy )"));
    free(out);
}
