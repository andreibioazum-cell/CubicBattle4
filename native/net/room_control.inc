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
        && x->cls==y->cls && x->level==y->level && x->skin==y->skin && !strcmp(x->nick,y->nick);
}
/* Поля позиции в базе валидируются правилами Firebase как 0..1 (снаряды —
 * -1..1). Одно значение вне диапазона отклоняет PUT целиком, и клиент
 * застревает в подключении. На всякий случай обрезаем всё прямо перед
 * отправкой: нормализация в скрипте уже есть, но это последний рубеж. */
static double clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : safe(v)); }
static double clamp11(double v) { v = safe(v); return v < -1 ? -1 : (v > 1 ? 1 : v); }
