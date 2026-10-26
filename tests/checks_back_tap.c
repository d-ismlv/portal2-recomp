// Tapes au dos du port iOS (runtime/ios/back_tap.c, C portable, testé ici
// sur l'hôte) : échantillons CoreMotion synthétiques, à 100 et 200 Hz.
#include "checks.h"
#include <math.h>
#include <string.h>
#include "../runtime/ios/back_tap.c"

// Une tape : poussée selon z (dos : vers l'utilisateur ; écran : à l'opposé)
// ou selon y (tranche), à la position y de signe ry dans le repère de
// l'appareil. CoreMotion rend l'opposé de l'accélération physique.
typedef struct { double t; int ry; int axis; float accel, rot; } Tap;
typedef struct {
    double rate, length;
    Tap taps[4]; int ntaps;
    double touches[4]; int ntouch;
    float gravity[3];
    double shake_from, shake_to;      // secousse de la main : 4 Hz, 1 g
    double turn_from;                 // téléphone qu'on tourne à 1 rad/s autour de y
    float wobble;                     // geste ample et lent : 2 Hz selon z (g)
    float noise;                      // amplitude du bruit d'accélération (g)
    int orient, active;
    double active_from;               // en menu jusque-là (0 : toujours la valeur active)
    rt_backtap_params params;
} Scene;
typedef struct { int accepted, reports, side[8]; const char *method[8], *reject[8]; int weak[8]; } Outcome;

static float half_sine(double x, double width) { return x < 0 || x > width ? 0 : (float)sin(M_PI * x / width); }

static Outcome play(const Scene *sc) {
    rt_backtap d; rt_backtap_init(&d, &sc->params);
    Outcome o; memset(&o, 0, sizeof(o));
    unsigned seed = 12345;
    int touched = 0;
    for (long i = 0; i < (long)(sc->length * sc->rate); ++i) {
        double t = 100 + i / sc->rate;   // horloge de l'appareil : secondes depuis le démarrage
        rt_backtap_sample s; memset(&s, 0, sizeof(s));
        s.t = t;
        for (int k = 0; k < 3; ++k) {
            seed = seed * 1103515245u + 12345u;
            s.a[k] = sc->noise * ((float)(seed >> 8 & 0xffff) / 32768.0f - 1);
            seed = seed * 1103515245u + 12345u;
            s.w[k] = 0.01f * ((float)(seed >> 8 & 0xffff) / 32768.0f - 1);
            s.g[k] = sc->gravity[k];
        }
        for (int j = 0; j < sc->ntaps; ++j) {
            const Tap *p = &sc->taps[j];
            double x = t - 100 - p->t;
            float push = p->accel * half_sine(x, 0.020) - 0.3f * p->accel * half_sine(x - 0.020, 0.020);
            s.a[p->axis] -= push;
            if (x >= 0) s.w[0] += p->ry * p->rot * (float)(x < 0.005 ? x / 0.005 : exp(-(x - 0.005) / 0.030));
        }
        double u = t - 100;
        if (u >= sc->shake_from && u < sc->shake_to) {
            s.a[1] += (float)sin(2 * M_PI * 4 * (u - sc->shake_from));
            s.w[2] += 2 * (float)cos(2 * M_PI * 4 * (u - sc->shake_from));
        }
        s.a[2] += sc->wobble * (float)sin(2 * M_PI * 2 * u);
        if (sc->turn_from > 0 && u >= sc->turn_from) {
            double a = u - sc->turn_from;
            s.g[0] = sc->gravity[0] * (float)cos(a) + sc->gravity[2] * (float)sin(a);
            s.g[2] = -sc->gravity[0] * (float)sin(a) + sc->gravity[2] * (float)cos(a);
            s.w[1] += 1;
        }
        // Toucher : livré par UIKit ~20 ms après son horodatage.
        while (touched < sc->ntouch && 100 + sc->touches[touched] + 0.020 <= t) rt_backtap_touch(&d, 100 + sc->touches[touched++]);
        rt_backtap_report r;
        int active = sc->active && u >= sc->active_from;
        if (rt_backtap_feed(&d, &s, sc->orient, active, &r)) {
            if (o.reports < 8) {
                o.side[o.reports] = r.side; o.method[o.reports] = r.method;
                o.reject[o.reports] = r.reject; o.weak[o.reports] = r.weak;
            }
            ++o.reports;
            if (!r.reject) ++o.accepted;
        }
    }
    return o;
}

static Scene scene(double rate) {
    Scene sc; memset(&sc, 0, sizeof(sc));
    sc.rate = rate; sc.length = 2.5; sc.noise = 0.005f; sc.orient = 1; sc.active = 1;
    sc.gravity[0] = -0.8f; sc.gravity[2] = -0.6f;   // paysage, incliné en arrière
    sc.shake_from = sc.shake_to = -1;
    rt_backtap_defaults(&sc.params);
    return sc;
}
// Tape au dos du côté gauche (-1) ou droit (+1) de l'écran ; orient : +1 si
// l'axe y de l'appareil pointe vers la droite de l'écran.
static void back_tap(Scene *sc, double t, int screen_side) {
    sc->taps[sc->ntaps++] = (Tap){t, screen_side * sc->orient, 2, 0.3f, 0.4f};
}
static const double kRates[] = {100, 200};

RT_CHECK(backtap_left_and_right_back_taps_pick_their_side) {
    for (int r = 0; r < 2; ++r)
        for (int orient = -1; orient <= 1; orient += 2)
            for (int side = -1; side <= 1; side += 2) {
                Scene sc = scene(kRates[r]); sc.orient = orient;
                back_tap(&sc, 1.0, side);
                Outcome o = play(&sc);
                assert(o.reports == 1 && o.accepted == 1);
                assert(o.side[0] == side && !strcmp(o.method[0], "rotation"));
            }
}

// Orientation inconnue : déduite du signe de la gravité selon x.
RT_CHECK(backtap_orientation_from_gravity) {
    Scene sc = scene(100);
    back_tap(&sc, 1.0, -1);   // à gauche avec l'axe y vers la droite (orient = 1)
    sc.gravity[0] = 0.8f;     // axe y vers la droite de l'écran <=> gravité selon +x
    sc.orient = 0;
    Outcome o = play(&sc);
    assert(o.accepted == 1 && o.side[0] == -1);
    sc.gravity[0] = -0.8f;    // l'autre paysage : mêmes données, côté opposé
    o = play(&sc);
    assert(o.accepted == 1 && o.side[0] == 1);
    sc.gravity[0] = 0.1f;     // presque à plat : indécidable
    o = play(&sc);
    assert(o.accepted == 0 && !strcmp(o.reject[0], "orientation inconnue"));
}

RT_CHECK(backtap_side_tap_uses_lateral_acceleration) {
    for (int r = 0; r < 2; ++r) {
        // Tape sur la tranche gauche : poussée physique vers la droite (+y si orient = 1).
        Scene sc = scene(kRates[r]);
        sc.taps[sc.ntaps++] = (Tap){1.0, 0, 1, 0.3f, 0};
        Outcome o = play(&sc);
        assert(o.accepted == 1 && o.side[0] == -1 && !strcmp(o.method[0], "latéral"));
        sc.params.lateral_sign = 0;
        o = play(&sc);
        assert(o.accepted == 0 && !strcmp(o.reject[0], "côté indécis"));
    }
}

RT_CHECK(backtap_shake_is_not_a_tap) {
    for (int r = 0; r < 2; ++r) {
        Scene sc = scene(kRates[r]);
        sc.shake_from = 0.8; sc.shake_to = 1.8;
        Outcome o = play(&sc);
        assert(o.reports > 0 && o.accepted == 0);
        sc.shake_from = 0.4;   // tape au milieu de la secousse
        back_tap(&sc, 1.0, 1);
        o = play(&sc);
        assert(o.accepted == 0);
    }
}

RT_CHECK(backtap_screen_touch_rejects_nearby_peak) {
    const double offsets[] = {-0.06, 0.0, 0.01, 0.07};
    for (int r = 0; r < 2; ++r)
        for (int i = 0; i < 4; ++i) {
            Scene sc = scene(kRates[r]);
            sc.taps[sc.ntaps++] = (Tap){1.0, 1, 2, -0.3f, 0.4f};   // écran : poussée vers l'arrière
            sc.touches[sc.ntouch++] = 1.0 + offsets[i];
            Outcome o = play(&sc);
            assert(o.reports == 1 && o.accepted == 0 && !strcmp(o.reject[0], "toucher à l'écran"));
        }
    // Toucher lointain (doigt posé sur un stick bien avant) : tape acceptée.
    Scene sc = scene(100);
    back_tap(&sc, 1.0, 1);
    sc.touches[sc.ntouch++] = 0.5; sc.touches[sc.ntouch++] = 1.2;
    Outcome o = play(&sc);
    assert(o.accepted == 1 && o.side[0] == 1);
}

// Sens z exigé (après calibration) : le dos et l'écran poussent en sens opposés.
RT_CHECK(backtap_z_sign_separates_back_from_screen) {
    Scene sc = scene(200);
    sc.params.z_sign = -1;   // dos : poussée vers l'utilisateur, a_z CoreMotion < 0
    back_tap(&sc, 1.0, -1);
    Outcome o = play(&sc);
    assert(o.accepted == 1);
    sc.taps[0].accel = -0.3f;
    o = play(&sc);
    assert(o.accepted == 0 && !strcmp(o.reject[0], "sens z"));
}

RT_CHECK(backtap_nothing_in_menus) {
    Scene sc = scene(100);
    sc.active = 0;
    back_tap(&sc, 1.0, 1);
    Outcome o = play(&sc);
    assert(o.accepted == 0 && !strcmp(o.reject[0], "menu"));
    // Clic qui reprend la partie : pic en menu, jugé une fois en partie.
    const double resume[] = {1.03, 0.95};
    for (int i = 0; i < 2; ++i) {
        sc.active = 1; sc.active_from = resume[i];
        o = play(&sc);
        assert(o.reports == 1 && o.accepted == 0 && !strcmp(o.reject[0], "menu"));
    }
    sc.active_from = 0.85;   // partie reprise bien avant la tape
    o = play(&sc);
    assert(o.accepted == 1);
}

RT_CHECK(backtap_refractory_period) {
    for (int r = 0; r < 2; ++r) {
        Scene sc = scene(kRates[r]);
        back_tap(&sc, 1.0, -1); back_tap(&sc, 1.15, 1);   // rebond ou deuxième tape trop proche
        Outcome o = play(&sc);
        assert(o.reports == 1 && o.accepted == 1 && o.side[0] == -1);
        sc.taps[1].t = 1.4;
        o = play(&sc);
        assert(o.accepted == 2 && o.side[0] == -1 && o.side[1] == 1);
        // Période courte : la tape d'avant ne compte pas dans l'agitation.
        sc.params.refractory = 0.1f; sc.taps[1].t = 1.15;
        o = play(&sc);
        assert(o.accepted == 2 && o.side[0] == -1 && o.side[1] == 1);
    }
}

RT_CHECK(backtap_motion_rejects) {
    Scene sc = scene(100);
    sc.turn_from = 0.6;   // téléphone qu'on tourne
    back_tap(&sc, 1.0, 1);
    Outcome o = play(&sc);
    assert(o.accepted == 0 && !strcmp(o.reject[0], "gravité qui change"));
    for (int r = 0; r < 2; ++r) {
        sc = scene(kRates[r]);
        sc.wobble = 0.12f;    // geste lent : pas de candidat à lui seul, mais la tape est rejetée
        o = play(&sc);
        assert(o.reports == 0);
        back_tap(&sc, 1.0, 1);
        o = play(&sc);
        assert(o.reports == 1 && o.accepted == 0 && !strcmp(o.reject[0], "agitation avant le pic"));
    }
    sc = scene(100);
    sc.noise = 0.12f;     // marche : agitation continue
    back_tap(&sc, 1.0, 1);
    o = play(&sc);
    assert(o.accepted == 0);
    for (int i = 0; i < o.reports && i < 8; ++i) assert(o.reject[i]);
}

// Candidats sous les seuils : seulement avec log_factor < 1 (journal), sans
// période réfractaire.
RT_CHECK(backtap_weak_candidates_only_logged) {
    Scene sc = scene(200);
    sc.taps[sc.ntaps++] = (Tap){1.0, 1, 2, 0.07f, 0.4f};
    Outcome o = play(&sc);
    assert(o.reports == 0);
    sc.params.log_factor = 0.5f;
    back_tap(&sc, 1.12, -1);
    o = play(&sc);
    assert(o.reports == 2 && o.weak[0] && !strcmp(o.reject[0], "sous les seuils"));
    assert(o.accepted == 1 && o.side[1] == -1);
}
