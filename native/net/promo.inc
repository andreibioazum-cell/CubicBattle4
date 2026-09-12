/* Персональный промокод игрока: карточки в соло-матчах и экран ввода кода.
 *
 * Код детерминированно выводится из ника (два FNV-1a с солью проекта):
 * тот же ник - тот же код всегда, у разных игроков - разные коды. Поэтому
 * код известен даже без сети, и «записать в базу чужой код» бесполезно:
 * клиент всегда пересчитает его из собственного ника.
 *
 * В облако код пишется отдельным PATCH'ем в профиль /users/<ник> (как
 * настройки в settings_storage.inc): promo - сам код, promo_used -
 * активирован ли он (одна награда на аккаунт). Отдельный запрос намеренно:
 * пока правила Firebase не опубликованы заново, эти PATCH'и просто не
 * пройдут, а прогресс (кубки, классы, уровни) продолжит сохраняться.
 *
 * Локально - promo.dat рядом с progress.dat:
 *   used 1
 *   streak 7
 * used - активирован ли промокод на этом устройстве, streak - сколько матчей
 * подряд без карточки (растёт шанс выпадения, см. card_roll в promo.ds).
 * Неизвестные строки при чтении игнорируются, запись атомарная
 * (временный файл + rename).
 */
#define PROMO_FILE "promo.dat"
#define PROMO_MAX 24

typedef struct {
    DSMutex lock;
    int loaded;
    int used;
    int streak;
} Promo;
static Promo pr = { .lock = DS_MUTEX_INIT };
static void pr_lock(void) { ds_mutex_lock(pr.lock); }
static void pr_unlock(void) { ds_mutex_unlock(pr.lock); }
static char s_promo_ret[PROMO_MAX];

/* FNV-1a с солью: код стабилен между сборками (соли зашиты здесь), а
 * разный второй basis даёт независимую вторую половину кода. */
static unsigned long long promo_hash(const char *nick, unsigned long long basis) {
    const unsigned long long prime = 1099511628211ull;
    unsigned long long h = basis;
    for (const char *p = nick; *p; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        h ^= (unsigned char)c;
        h *= prime;
    }
    return h;
}

/* Код формата CB4-XXXX-XXXX-XXXX (12 hex-цифр = 48 бит на два хэша). */
static void promo_code_for_nick(const char *nick, char *out, size_t cap) {
    out[0] = 0;
    if (!nick || !*nick || cap < 20) return;
    if (strlen(nick) > LOGIN_NICK_MAX) return;
    unsigned long long a = promo_hash(nick, 1469598103934665603ull);
    unsigned long long b = promo_hash(nick, 0xCBF29CE484222325ull);
    snprintf(out, cap, "CB4-%04X-%04X-%04X",
             (int)(a >> 48), (int)(a >> 16 & 0xFFFF), (int)(b & 0xFFFF));
}

/* Личный код текущего игрока (пусто, если не в аккаунте). */
const char *net_promo_code(void) {
    char nick[LOGIN_NICK_MAX + 1] = "";
    lg_lock();
    if (lg.status == NET_LOGIN_OK && lg.session_nick[0]) {
        snprintf(nick, sizeof(nick), "%s", lg.session_nick);
    }
    lg_unlock();
    s_promo_ret[0] = 0;
    promo_code_for_nick(nick, s_promo_ret, sizeof(s_promo_ret));
    return s_promo_ret;
}

static void promo_read(void) {
    char path[320], line[64];
    FILE *f;
    int used = 0, streak = 0;
    pr_lock();
    if (pr.loaded) { pr_unlock(); return; }
    pr_unlock();
    data_file_path(path, sizeof(path), PROMO_FILE);
    f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            int v = 0;
            if (sscanf(line, "used %d", &v) == 1) { used = v ? 1 : 0; continue; }
            if (sscanf(line, "streak %d", &v) == 1) { streak = v < 0 ? 0 : v; continue; }
        }
        fclose(f);
    }
    pr_lock();
    pr.used = used;
    pr.streak = streak;
    pr.loaded = 1;
    pr_unlock();
}

static void promo_write(void) {
    char path[320], text[96];
    int used = 0, streak = 0;
    promo_read();
    pr_lock();
    used = pr.used; streak = pr.streak;
    pr_unlock();
    data_file_path(path, sizeof(path), PROMO_FILE);
    snprintf(text, sizeof(text), "used %d\nstreak %d\n", used, streak);
    file_write_atomic(path, text);
}

double net_promo_used(void) {
    double v;
    promo_read();
    pr_lock(); v = (double)pr.used; pr_unlock();
    return v;
}

/* Сколько соло-матчей подряд без карточки (для роста шанса в card_roll). */
double net_promo_streak(void) {
    double v;
    promo_read();
    pr_lock(); v = (double)pr.streak; pr_unlock();
    return v;
}

/* Матч закончился без карточки - шанс в следующем растёт. */
void net_promo_bump_streak(void) {
    promo_read();
    pr_lock();
    pr.streak += 1;
    pr_unlock();
    promo_write();
}

/* Карточка выпала - счётчик сбрасывается. */
void net_promo_reset_streak(void) {
    promo_read();
    pr_lock();
    pr.streak = 0;
    pr_unlock();
    promo_write();
}

/* Активировать промокод: локально + отдельным PATCH'ем в облако. */
void net_promo_mark_used(void) {
    char nick[LOGIN_NICK_MAX + 1] = "";
    promo_read();
    pr_lock();
    pr.used = 1;
    pr_unlock();
    promo_write();
    lg_lock();
    if (lg.status == NET_LOGIN_OK && lg.session_nick[0]) {
        snprintf(nick, sizeof(nick), "%s", lg.session_nick);
    }
    lg_unlock();
    if (!nick[0] || !net.base[0]) return;
    char url[URL], body[BODY];
    snprintf(url, sizeof(url), "%s/users/%s.json", net.base, nick);
    snprintf(body, sizeof(body), "{\"promo_used\":1}");
    http_patch_async(url, body);
    if (net_log_ok()) LOG("promo marked used for %s", nick);
}

/* Записать личный код в облачный профиль (идемпотентно): при сборе
 * карточки и при входе, если в профиле кода ещё нет. */
void net_promo_register(void) {
    char nick[LOGIN_NICK_MAX + 1] = "";
    char code[PROMO_MAX] = "";
    lg_lock();
    if (lg.status == NET_LOGIN_OK && lg.session_nick[0]) {
        snprintf(nick, sizeof(nick), "%s", lg.session_nick);
    }
    lg_unlock();
    if (!nick[0] || !net.base[0]) return;
    promo_code_for_nick(nick, code, sizeof(code));
    char url[URL], body[BODY];
    snprintf(url, sizeof(url), "%s/users/%s.json", net.base, nick);
    snprintf(body, sizeof(body), "{\"promo\":\"%s\"}", code);
    http_patch_async(url, body);
    if (net_log_ok()) LOG("promo registered for %s: %s", nick, code);
}

/* После загрузки профиля из облака (см. profile_apply.inc): кода в записи
 * нет - дописываем его; promo_used=1 в облаке - не даём активировать код
 * заново на чистом устройстве (локальный файл мог потеряться). */
int promo_sync_with_cloud(const char *resp) {
    char stored[PROMO_MAX] = "";
    int changed = 0;
    int used = (int)num(resp, "promo_used", -1);
    strv(resp, "promo", stored, sizeof(stored));
    char nick[LOGIN_NICK_MAX + 1] = "";
    lg_lock();
    if (lg.status == NET_LOGIN_OK && lg.session_nick[0]) {
        snprintf(nick, sizeof(nick), "%s", lg.session_nick);
    }
    lg_unlock();
    if (!nick[0]) return 0;
    if (!stored[0]) {
        net_promo_register();
        changed = 1;
    }
    if (used > 0) {
        promo_read();
        pr_lock();
        int was = pr.used;
        pr.used = 1;
        pr_unlock();
        if (!was) {
            promo_write();
            changed = 1;
        }
    }
    return changed;
}
