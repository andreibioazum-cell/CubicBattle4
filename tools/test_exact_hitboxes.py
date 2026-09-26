#!/usr/bin/env python3
"""The hitbox mode shows exactly the zone that deals damage.

For every attack a frame is rendered with the real game scripts, and the drawn
zone (the rect_rot of a strip, the circle of a snowball or a burst) is read
back from the recorded draw commands. The target's body is read from the drawn
sprite itself: the cube of the sprite, pixels 9..40 of 50, at the scale and the
angle the tex command drew it with. The overlap of those two drawn shapes is
computed here independently (polygon clipping and distances to edges, not the
separating axis test of combat/hit_geometry.ds) and must agree with the game's
own hit decision for thousands of random placements:

  - our punch against the bot (enemy_in_punch, used by the battle update),
  - the bot's strike against us (punch_resolve_target with his latched pose),
  - our dash against the bot (tick_dash_solo_hit),
  - our snowball and its burst against the bot (circle_hits_enemy),
  - the universe: its zone is the whole arena.

It also checks that the bot's wind-up shows no zone (it cannot hit yet) and
that a zone shows at full strength from its first frame. Run from the
repository root after python3 gen.py:

    python3 tools/test_exact_hitboxes.py
"""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import test_dash_hitbox  # noqa: E402  (the same build: real scripts, recorded commands)

MAIN = r"""
#define ZONE 0x60000000u
typedef struct { double x, y; } P2;

static void check(int cond, const char *what) {
    if (cond) return;
    printf("FAIL: %s\n", what);
    exit(1);
}
static double urand(double a, double b) { return a + (b - a) * (rand() / (double)RAND_MAX); }

/* --- independent geometry: convex polygons by clipping, circles by edges --- */
static double cross3(P2 a, P2 b, P2 c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }
static int clip(const P2 *in, int n, P2 a, P2 b, P2 *out) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        P2 p = in[i], q = in[(i + 1) % n];
        double dp = cross3(a, b, p), dq = cross3(a, b, q);
        if (dp >= 0) out[m++] = p;
        if ((dp >= 0) != (dq >= 0)) {
            double t = dp / (dp - dq);
            out[m].x = p.x + (q.x - p.x) * t; out[m].y = p.y + (q.y - p.y) * t; m++;
        }
    }
    return m;
}
static double poly_area(const P2 *p, int n) {
    double s = 0;
    for (int i = 0; i < n; i++) s += p[i].x * p[(i + 1) % n].y - p[(i + 1) % n].x * p[i].y;
    return s / 2;
}
static void ccw(P2 *p) { if (poly_area(p, 4) < 0) { P2 t = p[1]; p[1] = p[3]; p[3] = t; } }
/* Area of the intersection of two convex quads (Sutherland-Hodgman). */
static double quad_overlap_area(P2 *a, P2 *b) {
    P2 buf1[32], buf2[32];
    ccw(a); ccw(b);
    int n = 4;
    for (int i = 0; i < 4; i++) buf1[i] = a[i];
    for (int e = 0; e < 4 && n > 0; e++) {
        n = clip(buf1, n, b[e], b[(e + 1) % 4], buf2);
        for (int i = 0; i < n; i++) buf1[i] = buf2[i];
    }
    return n >= 3 ? fabs(poly_area(buf1, n)) : 0;
}
static int point_in_quad(P2 *q, P2 p) {
    ccw(q);
    for (int e = 0; e < 4; e++) if (cross3(q[e], q[(e + 1) % 4], p) < 0) return 0;
    return 1;
}
static double seg_point(P2 a, P2 b, P2 p) {
    double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    double t = l2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / l2 : 0;
    if (t < 0) t = 0; if (t > 1) t = 1;
    double ex = a.x + dx * t - p.x, ey = a.y + dy * t - p.y;
    return sqrt(ex * ex + ey * ey);
}
/* Signed distance from a circle centre to a quad (negative inside). */
static double quad_dist(P2 *q, P2 p) {
    double d = 1e30;
    for (int e = 0; e < 4; e++) { double s = seg_point(q[e], q[(e + 1) % 4], p); if (s < d) d = s; }
    return point_in_quad(q, p) ? -d : d;
}
static void rot_quad(double cx, double cy, double hw, double hh, double ang, P2 *q) {
    static const double sx[4] = { -1, 1, 1, -1 }, sy[4] = { -1, -1, 1, 1 };
    double ca = cos(ang), sa = sin(ang);
    for (int i = 0; i < 4; i++) {
        double lx = sx[i] * hw, ly = sy[i] * hh;
        q[i].x = cx + ca * lx - sa * ly; q[i].y = cy + sa * lx + ca * ly;
    }
}

/* --- reading the frame back --- */
static int zone_rects(P2 q[][4], int max) {
    int n = 0;
    for (size_t k = 0; k < cmd_n && n < max; k++) {
        if (cmds[k].t != DS_CMD_RECT_ROT || cmds[k].v.rot.c != ZONE) continue;
        float x = cmds[k].v.rot.x, y = cmds[k].v.rot.y, w = cmds[k].v.rot.w, h = cmds[k].v.rot.h;
        rot_quad(x + w / 2, y + h / 2, w / 2, h / 2, cmds[k].v.rot.ang, q[n]);
        n++;
    }
    /* An unturned strip goes through geo_rect as a plain rect. */
    for (size_t k = 0; k < cmd_n && n < max; k++) {
        if (cmds[k].t != DS_CMD_RECT || cmds[k].v.rc.c != ZONE) continue;
        float x = cmds[k].v.rc.x, y = cmds[k].v.rc.y, w = cmds[k].v.rc.w, h = cmds[k].v.rc.h;
        rot_quad(x + w / 2, y + h / 2, w / 2, h / 2, 0, q[n]);
        n++;
    }
    return n;
}
static int zone_circle(double *x, double *y, double *r) {
    int n = 0;
    for (size_t k = 0; k < cmd_n; k++) {
        if (cmds[k].t != DS_CMD_CIRCLE || cmds[k].v.ci.c != ZONE) continue;
        *x = cmds[k].v.ci.x; *y = cmds[k].v.ci.y; *r = cmds[k].v.ci.r; n++;
    }
    return n;
}
/* The cube of a drawn sprite: pixels 9..40 of the 50 px texture, turned and
 * scaled exactly as geo_tex draws it. */
static int sprite_body(const char *name, P2 *q) {
    for (size_t k = 0; k < cmd_n; k++) {
        if (cmds[k].t != DS_CMD_TEX || !cmds[k].v.tx.tx) continue;
        if (strcmp(cmds[k].v.tx.tx->name, name) != 0) continue;
        Texture *t = cmds[k].v.tx.tx;
        double sc = cmds[k].v.tx.sc, hw = t->w * 0.5 * sc;
        double cx = cmds[k].v.tx.x + hw, cy = cmds[k].v.tx.y + t->h * 0.5 * sc;
        /* body pixels 9..40 inclusive: from 9.0 to 41.0, centre 25, half 16 */
        double off = (25.0 - t->w * 0.5) * sc;
        rot_quad(cx + off, cy + off, 16 * sc, 16 * sc, cmds[k].v.tx.a, q);
        return 1;
    }
    return 0;
}

static void clear_zones(void) {
    aim_a = 0; enemy_punch_a = 0; dash_box_a = 0; edash_box_a = 0;
    snow_ball_a = 0; snow_boom_a = 0; esnow_ball_a = 0; esnow_boom_a = 0; universe_box_a = 0;
    gift->active = 0; boom_t = 0; dash_active = 0; enemy_dash_active = 0; universe_active = 0;
    enemy->state = 0; punch_left = 0; punch->active = 0;
}
static void place_fighters(void) {
    player->x = urand(300, 980); player->y = urand(200, 520); player->angle = urand(-M_PI, M_PI);
    double a = urand(-M_PI, M_PI), d = urand(0, 260);
    enemy->x = player->x + cos(a) * d; enemy->y = player->y + sin(a) * d;
    enemy->angle = urand(-M_PI, M_PI);
    enemy->hp = 1000; enemy->max_hp = 1000; player->hp = 1000; player->max_hp = 1000;
}
/* Agreement with a margin: placements whose drawn overlap is within EPS of
 * touching are skipped as ties (float rounding of the recorded commands). */
#define EPS 1e-3

int main(void) {
    screen_w = 1280; screen_h = 720;
    amgr = (AAssetManager *)&dummy_amgr_storage;
    ds_main();
    ds_fn_init();
    language = 1; show_hitboxes = 1;
    player_class = CLASS_SANTA;
    game_state = ST_SOLO;
    ds_fn_init_game();
    enemy_class = CLASS_ORDINARY;
    dt = 1.0 / 60.0;
    frame_open = 1;
    srand(7);
    int hits, misses, ties;

    /* 1. Our punch against the bot. */
    hits = misses = ties = 0;
    for (int it = 0; it < 4000; it++) {
        ds_fn_reset_battle(); clear_zones(); place_fighters();
        double pa = urand(-M_PI, M_PI);
        punch->x = player->x; punch->y = player->y; punch->dx = cos(pa); punch->dy = sin(pa);
        punch->active = 1; punch_left = punch_time; aim_a = 1; player->angle = pa;
        cmd_n = 0; ds_fn_draw();
        P2 z[4][4], body[4];
        check(zone_rects(z, 4) == 1, "punch: exactly one zone strip is drawn");
        check(sprite_body("ordinary.png", body), "punch: the bot sprite is drawn");
        double area = quad_overlap_area(z[0], body);
        int game = (int)ds_fn_enemy_in_punch();
        if (area > EPS) { check(game == 1, "punch: the drawn zone touches the bot but does not hit"); hits++; }
        else if (area == 0) {
            /* disjoint or touching at a point: tell them apart by distance */
            double gap = 1e30;
            for (int i = 0; i < 4; i++) { double g = quad_dist(body, z[0][i]); if (g < gap) gap = g; }
            for (int i = 0; i < 4; i++) { double g = quad_dist(z[0], body[i]); if (g < gap) gap = g; }
            if (gap > EPS) { check(game == 0, "punch: hits the bot although the drawn zone does not touch him"); misses++; }
            else ties++;
        } else ties++;
    }
    printf("our punch vs bot:   %d hits, %d misses agree with the drawn zone (%d ties)\n", hits, misses, ties);
    check(hits > 300 && misses > 300, "punch: too few hits or misses sampled");

    /* 2. The bot's strike against us: only the strike shows a zone. */
    hits = misses = ties = 0;
    for (int it = 0; it < 4000; it++) {
        ds_fn_reset_battle(); clear_zones(); place_fighters();
        double ea = urand(-M_PI, M_PI);
        enemy->angle = ea; enemy->state = 2;
        enemy_punch_x = enemy->x; enemy_punch_y = enemy->y;
        enemy_punch_dx = cos(ea); enemy_punch_dy = sin(ea);
        ds_fn_tick_hitbox_fades();
        check(fabs(enemy_punch_a - 1) < 1e-9, "bot strike: the zone does not show at full strength on its first frame");
        cmd_n = 0; ds_fn_draw();
        P2 z[4][4], body[4];
        check(zone_rects(z, 4) == 1, "bot strike: exactly one zone strip is drawn");
        check(sprite_body("santa.png", body), "bot strike: our sprite is drawn");
        double area = quad_overlap_area(z[0], body);
        int game = ds_fn_punch_resolve_target(enemy_punch_x, enemy_punch_y, enemy_punch_dx, enemy_punch_dy,
                                              enemy_punch_reach, enemy_punch_width) == 0;
        if (area > EPS) { check(game, "bot strike: the drawn zone touches us but does not hit"); hits++; }
        else {
            double gap = 1e30;
            for (int i = 0; i < 4; i++) { double g = quad_dist(body, z[0][i]); if (g < gap) gap = g; }
            for (int i = 0; i < 4; i++) { double g = quad_dist(z[0], body[i]); if (g < gap) gap = g; }
            if (gap > EPS) { check(!game, "bot strike: hits us although the drawn zone does not touch us"); misses++; }
            else ties++;
        }
    }
    printf("bot strike vs us:   %d hits, %d misses agree with the drawn zone (%d ties)\n", hits, misses, ties);
    check(hits > 300 && misses > 300, "bot strike: too few hits or misses sampled");
    ds_fn_reset_battle(); clear_zones();
    enemy->state = 1;   /* wind-up */
    enemy_punch_a = 0;
    for (int i = 0; i < 10; i++) ds_fn_tick_hitbox_fades();
    check(enemy_punch_a == 0, "bot wind-up: a zone shows although he cannot hit yet");

    /* 3. Our dash against the bot: the drawn strip over the travelled path. */
    hits = misses = ties = 0;
    for (int it = 0; it < 3000; it++) {
        ds_fn_reset_battle(); clear_zones(); place_fighters();
        double da = urand(-M_PI, M_PI);
        dash_active = 1; dash_hit = 0; dash_box_a = 1; finished = 0; enemy_revive_prot = 0;
        dash_dx = cos(da); dash_dy = sin(da);
        dash_travel = urand(1, azum_dash_speed * azum_dash_time);
        dash_x0 = player->x - dash_dx * dash_travel; dash_y0 = player->y - dash_dy * dash_travel;
        cmd_n = 0; ds_fn_draw();
        P2 z[4][4], body[4];
        check(zone_rects(z, 4) == 1, "dash: exactly one zone strip is drawn");
        check(sprite_body("ordinary.png", body), "dash: the bot sprite is drawn");
        double area = quad_overlap_area(z[0], body);
        ds_fn_tick_dash_solo_hit();
        int game = dash_hit == 1;
        if (area > EPS) { check(game, "dash: the drawn strip touches the bot but does not hit"); hits++; }
        else {
            double gap = 1e30;
            for (int i = 0; i < 4; i++) { double g = quad_dist(body, z[0][i]); if (g < gap) gap = g; }
            for (int i = 0; i < 4; i++) { double g = quad_dist(z[0], body[i]); if (g < gap) gap = g; }
            if (gap > EPS) { check(!game, "dash: hits the bot although the drawn strip does not touch him"); misses++; }
            else ties++;
        }
    }
    printf("our dash vs bot:    %d hits, %d misses agree with the drawn strip (%d ties)\n", hits, misses, ties);
    check(hits > 200 && misses > 200, "dash: too few hits or misses sampled");

    /* 4. Our snowball and its burst against the bot: drawn circle vs his cube. */
    for (int kind = 0; kind < 2; kind++) {
        hits = misses = ties = 0;
        for (int it = 0; it < 3000; it++) {
            ds_fn_reset_battle(); clear_zones(); place_fighters();
            double bx = enemy->x + urand(-160, 160), by = enemy->y + urand(-160, 160);
            if (kind == 0) { gift->active = 1; gift->x = bx; gift->y = by; gift->t = 0; snow_ball_a = 1; }
            else { boom_t = boom_time; boom_x = bx; boom_y = by; snow_boom_a = 1; }
            cmd_n = 0; ds_fn_draw();
            double cx, cy, r; P2 body[4];
            check(zone_circle(&cx, &cy, &r) == 1, "snow: exactly one zone circle is drawn");
            check(sprite_body("ordinary.png", body), "snow: the bot sprite is drawn");
            double gap = quad_dist(body, (P2){ cx, cy }) - r;
            int game = (int)ds_fn_circle_hits_enemy(cx, cy, kind == 0 ? snow_ball_r : super_radius);
            check(fabs(r - (kind == 0 ? snow_ball_r : super_radius)) < 1e-3, "snow: the drawn radius is not the damage radius");
            if (gap < -EPS) { check(game == 1, "snow: the drawn circle touches the bot but does not hit"); hits++; }
            else if (gap > EPS) { check(game == 0, "snow: hits the bot although the drawn circle does not touch him"); misses++; }
            else ties++;
        }
        printf("%s vs bot: %d hits, %d misses agree with the drawn circle (%d ties)\n",
               kind == 0 ? "snowball   " : "snow burst ", hits, misses, ties);
        check(hits > 200 && misses > 200, "snow: too few hits or misses sampled");
    }

    /* 5. The universe hits the whole arena, so its zone is the whole arena. */
    ds_fn_reset_battle(); clear_zones(); universe_active = 1; universe_box_a = 1;
    cmd_n = 0; ds_fn_draw();
    int full = 0;
    for (size_t k = 0; k < cmd_n; k++)
        if (cmds[k].t == DS_CMD_RECT && cmds[k].v.rc.c == ZONE && cmds[k].v.rc.x <= 0 && cmds[k].v.rc.y <= 0 &&
            cmds[k].v.rc.w >= screen_w && cmds[k].v.rc.h >= screen_h) full++;
    check(full == 1, "universe: the zone is not the whole arena");
    puts("universe: the zone covers the whole arena, as its damage does");

    puts("exact hitboxes: every drawn zone is exactly the zone that deals damage");
    return 0;
}
"""


def main() -> int:
    subprocess.run([sys.executable, str(ROOT / "gen.py")], cwd=str(ROOT), check=True,
                   stdout=subprocess.DEVNULL)
    test_dash_hitbox.MAIN = MAIN
    with tempfile.TemporaryDirectory() as tmp:
        binary = test_dash_hitbox.build(Path(tmp))
        run = subprocess.run([str(binary)], cwd=str(ROOT), capture_output=True, text=True, timeout=600)
        print(run.stdout, end="")
        if run.returncode != 0:
            print(run.stderr[-2000:], end="")
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
