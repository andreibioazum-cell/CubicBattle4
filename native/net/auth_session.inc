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

/* Создание записи нового игрока. pass — только для старого режима без
 * Firebase Auth: там пароль хранится в базе (и это плохо). */
static int put_default_user(const char *req_url, const char *nick, const char *pass) {
    Profile empty;
    char body[BODY], epass[128];
    size_t n;
    memset(&empty, 0, sizeof(empty));
    snprintf(empty.owner, sizeof(empty.owner), "%s", nick ? nick : "");
    profile_json(body, sizeof(body), &empty, nick ? nick : "", 1);
    if (!body[0]) return 0;
    if (!fb_enabled() && pass && *pass) {
        json_escape(pass, epass, sizeof(epass));
        n = strlen(body);
        if (body[n - 1] == '}' && n + strlen(epass) + 12 < sizeof(body))
            snprintf(body + n - 1, sizeof(body) - (n - 1), ",\"pass\":\"%s\"}", epass);
    }
    return fb_http("PUT", req_url, body, NULL, 0);
}

/* Сессия установлена (вход или регистрация), а записи в облаке не было.
 * Локальный прогресс переносим в новый аккаунт, если он ничей (файл от старой
 * версии) или принадлежит этому нику: оффлайн-покупки не должны пропадать, но
 * и чужой аккаунт на этом устройстве подхватывать нельзя. */
static void net_auth_session_ok(const char *nick, const char *pass) {
    Profile p;
    int moved = 0, mine = 0;
    memset(&p, 0, sizeof(p));
    progress_read();
    achievements_read();
    pg_lock();
    mine = pg.loaded && (pg.p.owner[0] == 0 || nick_same(pg.p.owner, nick));
    if (mine) {
        p = pg.p;
        moved = p.cups > 0 || p.candies > 0 || p.azum || p.santa || p.ebuc ||
                p.cls != 0 || p.bp_level > 0 || p.azum_skin > 0 ||
                p.ordinary_levels_unlocked > 0 || p.azum_levels_unlocked > 0 ||
                p.santa_levels_unlocked > 0 || p.ebuc_levels_unlocked > 0;
    }
    pg_unlock();
    if (!mine) {
        /* Копия на устройстве принадлежит другому нику — его награды новому
         * аккаунту не отдаём (свои он получит заново: check_achievements()
         * выдаёт «Привет!» и «Потрогай траву» по текущему прогрессу). */
        ds_mutex_lock(ach_lock);
        ach_flags = 0;
        ach_loaded = 1;
        ds_mutex_unlock(ach_lock);
        achievements_write(0);
    }
    lg_lock();
    snprintf(lg.session_nick, sizeof(lg.session_nick), "%s", nick);
    snprintf(lg.session_pass, sizeof(lg.session_pass), "%s", pass);
    lg.status = NET_LOGIN_OK;
    lg_unlock();
    session_save(nick, pass);
    /* profile_save поставит владельца, ts, подтянет достижения, запишет файл и
     * отправит профиль в облако (для нового аккаунта — нулевой). */
    profile_save(&p);
    if (moved) LOG("moved local progress into account '%s'", nick);
}

/* Вход в существующий аккаунт: запоминаем сессию (auth.dat), а если слияние
 * показало, что облако отстало от локальной копии, сразу догоняем его. */
static void net_login_ok_existing(const char *nick, const char *pass, int cloud_behind) {
    lg_lock();
    snprintf(lg.session_nick, sizeof(lg.session_nick), "%s", nick);
    snprintf(lg.session_pass, sizeof(lg.session_pass), "%s", pass);
    lg.status = NET_LOGIN_OK;
    lg_unlock();
    session_save(nick, pass);
    if (cloud_behind) progress_push_cloud_current();
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
