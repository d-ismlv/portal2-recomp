// Rejoue un enregistrement brut des tapes au dos (P2_BACKTAP_RAW sur l'iPhone)
// dans le détecteur de runtime/ios/back_tap.c, pour régler les seuils sur de
// vraies données. Mêmes variables d'environnement que le jeu (P2_BACKTAP_ACCEL,
// P2_BACKTAP_AGITATION, ...) ; P2_BACKTAP_LOG=1 montre aussi les candidats faibles.
//
//     cc -O2 -o build/backtap-replay tools/backtap_replay.c -lm
//     build/backtap-replay backtap.raw
#include "../runtime/ios/back_tap.c"
#include <stdio.h>
#include <stdlib.h>

static float env(const char *name, float def) { const char *e = getenv(name); return e && *e ? (float)atof(e) : def; }
static int env_sign(const char *name, int def) { const char *e = getenv(name); return !e || !*e ? def : atoi(e) > 0 ? 1 : atoi(e) < 0 ? -1 : 0; }

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage : %s <enregistrement>\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 1; }
    rt_backtap_params p;
    rt_backtap_defaults(&p);
    p.accel_min = env("P2_BACKTAP_ACCEL", p.accel_min);
    p.jerk_min = env("P2_BACKTAP_JERK", p.jerk_min);
    p.rot_min = env("P2_BACKTAP_ROT", p.rot_min);
    p.gyro_accel_min = env("P2_BACKTAP_GYRO_ACCEL", p.gyro_accel_min);
    p.gyro_rot_min = env("P2_BACKTAP_GYRO_ROT", p.gyro_rot_min);
    p.gyro_jerk_min = env("P2_BACKTAP_GYRO_JERK", p.gyro_jerk_min);
    p.gyro_calm_max = env("P2_BACKTAP_GYRO_CALM", p.gyro_calm_max);
    p.refractory = env("P2_BACKTAP_REFRACTORY", p.refractory);
    p.touch_guard = env("P2_BACKTAP_TOUCH_GUARD", p.touch_guard);
    p.max_width = env("P2_BACKTAP_WIDTH", p.max_width);
    p.pre_rms_max = env("P2_BACKTAP_AGITATION", p.pre_rms_max);
    p.gravity_max = env("P2_BACKTAP_GRAVITY", p.gravity_max);
    p.flat_z = env("P2_BACKTAP_FLAT", p.flat_z);
    p.lateral_sign = env_sign("P2_BACKTAP_LATERAL", p.lateral_sign);
    p.z_sign = env_sign("P2_BACKTAP_ZSIGN", p.z_sign);
    p.invert = env_sign("P2_BACKTAP_INVERT", 0) > 0;
    int log = getenv("P2_BACKTAP_LOG") && *getenv("P2_BACKTAP_LOG") != '0';
    static rt_backtap d;
    rt_backtap_init(&d, &p);
    char line[512];
    double origin = 0;
    int taps[2] = {0, 0}, rejects = 0;
    while (fgets(line, sizeof(line), f)) {
        rt_backtap_sample s; int orient, active; double t;
        if (line[0] == 't' && sscanf(line, "t %lf", &t) == 1) { rt_backtap_touch(&d, t); continue; }
        if (line[0] != 'm' || sscanf(line, "m %lf %f %f %f %f %f %f %f %f %f %d %d", &s.t, &s.a[0], &s.a[1], &s.a[2], &s.w[0],
                                     &s.w[1], &s.w[2], &s.g[0], &s.g[1], &s.g[2], &orient, &active) != 12) continue;
        if (!origin) origin = s.t;
        rt_backtap_report r;
        if (!rt_backtap_feed(&d, &s, orient, active, &r) || (r.weak && !log)) continue;
        if (!r.reject) ++taps[r.side > 0];
        else ++rejects;
        char touch[16]; if (fabs(r.touch_dt) < 10) snprintf(touch, sizeof(touch), "%+.0fms", r.touch_dt * 1000); else snprintf(touch, sizeof(touch), "aucun");
        printf("%8.2f s  pic %.2f g  secousse %3.0f  rot x %+.2f  montée %2.0f ms  avant %.3f  fin %.3f  grav z %+.2f  toucher %s  -> %s\n",
               r.t - origin, r.peak, r.jerk, r.rot[0], r.rise * 1000, r.pre_rms, r.tail_rms, r.grav[2],
               touch,
               r.reject ? r.reject : r.side < 0 ? (r.gyro ? "GAUCHE (gyro)" : "GAUCHE") : (r.gyro ? "DROITE (gyro)" : "DROITE"));
    }
    printf("%d gauche, %d droite, %d rejets\n", taps[0], taps[1], rejects);
    return 0;
}
