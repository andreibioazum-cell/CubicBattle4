#!/usr/bin/env python3
"""Native promo checks: random card codes, promo.dat and the cloud sync.

Compiles the REAL net.c (the same translation unit the Android build uses,
with temporary Android/JNI header stubs — this is not a PC build) and runs it
against a real temporary data directory:

  write  — every card gets a fresh random code of 3 letters and 1 digit (no I,
           O, 0 or 1), never the same code twice in a row, spread over many
           values and digit positions; only the code of the last card redeems;
  read   — a fresh process ("restart") still knows the code of the last card;
           taking the reward spends it and sets the one-per-account flag;
  cloud  — on a clean device a card code found on another phone is adopted,
           promo_used=1 from the cloud blocks a second reward, and an old
           profile value that is not a card code is ignored;
  legacy — a promo.dat of an older version (streak, card_found) still reads.

Requires a host C compiler (CC).
"""
from pathlib import Path
import os
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

ANDROID_LOG_H = """
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_ERROR 6
int __android_log_print(int prio, const char *tag, const char *fmt, ...);
"""

JNI_H = """
#ifndef STUB_JNI_H
#define STUB_JNI_H
typedef int jint; typedef unsigned char jbyte; typedef int jsize; typedef int jboolean;
#define JNI_OK 0
#define JNI_TRUE 1
#define JNI_FALSE 0
#define JNI_VERSION_1_6 0x00010006
typedef void *jobject; typedef void *jclass; typedef void *jstring;
typedef void *jbyteArray; typedef void *jthrowable; typedef void *jmethodID;
struct JNINativeInterface {
    void *reserved0;
    jclass (*FindClass)(void *env, const char *name);
    jobject (*NewObject)(void *env, jclass cls, jmethodID id, ...);
    jclass (*GetObjectClass)(void *env, jobject obj);
    jmethodID (*GetMethodID)(void *env, jclass cls, const char *name, const char *sig);
    jobject (*CallObjectMethod)(void *env, jobject obj, jmethodID id, ...);
    jint (*CallIntMethod)(void *env, jobject obj, jmethodID id, ...);
    void (*CallVoidMethod)(void *env, jobject obj, jmethodID id, ...);
    jstring (*NewStringUTF)(void *env, const char *bytes);
    const char *(*GetStringUTFChars)(void *env, jstring s, jboolean *is_copy);
    void (*ReleaseStringUTFChars)(void *env, jstring s, const char *bytes);
    jbyteArray (*NewByteArray)(void *env, jsize len);
    void (*SetByteArrayRegion)(void *env, jbyteArray a, jsize start, jsize len, const jbyte *buf);
    void (*GetByteArrayRegion)(void *env, jbyteArray a, jsize start, jsize len, jbyte *buf);
    void (*ExceptionClear)(void *env);
    jboolean (*ExceptionCheck)(void *env);
    jthrowable (*ExceptionOccurred)(void *env);
    int (*PushLocalFrame)(void *env, jint capacity);
    jobject (*PopLocalFrame)(void *env, jobject result);
};
struct JNIInvokeInterface {
    void *reserved0;
    int (*AttachCurrentThread)(void *vm, void *env, void *args);
    int (*DetachCurrentThread)(void *vm);
    int (*GetEnv)(void *vm, void *env, int version);
};
typedef const struct JNINativeInterface *JNIEnv;
typedef const struct JNIInvokeInterface *JavaVM;
#endif
"""

# The test includes the real net.c, so it also sees the static functions of the
# module (promo_sync_with_cloud, the lg session).
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

static int format_ok(const char *c) {
    int l = 0, d = 0;
    if (strlen(c) != 4) return 0;
    for (int i = 0; i < 4; i++) {
        if (c[i] >= 'A' && c[i] <= 'Z' && c[i] != 'I' && c[i] != 'O') l++;
        else if (c[i] >= '2' && c[i] <= '9') d++;
        else return 0;
    }
    return l == 3 && d == 1;
}

static int run_write(const char *dir) {
    net_set_data_path(dir);
    assert(strcmp(net_promo_code(), "") == 0);   /* no card yet, no code */
    assert(net_promo_check("ABC7") == 0);        /* a well-formed guess is wrong */
    assert(net_promo_used() == 0);
    /* Random codes: valid, never repeated back to back, spread out. */
    char prev[8] = "", seen[400][8];
    int distinct = 0, pos_seen[4] = {0, 0, 0, 0};
    for (int n = 0; n < 400; n++) {
        const char *c = net_promo_new_code();
        assert(format_ok(c));
        assert(strcmp(c, prev) != 0);
        assert(strcmp(net_promo_code(), c) == 0);
        int dup = 0;
        for (int k = 0; k < distinct; k++) if (!strcmp(seen[k], c)) { dup = 1; break; }
        if (!dup) snprintf(seen[distinct++], 8, "%s", c);
        for (int i = 0; i < 4; i++) if (c[i] >= '2' && c[i] <= '9') pos_seen[i] = 1;
        snprintf(prev, sizeof(prev), "%s", c);
    }
    assert(distinct >= 390);
    assert(pos_seen[0] && pos_seen[1] && pos_seen[2] && pos_seen[3]);
    /* Only the code of the last card redeems. */
    const char *last = net_promo_code();
    assert(net_promo_check(last) == 1);
    assert(net_promo_check(seen[0]) == 0 || !strcmp(seen[0], last));
    char lower[8]; snprintf(lower, sizeof(lower), "%s", last);
    for (int i = 0; i < 4; i++) if (lower[i] >= 'A' && lower[i] <= 'Z') lower[i] += 32;
    assert(net_promo_check(lower) == 0);         /* the script upper-cases first */
    assert(net_promo_check("") == 0);
    printf("PROMO_CODE=%s\n", last);
    printf("promo write: %d distinct random codes out of 400, digit in every position\n", distinct);
    return 0;
}

static int run_read(const char *dir, const char *code) {
    net_set_data_path(dir);
    assert(strcmp(net_promo_code(), code) == 0); /* the restart keeps the last card */
    assert(net_promo_check(code) == 1);
    assert(net_promo_used() == 0);
    net_promo_mark_used();                       /* the reward is taken */
    assert(net_promo_used() == 1);
    assert(net_promo_check(code) == 0);          /* the code is spent */
    assert(strcmp(net_promo_code(), "") == 0);
    /* A later card still shows a new random code, the flag stays. */
    assert(format_ok(net_promo_new_code()));
    assert(net_promo_used() == 1);
    puts("promo read: the last card survives a restart, the reward spends it once");
    return 0;
}

static void login(const char *nick) {
    lg_lock();
    lg.status = NET_LOGIN_OK;
    snprintf(lg.session_nick, sizeof(lg.session_nick), "%s", nick);
    lg_unlock();
}

/* Clean device: the cloud knows the card found on another phone. */
static int run_cloud(const char *dir) {
    net_set_data_path(dir);
    assert(promo_sync_with_cloud("{\"nick\":\"tester\",\"promo\":\"KXM7\"}") == 0); /* no session */
    login("tester");
    /* An old profile value that is not a card code is ignored. */
    assert(promo_sync_with_cloud("{\"nick\":\"tester\",\"promo\":\"CB4-1A2B-3C4D-5E6F\"}") == 0);
    assert(strcmp(net_promo_code(), "") == 0);
    assert(promo_sync_with_cloud("{\"nick\":\"tester\",\"promo\":\"KXM7\",\"promo_used\":0}") == 1);
    assert(strcmp(net_promo_code(), "KXM7") == 0);
    assert(net_promo_check("KXM7") == 1);
    /* The reward was taken on the other phone: no second one here. */
    assert(promo_sync_with_cloud("{\"nick\":\"tester\",\"promo\":\"KXM7\",\"promo_used\":1}") == 1);
    assert(net_promo_used() == 1);
    assert(net_promo_check("KXM7") == 0);
    puts("promo cloud: a card code from another phone is adopted, promo_used blocks a second reward");
    return 0;
}

/* promo.dat of an older version: streak and card_found are ignored. */
static int run_legacy(const char *dir) {
    net_set_data_path(dir);
    assert(net_promo_used() == 0);
    assert(strcmp(net_promo_code(), "") == 0);
    puts("promo legacy: an old promo.dat still reads");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: test <write|read|cloud|legacy> <dir> [code]\n"); return 2; }
    if (strcmp(argv[1], "write") == 0) return run_write(argv[2]);
    if (strcmp(argv[1], "read") == 0 && argc > 3) return run_read(argv[2], argv[3]);
    if (strcmp(argv[1], "cloud") == 0) return run_cloud(argv[2]);
    if (strcmp(argv[1], "legacy") == 0) return run_legacy(argv[2]);
    fprintf(stderr, "unknown mode '%s'\n", argv[1]);
    return 2;
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="cubic-promo-") as directory:
        temp = Path(directory)
        android = temp / "android"
        android.mkdir()
        (android / "log.h").write_text(ANDROID_LOG_H)
        (android / "asset_manager.h").write_text(
            "typedef struct AAssetManager AAssetManager;\n")
        (android / "native_window.h").write_text("typedef struct ANativeWindow ANativeWindow;\n")
        (temp / "jni.h").write_text(JNI_H)
        (temp / "test.c").write_text(HARNESS)
        subprocess.run([
            *shlex.split(os.environ.get("CC", "cc")), "-std=gnu99", "-O0",
            "-D_POSIX_C_SOURCE=200809L", "-D__ANDROID__",
            "-Werror=implicit-function-declaration",
            "-I", str(temp), "-I", str(ROOT),
            str(temp / "test.c"), "-lm", "-lpthread", "-o", str(temp / "test"),
        ], check=True)

        data = temp / "data"
        data.mkdir()
        run = [str(temp / "test")]
        written = subprocess.run(
            [*run, "write", str(data)], check=True,
            capture_output=True, text=True).stdout
        sys.stdout.write(written)
        code = [l for l in written.splitlines() if "PROMO_CODE=" in l][0].split("=")[1].strip()
        assert len(code) == 4, code
        saved = (data / "promo.dat").read_text(encoding="utf-8")
        assert f"code {code}" in saved and "used 0" in saved, saved
        subprocess.run([*run, "read", str(data), code], check=True)
        spent = (data / "promo.dat").read_text(encoding="utf-8")
        assert "used 1" in spent and f"code {code}" not in spent, spent

        fresh = temp / "fresh-device"
        fresh.mkdir()
        subprocess.run([*run, "cloud", str(fresh)], check=True)
        adopted = (fresh / "promo.dat").read_text(encoding="utf-8")
        assert "used 1" in adopted, adopted

        old = temp / "old-version"
        old.mkdir()
        (old / "promo.dat").write_text("used 0\nstreak 7\ncard_found 1\n", encoding="utf-8")
        subprocess.run([*run, "legacy", str(old)], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
