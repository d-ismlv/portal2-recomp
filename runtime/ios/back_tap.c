// Tapes au dos ou sur les côtés du téléphone tenu en paysage : un index qui
// frappe le dos près du bord gauche tire le portail bleu, près du bord droit
// l'orange.
//
// Repère de l'appareil (CoreMotion) : x le long du petit côté, y le long du
// grand côté (vers le haut en portrait), z sortant de l'écran. En paysage, y
// est horizontal pour le joueur et x vertical.
//
// Détection : un candidat commence quand l'accélération utilisateur dépasse
// accel_min et que sa variation d'un échantillon au suivant (secousse)
// dépasse jerk_min. Il est jugé DELAY plus tard, une fois connus son pic, sa
// durée et les touchers qui l'entourent :
// - durée : temps passé au-dessus de max(accel_min, 35 % du pic) ; la main
//   fait vibrer le téléphone 50 à 100 ms après une tape (calibration) : la
//   limite par défaut est au-delà de la fenêtre de mesure, donc inactive ;
// - agitation : moyenne quadratique de |a| dans les 300 ms qui précèdent, sans
//   les 300 ms qui suivent le candidat d'avant (une tape forte fait vibrer le
//   téléphone jusque-là) ; un pic au milieu d'un mouvement n'est pas une tape ;
//   il en faut au moins 80 ms hors de cette vibration (une secousse enchaîne
//   les candidats tous les 250 ms : il n'en resterait rien) ;
// - téléphone à plat, écran vers le haut (gravité z ≈ -1) : posé sur une
//   table, on ne tape pas son dos ; le poser ou le reprendre secoue ;
// - gravité : sa variation dans les 200 ms qui précèdent (téléphone qu'on
//   tourne) ;
// - toucher à l'écran à moins de touch_guard (une tape sur l'écran secoue
//   aussi le téléphone) ; d'où le délai du jugement, touch_guard + 20 ms pour
//   que l'événement UIKit soit arrivé.
//
// Côté : une poussée F selon z appliquée au point r = (rx, ry, 0) donne un
// couple r × F = (ry·F, -rx·F, 0) : la rotation brève autour de x a le signe
// de ry, la position de la tape le long du grand côté. Le signe est pris sur
// l'écart maximal à la rotation moyenne d'avant le pic. Une tape sur la
// tranche d'un petit côté (poussée selon y) ne fait presque pas tourner :
// alors, si l'accélération est surtout selon y, son signe décide (repli
// « latéral »). CoreMotion rend l'accélération utilisateur avec le signe
// opposé à l'accélération physique (téléphone posé à plat : gravité
// (0, 0, -1)) ; si l'essai sur l'appareil dit le contraire, lateral_sign = -1.
// L'orientation de l'écran (paysage gauche ou droit) dit vers quel bord pointe
// y ; à défaut, le signe de la gravité selon x.
#include "back_tap.h"
#include <math.h>
#include <string.h>

static const double kPre = 0.300, kPreGap = 0.015, kGravityLag = 0.200, kWindow = 0.040, kRinging = 0.300, kMinHistory = 0.080;

// Valeurs par défaut : estimations à calibrer (docs/ios.md). Une tape d'index
// sur un téléphone tenu à deux mains : pic de 0,1 à 0,5 g, rotation de 0,1 à
// 1 rad/s (téléphone seul : ~0,7 rad/s pour un doigt de 10 g à 0,5 m/s, la
// main en absorbe une part). Au repos en jeu : bruit ~0,005 g et 0,01 rad/s,
// pouces sur l'écran quelques centièmes de g, variations lentes.
void rt_backtap_defaults(rt_backtap_params *p) {
    p->accel_min = 0.10f;     // 20 fois le bruit, sous une tape légère
    p->jerk_min = 6.0f;       // 0,06 g en 10 ms : un geste de la main monte 10 fois moins vite
    p->rot_min = 0.08f;       // 8 fois le bruit du gyroscope
    p->lateral_ratio = 0.7f;  // repli latéral seulement si y porte 70 % du pic
    p->lateral_sign = 1;
    p->invert = 0;
    p->z_sign = 0;            // à fixer après calibration : dos et écran poussent en sens opposés
    p->refractory = 0.25f;    // 4 tapes/s au plus ; couvre les rebonds de la main
    p->touch_guard = 0.08f;   // ±80 ms autour d'un début ou d'une fin de toucher
    p->max_width = 0.12f;     // inactive : tapes mesurées de 50 à 100 ms (vibration de la main), fenêtre de 100 ms
    p->flat_z = -0.95f;       // en jeu (calibration) : -0,75 à -0,86 ; posé sur la table : -1,00
    p->pre_rms_max = 0.05f;   // téléphone tenu immobile : ~0,01 g ; marche : 0,1 g et plus
    p->gravity_max = 0.12f;   // ~7° de rotation en 200 ms
    p->log_factor = 1.0f;
}

void rt_backtap_init(rt_backtap *d, const rt_backtap_params *p) {
    memset(d, 0, sizeof(*d));
    d->p = *p;
    d->refractory_until = -1e9;
    d->last_candidate = -1e9;
    d->last_inactive = -1e9;
}

void rt_backtap_touch(rt_backtap *d, double t) {
    d->touches[d->ntouch++ % RT_BACKTAP_TOUCHES] = t;
}

static float norm3(const float *v) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
static double delay_of(const rt_backtap_params *p) { return fmax(0.100, p->touch_guard + 0.020); }
static const rt_backtap_sample *at(const rt_backtap *d, unsigned long i) { return &d->ring[i % RT_BACKTAP_RING]; }

static void judge(rt_backtap *d, int orient, int active, rt_backtap_report *r) {
    const rt_backtap_params *p = &d->p;
    double t0 = d->onset, delay = delay_of(p);
    unsigned long first = d->count > RT_BACKTAP_RING ? d->count - RT_BACKTAP_RING : 0, end = d->count;
    memset(r, 0, sizeof(*r));
    r->t = t0; r->touch_dt = 1e9;

    // Avant le pic : agitation, rotation moyenne, gravité (dernier échantillon
    // avant t0 - 15 ms, et 200 ms plus tôt).
    double base[3] = {0, 0, 0}, sq = 0, span = 0; int n = 0;
    const rt_backtap_sample *g_now = NULL, *g_then = NULL;
    unsigned long onset = end;   // indice du premier échantillon du pic
    for (unsigned long i = first; i < end; ++i) {
        const rt_backtap_sample *s = at(d, i);
        if (s->t >= t0) { onset = i; break; }
        if (s->t > t0 - kPreGap) continue;
        g_now = s;
        if (!g_then || s->t <= t0 - kPreGap - kGravityLag) g_then = s;
        if (s->t < t0 - kPre) continue;
        if (s->t >= d->last_candidate && s->t < d->last_candidate + kRinging) continue;   // rebonds de la tape d'avant
        const rt_backtap_sample *q = i > first ? at(d, i - 1) : NULL;
        if (q) span += fmin(s->t - q->t, 0.02);
        float m = norm3(s->a);
        sq += (double)m * m;
        for (int k = 0; k < 3; ++k) base[k] += s->w[k];
        ++n;
    }
    if (n) for (int k = 0; k < 3; ++k) base[k] /= n;
    r->pre_rms = n ? (float)sqrt(sq / n) : 0;
    if (g_now && g_then) {
        float dg[3] = {g_now->g[0] - g_then->g[0], g_now->g[1] - g_then->g[1], g_now->g[2] - g_then->g[2]};
        r->gravity = norm3(dg);
    }
    if (g_now) memcpy(r->grav, g_now->g, sizeof(r->grav));

    // Pic (40 ms) : amplitude, secousse, rotation ; la rotation peut précéder
    // l'accélération d'un échantillon.
    float onset_az = onset < end ? at(d, onset)->a[2] : 0;
    for (unsigned long i = onset > first ? onset - 1 : onset; i < end; ++i) {
        const rt_backtap_sample *s = at(d, i);
        if (s->t > t0 + kWindow) break;
        for (int k = 0; k < 3; ++k) {
            float v = s->w[k] - (float)base[k];
            if (fabsf(v) > fabsf(r->rot[k])) r->rot[k] = v;
        }
        if (i < onset) continue;
        float m = norm3(s->a);
        if (m > r->peak) { r->peak = m; memcpy(r->acc, s->a, sizeof(r->acc)); r->rise = (float)(s->t - t0); }
        const rt_backtap_sample *q = i > first ? at(d, i - 1) : NULL;
        if (q && s->t > q->t) {
            float da[3] = {s->a[0] - q->a[0], s->a[1] - q->a[1], s->a[2] - q->a[2]};
            float j = norm3(da) / (float)(s->t - q->t);
            if (j > r->jerk) r->jerk = j;
        }
    }
    // Durée : au-dessus de max(accel_min, 35 % du pic), jusqu'au jugement.
    float settle = fmaxf(p->accel_min, 0.35f * r->peak);
    double above_first = -1, above_last = -1;
    for (unsigned long i = onset; i < end; ++i) {
        const rt_backtap_sample *s = at(d, i);
        if (s->t > t0 + delay) break;
        if (norm3(s->a) >= settle) { if (above_first < 0) above_first = s->t; above_last = s->t; }
    }
    r->width = above_first < 0 ? 0 : (float)(above_last - above_first);

    for (unsigned i = 0; i < RT_BACKTAP_TOUCHES && i < d->ntouch; ++i) {
        double dt = d->touches[i] - t0;
        if (fabs(dt) < fabs(r->touch_dt)) r->touch_dt = dt;
    }

    // Orientation : écran d'abord, gravité à défaut.
    r->orient = orient;
    if (!r->orient && g_now && fabsf(g_now->g[0]) >= 0.3f) r->orient = g_now->g[0] > 0 ? 1 : -1;

    // Côté, dans le repère de l'appareil (+1 : vers +y), puis de l'écran.
    if (fabsf(r->rot[0]) >= p->rot_min) {
        r->side = r->rot[0] > 0 ? 1 : -1; r->method = "rotation";
    } else if (p->lateral_sign && r->peak > 0 && fabsf(r->acc[1]) >= p->lateral_ratio * r->peak) {
        // Poussée physique vers +y (a_y CoreMotion < 0) : tape venue du côté -y.
        r->side = (r->acc[1] > 0 ? 1 : -1) * p->lateral_sign; r->method = "latéral";
    }
    r->side *= r->orient * (p->invert ? -1 : 1);

    r->weak = r->peak < p->accel_min || r->jerk < p->jerk_min;
    if (r->weak) r->reject = "sous les seuils";
    // En menu jusqu'à touch_guard avant le pic : le clic qui reprend la partie
    // (toucher passé à SDL, inconnu ici) secoue aussi le téléphone.
    else if (!active || d->last_inactive >= t0 - p->touch_guard) r->reject = "menu";
    else if (fabs(r->touch_dt) <= p->touch_guard) r->reject = "toucher à l'écran";
    else if (n < 3 || span < kMinHistory) r->reject = "historique insuffisant";
    else if (r->pre_rms > p->pre_rms_max) r->reject = "agitation avant le pic";
    else if (r->gravity > p->gravity_max) r->reject = "gravité qui change";
    else if (g_now && g_now->g[2] < p->flat_z) r->reject = "téléphone à plat";
    else if (r->width > p->max_width) r->reject = "pic trop long";
    else if (p->z_sign && (onset_az > 0 ? 1 : -1) != p->z_sign) r->reject = "sens z";
    else if (!r->orient) r->reject = "orientation inconnue";
    else if (!r->side) r->reject = "côté indécis";
    if (r->reject) r->side = 0;
}

int rt_backtap_feed(rt_backtap *d, const rt_backtap_sample *s, int orient, int active, rt_backtap_report *r) {
    d->ring[d->count++ % RT_BACKTAP_RING] = *s;
    if (!active) d->last_inactive = s->t;
    if (d->count < 2) return 0;
    if (!d->onset) {
        const rt_backtap_sample *prev = at(d, d->count - 2);
        double dt = s->t - prev->t;
        if (s->t < d->refractory_until || dt <= 0 || dt > 0.1) return 0;
        float da[3] = {s->a[0] - prev->a[0], s->a[1] - prev->a[1], s->a[2] - prev->a[2]};
        float f = d->p.log_factor;
        if (norm3(s->a) >= d->p.accel_min * f && norm3(da) / dt >= d->p.jerk_min * f) d->onset = s->t;
        return 0;
    }
    if (s->t < d->onset + delay_of(&d->p)) return 0;
    judge(d, orient, active, r);
    if (!r->weak) { d->refractory_until = d->onset + d->p.refractory; d->last_candidate = d->onset; }
    d->onset = 0;
    return 1;
}
