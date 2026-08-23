/* audio.c — маленький звуковой движок ZeroHabit.
 *
 * Умеет ровно то, что нужно игре: щёлкать кнопкой и крутить фоновую мелодию
 * по кругу. Файлы лежат в game/assets/music (WAV, PCM 16 бит) и грузятся тем
 * же путём, что картинки, — через ds_asset_read, поэтому одинаково работают
 * из APK, из ресурсов Game.exe и из web-сборки.
 *
 * Бэкенды:
 *   Windows      — waveOut, свой поток и программный микшер;
 *   Android      — AAudio, тот же микшер в callback;
 *   Emscripten   — HTMLAudioElement поверх файла из MEMFS (микшер не нужен);
 *   всё остальное (в том числе headless-тест) — тихая заглушка.
 */
#include "runtime.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define DS_AUDIO_RATE 44100
#define DS_AUDIO_CLIPS 8
#define DS_AUDIO_VOICES 8

#if defined(__EMSCRIPTEN__)
/* ------------------------------------------------------------------ web -- */
#include <emscripten.h>

/* Файлы уже лежат в MEMFS (--preload-file), поэтому читаем их в Blob и
 * отдаём обычному <audio>. Автозапуск музыки браузеры глушат до первого
 * клика — тогда пробуем ещё раз по первому касанию страницы. */
EM_JS(void, ds_web_audio_play, (const char *name, double volume, int loop), {
    var file = UTF8ToString(name);
    Module.dsAudioUrls = Module.dsAudioUrls || {};
    if (!Module.dsAudioUrls[file]) {
        try {
            var bytes = FS.readFile('/assets/' + file);
            Module.dsAudioUrls[file] = URL.createObjectURL(new Blob([bytes], { type: 'audio/wav' }));
        } catch (e) { return; }
    }
    var url = Module.dsAudioUrls[file];
    if (loop) {
        if (Module.dsMusic) { Module.dsMusic.pause(); }
        var m = new Audio(url);
        m.loop = true;
        m.volume = Math.max(0, Math.min(1, volume));
        Module.dsMusic = m;
        var tryPlay = function () {
            var p = m.play();
            if (p && p.catch) p.catch(function () { });
        };
        tryPlay();
        window.addEventListener('pointerdown', tryPlay, { once: true });
    } else {
        var s = new Audio(url);
        s.volume = Math.max(0, Math.min(1, volume));
        var p = s.play();
        if (p && p.catch) p.catch(function () { });
    }
});
EM_JS(void, ds_web_audio_stop, (void), {
    if (Module.dsMusic) { Module.dsMusic.pause(); Module.dsMusic = null; }
});

int ds_audio_init(void) { return 1; }
void ds_audio_shutdown(void) { ds_web_audio_stop(); }
void sound_play(const char *name, double volume) {
    if (name) ds_web_audio_play(name, volume, 0);
}
void music_play(const char *name, double volume) {
    if (name) ds_web_audio_play(name, volume, 1);
}
void music_stop(void) { ds_web_audio_stop(); }

#elif defined(_WIN32) || defined(__ANDROID__)
/* ------------------------------------------- Windows и Android: микшер -- */

typedef struct {
    char name[96];
    int16_t *pcm;
    size_t frames;
    int channels;
    int rate;
} DSClip;

typedef struct {
    DSClip *clip;
    double pos;      /* позиция в кадрах исходного файла */
    double step;     /* шаг с учётом разницы частот дискретизации */
    float volume;
    int loop;
    int active;
} DSVoice;

static DSClip g_clips[DS_AUDIO_CLIPS];
static int g_clip_count = 0;
static DSVoice g_voices[DS_AUDIO_VOICES];
static int g_music_voice = -1;
static int g_audio_ready = 0;

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8)); }

/* Разбор WAV: нужен только несжатый PCM 16 бит, моно или стерео. */
static int wav_parse(const uint8_t *data, size_t size, DSClip *out) {
    if (!data || size < 44 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) return 0;
    size_t off = 12;
    int channels = 0, rate = 0, bits = 0;
    const uint8_t *pcm = NULL;
    size_t pcm_size = 0;
    while (off + 8 <= size) {
        uint32_t id_off = (uint32_t)off;
        uint32_t len = rd32(data + off + 4);
        const uint8_t *body = data + off + 8;
        if (off + 8 + (size_t)len > size) len = (uint32_t)(size - off - 8);
        if (memcmp(data + id_off, "fmt ", 4) == 0 && len >= 16) {
            if (rd16(body) != 1) return 0; /* только PCM */
            channels = rd16(body + 2);
            rate = (int)rd32(body + 4);
            bits = rd16(body + 14);
        } else if (memcmp(data + id_off, "data", 4) == 0) {
            pcm = body;
            pcm_size = len;
        }
        off += 8 + len + (len & 1u);
    }
    if (!pcm || bits != 16 || channels < 1 || channels > 2 || rate < 8000) return 0;
    size_t frames = pcm_size / (size_t)(2 * channels);
    if (!frames) return 0;
    out->pcm = (int16_t *)malloc(frames * (size_t)channels * 2u);
    if (!out->pcm) return 0;
    memcpy(out->pcm, pcm, frames * (size_t)channels * 2u);
    out->frames = frames;
    out->channels = channels;
    out->rate = rate;
    return 1;
}

static DSClip *clip_get(const char *name) {
    if (!name || !*name) return NULL;
    for (int i = 0; i < g_clip_count; i++) {
        if (strcmp(g_clips[i].name, name) == 0) return &g_clips[i];
    }
    if (g_clip_count >= DS_AUDIO_CLIPS) return NULL;
    uint8_t *data = NULL;
    size_t size = 0;
    if (!ds_asset_read(name, &data, &size)) {
        ds_log_err("audio: asset '%s' not found", name);
        return NULL;
    }
    DSClip clip;
    memset(&clip, 0, sizeof(clip));
    int ok = wav_parse(data, size, &clip);
    free(data);
    if (!ok) { ds_log_err("audio: '%s' is not 16-bit PCM WAV", name); return NULL; }
    snprintf(clip.name, sizeof(clip.name), "%s", name);
    g_clips[g_clip_count] = clip;
    return &g_clips[g_clip_count++];
}

/* Микс всех живых голосов в стерео-буфер. Голоса складываются с мягким
 * ограничением, чтобы щелчок поверх музыки не хрипел. */
static void mix_frames(int16_t *out, int frames) {
    memset(out, 0, (size_t)frames * 2u * sizeof(int16_t));
    if (!g_audio_ready) return;
    for (int v = 0; v < DS_AUDIO_VOICES; v++) {
        DSVoice *voice = &g_voices[v];
        if (!voice->active || !voice->clip) continue;
        DSClip *c = voice->clip;
        for (int i = 0; i < frames; i++) {
            size_t idx = (size_t)voice->pos;
            if (idx >= c->frames) {
                if (!voice->loop) { voice->active = 0; break; }
                voice->pos -= (double)c->frames;
                idx = (size_t)voice->pos;
                if (idx >= c->frames) { voice->active = 0; break; }
            }
            const int16_t *src = c->pcm + idx * (size_t)c->channels;
            float l = (float)src[0] * voice->volume;
            float r = (float)src[c->channels > 1 ? 1 : 0] * voice->volume;
            int li = out[i * 2] + (int)l;
            int ri = out[i * 2 + 1] + (int)r;
            out[i * 2] = (int16_t)(li > 32767 ? 32767 : (li < -32768 ? -32768 : li));
            out[i * 2 + 1] = (int16_t)(ri > 32767 ? 32767 : (ri < -32768 ? -32768 : ri));
            voice->pos += voice->step;
        }
    }
}

static void voice_start(const char *name, double volume, int loop) {
    DSClip *clip = clip_get(name);
    if (!clip) return;
    int slot = -1;
    if (loop && g_music_voice >= 0) slot = g_music_voice;
    if (slot < 0) {
        for (int i = 0; i < DS_AUDIO_VOICES; i++) {
            if (!g_voices[i].active) { slot = i; break; }
        }
    }
    if (slot < 0) slot = 0; /* всё занято — перебиваем самый старый */
    DSVoice *v = &g_voices[slot];
    v->clip = clip;
    v->pos = 0.0;
    v->step = (double)clip->rate / (double)DS_AUDIO_RATE;
    v->volume = (float)(volume <= 0.0 ? 0.0 : (volume > 1.0 ? 1.0 : volume));
    v->loop = loop;
    v->active = 1;
    if (loop) g_music_voice = slot;
}

void sound_play(const char *name, double volume) { voice_start(name, volume, 0); }
void music_play(const char *name, double volume) { voice_start(name, volume, 1); }
void music_stop(void) {
    if (g_music_voice >= 0) { g_voices[g_music_voice].active = 0; g_music_voice = -1; }
}

#ifdef _WIN32
/* --------------------------------------------------------------- waveOut -- */
#include <windows.h>
#include <mmsystem.h>
#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif

#define WO_BUFFERS 4
#define WO_FRAMES 1024

static HWAVEOUT g_wo = NULL;
static WAVEHDR g_hdr[WO_BUFFERS];
static int16_t *g_wo_buf[WO_BUFFERS];
static HANDLE g_wo_event = NULL, g_wo_thread = NULL;
static volatile LONG g_wo_run = 0;

static DWORD WINAPI wo_thread(LPVOID arg) {
    (void)arg;
    while (InterlockedCompareExchange(&g_wo_run, 1, 1)) {
        int idle = 1;
        for (int i = 0; i < WO_BUFFERS; i++) {
            if (g_hdr[i].dwFlags & WHDR_INQUEUE) continue;
            mix_frames(g_wo_buf[i], WO_FRAMES);
            g_hdr[i].dwFlags &= ~WHDR_DONE;
            waveOutWrite(g_wo, &g_hdr[i], sizeof(WAVEHDR));
            idle = 0;
        }
        if (idle) WaitForSingleObject(g_wo_event, 8);
    }
    return 0;
}

int ds_audio_init(void) {
    if (g_audio_ready) return 1;
    WAVEFORMATEX fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = DS_AUDIO_RATE;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = DS_AUDIO_RATE * 4;
    g_wo_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (waveOutOpen(&g_wo, WAVE_MAPPER, &fmt, (DWORD_PTR)g_wo_event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        ds_log_err("audio: waveOutOpen failed, игра будет тихой");
        g_wo = NULL;
        return 0;
    }
    for (int i = 0; i < WO_BUFFERS; i++) {
        g_wo_buf[i] = (int16_t *)calloc(WO_FRAMES * 2, sizeof(int16_t));
        memset(&g_hdr[i], 0, sizeof(WAVEHDR));
        g_hdr[i].lpData = (LPSTR)g_wo_buf[i];
        g_hdr[i].dwBufferLength = WO_FRAMES * 2 * sizeof(int16_t);
        waveOutPrepareHeader(g_wo, &g_hdr[i], sizeof(WAVEHDR));
    }
    g_audio_ready = 1;
    InterlockedExchange(&g_wo_run, 1);
    g_wo_thread = CreateThread(NULL, 0, wo_thread, NULL, 0, NULL);
    return 1;
}

void ds_audio_shutdown(void) {
    if (!g_audio_ready) return;
    InterlockedExchange(&g_wo_run, 0);
    if (g_wo_event) SetEvent(g_wo_event);
    if (g_wo_thread) { WaitForSingleObject(g_wo_thread, 500); CloseHandle(g_wo_thread); g_wo_thread = NULL; }
    if (g_wo) {
        waveOutReset(g_wo);
        for (int i = 0; i < WO_BUFFERS; i++) {
            waveOutUnprepareHeader(g_wo, &g_hdr[i], sizeof(WAVEHDR));
            free(g_wo_buf[i]);
            g_wo_buf[i] = NULL;
        }
        waveOutClose(g_wo);
        g_wo = NULL;
    }
    if (g_wo_event) { CloseHandle(g_wo_event); g_wo_event = NULL; }
    g_audio_ready = 0;
}

#else
/* ---------------------------------------------------------------- AAudio -- */
#include <aaudio/AAudio.h>

static AAudioStream *g_stream = NULL;

static aaudio_data_callback_result_t aa_cb(AAudioStream *s, void *user, void *audio, int32_t frames) {
    (void)s; (void)user;
    mix_frames((int16_t *)audio, (int)frames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

int ds_audio_init(void) {
    if (g_audio_ready) return 1;
    AAudioStreamBuilder *b = NULL;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK || !b) {
        ds_log_err("audio: AAudio недоступен, игра будет тихой");
        return 0;
    }
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, DS_AUDIO_RATE);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_NONE);
    AAudioStreamBuilder_setDataCallback(b, aa_cb, NULL);
    aaudio_result_t res = AAudioStreamBuilder_openStream(b, &g_stream);
    AAudioStreamBuilder_delete(b);
    if (res != AAUDIO_OK || !g_stream) {
        ds_log_err("audio: не удалось открыть поток AAudio");
        g_stream = NULL;
        return 0;
    }
    g_audio_ready = 1;
    AAudioStream_requestStart(g_stream);
    return 1;
}

void ds_audio_shutdown(void) {
    if (g_stream) {
        AAudioStream_requestStop(g_stream);
        AAudioStream_close(g_stream);
        g_stream = NULL;
    }
    g_audio_ready = 0;
}
#endif /* _WIN32 / __ANDROID__ */

#else
/* ------------------------------------------------- тихая заглушка (тест) -- */
int ds_audio_init(void) { return 0; }
void ds_audio_shutdown(void) { }
void sound_play(const char *name, double volume) { (void)name; (void)volume; }
void music_play(const char *name, double volume) { (void)name; (void)volume; }
void music_stop(void) { }
#endif
