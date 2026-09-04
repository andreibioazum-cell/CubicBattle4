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
    double cls, level;
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

typedef struct {
    DSMutex lock;
    int loaded, cups, candies, cls, azum, santa, ebuc, level, levels_unlocked;
    int ordinary_level, ordinary_levels_unlocked;
    int azum_level, azum_levels_unlocked;
    int santa_level, santa_levels_unlocked;
    int ebuc_level, ebuc_levels_unlocked;
    int bp_level, azum_skin;
} Progress;
static Progress pg = { .lock = DS_MUTEX_INIT };
#define LEVEL_MAX 3
#define BP_MAX 5
static int pending_cls = 0;
static int pending_level = 0;
static void pg_lock(void) { ds_mutex_lock(pg.lock); }
static void pg_unlock(void) { ds_mutex_unlock(pg.lock); }
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
static void progress_write(int cups, int candies, int cls, int azum, int santa, int ebuc,
                           int level, int levels_unlocked,
                           int ordinary_level, int ordinary_levels_unlocked,
                           int azum_level, int azum_levels_unlocked,
                           int santa_level, int santa_levels_unlocked,
                           int ebuc_level, int ebuc_levels_unlocked,
                           int bp_level, int azum_skin) {
    char path[320]; FILE *f;
    data_file_path(path, sizeof(path), PROGRESS_FILE);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n",
                cups, cls, azum, santa, candies, level, levels_unlocked,
                ordinary_level, ordinary_levels_unlocked,
                azum_level, azum_levels_unlocked,
                santa_level, santa_levels_unlocked,
                ebuc_level, ebuc_levels_unlocked, ebuc,
                bp_level, azum_skin);
        fclose(f);
    }
}
static void progress_read(void) {
    char path[320]; FILE *f;
    int cups=0, candies=0, cls=0, azum=0, santa=0, ebuc=0, level=0, levels_unlocked=0;
    int ordinary_level=-1, ordinary_levels_unlocked=-1;
    int azum_level=-1, azum_levels_unlocked=-1;
    int santa_level=-1, santa_levels_unlocked=-1;
    int ebuc_level=-1, ebuc_levels_unlocked=-1;
    int bp_level=0, azum_skin=0;
    pg_lock();
    if (pg.loaded) { pg_unlock(); return; }
    pg_unlock();
    data_file_path(path, sizeof(path), PROGRESS_FILE);
    f = fopen(path, "r");
    if (f) {
        int n = fscanf(f, "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
                       &cups, &cls, &azum, &santa, &candies,
                       &level, &levels_unlocked,
                       &ordinary_level, &ordinary_levels_unlocked,
                       &azum_level, &azum_levels_unlocked,
                       &santa_level, &santa_levels_unlocked,
                       &ebuc_level, &ebuc_levels_unlocked, &ebuc,
                       &bp_level, &azum_skin);
        if (n < 3) {
            cups=0; candies=0; cls=0; azum=0; santa=0; ebuc=0; level=0; levels_unlocked=0;
            ordinary_level=ordinary_levels_unlocked=0;
            azum_level=azum_levels_unlocked=0;
            santa_level=santa_levels_unlocked=0;
            ebuc_level=ebuc_levels_unlocked=0;
        } else {
            if (n < 6) { level=0; levels_unlocked=0; }
            else if (n < 7) {
                levels_unlocked=level;
            }
            if (n < 13) {
                ordinary_level=ordinary_levels_unlocked=0;
                azum_level=azum_levels_unlocked=0;
                santa_level=santa_levels_unlocked=0;
                ebuc_level=ebuc_levels_unlocked=0;
                if (cls==1 && azum) { azum_level=level; azum_levels_unlocked=levels_unlocked; }
                else if (cls==2 && santa) { santa_level=level; santa_levels_unlocked=levels_unlocked; }
                else { ordinary_level=level; ordinary_levels_unlocked=levels_unlocked; }
            }
            if (n < 16) {
                ebuc_level=ebuc_levels_unlocked=0;
                ebuc=0;
            }
            if (n < 17) { bp_level=0; azum_skin=0; }
            else if (n < 18) { azum_skin=0; }
        }
        fclose(f);
    }
    if (cups < 0) cups = 0;
    if (candies < 0) candies = 0;
    if (cls != 1 && cls != 2 && cls != 3) cls = 0;
    azum = azum ? 1 : 0;
    santa = santa ? 1 : 0;
    ebuc = ebuc ? 1 : 0;
    if (cls == 1 && !azum) cls = 0;
    if (cls == 2 && !santa) cls = 0;
    if (cls == 3 && !ebuc) cls = 0;
    normalize_level_pair(&level, &levels_unlocked);
    normalize_level_pair(&ordinary_level, &ordinary_levels_unlocked);
    normalize_level_pair(&azum_level, &azum_levels_unlocked);
    normalize_level_pair(&santa_level, &santa_levels_unlocked);
    normalize_level_pair(&ebuc_level, &ebuc_levels_unlocked);
    if (cls == 0) { level=ordinary_level; levels_unlocked=ordinary_levels_unlocked; }
    else if (cls == 1) { level=azum_level; levels_unlocked=azum_levels_unlocked; }
    else if (cls == 2) { level=santa_level; levels_unlocked=santa_levels_unlocked; }
    else { level=ebuc_level; levels_unlocked=ebuc_levels_unlocked; }
    if (bp_level < 0) bp_level = 0;
    if (bp_level > BP_MAX) bp_level = BP_MAX;
    azum_skin = azum_skin ? 1 : 0;
    pg_lock();
    pg.cups = cups; pg.candies = candies; pg.cls = cls; pg.azum = azum; pg.santa = santa; pg.ebuc = ebuc;
    pg.level = level; pg.levels_unlocked = levels_unlocked;
    pg.ordinary_level = ordinary_level; pg.ordinary_levels_unlocked = ordinary_levels_unlocked;
    pg.azum_level = azum_level; pg.azum_levels_unlocked = azum_levels_unlocked;
    pg.santa_level = santa_level; pg.santa_levels_unlocked = santa_levels_unlocked;
    pg.ebuc_level = ebuc_level; pg.ebuc_levels_unlocked = ebuc_levels_unlocked;
    pg.bp_level = bp_level; pg.azum_skin = azum_skin;
    pg.loaded = 1;
    pg_unlock();
}
double net_load_cups(void) { double v; progress_read(); pg_lock(); v = (double)pg.cups; pg_unlock(); return v; }
double net_load_candies(void) { double v; progress_read(); pg_lock(); v = (double)pg.candies; pg_unlock(); return v; }
double net_load_class(void) { double v; progress_read(); pg_lock(); v = (double)pg.cls; pg_unlock(); return v; }
double net_load_azum(void) { double v; progress_read(); pg_lock(); v = (double)pg.azum; pg_unlock(); return v; }
double net_load_santa(void) { double v; progress_read(); pg_lock(); v = (double)pg.santa; pg_unlock(); return v; }
double net_load_ebuc(void) { double v; progress_read(); pg_lock(); v = (double)pg.ebuc; pg_unlock(); return v; }
double net_load_level(void) { double v; progress_read(); pg_lock(); v = (double)pg.level; pg_unlock(); return v; }
double net_load_levels_unlocked(void) { double v; progress_read(); pg_lock(); v = (double)pg.levels_unlocked; pg_unlock(); return v; }
double net_load_ordinary_level(void) { double v; progress_read(); pg_lock(); v = (double)pg.ordinary_level; pg_unlock(); return v; }
double net_load_ordinary_levels_unlocked(void) { double v; progress_read(); pg_lock(); v = (double)pg.ordinary_levels_unlocked; pg_unlock(); return v; }
double net_load_azum_level(void) { double v; progress_read(); pg_lock(); v = (double)pg.azum_level; pg_unlock(); return v; }
double net_load_azum_levels_unlocked(void) { double v; progress_read(); pg_lock(); v = (double)pg.azum_levels_unlocked; pg_unlock(); return v; }
double net_load_santa_level(void) { double v; progress_read(); pg_lock(); v = (double)pg.santa_level; pg_unlock(); return v; }
double net_load_santa_levels_unlocked(void) { double v; progress_read(); pg_lock(); v = (double)pg.santa_levels_unlocked; pg_unlock(); return v; }
double net_load_ebuc_level(void) { double v; progress_read(); pg_lock(); v = (double)pg.ebuc_level; pg_unlock(); return v; }
double net_load_ebuc_levels_unlocked(void) { double v; progress_read(); pg_lock(); v = (double)pg.ebuc_levels_unlocked; pg_unlock(); return v; }
double net_load_bp_level(void) { double v; progress_read(); pg_lock(); v = (double)pg.bp_level; pg_unlock(); return v; }
double net_load_azum_skin(void) { double v; progress_read(); pg_lock(); v = (double)pg.azum_skin; pg_unlock(); return v; }

typedef struct { char url[FB_URL_MAX]; char body[BODY*2]; } HttpJob;
static int fb_enabled(void);
static void fb_url(char *dst, size_t cap, const char *url);
static int http(const char *method,const char *url,const char *body,char *out,size_t cap);
static void *http_delete_job(void *arg);
static void room_slot_delete_async(int slot);
static void *http_patch_job(void *arg) {
    HttpJob *j = (HttpJob*)arg;
    if (j) { http("PATCH", j->url, j->body, NULL, 0); free(j); }
    return NULL;
}
static void http_patch_async(const char *url, const char *body) {
    HttpJob *j = (HttpJob*)malloc(sizeof(*j));
    DSThread t;
    if (!j) return;
    fb_url(j->url, sizeof(j->url), url);
    snprintf(j->body, sizeof(j->body), "%s", body);
    t = ds_thread_start(http_patch_job, j);
    if (t) { ds_thread_detach(t); return; }
    free(j);
}

void net_save_progress(double cups, double candies, double cls, double azum, double santa, double ebuc,
                       double level, double levels_unlocked) {
    int k=(int)cls;
    int ol,ou,al,au,sl,su,el,eu,bp,sk;
    progress_read();
    pg_lock();
    ol=pg.ordinary_level; ou=pg.ordinary_levels_unlocked;
    al=pg.azum_level; au=pg.azum_levels_unlocked;
    sl=pg.santa_level; su=pg.santa_levels_unlocked;
    el=pg.ebuc_level; eu=pg.ebuc_levels_unlocked;
    bp=pg.bp_level; sk=pg.azum_skin;
    pg_unlock();
    if (k==0) { ol=(int)level; ou=(int)levels_unlocked; }
    else if (k==1) { al=(int)level; au=(int)levels_unlocked; }
    else if (k==2) { sl=(int)level; su=(int)levels_unlocked; }
    else if (k==3) { el=(int)level; eu=(int)levels_unlocked; }
    net_save_progress_all(cups,candies,cls,azum,santa,ebuc,level,levels_unlocked,
                          ol,ou,al,au,sl,su,el,eu,bp,sk);
}

void net_save_progress_all(double cups, double candies, double cls, double azum, double santa, double ebuc,
                           double level, double levels_unlocked,
                           double ordinary_level, double ordinary_levels_unlocked,
                           double azum_level, double azum_levels_unlocked,
                           double santa_level, double santa_levels_unlocked,
                           double ebuc_level, double ebuc_levels_unlocked,
                           double bp_level, double azum_skin) {
    int c = (int)cups, cd = (int)candies, k = (int)cls, a = azum ? 1 : 0, sn = santa ? 1 : 0, eu = ebuc ? 1 : 0;
    int lv = (int)level, lu = (int)levels_unlocked;
    int ol = (int)ordinary_level, ou = (int)ordinary_levels_unlocked;
    int al = (int)azum_level, au = (int)azum_levels_unlocked;
    int sl = (int)santa_level, su = (int)santa_levels_unlocked;
    int el = (int)ebuc_level, eul = (int)ebuc_levels_unlocked;
    int bp = (int)bp_level, sk = azum_skin ? 1 : 0;
    if (c < 0) c = 0;
    if (cd < 0) cd = 0;
    if (k != 1 && k != 2 && k != 3) k = 0;
    if (k == 1 && !a) k = 0;
    if (k == 2 && !sn) k = 0;
    if (k == 3 && !eu) k = 0;
    if (bp < 0) bp = 0;
    if (bp > BP_MAX) bp = BP_MAX;
    normalize_level_pair(&lv, &lu);
    normalize_level_pair(&ol, &ou);
    normalize_level_pair(&al, &au);
    normalize_level_pair(&sl, &su);
    normalize_level_pair(&el, &eul);
    if (k == 0) { lv=ol; lu=ou; }
    else if (k == 1) { lv=al; lu=au; }
    else if (k == 2) { lv=sl; lu=su; }
    else { lv=el; lu=eul; }
    pg_lock();
    pg.cups = c; pg.candies = cd; pg.cls = k; pg.azum = a; pg.santa = sn; pg.ebuc = eu;
    pg.level = lv; pg.levels_unlocked = lu; pg.loaded = 1;
    pg.ordinary_level = ol; pg.ordinary_levels_unlocked = ou;
    pg.azum_level = al; pg.azum_levels_unlocked = au;
    pg.santa_level = sl; pg.santa_levels_unlocked = su;
    pg.ebuc_level = el; pg.ebuc_levels_unlocked = eul;
    pg.bp_level = bp; pg.azum_skin = sk;
    pg_unlock();
    pending_cls = k;
    pending_level = lv;
    progress_write(c, cd, k, a, sn, eu, lv, lu, ol, ou, al, au, sl, su, el, eul, bp, sk);

    char nick[LOGIN_NICK_MAX + 1] = "";
    lg_lock();
    if (lg.status == NET_LOGIN_OK && lg.session_nick[0]) {
        snprintf(nick, sizeof(nick), "%s", lg.session_nick);
    }
    lg_unlock();
    if (nick[0] && net.base[0]) {
        char url[URL], body[BODY*2];
        snprintf(url, sizeof(url), "%s/users/%s.json", net.base, nick);
        snprintf(body, sizeof(body),
                 "{\"cups\":%d,\"candies\":%d,\"cls\":%d,\"azum\":%d,\"santa\":%d,\"ebuc\":%d,\"level\":%d,\"levels\":%d,\"ordinary_level\":%d,\"ordinary_levels\":%d,\"azum_level\":%d,\"azum_levels\":%d,\"santa_level\":%d,\"santa_levels\":%d,\"ebuc_level\":%d,\"ebuc_levels\":%d,\"bp\":%d,\"azum_skin\":%d}",
                 c, cd, k, a, sn, eu, lv, lu, ol, ou, al, au, sl, su, el, eul, bp, sk);
        http_patch_async(url, body);
    }
}
/* Достижения — отдельный маленький файл с единым битмаском: ACH_FLAG_* задают
 * отдельные награды. Добавление нового бита не ломает старые записи. Путь
 * хранится рядом с аккаунтом и прогрессом. */
static DSMutex ach_lock = DS_MUTEX_INIT;
static int ach_loaded = 0;
static unsigned int ach_flags = 0;

static void achievements_read(void) {
    char path[320];
    FILE *f;
    unsigned int flags = 0;
    ds_mutex_lock(ach_lock);
    if (ach_loaded) { ds_mutex_unlock(ach_lock); return; }
    ds_mutex_unlock(ach_lock);
    data_file_path(path, sizeof(path), ACHIEVEMENTS_FILE);
    f = fopen(path, "r");
    if (f) {
        /* Современный формат — одно целое число-битмаск. Старые сборки писали
         * «welcome all_characters» (0/1 0/1): такой файл тоже читаем и
         * перекладываем в биты. */
        char line[64];
        if (fgets(line, sizeof(line), f)) {
            int a = 0, b = 0;
            if (sscanf(line, "%d %d", &a, &b) == 2) {
                flags = (unsigned int)((a ? ACH_FLAG_WELCOME : 0)
                                       | (b ? ACH_FLAG_ALL_CHARACTERS : 0));
            } else if (sscanf(line, "%u", &flags) != 1) {
                flags = 0;
            }
        } else {
            flags = 0;
        }
        fclose(f);
    }
    ds_mutex_lock(ach_lock);
    ach_flags = flags;
    ach_loaded = 1;
    ds_mutex_unlock(ach_lock);
}

static void achievements_write(unsigned int flags) {
    char path[320];
    FILE *f;
    data_file_path(path, sizeof(path), ACHIEVEMENTS_FILE);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "%u\n", flags);
        fclose(f);
    }
}

double net_load_achievement_flags(void) {
    double value;
    achievements_read();
    ds_mutex_lock(ach_lock); value = (double)ach_flags; ds_mutex_unlock(ach_lock);
    return value;
}

void net_save_achievement_flags(double flags) {
    unsigned int f = (unsigned int)flags;
    ds_mutex_lock(ach_lock);
    ach_flags = f;
    ach_loaded = 1;
    ds_mutex_unlock(ach_lock);
    achievements_write(f);
}

double net_has_achievement_flag(double flag) {
    double result;
    achievements_read();
    ds_mutex_lock(ach_lock);
    result = (ach_flags & (unsigned int)flag) ? 1.0 : 0.0;
    ds_mutex_unlock(ach_lock);
    return result;
}

void net_mark_achievement_flag(double flag) {
    unsigned int f;
    achievements_read();
    ds_mutex_lock(ach_lock);
    ach_flags |= (unsigned int)flag;
    f = ach_flags;
    ds_mutex_unlock(ach_lock);
    achievements_write(f);
}

void net_set_class(double cls) {
    int k = (int)cls;
    if (k != 1 && k != 2 && k != 3) k = 0;
    pending_cls = k;
    lock(); net.me.cls = (double)k;
    if (net.slot >= 0) net.players[net.slot].cls = (double)k;
    unlock();
}
void net_set_level(double level) {
    int lv = (int)level;
    if (lv < 0) lv = 0;
    if (lv > LEVEL_MAX) lv = LEVEL_MAX;
    pending_level = lv;
    lock(); net.me.level = (double)lv;
    if (net.slot >= 0) net.players[net.slot].level = (double)lv;
    unlock();
}

static int nick_valid(const char *n) {
    size_t i=0, bytes=n ? strlen(n) : 0, chars=0;
    if (bytes < 1 || bytes > LOGIN_NICK_MAX) return 0;
    while (i < bytes) {
        unsigned char c=(unsigned char)n[i]; unsigned long cp=0; size_t need=0;
        if (c<0x80) { cp=c; need=1; }
        else if ((c&0xE0)==0xC0) { cp=c&0x1F; need=2; }
        else if ((c&0xF0)==0xE0) { cp=c&0x0F; need=3; }
        else if ((c&0xF8)==0xF0) { cp=c&0x07; need=4; }
        else return 0;
        if (i+need>bytes) return 0;
        size_t k; for(k=1;k<need;k++) { unsigned char q=(unsigned char)n[i+k]; if((q&0xC0)!=0x80) return 0; cp=(cp<<6)|(q&0x3F); }
        if ((need==2&&cp<0x80)||(need==3&&cp<0x800)||(need==4&&cp<0x10000)||cp>0x10FFFF) return 0;
        if ((cp>='a'&&cp<='z')||(cp>='A'&&cp<='Z')||(cp>='0'&&cp<='9')||cp=='_'||(cp>=0x0400&&cp<=0x04FF)) chars++;
        else return 0;
        i+=need;
    }
    return chars>=1 && chars<=16;
}

static int pass_valid(const char *p) {
    if (!p) return 0;
    size_t len = strlen(p);
    if (len < 1 || len > 32) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c < 0x20 || c == 0x7F || c == '\"' || c == '\\' || c == ' ') return 0;
    }
    return 1;
}

static void session_save(const char *nick, const char *pass) {
    char path[320]; FILE *f;
    if (lg.path[0]) snprintf(path, sizeof(path), "%s/%s", lg.path, SESSION_FILE);
    else snprintf(path, sizeof(path), "%s", SESSION_FILE);
    f = fopen(path, "w");
    if (f) {
        if (pass && *pass) fprintf(f, "%s %s\n", nick, pass);
        else fprintf(f, "%s\n", nick);
        fclose(f);
    }
}
static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec*1000+t.tv_nsec/1000000;
}
static void sleep_ms(int ms) {
    struct timespec t = { ms/1000, (long)(ms%1000)*1000000L };
    nanosleep(&t, NULL);
}
static double safe(double v) { if (v != v) return 0; if (v > 1e300 || v < -1e300) return 0; return v; }
static void lock(void) { ds_mutex_lock(net.lock); }
static void unlock(void) { ds_mutex_unlock(net.lock); }
static void status(int s) { lock(); net.status=s; unlock(); }

static DSMutex net_log_lock = DS_MUTEX_INIT;
static int net_log_ok(void) {
    static long long last = 0;
    long long now = now_ms();
    int ok = 0;
    ds_mutex_lock(net_log_lock);
    if (now - last >= 2000) { last = now; ok = 1; }
    ds_mutex_unlock(net_log_lock);
    return ok;
}
void net_set_java_vm(JavaVM *vm) { net.vm=vm; }
static int http_ex(const char *method,const char *url,const char *body,char *out,size_t cap,
                   const char *header,const char *value,char *etag,size_t etag_cap) {
    JNIEnv *env=NULL; jobject conn=NULL, stream=NULL, urlobj=NULL;
    jclass urlc, connc, streamc; jbyteArray buf; jstring ju, jm;
    int code=0, attached=0, ok=0; size_t total=0;
    if(out&&cap)out[0]='\0';
    if(etag&&etag_cap)etag[0]='\0';
    if (!net.vm) return 0;
    if ((*net.vm)->GetEnv(net.vm,(void**)&env,JNI_VERSION_1_6)!=JNI_OK) {
        if ((*net.vm)->AttachCurrentThread(net.vm,&env,NULL)!=JNI_OK) return 0;
        attached=1;
    }
#define JNI_CHECK() do { if ((*env)->ExceptionCheck(env)) goto done; } while (0)
    if ((*env)->PushLocalFrame(env,32)!=0) goto done;
    urlc=(*env)->FindClass(env,"java/net/URL"); JNI_CHECK();
    connc=(*env)->FindClass(env,"java/net/HttpURLConnection"); JNI_CHECK();
    ju=(*env)->NewStringUTF(env,url); JNI_CHECK();
    urlobj=(*env)->NewObject(env,urlc,(*env)->GetMethodID(env,urlc,"<init>","(Ljava/lang/String;)V"),ju); JNI_CHECK();
    conn=(*env)->CallObjectMethod(env,urlobj,(*env)->GetMethodID(env,urlc,"openConnection","()Ljava/net/URLConnection;")); JNI_CHECK();
    {
        /* HttpURLConnection бросает ProtocolException на PATCH, а сохранение
         * прогресса и банов идёт именно им. Firebase умеет принимать такой
         * запрос как POST с заголовком X-HTTP-Method-Override. */
        const char *send_method = method;
        const char *override = NULL;
        if (!strcmp(method, "PATCH")) { send_method = "POST"; override = "PATCH"; }
        jm=(*env)->NewStringUTF(env,send_method); JNI_CHECK();
        (*env)->CallVoidMethod(env,conn,(*env)->GetMethodID(env,connc,"setRequestMethod","(Ljava/lang/String;)V"),jm); JNI_CHECK();
        if (override) {
            jmethodID set_override=(*env)->GetMethodID(env,connc,"setRequestProperty","(Ljava/lang/String;Ljava/lang/String;)V");
            jstring ok=(*env)->NewStringUTF(env,"X-HTTP-Method-Override"), ov=(*env)->NewStringUTF(env,override);
            (*env)->CallVoidMethod(env,conn,set_override,ok,ov); JNI_CHECK();
        }
    }
    {
        int tmo = net_fast ? 1200 : TIMEOUT;
        (*env)->CallVoidMethod(env,conn,(*env)->GetMethodID(env,connc,"setConnectTimeout","(I)V"),tmo); JNI_CHECK();
        (*env)->CallVoidMethod(env,conn,(*env)->GetMethodID(env,connc,"setReadTimeout","(I)V"),tmo); JNI_CHECK();
    }
    (*env)->CallVoidMethod(env,conn,(*env)->GetMethodID(env,connc,"setUseCaches","(Z)V"),JNI_FALSE); JNI_CHECK();
    {
        jmethodID set_header=(*env)->GetMethodID(env,connc,"setRequestProperty","(Ljava/lang/String;Ljava/lang/String;)V");
        jstring k=(*env)->NewStringUTF(env,"Content-Type"), v=(*env)->NewStringUTF(env,"application/json");
        (*env)->CallVoidMethod(env,conn,set_header,k,v); JNI_CHECK();
        if(header&&value) { k=(*env)->NewStringUTF(env,header); v=(*env)->NewStringUTF(env,value); (*env)->CallVoidMethod(env,conn,set_header,k,v); JNI_CHECK(); }
    }
    if (body&&*body) {
        jobject os=NULL; jbyteArray data; jsize len=(jsize)strlen(body);
        (*env)->CallVoidMethod(env,conn,(*env)->GetMethodID(env,connc,"setDoOutput","(Z)V"),JNI_TRUE); JNI_CHECK();
        (*env)->CallVoidMethod(env,conn,(*env)->GetMethodID(env,connc,"setFixedLengthStreamingMode","(I)V"),len); JNI_CHECK();
        os=(*env)->CallObjectMethod(env,conn,(*env)->GetMethodID(env,connc,"getOutputStream","()Ljava/io/OutputStream;")); JNI_CHECK();
        if(!os) goto done;
        data=(*env)->NewByteArray(env,len); JNI_CHECK();
        (*env)->SetByteArrayRegion(env,data,0,len,(const jbyte*)body);
        (*env)->CallVoidMethod(env,os,(*env)->GetMethodID(env,(*env)->GetObjectClass(env,os),"write","([B)V"),data); JNI_CHECK();
        (*env)->CallVoidMethod(env,os,(*env)->GetMethodID(env,(*env)->GetObjectClass(env,os),"close","()V")); JNI_CHECK();
    }
    code=(int)(*env)->CallIntMethod(env,conn,(*env)->GetMethodID(env,connc,"getResponseCode","()I")); JNI_CHECK();
    if(etag&&etag_cap) {
        jstring key=(*env)->NewStringUTF(env,"ETag"); JNI_CHECK();
        jstring val=(jstring)(*env)->CallObjectMethod(env,conn,(*env)->GetMethodID(env,connc,"getHeaderField","(Ljava/lang/String;)Ljava/lang/String;"),key); JNI_CHECK();
        if(val) { const char *s=(*env)->GetStringUTFChars(env,val,NULL); if(s){snprintf(etag,etag_cap,"%s",s);(*env)->ReleaseStringUTFChars(env,val,s);} }
    }
    stream=(*env)->CallObjectMethod(env,conn,(*env)->GetMethodID(env,connc,code>=400?"getErrorStream":"getInputStream","()Ljava/io/InputStream;")); JNI_CHECK();
    if(!stream) goto closeconn;
    streamc=(*env)->GetObjectClass(env,stream); JNI_CHECK();
    buf=(*env)->NewByteArray(env,2048); JNI_CHECK();
    for (;;) {
        jint n=(*env)->CallIntMethod(env,stream,(*env)->GetMethodID(env,streamc,"read","([B)I"),buf); JNI_CHECK();
        if (n<=0) break;
        if (out&&cap&&total+(size_t)n<cap) { (*env)->GetByteArrayRegion(env,buf,0,n,(jbyte*)(out+total)); total+=(size_t)n; out[total]='\0'; }
    }
    (*env)->CallVoidMethod(env,stream,(*env)->GetMethodID(env,streamc,"close","()V")); JNI_CHECK();
closeconn:
    if (conn) { (*env)->CallVoidMethod(env,conn,(*env)->GetMethodID(env,connc,"disconnect","()V")); (*env)->ExceptionClear(env); }
    ok=1;
done:
    if ((*env)->ExceptionCheck(env)) {
        jthrowable ex = (*env)->ExceptionOccurred(env);
        (*env)->ExceptionClear(env);
        if (net_log_ok() && ex) {
            jclass tcls = (*env)->GetObjectClass(env, ex);
            jmethodID gm = tcls ? (*env)->GetMethodID(env, tcls, "getMessage", "()Ljava/lang/String;") : NULL;
            if (gm) {
                jstring jmsg = (jstring)(*env)->CallObjectMethod(env, ex, gm);
                if (jmsg && !(*env)->ExceptionCheck(env)) {
                    const char *s = (*env)->GetStringUTFChars(env, jmsg, NULL);
                    if (s) { LOGERR("http %s: %s", method, s); (*env)->ReleaseStringUTFChars(env, jmsg, s); }
                }
            }
        }
        (*env)->ExceptionClear(env);
    }
    (*env)->PopLocalFrame(env,NULL);
    if (attached) (*net.vm)->DetachCurrentThread(net.vm);
    return ok ? code : 0;
#undef JNI_CHECK
}
static int http(const char *method,const char *url,const char *body,char *out,size_t cap) {
    return http_ex(method,url,body,out,cap,NULL,NULL,NULL,0);
}
static const char *skip_ws(const char *p) { while(p&&*p&&(*p==' '||*p=='\t'||*p=='\n'||*p=='\r')) p++; return p; }
static const char *skip_str(const char *p) { if(!p||*p!='\"') return NULL; for(p++;*p;p++){ if(*p=='\\'&&p[1]){p++;continue;} if(*p=='\"') return p+1; } return NULL; }
static const char *skip_box(const char *p,char open,char close) { int d=0; if(!p||*p!=open) return NULL; for(;*p;p++){ if(*p=='\"'){p=skip_str(p); if(!p)return NULL; p--; continue;} if(*p==open)d++; else if(*p==close&&--d==0)return p+1;} return NULL; }
static const char *skip_val(const char *p) { p=skip_ws(p); if(!p||!*p)return NULL; if(*p=='\"')return skip_str(p); if(*p=='{')return skip_box(p,'{','}'); if(*p=='[')return skip_box(p,'[',']'); while(*p&&*p!=','&&*p!='}'&&*p!=']')p++; return p; }
static const char *member(const char *o,const char *key) {
    size_t n=strlen(key); const char *p=skip_ws(o);
    if(!p||*p!='{') return NULL;
    for(p=skip_ws(p+1);p&&*p&&*p!='}';) {
        const char *name=p,*end=skip_str(p); if(!end)return NULL; p=skip_ws(end); if(*p!=':')return NULL; p=skip_ws(p+1);
        if((size_t)(end-name-2)==n&&strncmp(name+1,key,n)==0) return p;
        p=skip_val(p); p=skip_ws(p); if(p&&*p==',')p=skip_ws(p+1);
    }
    return NULL;
}
static const char *element(const char *a,size_t wanted) {
    const char *p=skip_ws(a); size_t index=0;
    if(!p||*p!='[')return NULL;
    for(p=skip_ws(p+1);p&&*p&&*p!=']';index++) {
        if(index==wanted)return p;
        p=skip_val(p); p=skip_ws(p); if(p&&*p==',')p=skip_ws(p+1); else break;
    }
    return NULL;
}
static const char *path_val(const char *json,const char *path) {
    char part[48],*end; const char *v=json;
    while(path&&*path&&v) {
        const char *slash=strchr(path,'/'); size_t n=slash?(size_t)(slash-path):strlen(path),index;
        if(!n||n>=sizeof(part))return NULL;
        memcpy(part,path,n); part[n]=0; v=skip_ws(v);
        if(*v=='[') { index=strtoul(part,&end,10); if(!*part||*end)return NULL; v=element(v,index); }
        else v=member(v,part);
        path=slash?slash+1:path+n;
    }
    return v;
}
static double num(const char *json,const char *path,double fb) { const char *v=path_val(json,path); if(!v||!strncmp(v,"null",4))return fb; if(*v=='t')return 1; if(*v=='f')return 0; if(*v=='\"')v++; return atof(v); }
static void strv(const char *json,const char *path,char *out,size_t cap) {
    const char *v=path_val(json,path); size_t i=0; if(cap)out[0]=0; if(!v||*v!='\"')return;
    for(v++;*v&&*v!='\"'&&i+1<cap;v++){ if(*v=='\\'&&v[1])v++; out[i++]=*v; } out[i]=0;
}
static void json_escape(const char *src, char *dst, size_t cap){
    size_t o=0;
    if(cap==0) return;
    for(size_t i=0; src[i] && o+2<cap; i++){
        char c=src[i];
        if(c=='\"'){ dst[o++]='\\'; dst[o++]='\"'; }
        else if(c=='\\'){ dst[o++]='\\'; dst[o++]='\\'; }
        else if(c=='\n'){ dst[o++]='\\'; dst[o++]='n'; }
        else if(c=='\r'){ dst[o++]='\\'; dst[o++]='r'; }
        else if((unsigned char)c<0x20){  }
        else { dst[o++]=c; }
    }
    dst[o]='\0';
}

/* ────────────────────────────────────────────────────────────────────────────
 * Firebase Auth: базу могут читать и писать только запросы с ?auth=<idToken>.
 *
 * Раньше правила базы были открыты («users: .read true»), а URL лежал в
 * открытом репозитории — любой мог выкачать чужие пароли (они хранились в
 * открытом виде), затереть чат, слоты или баны и просто завалить базу
 * запросами. Теперь клиент логинится в Firebase Auth по e-mail/паролю
 * <ник>@cb4.game (пароль = игровой пароль + суффикс), получает idToken и
 * подписывает им каждый запрос. API-ключ ограничен нашим Android-пакетом
 * com.cb4 в Google Cloud Console, поэтому токен можно получить только из
 * нашего приложения, а правила firebase.rules.json пускают одних лишь
 * authenticated-запросов. Пароли из базы исчезают совсем.
 *
 * Если FB-ключ не задан (config.ds: FB_KEY=""), клиент работает по-старому —
 * это переходный режим для уже собранных версий.
 * ──────────────────────────────────────────────────────────────────────────── */
#define FB_EMAIL_DOMAIN "@cb4.game"
/* Пароль Firebase = игровой + суффикс: у Firebase минимум 6 символов, а игра
 * разрешает короткие пароли; суффикс добивает длину и не даёт использовать
 * базу паролей Firebase как самостоятельный вход. */
#define FB_PASS_SUFFIX "|cb4v1"
typedef struct {
    DSMutex lock;
    char key[128];
    char id_token[FB_ID_MAX];
    char refresh_token[FB_REFRESH_MAX];
    long long expires_at;   /* мс; обновляем за минуту до истечения */
} FirebaseAuth;
static FirebaseAuth fb = { .lock = DS_MUTEX_INIT };

void net_set_firebase_key(const char *key) {
    size_t n;
    if (!key) key = "";
    while (*key == ' ' || *key == '\t') key++;
    ds_mutex_lock(fb.lock);
    snprintf(fb.key, sizeof(fb.key), "%s", key);
    n = strlen(fb.key);
    while (n && (fb.key[n-1]==' ' || fb.key[n-1]=='\t' || fb.key[n-1]=='\r' || fb.key[n-1]=='\n'))
        fb.key[--n] = 0;
    ds_mutex_unlock(fb.lock);
    if (fb.key[0]) LOG("firebase auth enabled (key set)");
}
static int fb_enabled(void) { return fb.key[0] != 0; }

static void fb_store_tokens(const char *id_token, const char *refresh_token, double expires_in_sec) {
    ds_mutex_lock(fb.lock);
    snprintf(fb.id_token, sizeof(fb.id_token), "%s", id_token ? id_token : "");
    snprintf(fb.refresh_token, sizeof(fb.refresh_token), "%s", refresh_token ? refresh_token : "");
    if (expires_in_sec < 60) expires_in_sec = 60;
    fb.expires_at = now_ms() + (long long)(expires_in_sec * 1000.0) - 60000;
    ds_mutex_unlock(fb.lock);
}

/* URL auth-эндпоинтов. Боевой путь — googleapis.com; если же базой указан
 * локальный http:// тестовый сервер, auth-эндпоинты тоже ищутся на нём —
 * так тестовый прогон проходит ровно тот же код, что и бой. */
static void fb_auth_endpoint(char *dst, size_t cap, const char *path) {
    if (net.base[0] && !strncmp(net.base, "http://", 7)) {
        snprintf(dst, cap, "%s%s?key=%s", net.base, path, fb.key);
        return;
    }
    if (!strcmp(path, "/v1/token"))
        snprintf(dst, cap, "https://securetoken.googleapis.com/v1/token?key=%s", fb.key);
    else
        /* path уже содержит /v1/... (например /v1/accounts:signInWithPassword),
         * поэтому второй раз «v1» подставлять нельзя — иначе получится
         * /v1/v1/... и Firebase Auth ответит ошибкой, а клиент покажет
         * «No connection». */
        snprintf(dst, cap, "https://identitytoolkit.googleapis.com%s?key=%s", path, fb.key);
}

/* Обновление idToken по refreshToken (живёт ~1 час). 1 = ок. */
static int fb_refresh_token_now(void) {
    char url[FB_URL_MAX], body[768], resp[2048], rt[FB_REFRESH_MAX];
    int code;
    if (!fb_enabled()) return 0;
    ds_mutex_lock(fb.lock);
    snprintf(rt, sizeof(rt), "%s", fb.refresh_token);
    ds_mutex_unlock(fb.lock);
    if (!rt[0]) return 0;
    fb_auth_endpoint(url, sizeof(url), "/v1/token");
    snprintf(body, sizeof(body),
             "{\"grant_type\":\"refresh_token\",\"refresh_token\":\"%s\"}", rt);
    code = http("POST", url, body, resp, sizeof(resp));
    if (code == 200 && strstr(resp, "id_token")) {
        char idt[FB_ID_MAX], nrt[FB_REFRESH_MAX];
        strv(resp, "id_token", idt, sizeof(idt));
        strv(resp, "refresh_token", nrt, sizeof(nrt));
        if (idt[0]) {
            fb_store_tokens(idt, nrt[0] ? nrt : rt, num(resp, "expires_in", 3600));
            return 1;
        }
    }
    if (net_log_ok()) LOGERR("firebase token refresh failed: HTTP %d %.120s", code, resp);
    return 0;
}

/* Есть ли живой idToken (по необходимости обновляем). */
static int fb_ensure_token(void) {
    int ok = 0;
    long long now = now_ms();
    ds_mutex_lock(fb.lock);
    ok = fb.id_token[0] != 0 && now < fb.expires_at;
    ds_mutex_unlock(fb.lock);
    if (ok) return 1;
    return fb_refresh_token_now();
}

/* Копия url с приписанным ?auth=<idToken> (если токен есть). */
static void fb_url(char *dst, size_t cap, const char *url) {
    char tok[FB_ID_MAX];
    size_t n;
    if (!url) url = "";
    tok[0] = 0;
    if (fb_enabled()) {
        ds_mutex_lock(fb.lock);
        snprintf(tok, sizeof(tok), "%s", fb.id_token);
        ds_mutex_unlock(fb.lock);
    }
    if (!tok[0]) { snprintf(dst, cap, "%s", url); return; }
    n = (size_t)snprintf(dst, cap, "%s%sauth=%s", url, strchr(url, '?') ? "&" : "?", tok);
    if (n >= cap && net_log_ok()) LOGERR("firebase: auth URL truncated");
}

/* Все обращения к данным базы идут через fb_http*: без ключа это просто http,
 * с ключом — подписанный запрос плюс один повтор после обновления протухшего
 * токена. 403 — это отказ правил: ретраить такой ответ бессмысленно. */
static int fb_http_ex(const char *method,const char *url,const char *body,char *out,size_t cap,
                      const char *header,const char *value,char *etag,size_t etag_cap) {
    char authed[FB_URL_MAX];
    int code;
    if (!fb_enabled()) return http_ex(method,url,body,out,cap,header,value,etag,etag_cap);
    fb_ensure_token();
    fb_url(authed, sizeof(authed), url);
    code = http_ex(method, authed, body, out, cap, header, value, etag, etag_cap);
    if (code == 401 && fb_refresh_token_now()) {
        fb_url(authed, sizeof(authed), url);
        code = http_ex(method, authed, body, out, cap, header, value, etag, etag_cap);
    }
    if (code == 403 && net_log_ok())
        LOGERR("firebase: HTTP 403 (permission denied) — проверь firebase.rules.json и FB_KEY");
    return code;
}
static int fb_http(const char *method,const char *url,const char *body,char *out,size_t cap) {
    return fb_http_ex(method,url,body,out,cap,NULL,NULL,NULL,0);
}

/* E-mail аккаунта игры: <ник>@cb4.game, всегда в нижнем регистре — правила
 * базы сверяют токен с ником в нижнем регистре, а Firebase может
 * нормализовать регистр e-mail по-своему. Ник в /users сохраняет регистр. */
static void fb_email_of(const char *nick, char *dst, size_t cap) {
    static const char domain[] = FB_EMAIL_DOMAIN;
    size_t i = 0;
    if (!nick) nick = "";
    for (; nick[i] && i + sizeof(domain) < cap; i++) {
        char c = nick[i];
        dst[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    memcpy(dst + i, domain, sizeof(domain));
}

static int fb_err_is(const char *resp, const char *code_name) {
    return resp && strstr(resp, code_name) != NULL;
}

/* Вход/регистрация в Firebase Auth.
 * 0 = ок (*registered=1, если аккаунт создан сейчас), 1 = неверный пароль,
 * 2 = ошибка сети/сервера. Ник уже проверен nick_valid(): в защищённом
 * режиме допустимы только латиница/цифры/«_» — из них собирается e-mail. */
static int fb_sign_in(const char *nick, const char *pass, int *registered) {
    char url[FB_URL_MAX], body[512], resp[2048];
    char email[96], epass[192], idt[FB_ID_MAX], rt[FB_REFRESH_MAX];
    int code;
    if (registered) *registered = 0;
    fb_email_of(nick, email, sizeof(email));
    snprintf(epass, sizeof(epass), "%s%s", pass, FB_PASS_SUFFIX);

    fb_auth_endpoint(url, sizeof(url), "/v1/accounts:signInWithPassword");
    snprintf(body, sizeof(body),
             "{\"email\":\"%s\",\"password\":\"%s\",\"returnSecureToken\":true}", email, epass);
    code = http("POST", url, body, resp, sizeof(resp));
    idt[0] = 0; rt[0] = 0;
    if (code == 200) strv(resp, "idToken", idt, sizeof(idt));
    if (code == 200 && idt[0]) {
        strv(resp, "refreshToken", rt, sizeof(rt));
        fb_store_tokens(idt, rt, num(resp, "expiresIn", 3600));
        return 0;
    }
    if (code == 0) {
        if (net_log_ok()) LOGERR("firebase auth: network error");
        return 2;
    }
    /* EMAIL_NOT_FOUND — игрока ещё нет, регистрируем. С включённой защитой от
     * перебора e-mail тот же ответ приходит и на неверный пароль, поэтому
     * пробуем signUp в обоих случаях: EMAIL_EXISTS означает «аккаунт есть,
     * значит пароль не подошёл». */
    if (!fb_err_is(resp, "EMAIL_NOT_FOUND") &&
        !fb_err_is(resp, "INVALID_PASSWORD") &&
        !fb_err_is(resp, "INVALID_LOGIN_CREDENTIALS")) {
        if (net_log_ok()) LOGERR("firebase signIn: HTTP %d %.160s", code, resp);
        return 2;
    }

    fb_auth_endpoint(url, sizeof(url), "/v1/accounts:signUp");
    snprintf(body, sizeof(body),
             "{\"email\":\"%s\",\"password\":\"%s\",\"returnSecureToken\":true}", email, epass);
    code = http("POST", url, body, resp, sizeof(resp));
    idt[0] = 0; rt[0] = 0;
    if (code == 200) strv(resp, "idToken", idt, sizeof(idt));
    if (code == 200 && idt[0]) {
        strv(resp, "refreshToken", rt, sizeof(rt));
        fb_store_tokens(idt, rt, num(resp, "expiresIn", 3600));
        if (registered) *registered = 1;
        return 0;
    }
    if (fb_err_is(resp, "EMAIL_EXISTS") || fb_err_is(resp, "WEAK_PASSWORD")) {
        /* Аккаунт существует, но пароль не подошёл (или гонка регистраций). */
        if (net_log_ok()) LOG("firebase auth: wrong password for '%s'", nick);
        return 1;
    }
    if (net_log_ok()) LOGERR("firebase signUp: HTTP %d %.160s", code, resp);
    return 2;
}

/* Разбор записи /users/<ник> в локальный прогресс (без пароля: в защищённом
 * режиме пароль проверяет Firebase Auth, а не база). */
static void apply_user_json(const char *resp) {
    int cups = (int)num(resp, "cups", 0);
    int candies = (int)num(resp, "candies", 0);
    int cls = (int)num(resp, "cls", 0);
    int azum = (int)num(resp, "azum", 0);
    int santa = (int)num(resp, "santa", 0);
    int ebuc = (int)num(resp, "ebuc", 0);
    int level = (int)num(resp, "level", 0);
    int levels_unlocked = (int)num(resp, "levels", -1);
    int ordinary_level = (int)num(resp, "ordinary_level", -1);
    int ordinary_levels_unlocked = (int)num(resp, "ordinary_levels", -1);
    int azum_level = (int)num(resp, "azum_level", -1);
    int azum_levels_unlocked = (int)num(resp, "azum_levels", -1);
    int santa_level = (int)num(resp, "santa_level", -1);
    int santa_levels_unlocked = (int)num(resp, "santa_levels", -1);
    int ebuc_level = (int)num(resp, "ebuc_level", -1);
    int ebuc_levels_unlocked = (int)num(resp, "ebuc_levels", -1);
    int bp_level = (int)num(resp, "bp", 0);
    int azum_skin = (int)num(resp, "azum_skin", 0);
    if (levels_unlocked < 0) levels_unlocked = level;
    if (cups < 0) cups = 0;
    if (candies < 0) candies = 0;
    if (cls != 1 && cls != 2 && cls != 3) cls = 0;
    azum = azum ? 1 : 0;
    santa = santa ? 1 : 0;
    ebuc = ebuc ? 1 : 0;
    if (cls == 1 && !azum) cls = 0;
    if (cls == 2 && !santa) cls = 0;
    if (cls == 3 && !ebuc) cls = 0;
    if (ordinary_level < 0 && ordinary_levels_unlocked < 0 &&
        azum_level < 0 && azum_levels_unlocked < 0 &&
        santa_level < 0 && santa_levels_unlocked < 0 &&
        ebuc_level < 0 && ebuc_levels_unlocked < 0) {
        ordinary_level=ordinary_levels_unlocked=0;
        azum_level=azum_levels_unlocked=0;
        santa_level=santa_levels_unlocked=0;
        ebuc_level=ebuc_levels_unlocked=0;
        if (cls==1 && azum) { azum_level=level; azum_levels_unlocked=levels_unlocked; }
        else if (cls==2 && santa) { santa_level=level; santa_levels_unlocked=levels_unlocked; }
        else if (cls==3 && ebuc) { ebuc_level=level; ebuc_levels_unlocked=levels_unlocked; }
        else { ordinary_level=level; ordinary_levels_unlocked=levels_unlocked; }
    } else {
        if (ordinary_level < 0) ordinary_level = 0;
        if (ordinary_levels_unlocked < 0) ordinary_levels_unlocked = ordinary_level;
        if (azum_level < 0) azum_level = 0;
        if (azum_levels_unlocked < 0) azum_levels_unlocked = azum_level;
        if (santa_level < 0) santa_level = 0;
        if (santa_levels_unlocked < 0) santa_levels_unlocked = santa_level;
        if (ebuc_level < 0) ebuc_level = 0;
        if (ebuc_levels_unlocked < 0) ebuc_levels_unlocked = ebuc_level;
    }
    normalize_level_pair(&level, &levels_unlocked);
    normalize_level_pair(&ordinary_level, &ordinary_levels_unlocked);
    normalize_level_pair(&azum_level, &azum_levels_unlocked);
    normalize_level_pair(&santa_level, &santa_levels_unlocked);
    normalize_level_pair(&ebuc_level, &ebuc_levels_unlocked);
    if (cls == 0) { level=ordinary_level; levels_unlocked=ordinary_levels_unlocked; }
    else if (cls == 1) { level=azum_level; levels_unlocked=azum_levels_unlocked; }
    else if (cls == 2) { level=santa_level; levels_unlocked=santa_levels_unlocked; }
    else { level=ebuc_level; levels_unlocked=ebuc_levels_unlocked; }
    if (bp_level < 0) bp_level = 0;
    if (bp_level > BP_MAX) bp_level = BP_MAX;
    azum_skin = azum_skin ? 1 : 0;
    pg_lock();
    pg.cups = cups; pg.candies = candies; pg.cls = cls; pg.azum = azum; pg.santa = santa; pg.ebuc = ebuc;
    pg.level = level; pg.levels_unlocked = levels_unlocked;
    pg.ordinary_level = ordinary_level; pg.ordinary_levels_unlocked = ordinary_levels_unlocked;
    pg.azum_level = azum_level; pg.azum_levels_unlocked = azum_levels_unlocked;
    pg.santa_level = santa_level; pg.santa_levels_unlocked = santa_levels_unlocked;
    pg.ebuc_level = ebuc_level; pg.ebuc_levels_unlocked = ebuc_levels_unlocked;
    pg.bp_level = bp_level; pg.azum_skin = azum_skin;
    pg.loaded = 1;
    pg_unlock();
    pending_cls = cls;
    pending_level = level;
    progress_write(cups, candies, cls, azum, santa, ebuc, level, levels_unlocked,
                   ordinary_level, ordinary_levels_unlocked,
                   azum_level, azum_levels_unlocked,
                   santa_level, santa_levels_unlocked,
                   ebuc_level, ebuc_levels_unlocked,
                   bp_level, azum_skin);
}

/* Протолкнуть текущий локальный прогресс в облако (только после того, как
 * сессия логина уже установлена — иначе PATCH уйдёт без авторизации). */
static void push_pg_to_cloud(void) {
    int cups, candies, cls, azum, santa, ebuc, lv, lu;
    int ol, ou, al, au, sl, su, el, eul, bp, sk;
    pg_lock();
    cups = pg.cups; candies = pg.candies; cls = pg.cls;
    azum = pg.azum; santa = pg.santa; ebuc = pg.ebuc;
    lv = pg.level; lu = pg.levels_unlocked;
    ol = pg.ordinary_level; ou = pg.ordinary_levels_unlocked;
    al = pg.azum_level; au = pg.azum_levels_unlocked;
    sl = pg.santa_level; su = pg.santa_levels_unlocked;
    el = pg.ebuc_level; eul = pg.ebuc_levels_unlocked;
    bp = pg.bp_level; sk = pg.azum_skin;
    pg_unlock();
    net_save_progress_all(cups, candies, cls, azum, santa, ebuc,
                          lv, lu, ol, ou, al, au, sl, su, el, eul, bp, sk);
}

/* Раньше правила Firebase не пропускали поля ebuc / ebuc_level / ebuc_levels и
 * cls=3, поэтому покупка бука не попадала в облако: после перезахода облачная
 * запись «стирала» локально купленный класс. Теперь, если на устройстве бук
 * уже куплен (progress.dat), а облачная запись его не знает, возвращаем
 * локальную покупку. Возврат 1 — облако нужно обновить после логина. */
static int apply_user_json_keep_local(const char *resp) {
    int had = 0, local_ebuc = 0, local_cls = 0, local_el = 0, local_eu = 0;
    progress_read();
    pg_lock();
    if (pg.loaded) { had = 1; local_ebuc = pg.ebuc; local_cls = pg.cls;
                    local_el = pg.ebuc_level; local_eu = pg.ebuc_levels_unlocked; }
    pg_unlock();
    apply_user_json(resp);
    if (!had || local_ebuc != 1) return 0;
    pg_lock();
    if (pg.ebuc != 0) { pg_unlock(); return 0; }
    pg.ebuc = 1;
    if (local_cls == 3) pg.cls = 3;
    if (local_el > pg.ebuc_level) pg.ebuc_level = local_el;
    if (local_eu > pg.ebuc_levels_unlocked) pg.ebuc_levels_unlocked = local_eu;
    int cups = pg.cups, candies = pg.candies, cls = pg.cls;
    int azum = pg.azum, santa = pg.santa, lv = pg.level, lu = pg.levels_unlocked;
    int ol = pg.ordinary_level, ou = pg.ordinary_levels_unlocked;
    int al = pg.azum_level, au = pg.azum_levels_unlocked;
    int sl = pg.santa_level, su = pg.santa_levels_unlocked;
    int el = pg.ebuc_level, eul = pg.ebuc_levels_unlocked;
    int bp = pg.bp_level, sk = pg.azum_skin;
    pg_unlock();
    pending_cls = cls; pending_level = lv;
    progress_write(cups, candies, cls, azum, santa, 1, lv, lu,
                   ol, ou, al, au, sl, su, el, eul, bp, sk);
    LOG("restored locally bought ebuc (cloud sync after login)");
    return 1;
}

/* Создание записи нового игрока. with_pass — только для старого режима без
 * Firebase Auth: там пароль хранится в базе (и это плохо). */
static int put_default_user(const char *req_url, const char *nick, const char *pass) {
    char body[BODY], epass[128], enick[64];
    json_escape(pass ? pass : "", epass, sizeof(epass));
    json_escape(nick, enick, sizeof(enick));
    snprintf(body, sizeof(body),
             "{\"nick\":\"%s\",\"cups\":0,\"candies\":0,\"cls\":0,\"azum\":0,\"santa\":0,\"ebuc\":0,"
             "\"level\":0,\"levels\":0,\"ordinary_level\":0,\"ordinary_levels\":0,"
             "\"azum_level\":0,\"azum_levels\":0,\"santa_level\":0,\"santa_levels\":0,"
             "\"ebuc_level\":0,\"ebuc_levels\":0,\"bp\":0,\"azum_skin\":0%s%s}",
             enick, fb_enabled() ? "" : ",\"pass\":\"", fb_enabled() ? "" : epass);
    return fb_http("PUT", req_url, body, NULL, 0);
}

static void net_auth_session_ok(const char *nick, const char *pass) {
    /* Если на устройстве уже есть оффлайн-прогресс (купленные классы,
     * валюты, уровни), создание нового аккаунта не должно его стирать:
     * переносим всё в облачную запись нового ника. */
    int keep = 0;
    int cups = 0, candies = 0, cls = 0, azum = 0, santa = 0, ebuc = 0;
    int lv = 0, lu = 0, ol = 0, ou = 0, al = 0, au = 0, sl = 0, su = 0, el = 0, eul = 0;
    int bp = 0, sk = 0;
    progress_read();
    pg_lock();
    if (pg.loaded) {
        cups = pg.cups; candies = pg.candies; cls = pg.cls;
        azum = pg.azum; santa = pg.santa; ebuc = pg.ebuc;
        lv = pg.level; lu = pg.levels_unlocked;
        ol = pg.ordinary_level; ou = pg.ordinary_levels_unlocked;
        al = pg.azum_level; au = pg.azum_levels_unlocked;
        sl = pg.santa_level; su = pg.santa_levels_unlocked;
        el = pg.ebuc_level; eul = pg.ebuc_levels_unlocked;
        bp = pg.bp_level; sk = pg.azum_skin;
        if (cups > 0 || candies > 0 || azum || santa || ebuc || cls != 0 || bp > 0) keep = 1;
    }
    pg_unlock();
    if (keep) {
        lg_lock();
        snprintf(lg.session_nick, sizeof(lg.session_nick), "%s", nick);
        snprintf(lg.session_pass, sizeof(lg.session_pass), "%s", pass);
        lg.status = NET_LOGIN_OK;
        lg_unlock();
        session_save(nick, pass);
        net_save_progress_all(cups, candies, cls, azum, santa, ebuc,
                              lv, lu, ol, ou, al, au, sl, su, el, eul, bp, sk);
        LOG("moved local progress into new account '%s'", nick);
        return;
    }
    pg_lock();
    pg.cups = 0; pg.candies = 0; pg.cls = 0; pg.azum = 0; pg.santa = 0; pg.ebuc = 0;
    pg.level = 0; pg.levels_unlocked = 0;
    pg.ordinary_level = 0; pg.ordinary_levels_unlocked = 0;
    pg.azum_level = 0; pg.azum_levels_unlocked = 0;
    pg.santa_level = 0; pg.santa_levels_unlocked = 0;
    pg.ebuc_level = 0; pg.ebuc_levels_unlocked = 0;
    pg.bp_level = 0; pg.azum_skin = 0;
    pg.loaded = 1;
    pg_unlock();
    pending_cls = 0; pending_level = 0;
    progress_write(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    lg_lock();
    snprintf(lg.session_nick, sizeof(lg.session_nick), "%s", nick);
    snprintf(lg.session_pass, sizeof(lg.session_pass), "%s", pass);
    lg.status = NET_LOGIN_OK;
    lg_unlock();
    session_save(nick, pass);
}

/* В защищённом режиме ник обязан быть ASCII (латиница/цифры/«_»): из него
 * собирается e-mail аккаунта Firebase Auth. */
static int nick_ascii_ok(const char *nick) {
    for (const char *p = nick; p && *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_'))
            return 0;
    }
    return 1;
}

double net_auth(const char *url, const char *nick, const char *pass) {
    if (!nick || !nick_valid(nick)) return (double)NET_LOGIN_BAD_NICK;
    if (!pass || !pass_valid(pass)) return (double)NET_LOGIN_BAD_PASS;
    const char *base = (url && *url) ? url : net.base;
    if (!base || !*base) return (double)NET_ERROR;
    if (!net.base[0] && url && *url) {
        snprintf(net.base, sizeof(net.base), "%s", url);
        size_t n = strlen(net.base);
        while (n && net.base[n - 1] == '/') net.base[--n] = 0;
    }

    char req_url[URL], resp[RESP];
    snprintf(req_url, sizeof(req_url), "%s/users/%s.json", net.base, nick);

    /* ── Защищённый режим: пароль проверяет Firebase Auth, база пароли не
     * хранит. Токен подписывает и все остальные запросы (fb_http). ── */
    if (fb_enabled()) {
        int registered = 0;
        int r;
        if (!nick_ascii_ok(nick)) {
            /* Из ника собирается e-mail аккаунта, поэтому в защищённом режиме
             * допустимы только латиница, цифры и «_». */
            LOGERR("'%s': in secured mode nick must be latin letters, digits or '_'", nick);
            return (double)NET_LOGIN_BAD_NICK;
        }
        r = fb_sign_in(nick, pass, &registered);
        if (r == 1) return (double)NET_LOGIN_WRONG_PASS;
        if (r != 0) return (double)NET_ERROR;
        int code = fb_http("GET", req_url, NULL, resp, sizeof(resp));
        const char *p = skip_ws(resp);
        if (code == 0) {
            if (net_log_ok()) LOGERR("net_auth: network error connecting to %s", req_url);
            return (double)NET_ERROR;
        }
        if (code != 200 || !p || !*p || !strncmp(p, "null", 4)) {
            /* Записи ещё нет (новый аккаунт или старый без записи) — создаём
             * дефолтную без пароля. */
            int put_code = put_default_user(req_url, nick, NULL);
            if (put_code != 200) {
                if (net_log_ok()) LOGERR("net_auth: create user record failed HTTP %d", put_code);
                return (double)NET_ERROR;
            }
            net_auth_session_ok(nick, pass);
            LOG("%s and logged in: '%s'", registered ? "registered" : "account restored", nick);
            return (double)NET_LOGIN_OK;
        }
        if (*p == '{') {
            int restored = apply_user_json_keep_local(resp);
            lg_lock();
            snprintf(lg.session_nick, sizeof(lg.session_nick), "%s", nick);
            snprintf(lg.session_pass, sizeof(lg.session_pass), "%s", pass);
            lg.status = NET_LOGIN_OK;
            lg_unlock();
            session_save(nick, pass);
            if (restored) push_pg_to_cloud();
            LOG("logged in: '%s' (cups=%d, candies=%d)", nick,
                (int)num(resp, "cups", 0), (int)num(resp, "candies", 0));
            return (double)NET_LOGIN_OK;
        }
        return (double)NET_ERROR;
    }

    /* ── Переходный режим без ключа Firebase: как раньше, пароль в базе. ── */
    int code = http("GET", req_url, NULL, resp, sizeof(resp));
    if (code == 0) {
        if (net_log_ok()) LOGERR("net_auth: network error connecting to %s", req_url);
        return (double)NET_ERROR;
    }

    const char *p = skip_ws(resp);
    if (!p || !*p || !strncmp(p, "null", 4) || code == 404) {
        int put_code = put_default_user(req_url, nick, pass);
        if (put_code != 200) {
            if (net_log_ok()) LOGERR("net_auth: register failed HTTP %d", put_code);
            return (double)NET_ERROR;
        }
        net_auth_session_ok(nick, pass);
        LOG("registered and logged in: '%s'", nick);
        return (double)NET_LOGIN_OK;
    }

    if (*p == '{') {
        char got_pass[64] = "";
        strv(resp, "pass", got_pass, sizeof(got_pass));
        if (strcmp(got_pass, pass) == 0) {
            int restored = apply_user_json_keep_local(resp);
            lg_lock();
            snprintf(lg.session_nick, sizeof(lg.session_nick), "%s", nick);
            snprintf(lg.session_pass, sizeof(lg.session_pass), "%s", pass);
            lg.status = NET_LOGIN_OK;
            lg_unlock();
            session_save(nick, pass);
            if (restored) push_pg_to_cloud();
            LOG("logged in: '%s' (cups=%d, candies=%d)", nick,
                (int)num(resp, "cups", 0), (int)num(resp, "candies", 0));
            return (double)NET_LOGIN_OK;
        } else {
            if (net_log_ok()) LOG("wrong password for '%s'", nick);
            return (double)NET_LOGIN_WRONG_PASS;
        }
    }

    return (double)NET_ERROR;
}

double net_set_nick(const char *nick) {
    if (!nick || !nick_valid(nick)) return 0.0;
    /* В защищённом режиме «примерить» чужой ник с дефолтным паролем нельзя:
     * это создало бы аккаунт Firebase Auth на чужое имя. Ник меняется только
     * через полноценный net_auth со своим паролем. */
    if (fb_enabled()) return 0.0;
    return net_auth(net.base, nick, "123456") == (double)NET_LOGIN_OK ? 1.0 : 0.0;
}

void net_logout(void) {
    char path[320];
    lg_lock();
    lg.session_nick[0] = 0;
    lg.session_pass[0] = 0;
    lg.status = NET_LOGIN_IDLE;
    lg_unlock();
    if (lg.path[0]) snprintf(path, sizeof(path), "%s/%s", lg.path, SESSION_FILE);
    else snprintf(path, sizeof(path), "%s", SESSION_FILE);
    remove(path);
}

void net_set_data_path(const char *path) {
    lg_lock();
    if (path && *path) snprintf(lg.path, sizeof(lg.path), "%s", path);
    else lg.path[0] = 0;
    lg_unlock();
}

void net_autologin(const char *url) {
    char path[320], nick[LOGIN_NICK_MAX+2] = "", pass[64] = "";
    FILE *f;
    lg_lock();
    if (lg.status == NET_LOGIN_OK) { lg_unlock(); return; }
    if (lg.path[0]) snprintf(path, sizeof(path), "%s/%s", lg.path, SESSION_FILE);
    else snprintf(path, sizeof(path), "%s", SESSION_FILE);
    lg_unlock();
    f = fopen(path, "r");
    if (!f) return;
    int n = fscanf(f, "%17s %63s", nick, pass);
    fclose(f);
    if (n < 1 || !nick_valid(nick)) return;
    if (url && *url && !net.base[0]) {
        snprintf(net.base, sizeof(net.base), "%s", url);
        size_t bl = strlen(net.base);
        while (bl && net.base[bl - 1] == '/') net.base[--bl] = 0;
    }
    if (pass[0] && url && *url) {
        double res = net_auth(url, nick, pass);
        if (res == 2.0) {
            LOG("autologin (cloud): nick '%s'", nick);
            return;
        }
    }
    lg_lock();
    snprintf(lg.session_nick, sizeof(lg.session_nick), "%s", nick);
    snprintf(lg.session_pass, sizeof(lg.session_pass), "%s", pass);
    lg.status = NET_LOGIN_OK;
    lg_unlock();
    LOG("autologin (cached): nick '%s'", nick);
}

double net_login_status(void) {
    double v;
    lg_lock(); v = (double)lg.status; lg_unlock();
    return v;
}
const char *net_login_nick(void) {
    lg_lock();
    if (lg.status == NET_LOGIN_OK && lg.session_nick[0])
        snprintf(s_lg_nick_ret, sizeof(s_lg_nick_ret), "%s", lg.session_nick);
    else s_lg_nick_ret[0] = 0;
    lg_unlock();
    return s_lg_nick_ret;
}
const char *net_login_pass(void) {
    lg_lock();
    if (lg.status == NET_LOGIN_OK && lg.session_pass[0])
        snprintf(s_lg_pass_ret, sizeof(s_lg_pass_ret), "%s", lg.session_pass);
    else s_lg_pass_ret[0] = 0;
    lg_unlock();
    return s_lg_pass_ret;
}

/* Бан система */
static void bans_file_path(char *path, size_t cap) {
    data_file_path(path, cap, BANS_FILE);
}
static int is_banned_in_file(const char *nick) {
    if (!nick || !*nick) return 0;
    char path[320];
    bans_file_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[64];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        // trim whitespace
        size_t len = strlen(line);
        while (len && (line[len-1]=='\n'||line[len-1]=='\r'||line[len-1]==' '||line[len-1]=='\t')) line[--len]=0;
        char *p = line;
        while (*p==' '||*p=='\t') p++;
        if (strcmp(p, nick)==0) { found=1; break; }
    }
    fclose(f);
    return found;
}
static void add_ban_to_file(const char *nick) {
    if (!nick || !*nick) return;
    if (is_banned_in_file(nick)) return;
    char path[320];
    bans_file_path(path, sizeof(path));
    FILE *f = fopen(path, "a");
    if (f) { fprintf(f, "%s\n", nick); fclose(f); }
}
static void remove_ban_from_file(const char *nick) {
    if (!nick || !*nick) return;
    char path[320];
    bans_file_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return;
    char lines[256][64]; int count=0;
    char line[64];
    while (fgets(line, sizeof(line), f) && count<256) {
        size_t len = strlen(line);
        while (len && (line[len-1]=='\n'||line[len-1]=='\r')) line[--len]=0;
        char *p = line;
        while (*p==' '||*p=='\t') p++;
        if (!*p) continue;
        if (strcmp(p, nick)!=0) {
            snprintf(lines[count], sizeof(lines[count]), "%s", p);
            count++;
        }
    }
    fclose(f);
    f = fopen(path, "w");
    if (f) {
        for (int i=0;i<count;i++) fprintf(f, "%s\n", lines[i]);
        fclose(f);
    }
}

/* Кэш облачного списка банов: заменяется целиком при каждом ban_sync(). */
static int ban_cache_has(const char *nick) {
    int i, found = 0;
    if (!nick || !*nick) return 0;
    ban_lock();
    for (i = 0; i < banlist.count; i++) {
        if (strcmp(banlist.nick[i], nick) == 0) { found = 1; break; }
    }
    ban_unlock();
    return found;
}
static void ban_cache_put(const char *nick) {
    if (!nick || !*nick) return;
    ban_lock();
    if (banlist.count < BAN_MAX) {
        int i, dup = 0;
        for (i = 0; i < banlist.count; i++) {
            if (strcmp(banlist.nick[i], nick) == 0) { dup = 1; break; }
        }
        if (!dup) snprintf(banlist.nick[banlist.count++], LOGIN_NICK_MAX + 1, "%s", nick);
    }
    ban_unlock();
}
static void ban_cache_drop(const char *nick) {
    int i, j;
    if (!nick || !*nick) return;
    ban_lock();
    for (i = 0, j = 0; i < banlist.count; i++) {
        if (strcmp(banlist.nick[i], nick) == 0) continue;
        if (j != i) snprintf(banlist.nick[j], LOGIN_NICK_MAX + 1, "%s", banlist.nick[i]);
        j++;
    }
    banlist.count = j;
    ban_unlock();
}
static int ban_synced(void) { int v; ban_lock(); v = banlist.synced; ban_unlock(); return v; }
static void ban_invalidate(void) { ban_lock(); banlist.synced = 0; ban_unlock(); }

/* Читает /bans.json целиком. Возвращает 1, если список удалось обновить. */
static int ban_sync(void) {
    char url[URL], resp[RESP], keys[BAN_MAX][LOGIN_NICK_MAX + 1];
    const char *p;
    int code, n = 0;
    if (!net.base[0]) return 0;
    snprintf(url, sizeof(url), "%s/bans.json", net.base);
    code = fb_http("GET", url, NULL, resp, sizeof(resp));
    if (code != 200) {
        if (net_log_ok()) LOGERR("ban list: HTTP %d", code);
        return 0;
    }
    p = skip_ws(resp);
    if (!p || !*p || !strncmp(p, "null", 4)) {
        ban_lock();
        banlist.count = 0; banlist.synced = 1; banlist.synced_at = now_ms();
        ban_unlock();
        return 1;
    }
    if (*p != '{') return 0;
    for (p = skip_ws(p + 1); p && *p && *p != '}' && n < BAN_MAX; ) {
        const char *kend;
        size_t kl;
        if (*p != '"') break;
        kend = skip_str(p);
        if (!kend) break;
        kl = (size_t)(kend - p - 2);
        if (kl > LOGIN_NICK_MAX) kl = LOGIN_NICK_MAX;
        memcpy(keys[n], p + 1, kl);
        keys[n][kl] = 0;
        n++;
        p = skip_ws(kend);
        if (*p != ':') break;
        p = skip_val(skip_ws(p + 1));
        if (!p) break;
        p = skip_ws(p);
        if (*p == ',') p = skip_ws(p + 1);
    }
    ban_lock();
    banlist.count = 0;
    for (int i = 0; i < n; i++) snprintf(banlist.nick[i], LOGIN_NICK_MAX + 1, "%s", keys[i]);
    banlist.count = n; banlist.synced = 1; banlist.synced_at = now_ms();
    ban_unlock();
    LOG("ban list: %d entry(ies)", n);
    return 1;
}

/* Забанен ли ник по данным этого клиента: облачный список или локальный bans.dat. */
static int nick_banned_locally(const char *nick) {
    if (!nick || !*nick) return 0;
    return ban_cache_has(nick) || is_banned_in_file(nick);
}

/* Синхронная облачная проверка бана: только для фоновых потоков сети
 * (thread_main), из игрового потока её звать нельзя — блокирует кадр. */
static int ban_check_cloud_sync(const char *nick) {
    char url[URL], resp[RESP];
    const char *p;
    int code;
    if (!nick || !*nick) return 0;
    if (nick_banned_locally(nick)) return 1;
    if (!net.base[0]) return 0;
    snprintf(url, sizeof(url), "%s/bans/%s.json", net.base, nick);
    code = fb_http("GET", url, NULL, resp, sizeof(resp));
    if (code == 200) {
        p = skip_ws(resp);
        if (p && *p && strncmp(p, "null", 4) != 0) {
            // true или объект — считаем забаненным
            ban_cache_put(nick);
            return 1;
        }
    } else if (net_log_ok()) {
        LOGERR("ban check '%s': HTTP %d", nick, code);
    }
    return 0;
}

/* Фоновая проверка бана: результат оседает в кеше, игровой поток не ждёт. */
typedef struct { char nick[LOGIN_NICK_MAX + 1]; } BanCheckJob;
static void *ban_check_job(void *arg) {
    BanCheckJob *j = (BanCheckJob *)arg;
    if (j) { ban_check_cloud_sync(j->nick); free(j); }
    return NULL;
}

/* Забанен ли ник с точки зрения игрового потока. Раньше здесь был
 * синхронный HTTP-запрос: при отсутствии соединения он держал игровой
 * поток до таймаута сети, кадры и ввод замирали, и система показывала
 * «Приложение не отвечает». Теперь ответ даётся мгновенно по локальному
 * кешу, а облачная проверка уходит в отдельный поток и пополняет кеш. */
double net_is_banned(const char *nick) {
    static long long s_last_check;
    long long t;
    if (!nick || !*nick) return 0;
    if (nick_banned_locally(nick)) return 1;
    if (!net.base[0]) return 0;
    t = now_ms();
    ban_lock();
    if (t - s_last_check < 1500) { ban_unlock(); return 0; }
    s_last_check = t;
    ban_unlock();
    {
        BanCheckJob *j = (BanCheckJob *)malloc(sizeof(*j));
        DSThread th;
        if (!j) return 0;
        snprintf(j->nick, sizeof(j->nick), "%s", nick);
        th = ds_thread_start(ban_check_job, j);
        if (th) ds_thread_detach(th);
        else free(j);
    }
    return 0;
}

double net_banned(void) {
    double v;
    lock(); v = net.self_banned ? 1 : 0; unlock();
    return v;
}

/* Пишет бан в облако отдельным потоком и проверяет, что запись прошла. */
typedef struct { char url[FB_URL_MAX]; char nick[LOGIN_NICK_MAX + 1]; int banned; } BanJob;
static void *ban_write_job(void *arg) {
    BanJob *j = (BanJob *)arg;
    int code;
    if (!j) return NULL;
    if (j->banned) {
        code = http("PUT", j->url, "true", NULL, 0);
        if (code == 200) {
            LOG("ban %s: saved to cloud", j->nick);
            ban_sync();
        } else {
            LOGERR("ban %s: cloud write FAILED (HTTP %d), ban is local only", j->nick, code);
        }
    } else {
        code = http("DELETE", j->url, NULL, NULL, 0);
        if (code == 200) {
            LOG("unban %s: removed from cloud", j->nick);
            ban_sync();
        } else {
            LOGERR("unban %s: cloud delete FAILED (HTTP %d)", j->nick, code);
        }
    }
    free(j);
    return NULL;
}
static void ban_write_async(const char *url, const char *nick, int banned) {
    BanJob *j = (BanJob *)malloc(sizeof(*j));
    DSThread t;
    if (!j) return;
    fb_url(j->url, sizeof(j->url), url);
    snprintf(j->nick, sizeof(j->nick), "%s", nick);
    j->banned = banned;
    t = ds_thread_start(ban_write_job, j);
    if (t) { ds_thread_detach(t); return; }
    free(j);
}

void net_ban_set(const char *nick, double banned) {
    if (!nick || !*nick) return;
    if (!nick_valid(nick)) {
        LOGERR("ban: invalid nick '%s'", nick);
        return;
    }
    if (banned >= 0.5) {
        add_ban_to_file(nick);
        ban_cache_put(nick);
        if (net.base[0]) {
            char url[URL];
            snprintf(url, sizeof(url), "%s/bans/%s.json", net.base, nick);
            ban_write_async(url, nick, 1);
        } else {
            LOGERR("ban %s: no server, ban is local only", nick);
        }
        LOG("ban %s", nick);
    } else {
        remove_ban_from_file(nick);
        ban_cache_drop(nick);
        if (net.base[0]) {
            char url[URL];
            snprintf(url, sizeof(url), "%s/bans/%s.json", net.base, nick);
            ban_write_async(url, nick, 0);
        }
        LOG("unban %s", nick);
    }
}

/* event — скаляр, поэтому PUT, а не PATCH: Firebase принимает PATCH только с
 * объектом в теле («Patch requires a JSON object ...»), и запись с телом "2"
 * сервер отклонял — ивент загорался лишь у того, кто ввёл команду, и пропадал
 * после следующего опроса базы. Пишем асинхронно и проверяем код ответа, как в
 * банах: молчаливый «успех» здесь так же не нужен. */
static void *event_write_job(void *arg) {
    HttpJob *j = (HttpJob *)arg;
    int code;
    if (!j) return NULL;
    code = http("PUT", j->url, j->body, NULL, 0);
    if (code == 200) LOG("event saved to cloud (%s)", j->body);
    else LOGERR("event write FAILED (HTTP %d), event is local only", code);
    free(j);
    return NULL;
}
static void event_write_async(const char *url, const char *body) {
    HttpJob *j = (HttpJob*)malloc(sizeof(*j));
    DSThread t;
    if (!j) return;
    fb_url(j->url, sizeof(j->url), url);
    snprintf(j->body, sizeof(j->body), "%s", body);
    t = ds_thread_start(event_write_job, j);
    if (t) { ds_thread_detach(t); return; }
    free(j);
}

void net_event_set(double mode) {
    int m = (int)mode;
    if (m<0) m=0;
    if (m>2) m=2;
    if (!net.base[0]) return;
    char url[URL], body[32];
    snprintf(url, sizeof(url), "%s/event.json", net.base);
    snprintf(body, sizeof(body), "%d", m);
    event_write_async(url, body);
    lock(); net.event=m; unlock();
    LOG("event set %d", m);
}

/* ── Команда админа "text <сообщение> <цвет>" и баннер ──────────────────────
 * Разбор команды живёт в C, а не в скрипте: из DimScript нет подстрок, только
 * str_eq/str_len. Формат: "text hello, world! green" — сообщение всё между
 * "text " и последним словом; последнее слово, если это известное имя цвета,
 * становится цветом баннера, иначе остаётся частью сообщения (цвет — белый).
 */
static const char *banner_color_names[] = {
    "white", "red", "green", "blue", "yellow", "orange", "purple", "pink",
    "cyan", "black"
};
static int banner_color_known(const char *word) {
    size_t i;
    if (!word || !*word) return 0;
    for (i = 0; i < sizeof(banner_color_names)/sizeof(banner_color_names[0]); i++)
        if (!strcmp(word, banner_color_names[i])) return 1;
    return 0;
}
double net_chat_is_text_cmd(const char *msg) {
    if (!msg) return 0;
    if (strlen(msg) >= 5 && strncmp(msg, "text ", 5) == 0) {
        const char *t = msg + 5;
        while (*t==' '||*t=='\t') t++;
        if (*t) return 1;
    }
    return 0;
}
static void banner_split_cmd(const char *msg, char *text_out, size_t tcap, char *color_out, size_t ccap) {
    const char *t, *c, *last_space = NULL;
    char word[BANNER_COLOR_MAX];
    size_t n;
    if (tcap) text_out[0] = 0;
    snprintf(color_out, ccap, "white");
    if (!msg || strncmp(msg, "text ", 5) != 0) return;
    t = msg + 5;
    while (*t==' '||*t=='\t') t++;
    /* Последняя граница слова: пробел, после которого идёт не-пробел. */
    for (c = t; *c; c++)
        if ((*c==' '||*c=='\t') && c[1] && c[1]!=' ' && c[1]!='\t') last_space = c;
    if (last_space) {
        /* Хвост слова чистим от концевых пробелов: "green  " — тот же цвет. */
        snprintf(word, sizeof(word), "%s", last_space + 1);
        n = strlen(word);
        while (n > 0 && (word[n-1]==' '||word[n-1]=='\t')) word[--n] = 0;
        if (banner_color_known(word)) {
            n = (size_t)(last_space - t);
            while (n > 0 && (t[n-1]==' '||t[n-1]=='\t')) n--;
            if (n >= tcap) n = tcap - 1;
            memcpy(text_out, t, n);
            text_out[n] = 0;
            snprintf(color_out, ccap, "%s", word);
            return;
        }
    }
    snprintf(text_out, tcap, "%s", t);
}
const char* net_chat_text_cmd_text(const char *msg) {
    banner_split_cmd(msg, s_text_cmd_text_ret, sizeof(s_text_cmd_text_ret),
                     s_text_cmd_color_ret, sizeof(s_text_cmd_color_ret));
    return s_text_cmd_text_ret;
}
const char* net_chat_text_cmd_color(const char *msg) {
    banner_split_cmd(msg, s_text_cmd_text_ret, sizeof(s_text_cmd_text_ret),
                     s_text_cmd_color_ret, sizeof(s_text_cmd_color_ret));
    return s_text_cmd_color_ret;
}
/* Пишем banner.json в базу (правило — только для админов, как event.json).
 * PUT целиком: старый текст баннера заменяется новым. */
void net_banner_send(const char *text, const char *color) {
    char url[URL], body[BODY], esc[CHAT_TEXT_MAX * 2], ecol[BANNER_COLOR_MAX * 2];
    if (!net.started || !net.run) return;
    if (!text || !*text) return;
    if (!color || !*color) color = "white";
    json_escape(text, esc, sizeof(esc));
    json_escape(color, ecol, sizeof(ecol));
    snprintf(url, sizeof(url), "%s/banner.json", net.base);
    snprintf(body, sizeof(body), "{\"ts\":%lld,\"text\":\"%s\",\"color\":\"%s\"}", now_ms(), esc, ecol);
    event_write_async(url, body);
    LOG("banner send: %s (%s)", text, color);
}

/* Чат парсеры для админ команд */
double net_chat_is_ban(const char *msg) {
    if (!msg) return 0;
    // check starts with "ban "
    if (strlen(msg)>=4 && strncmp(msg,"ban ",4)==0) {
        // ensure there is target
        const char *t = msg+4;
        while (*t==' '||*t=='\t') t++;
        if (*t) return 1;
    }
    return 0;
}
double net_chat_is_unban(const char *msg) {
    if (!msg) return 0;
    if (strlen(msg)>=6 && strncmp(msg,"unban ",6)==0) {
        const char *t = msg+6;
        while (*t==' '||*t=='\t') t++;
        if (*t) return 1;
    }
    return 0;
}
const char* net_chat_ban_target(const char *msg) {
    s_ban_target_ret[0]=0;
    if (!msg) return s_ban_target_ret;
    if (strncmp(msg,"ban ",4)!=0) return s_ban_target_ret;
    const char *t = msg+4;
    while (*t==' '||*t=='\t') t++;
    size_t i=0;
    while (t[i] && i<sizeof(s_ban_target_ret)-1 && t[i]!=' '&&t[i]!='\t'&&t[i]!='\n'&&t[i]!='\r') {
        s_ban_target_ret[i]=t[i];
        i++;
    }
    s_ban_target_ret[i]=0;
    return s_ban_target_ret;
}
const char* net_chat_unban_target(const char *msg) {
    s_ban_target_ret[0]=0;
    if (!msg) return s_ban_target_ret;
    if (strncmp(msg,"unban ",6)!=0) return s_ban_target_ret;
    const char *t = msg+6;
    while (*t==' '||*t=='\t') t++;
    size_t i=0;
    while (t[i] && i<sizeof(s_ban_target_ret)-1 && t[i]!=' '&&t[i]!='\t'&&t[i]!='\n'&&t[i]!='\r') {
        s_ban_target_ret[i]=t[i];
        i++;
    }
    s_ban_target_ret[i]=0;
    return s_ban_target_ret;
}

/* Лидерборд */
typedef struct {
    char nick[24];
    int cups;
} LeaderboardEntry;

typedef struct {
    DSMutex lock;
    int status;
    int count;
    LeaderboardEntry entries[16];
} Leaderboard;
static Leaderboard lb = { .lock = DS_MUTEX_INIT };

static int parse_leaderboard(const char *json, LeaderboardEntry *out, int max_out) {
    const char *p = skip_ws(json);
    int count = 0;
    if (!p || *p != '{') return 0;
    for (p = skip_ws(p + 1); p && *p && *p != '}' && count < 64; ) {
        const char *kend = skip_str(p);
        if (!kend) break;
        char key[32] = {0};
        size_t kl = (size_t)(kend - p - 2);
        if (kl >= sizeof(key)) kl = sizeof(key) - 1;
        memcpy(key, p + 1, kl); key[kl] = 0;
        p = skip_ws(kend);
        if (*p != ':') break;
        p = skip_ws(p + 1);
        const char *obj = p;
        p = skip_val(p);
        if (!p) break;
        if (*obj == '{') {
            char nick[24] = {0};
            strv(obj, "nick", nick, sizeof(nick));
            if (!nick[0]) snprintf(nick, sizeof(nick), "%s", key);
            int cups = (int)num(obj, "cups", 0);
            if (cups < 0) cups = 0;
            snprintf(out[count].nick, sizeof(out[count].nick), "%s", nick);
            out[count].cups = cups;
            count++;
        }
        p = skip_ws(p);
        if (p && *p == ',') p = skip_ws(p + 1);
    }
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - 1 - i; j++) {
            if (out[j].cups < out[j + 1].cups) {
                LeaderboardEntry tmp = out[j];
                out[j] = out[j + 1];
                out[j + 1] = tmp;
            }
        }
    }
    if (count > max_out) count = max_out;
    return count;
}

static void *lb_fetch_job(void *arg) {
    char *base = (char *)arg;
    char url[URL], resp[65536];
    snprintf(url, sizeof(url), "%s/users.json", base);
    free(base);
    int code = fb_http("GET", url, NULL, resp, sizeof(resp));
    if (code != 200) {
        ds_mutex_lock(lb.lock);
        lb.status = 4;
        ds_mutex_unlock(lb.lock);
        return NULL;
    }
    LeaderboardEntry tmp[64];
    int cnt = parse_leaderboard(resp, tmp, 10);
    ds_mutex_lock(lb.lock);
    lb.count = cnt;
    for (int i = 0; i < cnt; i++) lb.entries[i] = tmp[i];
    lb.status = 2;
    ds_mutex_unlock(lb.lock);
    return NULL;
}
void net_leaderboard_fetch(const char *url) {
    const char *base = (url && *url) ? url : net.base;
    if (!base || !*base) return;
    ds_mutex_lock(lb.lock);
    lb.status = 1;
    lb.count = 0;
    ds_mutex_unlock(lb.lock);
    char *arg = (char*)malloc(URL);
    if (!arg) return;
    snprintf(arg, URL, "%s", base);
    DSThread t;
    t = ds_thread_start(lb_fetch_job, arg);
    if (t) ds_thread_detach(t);
    else {
        ds_mutex_lock(lb.lock); lb.status = 4; ds_mutex_unlock(lb.lock);
        free(arg);
    }
}
double net_leaderboard_status(void) {
    double s;
    ds_mutex_lock(lb.lock); s = (double)lb.status; ds_mutex_unlock(lb.lock);
    return s;
}
double net_leaderboard_count(void) {
    double c;
    ds_mutex_lock(lb.lock); c = (double)lb.count; ds_mutex_unlock(lb.lock);
    return c;
}
static char s_lb_nick_ret[24];
const char *net_leaderboard_nick(double idx) {
    int i = (int)idx;
    ds_mutex_lock(lb.lock);
    if (i >= 0 && i < lb.count) snprintf(s_lb_nick_ret, sizeof(s_lb_nick_ret), "%s", lb.entries[i].nick);
    else s_lb_nick_ret[0] = 0;
    ds_mutex_unlock(lb.lock);
    return s_lb_nick_ret;
}
double net_leaderboard_cups(double idx) {
    int i = (int)idx;
    double cups = 0;
    ds_mutex_lock(lb.lock);
    if (i >= 0 && i < lb.count) cups = (double)lb.entries[i].cups;
    ds_mutex_unlock(lb.lock);
    return cups;
}

static void *http_post_job(void *arg) {
    HttpJob *j = (HttpJob*)arg;
    if (j) { http("POST", j->url, j->body, NULL, 0); free(j); }
    return NULL;
}
static void http_post_async(const char *url, const char *body) {
    HttpJob *j = (HttpJob*)malloc(sizeof(*j));
    DSThread t;
    if (!j) return;
    fb_url(j->url, sizeof(j->url), url);
    snprintf(j->body, sizeof(j->body), "%s", body);
    t = ds_thread_start(http_post_job, j);
    if (t) { ds_thread_detach(t); return; }
    free(j);
}
static void *http_delete_job(void *arg) {
    char *url = (char*)arg;
    if (url) { http("DELETE", url, NULL, NULL, 0); free(url); }
    return NULL;
}
static void chat_delete_key_async(const char *key) {
    char *url;
    DSThread t;
    if (!key || !*key) return;
    if (strchr(key, '/') || strchr(key, '.') || strchr(key, '#')) return;
    url = (char*)malloc(FB_URL_MAX);
    if (!url) return;
    {
        char plain[URL];
        snprintf(plain, sizeof(plain), "%s/rooms/%s/chat/%s.json", net.base, net.room, key);
        fb_url(url, FB_URL_MAX, plain);
    }
    t = ds_thread_start(http_delete_job, url);
    if (t) { ds_thread_detach(t); return; }
    free(url);
}
/* Анти-флуд (меньше нагрузки на базу): сравнение полей актора — если игрок
 * стоит на месте и ничего не меняет, PUT можно пропустить. */
static int actor_same(const Actor *x, const Actor *y) {
    return x->x==y->x && x->y==y->y && x->a==y->a && x->hp==y->hp && x->alive==y->alive
        && x->punch_x==y->punch_x && x->punch_y==y->punch_y && x->punch_dx==y->punch_dx && x->punch_dy==y->punch_dy && x->punch==y->punch
        && x->snow_x==y->snow_x && x->snow_y==y->snow_y && x->snow_dx==y->snow_dx && x->snow_dy==y->snow_dy && x->snow==y->snow
        && x->station_x==y->station_x && x->station_y==y->station_y && x->station_hp==y->station_hp && x->station==y->station
        && x->universe_x==y->universe_x && x->universe_y==y->universe_y && x->universe==y->universe
        && x->cls==y->cls && x->level==y->level && !strcmp(x->nick,y->nick);
}
/* Поля позиции в базе валидируются правилами Firebase как 0..1 (снаряды —
 * -1..1). Одно значение вне диапазона отклоняет PUT целиком, и клиент
 * застревает в подключении. На всякий случай обрезаем всё прямо перед
 * отправкой: нормализация в скрипте уже есть, но это последний рубеж. */
static double clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : safe(v)); }
static double clamp11(double v) { v = safe(v); return v < -1 ? -1 : (v > 1 ? 1 : v); }

/* Последнее успешно отправленное состояние: анти-флуд (см. push_state).
 * Сбрасывается в net_connect, чтобы первая отправка новой сессии не сравнивалась
 * с состоянием прошлой. */
static Actor ps_last;
static long long ps_last_put = 0;
static int ps_have_last = 0;
static int push_state(void) {
    Actor a; int slot; unsigned long seq; char url[URL], body[BODY], enick[64];
    lock(); a=net.me; slot=net.slot; seq=++net.seq; unlock();
    if(slot<0) return 0;
    /* Стоим на месте и свежий heartbeat уже был — базу не дёргаем. seq обязан
     * обновляться хотя бы раз в HEARTBEAT_TICK: иначе другие игроки через
     * TIMEOUT сочтут слот мёртвым и заберут его. */
    if (ps_have_last && actor_same(&a,&ps_last) && now_ms()-ps_last_put < HEARTBEAT_TICK) return 1;
    json_escape(a.nick,enick,sizeof(enick));
    snprintf(url,sizeof(url),"%s/rooms/%s/players/%d.json",net.base,net.room,slot);
    snprintf(body,sizeof(body),"{\"uid\":\"%s\",\"nick\":\"%s\",\"x\":%.5f,\"y\":%.5f,\"angle\":%.5f,\"hp\":%.0f,\"alive\":%.0f,\"seq\":%lu,\"px\":%.5f,\"py\":%.5f,\"pdx\":%.5f,\"pdy\":%.5f,\"punch\":%.0f,\"sx\":%.5f,\"sy\":%.5f,\"sdx\":%.5f,\"sdy\":%.5f,\"snow\":%.0f,\"stx\":%.5f,\"sty\":%.5f,\"sthp\":%.0f,\"station\":%.0f,\"uzx\":%.5f,\"uzy\":%.5f,\"universe\":%.0f,\"cls\":%.0f,\"level\":%.0f}",
        net.uid,enick,clamp01(a.x),clamp01(a.y),safe(a.a),safe(a.hp),safe(a.alive),seq,
        clamp01(a.punch_x),clamp01(a.punch_y),clamp11(a.punch_dx),clamp11(a.punch_dy),safe(a.punch),
        clamp01(a.snow_x),clamp01(a.snow_y),clamp11(a.snow_dx),clamp11(a.snow_dy),safe(a.snow),clamp01(a.station_x),clamp01(a.station_y),safe(a.station_hp),safe(a.station),clamp01(a.universe_x),clamp01(a.universe_y),safe(a.universe),safe(a.cls),safe(a.level));
    int c = fb_http("PUT",url,body,NULL,0);
    if (c != 200 && net_log_ok()) LOGERR("push state: HTTP %d", c);
    if (c == 200) { ps_last = a; ps_last_put = now_ms(); ps_have_last = 1; }
    return c == 200;
}
static int pull_state(char *resp,size_t cap) {
    char url[URL];
    snprintf(url,sizeof(url),"%s/rooms/%s/players.json",net.base,net.room);
    int c = fb_http("GET",url,NULL,resp,cap);
    if (c != 200 && net_log_ok()) LOGERR("pull state: HTTP %d", c);
    return c == 200;
}
static int pull_event(char *resp,size_t cap) {
    char url[URL];
    snprintf(url,sizeof(url),"%s/event.json",net.base);
    int c = fb_http("GET",url,NULL,resp,cap);
    if (c != 200 && net_log_ok()) LOGERR("pull event: HTTP %d", c);
    return c == 200;
}
static void parse_and_store_banner(const char *json) {
    char text[CHAT_TEXT_MAX], color[BANNER_COLOR_MAX];
    long long ts;
    if (!json || !*json || !strncmp(json,"null",4)) return;
    ts = (long long)num(json,"ts",0);
    if (ts <= 0) return;
    strv(json,"text",text,sizeof(text));
    strv(json,"color",color,sizeof(color));
    if (!text[0]) return;
    lock();
    snprintf(net.banner_text,sizeof(net.banner_text),"%s",text);
    snprintf(net.banner_color,sizeof(net.banner_color),"%s",color[0] ? color : "white");
    net.banner_ts = ts;
    unlock();
}
static int pull_banner(char *resp,size_t cap) {
    char url[URL];
    snprintf(url,sizeof(url),"%s/banner.json",net.base);
    return fb_http("GET",url,NULL,resp,cap)==200;
}

static void release_slot(void) {
    int slot; char url[URL]; lock(); slot=net.slot; net.slot=-1; unlock(); if(slot<0)return;
    snprintf(url,sizeof(url),"%s/rooms/%s/players/%d.json",net.base,net.room,slot); fb_http("DELETE",url,NULL,NULL,0);
}
static int claim_slot(void) {
    char resp[RESP],url[URL],body[BODY],uid[24],etag[96]; long long t=now_ms(); int slot;
    for(slot=0;slot<NET_SLOTS;slot++) {
        unsigned long seq; int claim=0,code;
        if (!net.run) return -1;
        snprintf(url,sizeof(url),"%s/rooms/%s/players/%d.json",net.base,net.room,slot);
        code=fb_http_ex("GET",url,NULL,resp,sizeof(resp),"X-Firebase-ETag","true",etag,sizeof(etag));
        if(code!=200) { if(net_log_ok()) LOGERR("claim slot %d: HTTP %d", slot, code); continue; }
        strv(resp,"uid",uid,sizeof(uid));
        if(uid[0]&&!strcmp(uid,net.uid))return slot;
        seq=(unsigned long)num(resp,"seq",0);
        if(!uid[0])claim=1;
        else if(etag[0]&&(strcmp(uid,claim_seen_uid[slot])||seq!=claim_seen_seq[slot])) { snprintf(claim_seen_uid[slot],sizeof(claim_seen_uid[slot]),"%s",uid); claim_seen_seq[slot]=seq; claim_seen_at[slot]=t; }
        else if(etag[0]&&t-claim_seen_at[slot]>=STALE)claim=1;
        if(!claim)continue;
        if(!etag[0]) {
            if(net_log_ok()) LOG("claim slot %d: no ETag", slot);
        }
        {
            char enick[64];
            json_escape(net.me.nick,enick,sizeof(enick));
            snprintf(body,sizeof(body),"{\"uid\":\"%s\",\"nick\":\"%s\",\"x\":0,\"y\":0,\"angle\":0,\"hp\":0,\"alive\":0,\"seq\":0,\"px\":0,\"py\":0,\"pdx\":0,\"pdy\":0,\"punch\":0,\"sx\":0,\"sy\":0,\"sdx\":0,\"sdy\":0,\"snow\":0,\"stx\":0,\"sty\":0,\"sthp\":0,\"station\":0,\"uzx\":0,\"uzy\":0,\"universe\":0,\"cls\":%.0f,\"level\":%.0f}",net.uid,enick,safe(net.me.cls),safe(net.me.level));
        }
        code=fb_http_ex("PUT",url,body,NULL,0, etag[0] ? "if-match" : NULL, etag[0] ? etag : NULL, NULL, 0);
        if(code==200) { claim_seen_uid[slot][0]=0; claim_seen_seq[slot]=0; claim_seen_at[slot]=0; LOG("slot %d uid %s",slot,net.uid); return slot; }
        if(net_log_ok()) LOGERR("claim slot %d: PUT failed, HTTP %d", slot, code);
    }
    return -1;
}
static void read_players(const char *resp) {
    Actor ps[NET_SLOTS]; long long t=now_ms(); int local,count=0,slot;
    memset(ps,0,sizeof(ps));
    lock(); local=net.slot; unlock();
    for(slot=0;slot<NET_SLOTS;slot++) {
        char bp[24],p[40],uid[24]; unsigned long sq; int online;
        if(slot==local) { lock(); ps[slot]=net.me; ps[slot].online=local>=0; unlock(); if(local>=0)count++; continue; }
        snprintf(bp,sizeof(bp),"%d",slot); snprintf(p,sizeof(p),"%s/uid",bp); strv(resp,p,uid,sizeof(uid));
        if(!uid[0]||!strcmp(uid,net.uid)){ rp_lseq[slot]=0; rp_lch[slot]=0; continue; }
        snprintf(p,sizeof(p),"%s/seq",bp); sq=(unsigned long)num(resp,p,0);
        if(sq!=rp_lseq[slot]){ rp_lseq[slot]=sq; rp_lch[slot]=t; } else if(!rp_lch[slot]) rp_lch[slot]=t;
        online=t-rp_lch[slot]<TIMEOUT; if(!online)continue;
        snprintf(p,sizeof(p),"%s/nick",bp); strv(resp,p,ps[slot].nick,sizeof(ps[slot].nick));
        if(ban_cache_has(ps[slot].nick)) {
            /* Забаненный игрок не показывается и выкидывается из комнаты:
             * так бан действует и на тех, кто зашёл со старой версией. */
            if(t-rp_kicked[slot]>=BAN_KICK_TICK) {
                rp_kicked[slot]=t;
                LOG("slot %d: '%s' is banned, kicking", slot, ps[slot].nick);
                room_slot_delete_async(slot);
            }
            rp_lseq[slot]=0; rp_lch[slot]=0;
            continue;
        }
        snprintf(p,sizeof(p),"%s/x",bp); ps[slot].x=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/y",bp); ps[slot].y=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/angle",bp); ps[slot].a=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/hp",bp); ps[slot].hp=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/alive",bp); ps[slot].alive=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/px",bp); ps[slot].punch_x=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/py",bp); ps[slot].punch_y=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/pdx",bp); ps[slot].punch_dx=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/pdy",bp); ps[slot].punch_dy=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/punch",bp); ps[slot].punch=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/sx",bp); ps[slot].snow_x=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/sy",bp); ps[slot].snow_y=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/sdx",bp); ps[slot].snow_dx=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/sdy",bp); ps[slot].snow_dy=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/snow",bp); ps[slot].snow=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/stx",bp); ps[slot].station_x=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/sty",bp); ps[slot].station_y=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/sthp",bp); ps[slot].station_hp=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/station",bp); ps[slot].station=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/uzx",bp); ps[slot].universe_x=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/uzy",bp); ps[slot].universe_y=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/universe",bp); ps[slot].universe=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/cls",bp); ps[slot].cls=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/level",bp); ps[slot].level=num(resp,p,0);
        ps[slot].online=1; count++;
    }
    lock(); memcpy(net.players,ps,sizeof(ps)); net.count=count; unlock();
}
static int parse_chat_list(const char *json, ChatMsg *tmp, int cap){
    int tmp_cnt=0;
    const char *p=skip_ws(json);
    if(!p||!*p||!strncmp(p,"null",4)) return 0;
    if(*p!='{') return 0;
    for(p=skip_ws(p+1); p&&*p&&*p!='}'&&tmp_cnt<cap; ){
        if(*p!='\"') break;
        const char *kend=skip_str(p);
        if(!kend) break;
        char key[28]={0};
        size_t kl=(size_t)(kend-p-2);
        if(kl>=sizeof(key)) kl=sizeof(key)-1;
        memcpy(key,p+1,kl); key[kl]=0;
        p=skip_ws(kend);
        if(*p!=':') break;
        p=skip_ws(p+1);
        const char *obj=p;
        p=skip_val(p);
        if(!p) break;
        memset(&tmp[tmp_cnt], 0, sizeof(tmp[tmp_cnt]));
        strncpy(tmp[tmp_cnt].key, key, sizeof(tmp[tmp_cnt].key)-1);
        strv(obj,"uid",tmp[tmp_cnt].uid,sizeof(tmp[tmp_cnt].uid));
        strv(obj,"nick",tmp[tmp_cnt].nick,sizeof(tmp[tmp_cnt].nick));
        strv(obj,"text",tmp[tmp_cnt].text,sizeof(tmp[tmp_cnt].text));
        tmp[tmp_cnt].valid=1;
        tmp_cnt++;
        p=skip_ws(p);
        if(p&&*p==',') p=skip_ws(p+1);
    }
    return tmp_cnt;
}
static void parse_and_store_chat(const char *json){
    ChatMsg tmp[CHAT_MAX+16];
    int tmp_cnt, keep, start, i;
    char extra[CHAT_MAX+16][28];
    int nextra=0;
    if(!json || !*json) return;
    tmp_cnt=parse_chat_list(json, tmp, CHAT_MAX+16);
    if(tmp_cnt<=0) return;
    lock(); keep=chat_keep; unlock();
    if(keep<1) keep=1;
    if(keep>CHAT_MAX) keep=CHAT_MAX;
    start=0;
    if(tmp_cnt>keep){
        int drop=tmp_cnt-keep;
        for(i=0;i<drop;i++){
            if(tmp[i].key[0]){
                strncpy(extra[nextra], tmp[i].key, sizeof(extra[0])-1);
                extra[nextra][sizeof(extra[0])-1]=0;
                nextra++;
            }
        }
        start=drop;
    }
    lock();
    net.chat_count=0;
    for(i=start;i<tmp_cnt && net.chat_count<CHAT_MAX;i++)
        net.chats[net.chat_count++]=tmp[i];
    unlock();
    for(i=0;i<nextra;i++) chat_delete_key_async(extra[i]);
}
static int pull_chat(char *resp, size_t cap){
    char url[URL];
    snprintf(url,sizeof(url),"%s/rooms/%s/chat.json?orderBy=%%22$key%%22&limitToLast=%d",net.base,net.room,CHAT_MAX);
    return fb_http("GET",url,NULL,resp,cap)==200;
}
static void prune_old_chat(void){
    char url[URL], resp[CHAT_RESP];
    ChatMsg oldm[16];
    char keepkeys[CHAT_MAX][28];
    int n, nk=0, count, keep, i, j;
    lock(); keep=chat_keep; count=net.chat_count;
    for(i=0;i<net.chat_count;i++){
        strncpy(keepkeys[nk], net.chats[i].key, sizeof(keepkeys[0])-1);
        keepkeys[nk][sizeof(keepkeys[0])-1]=0;
        nk++;
    }
    unlock();
    if(count<keep || keep<1) return;
    snprintf(url,sizeof(url),"%s/rooms/%s/chat.json?orderBy=%%22$key%%22&limitToFirst=16",net.base,net.room);
    if(fb_http("GET",url,NULL,resp,sizeof(resp))!=200) return;
    n=parse_chat_list(resp, oldm, 16);
    for(i=0;i<n;i++){
        int kept=0;
        if(!oldm[i].key[0]) continue;
        for(j=0;j<nk;j++) if(!strcmp(oldm[i].key, keepkeys[j])){ kept=1; break; }
        if(!kept) chat_delete_key_async(oldm[i].key);
    }
}

/* Ждём первую загрузку списка банов, но не дольше BAN_WAIT мс. */
static void wait_ban_list(void) {
    long long deadline = now_ms() + BAN_WAIT;
    while (net.run && !ban_synced() && now_ms() < deadline) sleep_ms(50);
}
/* Пауза после неудач: с каждой следующей ошибкой вдвое длиннее (300 мс …
 * 8 с, плюс джиттер). Раньше потоки бились в базу каждые 300–500 мс даже
 * когда она лежала, усиливая аварию. */
static void net_fail_sleep(int fails) {
    int ms = 300;
    for (int i = 1; i < fails && ms < 8000; i++) ms *= 2;
    if (ms > 8000) ms = 8000;
    sleep_ms(ms + (int)(now_ms() % 250));
}
static void mark_self_banned(const char *nick) {
    int fresh;
    lock(); fresh = !net.self_banned; net.self_banned = 1; unlock();
    if (fresh) LOGERR("'%s' is banned: online closed", nick);
}
/* Выкидываем слот забаненного игрока, чтобы он не мешал бою у остальных. */
static void room_slot_delete_async(int slot) {
    char *url = (char*)malloc(FB_URL_MAX);
    DSThread t;
    if (!url || !net.base[0]) { free(url); return; }
    {
        char plain[URL];
        snprintf(plain, sizeof(plain), "%s/rooms/%s/players/%d.json", net.base, net.room, slot);
        fb_url(url, FB_URL_MAX, plain);
    }
    t = ds_thread_start(http_delete_job, url);
    if (t) { ds_thread_detach(t); return; }
    free(url);
}
static void *thread_main(void *arg) {
    int fails=0; (void)arg;
    while(net.run) {
        long long start=now_ms(); int slot;
        lock(); slot=net.slot; unlock();
        if(slot<0) {
            char nick[24];
            status(NET_CONNECTING);
            /* В комнату не пускаем, пока не проверили свой ник по списку банов.
             * Ждём недолго: если сеть лежит, слот всё равно не достанется. */
            wait_ban_list();
            lock(); snprintf(nick,sizeof(nick),"%s",net.me.nick); unlock();
            if(nick[0] && ban_check_cloud_sync(nick)) {
                mark_self_banned(nick);
                sleep_ms(500);
                continue;
            }
            slot=claim_slot();
            if(slot<0){ if(++fails>6){status(NET_ERROR); LOGERR("network error: cannot claim player slot");} net_fail_sleep(fails); continue; }
            lock(); net.slot=slot; net.seq=0; net.players[slot]=net.me; net.players[slot].online=1; unlock(); fails=0;
            LOG("slot %d claimed", (int)net.slot);
        }
        if(!push_state()){ if(++fails>6){status(NET_ERROR); LOGERR("network error: failed to push state");} net_fail_sleep(fails); continue; }
        fails=0; status(NET_PLAYING);
        long long spent=now_ms()-start; if(spent<WRITE_TICK)sleep_ms((int)(WRITE_TICK-spent));
    }
    release_slot(); status(NET_OFFLINE); return NULL;
}
static void *reader_thread(void *arg) {
    char resp[RESP], chat_resp[CHAT_RESP], event_resp[RESP], banner_resp[512];
    long long next_chat=0, next_event=0, next_ban=0, next_prune=0, next_banner=0;
    int fails=0;
    (void)arg;
    while(net.run) {
        long long start=now_ms(); int slot;
        /* Список банов тянем и пока подключаемся, и во время боя: забаненный
         * игрок не должен ни зайти, ни остаться в комнате. */
        if(start>=next_ban) {
            if(ban_sync()) {
                char nick[24];
                lock(); snprintf(nick,sizeof(nick),"%s",net.me.nick); unlock();
                if(nick[0] && ban_cache_has(nick)) {
                    mark_self_banned(nick);
                    net.run=0;
                    break;
                }
            }
            next_ban=now_ms()+BAN_TICK;
        }
        lock(); slot=net.slot; unlock();
        if(slot<0){ sleep_ms(50); continue; }
        if(pull_state(resp,sizeof(resp))) { read_players(resp); fails=0; }
        else { if(fails<20) fails++; net_fail_sleep(fails); }
        if(start>=next_chat) {
            if(pull_chat(chat_resp,sizeof(chat_resp))) {
                parse_and_store_chat(chat_resp);
                /* Чистку старых сообщений гоняем редко: это отдельный GET,
                 * который базе не нужен каждую секунду. */
                if(start>=next_prune) { prune_old_chat(); next_prune=now_ms()+CHAT_PRUNE_TICK; }
            }
            next_chat=now_ms()+CHAT_TICK;
        }
        if(start>=next_event) {
            if(pull_event(event_resp,sizeof(event_resp))) {
                int ev=(int)num(event_resp,"",0);
                if(ev<0) ev=0;
                lock(); net.event=ev; unlock();
            }
            next_event=now_ms()+EVENT_TICK;
        }
        if(start>=next_banner) {
            if(pull_banner(banner_resp,sizeof(banner_resp)))
                parse_and_store_banner(banner_resp);
            next_banner=now_ms()+BANNER_TICK;
        }
        long long spent=now_ms()-start; if(spent<READ_TICK)sleep_ms((int)(READ_TICK-spent));
    }
    return NULL;
}
static void make_uid(void) {
    unsigned long a,b; int local=0;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    a=(unsigned long)time(NULL)^((unsigned long)t.tv_nsec<<8);
    b=(unsigned long)getpid()^(unsigned long)(uintptr_t)&local;
    snprintf(net.uid,sizeof(net.uid),"%08lx%08lx",a&0xfffffffful,b&0xfffffffful);
}
void net_connect(const char *url, const char *room) {
    size_t n; if(!url||!*url)return;
    net_fast = 0;
    if(net.started)net_disconnect();
    memset(&net.me,0,sizeof(net.me));
    snprintf(net.base,sizeof(net.base),"%s",url); n=strlen(net.base); while(n&&net.base[n-1]=='/')net.base[--n]=0;
    snprintf(net.room,sizeof(net.room),"%s",(room&&*room)?room:"main");
    if(!net.uid[0])make_uid();
    {
        char snick[LOGIN_NICK_MAX+1];
        lg_lock(); if (lg.status == NET_LOGIN_OK) snprintf(snick,sizeof(snick),"%s",lg.session_nick); else snick[0]=0; lg_unlock();
        if (snick[0]) snprintf(net.me.nick,sizeof(net.me.nick),"%s",snick);
    }
    net.me.cls = (double)pending_cls;
    net.me.level = (double)pending_level;
    lock(); net.self_banned = 0; unlock();
    /* Список банов нужно перечитать: вдруг бан поставили, пока мы сидели в меню. */
    ban_invalidate();
    /* Сбрасываем всё состояние прошлой комнаты: чат, игроков, ивент/баннер и
     * статики наблюдения за слотами. Без этого, пока новый клиент ещё не
     * захватил слот (комната занята «призраками» убитых приложений), на
     * экране висят сообщения и снимки игроков из прошлой сессии — выглядит
     * как «чат прогрузился, а никто не двигается, висит подключение». */
    memset(claim_seen_seq, 0, sizeof(claim_seen_seq));
    memset(claim_seen_at, 0, sizeof(claim_seen_at));
    memset(claim_seen_uid, 0, sizeof(claim_seen_uid));
    memset(rp_lseq, 0, sizeof(rp_lseq));
    memset(rp_lch, 0, sizeof(rp_lch));
    memset(rp_kicked, 0, sizeof(rp_kicked));
    memset(&ps_last, 0, sizeof(ps_last));
    ps_last_put = 0; ps_have_last = 0;
    lock();
    memset(net.players, 0, sizeof(net.players));
    memset(net.chats, 0, sizeof(net.chats));
    net.chat_count = 0;
    net.count = 0;
    net.slot = -1;
    net.seq = 0;
    net.event = 0;
    net.banner_text[0] = 0;
    net.banner_color[0] = 0;
    net.banner_ts = 0;
    unlock();
    net.started=1;
    net.status=NET_CONNECTING; net.run=1;
    LOG("connect %s/%s", net.base, net.room);
    net.thread = ds_thread_start(thread_main, NULL);
    net.rthread = ds_thread_start(reader_thread, NULL);
}
void net_disconnect(void) {
    if(!net.started)return;
    net_fast = 1;
    net.run=0;
    ds_thread_join(net.thread); net.thread=(DSThread)0;
    ds_thread_join(net.rthread); net.rthread=(DSThread)0;
    net.started=0; net.slot=-1; status(NET_OFFLINE);
    net_fast = 0;
}
void net_publish(double x, double y, double angle, double hp, double alive) {
    lock();
    net.me.x=x; net.me.y=y; net.me.a=angle; net.me.hp=hp; net.me.alive=alive;
    if(net.slot>=0){ net.players[net.slot]=net.me; net.players[net.slot].online=1; }
    unlock();
}
void net_publish_punch(double x, double y, double dx, double dy, double punch) {
    lock();
    net.me.punch_x=x; net.me.punch_y=y;
    net.me.punch_dx=dx; net.me.punch_dy=dy; net.me.punch=punch;
    if(net.slot>=0){ net.players[net.slot]=net.me; net.players[net.slot].online=1; }
    unlock();
}
void net_publish_snow(double x, double y, double dx, double dy, double snow) {
    lock();
    net.me.snow_x=x; net.me.snow_y=y;
    net.me.snow_dx=dx; net.me.snow_dy=dy; net.me.snow=snow;
    if(net.slot>=0){ net.players[net.slot]=net.me; net.players[net.slot].online=1; }
    unlock();
}
void net_publish_station(double x, double y, double hp, double counter) {
    lock();
    net.me.station_x=x; net.me.station_y=y; net.me.station_hp=hp; net.me.station=counter;
    if(net.slot>=0){ net.players[net.slot]=net.me; net.players[net.slot].online=1; }
    unlock();
}
void net_publish_universe(double x, double y, double counter) {
    lock();
    net.me.universe_x=x; net.me.universe_y=y; net.me.universe=counter;
    if(net.slot>=0){ net.players[net.slot]=net.me; net.players[net.slot].online=1; }
    unlock();
}
void net_chat_send(const char *text){
    if(!net.started || !text || !*text) return;
    if(!net.run) return;
    char url[URL], body[BODY*2], esc[CHAT_TEXT_MAX*2], enick[64];
    const char *nick;
    json_escape(text, esc, sizeof(esc));
    lock(); nick = net.me.nick[0] ? net.me.nick : net.uid; json_escape(nick, enick, sizeof(enick)); unlock();
    snprintf(url,sizeof(url),"%s/rooms/%s/chat.json",net.base,net.room);
    snprintf(body,sizeof(body),"{\"uid\":\"%s\",\"nick\":\"%s\",\"text\":\"%s\"}",net.uid,enick,esc);
    http_post_async(url, body);
    LOG("chat send %s",text);
}
void net_chat_trim(double keep){
    int k=(int)keep;
    char extra[CHAT_MAX][28];
    int nextra=0, drop, i;
    if(k<1) k=1;
    if(k>CHAT_MAX) k=CHAT_MAX;
    lock();
    chat_keep=k;
    if(net.chat_count>k){
        drop=net.chat_count-k;
        for(i=0;i<drop;i++){
            if(net.chats[i].key[0]){
                strncpy(extra[nextra], net.chats[i].key, sizeof(extra[0])-1);
                extra[nextra][sizeof(extra[0])-1]=0;
                nextra++;
            }
        }
        memmove(net.chats, net.chats+drop, (size_t)k*sizeof(net.chats[0]));
        net.chat_count=k;
        if(k<CHAT_MAX) memset(net.chats+k, 0, (size_t)(CHAT_MAX-k)*sizeof(net.chats[0]));
    }
    unlock();
    for(i=0;i<nextra;i++) chat_delete_key_async(extra[i]);
}
double net_chat_count(void){ double v; lock(); v=net.chat_count; unlock(); return v; }
const char* net_chat_text(double idx){
    int i=(int)idx;
    lock();
    if(i>=0 && i<net.chat_count && net.chats[i].valid){
        strncpy(s_chat_text_ret, net.chats[i].text, sizeof(s_chat_text_ret)-1);
        s_chat_text_ret[sizeof(s_chat_text_ret)-1]='\0';
    }else{
        s_chat_text_ret[0]='\0';
    }
    unlock();
    return s_chat_text_ret;
}
const char* net_chat_uid(double idx){
    int i=(int)idx;
    lock();
    if(i>=0 && i<net.chat_count && net.chats[i].valid){
        const char *src = net.chats[i].nick[0] ? net.chats[i].nick : net.chats[i].uid;
        strncpy(s_chat_uid_ret, src, sizeof(s_chat_uid_ret)-1);
        s_chat_uid_ret[sizeof(s_chat_uid_ret)-1]='\0';
    }else{
        s_chat_uid_ret[0]='\0';
    }
    unlock();
    return s_chat_uid_ret;
}
static int sidx(double slot){ int i=(int)slot; return i>=0&&i<NET_SLOTS?i:-1; }
double net_status(void){ double v; lock(); v=net.status; unlock(); return v; }
double net_slot(void){ double v; lock(); v=net.slot; unlock(); return v; }
double net_event(void){ double v; lock(); v=net.event; unlock(); return v; }

/* Баннер админа: последний баннер, прочитанный из облака. Скрипт сравнивает
 * ts с тем, что уже показывал, и решает, показывать ли текст заново. */
double net_banner_ts(void){ double v; lock(); v=(double)net.banner_ts; unlock(); return v; }
const char *net_banner_text(void){
    lock();
    snprintf(s_banner_text_ret,sizeof(s_banner_text_ret),"%s",net.banner_text);
    unlock();
    return s_banner_text_ret;
}
const char *net_banner_color(void){
    lock();
    snprintf(s_banner_color_ret,sizeof(s_banner_color_ret),"%s",net.banner_color);
    unlock();
    return s_banner_color_ret;
}
double net_count(void){ double v; lock(); v=net.count; unlock(); return v; }
const char* net_player_nick(double slot){
    int i=sidx(slot);
    lock();
    if(i>=0 && net.players[i].nick[0]){
        strncpy(s_nick_ret, net.players[i].nick, sizeof(s_nick_ret)-1);
        s_nick_ret[sizeof(s_nick_ret)-1]='\0';
    }else{
        s_nick_ret[0]='\0';
    }
    unlock();
    return s_nick_ret;
}
#define READER(name, field) double name(double slot){ int i=sidx(slot); double v=0; if(i>=0){lock();v=field;unlock();} return v; }
READER(net_player_online, net.players[i].online?1:0)
READER(net_player_x, net.players[i].x)
READER(net_player_y, net.players[i].y)
READER(net_player_angle, net.players[i].a)
READER(net_player_hp, net.players[i].hp)
READER(net_player_alive, net.players[i].alive)
READER(net_player_punch_x, net.players[i].punch_x)
READER(net_player_punch_y, net.players[i].punch_y)
READER(net_player_punch_dx, net.players[i].punch_dx)
READER(net_player_punch_dy, net.players[i].punch_dy)
READER(net_player_punch, net.players[i].punch)
READER(net_player_snow_x, net.players[i].snow_x)
READER(net_player_snow_y, net.players[i].snow_y)
READER(net_player_snow_dx, net.players[i].snow_dx)
READER(net_player_snow_dy, net.players[i].snow_dy)
READER(net_player_snow, net.players[i].snow)
READER(net_player_station_x, net.players[i].station_x)
READER(net_player_station_y, net.players[i].station_y)
READER(net_player_station_hp, net.players[i].station_hp)
READER(net_player_station, net.players[i].station)
READER(net_player_universe_x, net.players[i].universe_x)
READER(net_player_universe_y, net.players[i].universe_y)
READER(net_player_universe, net.players[i].universe)
READER(net_player_class, net.players[i].cls)
#undef READER
double net_player_level(double slot) {
    int i=sidx(slot); double v=0;
    if(i>=0){ lock(); v=net.players[i].level; unlock(); }
    if(v<0) v=0;
    if(v>LEVEL_MAX) v=LEVEL_MAX;
    return v;
}