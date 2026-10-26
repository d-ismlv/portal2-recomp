// Compteur d'images côté hôte, activé/désactivé par F3 (fps_overlay_toggle,
// appelé par sdl_input.c). Dessiné juste avant l'échange d'images dans le
// framebuffer par défaut, avec sauvegarde et restauration de l'état GL : le
// rendu du jeu n'est pas modifié. P2_FPS=1 l'active dès le lancement.
#include "rt.h"
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <mach/mach_time.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

static atomic_int enabled = -1;
static uint64_t last_ticks, frames;
static double shown_fps;

void fps_overlay_toggle(void) {
    if (atomic_load(&enabled) < 0) atomic_store(&enabled, 0);
    atomic_store(&enabled, !atomic_load(&enabled));
}

// Police 5x7 : chiffres, point, espace, « f », « p », « s ».
static const char glyph_chars[] = "0123456789. fps";
static const uint8_t glyphs[][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}, {0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x06,0x09,0x08,0x1C,0x08,0x08,0x08}, {0x00,0x00,0x1E,0x11,0x1E,0x10,0x10},
    {0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E},
};

// Glyphe agrandi d'un facteur `scale` en bitmap GL (lignes de bas en haut).
static void draw_glyph(int index, int x, int y, int scale) {
    enum { MAX_SCALE = 6 };
    uint8_t bits[7 * MAX_SCALE][(5 * MAX_SCALE + 7) / 8];
    if (scale > MAX_SCALE) scale = MAX_SCALE;
    int width = 5 * scale, height = 7 * scale, stride = (width + 7) / 8;
    memset(bits, 0, sizeof(bits));
    for (int row = 0; row < height; ++row) {
        uint8_t source = glyphs[index][6 - row / scale];
        for (int col = 0; col < width; ++col)
            if (source & (0x10 >> (col / scale))) bits[row][col / 8] |= (uint8_t)(0x80 >> (col % 8));
    }
    GLubyte packed[7 * MAX_SCALE * ((5 * MAX_SCALE + 7) / 8)];
    for (int row = 0; row < height; ++row) memcpy(packed + row * stride, bits[row], (size_t)stride);
    glWindowPos2i(x, y);
    glBitmap(width, height, 0, 0, 0, 0, packed);
}

static void draw_text(const char *text, int x, int y, int scale) {
    for (const char *p = text; *p; ++p, x += 6 * scale) {
        const char *at = strchr(glyph_chars, *p);
        if (at) draw_glyph((int)(at - glyph_chars), x, y, scale);
    }
}

void fps_overlay_frame(int drawable_width, int drawable_height) {
    if (atomic_load(&enabled) < 0) {
        const char *e = getenv("P2_FPS");
        atomic_store(&enabled, e && *e && strcmp(e, "0"));
    }
    static mach_timebase_info_data_t base;
    if (!base.denom) mach_timebase_info(&base);
    uint64_t now = mach_absolute_time();
    ++frames;
    if (!last_ticks) last_ticks = now;
    double elapsed = (double)(now - last_ticks) * base.numer / base.denom / 1e9;
    if (elapsed >= 0.5) { shown_fps = (double)frames / elapsed; frames = 0; last_ticks = now; }
    if (!atomic_load(&enabled) || drawable_width <= 0 || drawable_height <= 0) return;

    char text[32];
    snprintf(text, sizeof(text), "%.0f fps", shown_fps);
    int scale = drawable_height >= 1200 ? 4 : 3;
    int x = 12 * scale / 3, y = drawable_height - 9 * scale - 12;

    GLint draw_framebuffer = 0, program = 0, unpack_buffer = 0, unpack_alignment = 4;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING_EXT, &draw_framebuffer);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_alignment);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
    glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, 0);
    glUseProgram(0);
    if (unpack_buffer) glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_ALPHA_TEST); glDisable(GL_TEXTURE_2D);
    glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, drawable_width, drawable_height);
    glColor3f(0.0f, 0.0f, 0.0f); draw_text(text, x + scale, y - scale, scale);   // ombre
    glColor3f(1.0f, 0.85f, 0.1f); draw_text(text, x, y, scale);
    glPopClientAttrib();
    glPopAttrib();
    if (unpack_buffer) glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)unpack_buffer);
    glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_alignment);
    glUseProgram((GLuint)program);
    glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, (GLuint)draw_framebuffer);
}
