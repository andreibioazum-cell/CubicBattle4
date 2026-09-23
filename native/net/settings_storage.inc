/* Player settings: the language and the hitbox toggle.
 *
 * They are stored the way progress is: on the device first (settings.dat next to
 * progress.dat and auth.dat) and then in the cloud through a PATCH of its own into
 * the profile /users/<nick>. The separate request is deliberate, because the
 * Firebase rules may be published before the new fields exist, and then the
 * settings PATCH simply fails while progress, cups, classes and levels, keeps
 * saving as before.
 *
 * The settings file is line based and tolerant of future fields:
 *   lang 1
 *   hitboxes 0
 *   musicvol 70
 *   winter 1
 *   showfps 0
 * Unknown lines are ignored while reading, so a new setting does not break old
 * saves. For the same reason files that still hold removed settings survive a
 * restart without trouble (the lines "fps N", "scale N" and the mod list
 * "mods N"/"mod name"): the game has neither a frame limit nor an upscale any
 * more, so those lines are skipped and disappear at the first write.
 */
#define SETTINGS_FILE "settings.dat"

typedef struct {
    DSMutex lock;
    int loaded, has_file;
    int language, hitboxes, musicvol, winter, showfps;
    int legal; /* unix time of the consent button press on the warning screen */
} Settings;
static Settings st = { .lock = DS_MUTEX_INIT };

static void st_lock(void) { ds_mutex_lock(st.lock); }
static void st_unlock(void) { ds_mutex_unlock(st.lock); }

static void settings_read(void) {
    char path[320], line[128];
    FILE *f;
    int language = 0, hitboxes = 1, musicvol = 70, has_file = 0;
    int winter = 1, showfps = 0, legal = 0;
    st_lock();
    if (st.loaded) { st_unlock(); return; }
    st_unlock();
    data_file_path(path, sizeof(path), SETTINGS_FILE);
    f = fopen(path, "r");
    if (f) {
        has_file = 1;
        while (fgets(line, sizeof(line), f)) {
            int value = 0;
            if (sscanf(line, "lang %d", &value) == 1) { language = value; continue; }
            if (sscanf(line, "hitboxes %d", &value) == 1) { hitboxes = value; continue; }
            if (sscanf(line, "musicvol %d", &value) == 1) { musicvol = value; continue; }
            if (sscanf(line, "winter %d", &value) == 1) { winter = value; continue; }
            if (sscanf(line, "showfps %d", &value) == 1) { showfps = value; continue; }
            if (sscanf(line, "legal %d", &value) == 1) { legal = value; continue; }
        }
        fclose(f);
    }
    st_lock();
    st.language = language ? 1 : 0;
    st.hitboxes = hitboxes ? 1 : 0;
    st.musicvol = musicvol < 0 ? 0 : (musicvol > 100 ? 100 : musicvol);
    st.winter = winter ? 1 : 0;
    st.showfps = showfps ? 1 : 0;
    st.legal = legal;
    st.has_file = has_file;
    st.loaded = 1;
    st_unlock();
}

/* Atomic write through a temporary file and a rename, as for progress and
 * achievements. */
static void settings_write(void) {
    char path[320], text[192];
    settings_read();
    data_file_path(path, sizeof(path), SETTINGS_FILE);
    st_lock();
    snprintf(text, sizeof(text),
             "lang %d\nhitboxes %d\nmusicvol %d\nwinter %d\nshowfps %d\n"
             "legal %d\n",
             st.language, st.hitboxes, st.musicvol, st.winter, st.showfps,
             st.legal);
    st.has_file = 1;
    st_unlock();
    file_write_atomic(path, text);
}

/* Settings to the cloud: lang and hitboxes in a single PATCH. */
static void settings_push_to_cloud(void) {
    char nick[LOGIN_NICK_MAX + 1] = "";
    char body[BODY];
    int language, hitboxes, musicvol, winter, showfps;
    settings_read();
    st_lock();
    language = st.language; hitboxes = st.hitboxes; musicvol = st.musicvol;
    winter = st.winter; showfps = st.showfps;
    st_unlock();
    lg_lock();
    if (lg.status == NET_LOGIN_OK && lg.session_nick[0]) {
        snprintf(nick, sizeof(nick), "%s", lg.session_nick);
    }
    lg_unlock();
    if (!nick[0] || !net.base[0]) return;
    char url[URL];
    snprintf(url, sizeof(url), "%s/users/%s.json", net.base, nick);
    snprintf(body, sizeof(body),
             "{\"lang\":%d,\"hitboxes\":%d,\"musicvol\":%d,"
             "\"winter\":%d,\"showfps\":%d}",
             language, hitboxes, musicvol, winter, showfps);
    http_patch_async(url, body);
}

double net_load_language(void) {
    double v;
    settings_read();
    st_lock(); v = (double)st.language; st_unlock();
    return v;
}

double net_load_hitboxes(void) {
    double v;
    settings_read();
    st_lock(); v = (double)st.hitboxes; st_unlock();
    return v;
}

double net_load_music_volume(void) {
    double v;
    settings_read();
    st_lock(); v = (double)st.musicvol; st_unlock();
    return v;
}

/* Music volume, 0 to 100: written to the device and to the cloud at once. */
void net_save_music_volume(double volume) {
    int v = (int)volume;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    settings_read();
    st_lock();
    st.musicvol = v;
    st.loaded = 1;
    st_unlock();
    settings_write();
    settings_push_to_cloud();
}

/* Winter theme, a snowy arena background and snowfall, on by default. */
double net_load_winter_theme(void) {
    double v;
    settings_read();
    st_lock(); v = (double)st.winter; st_unlock();
    return v;
}

void net_save_winter_theme(double on) {
    settings_read();
    st_lock();
    st.winter = on ? 1 : 0;
    st.loaded = 1;
    st_unlock();
    settings_write();
    settings_push_to_cloud();
}

/* The fps counter in battle, off by default. */
double net_load_fps_meter(void) {
    double v;
    settings_read();
    st_lock(); v = (double)st.showfps; st_unlock();
    return v;
}

void net_save_fps_meter(double on) {
    settings_read();
    st_lock();
    st.showfps = on ? 1 : 0;
    st.loaded = 1;
    st_unlock();
    settings_write();
    settings_push_to_cloud();
}

/* Language and hitboxes: written to the device and pushed to the cloud at once. */
void net_save_settings(double language, double hitboxes) {
    settings_read();
    st_lock();
    st.language = language ? 1 : 0;
    st.hitboxes = hitboxes ? 1 : 0;
    st.loaded = 1;
    st_unlock();
    settings_write();
    settings_push_to_cloud();
}

/* After a login the cloud may add settings where there are none, on a fresh
 * install, but never takes away what the device holds, the way progress behaves.
 * A return of 1 means the local settings are newer and have to be pushed up. */
int settings_sync_with_cloud(const char *resp) {
    int had_file, language, hitboxes, musicvol, winter, showfps;
    settings_read();
    st_lock();
    had_file = st.has_file;
    st_unlock();
    if (had_file) {
        settings_push_to_cloud();
        return 1;
    }
    language = (int)num(resp, "lang", -1);
    hitboxes = (int)num(resp, "hitboxes", -1);
    musicvol = (int)num(resp, "musicvol", -1);
    winter = (int)num(resp, "winter", -1);
    showfps = (int)num(resp, "showfps", -1);
    if (language < 0 && hitboxes < 0 && musicvol < 0 &&
        winter < 0 && showfps < 0) return 0;
    st_lock();
    if (language >= 0) st.language = language ? 1 : 0;
    if (hitboxes >= 0) st.hitboxes = hitboxes ? 1 : 0;
    if (musicvol >= 0) st.musicvol = musicvol > 100 ? 100 : musicvol;
    if (winter >= 0) st.winter = winter ? 1 : 0;
    if (showfps >= 0) st.showfps = showfps ? 1 : 0;
    st.loaded = 1;
    st_unlock();
    settings_write();
    if (net_log_ok()) LOG("settings restored from cloud (lang=%d hitboxes=%d musicvol=%d winter=%d showfps=%d)",
                          language, hitboxes, musicvol, winter, showfps);
    return 0;
}
/* Consent to the epilepsy warning: marking and reading the timestamp.
 *
 * It lives on the device only (the line "legal <unix time>" in settings.dat) and
 * is deliberately not synced with the cloud: consent is given by the person in
 * front of this screen, not by an account. settings_mark_legal is called by the
 * script when the "I understand and accept the risk" button on the warning
 * screen is pressed, and settings_legal_ts hands the timestamp to tests and
 * debug code.
 */
void settings_mark_legal(void) {
    settings_read();
    st_lock();
    st.legal = (int)time(NULL);
    st.loaded = 1;
    st_unlock();
    settings_write();
}

double settings_legal_ts(void) {
    double v;
    settings_read();
    st_lock();
    v = (double)st.legal;
    st_unlock();
    return v;
}
