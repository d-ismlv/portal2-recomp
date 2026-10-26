// GLSL ES 1.00 (sortie de gl4es) -> GLSL ES 3.00, pour un contexte OpenGL ES 3.
//
// gl4es traduit le GLSL 1.20 de togl en GLSL ES 1.00, qui n'a ni textures 3D
// (sampler3D sans précision possible) ni échantillonneurs d'ombre
// (shadow2D, shadow2DLod) : sur iOS, environ 1 400 shaders de Portal 2
// échouaient, et les surfaces s'affichaient sans leurs textures. GLSL ES 3.00
// a tout cela ; la réécriture est textuelle, par identifiants entiers :
// version, extensions devenues standard, attribute/varying -> in/out,
// gl_FragColor -> sortie déclarée, fonctions texture*D* -> texture*,
// shadow2D* -> fonctions d'appoint qui rendent le vec4 de GLSL 1.20.
//
// sRGB (srgb != 0, shaders de fragments) : OpenGL ES 3 d'iOS n'a ni
// EXT_texture_sRGB_decode ni EXT_sRGB_write_control, que togl utilise pour
// basculer, par texture et par tracé, le décodage des textures sRGB et
// l'encodage de la sortie (GL_FRAMEBUFFER_SRGB). Les deux conversions sont
// faites ici dans le shader, commandées par des uniformes que srgb.c règle
// avant chaque tracé : un masque d'unités à décoder par type d'échantillonneur
// (p2_srgb_2d, p2_srgb_cube, p2_srgb_3d) et p2_srgb_write.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *data; size_t length, capacity; } Buffer;
static int put(Buffer *b, const char *s, size_t n) {
    if (b->length + n + 1 > b->capacity) {
        size_t capacity = (b->length + n + 1) * 2;
        char *grown = realloc(b->data, capacity);
        if (!grown) return -1;
        b->data = grown; b->capacity = capacity;
    }
    memcpy(b->data + b->length, s, n);
    b->length += n; b->data[b->length] = 0;
    return 0;
}
static int puts_(Buffer *b, const char *s) { return put(b, s, strlen(s)); }

static int ident_char(int c) { return isalnum(c) || c == '_'; }

// Identifiants renommés (mot entier). Les deux tableaux dépendent du stade.
typedef struct { const char *from, *to; } Rename;
static const Rename common[] = {
    {"texture2D", "texture"}, {"texture2DProj", "textureProj"}, {"texture2DLod", "textureLod"},
    {"texture2DLodEXT", "textureLod"}, {"texture2DProjLod", "textureProjLod"}, {"texture2DProjLodEXT", "textureProjLod"},
    {"texture2DGradEXT", "textureGrad"}, {"textureCube", "texture"}, {"textureCubeLod", "textureLod"},
    {"textureCubeLodEXT", "textureLod"}, {"textureCubeGradEXT", "textureGrad"}, {"texture3D", "texture"},
    {"texture3DLod", "textureLod"}, {"texture3DProj", "textureProj"}, {"texture3DProjLod", "textureProjLod"},
    {"shadow2D", "gl4es_shadow2D"}, {"shadow2DEXT", "gl4es_shadow2D"}, {"shadow2DLod", "gl4es_shadow2DLod"},
    {"shadow2DProj", "gl4es_shadow2DProj"}, {"shadow2DProjEXT", "gl4es_shadow2DProj"},
    {"shadow2DProjLod", "gl4es_shadow2DProjLod"}, {"gl_FragDepthEXT", "gl_FragDepth"},
};
static const Rename vertex[] = {{"attribute", "in"}, {"varying", "out"}};
static const Rename fragment[] = {{"varying", "in"}, {"gl_FragColor", "gl4es_FragColor"},
                                  {"gl_FragData", "gl4es_FragData"}};

static const char *renamed(const char *word, size_t n, int is_fragment) {
    const Rename *tables[2] = {common, is_fragment ? fragment : vertex};
    size_t sizes[2] = {sizeof(common) / sizeof(*common),
                       is_fragment ? sizeof(fragment) / sizeof(*fragment) : sizeof(vertex) / sizeof(*vertex)};
    for (int t = 0; t < 2; ++t)
        for (size_t i = 0; i < sizes[t]; ++i)
            if (strlen(tables[t][i].from) == n && !strncmp(word, tables[t][i].from, n)) return tables[t][i].to;
    return NULL;
}

// Extensions de GLSL ES 1.00 devenues standard (ou inutiles) en 3.00.
static int dropped_extension(const char *line) {
    static const char *names[] = {"GL_EXT_shader_texture_lod", "GL_OES_standard_derivatives", "GL_EXT_frag_depth",
                                  "GL_OES_texture_3D", "GL_EXT_shadow_samplers", "GL_EXT_draw_buffers",
                                  "GL_EXT_shader_non_constant_global_initializers"};
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i) if (strstr(line, names[i])) return 1;
    return 0;
}

static int uses(const char *source, const char *word) {
    size_t n = strlen(word);
    for (const char *p = strstr(source, word); p; p = strstr(p + 1, word))
        if ((p == source || !ident_char((unsigned char)p[-1])) && !ident_char((unsigned char)p[n])) return 1;
    return 0;
}

// Type des échantillonneurs samplerN déclarés : 0 = 2D, 1 = cube, 2 = 3D,
// -1 = autre (ombre) ou absent.
enum { MAX_SAMPLERS = 32 };
static void sampler_types(const char *source, signed char types[MAX_SAMPLERS]) {
    memset(types, -1, MAX_SAMPLERS);
    static const char *names[] = {"sampler2D", "samplerCube", "sampler3D"};
    for (const char *p = strstr(source, "uniform"); p; p = strstr(p + 7, "uniform")) {
        if (p != source && ident_char((unsigned char)p[-1])) continue;
        const char *q = p + 7;
        while (*q == ' ' || *q == '\t') ++q;
        if (!strncmp(q, "lowp ", 5)) q += 5; else if (!strncmp(q, "mediump ", 8)) q += 8; else if (!strncmp(q, "highp ", 6)) q += 6;
        int type = -1; size_t n = 0;
        for (int t = 0; t < 3; ++t) {
            size_t m = strlen(names[t]);
            if (!strncmp(q, names[t], m) && !ident_char((unsigned char)q[m])) { type = t; n = m; }
        }
        if (type < 0) continue;
        q += n;
        while (*q == ' ' || *q == '\t') ++q;
        if (strncmp(q, "sampler", 7) || !isdigit((unsigned char)q[7])) continue;
        int unit = atoi(q + 7);
        if (unit >= 0 && unit < MAX_SAMPLERS) types[unit] = (signed char)type;
    }
}
// Appel de texture « nom ( samplerN » : unité N, ou -1.
static int texture_call_unit(const char *after_name) {
    const char *q = after_name;
    while (*q == ' ' || *q == '\t') ++q;
    if (*q++ != '(') return -1;
    while (*q == ' ' || *q == '\t') ++q;
    if (strncmp(q, "sampler", 7) || !isdigit((unsigned char)q[7])) return -1;
    const char *d = q + 7;
    while (isdigit((unsigned char)*d)) ++d;
    if (ident_char((unsigned char)*d)) return -1;
    return atoi(q + 7);
}
static int texture_function(const char *name) {
    return !strcmp(name, "texture") || !strcmp(name, "textureLod") || !strcmp(name, "textureProj") ||
           !strcmp(name, "textureProjLod") || !strcmp(name, "textureGrad");
}
static const char srgb_helpers[] =
    "uniform int p2_srgb_2d;\nuniform int p2_srgb_cube;\nuniform int p2_srgb_3d;\nuniform float p2_srgb_write;\n"
    "vec4 p2_srgb_read(int mask, int unit, vec4 c) {\n"
    "    if (((mask >> unit) & 1) == 0) return c;\n"
    "    vec3 x = clamp(c.rgb, 0.0, 1.0);\n"
    "    return vec4(mix(pow((x + 0.055) / 1.055, vec3(2.4)), x / 12.92, vec3(lessThanEqual(x, vec3(0.04045)))), c.a);\n"
    "}\n"
    "vec3 p2_srgb_encode(vec3 c) {\n"
    "    vec3 x = clamp(c, 0.0, 1.0);\n"
    "    return mix(1.055 * pow(x, vec3(1.0 / 2.4)) - 0.055, x * 12.92, vec3(lessThan(x, vec3(0.0031308))));\n"
    "}\n";

// NULL si le source n'est pas du GLSL ES 1.00 (déjà 3.00, ou autre version).
char *rt_essl1_to_essl3(const char *source, int is_fragment, int srgb) {
    const char *p = source;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    int versioned = !strncmp(p, "#version", 8);
    if (versioned) {
        const char *v = p + 8;
        while (*v == ' ' || *v == '\t') ++v;
        if (strncmp(v, "100", 3)) return NULL;
        p = strchr(p, '\n'); p = p ? p + 1 : p + strlen(p);
    } else p = source;
    Buffer out = {0};
    if (puts_(&out, "#version 300 es\n")) return NULL;
    // Directives de tête (#extension...) recopiées en premier, sauf celles
    // devenues standard ; les précisions des types ajoutés viennent ensuite.
    while (*p == '#' || *p == '\n' || *p == '\r' || *p == ' ' || *p == '\t') {
        const char *end = strchr(p, '\n'); size_t n = end ? (size_t)(end - p) + 1 : strlen(p);
        int extension = !strncmp(p, "#extension", 10);
        if (!extension) break;
        char line[256]; size_t m = n < sizeof(line) - 1 ? n : sizeof(line) - 1;
        memcpy(line, p, m); line[m] = 0;
        if (!dropped_extension(line) && put(&out, p, n)) goto fail;
        p += n;
    }
    if (puts_(&out, "precision highp float;\nprecision highp int;\nprecision highp sampler3D;\n"
                    "precision highp sampler2DShadow;\nprecision highp samplerCubeShadow;\n"))
        goto fail;
    int shadow = uses(source, "shadow2D") || uses(source, "shadow2DEXT") || uses(source, "shadow2DLod") ||
                 uses(source, "shadow2DProj") || uses(source, "shadow2DProjEXT") || uses(source, "shadow2DProjLod");
    int frag_color = is_fragment && uses(source, "gl_FragColor");
    int frag_data = is_fragment && uses(source, "gl_FragData");
    // Les deux à la fois (shaders de pipeline fixe de gl4es autour d'un shader
    // de togl) : une seule sortie, gl_FragColor devient gl4es_FragData[0].
    int color_as_data = frag_color && frag_data;
    if (color_as_data) frag_color = 0;
    // Sorties déclarées en tête : gl4es peut les écrire dans une fonction
    // placée avant main (_gl4es_main de ses shaders de pipeline fixe).
    if (frag_color && puts_(&out, "out vec4 gl4es_FragColor;\n")) goto fail;
    if (frag_data && puts_(&out, "layout(location = 0) out vec4 gl4es_FragData[4];\n")) goto fail;
    srgb = srgb && is_fragment && (frag_color || frag_data);
    signed char types[MAX_SAMPLERS];
    if (srgb) { sampler_types(source, types); if (puts_(&out, srgb_helpers)) goto fail; }
    static const char *masks[] = {"p2_srgb_2d", "p2_srgb_cube", "p2_srgb_3d"};
    int depth = 0, closes[64], pending = 0;   // parenthèses à fermer après un appel enveloppé
    int inserted = 0;
    // Corps : identifiants renommés ; déclarations ajoutées juste avant main,
    // une fois la précision par défaut des flottants établie par gl4es.
    while (*p) {
        if (!inserted && !strncmp(p, "void main", 9) && (p == source || p[-1] == '\n')) {
            if (shadow && puts_(&out,
                    "vec4 gl4es_shadow2D(sampler2DShadow s, vec3 c) { return vec4(texture(s, c)); }\n"
                    "vec4 gl4es_shadow2DLod(sampler2DShadow s, vec3 c, float l) { return vec4(textureLod(s, c, l)); }\n"
                    "vec4 gl4es_shadow2DProj(sampler2DShadow s, vec4 c) { return vec4(textureProj(s, c)); }\n"
                    "vec4 gl4es_shadow2DProjLod(sampler2DShadow s, vec4 c, float l) { return vec4(textureProjLod(s, c, l)); }\n"))
                goto fail;
            inserted = 1;
            if (srgb) { if (puts_(&out, "void p2_main")) goto fail; p += 9; continue; }
        }
        if (ident_char((unsigned char)*p) && !isdigit((unsigned char)*p)) {
            const char *start = p;
            while (ident_char((unsigned char)*p)) ++p;
            const char *to = renamed(start, (size_t)(p - start), is_fragment);
            if (color_as_data && to && !strcmp(to, "gl4es_FragColor")) to = "gl4es_FragData[0]";
            int unit = srgb && to && texture_function(to) && pending < 64 ? texture_call_unit(p) : -1;
            if (unit >= 0 && unit < MAX_SAMPLERS && types[unit] >= 0) {
                char wrap[64];
                snprintf(wrap, sizeof(wrap), "p2_srgb_read(%s, %d, ", masks[types[unit]], unit);
                if (puts_(&out, wrap)) goto fail;
                closes[pending++] = depth;
            }
            if (to ? puts_(&out, to) : put(&out, start, (size_t)(p - start))) goto fail;
        } else if (p[0] == '/' && p[1] == '/') {   // commentaires recopiés tels quels
            const char *end = strchr(p, '\n'); size_t n = end ? (size_t)(end - p) : strlen(p);
            if (put(&out, p, n)) goto fail;
            p += n;
        } else if (p[0] == '/' && p[1] == '*') {
            const char *end = strstr(p + 2, "*/"); size_t n = end ? (size_t)(end - p) + 2 : strlen(p);
            if (put(&out, p, n)) goto fail;
            p += n;
        } else if (isdigit((unsigned char)*p)) {   // nombres (1.0e5, 2u...) recopiés d'un bloc
            const char *start = p;
            while (ident_char((unsigned char)*p) || *p == '.') ++p;
            if (put(&out, start, (size_t)(p - start))) goto fail;
        } else {
            if (put(&out, p, 1)) goto fail;
            if (*p == '(') ++depth;
            else if (*p == ')') {
                --depth;
                while (pending && closes[pending - 1] == depth) { if (put(&out, ")", 1)) goto fail; --pending; }
            }
            ++p;
        }
    }
    if (!inserted || pending) goto fail;   // pas de main : laisser le source tel quel
    if (srgb) {
        const char *output = frag_color ? "gl4es_FragColor" : "gl4es_FragData[0]";
        char tail[256];
        snprintf(tail, sizeof(tail), "\nvoid main() {\n    p2_main();\n    if (p2_srgb_write > 0.5) %s.rgb = p2_srgb_encode(%s.rgb);\n}\n",
                 output, output);
        if (puts_(&out, tail)) goto fail;
    }
    return out.data;
fail:
    free(out.data);
    return NULL;
}

#ifdef RT_ESSL3_TEST
#include <stdio.h>
int main(void) {
    const char *fs = "#version 100\n#extension GL_EXT_shader_non_constant_global_initializers : enable\n"
                     "vec4 gl4es_ClipVertex;\nprecision highp float;\nuniform sampler3D sampler15;\n"
                     "uniform sampler2DShadow sampler3;\nuniform sampler2D sampler1;\nvarying vec4 otex;\nvoid main()\n{\nvec4 r0 = texture3D( sampler15, otex.xyz );\n"
                     "r0 *= texture2D(sampler1, vec2(max(otex.x, 0.5), otex.y)) /* (x) */;\n"
                     "r0 += shadow2D(sampler3, otex.xyz) * 2.0e-1;\ngl_FragColor = r0;\n}\n";
    char *s = rt_essl1_to_essl3(fs, 1, 1);
    puts(s ? s : "(null)");
    free(s);
    return 0;
}
#endif
