#include "net.h"
#include "runtime.h"
#include <math.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <android/log.h>
#define LOG(...) do { __android_log_print(ANDROID_LOG_INFO, "DimScriptNet", __VA_ARGS__); ds_console_log(0, __VA_ARGS__); } while (0)
#define LOGERR(...) do { __android_log_print(ANDROID_LOG_ERROR, "DimScriptNet", __VA_ARGS__); ds_console_log(1, __VA_ARGS__); } while (0)
#define URL 512
#define BODY 1024
#define RESP 4096
#define CHAT_RESP 8192
#define WRITE_TICK 60
#define READ_TICK 60
#define CHAT_TICK 1000
#define EVENT_TICK 500
/* Баннер админа ("text ..."): опрос banner.json раз в секунду — команда не
 * претендует на мгновенную доставку, как позиция игрока. */
#define BANNER_TICK 1000
#define BANNER_COLOR_MAX 16
/* Анти-дудос со своей стороны: если состояние игрока не меняется, слот всё
 * равно «тикать» должен чаще, чем TIMEOUT других клиентов. */
#define HEARTBEAT_TICK 2500
/* Чистку старых сообщений чата не нужно гонять каждую секунду. */
#define CHAT_PRUNE_TICK 5000
#define TIMEOUT 4000
#define STALE 6000
#define CHAT_MAX 32
#define CHAT_TEXT_MAX 96
#define LOGIN_NICK_MAX 16
#define SESSION_FILE "auth.dat"
#define PROGRESS_FILE "progress.dat"
#define ACHIEVEMENTS_FILE "achievements.dat"
#define BANS_FILE "bans.dat"
#define BAN_MAX 64
#define BAN_TICK 2000
#define BAN_WAIT 2500
#define BAN_KICK_TICK 2000

/* Firebase Auth: буферы под подписанные токеном URL (URL + ?auth=<idToken>). */
#define FB_URL_MAX 2048
#define FB_ID_MAX 1100
#define FB_REFRESH_MAX 512
typedef pthread_mutex_t DSMutex;
#define DS_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
#define ds_mutex_lock(m) pthread_mutex_lock(&(m))
#define ds_mutex_unlock(m) pthread_mutex_unlock(&(m))
typedef pthread_t DSThread;
static DSThread ds_thread_start(void *(*fn)(void *), void *arg) {
    pthread_t t;
    if (pthread_create(&t, NULL, fn, arg) != 0) return (pthread_t)0;
    return t;
}
static void ds_thread_join(DSThread t) { if (t) pthread_join(t, NULL); }
static void ds_thread_detach(DSThread t) { if (t) pthread_detach(t); }

typedef struct {
    double x,y,a,hp,alive;
    double punch_x,punch_y,punch_dx,punch_dy,punch;
    double snow_x,snow_y,snow_dx,snow_dy,snow;
    double station_x,station_y,station_hp,station;
    double universe_x,universe_y,universe;
    double cls, level, skin;
    int online;
    char nick[24];
} Actor;
typedef struct { char key[28]; char uid[24]; char nick[24]; char text[CHAT_TEXT_MAX]; int valid; } ChatMsg;
static int chat_keep = 20;
static struct {
    DSThread thread, rthread; DSMutex lock;
    JavaVM *vm;
    int run, started, slot, status;
    int self_banned;
    char base[256], room[48], uid[24];
    Actor me; unsigned long seq, count;
    Actor players[NET_SLOTS];
    ChatMsg chats[CHAT_MAX]; int chat_count;
    int event;
    char banner_text[CHAT_TEXT_MAX];
    char banner_color[BANNER_COLOR_MAX];
    long long banner_ts;
} net = { .lock = DS_MUTEX_INIT };

/* Список банов, прочитанный из облака. Это общий для всех клиентов источник
 * правды: локальный bans.dat есть только у того, кто ставил бан, поэтому
 * выкидывать из комнаты чужого забаненного игрока можно лишь по этому списку. */
typedef struct {
    DSMutex lock;
    char nick[BAN_MAX][LOGIN_NICK_MAX + 1];
    int count, synced;
    long long synced_at;
} BanList;
static BanList banlist = { .lock = DS_MUTEX_INIT };
static void ban_lock(void) { ds_mutex_lock(banlist.lock); }
static void ban_unlock(void) { ds_mutex_unlock(banlist.lock); }

static volatile sig_atomic_t net_fast = 0;
/* Статики наблюдения за слотами (claim_slot) и приёма игроков (read_players):
 * живут на уровне файла, чтобы net_connect мог сбросить их при каждом новом
 * подключении — иначе записи прошлой сессии «помнят» чужие uid/seq и старые
 * метки времени, что путает захват слота и определение онлайна после
 * переподключения. */
static unsigned long claim_seen_seq[NET_SLOTS];
static long long claim_seen_at[NET_SLOTS];
static char claim_seen_uid[NET_SLOTS][24];
static unsigned long rp_lseq[NET_SLOTS];
static long long rp_lch[NET_SLOTS];
static long long rp_kicked[NET_SLOTS];
static char s_chat_text_ret[CHAT_TEXT_MAX];
static char s_chat_uid_ret[24];
static char s_nick_ret[24];
static char s_lg_nick_ret[24];
static char s_lg_pass_ret[64];
static char s_ban_target_ret[32];
static char s_banner_text_ret[CHAT_TEXT_MAX];
static char s_banner_color_ret[BANNER_COLOR_MAX];
static char s_text_cmd_text_ret[CHAT_TEXT_MAX];
static char s_text_cmd_color_ret[BANNER_COLOR_MAX];
static long long now_ms(void);
static void lock(void);
static void unlock(void);
/* Общий HTTP-каркас: структура задачи для фоновых запросов и объявления
 * функций, которые определены в других частях net.c (translation unit один). */
typedef struct { char url[FB_URL_MAX]; char body[BODY*2]; } HttpJob;
static int fb_enabled(void);
static void fb_url(char *dst, size_t cap, const char *url);
static int http(const char *method,const char *url,const char *body,char *out,size_t cap);
static void *http_delete_job(void *arg);
static void room_slot_delete_async(int slot);

typedef struct {
    DSMutex lock;
    int status;
    char session_nick[LOGIN_NICK_MAX+1];
    char session_pass[64];
    char path[256];
} Login;
static Login lg = { .lock = DS_MUTEX_INIT };
static void lg_lock(void) { ds_mutex_lock(lg.lock); }
static void lg_unlock(void) { ds_mutex_unlock(lg.lock); }

/* Профиль игрока одной структурой: валюты, купленные классы, уровни каждой
 * ветки, батл пасс, скин, достижения и метка времени сохранения.
 *
 * Раньше всё это ездило списком из 18 аргументов через progress_write /
 * progress_read / net_save_progress_all, и порядок полей в файле не совпадал
 * с порядком в облачном JSON: покупка буКа (ebuc) легко терялась при входе.
 * Теперь источник один.
 *
 * Поля, которых нет в progress.dat:
 *   ach   — битмаск достижений, локально живёт в achievements.dat, а в облако
 *           уходит вместе с профилем (см. profile_push_cloud);
 *   ts    — когда профиль сохранён (настенное время, мс): по нему слияние
 *           облака и локальной копии решает, где валюты свежее;
 *   owner — ник, которому принадлежит локальная копия: без него прогресс
 *           прошлого аккаунта перетекал бы в новый на этом же устройстве. */
typedef struct {
    int cups, candies, cls, azum, santa, ebuc, level, levels_unlocked;
    int ordinary_level, ordinary_levels_unlocked;
    int azum_level, azum_levels_unlocked;
    int santa_level, santa_levels_unlocked;
    int ebuc_level, ebuc_levels_unlocked;
    int bp_level, azum_skin;
    /* Язык интерфейса (0 — English, 1 — Русский): единственная настройка,
     * которая раньше вообще нигде не хранилась и сбрасывалась при перезапуске. */
    int lang;
    unsigned int ach;
    long long ts;
    char owner[LOGIN_NICK_MAX + 1];
} Profile;

typedef struct {
    DSMutex lock;
    int loaded;
    Profile p;
} Progress;
static Progress pg = { .lock = DS_MUTEX_INIT };
#define LEVEL_MAX 3
#define BP_MAX 5
#define SKIN_MAX 1
/* Сколько чисел в первой строке progress.dat: 18 полей прогресса + ts + lang. */
#define PROFILE_NUMBERS 20
static int pending_cls = 0;
static int pending_level = 0;
/* Скин своего бойца: как и класс с уровнем, уходит в сетевой снимок, чтобы
 * соперники видели Азума-зомби, а не только сам игрок. */
static int pending_skin = 0;
/* Достижения — единый битмаск (ACH_FLAG_* в net.h). Локально хранятся в
 * achievements.dat, в облаке — в поле ach записи игрока. */
static DSMutex ach_lock = DS_MUTEX_INIT;
static int ach_loaded = 0;
static unsigned int ach_flags = 0;
static void pg_lock(void) { ds_mutex_lock(pg.lock); }
static void pg_unlock(void) { ds_mutex_unlock(pg.lock); }
/* Определения живут в других частях net.c (translation unit один). */
static void json_escape(const char *src, char *dst, size_t cap);
static int fb_http(const char *method, const char *url, const char *body, char *out, size_t cap);
static int net_log_ok(void);
/* Настенное время для ts профиля. now_ms() — монотонное (считает от загрузки
 * устройства), для сравнения «чья копия новее» оно не годится. */
static long long wall_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int imax(int a, int b) { return a > b ? a : b; }
static long long llmax(long long a, long long b) { return a > b ? a : b; }
/* Ники сравниваем без учёта регистра латиницы: запись в облаке и e-mail
 * аккаунта приводятся к нижнему регистру, а progress.dat хранит ник как есть. */
static int nick_same(const char *a, const char *b) {
    size_t i;
    if (!a || !b) return 0;
    for (i = 0; a[i] && b[i]; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return a[i] == 0 && b[i] == 0;
}
static void data_file_path(char *path, size_t cap, const char *name) {
    lg_lock();
    if (lg.path[0]) snprintf(path, cap, "%s/%s", lg.path, name);
    else snprintf(path, cap, "%s", name);
    lg_unlock();
}
static int clamp_level_value(int value) {
    if (value < 0) return 0;
    if (value > LEVEL_MAX) return LEVEL_MAX;
    return value;
}
static int clamp_levels_unlocked(int value) {
    if (value < 0) return 0;
    if (value > LEVEL_MAX) return LEVEL_MAX;
    return value;
}
static void normalize_level_pair(int *level, int *unlocked) {
    *unlocked = clamp_levels_unlocked(*unlocked);
    *level = clamp_level_value(*level);
    if (*level > *unlocked) *level = 0;
}
