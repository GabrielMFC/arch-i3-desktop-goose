#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

// ─── som ─────────────────────────────────────────────────────────────────────
static void play_sound(const char *path) {
    pid_t pid = fork();
    if (pid == 0) {
        // filho: toca o som e morre
        // redireciona stdout/stderr pro nada pra não poluir o terminal
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
        execl("/usr/bin/mpv", "mpv", "--no-terminal", "--volume=60", path, NULL);
        _exit(1);
    }
    // pai não espera — continua o game loop
    // SIGCHLD vai ser ignorado pra não acumular zumbis
}

#define MAX_HONKS 32
static char honk_paths[MAX_HONKS][512];
static int  honk_count = 0;

static void load_honks(const char *honks_dir) {
    DIR *d = opendir(honks_dir);
    if (!d) { fprintf(stderr, "Aviso: pasta de honks não encontrada: %s\n", honks_dir); return; }
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && honk_count < MAX_HONKS) {
        const char *name = entry->d_name;
        size_t len = strlen(name);
        if (len > 4 && strcasecmp(name + len - 4, ".mp3") == 0) {
            snprintf(honk_paths[honk_count], sizeof(honk_paths[0]),
                     "%s/%s", honks_dir, name);
            honk_count++;
        }
    }
    closedir(d);
}

static void play_honk() {
    if (honk_count == 0) return;
    play_sound(honk_paths[rand() % honk_count]);
}



typedef struct { float x, y; } Vec2;

typedef enum { TASK_WANDER, TASK_NAB_MOUSE, TASK_TRACK_MUD, TASK_COLLECT_WINDOW } GooseTask;
typedef enum { NAB_SEEKING, NAB_DRAGGING, NAB_DECELERATING } NabStage;
typedef enum { MUD_RUNNING_OFF, MUD_WANDERING } MudStage;
typedef enum { CW_WALKING_OFF, CW_WAITING, CW_DRAGGING } CWStage;

#define MAX_FOOTMARKS 64
typedef struct { Vec2 pos; double time; } FootMark;

// ─── tempo ───────────────────────────────────────────────────────────────────

static double get_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// ─── math ────────────────────────────────────────────────────────────────────

static float vec2_dist(Vec2 a, Vec2 b) {
    float dx = a.x-b.x, dy = a.y-b.y;
    return sqrtf(dx*dx+dy*dy);
}
static Vec2 vec2_norm(Vec2 v) {
    float m = sqrtf(v.x*v.x+v.y*v.y);
    if (m < 0.0001f) return (Vec2){0,0};
    return (Vec2){v.x/m, v.y/m};
}
static float randf(float lo, float hi) { return lo+(hi-lo)*((float)rand()/RAND_MAX); }
static float clampf(float v, float lo, float hi) { return v<lo?lo:v>hi?hi:v; }
static float lerpf(float a, float b, float t) { return a+(b-a)*t; }

// ─── goose ───────────────────────────────────────────────────────────────────

#define SOUND_BITE    "./Assets/Sound/Effects/BITE.mp3"
#define SOUND_MUD     "./Assets/Sound/Effects/MudSquith.mp3"
#define SOUND_HONKS   "./Assets/Sound/Effects/Honks"

typedef struct {
    Vec2  pos, vel, target;
    float direction, speed, accel;

    // pés
    Vec2  l_foot, r_foot, l_foot_origin, r_foot_origin;
    float l_foot_t, r_foot_t, step_time;

    // neck
    float neck_lerp;

    // wander
    double wander_start, wander_dur, pause_start, pause_dur;

    // task
    GooseTask task;
    NabStage  nab_stage;
    double    nab_start;
    Vec2      nab_drag_to;

    // input
    Vec2 mouse_pos;
    int  mouse_clicked;

    // track mud
    MudStage  mud_stage;
    double    mud_end_time;
    double    mud_dir_change;
    FootMark  footmarks[MAX_FOOTMARKS];
    int       footmark_idx;

    // collect window
    CWStage cw_stage;
    double  cw_wait_start;
    double  cw_wait_dur;
    long    cw_window_id;
    Vec2    cw_window_offset;
    int     cw_from_left;  // 1 = saiu pela esquerda, 0 = direita

    // som
    double next_honk_time;
    int    mud_step_count;

    int screen_w, screen_h;
} Goose;

// forward declaration
static Vec2 calc_beak(Goose *g);

static void set_wander(Goose *g, double now) {
    g->task        = TASK_WANDER;
    g->speed       = 80.0f;
    g->accel       = 1300.0f;
    g->step_time   = 0.2f;
    g->wander_start = now;
    g->wander_dur   = randf(5.0f, 12.0f);
    g->pause_start  = -1;
    g->target = (Vec2){ randf(80, g->screen_w-80), randf(80, g->screen_h-80) };
}

static void set_nab(Goose *g, double now) {
    g->task      = TASK_NAB_MOUSE;
    g->nab_stage = NAB_SEEKING;
    g->nab_start = now;
    g->speed     = 400.0f;
    g->accel     = 2300.0f;
    g->step_time = 0.1f;
}

static void set_mud(Goose *g, double now) {
    g->task          = TASK_TRACK_MUD;
    g->mud_stage     = MUD_RUNNING_OFF;
    g->mud_end_time  = now + 15.0f;
    g->mud_dir_change = now + 2.0f;
    g->speed         = 200.0f;
    g->accel         = 1300.0f;
    g->step_time     = 0.15f;
    // corre pra fora da tela primeiro
    g->target = (Vec2){ g->pos.x < g->screen_w/2 ? -50.0f : g->screen_w+50.0f, g->pos.y };
}

static void add_footmark(Goose *g, Vec2 pos, double now) {
    g->footmarks[g->footmark_idx].pos  = pos;
    g->footmarks[g->footmark_idx].time = now;
    g->footmark_idx = (g->footmark_idx + 1) % MAX_FOOTMARKS;
}

static void goose_init(Goose *g, int sw, int sh) {
    g->pos       = (Vec2){ sw/2.0f, sh/2.0f };
    g->vel       = (Vec2){ 0, 0 };
    g->direction = 0.0f;
    g->neck_lerp = 0.0f;
    g->screen_w  = sw;
    g->screen_h  = sh;
    g->l_foot    = (Vec2){ g->pos.x-5, g->pos.y+9 };
    g->r_foot    = (Vec2){ g->pos.x+5, g->pos.y+9 };
    g->l_foot_t  = -1;
    g->r_foot_t  = -1;
    g->mouse_clicked = 0;
    g->footmark_idx  = 0;
    g->next_honk_time = get_time() + randf(2.0f, 8.0f);
    g->mud_step_count = 0;
    for (int i = 0; i < MAX_FOOTMARKS; i++) g->footmarks[i].time = -999;
    set_wander(g, get_time());
}

static Vec2 foot_home(Goose *g, int right) {
    float rad  = g->direction * 3.14159f / 180.0f;
    float side = right ? 1.0f : -1.0f;
    float px   = -sinf(rad) * side * 6.0f;
    float py   =  cosf(rad) * side * 6.0f;
    return (Vec2){ g->pos.x+px, g->pos.y+py+9.0f };
}

static void goose_tick(Goose *g, double now, double dt) {
    Vec2 mouse = g->mouse_pos;

    // ── task AI ──
    if (g->task == TASK_WANDER) {
        // clicou perto do goose?
        if (g->mouse_clicked && vec2_dist(g->pos, mouse) < 40.0f) {
            set_nab(g, now);
            goto physics;
        }

        if (g->pause_start > 0) {
            g->vel.x = 0; g->vel.y = 0;
            if (now - g->pause_start > g->pause_dur) {
                g->pause_start = -1;
                g->target = (Vec2){ randf(80,g->screen_w-80), randf(80,g->screen_h-80) };
            }
        } else {
            float dist = vec2_dist(g->pos, g->target);
            if (dist < 20.0f) {
                g->vel.x = 0; g->vel.y = 0;
                g->pos   = g->target;
                g->pause_start = now;
                g->pause_dur   = randf(1.0f, 2.5f);
            } else if (dist < 60.0f) {
                g->vel.x *= 0.88f;
                g->vel.y *= 0.88f;
            }
            if (now - g->wander_start > g->wander_dur) {
                float r = randf(0, 1);
                if (r < 0.35f)
                    set_mud(g, now);
                else
                    set_wander(g, now);
            }
        }

    } else if (g->task == TASK_NAB_MOUSE) {
        if (g->nab_stage == NAB_SEEKING) {
            // goose mira com o bico no cursor
            Vec2 beak = calc_beak(g);
            Vec2 offset = { g->pos.x - beak.x, g->pos.y - beak.y };
            g->target.x = mouse.x + offset.x;
            g->target.y = mouse.y + offset.y;
            if (vec2_dist(beak, mouse) < 15.0f) {
                g->nab_stage  = NAB_DRAGGING;
                play_sound(SOUND_BITE);
                g->nab_drag_to = (Vec2){
                    randf(100, g->screen_w-100),
                    randf(100, g->screen_h-100)
                };
                g->target = g->nab_drag_to;
            }
            if (now - g->nab_start > 9.0f)
                g->nab_stage = NAB_DECELERATING;

        } else if (g->nab_stage == NAB_DRAGGING) {
            // pescoço sempre esticado durante drag
            g->neck_lerp = 1.0f;
            if (vec2_dist(g->pos, g->nab_drag_to) < 30.0f)
                g->nab_stage = NAB_DECELERATING;

        } else if (g->nab_stage == NAB_DECELERATING) {
            // freia ativamente
            g->vel.x *= 0.85f;
            g->vel.y *= 0.85f;
            float spd = sqrtf(g->vel.x*g->vel.x + g->vel.y*g->vel.y);
            if (spd < 20.0f) {
                g->vel.x = 0;
                g->vel.y = 0;
                set_wander(g, now);
            }
        }
    } else if (g->task == TASK_TRACK_MUD) {
        if (g->mud_stage == MUD_RUNNING_OFF) {
            // considera que saiu quando chega perto da borda
            int near_edge = g->pos.x < 30 || g->pos.x > g->screen_w-30 ||
                            g->pos.y < 30 || g->pos.y > g->screen_h-30;
            if (near_edge) {
                play_sound(SOUND_MUD);
                g->mud_stage = MUD_WANDERING;
                g->target = (Vec2){ randf(80, g->screen_w-80), randf(80, g->screen_h-80) };
            }
        } else {
            // muda de direção periodicamente
            if (now > g->mud_dir_change || vec2_dist(g->pos, g->target) < 20.0f) {
                g->target = (Vec2){ randf(80, g->screen_w-80), randf(80, g->screen_h-80) };
                g->mud_dir_change = now + randf(1.5f, 3.0f);
            }
            // acabou o tempo
            if (now > g->mud_end_time)
                set_wander(g, now);
        }
    }
physics:;
    // ── física ──
    Vec2 dir_norm = vec2_norm((Vec2){ g->target.x-g->pos.x, g->target.y-g->pos.y });
    float dist_to_target = vec2_dist(g->pos, g->target);

    int braking = (g->task == TASK_WANDER && g->pause_start < 0 && dist_to_target < 60.0f);
    int stopped  = (g->task == TASK_WANDER && g->pause_start > 0);
    int dragging_back = (g->task == TASK_COLLECT_WINDOW && g->cw_stage == CW_DRAGGING);

    if (!stopped && !braking && dist_to_target > 20.0f) {
        float sign = dragging_back ? -1.0f : 1.0f;
        g->vel.x += sign * dir_norm.x * g->accel * dt;
        g->vel.y += sign * dir_norm.y * g->accel * dt;
    }

    float spd = sqrtf(g->vel.x*g->vel.x + g->vel.y*g->vel.y);
    if (spd > g->speed) {
        g->vel.x = g->vel.x/spd * g->speed;
        g->vel.y = g->vel.y/spd * g->speed;
    }

    g->pos.x += g->vel.x * dt;
    g->pos.y += g->vel.y * dt;

    // não sai da tela (exceto quando tá indo pra borda buscar janela)
    int walking_off = (g->task == TASK_COLLECT_WINDOW && g->cw_stage == CW_WALKING_OFF);
    if (!walking_off) {
        if (g->pos.x < 20)            { g->pos.x = 20;            g->vel.x = fabsf(g->vel.x); }
        if (g->pos.x > g->screen_w-20){ g->pos.x = g->screen_w-20; g->vel.x = -fabsf(g->vel.x); }
        if (g->pos.y < 20)            { g->pos.y = 20;            g->vel.y = fabsf(g->vel.y); }
        if (g->pos.y > g->screen_h-20){ g->pos.y = g->screen_h-20; g->vel.y = -fabsf(g->vel.y); }
    }

    // ── direção ──
    if (spd > 30.0f) {
        float target_dir = atan2f(g->vel.y, g->vel.x) * 180.0f / 3.14159f;
        float diff = target_dir - g->direction;
        while (diff >  180) diff -= 360;
        while (diff < -180) diff += 360;
        g->direction += diff * 0.1f;
    }

    // ── neck ──
    float neck_target = (spd > 150.0f) ? 1.0f : 0.0f;
    g->neck_lerp = lerpf(g->neck_lerp, neck_target, 0.075f);

    // ── pés ──
    Vec2 lh = foot_home(g, 0);
    Vec2 rh = foot_home(g, 1);

    if (g->l_foot_t < 0 && g->r_foot_t < 0) {
        if (vec2_dist(g->l_foot, lh) > 5.0f) {
            g->l_foot_origin = g->l_foot;
            g->l_foot_t = 0.0f;
        } else if (vec2_dist(g->r_foot, rh) > 5.0f) {
            g->r_foot_origin = g->r_foot;
            g->r_foot_t = 0.0f;
        }
    }
    if (g->l_foot_t >= 0) {
        g->l_foot_t += dt / g->step_time;
        float t  = clampf(g->l_foot_t, 0, 1);
        float et = t < 0.5f ? 4*t*t*t : 1-powf(-2*t+2,3)/2;
        g->l_foot.x = lerpf(g->l_foot_origin.x, lh.x, et);
        g->l_foot.y = lerpf(g->l_foot_origin.y, lh.y, et);
        if (g->l_foot_t >= 1.0f) {
            g->l_foot_t = -1;
            if (g->task == TASK_TRACK_MUD && g->mud_stage == MUD_WANDERING) {
                add_footmark(g, g->l_foot, now);
                g->mud_step_count++;
            }
        }
    }
    if (g->r_foot_t >= 0) {
        g->r_foot_t += dt / g->step_time;
        float t  = clampf(g->r_foot_t, 0, 1);
        float et = t < 0.5f ? 4*t*t*t : 1-powf(-2*t+2,3)/2;
        g->r_foot.x = lerpf(g->r_foot_origin.x, rh.x, et);
        g->r_foot.y = lerpf(g->r_foot_origin.y, rh.y, et);
        if (g->r_foot_t >= 1.0f) {
            g->r_foot_t = -1;
            if (g->task == TASK_TRACK_MUD && g->mud_stage == MUD_WANDERING) {
                add_footmark(g, g->r_foot, now);
                g->mud_step_count++;
            }
        }
    }

    // ── honk aleatório ──
    if (now > g->next_honk_time) {
        play_honk();
        g->next_honk_time = now + randf(2.0f, 8.0f);
    }

    g->mouse_clicked = 0; // consome o clique
}

// ─── rig ─────────────────────────────────────────────────────────────────────

static Vec2 calc_beak(Goose *g) {
    float dir      = cosf(g->direction * 3.14159f / 180.0f) >= 0 ? 1.0f : -1.0f;
    float neck_h1  = lerpf(20.0f, 10.0f, g->neck_lerp);
    float neck_fw  = lerpf(3.0f,  16.0f, g->neck_lerp);
    Vec2  body     = { g->pos.x,              g->pos.y - 14 };
    Vec2  nbase    = { body.x + dir*15,       body.y };
    Vec2  nhead    = { nbase.x + dir*neck_fw, nbase.y - neck_h1 };
    Vec2  head1    = { nhead.x + dir*3,       nhead.y - 1 };
    Vec2  head2    = { head1.x + dir*5,       head1.y };
    Vec2  beak     = { head2.x + dir*3,       head2.y };
    return beak;
}



static void render(cairo_t *cr, Goose *g, int sw, int sh) {
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);

    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    // marcas de pata
    double now = get_time();
    for (int i = 0; i < MAX_FOOTMARKS; i++) {
        double age = now - g->footmarks[i].time;
        if (age < 0 || age > 60.0) continue;
        float alpha = (float)clampf(1.0f - (float)(age / 60.0f), 0, 1);
        float radius = 3.0f * alpha;
        cairo_set_source_rgba(cr, 0.55, 0.27, 0.07, alpha);
        cairo_arc(cr, g->footmarks[i].pos.x, g->footmarks[i].pos.y, radius, 0, 2*3.14159);
        cairo_fill(cr);
    }

    float dir = cosf(g->direction * 3.14159f / 180.0f) >= 0 ? 1.0f : -1.0f;
    float fx  = dir;
    Vec2  pos = g->pos;

    float neck_h1 = lerpf(20.0f, 10.0f, g->neck_lerp);
    float neck_fw = lerpf(3.0f,  16.0f, g->neck_lerp);

    Vec2 body_center = { pos.x,                    pos.y - 14 };
    Vec2 under       = { pos.x,                    pos.y - 9  };
    Vec2 neck_base   = { body_center.x + dir*15,   body_center.y };
    Vec2 neck_head   = { neck_base.x + dir*neck_fw, neck_base.y - neck_h1 };
    Vec2 head1       = { neck_head.x + dir*3,       neck_head.y - 1 };
    Vec2 head2       = { head1.x + dir*5,           head1.y };
    Vec2 beak        = { head2.x + dir*3,           head2.y };
    Vec2 eye         = { neck_head.x + dir*5,       neck_head.y - 3 };

    // sombra
    cairo_set_source_rgba(cr, 0.3, 0.3, 0.3, 0.3);
    cairo_arc(cr, pos.x, pos.y+2, 18, 0, 2*3.14159);
    cairo_fill(cr);

    // pés
    cairo_set_source_rgb(cr, 1.0, 0.55, 0.0);
    cairo_arc(cr, g->l_foot.x, g->l_foot.y, 4, 0, 2*3.14159); cairo_fill(cr);
    cairo_arc(cr, g->r_foot.x, g->r_foot.y, 4, 0, 2*3.14159); cairo_fill(cr);

    // underbody
    cairo_set_source_rgb(cr, 0.83, 0.83, 0.83);
    cairo_set_line_width(cr, 15.0);
    cairo_move_to(cr, under.x - fx*7, under.y);
    cairo_line_to(cr, under.x + fx*7, under.y);
    cairo_stroke(cr);

    // corpo
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_set_line_width(cr, 22.0);
    cairo_move_to(cr, body_center.x - fx*11, body_center.y);
    cairo_line_to(cr, body_center.x + fx*11, body_center.y);
    cairo_stroke(cr);

    // pescoço
    cairo_set_line_width(cr, 13.0);
    cairo_move_to(cr, neck_base.x, neck_base.y);
    cairo_line_to(cr, neck_head.x, neck_head.y);
    cairo_stroke(cr);

    // cabeça 1
    cairo_set_line_width(cr, 15.0);
    cairo_move_to(cr, neck_head.x, neck_head.y);
    cairo_line_to(cr, head1.x, head1.y);
    cairo_stroke(cr);

    // cabeça 2
    cairo_set_line_width(cr, 10.0);
    cairo_move_to(cr, head1.x, head1.y);
    cairo_line_to(cr, head2.x, head2.y);
    cairo_stroke(cr);

    // bico
    cairo_set_source_rgb(cr, 1.0, 0.55, 0.0);
    cairo_set_line_width(cr, 9.0);
    cairo_move_to(cr, head2.x, head2.y);
    cairo_line_to(cr, beak.x, beak.y);
    cairo_stroke(cr);

    // olho
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_arc(cr, eye.x, eye.y, 2, 0, 2*3.14159);
    cairo_fill(cr);

    (void)sw; (void)sh;
}

// ─── main ────────────────────────────────────────────────────────────────────

int main() {
    srand(time(NULL));
    system("pgrep -x picom > /dev/null || picom -b");

    // ignora SIGCHLD pra não acumular processos zumbi dos sons
    signal(SIGCHLD, SIG_IGN);

    load_honks(SOUND_HONKS);

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "Erro: sem display X11\n"); return 1; }

    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    int sw = DisplayWidth(dpy, screen);
    int sh = DisplayHeight(dpy, screen);

    XVisualInfo vinfo;
    if (!XMatchVisualInfo(dpy, screen, 32, TrueColor, &vinfo)) {
        fprintf(stderr, "Erro: visual 32-bit indisponível\n"); return 1;
    }

    XSetWindowAttributes attrs;
    attrs.override_redirect = True;
    attrs.background_pixel  = 0;
    attrs.border_pixel      = 0;
    attrs.colormap = XCreateColormap(dpy, root, vinfo.visual, AllocNone);
    attrs.event_mask = KeyPressMask | ButtonPressMask | PointerMotionMask;

    Window win = XCreateWindow(
        dpy, root, 0, 0, sw, sh, 0,
        vinfo.depth, InputOutput, vinfo.visual,
        CWOverrideRedirect|CWBackPixel|CWBorderPixel|CWColormap|CWEventMask,
        &attrs
    );

    // sempre no topo
    Atom wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom wm_above = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
    XChangeProperty(dpy, win, wm_state, XA_ATOM, 32,
                    PropModeReplace, (unsigned char*)&wm_above, 1);

    // click-through padrão (região de input vazia)
    XRectangle empty = {0,0,0,0};
    XShapeCombineRectangles(dpy, win, ShapeInput, 0, 0, &empty, 1, ShapeSet, 0);

    XMapRaised(dpy, win);
    XFlush(dpy);

    cairo_surface_t *surface = cairo_xlib_surface_create(dpy, win, vinfo.visual, sw, sh);
    cairo_t *cr = cairo_create(surface);

    Goose goose;
    goose_init(&goose, sw, sh);

    double last = get_time();
    int running = 1;

    while (running) {
        // pega posição do mouse via XQueryPointer (não bloqueante)
        {
            Window wr, wc;
            int rx, ry, wx, wy;
            unsigned int mask;
            XQueryPointer(dpy, root, &wr, &wc, &rx, &ry, &wx, &wy, &mask);
            goose.mouse_pos = (Vec2){ (float)rx, (float)ry };

            // detecta clique esquerdo via máscara de botões
            static unsigned int last_mask = 0;
            if ((mask & Button1Mask) && !(last_mask & Button1Mask))
                goose.mouse_clicked = 1;
            last_mask = mask;
        }

        // eventos de teclado
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
            if (e.type == KeyPress) running = 0;
        }

        double now = get_time();
        double dt  = now - last;
        last = now;
        if (dt > 0.05) dt = 0.05;

        // warp do mouse para o bico quando dragging
        if (goose.task == TASK_NAB_MOUSE && goose.nab_stage == NAB_DRAGGING) {
            Vec2 beak = calc_beak(&goose);
            XWarpPointer(dpy, None, root, 0,0,0,0, (int)beak.x, (int)beak.y);
        }

        goose_tick(&goose, now, dt);
        render(cr, &goose, sw, sh);
        cairo_surface_flush(surface);
        XFlush(dpy);

        usleep(8333);
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
