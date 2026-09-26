"""Language tables: types, built-in functions, engine variables, namespaces.

Everything the compiler knows about names that are not declared in a script
lives here, so adding a native function or an engine variable is a one-line
change in this file."""


TYPES = {
    'num': 'double',
    'number': 'double',
    'int': 'double',
    'float': 'double',
    'double': 'double',
    'str': 'const char*',
    'string': 'const char*',
    'bool': 'double',
    'col': 'uint32_t',
    'color': 'uint32_t',
    'arr': 'DSArray*',
    'array': 'DSArray*',
}

_TYPE_ALIAS = {
    'number': 'num',
    'int': 'num',
    'float': 'num',
    'double': 'num',
    'string': 'str',
    'bool': 'num',
    'color': 'col',
    'array': 'arr',
}


def canon_type(t):
    return _TYPE_ALIAS.get(t, t)


BUILTINS = frozenset({
    'rect', 'roundrect', 'rect_rot', 'circle', 'ring', 'line', 'tex', 'tex_tint', 'text',
    'text_scaled', 'text_ink_width', 'text_ink_height', 'text_ink_top',
    'png_load', 'clear_screen', 'text_width', 'text_height', 'sqrt', 'sin', 'cos',
    'atan2', 'floor', 'rand', 'snd_load', 'snd_play', 'snd_loop', 'snd_stop',
    'snd_playing', 'snd_volume', 'snd_stop_all', 'sound_play', 'net_connect',
    'net_disconnect', 'net_publish', 'net_publish_punch', 'net_publish_snow',
    'net_publish_station', 'net_publish_universe',
    'net_set_class', 'net_status', 'net_slot', 'net_player_online',
    'net_player_x', 'net_player_y', 'net_player_angle', 'net_player_hp',
    'net_player_alive', 'net_player_nick', 'net_player_punch_x',
    'net_player_punch_y', 'net_player_punch_dx', 'net_player_punch_dy',
    'net_player_punch', 'net_player_snow_x', 'net_player_snow_y',
    'net_player_snow_dx', 'net_player_snow_dy', 'net_player_snow',
    'net_player_station_x', 'net_player_station_y', 'net_player_station_hp',
    'net_player_station2_x', 'net_player_station2_y', 'net_player_station2_hp',
    'net_player_station3_x', 'net_player_station3_y', 'net_player_station3_hp',
    'net_player_station', 'net_player_universe_x', 'net_player_universe_y',
    'net_player_universe', 'net_publish_turrets', 'net_publish_dash',
    'net_player_dash', 'net_player_dash_x', 'net_player_dash_y',
    'net_player_dash_dx', 'net_player_dash_dy',
    'net_publish_thud', 'net_player_thud',
    'net_player_class', 'net_player_level', 'net_player_prime_level',
    'net_set_level', 'net_set_prime_level', 'net_set_skin', 'net_player_skin', 'net_event', 'net_event_set',
    'net_chat_send',
    'net_chat_trim', 'net_chat_count', 'net_chat_text', 'net_chat_uid',
    'net_chat_key',
    'net_is_banned', 'net_banned', 'net_ban_set', 'net_chat_is_ban',
    'net_chat_is_unban', 'net_chat_ban_target', 'net_chat_unban_target',
    'net_chat_is_text_cmd', 'net_chat_text_cmd_text', 'net_chat_text_cmd_color',
    'net_banner_send', 'net_banner_ts', 'net_banner_text', 'net_banner_color',
    'net_autologin', 'net_set_nick', 'net_login_status', 'net_login_nick',
    'net_set_firebase_key',
    'net_login_pass',
    'net_auth', 'net_logout', 'net_leaderboard_fetch', 'net_leaderboard_status',
    'net_leaderboard_count', 'net_leaderboard_nick', 'net_leaderboard_cups',
    'net_load_cups', 'net_load_candies', 'net_load_primes', 'net_load_class',
    'net_load_azum', 'net_load_santa', 'net_load_ebuc', 'net_load_level',
    'net_load_levels_unlocked', 'net_load_ordinary_level',
    'net_load_ordinary_levels_unlocked', 'net_load_azum_level',
    'net_load_azum_levels_unlocked', 'net_load_santa_level',
    'net_load_santa_levels_unlocked', 'net_load_ebuc_level',
    'net_load_ebuc_levels_unlocked', 'net_load_bp_level', 'net_load_azum_skin',
    'net_load_ordinary_prime_level',
    'net_load_azum_prime_level', 'net_load_santa_prime_level',
    'net_save_progress', 'net_save_progress_all', 'net_load_achievement_flags',
    'net_save_achievement_flags', 'net_has_achievement_flag',
    'net_mark_achievement_flag', 'net_load_azum_revives', 'net_save_azum_revives',
    'net_promo_code', 'net_promo_new_code', 'net_promo_check', 'net_promo_used',
    'net_promo_mark_used',
    'net_load_playtime', 'net_save_playtime', 'net_add_playtime',
    'net_save_quest_state', 'net_load_quest_state', 'net_quest_has_state',
    'net_load_language', 'net_load_hitboxes', 'net_save_settings',
    'net_load_music_volume', 'net_save_music_volume',
    'net_load_winter_theme', 'net_save_winter_theme',
    'net_load_fps_meter', 'net_save_fps_meter',
    'settings_mark_legal', 'settings_legal_ts',
    'alpha_notice_show',
    'keyboard_show',
    'keyboard_hide', 'keyboard_get_text', 'keyboard_get_raw', 'keyboard_clear',
    'keyboard_enter_pressed', 'keyboard_type', 'keyboard_visible', 'str_len',
    'str_eq', 'str_contains', 'str_index_of', 'str_sub', 'str_to_num', 'str_trim',
    'str_starts_with', 'str_ends_with', 'str_lower', 'str_upper',
    'ds_log', 'console_count', 'console_line', 'console_type',
    'console_clear', 'arr_new', 'arr_push', 'arr_get', 'arr_set', 'arr_len',
    'arr_clear', 'clamp', 'lerp', 'dist',
    # Maths for scripts. In C those names carry the ds_ prefix (see FUNCTION_MAP
    # and native/runtime/core.inc) so they do not clash with libc, where abs(),
    # round() and the like already exist with other signatures.
    'min', 'max', 'abs', 'round', 'sign', 'mod', 'trunc',
})

# Script names to C names. Empty means the name is the same.
FUNCTION_MAP = {
    'min': 'ds_min',
    'max': 'ds_max',
    'abs': 'ds_abs',
    'round': 'ds_round',
    'sign': 'ds_sign',
    'mod': 'ds_mod',
    'trunc': 'ds_trunc',
}

ENGINE_VARS = {
    'screen_w': 'num',
    'screen_h': 'num',
    'dt': 'num',
    'joy': 'joy',
    'mouse_clicked': 'num',
    'ds_mouse_x': 'num',
    'ds_mouse_y': 'num',
}

STR_BUILTINS = frozenset({
    'console_line',
    'keyboard_get_text',
    'keyboard_get_raw',
    'net_chat_text',
    'net_chat_uid',
    'net_chat_key',
    'net_chat_ban_target',
    'net_chat_unban_target',
    'net_chat_text_cmd_text',
    'net_chat_text_cmd_color',
    'net_banner_text',
    'net_banner_color',
    'net_promo_code',
    'net_promo_new_code',
    'net_login_nick',
    'net_login_pass',
    'net_player_nick',
    'net_leaderboard_nick',
    'str_sub',
    'str_trim',
    'str_lower',
    'str_upper',
})

# Built-in namespaces: import Dim.System.Graphics allows Graphics.rect(...),
# while the short rect(...) calls keep working.
STD_NAMESPACES = {
    'Dim.System.Graphics': 'Graphics',
    'Dim.System.Math': 'Math',
}
_MATH_NS = {'min', 'max', 'abs', 'round', 'sign', 'mod', 'trunc', 'clamp',
            'lerp', 'dist', 'sqrt', 'sin', 'cos', 'atan2', 'floor', 'rand'}
# Maths from math.h, which the generated game.c includes.
NATIVE_MATH = frozenset({'fabs'})
_MODS = ('public', 'private')
