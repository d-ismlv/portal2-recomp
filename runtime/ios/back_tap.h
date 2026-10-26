// Détection des tapes au dos ou sur les côtés du téléphone (accéléromètre et
// gyroscope), en C portable : runtime/ios/back_tap.c, testé sur l'hôte par
// tests/checks_back_tap.c, nourri par CoreMotion dans touch_controls.m.
#pragma once

// Seuils (unités CoreMotion : g, rad/s, secondes). rt_backtap_defaults donne
// les valeurs par défaut, expliquées dans back_tap.c.
typedef struct {
    float accel_min;     // g : accélération utilisateur minimale au pic
    float jerk_min;      // g/s : secousse minimale (variation d'un échantillon au suivant)
    float rot_min;       // rad/s : rotation minimale autour de l'axe x pour décider le côté
    float lateral_ratio; // tape latérale : part minimale de l'axe y dans le pic d'accélération
    int lateral_sign;    // tape latérale : 1 convention supposée, -1 inversée, 0 désactivée
    int invert;          // 1 : gauche et droite permutées
    int z_sign;          // 0 : indifférent ; ±1 : signe exigé de a_z au départ du pic
    float refractory;    // s : rien après un candidat pendant ce délai
    float touch_guard;   // s : rien à moins de ce délai d'un début ou d'une fin de toucher
    float max_width;     // s : durée maximale du pic (une secousse dure plus longtemps)
    float max_rise;      // s : délai maximal entre le début du pic et son maximum (un geste monte lentement)
    float pre_rms_max;   // g : agitation maximale (moyenne quadratique) avant le pic
    float gravity_max;   // g : variation maximale de la gravité dans les 200 ms avant le pic
    float log_factor;    // seuils de candidature multipliés par ce facteur (journal : < 1)
} rt_backtap_params;

typedef struct {
    double t;     // s, horloge de CMLogItem.timestamp (= UITouch.timestamp)
    float a[3];   // accélération utilisateur (g, gravité retirée), repère de l'appareil
    float w[3];   // vitesse de rotation (rad/s)
    float g[3];   // gravité (g)
} rt_backtap_sample;

// Bilan d'un candidat, accepté ou rejeté (journal de calibration).
typedef struct {
    double t;              // début du pic
    float peak;            // g : |a| maximal
    float jerk;            // g/s : secousse maximale
    float acc[3];          // g : vecteur a au pic
    float rot[3];          // rad/s : écart de rotation maximal (signé) par axe
    float width;           // s : durée du pic
    float rise;            // s : du début du pic à son maximum
    float grav[3];         // g : gravité juste avant le pic (téléphone à plat : z ≈ ±1)
    float pre_rms;         // g : agitation avant le pic
    float gravity;         // g : variation de la gravité avant le pic
    double touch_dt;       // s : toucher le plus proche (t_toucher - t), 1e9 si aucun
    int orient;            // +1 : axe y de l'appareil vers la droite de l'écran, -1 : vers la gauche
    int side;              // -1 gauche (portail bleu), +1 droite (orange), 0 aucun
    const char *method;    // "rotation", "latéral" ou NULL
    const char *reject;    // raison du rejet, NULL si accepté
    int weak;              // sous les seuils (candidat de journal seulement)
} rt_backtap_report;

enum { RT_BACKTAP_RING = 256, RT_BACKTAP_TOUCHES = 8 };

typedef struct {
    rt_backtap_params p;
    rt_backtap_sample ring[RT_BACKTAP_RING];
    unsigned long count;                  // échantillons reçus
    double touches[RT_BACKTAP_TOUCHES];   // derniers débuts et fins de toucher
    unsigned ntouch;
    double onset;                         // candidat en cours (0 : aucun)
    double refractory_until;
    double last_candidate;                // ses échantillons sont exclus de l'agitation suivante
    double last_inactive;                 // dernier échantillon en menu (ses touchers échappent à la surcouche)
} rt_backtap;

void rt_backtap_defaults(rt_backtap_params *p);
void rt_backtap_init(rt_backtap *d, const rt_backtap_params *p);
// Début ou fin d'un toucher à l'écran, même horloge que les échantillons.
void rt_backtap_touch(rt_backtap *d, double t);
// Un échantillon. orient : +1/-1 (voir rt_backtap_report.orient) ou 0 pour le
// déduire de la gravité ; active : 0 en menu ou manette masquée. Rend 1 quand
// un candidat vient d'être jugé (bilan dans *r ; tape si r->reject est NULL).
int rt_backtap_feed(rt_backtap *d, const rt_backtap_sample *s, int orient, int active, rt_backtap_report *r);
