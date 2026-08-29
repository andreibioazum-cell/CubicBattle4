/* sound.c — звуки и музыка для DimScript.
 *
 * Файлы лежат в game/sounds (в APK — assets/sounds).
 * Поддерживается несжатый PCM WAV
 * (8/16/24/32-бит, моно или стерео, любая частота — микшер сам пересчитает
 * линейной интерполяцией в 44100 Гц стерео).
 *
 * Встроенные функции скрипта:
 *   snd_load name      — загрузить WAV из папки звуков (1 = удалось, 0 = нет);
 *   snd_play name      — проиграть один раз;
 *   snd_loop name      — проиграть по кругу (музыка);
 *   snd_stop name      — остановить;
 *   snd_playing name   — 1, если звук сейчас играет;
 *   snd_volume name, v — громкость этого звука (0..1);
 *   snd_stop_all       — остановить всё.
 *
 * Вывод звука: Android — AudioTrack через JNI (никаких лишних библиотек).
 */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define SND_RATE 44100
#define SND_MAX_SOUNDS 16
#define SND_VOICES 8
#define SND_MAX_BYTES (64u << 20) /* лимит одного WAV: 64 МБ */

typedef struct {
    char *name;          /* ключ: имя файла без папки ("lobbymusic.wav") */
    int16_t *samples;    /* interleaved stereo, частота src_rate */
    size_t frames;
    int src_rate;
    double volume;
} SndSound;

typedef struct {
    SndSound *snd;
    double pos;          /* позиция в кадрах (дробная — для ресемплинга) */
    int loop;
    int active;
} SndVoice;

static SndSound snd_sounds[SND_MAX_SOUNDS];
static SndVoice snd_voices[SND_VOICES];
static int snd_sounds_loaded = 0;

/* ------------------------------------------------------------------ */
/* блокировка: громкость/запуск меняет поток игры, микширует поток аудио */
/* ------------------------------------------------------------------ */
#include <pthread.h>
static pthread_mutex_t snd_lock = PTHREAD_MUTEX_INITIALIZER;
static void snd_lock_take(void) { pthread_mutex_lock(&snd_lock); }
static void snd_lock_drop(void) { pthread_mutex_unlock(&snd_lock); }

#ifdef __ANDROID__
static AAssetManager *snd_amgr = NULL;
#endif

static char *snd_strdup(const char *s) {
    if (!s) s = "";
    size_t n = strlen(s) + 1;
    char *c = (char *)malloc(n);
    if (c) memcpy(c, s, n);
    return c;
}

/* ------------------------------------------------------------------ */
/* чтение файла звука: Android — ассеты из APK */
/* ------------------------------------------------------------------ */
static int snd_open_candidate(const char *path, uint8_t **out, size_t *sz) {
    if (!snd_amgr || !path || !out || !sz) return 0;
    AAsset *a = AAssetManager_open(snd_amgr, path, AASSET_MODE_BUFFER);
    if (!a) return 0;
    off_t len = AAsset_getLength(a);
    if (len <= 0 || (unsigned long)len > SND_MAX_BYTES) { AAsset_close(a); return 0; }
    uint8_t *buf = (uint8_t *)malloc((size_t)len);
    if (!buf) { AAsset_close(a); return 0; }
    size_t off = 0;
    while (off < (size_t)len) {
        int nr = AAsset_read(a, buf + off, (size_t)len - off);
        if (nr <= 0) break;
        off += (size_t)nr;
    }
    AAsset_close(a);
    if (off != (size_t)len) { free(buf); return 0; }
    *out = buf; *sz = off;
    return 1;
}

/* Имя звука может прийти как "lobbymusic.wav", "sounds/lobbymusic.wav" или
 * "game/sounds/lobbymusic.wav" — приводим к голому имени файла. */
static const char *snd_base_name(const char *name) {
    if (!name) return "";
    const char *n = name;
    while (strncmp(n, "./", 2) == 0) n += 2;
    if (strncmp(n, "game/sounds/", 12) == 0) n += 12;
    else if (strncmp(n, "sounds/", 7) == 0) n += 7;
    else if (strncmp(n, "game/assets/sounds/", 19) == 0) n += 19;
    else if (strncmp(n, "assets/sounds/", 14) == 0) n += 14;
    const char *slash = strrchr(n, '/');
    if (slash) n = slash + 1;
    if (!*n || strchr(n, '\\')) return "";
    return n;
}

static int snd_open_by_name(const char *base, uint8_t **out, size_t *sz) {
    char path[1200];
    /* На случай опечатки ".waw" пробуем и вариант с ".wav". */
    size_t n = strlen(base);
    int has_waw = n > 4 && strcmp(base + n - 4, ".waw") == 0;
    snprintf(path, sizeof(path), "sounds/%s", base);
    if (snd_open_candidate(path, out, sz)) return 1;
    if (has_waw) {
        snprintf(path, sizeof(path), "sounds/%.*s.wav", (int)(n - 4), base);
        if (snd_open_candidate(path, out, sz)) return 1;
    }
    snprintf(path, sizeof(path), "game/sounds/%s", base);
    if (snd_open_candidate(path, out, sz)) return 1;
    if (has_waw) {
        snprintf(path, sizeof(path), "game/sounds/%.*s.wav", (int)(n - 4), base);
        if (snd_open_candidate(path, out, sz)) return 1;
    }
    snprintf(path, sizeof(path), "%s", base);
    if (snd_open_candidate(path, out, sz)) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* разбор PCM WAV -> int16 stereo                                      */
/* ------------------------------------------------------------------ */
static uint32_t rd_u32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static int wav_decode(const uint8_t *data, size_t size, int16_t **out, size_t *frames, int *rate) {
    if (!data || size < 12) return 0;
    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) return 0;
    int fmt_ok = 0, channels = 0, bits = 0, hz = 0;
    const uint8_t *audio = NULL;
    size_t audio_len = 0;
    size_t pos = 12;
    while (pos + 8 <= size) {
        const uint8_t *id = data + pos;
        uint32_t csz = rd_u32(data + pos + 4);
        size_t body = pos + 8;
        size_t avail = size - body;
        size_t take = (size_t)csz < avail ? (size_t)csz : avail;
        if (memcmp(id, "fmt ", 4) == 0 && take >= 16) {
            uint16_t format = rd_u16(data + body);
            channels = rd_u16(data + body + 2);
            hz = (int)rd_u32(data + body + 4);
            bits = rd_u16(data + body + 14);
            /* WAVE_FORMAT_EXTENSIBLE: настоящий формат лежит в подформате GUID. */
            if (format == 0xFFFE && take >= 40) {
                uint16_t sub = rd_u16(data + body + 24);
                if (sub == 1) format = 1;
            }
            if (format != 1) { ds_log_err("sound not loaded: only uncompressed PCM WAV is supported"); return 0; }
            if (channels < 1 || channels > 2) { ds_log_err("sound not loaded: only mono or stereo WAV is supported"); return 0; }
            if (bits != 8 && bits != 16 && bits != 24 && bits != 32) { ds_log_err("sound not loaded: WAV must be 8/16/24/32-bit PCM"); return 0; }
            if (hz < 4000 || hz > 192000) { ds_log_err("sound not loaded: strange sample rate %d", hz); return 0; }
            fmt_ok = 1;
        } else if (memcmp(id, "data", 4) == 0) {
            audio = data + body;
            audio_len = take;
        }
        size_t next = body + ((csz + 1) & ~(uint32_t)1);
        if (next <= pos) break; /* защита от кривого размера чанка */
        pos = next;
    }
    if (!fmt_ok || !audio || audio_len == 0) { ds_log_err("sound not loaded: broken WAV file (no fmt/data chunk)"); return 0; }
    int bps = bits / 8;
    if (audio_len / (size_t)(bps * channels) == 0) { ds_log_err("sound not loaded: WAV has no samples"); return 0; }
    size_t n = audio_len / (size_t)(bps * channels);
    if (n > SND_MAX_BYTES / 4) { ds_log_err("sound not loaded: WAV too long"); return 0; }
    int16_t *samples = (int16_t *)malloc(n * 2 * sizeof(int16_t));
    if (!samples) { ds_log_err("sound not loaded: out of memory"); return 0; }
    for (size_t i = 0; i < n; i++) {
        const uint8_t *p = audio + i * (size_t)(bps * channels);
        int l = 0, r = 0;
        if (bits == 8) {
            l = ((int)p[0] - 128) << 8;
            if (channels == 2) r = ((int)p[1] - 128) << 8; else r = l;
        } else if (bits == 16) {
            l = (int16_t)rd_u16(p);
            if (channels == 2) r = (int16_t)rd_u16(p + 2); else r = l;
        } else if (bits == 24) {
            l = ((int)p[0] << 8 | (int)p[1] << 16 | (int)p[2] << 24) >> 16;
            if (channels == 2) { const uint8_t *q = p + 3; r = ((int)q[0] << 8 | (int)q[1] << 16 | (int)q[2] << 24) >> 16; }
            else r = l;
        } else {
            l = (int)((int32_t)rd_u32(p) >> 16);
            if (channels == 2) r = (int)((int32_t)rd_u32(p + 4) >> 16); else r = l;
        }
        samples[i * 2] = (int16_t)l;
        samples[i * 2 + 1] = (int16_t)r;
    }
    *out = samples; *frames = n; *rate = hz;
    return 1;
}

/* ------------------------------------------------------------------ */
/* загрузка/поиск звуков                                               */
/* ------------------------------------------------------------------ */
static SndSound *snd_find(const char *base) {
    for (int i = 0; i < SND_MAX_SOUNDS; i++)
        if (snd_sounds[i].name && strcmp(snd_sounds[i].name, base) == 0)
            return &snd_sounds[i];
    return NULL;
}

int snd_load(const char *name) {
    const char *base = snd_base_name(name);
    if (!*base) { ds_log_err("sound not loaded: bad name '%s'", name ? name : "(null)"); return 0; }
    snd_lock_take();
    if (snd_find(base)) { snd_lock_drop(); return 1; }
    int slot = -1;
    for (int i = 0; i < SND_MAX_SOUNDS; i++) if (!snd_sounds[i].name) { slot = i; break; }
    if (slot < 0) { snd_lock_drop(); ds_log_err("sound not loaded: too many sounds (max %d)", SND_MAX_SOUNDS); return 0; }
    /* Читаем файл вне блокировки: музыка бывает большой, микшер не должен
     * ждать загрузки. Слот сразу помечаем занятым (name), чтобы двойной
     * вызов snd_load не загрузил файл дважды. */
    char *key = snd_strdup(base);
    if (!key) { snd_lock_drop(); ds_log_err("sound not loaded: out of memory"); return 0; }
    snd_sounds[slot].name = key;
    snd_lock_drop();
    uint8_t *data = NULL; size_t size = 0;
    if (!snd_open_by_name(base, &data, &size)) {
        snd_lock_take();
        free(snd_sounds[slot].name); snd_sounds[slot].name = NULL;
        snd_lock_drop();
        free(data);
        ds_log_err("sound not loaded: '%s' not found in game/sounds", base);
        return 0;
    }
    int16_t *samples = NULL; size_t frames = 0; int rate = 0;
    int ok = wav_decode(data, size, &samples, &frames, &rate);
    free(data);
    snd_lock_take();
    if (!ok) { free(snd_sounds[slot].name); snd_sounds[slot].name = NULL; snd_lock_drop(); return 0; }
    snd_sounds[slot].samples = samples;
    snd_sounds[slot].frames = frames;
    snd_sounds[slot].src_rate = rate > 0 ? rate : SND_RATE;
    snd_sounds[slot].volume = 1.0;
    snd_sounds_loaded = 1;
    snd_lock_drop();
    return 1;
}

static SndVoice *snd_voice_for(SndSound *s) {
    for (int i = 0; i < SND_VOICES; i++) if (snd_voices[i].active && snd_voices[i].snd == s) return &snd_voices[i];
    for (int i = 0; i < SND_VOICES; i++) if (!snd_voices[i].active) return &snd_voices[i];
    return &snd_voices[0]; /* перекрываем самый первый голос */
}

static int snd_start(const char *name, int loop) {
    const char *base = snd_base_name(name);
    if (!*base) return 0;
    snd_lock_take();
    SndSound *s = snd_find(base);
    if (!s) { snd_lock_drop(); return 0; }
    SndVoice *v = snd_voice_for(s);
    v->snd = s; v->pos = 0; v->loop = loop; v->active = 1;
    snd_lock_drop();
    return 1;
}

int snd_play(const char *name) { return snd_start(name, 0); }
int sound_play(const char *name) { return snd_play(name); }
int snd_loop(const char *name) { return snd_start(name, 1); }

void snd_stop(const char *name) {
    const char *base = snd_base_name(name);
    if (!*base) return;
    snd_lock_take();
    SndSound *s = snd_find(base);
    if (s) for (int i = 0; i < SND_VOICES; i++) if (snd_voices[i].active && snd_voices[i].snd == s) snd_voices[i].active = 0;
    snd_lock_drop();
}

int snd_playing(const char *name) {
    const char *base = snd_base_name(name);
    if (!*base) return 0;
    snd_lock_take();
    SndSound *s = snd_find(base);
    int playing = 0;
    if (s) for (int i = 0; i < SND_VOICES; i++) if (snd_voices[i].active && snd_voices[i].snd == s) { playing = 1; break; }
    snd_lock_drop();
    return playing;
}

void snd_volume(const char *name, double volume) {
    const char *base = snd_base_name(name);
    if (!*base) return;
    if (volume < 0) volume = 0;
    if (volume > 1) volume = 1;
    snd_lock_take();
    SndSound *s = snd_find(base);
    if (s) s->volume = volume;
    snd_lock_drop();
}

void snd_stop_all(void) {
    snd_lock_take();
    for (int i = 0; i < SND_VOICES; i++) snd_voices[i].active = 0;
    snd_lock_drop();
}

/* ------------------------------------------------------------------ */
/* микшер: сумма активных голосов, линейный ресемплинг в SND_RATE       */
/* ------------------------------------------------------------------ */
static int snd_clamp16(int v) { if (v < -32768) return -32768; if (v > 32767) return 32767; return v; }

static void snd_frame(int16_t *out, int frames) {
    if (!out || frames <= 0) return;
    memset(out, 0, (size_t)frames * 2 * sizeof(int16_t));
    snd_lock_take();
    for (int vi = 0; vi < SND_VOICES; vi++) {
        SndVoice *v = &snd_voices[vi];
        if (!v->active || !v->snd || !v->snd->samples || v->snd->frames == 0) continue;
        SndSound *s = v->snd;
        double step = (double)s->src_rate / (double)SND_RATE;
        if (step <= 0) step = 1;
        double vol = s->volume;
        for (int i = 0; i < frames; i++) {
            if (v->pos >= (double)s->frames) {
                if (v->loop && s->frames > 1) v->pos -= (double)s->frames;
                else { v->active = 0; break; }
            }
            size_t i0 = (size_t)v->pos;
            double frac = v->pos - (double)i0;
            size_t i1 = i0 + 1;
            if (i1 >= s->frames) i1 = i0;
            int l = (int)((1.0 - frac) * s->samples[i0 * 2] + frac * s->samples[i1 * 2]);
            int r = (int)((1.0 - frac) * s->samples[i0 * 2 + 1] + frac * s->samples[i1 * 2 + 1]);
            out[i * 2] = (int16_t)snd_clamp16(out[i * 2] + (int)(l * vol));
            out[i * 2 + 1] = (int16_t)snd_clamp16(out[i * 2 + 1] + (int)(r * vol));
            v->pos += step;
        }
    }
    snd_lock_drop();
}

static void snd_release_sounds(void) {
    snd_lock_take();
    for (int i = 0; i < SND_MAX_SOUNDS; i++) {
        free(snd_sounds[i].name); snd_sounds[i].name = NULL;
        free(snd_sounds[i].samples); snd_sounds[i].samples = NULL;
        snd_sounds[i].frames = 0; snd_sounds[i].src_rate = SND_RATE; snd_sounds[i].volume = 1;
    }
    for (int i = 0; i < SND_VOICES; i++) { snd_voices[i].active = 0; snd_voices[i].snd = NULL; snd_voices[i].pos = 0; }
    snd_sounds_loaded = 0;
    snd_lock_drop();
}

/* ------------------------------------------------------------------ */
/* Android: система AudioTrack через JNI. Специально без OpenSL ES и   */
/* AAudio: им нужны флаги линковки (-lOpenSLES/-laaudio), а JNI уже    */
/* доступен через JavaVM из NativeActivity — сборка не требует ничего. */
/* ------------------------------------------------------------------ */
#ifdef __ANDROID__
#include <jni.h>
#include <pthread.h>

#define AT_FRAMES 1024          /* кадров в одном write() (23 мс) */
#define AT_REFILL 4             /* сколько minbuf держим в очереди устройства */

static JavaVM *snd_vm = NULL;
static jobject snd_at_track = NULL;   /* global ref на AudioTrack */
static jshortArray snd_at_buf = NULL; /* global ref на буфер write() */
static jmethodID snd_at_write = NULL;
static int16_t snd_at_mixbuf[AT_FRAMES * 2];
static pthread_t snd_at_thread;
static volatile int snd_at_run = 0;
static int snd_at_thread_ok = 0;

static JNIEnv *snd_jni_env(int *attached) {
    JNIEnv *env = NULL;
    if (attached) *attached = 0;
    if (!snd_vm) return NULL;
    if ((*snd_vm)->GetEnv(snd_vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*snd_vm)->AttachCurrentThread(snd_vm, &env, NULL) != JNI_OK) return NULL;
        if (attached) *attached = 1;
    }
    return env;
}

static void snd_jni_detach(int attached) {
    if (attached && snd_vm) (*snd_vm)->DetachCurrentThread(snd_vm);
}

static void *snd_at_thread_fn(void *arg) {
    (void)arg;
    int attached = 0;
    JNIEnv *env = snd_jni_env(&attached);
    if (!env) { ds_log_err("audio: thread cannot attach to JVM"); return NULL; }
    while (snd_at_run) {
        /* write() в MODE_STREAM блокируется, когда буфер устройства полон, —
         * это и задаёт темп потоку, без sleep. */
        jint written = (*env)->CallIntMethod(env, snd_at_track, snd_at_write, snd_at_buf, 0, (jint)(AT_FRAMES * 2));
        if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); ds_log_err("audio: AudioTrack.write failed"); break; }
        if (written < 0) break;
        /* Буфер уезжает в систему целиком, поэтому микшируем ПОСЛЕ write,
         * а первый блок заполнен заранее (в snd_backend_start). */
        if (!snd_at_run) break;
        snd_frame((int16_t *)snd_at_mixbuf, AT_FRAMES);
        (*env)->SetShortArrayRegion(env, snd_at_buf, 0, (jsize)(AT_FRAMES * 2), snd_at_mixbuf);
        if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); break; }
    }
    snd_jni_detach(attached);
    return NULL;
}

static int snd_backend_start(void) {
    if (snd_at_track) return 1;
    if (!snd_vm) { ds_log_err("audio: no JavaVM (ds_sound_set_java_vm not called)"); return 0; }
    int attached = 0;
    JNIEnv *env = snd_jni_env(&attached);
    if (!env) { ds_log_err("audio: cannot attach to JVM"); return 0; }
    jobject track = NULL;
    do {
        if ((*env)->PushLocalFrame(env, 16) != 0) break;
        jclass atc = (*env)->FindClass(env, "android/media/AudioTrack");
        if (!atc || (*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); ds_log_err("audio: AudioTrack class not found"); break; }
        /* STREAM_MUSIC=3, CHANNEL_OUT_STEREO=12, ENCODING_PCM_16BIT=2, MODE_STREAM=1 */
        jint minbuf = (*env)->CallStaticIntMethod(env, atc,
            (*env)->GetStaticMethodID(env, atc, "getMinBufferSize", "(III)I"),
            (jint)SND_RATE, (jint)12, (jint)2);
        if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); minbuf = 0; }
        if (minbuf < 4096) minbuf = 4096;               /* страховка от 0/-1 */
        if (minbuf > SND_RATE * 8) minbuf = SND_RATE * 8; /* и от гигантских */
        track = (*env)->NewObject(env, atc,
            (*env)->GetMethodID(env, atc, "<init>", "(IIIIII)V"),
            (jint)3, (jint)SND_RATE, (jint)12, (jint)2, (jint)minbuf * AT_REFILL, (jint)1);
        if (!track || (*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); ds_log_err("audio: AudioTrack create failed"); track = NULL; break; }
        (*env)->CallVoidMethod(env, track, (*env)->GetMethodID(env, atc, "play", "()V"));
        if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); ds_log_err("audio: AudioTrack.play failed"); track = NULL; break; }
        snd_at_write = (*env)->GetMethodID(env, atc, "write", "([SII)I");
        if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); track = NULL; break; }
        snd_at_track = (*env)->NewGlobalRef(env, track);
        snd_at_buf = (*env)->NewGlobalRef(env, (*env)->NewShortArray(env, AT_FRAMES * 2));
        if (!snd_at_track || !snd_at_buf) {
            ds_log_err("audio: global refs failed");
            if (snd_at_track) { (*env)->DeleteGlobalRef(env, snd_at_track); snd_at_track = NULL; }
            if (snd_at_buf) { (*env)->DeleteGlobalRef(env, snd_at_buf); snd_at_buf = NULL; }
            snd_at_write = NULL;
            break;
        }
        /* Первый блок уходит сразу, дальше поток держит очередь заполненной. */
        snd_frame((int16_t *)snd_at_mixbuf, AT_FRAMES);
        (*env)->SetShortArrayRegion(env, snd_at_buf, 0, (jsize)(AT_FRAMES * 2), snd_at_mixbuf);
        snd_at_run = 1;
        if (pthread_create(&snd_at_thread, NULL, snd_at_thread_fn, NULL) != 0) {
            ds_log_err("audio: cannot start audio thread");
            snd_at_run = 0;
            (*env)->DeleteGlobalRef(env, snd_at_track); snd_at_track = NULL;
            (*env)->DeleteGlobalRef(env, snd_at_buf); snd_at_buf = NULL;
        } else {
            snd_at_thread_ok = 1;
            ds_log("audio: AudioTrack started (minbuf %d bytes)", (int)minbuf);
        }
    } while (0);
    if (track) (*env)->DeleteLocalRef(env, track);
    (*env)->PopLocalFrame(env, NULL);
    snd_jni_detach(attached);
    return snd_at_track ? 1 : 0;
}

static void snd_backend_stop(void) {
    if (!snd_at_track) return;
    int attached = 0;
    JNIEnv *env = snd_jni_env(&attached);
    /* Если трек на паузе (приложение теряло фокус), write() в аудиопотоке
     * заблокирован навсегда: сначала возвращаем play(), чтобы буфер
     * опустел и поток смог увидеть флаг остановки. */
    if (env) {
        jclass atc0 = (*env)->GetObjectClass(env, snd_at_track);
        (*env)->CallVoidMethod(env, snd_at_track, (*env)->GetMethodID(env, atc0, "play", "()V"));
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    }
    snd_at_run = 0;
    if (snd_at_thread_ok) { pthread_join(snd_at_thread, NULL); snd_at_thread_ok = 0; }
    if (env) {
        jclass atc = (*env)->GetObjectClass(env, snd_at_track);
        (*env)->CallVoidMethod(env, snd_at_track, (*env)->GetMethodID(env, atc, "stop", "()V"));
        (*env)->CallVoidMethod(env, snd_at_track, (*env)->GetMethodID(env, atc, "release", "()V"));
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        (*env)->DeleteGlobalRef(env, snd_at_track);
        (*env)->DeleteGlobalRef(env, snd_at_buf);
    }
    snd_at_track = NULL;
    snd_at_buf = NULL;
    snd_at_write = NULL;
    if (env) snd_jni_detach(attached);
}

static void snd_backend_pause(void) {
    if (!snd_at_track) return;
    int attached = 0;
    JNIEnv *env = snd_jni_env(&attached);
    if (!env) return;
    jclass atc = (*env)->GetObjectClass(env, snd_at_track);
    (*env)->CallVoidMethod(env, snd_at_track, (*env)->GetMethodID(env, atc, "pause", "()V"));
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    snd_jni_detach(attached);
}

static void snd_backend_resume(void) {
    if (!snd_at_track) return;
    int attached = 0;
    JNIEnv *env = snd_jni_env(&attached);
    if (!env) return;
    jclass atc = (*env)->GetObjectClass(env, snd_at_track);
    (*env)->CallVoidMethod(env, snd_at_track, (*env)->GetMethodID(env, atc, "play", "()V"));
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    snd_jni_detach(attached);
}

#endif

/* ------------------------------------------------------------------ */
/* жизненный цикл: вызывается из main.c (Android)                       */
/* ------------------------------------------------------------------ */
void ds_sound_set_java_vm(void *vm) {
    snd_vm = (JavaVM *)vm;
}

int ds_sound_init(AAssetManager *assets) {
    snd_amgr = assets;
    return snd_backend_start();
}

void ds_sound_shutdown(void) {
    snd_backend_stop();
    snd_release_sounds();
}

void ds_sound_pause(void) { snd_backend_pause(); }
void ds_sound_resume(void) { snd_backend_resume(); }
