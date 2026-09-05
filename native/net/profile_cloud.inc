/* Профиль в облаке: JSON записи /users/<ник> и фоновая отправка. Локальные
 * файлы ведёт profile_store.inc, слияние при входе — profile_merge.inc.
 *
 * Отправка одна на все сохранения. Профиль пишется часто (каждый подобранный
 * леденец в бою), а раньше каждое сохранение поднимало свой поток с PATCH:
 * запросы завершались в произвольном порядке, и устаревший снимок мог
 * перезаписать в базе свежий. Теперь поток один — он отправляет последний
 * профиль, а если за время запроса профиль снова изменился, отправляет ещё раз.
 * Заодно PATCH идёт через fb_http: он подписывает запрос токеном Firebase Auth
 * и обновляет протухший idToken (сохранение через час после входа раньше молча
 * уходило без подписи и отклонялось правилами базы). */
typedef struct {
    DSMutex lock;
    int busy, dirty;
    Profile pending;
    char nick[LOGIN_NICK_MAX + 1];
} ProfilePush;
static ProfilePush pushq = { .lock = DS_MUTEX_INIT };

/* JSON записи /users/<ник> из профиля. extras=1 добавляет новые поля (ach, ts,
 * lang), extras=0 — тот же профиль старым набором полей: запасной вариант для
 * правил базы, которые ещё не опубликованы заново. nick пишем всегда — правила
 * требуют его среди обязательных детей записи. */
static void profile_json(char *dst, size_t cap, const Profile *p, const char *nick, int extras) {
    char enick[64];
    size_t n;
    if (!dst || !cap) return;
    dst[0] = 0;
    if (!p) return;
    json_escape(nick ? nick : "", enick, sizeof(enick));
    n = (size_t)snprintf(dst, cap,
        "{\"nick\":\"%s\",\"cups\":%d,\"candies\":%d,\"cls\":%d,\"azum\":%d,\"santa\":%d,\"ebuc\":%d,"
        "\"level\":%d,\"levels\":%d,\"ordinary_level\":%d,\"ordinary_levels\":%d,"
        "\"azum_level\":%d,\"azum_levels\":%d,\"santa_level\":%d,\"santa_levels\":%d,"
        "\"ebuc_level\":%d,\"ebuc_levels\":%d,\"bp\":%d,\"azum_skin\":%d",
        enick, p->cups, p->candies, p->cls, p->azum, p->santa, p->ebuc,
        p->level, p->levels_unlocked, p->ordinary_level, p->ordinary_levels_unlocked,
        p->azum_level, p->azum_levels_unlocked, p->santa_level, p->santa_levels_unlocked,
        p->ebuc_level, p->ebuc_levels_unlocked, p->bp_level, p->azum_skin);
    if (n == 0 || n >= cap) { dst[0] = 0; return; }
    if (extras) snprintf(dst + n, cap - n, ",\"ach\":%u,\"ts\":%lld,\"lang\":%d}",
                         p->ach, p->ts, p->lang);
    else snprintf(dst + n, cap - n, "}");
}

/* Один PATCH профиля. 1 — база приняла запись. */
static int profile_patch_once(const Profile *p, const char *nick) {
    char url[URL], body[BODY*2], legacy[BODY*2], resp[256];
    int code, retry;
    resp[0] = 0;
    snprintf(url, sizeof(url), "%s/users/%s.json", net.base, nick);
    profile_json(body, sizeof(body), p, nick, 1);
    profile_json(legacy, sizeof(legacy), p, nick, 0);
    if (!body[0]) return 0;
    code = fb_http("PATCH", url, body, resp, sizeof(resp));
    if (code == 200) return 1;
    if ((code == 401 || code == 403) && legacy[0]) {
        /* Правила базы могут быть старыми (без полей ach/ts/lang): тогда
         * отклоняется вся запись целиком. Повторяем прежним набором полей,
         * чтобы прогресс всё равно доехал, и подсказываем, что пора
         * опубликовать новый firebase.rules.json. */
        retry = fb_http("PATCH", url, legacy, NULL, 0);
        if (net_log_ok()) {
            if (retry != 200)
                LOGERR("profile PATCH rejected twice (HTTP %d, then %d): publish firebase.rules.json from the repo", code, retry);
            else
                LOGERR("profile saved without ach/ts/lang (first PATCH: HTTP %d): publish the new firebase.rules.json", code);
        }
        return retry == 200;
    }
    if (net_log_ok()) LOGERR("profile PATCH failed: HTTP %d %.120s", code, resp);
    return 0;
}

static void *profile_push_job(void *arg) {
    (void)arg;
    for (;;) {
        Profile p;
        char nick[LOGIN_NICK_MAX + 1];
        ds_mutex_lock(pushq.lock);
        if (!pushq.dirty) { pushq.busy = 0; ds_mutex_unlock(pushq.lock); break; }
        pushq.dirty = 0;
        p = pushq.pending;
        snprintf(nick, sizeof(nick), "%s", pushq.nick);
        ds_mutex_unlock(pushq.lock);
        profile_patch_once(&p, nick);
    }
    return NULL;
}

/* Поставить профиль в очередь отправки. Без активной сессии ничего не делаем:
 * оффлайн-прогресс живёт в файле и уезжает в облако при входе. */
static void profile_push_cloud(const Profile *p) {
    char nick[LOGIN_NICK_MAX + 1] = "";
    Profile out;
    DSThread t;
    int start = 0;
    if (!p || !net.base[0]) return;
    lg_lock();
    if (lg.status == NET_LOGIN_OK && lg.session_nick[0])
        snprintf(nick, sizeof(nick), "%s", lg.session_nick);
    lg_unlock();
    if (!nick[0]) return;
    /* Маска достижений берётся из своего хранилища, а не из копии профиля: так
     * в облако уезжает самое свежее значение, включая награду, выданную между
     * сохранениями прогресса. */
    out = *p;
    achievements_read();
    ds_mutex_lock(ach_lock); out.ach = ach_flags; ds_mutex_unlock(ach_lock);
    ds_mutex_lock(pushq.lock);
    pushq.pending = out;
    snprintf(pushq.nick, sizeof(pushq.nick), "%s", nick);
    pushq.dirty = 1;
    if (!pushq.busy) { pushq.busy = 1; start = 1; }
    ds_mutex_unlock(pushq.lock);
    if (!start) return;
    t = ds_thread_start(profile_push_job, NULL);
    if (t) { ds_thread_detach(t); return; }
    /* Поток не создался — снимаем флаг, иначе очередь встанет навсегда. */
    ds_mutex_lock(pushq.lock);
    pushq.busy = 0;
    ds_mutex_unlock(pushq.lock);
}

/* Отправить в облако профиль, который уже в памяти (pg): после слияния на входе
 * и после выдачи достижения. Локальный файл не переписываем, ts остаётся от
 * последнего сохранения. */
static void progress_push_cloud_current(void) {
    Profile p;
    int have = 0;
    progress_read();
    pg_lock();
    if (pg.loaded) { p = pg.p; have = 1; }
    pg_unlock();
    if (!have) return;
    profile_push_cloud(&p);
}
