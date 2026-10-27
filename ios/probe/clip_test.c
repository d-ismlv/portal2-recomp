// Sonde : interpolation des varyings d'OpenGL ES 3 (sans gl4es) sur un grand
// triangle qui passe derrière la caméra, découpé au plan proche. Sol y = -1,
// coordonnées de texture = position dans le plan ; chaque pixel lu est comparé
// à l'intersection rayon-plan calculée ici. Écart attendu : < 3/255.
#include <OpenGLES/ES3/gl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static GLuint compile(GLenum type, const char *source) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &source, NULL);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(s, sizeof(log), NULL, log); printf("clip: compile %s\n", log); }
    return s;
}

// Écart maximal (sur 255) entre l'image et la valeur attendue ; near_z :
// bord proche du sol (positif = derrière la caméra).
static int run(int w, int h, float near_z, const char *label) {
    static const char *vs = "#version 300 es\nin vec3 p; uniform mat4 m; out vec2 uv;\n"
                            "void main() { uv = vec2((p.x + 10.0) / 20.0, (10.0 - p.z) / 110.0); gl_Position = m * vec4(p, 1.0); }\n";
    static const char *fs = "#version 300 es\nprecision highp float; in vec2 uv; out vec4 c;\n"
                            "void main() { c = vec4(uv, 0.0, 1.0); }\n";
    GLuint program = glCreateProgram();
    glAttachShader(program, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(program, compile(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(program, 0, "p");
    glLinkProgram(program);
    glUseProgram(program);
    float aspect = (float)w / (float)h, n = 0.1f, f = 1000.0f;
    float m[16] = {1.0f / aspect, 0, 0, 0, 0, 1, 0, 0, 0, 0, (f + n) / (n - f), -1, 0, 0, 2 * f * n / (n - f), 0};
    glUniformMatrix4fv(glGetUniformLocation(program, "m"), 1, GL_FALSE, m);
    float quad[] = {-10, -1, near_z, 10, -1, near_z, 10, -1, -100, -10, -1, near_z, 10, -1, -100, -10, -1, -100};
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, quad);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    int worst = 0, bad = 0, total = 0;
    for (int y = 1; y < h / 2 - 1; y += h / 40 + 1)
        for (int x = 1; x < w - 1; x += w / 40 + 1) {
            float dx = (2.0f * (x + 0.5f) / w - 1.0f) * aspect, dy = 2.0f * (y + 0.5f) / h - 1.0f;
            float t = -1.0f / dy, px = t * dx, pz = -t;
            if (px < -10 || px > 10 || pz < -100) continue;
            unsigned char pixel[4];
            glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
            int eu = (int)lroundf((px + 10.0f) / 20.0f * 255.0f), ev = (int)lroundf((10.0f - pz) / 110.0f * 255.0f);
            int d = abs(pixel[0] - eu) > abs(pixel[1] - ev) ? abs(pixel[0] - eu) : abs(pixel[1] - ev);
            if (d > worst) worst = d;
            bad += d > 3; ++total;
        }
    printf("clip: %s : écart max %d/255, %d/%d pixels faux\n", label, worst, bad, total);
    glDisableVertexAttribArray(0);
    glDeleteProgram(program);
    return worst;
}

void clip_test(int w, int h) {
    run(w, h, -0.5f, "sol devant la caméra");
    run(w, h, 10.0f, "sol passant derrière la caméra");
}
