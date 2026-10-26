// Commandes tactiles de Portal 2 sur iPhone/iPad.
//
// Une surcouche UIKit posée sur la fenêtre de SDL, toujours affichée (le moteur
// ne signale pas quand il est en partie : il ne capture jamais la souris sur
// iOS). Elle traduit les doigts en ce que le jeu comprend par défaut, clavier
// et souris :
// - stick gauche : déplacement, touches W/A/S/D (8 directions, zone morte) ;
// - stick droit : caméra, mouvements de souris relatifs à vitesse
//   proportionnelle à l'inclinaison (courbe quadratique pour viser finement) ;
//   chaque stick se centre là où le doigt se pose, dans sa moitié d'écran ;
// - portails : une paire bleu (clic gauche) / orange (clic droit) dans chaque
//   coin du haut, pour la main libre ;
// - icônes : sauter (espace), s'accroupir (Ctrl gauche), utiliser (E),
//   pause (Échap) ;
// - toucher bref sans glissement hors des boutons : clic gauche à cet endroit
//   (menus). En jeu, il tire donc aussi un portail bleu.
// Avec une manette ou un clavier connecté, elle se cache jusqu'au premier
// toucher. P2_TOUCH_CONTROLS=0 la désactive, =1 l'affiche toujours ; P2_TOUCH_LOOK règle la vitesse
// de la caméra (1 par défaut).
//
// Disposition et dessin inspirés de la manette tactile de TOTK de
// MacNativeGames (games/totk/ios/Sources/touch_pad.m, licence MIT).
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#import <GameController/GameController.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>
#include <SDL.h>
#include <SDL_syswm.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

typedef enum { ROLE_MOVE, ROLE_LOOK, ROLE_BUTTON } Role;
typedef enum { KEY_SPACE, KEY_CTRL, KEY_E, KEY_ESCAPE, MOUSE_LEFT, MOUSE_RIGHT, ACTION_COUNT } Action;
typedef struct { Action action; NSString *symbol; UIColor *color; CGPoint at; CGFloat size; } Button;
enum { MAX_BUTTONS = 10 };

static const CGFloat kButtonSize = 54, kPortalSize = 60;
static const NSTimeInterval kTapTime = 0.25;   // toucher bref = clic
static const CGFloat kTapSlop = 12;            // points de glissement tolérés pour un clic
static float look_speed(void) {
    static float speed;
    if (!speed) { const char *e = getenv("P2_TOUCH_LOOK"); speed = e && atof(e) > 0 ? (float)atof(e) : 1.0f; }
    return speed;
}

// La boucle de SDL (CFRunLoopRunInMode à délai nul) ne laisse jamais UIKit
// valider ses transactions : un dessin demandé n'apparaîtrait pas. Forcé ici.
static void redraw(UIView *view) {
    [view setNeedsDisplay];
    [view.layer displayIfNeeded];
    [CATransaction flush];
}

// Doigt suivi : rôle, point de départ, dernier point, instant du toucher.
@interface P2Finger : NSObject
@property Role role; @property Action action; @property CGPoint start, at; @property NSTimeInterval began;
@end
@implementation P2Finger
@end

@interface P2TouchControls : UIView
- (instancetype)initWithWindow:(SDL_Window *)window;
@end

@implementation P2TouchControls {
    SDL_Window *window;
    NSMapTable<UITouch *, P2Finger *> *fingers;
    int held[ACTION_COUNT];   // nombre de doigts par action
    BOOL wasd[4];             // W, A, S, D enfoncées
    BOOL concealed;           // cachée pour une manette ou un clavier
    NSTimer *tick;            // caméra : un pas par image tant que le stick est incliné
    float lookCarry[2];       // fractions de comptes de souris reportées
}

- (instancetype)initWithWindow:(SDL_Window *)sdlWindow {
    if ((self = [super initWithFrame:CGRectZero])) {
        window = sdlWindow;
        self.multipleTouchEnabled = YES; self.backgroundColor = UIColor.clearColor; self.opaque = NO;
        self.contentMode = UIViewContentModeRedraw;
        fingers = [NSMapTable strongToStrongObjectsMapTable];
        NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
        for (NSNotificationName name in @[GCControllerDidConnectNotification, GCControllerDidDisconnectNotification,
                                          GCKeyboardDidConnectNotification, GCKeyboardDidDisconnectNotification])
            [center addObserver:self selector:@selector(devicesChanged) name:name object:nil];
        [self devicesChanged];
        __weak P2TouchControls *weakSelf = self;
        tick = [NSTimer scheduledTimerWithTimeInterval:1.0 / 60 repeats:YES block:^(NSTimer *timer) {
            (void)timer; [weakSelf step];
        }];
    }
    return self;
}

- (void)devicesChanged {
    BOOL any = GCKeyboard.coalescedKeyboard != nil;
    for (GCController *c in GCController.controllers) any |= c.extendedGamepad != nil;
    const char *force = getenv("P2_TOUCH_CONTROLS");   // 1 : jamais cachée (essais sur simulateur)
    if (force && *force == '1') any = NO;
    if (any != concealed) { concealed = any; redraw(self); }
}

// À chaque image : rester devant la vue OpenGL de SDL (ajoutée après la
// fenêtre), suivre la taille de la fenêtre, tourner la caméra.
- (void)step {
    UIView *host = self.superview;
    if (host.subviews.lastObject != self) [host bringSubviewToFront:self];
    if (!CGRectEqualToRect(self.frame, host.bounds)) { self.frame = host.bounds; redraw(self); }
    for (P2Finger *f in fingers.objectEnumerator) {
        if (f.role != ROLE_LOOK) continue;
        CGFloat x, y; [self deflectionOf:f x:&x y:&y];
        // ~ 2 500 comptes/s à fond (≈ 165°/s à la sensibilité 3 du jeu), quadratique.
        float scale = 42.0f * look_speed();
        lookCarry[0] += (float)(x * fabs(x)) * scale; lookCarry[1] += (float)(-y * fabs(y)) * scale;
        int dx = (int)lookCarry[0], dy = (int)lookCarry[1];
        lookCarry[0] -= (float)dx; lookCarry[1] -= (float)dy;
        if (dx || dy) {
            SDL_Event e = {0};
            e.type = SDL_MOUSEMOTION; e.motion.windowID = SDL_GetWindowID(window);
            e.motion.xrel = dx; e.motion.yrel = dy;
            SDL_PushEvent(&e);
        }
    }
}

// ---- événements SDL ----
- (void)key:(SDL_Scancode)code down:(BOOL)down {
    SDL_Event e = {0};
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.windowID = SDL_GetWindowID(window); e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.keysym.scancode = code; e.key.keysym.sym = SDL_GetKeyFromScancode(code);
    SDL_PushEvent(&e);
}
// Position en points de la fenêtre SDL (sdl_ios.m la convertit en pixels).
- (void)mouseButton:(Uint8)button down:(BOOL)down at:(CGPoint)p {
    SDL_Event e = {0};
    e.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    e.button.windowID = SDL_GetWindowID(window); e.button.button = button;
    e.button.state = down ? SDL_PRESSED : SDL_RELEASED; e.button.clicks = 1;
    e.button.x = (Sint32)p.x; e.button.y = (Sint32)p.y;
    SDL_PushEvent(&e);
}
- (void)click:(CGPoint)p {
    SDL_Event e = {0};
    e.type = SDL_MOUSEMOTION; e.motion.windowID = SDL_GetWindowID(window);
    e.motion.x = (Sint32)p.x; e.motion.y = (Sint32)p.y;
    SDL_PushEvent(&e);
    [self mouseButton:SDL_BUTTON_LEFT down:YES at:p];
    [self mouseButton:SDL_BUTTON_LEFT down:NO at:p];
}
- (void)action:(Action)action down:(BOOL)down {
    int before = held[action];
    held[action] = MAX(0, held[action] + (down ? 1 : -1));
    if ((before > 0) == (held[action] > 0)) return;
    CGPoint centre = CGPointMake(CGRectGetMidX(self.bounds), CGRectGetMidY(self.bounds));
    switch (action) {
    case KEY_SPACE: [self key:SDL_SCANCODE_SPACE down:down]; break;
    case KEY_CTRL: [self key:SDL_SCANCODE_LCTRL down:down]; break;
    case KEY_E: [self key:SDL_SCANCODE_E down:down]; break;
    case KEY_ESCAPE: [self key:SDL_SCANCODE_ESCAPE down:down]; break;
    case MOUSE_LEFT: [self mouseButton:SDL_BUTTON_LEFT down:down at:centre]; break;
    case MOUSE_RIGHT: [self mouseButton:SDL_BUTTON_RIGHT down:down at:centre]; break;
    default: break;
    }
}
- (void)setDirections:(const BOOL *)wanted {
    static const SDL_Scancode codes[4] = {SDL_SCANCODE_W, SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D};
    for (int i = 0; i < 4; ++i)
        if (wanted[i] != wasd[i]) { wasd[i] = wanted[i]; [self key:codes[i] down:wanted[i]]; }
}

// ---- disposition (points, dans la zone sûre) ----
- (CGRect)area { return UIEdgeInsetsInsetRect(self.bounds, self.safeAreaInsets); }
- (CGFloat)stickRadius { return MIN(66, self.area.size.height * .15); }
- (CGPoint)restingStick:(BOOL)right {
    CGRect a = self.area;
    return CGPointMake(CGRectGetMinX(a) + a.size.width * (right ? .80 : .16), CGRectGetMinY(a) + a.size.height * .72);
}
- (int)buttons:(Button *)b {
    CGRect a = self.area; CGFloat x = CGRectGetMinX(a), y = CGRectGetMinY(a), w = a.size.width, h = a.size.height;
    UIColor *blue = [UIColor colorWithRed:.16 green:.55 blue:1 alpha:.6], *orange = [UIColor colorWithRed:1 green:.55 blue:.05 alpha:.6];
    UIColor *ink = [UIColor colorWithWhite:0 alpha:.35];   // fond sombre : icônes lisibles sur un mur clair
    CGFloat top = y + h * .14, gap = kPortalSize * 1.15;
    Button all[] = {
        // Portails, en miroir : bleu à l'extérieur, orange à l'intérieur, des deux côtés.
        {MOUSE_LEFT, nil, blue, {x + w * .06, top}, kPortalSize}, {MOUSE_RIGHT, nil, orange, {x + w * .06 + gap, top}, kPortalSize},
        {MOUSE_LEFT, nil, blue, {x + w * .94, top}, kPortalSize}, {MOUSE_RIGHT, nil, orange, {x + w * .94 - gap, top}, kPortalSize},
        {KEY_SPACE, @"arrow.up", ink, {x + w * .94, y + h * .50}, kButtonSize},
        {KEY_CTRL, @"arrow.down", ink, {x + w * .80, y + h * .40}, kButtonSize},
        {KEY_E, @"hand.point.up.left.fill", ink, {x + w * .94, y + h * .80}, kButtonSize},
        {KEY_ESCAPE, @"pause.fill", ink, {x + w * .5, y + h * .08}, kButtonSize * .75},
    };
    int n = (int)(sizeof(all) / sizeof(*all));
    for (int i = 0; i < n && i < MAX_BUTTONS; ++i) b[i] = all[i];
    return n;
}
- (int)buttonAt:(CGPoint)p {
    Button b[MAX_BUTTONS]; int n = [self buttons:b];
    for (int i = 0; i < n; ++i) if (hypot(p.x - b[i].at.x, p.y - b[i].at.y) < b[i].size * .62) return (int)b[i].action;
    return -1;
}

// ---- doigts ----
// Inclinaison du stick tenu par ce doigt, de -1 à 1 par axe (y vers le haut),
// zone morte de 15 % retranchée.
- (void)deflectionOf:(P2Finger *)f x:(CGFloat *)x y:(CGFloat *)y {
    CGFloat dx = f.at.x - f.start.x, dy = f.start.y - f.at.y, r = self.stickRadius, dist = hypot(dx, dy);
    if (dist > r) { dx *= r / dist; dy *= r / dist; dist = r; }
    CGFloat dead = r * .15;
    if (dist < dead) { *x = *y = 0; return; }
    CGFloat k = (dist - dead) / (r - dead) / dist;
    *x = dx * k; *y = dy * k;
}
- (void)updateMove {
    BOOL wanted[4] = {NO, NO, NO, NO};
    for (P2Finger *f in fingers.objectEnumerator) {
        if (f.role != ROLE_MOVE) continue;
        CGFloat x, y; [self deflectionOf:f x:&x y:&y];
        if (hypot(x, y) < .2) continue;
        CGFloat angle = atan2(y, x);   // secteurs de 45° : les diagonales enfoncent deux touches
        wanted[0] |= angle > M_PI / 8 && angle < M_PI - M_PI / 8;
        wanted[2] |= angle < -M_PI / 8 && angle > -M_PI + M_PI / 8;
        wanted[3] |= fabs(angle) < 3 * M_PI / 8;
        wanted[1] |= fabs(angle) > 5 * M_PI / 8;
    }
    [self setDirections:wanted];
}
- (BOOL)roleTaken:(Role)role {
    for (P2Finger *f in fingers.objectEnumerator) if (f.role == role) return YES;
    return NO;
}
- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    concealed = NO;   // un toucher ramène la surcouche cachée pour une manette
    for (UITouch *t in touches) {
        P2Finger *f = [P2Finger new];
        f.start = f.at = [t locationInView:self]; f.began = t.timestamp;
        int button = [self buttonAt:f.start];
        BOOL left = f.start.x < CGRectGetMidX(self.bounds);
        if (button >= 0) { f.role = ROLE_BUTTON; f.action = (Action)button; [self action:f.action down:YES]; }
        else if (left && ![self roleTaken:ROLE_MOVE]) f.role = ROLE_MOVE;
        else if (!left && ![self roleTaken:ROLE_LOOK]) f.role = ROLE_LOOK;
        else continue;   // second doigt sur un stick déjà tenu : ignoré
        [fingers setObject:f forKey:t];
    }
    [self updateMove]; redraw(self);
}
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    for (UITouch *t in touches) [fingers objectForKey:t].at = [t locationInView:self];
    [self updateMove]; redraw(self);
}
- (void)finish:(NSSet<UITouch *> *)touches tap:(BOOL)tapAllowed {
    for (UITouch *t in touches) {
        P2Finger *f = [fingers objectForKey:t];
        if (!f) continue;
        if (f.role == ROLE_BUTTON) [self action:f.action down:NO];
        else if (tapAllowed && t.timestamp - f.began < kTapTime && hypot(f.at.x - f.start.x, f.at.y - f.start.y) < kTapSlop)
            [self click:f.start];
        [fingers removeObjectForKey:t];
    }
    [self updateMove]; redraw(self);
}
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { (void)event; [self finish:touches tap:YES]; }
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { (void)event; [self finish:touches tap:NO]; }
- (void)safeAreaInsetsDidChange { [super safeAreaInsetsDidChange]; redraw(self); }

// ---- dessin ----
- (void)drawStick:(CGContextRef)c right:(BOOL)right ink:(UIColor *)ink {
    CGFloat r = self.stickRadius;
    CGPoint origin = [self restingStick:right], knob = origin;
    for (P2Finger *f in fingers.objectEnumerator)
        if (f.role == (right ? ROLE_LOOK : ROLE_MOVE)) { origin = f.start; knob = f.at; }
    CGFloat dx = knob.x - origin.x, dy = knob.y - origin.y, dist = hypot(dx, dy);
    if (dist > r) knob = CGPointMake(origin.x + dx * r / dist, origin.y + dy * r / dist);
    CGContextSetStrokeColorWithColor(c, ink.CGColor); CGContextSetFillColorWithColor(c, ink.CGColor);
    CGContextStrokeEllipseInRect(c, CGRectMake(origin.x - r, origin.y - r, 2 * r, 2 * r));
    CGContextFillEllipseInRect(c, CGRectMake(knob.x - kButtonSize / 2, knob.y - kButtonSize / 2, kButtonSize, kButtonSize));
}
- (void)drawRect:(CGRect)rect {
    (void)rect;
    if (concealed) return;
    CGContextRef c = UIGraphicsGetCurrentContext();
    UIColor *ink = [UIColor colorWithWhite:1 alpha:.3], *line = [UIColor colorWithWhite:1 alpha:.45];
    CGContextSetLineWidth(c, 2);
    [self drawStick:c right:NO ink:ink];
    [self drawStick:c right:YES ink:ink];
    Button b[MAX_BUTTONS]; int n = [self buttons:b];
    for (int i = 0; i < n; ++i) {
        CGFloat s = b[i].size;
        CGRect box = CGRectMake(b[i].at.x - s / 2, b[i].at.y - s / 2, s, s);
        UIColor *fill = held[b[i].action] ? [b[i].color colorWithAlphaComponent:.9] : b[i].color;
        CGContextSetFillColorWithColor(c, fill.CGColor); CGContextFillEllipseInRect(c, box);
        CGContextSetStrokeColorWithColor(c, line.CGColor); CGContextStrokeEllipseInRect(c, box);
        if (!b[i].symbol) continue;
        UIImageSymbolConfiguration *config = [UIImageSymbolConfiguration configurationWithPointSize:s * .38
                                                                                             weight:UIImageSymbolWeightBold];
        UIImage *icon = [[UIImage systemImageNamed:b[i].symbol withConfiguration:config]
                         imageWithTintColor:[UIColor colorWithWhite:1 alpha:.85] renderingMode:UIImageRenderingModeAlwaysOriginal];
        CGSize z = icon.size;
        [icon drawInRect:CGRectMake(b[i].at.x - z.width / 2, b[i].at.y - z.height / 2, z.width, z.height)];
    }
}
@end

static P2TouchControls *controls;

// Appelé à la création de la fenêtre du jeu (sdl_ios.m), sur le thread principal.
void rt_ios_touch_controls_attach(SDL_Window *window) {
    const char *off = getenv("P2_TOUCH_CONTROLS");
    if (controls || !window || (off && *off == '0')) return;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_UIKIT) {
        fprintf(stderr, "[ios] commandes tactiles : fenêtre UIKit introuvable (%s)\n", SDL_GetError());
        return;
    }
    // Sur la fenêtre elle-même : SDL remplace la vue de son contrôleur par la vue
    // OpenGL à la création du contexte, ce qui retirerait une sous-vue posée là.
    UIView *host = info.info.uikit.window;
    if (!host) return;
    controls = [[P2TouchControls alloc] initWithWindow:window];
    controls.frame = host.bounds;
    [host addSubview:controls];
}
#endif
