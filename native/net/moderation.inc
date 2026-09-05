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
        if (code != 200) {
            /* Прочитать профиль не дали (401/403/5xx). Считать запись
             * отсутствующей здесь нельзя: put_default_user перезаписал бы весь
             * облачный прогресс игрока нулями. Выходим ошибкой — локальная
             * копия при этом цела, а net_autologin продолжит по кэшу. */
            if (net_log_ok())
                LOGERR("net_auth: read profile '%s' failed HTTP %d (cloud record untouched)", nick, code);
            return (double)NET_ERROR;
        }
        if (!p || !*p || !strncmp(p, "null", 4)) {
            /* Записи действительно нет (новый аккаунт или старый без записи) —
             * создаём дефолтную без пароля. */
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
            int cloud_behind = apply_user_json_merge(resp, nick);
            net_login_ok_existing(nick, pass, cloud_behind);
            LOG("logged in: '%s' (cups=%d, candies=%d, cls=%d, ach=%u)", nick,
                (int)num(resp, "cups", 0), (int)num(resp, "candies", 0),
                (int)num(resp, "cls", 0), (unsigned int)num(resp, "ach", 0));
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
    if (code == 404 || (code == 200 && (!p || !*p || !strncmp(p, "null", 4)))) {
        int put_code = put_default_user(req_url, nick, pass);
        if (put_code != 200) {
            if (net_log_ok()) LOGERR("net_auth: register failed HTTP %d", put_code);
            return (double)NET_ERROR;
        }
        net_auth_session_ok(nick, pass);
        LOG("registered and logged in: '%s'", nick);
        return (double)NET_LOGIN_OK;
    }
    if (code != 200) {
        /* То же правило, что и в защищённом режиме: ошибка чтения — не
         * отсутствие записи, облачный прогресс не трогаем. */
        if (net_log_ok())
            LOGERR("net_auth: read profile '%s' failed HTTP %d (cloud record untouched)", nick, code);
        return (double)NET_ERROR;
    }

    if (*p == '{') {
        char got_pass[64] = "";
        strv(resp, "pass", got_pass, sizeof(got_pass));
        if (strcmp(got_pass, pass) == 0) {
            int cloud_behind = apply_user_json_merge(resp, nick);
            net_login_ok_existing(nick, pass, cloud_behind);
            LOG("logged in: '%s' (cups=%d, candies=%d, cls=%d, ach=%u)", nick,
                (int)num(resp, "cups", 0), (int)num(resp, "candies", 0),
                (int)num(resp, "cls", 0), (unsigned int)num(resp, "ach", 0));
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
