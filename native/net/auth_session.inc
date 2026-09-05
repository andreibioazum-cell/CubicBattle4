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
    pending_skin = azum_skin;
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
    pending_cls = cls; pending_level = lv; pending_skin = sk;
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
    pending_cls = 0; pending_level = 0; pending_skin = 0;
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
