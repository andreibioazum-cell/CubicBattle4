#!/usr/bin/env python3
"""Legal minimum: the epilepsy consent gate and the privacy screen.

Checks the wiring described in the README from the sources alone, without
compiling:

  gate    - the epilepsy warning never hides itself: update_warning no longer
            closes warn_open, entry goes through legal_accept only, and the
            system Back button on that screen exits the game, so the game cannot
            be entered without consent;
  stamp   - accepting writes a stamp into settings.dat natively
            (settings_mark_legal, the "legal %d" line); the stamp is local and
            never reaches the cloud PATCH;
  privacy - the ST_PRIVACY screen is drawn, reachable from settings (row 8) and
            returns to settings with a Back button; both languages are there;
  perms   - the manifest has exactly two permissions, INTERNET and
            ACCESS_NETWORK_STATE, and nothing about location or contacts.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]


def read(*parts):
    return (ROOT.joinpath(*parts)).read_text(encoding="utf-8")


def function_body(source, name):
    """Body of public function name ... end (DimScript)."""
    match = re.search(
        r"public function %s\(.*?\nend\n" % re.escape(name), source, re.S)
    assert match, "функция %s не найдена" % name
    return match.group(0)


def main():
    engine = read("game", "scripts", "core", "engine.ds")
    menu_input = read("game", "scripts", "ui", "menu_input.ds")
    menu_screens = read("game", "scripts", "ui", "menu_screens.ds")
    layout = read("game", "scripts", "ui", "layout.ds")
    locale = read("game", "scripts", "ui", "locale_core.ds")
    config = read("game", "scripts", "core", "config.ds")
    storage = read("native", "net", "settings_storage.inc")
    manifest = read("game", "AndroidManifest.xml")

    # --- consent gate: no auto hide, entry only through the button --------
    warn_update = function_body(engine, "update_warning")
    assert "warn_open = 0" in warn_update and "warn_closing == 1" in warn_update, \
        "предупреждение должно закрываться только после плавного затухания"
    assert "warn_close_t = clamp(warn_close_t + dt / warn_fade, 0, 1)" in warn_update and \
        "warn_a = 1 - p*p*(3-2*p)" in warn_update, \
        "после согласия нет плавного smoothstep fade-out предупреждения"
    accept = function_body(menu_input, "legal_accept")
    assert "warn_closing = 1" in accept and "settings_mark_legal()" in accept, \
        "согласие не запускает затухание или не пишет метку: %s" % accept
    assert "if warn_ready < 1 || warn_closing == 1 then" in accept
    assert "warn_t = warn_t + dt" in warn_update and \
        "warn_ready = clamp(warn_t / warn_wait, 0, 1)" in warn_update, \
        "отсчёт секунд должен вестись по warn_t"
    assert "warn_t=0, warn_wait=3" in config
    touch_warn = function_body(menu_input, "touch_warn")
    assert "hit_warn_btn" in touch_warn and "legal_accept()" in touch_warn
    touch = function_body(engine, "touch")
    assert "touch_warn(x, y, action)" in touch, \
        "тапы на экране предупреждения не обрабатываются"
    assert not re.search(r"if warn_open == 1 \|\| studio_open == 1 then\s*\n\s*return\s*\n",
                         touch), "тапы на предупреждении всё ещё глотаются"
    back = function_body(engine, "back_pressed")
    assert re.search(r"if warn_open == 1 then\s*\n(?:\s*--[^\n]*\n)*\s*return 0", back), \
        "системная «Назад» на предупреждении не выходит из игры"
    draw_warn = function_body(menu_input, "draw_warning")
    assert "tr_legal_accept()" in draw_warn and "warn_btn_y()" in draw_warn, \
        "на экране предупреждения нет кнопки согласия"
    # The button does not light up: brightness stays at its final value.
    assert "brightness" not in draw_warn, \
        "кнопка согласия снова меняет яркость со временем"
    # While counting down the label holds "(N)" seconds, then drops them.
    assert "warn_wait - floor(warn_t)" in draw_warn and "warn_ready < 1" in draw_warn, \
        "на кнопке нет отсчёта секунд до разблокировки"
    assert "warn_btn_w" in layout and "hit_warn_btn" in layout
    # The consent button has an outline, drawn after the fill: white geometry
    # under a fading translucent black fill shows through and makes the button
    # flash white right before it disappears, which is how the frame was lost.
    ui = read("game", "scripts", "core", "ui.ds")
    outline = function_body(ui, "roundrect_outline")
    order = [m.group(0) for m in re.finditer(r"(roundrect|roundrect_outline|text_in_box)\(", draw_warn)]
    assert order == ["roundrect(", "roundrect_outline(", "text_in_box("], order
    assert "line(" in outline and "outline_arc(" in outline, \
        "обводка должна состоять из штриха (line) по сторонам и дуг"
    # The stroke sits outside the edge: half of its thickness is offset from
    # x/y/w/h outwards.
    assert "local number o = t / 2" in outline and "line(x - o, y + rad, x - o, y + h - rad, t, c)" in outline
    assert "warn_frame_th" in config and "warn_frame_th" in draw_warn, \
        "на кнопке согласия нет обводки (warn_frame_th)"
    arcs = re.findall(r"outline_arc\(x[^\n]*\)", outline)
    assert len(arcs) == 4, "по углам кнопки должны быть четыре дуги обводки"
    for mx, my in (("-1", "-1"), ("1", "-1"), ("1", "1"), ("-1", "1")):
        assert any(mx in arc for arc in arcs) and any(my in arc for arc in arcs), \
            "дуги должны быть во всех четырёх четвертях"

    # --- consent stamp: local, survives a restart -------------------------
    assert 'sscanf(line, "legal %d", &value)' in storage
    assert '"legal %d\\n"' in storage or "legal %d\\n" in storage
    assert "void settings_mark_legal(void)" in storage
    assert "double settings_legal_ts(void)" in storage
    push = re.search(r"static void settings_push_to_cloud.*?\n}\n", storage, re.S)
    assert push and "legal" not in push.group(0).split("snprintf(body")[1], \
        "метка согласия не должна уходить в облако"
    net_h = read("net.h")
    assert "void settings_mark_legal(void);" in net_h
    assert "double settings_legal_ts(void);" in net_h
    compiler = read("ds_compiler.py")
    assert "'settings_mark_legal'" in compiler, \
        "нативная функция не зарегистрирована в BUILTINS компилятора"

    # --- privacy screen: drawn, reached from settings, back works ---------
    assert "ST_PRIVACY=16" in config
    assert "SETTINGS_ROWS=7" in layout
    draw_settings = function_body(menu_screens, "draw_settings")
    assert "settings_row_y(6)" in draw_settings and "tr_privacy()" in draw_settings
    draw_privacy = function_body(menu_screens, "draw_privacy")
    for i in range(1, 11):
        assert "tr_pv%d()" % i in draw_privacy, "строка tr_pv%d не рисуется" % i
    touch_settings = function_body(menu_input, "touch_settings")
    assert re.search(r"hit_settings_row\(x, y, 6\) == 1 then\s*\n\s*start_transition\(ST_PRIVACY\)",
                     touch_settings), "строка приватности не открывает экран"
    touch_menu = function_body(menu_input, "touch_menu")
    assert "ST_PRIVACY" in touch_menu and "back_hit" in touch_menu
    assert re.search(r"if game_state == ST_PRIVACY then\s*\n\s*start_transition\(ST_SETTINGS\)",
                     back), "«Назад» с экрана приватности не ведёт в настройки"
    draw_screen = function_body(engine, "draw_screen")
    assert "draw_privacy()" in draw_screen

    # --- texts in both languages ------------------------------------------
    for name in ["tr_legal_accept", "tr_privacy", "tr_privacy_title"] + \
                ["tr_pv%d" % i for i in range(1, 11)]:
        body = function_body(locale, name)
        assert "language == 1" in body, "%s: нет русской ветки" % name
        returns = re.findall(r'return "([^"]+)"', body)
        assert len(returns) == 2 and returns[0] and returns[1], \
            "%s: нужны RU и EN строки" % name
    accept_ru = function_body(locale, "tr_legal_accept")
    assert "риск" in accept_ru and "risk" in accept_ru

    # --- manifest permissions: network only -------------------------------
    perms = re.findall(r'uses-permission android:name="android\.permission\.([A-Z_]+)"', manifest)
    assert perms == ["INTERNET", "ACCESS_NETWORK_STATE"], perms

    print("ИТОГ: гейт согласия об эпилепсии, метка в settings.dat, экран "
          "приватности и состав разрешений на месте")
    return 0


if __name__ == "__main__":
    sys.exit(main())
