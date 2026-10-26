// sRGB de togl sur OpenGL ES 3 d'iOS (voir essl3.c pour la moitié shader).
//
// togl suppose EXT_texture_sRGB_decode et GL_FRAMEBUFFER_SRGB : une texture
// créée en format sRGB est décodée à la lecture sauf si son paramètre
// TEXTURE_SRGB_DECODE vaut SKIP_DECODE, et la sortie d'un tracé est encodée si
// GL_FRAMEBUFFER_SRGB est actif et que la couleur 0 de la cible est sRGB.
// iOS n'a aucun des deux interrupteurs, et gl4es stocke les textures sRGB
// comme des textures ordinaires (octets non convertis). Ce module suit, entre
// togl et gl4es, l'état qui décide des deux conversions et, avant chaque tracé,
// le transmet aux uniformes ajoutés par essl3.c (p2_srgb_2d/cube/3d,
// p2_srgb_write) au programme courant. Différences restantes avec un vrai
// GPU : filtrage et mélange (blending) faits sur les valeurs encodées.
//
// P2_IOS_SRGB=0 désactive l'émulation (comparaison).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <OpenGLES/ES3/gl.h>

void *gl4es_GetProcAddress(const char *name);

int rt_ios_srgb_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) { const char *e = getenv("P2_IOS_SRGB"); enabled = !(e && *e == '0'); }
    return enabled;
}

#define FRAMEBUFFER_SRGB 0x8DB9
#define TEXTURE_SRGB_DECODE 0x8A48
#define SKIP_DECODE 0x8A4A
#define TEXTURE_3D 0x806F
#define UNITS 16

enum { TEX_SRGB = 1, TEX_SKIP_DECODE = 2 };

// Tableaux indexés par nom d'objet, agrandis à la demande.
typedef struct { unsigned char *flags; size_t size; } ByteMap;
static unsigned char *byte_at(ByteMap *m, GLuint name) {
    if (name >= m->size) {
        size_t size = m->size ? m->size : 1024;
        while (size <= name) size *= 2;
        unsigned char *grown = realloc(m->flags, size);
        if (!grown) return NULL;
        memset(grown + m->size, 0, size - m->size);
        m->flags = grown; m->size = size;
    }
    return &m->flags[name];
}
static unsigned char byte_get(const ByteMap *m, GLuint name) { return name < m->size ? m->flags[name] : 0; }

typedef struct { GLuint *values; size_t size; } NameMap;
static GLuint *name_at(NameMap *m, GLuint name) {
    if (name >= m->size) {
        size_t size = m->size ? m->size : 256;
        while (size <= name) size *= 2;
        GLuint *grown = realloc(m->values, size * sizeof(GLuint));
        if (!grown) return NULL;
        memset(grown + m->size, 0, (size - m->size) * sizeof(GLuint));
        m->values = grown; m->size = size;
    }
    return &m->values[name];
}
static GLuint name_get(const NameMap *m, GLuint name) { return name < m->size ? m->values[name] : 0; }

static ByteMap textures;          // TEX_* par texture
static ByteMap renderbuffers;     // TEX_SRGB par renderbuffer
static NameMap fbo_color0;        // couleur 0 par FBO : texture, ou renderbuffer | RENDERBUFFER_BIT
#define RENDERBUFFER_BIT 0x80000000u
static GLuint bound_renderbuffer;
static GLuint bound[UNITS][3];    // texture liée par unité : 2D, cube, 3D
static GLuint active_unit;
static GLuint draw_fbo, read_fbo;
static int framebuffer_srgb;
static GLuint current_program;
static unsigned char color0_flags(GLuint fbo) {
    GLuint object = name_get(&fbo_color0, fbo);
    return object & RENDERBUFFER_BIT ? byte_get(&renderbuffers, object & ~RENDERBUFFER_BIT) : byte_get(&textures, object);
}

// Uniformes par programme : emplacements (-2 = à chercher) et dernières valeurs.
typedef struct { GLint loc[4]; GLint value[4]; } ProgramState;
static ProgramState *programs;
static size_t program_count;
static ProgramState *program_state(GLuint program) {
    if (program >= program_count) {
        size_t size = program_count ? program_count : 256;
        while (size <= program) size *= 2;
        ProgramState *grown = realloc(programs, size * sizeof(*grown));
        if (!grown) return NULL;
        for (size_t i = program_count; i < size; ++i)
            for (int k = 0; k < 4; ++k) { grown[i].loc[k] = -2; grown[i].value[k] = -1; }
        programs = grown; program_count = size;
    }
    return &programs[program];
}

static int kind_of(GLenum target) {
    if (target == GL_TEXTURE_2D) return 0;
    if (target == GL_TEXTURE_CUBE_MAP || (target >= 0x8515 && target <= 0x851A)) return 1;
    if (target == TEXTURE_3D) return 2;
    return -1;
}
static int srgb_format(GLint internal) { return internal >= 0x8C40 && internal <= 0x8C4F; }

// ---- suivi, appelé par les enveloppes de gl_ios.m ----
void rt_ios_srgb_bind_texture(GLenum target, GLuint name) {
    int kind = kind_of(target);
    if (kind >= 0 && active_unit < UNITS) bound[active_unit][kind] = name;
}
void rt_ios_srgb_tex_image(GLenum target, GLint level, GLint internal) {
    int kind = kind_of(target);
    if (level != 0 || kind < 0 || active_unit >= UNITS) return;
    GLuint name = bound[active_unit][kind];
    unsigned char *f = name ? byte_at(&textures, name) : NULL;
    if (f) *f = (unsigned char)((*f & ~TEX_SRGB) | (srgb_format(internal) ? TEX_SRGB : 0));
}
void rt_ios_srgb_decode(GLenum target, GLint value) {
    int kind = kind_of(target);
    if (kind < 0 || active_unit >= UNITS) return;
    GLuint name = bound[active_unit][kind];
    unsigned char *f = name ? byte_at(&textures, name) : NULL;
    if (f) *f = (unsigned char)((*f & ~TEX_SKIP_DECODE) | (value == SKIP_DECODE ? TEX_SKIP_DECODE : 0));
}

// ---- enveloppes propres à ce module ----
static void (*next_active_texture)(GLenum);
static void (*next_delete_textures)(GLsizei, const GLuint *);
static void (*next_enable)(GLenum), (*next_disable)(GLenum);
static GLboolean (*next_is_enabled)(GLenum);
static void (*next_bind_framebuffer)(GLenum, GLuint);
static void (*next_framebuffer_texture_2d)(GLenum, GLenum, GLenum, GLuint, GLint);
static void (*next_framebuffer_renderbuffer)(GLenum, GLenum, GLenum, GLuint);
static void (*next_delete_framebuffers)(GLsizei, const GLuint *);
static void (*next_bind_renderbuffer)(GLenum, GLuint);
static void (*next_renderbuffer_storage)(GLenum, GLenum, GLsizei, GLsizei);
static void (*next_renderbuffer_storage_multisample)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
static void (*next_delete_renderbuffers)(GLsizei, const GLuint *);
static void (*next_use_program)(GLuint);
static void (*next_link_program)(GLuint);
static void (*next_compressed_tex_image_2d)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *);
static void (*next_draw_arrays)(GLenum, GLint, GLsizei);
static void (*next_draw_elements)(GLenum, GLsizei, GLenum, const void *);
static void (*next_draw_range_elements)(GLenum, GLuint, GLuint, GLsizei, GLenum, const void *);
static void (*next_draw_elements_base_vertex)(GLenum, GLsizei, GLenum, const void *, GLint);
static void (*next_draw_range_elements_base_vertex)(GLenum, GLuint, GLuint, GLsizei, GLenum, const void *, GLint);
static GLint (*g4_get_uniform_location)(GLuint, const GLchar *);
static void (*g4_uniform1i)(GLint, GLint);
static void (*g4_uniform1f)(GLint, GLfloat);

static void active_texture(GLenum unit) { active_unit = unit - GL_TEXTURE0; next_active_texture(unit); }
static void delete_textures(GLsizei n, const GLuint *names) {
    for (GLsizei i = 0; i < n; ++i) {
        if (names[i] < textures.size) textures.flags[names[i]] = 0;
        for (int u = 0; u < UNITS; ++u)
            for (int k = 0; k < 3; ++k) if (bound[u][k] == names[i]) bound[u][k] = 0;
    }
    next_delete_textures(n, names);
}
static void enable(GLenum cap) { if (cap == FRAMEBUFFER_SRGB) framebuffer_srgb = 1; else next_enable(cap); }
static void disable(GLenum cap) { if (cap == FRAMEBUFFER_SRGB) framebuffer_srgb = 0; else next_disable(cap); }
static GLboolean is_enabled(GLenum cap) {
    return cap == FRAMEBUFFER_SRGB ? (GLboolean)framebuffer_srgb : next_is_enabled(cap);
}
static void bind_framebuffer(GLenum target, GLuint fbo) {
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) draw_fbo = fbo;
    if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) read_fbo = fbo;
    next_bind_framebuffer(target, fbo);
}
static GLuint target_fbo(GLenum target) { return target == GL_READ_FRAMEBUFFER ? read_fbo : draw_fbo; }
static void framebuffer_texture_2d(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
    GLuint fbo = target_fbo(target), *slot;
    if (attachment == GL_COLOR_ATTACHMENT0 && fbo && (slot = name_at(&fbo_color0, fbo))) *slot = texture;
    next_framebuffer_texture_2d(target, attachment, textarget, texture, level);
}
static void framebuffer_renderbuffer(GLenum target, GLenum attachment, GLenum rbtarget, GLuint rb) {
    GLuint fbo = target_fbo(target), *slot;
    if (attachment == GL_COLOR_ATTACHMENT0 && fbo && (slot = name_at(&fbo_color0, fbo))) *slot = rb ? rb | RENDERBUFFER_BIT : 0;
    next_framebuffer_renderbuffer(target, attachment, rbtarget, rb);
}
static void bind_renderbuffer(GLenum target, GLuint rb) { bound_renderbuffer = rb; next_bind_renderbuffer(target, rb); }
static void renderbuffer_format(GLenum internal) {
    unsigned char *f = bound_renderbuffer ? byte_at(&renderbuffers, bound_renderbuffer) : NULL;
    if (f) *f = srgb_format((GLint)internal) ? TEX_SRGB : 0;
}
static void renderbuffer_storage(GLenum target, GLenum internal, GLsizei w, GLsizei h) {
    renderbuffer_format(internal);
    next_renderbuffer_storage(target, internal, w, h);
}
static void renderbuffer_storage_multisample(GLenum target, GLsizei samples, GLenum internal, GLsizei w, GLsizei h) {
    static int logged;
    if (!logged++) fprintf(stderr, "[srgb] renderbuffer multi-échantillonné : %d échantillons, format %#x, %dx%d\n",
                           samples, internal, w, h);
    renderbuffer_format(internal);
    next_renderbuffer_storage_multisample(target, samples, internal, w, h);
}
static void delete_renderbuffers(GLsizei n, const GLuint *names) {
    for (GLsizei i = 0; i < n; ++i) {
        if (names[i] < renderbuffers.size) renderbuffers.flags[names[i]] = 0;
        if (bound_renderbuffer == names[i]) bound_renderbuffer = 0;
    }
    next_delete_renderbuffers(n, names);
}
static void delete_framebuffers(GLsizei n, const GLuint *names) {
    for (GLsizei i = 0; i < n; ++i) {
        if (names[i] < fbo_color0.size) fbo_color0.values[names[i]] = 0;
        if (draw_fbo == names[i]) draw_fbo = 0;
        if (read_fbo == names[i]) read_fbo = 0;
    }
    next_delete_framebuffers(n, names);
}
static void use_program(GLuint program) { current_program = program; next_use_program(program); }
static void link_program(GLuint program) {
    next_link_program(program);
    ProgramState *s = program_state(program);
    if (s) for (int k = 0; k < 4; ++k) { s->loc[k] = -2; s->value[k] = -1; }
}
static void compressed_tex_image_2d(GLenum t, GLint l, GLenum i, GLsizei w, GLsizei h, GLint b, GLsizei n, const void *d) {
    rt_ios_srgb_tex_image(t, l, (GLint)i);
    next_compressed_tex_image_2d(t, l, i, w, h, b, n, d);
}

// P2_IOS_SRGB_TRACE=1 : statistiques toutes les 2 000 tracés.
static void trace(GLint value[4], const ProgramState *s) {
    static int enabled = -1;
    if (enabled < 0) { const char *e = getenv("P2_IOS_SRGB_TRACE"); enabled = e && *e == '1'; }
    if (!enabled) return;
    static unsigned long draws, srgb_on, fbo_srgb, writes, reads, no_loc;
    ++draws; srgb_on += framebuffer_srgb != 0; reads += (value[0] | value[1] | value[2]) != 0;
    fbo_srgb += draw_fbo && (color0_flags(draw_fbo) & TEX_SRGB);
    writes += value[3] != 0; no_loc += s->loc[3] < 0;
    if (draws % 2000 == 0)
        fprintf(stderr, "[srgb] tracés %lu : FRAMEBUFFER_SRGB %lu, cible sRGB %lu, encodage %lu, décodage %lu, "
                        "sans uniforme %lu (fbo %u couleur0 %u)\n", draws, srgb_on, fbo_srgb, writes, reads, no_loc,
                draw_fbo, name_get(&fbo_color0, draw_fbo));
}

static void sync_uniforms(void) {
    if (!current_program) return;
    ProgramState *s = program_state(current_program);
    if (!s) return;
    static const char *names[4] = {"p2_srgb_2d", "p2_srgb_cube", "p2_srgb_3d", "p2_srgb_write"};
    if (s->loc[0] == -2)
        for (int k = 0; k < 4; ++k) s->loc[k] = g4_get_uniform_location(current_program, names[k]);
    GLint value[4] = {0, 0, 0, 0};
    for (int u = 0; u < UNITS; ++u)
        for (int k = 0; k < 3; ++k) {
            unsigned char f = byte_get(&textures, bound[u][k]);
            if ((f & (TEX_SRGB | TEX_SKIP_DECODE)) == TEX_SRGB) value[k] |= 1 << u;
        }
    if (framebuffer_srgb && draw_fbo) value[3] = color0_flags(draw_fbo) & TEX_SRGB;
    trace(value, s);
    for (int k = 0; k < 4; ++k) {
        if (s->loc[k] < 0 || s->value[k] == value[k]) continue;
        if (k < 3) g4_uniform1i(s->loc[k], value[k]); else g4_uniform1f(s->loc[k], value[k] ? 1.0f : 0.0f);
        s->value[k] = value[k];
    }
}
static void draw_arrays(GLenum m, GLint f, GLsizei c) { sync_uniforms(); next_draw_arrays(m, f, c); }
static void draw_elements(GLenum m, GLsizei c, GLenum t, const void *i) { sync_uniforms(); next_draw_elements(m, c, t, i); }
static void draw_range_elements(GLenum m, GLuint s, GLuint e, GLsizei c, GLenum t, const void *i) {
    sync_uniforms(); next_draw_range_elements(m, s, e, c, t, i);
}
static void draw_elements_base_vertex(GLenum m, GLsizei c, GLenum t, const void *i, GLint b) {
    sync_uniforms(); next_draw_elements_base_vertex(m, c, t, i, b);
}
static void draw_range_elements_base_vertex(GLenum m, GLuint s, GLuint e, GLsizei c, GLenum t, const void *i, GLint b) {
    sync_uniforms(); next_draw_range_elements_base_vertex(m, s, e, c, t, i, b);
}

// Enveloppe `fn` (ce que togl aurait reçu pour `name`) si ce module suit cette
// fonction ; sinon rend `fn`.
void *rt_ios_srgb_wrap(const char *name, void *fn) {
    if (!fn || !rt_ios_srgb_enabled()) return fn;
    static const struct { const char *name; void **next; void *wrapper; } table[] = {
        {"glActiveTexture", (void **)&next_active_texture, (void *)active_texture},
        {"glActiveTextureARB", (void **)&next_active_texture, (void *)active_texture},
        {"glDeleteTextures", (void **)&next_delete_textures, (void *)delete_textures},
        {"glDeleteTexturesEXT", (void **)&next_delete_textures, (void *)delete_textures},
        {"glEnable", (void **)&next_enable, (void *)enable},
        {"glDisable", (void **)&next_disable, (void *)disable},
        {"glIsEnabled", (void **)&next_is_enabled, (void *)is_enabled},
        {"glBindFramebuffer", (void **)&next_bind_framebuffer, (void *)bind_framebuffer},
        {"glBindFramebufferEXT", (void **)&next_bind_framebuffer, (void *)bind_framebuffer},
        {"glFramebufferTexture2D", (void **)&next_framebuffer_texture_2d, (void *)framebuffer_texture_2d},
        {"glFramebufferTexture2DEXT", (void **)&next_framebuffer_texture_2d, (void *)framebuffer_texture_2d},
        {"glFramebufferRenderbuffer", (void **)&next_framebuffer_renderbuffer, (void *)framebuffer_renderbuffer},
        {"glFramebufferRenderbufferEXT", (void **)&next_framebuffer_renderbuffer, (void *)framebuffer_renderbuffer},
        {"glDeleteFramebuffers", (void **)&next_delete_framebuffers, (void *)delete_framebuffers},
        {"glDeleteFramebuffersEXT", (void **)&next_delete_framebuffers, (void *)delete_framebuffers},
        {"glBindRenderbuffer", (void **)&next_bind_renderbuffer, (void *)bind_renderbuffer},
        {"glBindRenderbufferEXT", (void **)&next_bind_renderbuffer, (void *)bind_renderbuffer},
        {"glRenderbufferStorage", (void **)&next_renderbuffer_storage, (void *)renderbuffer_storage},
        {"glRenderbufferStorageEXT", (void **)&next_renderbuffer_storage, (void *)renderbuffer_storage},
        {"glRenderbufferStorageMultisample", (void **)&next_renderbuffer_storage_multisample,
         (void *)renderbuffer_storage_multisample},
        {"glRenderbufferStorageMultisampleEXT", (void **)&next_renderbuffer_storage_multisample,
         (void *)renderbuffer_storage_multisample},
        {"glDeleteRenderbuffers", (void **)&next_delete_renderbuffers, (void *)delete_renderbuffers},
        {"glDeleteRenderbuffersEXT", (void **)&next_delete_renderbuffers, (void *)delete_renderbuffers},
        {"glUseProgram", (void **)&next_use_program, (void *)use_program},
        {"glUseProgramObjectARB", (void **)&next_use_program, (void *)use_program},
        {"glLinkProgram", (void **)&next_link_program, (void *)link_program},
        {"glLinkProgramARB", (void **)&next_link_program, (void *)link_program},
        {"glCompressedTexImage2D", (void **)&next_compressed_tex_image_2d, (void *)compressed_tex_image_2d},
        {"glCompressedTexImage2DARB", (void **)&next_compressed_tex_image_2d, (void *)compressed_tex_image_2d},
        {"glDrawArrays", (void **)&next_draw_arrays, (void *)draw_arrays},
        {"glDrawElements", (void **)&next_draw_elements, (void *)draw_elements},
        {"glDrawRangeElements", (void **)&next_draw_range_elements, (void *)draw_range_elements},
        {"glDrawRangeElementsEXT", (void **)&next_draw_range_elements, (void *)draw_range_elements},
        {"glDrawElementsBaseVertex", (void **)&next_draw_elements_base_vertex, (void *)draw_elements_base_vertex},
        {"glDrawRangeElementsBaseVertex", (void **)&next_draw_range_elements_base_vertex,
         (void *)draw_range_elements_base_vertex},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(*table); ++i) {
        if (strcmp(name, table[i].name)) continue;
        if (!g4_get_uniform_location) {
            g4_get_uniform_location = gl4es_GetProcAddress("glGetUniformLocation");
            g4_uniform1i = gl4es_GetProcAddress("glUniform1i");
            g4_uniform1f = gl4es_GetProcAddress("glUniform1f");
        }
        if (!g4_get_uniform_location || !g4_uniform1i || !g4_uniform1f) return fn;
        *table[i].next = fn;
        return table[i].wrapper;
    }
    return fn;
}
