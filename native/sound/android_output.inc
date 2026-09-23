/* ------------------------------------------------------------------ */
/* Android: the system AudioTrack through JNI, and deliberately without OpenSL ES   */
/* and AAudio: both want linking flags (-lOpenSLES, -laaudio), while JNI is already    */
/* reachable through the JavaVM of NativeActivity, so the build needs nothing. */
/* ------------------------------------------------------------------ */
#ifdef __ANDROID__
#include <jni.h>
#include <pthread.h>

#define AT_FRAMES 1024          /* frames per write() call, about 23 ms */
#define AT_REFILL 4             /* how much minbuf is kept in the device queue */

static JavaVM *snd_vm = NULL;
static jobject snd_at_track = NULL;   /* global ref to AudioTrack */
static jshortArray snd_at_buf = NULL; /* global ref to the write() buffer */
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
        /* write() in MODE_STREAM blocks once the device buffer is full, which is what
         * sets the pace of the thread, with no sleeps. */
        jint written = (*env)->CallIntMethod(env, snd_at_track, snd_at_write, snd_at_buf, 0, (jint)(AT_FRAMES * 2));
        if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); ds_log_err("audio: AudioTrack.write failed"); break; }
        if (written < 0) break;
        /* The buffer goes to the system as a whole, so mixing happens AFTER the write
         * while the first block was filled in advance, in snd_backend_start. */
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
        if (minbuf < 4096) minbuf = 4096;               /* guards against 0 and -1 */
        if (minbuf > SND_RATE * 8) minbuf = SND_RATE * 8; /* and against huge values */
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
        /* The first block leaves at once, and after that the thread keeps the queue full. */
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
    /* When the track is paused, after the app lost focus, write() on the audio thread
     * stays blocked forever, so play() is restored first to drain the buffer and let
     * the thread notice the stop flag. */
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
/* lifecycle: called from main.c on Android                       */
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