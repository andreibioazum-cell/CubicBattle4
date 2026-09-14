#!/usr/bin/env python3
"""Regression checks for the chat sticker menu, without Android or Firebase.

Compile the real DimScript sources, then run the chat sticker functions with
mock rendering and networking. Requires a host C compiler (CC). Temporary
Android header stubs only satisfy runtime.h; this is not a PC build.

Covered:
  * /sticker <имя> parsing (known textures only, no prefix abuse);
  * sticker tap sends the message and instantly closes the chat;
  * sticker button toggles the small menu, outside tap closes it;
  * sticker messages render as images in the chat list and head bubbles.
"""
from pathlib import Path
import os
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from ds_compiler import DimScriptCompiler  # noqa: E402
from gen import find_ds_files  # noqa: E402

HARNESS = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "game.c"

int screen_w = 720, screen_h = 1280;
double dt = 0.1;
Joy joy;

/* ---------- string / misc native stubs ---------- */
double str_len(const char *s) { return (double)(int)strlen(s); }
int str_eq(const char *a, const char *b) { return a && b && strcmp(a, b) == 0; }
int str_contains(const char *hay, const char *needle) { return hay && needle && strstr(hay, needle) != NULL; }
double str_index_of(const char *hay, const char *needle) { const char *p = hay && needle ? strstr(hay, needle) : NULL; return p ? (double)(p - hay) : -1; }
int str_starts_with(const char *s, const char *pref) { return s && pref && strncmp(s, pref, strlen(pref)) == 0; }
int str_ends_with(const char *s, const char *suf) { size_t ls = s ? strlen(s) : 0, lf = suf ? strlen(suf) : 0; return ls >= lf && suf && strcmp(s + ls - lf, suf) == 0; }
const char *str_sub(const char *s, double start, double len) {
    /* Как в нативе: каждый вызов — отдельная выделенная строка. */
    size_t sl = s ? strlen(s) : 0, st = (size_t)start, ln = (size_t)len;
    if (st > sl) st = sl;
    if (st + ln > sl) ln = sl - st;
    char *out = malloc(ln + 1);
    assert(out);
    if (s && ln > 0) memcpy(out, s + st, ln);
    out[ln] = 0;
    return out;
}
char *ds_concat(const char *left, const char *right) {
    static char buf[1024];
    snprintf(buf, sizeof(buf), "%s%s", left ? left : "", right ? right : "");
    return buf;
}
double clamp(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
double dist(double x1, double y1, double x2, double y2) { return hypot(x1 - x2, y1 - y2); }
void ds_log(const char *format, ...) { (void)format; }
void ds_runtime_error(const char *format, ...) { fputs(format, stderr); abort(); }

/* ---------- array stubs (ds_main инициализирует глобальные массивы) ---------- */
struct DSArray { int len; double values[4096]; };
DSArray *arr_new(void) { DSArray *a = calloc(1, sizeof(*a)); assert(a); return a; }
void arr_free(DSArray *a) { free(a); }
void arr_clear(DSArray *a) { a->len = 0; }
void arr_push(DSArray *a, double v) { assert(a->len < 4096); a->values[a->len++] = v; }
double arr_get(DSArray *a, double i) { return i >= 0 && i < a->len ? a->values[(int)i] : 0; }
void arr_set(DSArray *a, double i, double v) { assert(i >= 0 && i < 4096); while (a->len <= (int)i) arr_push(a, 0); a->values[(int)i] = v; }
double arr_len(DSArray *a) { return a->len; }

/* ---------- rendering mocks ---------- */
typedef struct { const char *name; float x, y, angle, scale; } TexCall;
static TexCall tex_calls[64];
static int tex_count, ring_calls, circle_calls, rect_calls, roundrect_calls;
void rect(float x, float y, float w, float h, uint32_t color) { (void)x; (void)y; (void)w; (void)h; (void)color; rect_calls++; }
void roundrect(float x, float y, float w, float h, float r, uint32_t color) { (void)x; (void)y; (void)w; (void)h; (void)r; (void)color; roundrect_calls++; }
void circle(float x, float y, float r, uint32_t color) { (void)x; (void)y; (void)r; (void)color; circle_calls++; }
void ring(float x, float y, float r, float t, uint32_t color) { (void)x; (void)y; (void)r; (void)t; (void)color; ring_calls++; }
void line(float x1, float y1, float x2, float y2, float t, uint32_t color) { (void)x1; (void)y1; (void)x2; (void)y2; (void)t; (void)color; }
int png_load(const char *name) { (void)name; return 1; }
void tex(float x, float y, const char *name, float a, float sc) {
    if (!(isfinite(x + y + a + sc) && sc > 0) || tex_count >= 64) {
        fprintf(stderr, "bad tex: %s x=%g y=%g a=%g sc=%g (count=%d)\n", name, x, y, a, sc, tex_count);
        abort();
    }
    tex_calls[tex_count++] = (TexCall){name, x, y, a, sc};
}
void tex_tint(float x, float y, const char *name, float a, float sc, uint32_t c) { (void)c; tex(x, y, name, a, sc); }
static int ink_width(const char *s) { return s ? (int)strlen(s) * 10 : 0; }
void text(const char *s, float x, float y, uint32_t c) { (void)s; (void)x; (void)y; (void)c; }
void text_scaled(const char *s, float x, float y, uint32_t c, float sc) { (void)s; (void)x; (void)y; (void)c; (void)sc; }
int text_width(const char *s) { return ink_width(s); }
int text_height(const char *s) { (void)s; return 20; }
int text_ink_width(const char *s) { return ink_width(s); }
int text_ink_height(const char *s) { (void)s; return 20; }
int text_ink_top(const char *s) { (void)s; return 0; }

/* ---------- keyboard mock ---------- */
static int keyboard_up;
static char keyboard_buf[64] = "";
void keyboard_show(void) { keyboard_up = 1; }
void keyboard_hide(void) { keyboard_up = 0; }
const char *keyboard_get_text(void) { return keyboard_buf; }
const char *keyboard_get_raw(void) { return keyboard_buf; }
void keyboard_clear(void) { keyboard_buf[0] = 0; }
int keyboard_visible(void) { return keyboard_up; }
int keyboard_enter_pressed(void) { return 0; }

/* ---------- network mock ---------- */
double net_slot(void) { return 0; }
double net_login_status(void) { return 0; }
const char *net_login_nick(void) { return "Tester"; }
const char *net_login_pass(void) { return ""; }
double net_event(void) { return 0; }
void net_event_set(double mode) { (void)mode; }
double net_banned(void) { return 0; }
void net_ban_set(const char *nick, double banned) { (void)nick; (void)banned; }
double net_chat_is_ban(const char *msg) { (void)msg; return 0; }
double net_chat_is_unban(const char *msg) { (void)msg; return 0; }
const char *net_chat_ban_target(const char *msg) { (void)msg; return ""; }
const char *net_chat_unban_target(const char *msg) { (void)msg; return ""; }
double net_chat_is_text_cmd(const char *msg) { (void)msg; return 0; }
const char *net_chat_text_cmd_text(const char *msg) { (void)msg; return ""; }
const char *net_chat_text_cmd_color(const char *msg) { (void)msg; return ""; }
void net_banner_send(const char *text, const char *color) { (void)text; (void)color; }
double net_banner_ts(void) { return 0; }
const char *net_banner_text(void) { return ""; }
const char *net_banner_color(void) { return ""; }
double net_is_banned(const char *nick) { (void)nick; return 0; }
void net_save_settings(double language, double hitboxes) { (void)language; (void)hitboxes; }
void net_save_music_volume(double volume) { (void)volume; }
void net_save_fps_cap(double cap) { (void)cap; }
void net_save_render_scale(double scale) { (void)scale; }
void net_leaderboard_fetch(const char *url) { (void)url; }
void snd_volume(const char *name, double volume) { (void)name; (void)volume; }

static const char *chat_msgs[16];
static int chat_msg_count;
static char sent_buf[16][128];
static int sent_count;
void net_chat_send(const char *text) {
    assert(sent_count < 16 && strlen(text) < 128);
    strcpy(sent_buf[sent_count++], text);
}
void net_chat_trim(double keep) { (void)keep; }
double net_chat_count(void) { return chat_msg_count; }
const char *net_chat_text(double idx) { return idx >= 0 && idx < chat_msg_count ? chat_msgs[(int)idx] : ""; }
const char *net_chat_uid(double idx) { (void)idx; return "Tester"; }
const char *net_chat_key(double idx) { (void)idx; return "key"; }

static void near(double a, double b) { assert(fabs(a - b) < 0.001); }

static void test_parsing(void) {
    assert(strcmp(ds_fn_chat_sticker_tex("/sticker like.png"), "like.png") == 0);
    assert(strcmp(ds_fn_chat_sticker_tex("/sticker dislike.png"), "dislike.png") == 0);
    assert(strcmp(ds_fn_chat_sticker_tex("/sticker 75.png"), "75.png") == 0);
    /* Неизвестные текстуры и обход префикса — обычный текст. */
    assert(strcmp(ds_fn_chat_sticker_tex("/sticker hacked.png"), "") == 0);
    assert(strcmp(ds_fn_chat_sticker_tex("/sticker like.pngx"), "") == 0);
    assert(strcmp(ds_fn_chat_sticker_tex("/sticker"), "") == 0);
    assert(strcmp(ds_fn_chat_sticker_tex("/sticker "), "") == 0);
    assert(strcmp(ds_fn_chat_sticker_tex("/stickery"), "") == 0);
    assert(strcmp(ds_fn_chat_sticker_tex("hi /sticker like.png"), "") == 0);
    assert(strcmp(ds_fn_chat_sticker_tex(""), "") == 0);
    puts("parsing: only the three known sticker messages are recognized OK");
}

static void test_send_and_touch(void) {
    /* Отправка: сообщение уходит в чат, чат закрывается мгновенно. */
    sent_count = 0;
    ds_fn_chat_set_open(1);
    assert(chat_open == 1 && sticker_menu_open == 0);
    ds_fn_chat_send_sticker(STICKER_DISLIKE);
    assert(sent_count == 1 && strcmp(sent_buf[0], "/sticker dislike.png") == 0);
    assert(chat_open == 0 && sticker_menu_open == 0 && keyboard_up == 0);

    /* Кнопка: тап открывает меню, второй тап — закрывает. */
    ds_fn_chat_set_open(1);
    double bx = ds_fn_sticker_btn_x(), by = ds_fn_chat_input_y();
    near(bx, 16 + (screen_w - 32) - 112 - 8 - STICKER_BTN);
    assert(ds_fn_touch_chat(bx + 26, by + 26, 0) == 1);
    assert(sticker_menu_open == 1);
    assert(ds_fn_touch_chat(bx + 26, by + 26, 0) == 1);
    assert(sticker_menu_open == 0);

    /* Тап по плитке: стикер отправлен, чат закрыт, меню сброшено. */
    sent_count = 0;
    assert(ds_fn_touch_chat(bx + 26, by + 26, 0) == 1);  /* open menu */
    near(ds_fn_sticker_menu_x(), 16 + (screen_w - 32) - 240);
    near(ds_fn_sticker_menu_y(), by - 88 - 10);
    double tx = ds_fn_sticker_tile_x(2), ty = ds_fn_sticker_tile_y();
    assert(ds_fn_touch_chat(tx + 32, ty + 32, 0) == 1);
    assert(sent_count == 1 && strcmp(sent_buf[0], "/sticker 75.png") == 0);
    assert(chat_open == 0 && sticker_menu_open == 0);

    /* Тап в подложке меню ничего не шлёт и меню не закрывает; тап мимо — закрывает. */
    ds_fn_chat_set_open(1);
    assert(ds_fn_touch_chat(bx + 26, by + 26, 0) == 1);  /* open menu */
    sent_count = 0;
    assert(ds_fn_touch_chat(ds_fn_sticker_menu_x() + 2, ds_fn_sticker_menu_y() + 40, 0) == 1);
    assert(sent_count == 0 && sticker_menu_open == 1 && chat_open == 1);
    assert(ds_fn_touch_chat(60, 600, 0) == 1);
    assert(sticker_menu_open == 0 && chat_open == 1);

    /* Кнопка «закрыть» чат сбрасывает меню (круг с крестиком сверху по центру). */
    assert(ds_fn_touch_chat(bx + 26, by + 26, 0) == 1);  /* open menu */
    assert(ds_fn_touch_chat(ds_fn_chat_close_x(), ds_fn_chat_top_y(), 0) == 1);
    assert(chat_open == 0 && sticker_menu_open == 0);
    puts("touch: button toggles menu, tile sends + closes instantly, outside tap closes menu OK");
}

void ds_set_fps_cap(int fps) { (void)fps; }
void ds_set_render_scale(int scale) { (void)scale; }

static void test_video_settings(void) {
    /* Циклы значений в настройках: FPS макс -> 60 -> 30 -> макс, апскейл 1 -> 2 -> 3. */
    assert(ds_fn_next_fps_cap(0) == 60);
    assert(ds_fn_next_fps_cap(60) == 30);
    assert(ds_fn_next_fps_cap(30) == 0);
    assert(ds_fn_next_render_scale(1) == 2);
    assert(ds_fn_next_render_scale(2) == 3);
    assert(ds_fn_next_render_scale(3) == 1);
    /* Все 5 строк настроек помещаются на низком (landscape) экране. */
    screen_h = 720;
    assert(ds_fn_settings_row_y(4) + 56 <= screen_h - 4);
    screen_h = 1280;
    assert(ds_fn_settings_row_y(4) + 56 <= screen_h - 4);
    puts("video settings: fps/scale cycling and settings screen fit OK");
}

static void test_render(void) {
    /* История: стикеры рисуются картинкой в своей строке, текст — текстом. */
    game_state = ST_ONLINE;
    chat_msg_count = 3;
    chat_msgs[0] = "hello";
    chat_msgs[1] = "/sticker like.png";
    chat_msgs[2] = "/sticker 75.png";
    ds_fn_chat_set_open(1);
    tex_count = 0; ring_calls = 0; circle_calls = 0;
    ds_fn_draw_chat();
    assert(tex_count == 2);  /* только две строки-стикера */
    assert(strcmp(tex_calls[0].name, "like.png") == 0);
    assert(strcmp(tex_calls[1].name, "75.png") == 0);
    near(tex_calls[0].scale, 24.0 / 100.0);  /* стикеры 100x100, строка 24px */
    near(tex_calls[1].scale, 24.0 / 100.0);
    near(tex_calls[0].x, 16 + 8);
    /* верх списка = exit_cy + exit_r + 12 = 82 */
    near(tex_calls[0].y, 82 + 28 + 2);   /* вторая строка, по центру её высоты */
    near(tex_calls[1].y, 82 + 56 + 2);   /* третья строка */
    assert(ring_calls == 1 && circle_calls == 4);  /* смайлик + круг кнопки закрытия */

    /* Пузырь над головой: стикер — картинкой, обычный текст — без tex. */
    tex_count = 0; roundrect_calls = 0;
    ds_fn_draw_chat_bubble_at(360, 640, "/sticker dislike.png", 2.0);
    assert(tex_count == 1 && strcmp(tex_calls[0].name, "dislike.png") == 0);
    near(tex_calls[0].scale, 40.0 / 100.0);
    near(tex_calls[0].x, 360 - 56 / 2 + 8);
    near(tex_calls[0].y, 640 - 63 - 56 - 30 + 8);  /* hw = 25*1.4*1.8 = 63 */
    assert(roundrect_calls == 1);
    tex_count = 0;
    ds_fn_draw_chat_bubble_at(360, 640, "привет", 2.0);
    assert(tex_count == 0);
    puts("render: sticker rows in the chat list and sticker head bubbles OK");
}

static void expect_transition(int want) {
    assert(t_dir == 1 && t_target == want);
    t_dir = 0;  /* снять блокировку, чтобы следующий тап сработал */
}

/* Минималистичный UI: в лобби одна большая «Играть» и две поды, всё
 * остальное — на экране «Меню»; «назад» — компактный круг в углу; в бою
 * выход — крестик сверху по центру; настройки — пять строк. */
static void test_minimal_ui(void) {
    /* Лобби: Play -> режимы, Shop -> классы, More -> меню. */
    game_state = ST_LOBBY;
    ds_fn_touch_lobby((screen_w - lobby_play_w) / 2 + lobby_play_w / 2, ds_fn_lobby_play_y() + lobby_play_h / 2, 0);
    expect_transition(ST_MODES);
    ds_fn_touch_lobby(ds_fn_lobby_sub_x() + lobby_sub_w / 2, ds_fn_lobby_sub_y(0) + lobby_sub_h / 2, 0);
    expect_transition(ST_CLASSES);
    ds_fn_touch_lobby(ds_fn_lobby_sub_x() + lobby_sub_w / 2, ds_fn_lobby_sub_y(1) + lobby_sub_h / 2, 0);
    expect_transition(ST_MORE);

    /* Экран «Меню»: настройки, аккаунт, достижения, задания. */
    game_state = ST_MORE;
    ds_fn_touch_more(ds_fn_menu_x() + btn_w / 2, ds_fn_more_row(0) + btn_h / 2);
    expect_transition(ST_SETTINGS);
    ds_fn_touch_more(ds_fn_menu_x() + btn_w / 2, ds_fn_more_row(1) + btn_h / 2);
    expect_transition(ST_LOGIN);
    ds_fn_touch_more(ds_fn_menu_x() + btn_w / 2, ds_fn_more_row(2) + btn_h / 2);
    expect_transition(ST_ACHIEVEMENTS);
    ds_fn_touch_more(ds_fn_menu_x() + btn_w / 2, ds_fn_more_row(3) + btn_h / 2);
    expect_transition(ST_QUESTS);

    /* Режимы: четыре строки, «назад» больше не строка. */
    game_state = ST_MODES;
    ds_fn_touch_modes(ds_fn_menu_x() + btn_w / 2, ds_fn_modes_row(3) + btn_h / 2);
    expect_transition(ST_PROMO);

    /* Настройки: пять строк; музыка — левая половина минус, правая плюс. */
    game_state = ST_SETTINGS;
    music_volume = 50;
    ds_fn_touch_settings(ds_fn_menu_x() + btn_w * 0.25, ds_fn_settings_row_y(1) + btn_h / 2);
    assert(music_volume == 40);
    ds_fn_touch_settings(ds_fn_menu_x() + btn_w * 0.75, ds_fn_settings_row_y(1) + btn_h / 2);
    assert(music_volume == 50);
    ds_fn_touch_settings(ds_fn_menu_x() + btn_w / 2, ds_fn_settings_row_y(0) + btn_h / 2);
    assert(language == 1);
    language = 0;

    /* «Назад» в меню — круг в левом верхнем углу; в бою выход — крестик
     * сверху по центру, и наоборот. */
    game_state = ST_SETTINGS;
    assert(ds_fn_back_hit(back_cx, back_cy) == 1);
    assert(ds_fn_back_hit(screen_w / 2, exit_cy) == 0);
    game_state = ST_SOLO;
    assert(ds_fn_back_hit(screen_w / 2, exit_cy) == 1);
    assert(ds_fn_back_hit(back_cx, back_cy) == 0);
    game_state = ST_ONLINE;
    assert(ds_fn_hit_chat_circle(chat_cx, chat_cy) == 1);
    puts("minimal ui: lobby 3 buttons, more menu, 5 settings rows, corner back OK");
}

int main(void) {
    setbuf(stdout, NULL);
    ds_main();
    test_parsing();
    test_send_and_touch();
    test_video_settings();
    test_minimal_ui();
    test_render();
    return 0;
}
'''


def main():
    # Заглушки стикеров должны лежать в ассетах 100x100.
    for name in ("like.png", "dislike.png", "75.png"):
        data = (ROOT / "game/assets" / name).read_bytes()
        assert data[:8] == b"\x89PNG\r\n\x1a\n", f"{name} is not a PNG"
        assert int.from_bytes(data[16:20], "big") == 100, f"{name} width"
        assert int.from_bytes(data[20:24], "big") == 100, f"{name} height"
    with tempfile.TemporaryDirectory(prefix="cubic-sticker-") as directory:
        temp = Path(directory)
        compiler = DimScriptCompiler()
        assert compiler.compile(find_ds_files(str(ROOT / "game/scripts")), str(temp / "game.c"))
        assert not compiler.errors and not compiler.warnings
        # Wiring: в открытом чате рисуется стикер-кнопка и меню, в истории — картинки.
        draw_body = "".join(compiler.functions["draw_chat"][1])
        assert "draw_sticker_face(" in draw_body and "draw_sticker_tile(" in draw_body
        assert "sticker_menu_open == 1" in draw_body
        touch_body = "".join(compiler.functions["touch_chat"][1])
        assert "chat_send_sticker(" in touch_body
        # Настройки: FPS и апскейл меняются из экрана настроек.
        settings_body = "".join(compiler.functions["draw_settings"][1])
        assert "tr_fps_label()" in settings_body and "tr_scale_label()" in settings_body
        assert "settings_row_y(4)" in settings_body
        touch_settings_body = "".join(compiler.functions["touch_settings"][1])
        assert "ds_set_fps_cap(" in touch_settings_body
        assert "ds_set_render_scale(" in touch_settings_body
        bubble_body = "".join(compiler.functions["draw_chat_bubble_at"][1])
        assert "chat_sticker_tex(" in bubble_body
        android = temp / "android"
        android.mkdir()
        (android / "asset_manager.h").write_text("typedef struct AAssetManager AAssetManager;\n")
        (android / "log.h").touch()
        (android / "native_window.h").touch()
        (temp / "test.c").write_text(HARNESS)
        subprocess.run([
            *shlex.split(os.environ.get("CC", "cc")), "-std=c99", "-O0",
            "-Werror=implicit-function-declaration", "-Werror=incompatible-pointer-types",
            "-ffunction-sections", "-fdata-sections", "-I", str(temp), "-I", str(ROOT),
            str(temp / "test.c"), "-Wl,--gc-sections", "-lm", "-o", str(temp / "test"),
        ], check=True)
        subprocess.run([str(temp / "test")], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
