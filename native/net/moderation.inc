double net_set_nick(const char *nick) {
    if (!nick || !nick_valid(nick)) return 0.0;
    /* The protected mode does not allow putting on a foreign nick with the
     * default password, as that would create a Firebase Auth account under
     * someone else's name. A nick changes only through a full net_auth call
     * with a password of its own. */
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

/* Ban handling */
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

/* Cache of the cloud ban list, replaced as a whole by every ban_sync() call. */
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

/* Reads /bans.json as a whole and returns 1 when the list was refreshed. */
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

/* Whether a nick is banned as far as this client knows, from the cloud list or
 * from the local bans.dat. */
static int nick_banned_locally(const char *nick) {
    if (!nick || !*nick) return 0;
    return ban_cache_has(nick) || is_banned_in_file(nick);
}

/* Synchronous cloud ban check, for the background network threads only
 * (thread_main): calling it from the game thread blocks a frame. */
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
            // true or an object both mean banned
            ban_cache_put(nick);
            return 1;
        }
    } else if (net_log_ok()) {
        LOGERR("ban check '%s': HTTP %d", nick, code);
    }
    return 0;
}

/* Background ban check: the answer lands in the cache and the game thread does
 * not wait for it. */
typedef struct { char nick[LOGIN_NICK_MAX + 1]; } BanCheckJob;
static void *ban_check_job(void *arg) {
    BanCheckJob *j = (BanCheckJob *)arg;
    if (j) { ban_check_cloud_sync(j->nick); free(j); }
    return NULL;
}

/* Whether a nick is banned as far as the game thread knows. A synchronous HTTP
 * request used to sit here: with no connection it held the game thread until the
 * network timed out, frames and input froze, and the system showed an
 * application-not-responding dialog. The answer is now instant from the local
 * cache while the cloud check goes to a thread of its own and refills it. */
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