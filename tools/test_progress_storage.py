#!/usr/bin/env python3
"""Native progress checks: progress.dat on the device and the cloud profile merge.

Compiles the REAL net.c (the same translation unit the Android build uses, with
temporary Android/JNI header stubs — this is not a PC build) and runs it several
times against a real temporary data directory. Each run is a process of its own,
so "restart" here is a real restart: only the files on the device survive.

The complaint this guards: the player buys the buK (150 candies), picks another
character, re-enters the game — and the buK is "not bought" again. Three things
must hold:

  buy     — buying the buK writes progress.dat (candies spent, ebuc=1, its level);
  restart — a fresh process reads that very file back, so the buK stays bought
            even when it is not the selected class (cls=0);
  merge   — an outdated cloud profile (ebuc=0) may NOT take the purchase away;
            the client reports "restored", which is what makes net_auth push the
            local state back to the cloud (`if (restored) push_pg_to_cloud()`);
  fresh   — on a device without progress.dat the cloud profile is the only
            source, and a profile that knows the buK opens it (with its level).

Requires a host C compiler (CC).
"""
from pathlib import Path
import os
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
# net.c в обоих тестах собирается с одними и теми же заглушками Android/JNI —
# держать их копию здесь незачем, а разъехаться копиям нельзя.
from test_settings_storage import ANDROID_LOG_H, JNI_H  # noqa: E402

# Девятнадцать значений прогресса: сколько леденцов ушло за бука, что он куплен
# и открыт его первый уровень. Обычный класс остаётся выбранным (cls=0) — ровно
# то состояние, в котором игрок закрывает игру, не оставшись за бука.
BUY_ARGS = ("(20, 5, 0, 1, 0, 1, 0, 0, "
            "0, 0, 0, 0, 0, 0, 1, 1, 0, 0)")

# Облако отстало: покупки в профиле ещё нет (запись не успела уйти, профиль
# пришёл со старого устройства), а леденцов в нём больше — валюта приходит из
# профиля, покупка — нет.
CLOUD_STALE = ("{\"nick\":\"tester\",\"cups\":40,\"candies\":200,\"cls\":0,"
               "\"azum\":1,\"santa\":0,\"ebuc\":0,\"level\":0,\"levels\":0}")

CLOUD_WITH_BUK = ("{\"nick\":\"tester\",\"cups\":40,\"candies\":200,\"cls\":3,"
                  "\"azum\":1,\"santa\":0,\"ebuc\":1,\"level\":1,\"levels\":1,"
                  "\"ebuc_level\":1,\"ebuc_levels\":1}")

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "net.c"

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    (void)prio; (void)tag; (void)fmt;
    return 0;
}
void ds_console_log(int is_error, const char *format, ...) { (void)is_error; (void)format; }

/* Восемнадцать значений прогресса: сколько леденцов ушло за бука, что он
 * куплен и открыт его первый уровень. Обычный класс остаётся выбранным (cls=0)
 * — ровно то состояние, в котором игрок закрывает игру, не оставшись за бука:
 * cups, candies, cls, azum, santa, ebuc, level, levels, 4x (level, levels), bp, skin. */
#define BUY_CALL net_save_progress_all(20, 5, 0, 1, 0, 1, 0, 0, \
                                       0, 0, 0, 0, 0, 0, 1, 1, 0, 0)

/* Облако отстало: покупки в профиле ещё нет (запись не успела уйти, профиль
 * пришёл со старого устройства), а леденцов в нём больше. */
static const char *CLOUD_STALE =
    "{\"nick\":\"tester\",\"cups\":40,\"candies\":200,\"cls\":0,"
    "\"azum\":1,\"santa\":0,\"ebuc\":0,\"level\":0,\"levels\":0}";

/* Профиль, который покупку помнит: с него поднимается класс на новом устройстве. */
static const char *CLOUD_WITH_BUK =
    "{\"nick\":\"tester\",\"cups\":40,\"candies\":200,\"cls\":3,"
    "\"azum\":1,\"santa\":0,\"ebuc\":1,\"level\":1,\"levels\":1,"
    "\"ebuc_level\":1,\"ebuc_levels\":1}";

static void expect_buk(const char *what) {
    if (net_load_ebuc() != 1 || net_load_ebuc_level() != 1 || net_load_ebuc_levels_unlocked() != 1) {
        fprintf(stderr, "%s: buk is gone (ebuc=%g level=%g levels=%g)\n", what,
                net_load_ebuc(), net_load_ebuc_level(), net_load_ebuc_levels_unlocked());
        exit(1);
    }
}

/* Покупка бука: скрипт вызывает net_save_progress_all с выбранным обычным
 * классом (cls=0) и купленным буком (ebuc=1). */
static int run_buy(const char *dir) {
    net_set_data_path(dir);
    BUY_CALL;
    expect_buk("buy");
    assert(net_load_candies() == 5);      /* 150 леденцов списаны */
    assert(net_load_class() == 0);        /* выбран не бук, а обычный класс */
    puts("native buy: buk and its level are written to progress.dat");
    return 0;
}

/* Перезапуск игры: новое состояние памяти, на устройстве — тот же файл. */
static int run_restart(const char *dir) {
    net_set_data_path(dir);
    expect_buk("restart");
    assert(net_load_candies() == 5);
    assert(net_load_class() == 0);
    puts("native restart: a fresh process still sees the bought buk");
    return 0;
}

/* Автологин по нику: профиль облака отстал и про бука не знает. Покупка
 * остаётся, возврат 1 — сигнал «после логина протолкнуть профиль в облако». */
static int run_merge(const char *dir) {
    net_set_data_path(dir);
    int restored = apply_user_json_keep_local(CLOUD_STALE);
    expect_buk("merge");
    assert(restored == 1);
    /* Валюта приходит из профиля: облако — источник правды по деньгам. */
    assert(net_load_candies() == 200);
    printf("native merge: cloud without the buk did not take it away\n");
    return 0;
}

/* Вход на чистом устройстве (переустановка, второй телефон): устройство
 * ничего не знает, класс открывает профиль. */
static int run_fresh(const char *dir) {
    net_set_data_path(dir);
    apply_user_json_keep_local(CLOUD_WITH_BUK);
    expect_buk("fresh login");
    assert(net_load_class() == 3);
    assert(net_load_candies() == 200);
    puts("native fresh: the cloud profile opens the buk on a new device");
    return 0;
}

/* Тот же чистый вход, но профиль отстал: восстанавливать нечего — именно для
 * этого случая клиент проталкивает локальный прогресс в облако при логине
 * (см. merge), чтобы переустановка не стоила игроку покупки. */
static int run_fresh_stale(const char *dir) {
    net_set_data_path(dir);
    apply_user_json_keep_local(CLOUD_STALE);
    assert(net_load_ebuc() == 0);
    assert(net_load_candies() == 200);
    puts("native fresh+stale cloud: nothing local, cloud profile has no buk");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: test <buy|restart|merge|fresh|fresh_stale> <dir>\n"); return 2; }
    if (strcmp(argv[1], "buy") == 0) return run_buy(argv[2]);
    if (strcmp(argv[1], "restart") == 0) return run_restart(argv[2]);
    if (strcmp(argv[1], "merge") == 0) return run_merge(argv[2]);
    if (strcmp(argv[1], "fresh") == 0) return run_fresh(argv[2]);
    if (strcmp(argv[1], "fresh_stale") == 0) return run_fresh_stale(argv[2]);
    fprintf(stderr, "unknown mode '%s'\n", argv[1]);
    return 2;
}
'''

# Поля progress.dat по порядку записи (см. native/net/progress_file.inc):
# кубки, класс, азум, санта, леденцы, уровень, открыто, 4x (уровень, открыто),
# уровень бука, открытые уровни бука, бук, батл пасс, скин.
FIELD = {
    "cups": 0, "cls": 1, "azum": 2, "santa": 3, "candies": 4,
    "ebuc_level": 13, "ebuc_levels": 14, "ebuc": 15,
}


def read_progress(dir_path):
    text = (dir_path / "progress.dat").read_text(encoding="utf-8")
    return [int(value) for value in text.split()]


def main():
    with tempfile.TemporaryDirectory(prefix="cubic-progress-") as directory:
        temp = Path(directory)
        android = temp / "android"
        android.mkdir()
        (android / "log.h").write_text(ANDROID_LOG_H)
        (android / "asset_manager.h").write_text("typedef struct AAssetManager AAssetManager;\n")
        (android / "native_window.h").touch()
        (temp / "jni.h").write_text(JNI_H)
        (temp / "test.c").write_text(HARNESS)
        subprocess.run([
            *shlex.split(os.environ.get("CC", "cc")), "-std=gnu99", "-O0",
            "-D_POSIX_C_SOURCE=200809L", "-D__ANDROID__",
            "-Werror=implicit-function-declaration",
            "-I", str(temp), "-I", str(ROOT),
            str(temp / "test.c"), "-lm", "-lpthread", "-o", str(temp / "test"),
        ], check=True)

        device = temp / "device"
        device.mkdir()
        run = [str(temp / "test")]
        subprocess.run([*run, "buy", str(device)], check=True)

        saved = read_progress(device)
        assert saved[FIELD["ebuc"]] == 1, saved
        assert saved[FIELD["cls"]] == 0, saved
        assert saved[FIELD["candies"]] == 5, saved
        assert saved[FIELD["ebuc_level"]] == 1, saved
        assert saved[FIELD["ebuc_levels"]] == 1, saved

        subprocess.run([*run, "restart", str(device)], check=True)
        subprocess.run([*run, "merge", str(device)], check=True)

        # После слияния с отставшим облаком файл перезаписан локальным
        # состоянием: бук на месте, иначе следующий запуск его снова потеряет.
        merged = read_progress(device)
        assert merged[FIELD["ebuc"]] == 1, merged
        assert merged[FIELD["ebuc_level"]] == 1, merged
        assert merged[FIELD["cls"]] == 0, merged

        subprocess.run([*run, "fresh", str(temp / "new-device")], check=True)
        subprocess.run([*run, "fresh_stale", str(temp / "stale-device")], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
