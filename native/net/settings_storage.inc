/* Настройки игрока: язык и переключатель хитбоксов.
 *
 * Хранятся так же, как прогресс: сначала на устройстве (settings.dat рядом с
 * progress.dat и auth.dat), затем в облаке — отдельным PATCH'ем в профиль
 * /users/<ник>. Отдельный запрос намеренно: правила Firebase могут быть
 * опубликованы без новых полей, и тогда-settings PATCH просто не пройдёт, а
 * прогресс (кубки, классы, уровни) продолжит сохраняться как раньше.
 *
 * Файл настроек строковый и терпимый к будущим полям:
 *   lang 1
 *   hitboxes 0
 * Неизвестные строки при чтении игнорируются, поэтому добавление нового
 * параметра не ломает старые сохранения. По той же причине безболезненно
 * переживают перезапуск и старые файлы со списком модов (строки "mods N" и
 * "mod имя"): моды из игры убраны, их строки просто пропускаются и при первой
 * же записи исчезают из файла.
 */
#define SETTINGS_FILE "settings.dat"

typedef struct {
    DSMutex lock;
    int loaded, has_file;
    int language, hitboxes;
} Settings;
static Settings st = { .lock = DS_MUTEX_INIT };

static void st_lock(void) { ds_mutex_lock(st.lock); }
static void st_unlock(void) { ds_mutex_unlock(st.lock); }

static void settings_read(void) {
    char path[320], line[128];
    FILE *f;
    int language = 0, hitboxes = 1, has_file = 0;
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
        }
        fclose(f);
    }
    st_lock();
    st.language = language ? 1 : 0;
    st.hitboxes = hitboxes ? 1 : 0;
    st.has_file = has_file;
    st.loaded = 1;
    st_unlock();
}

/* Атомарная запись (временный файл + rename) — как у прогресса и достижений. */
static void settings_write(void) {
    char path[320], text[128];
    settings_read();
    data_file_path(path, sizeof(path), SETTINGS_FILE);
    st_lock();
    snprintf(text, sizeof(text), "lang %d\nhitboxes %d\n", st.language, st.hitboxes);
    st.has_file = 1;
    st_unlock();
    file_write_atomic(path, text);
}

/* Настройки в облако: lang и hitboxes одним PATCH'ем. */
static void settings_push_to_cloud(void) {
    char nick[LOGIN_NICK_MAX + 1] = "";
    char body[BODY];
    int language, hitboxes;
    settings_read();
    st_lock();
    language = st.language; hitboxes = st.hitboxes;
    st_unlock();
    lg_lock();
    if (lg.status == NET_LOGIN_OK && lg.session_nick[0]) {
        snprintf(nick, sizeof(nick), "%s", lg.session_nick);
    }
    lg_unlock();
    if (!nick[0] || !net.base[0]) return;
    char url[URL];
    snprintf(url, sizeof(url), "%s/users/%s.json", net.base, nick);
    snprintf(body, sizeof(body), "{\"lang\":%d,\"hitboxes\":%d}", language, hitboxes);
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

/* Язык и хитбоксы: пишем на устройство и сразу отправляем в облако. */
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

/* После входа: облако может добавить настройки туда, где их ещё нет (свежая
 * установка), но не отнимает то, что уже лежит на устройстве — как и в
 * прогрессе. Возврат 1 — локальные настройки новее облачных, их нужно поднять. */
int settings_sync_with_cloud(const char *resp) {
    int had_file, language, hitboxes;
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
    if (language < 0 && hitboxes < 0) return 0;
    st_lock();
    if (language >= 0) st.language = language ? 1 : 0;
    if (hitboxes >= 0) st.hitboxes = hitboxes ? 1 : 0;
    st.loaded = 1;
    st_unlock();
    settings_write();
    if (net_log_ok()) LOG("settings restored from cloud (lang=%d hitboxes=%d)",
                          language, hitboxes);
    return 0;
}
