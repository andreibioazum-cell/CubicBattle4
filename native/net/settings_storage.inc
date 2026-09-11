/* Настройки игрока: язык, хитбоксы и список модов.
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
 *   mods 2
 *   mod mymod.zip
 *   mod other.zip
 * Неизвестные строки при чтении игнорируются, поэтому добавление нового
 * параметра не ломает старые сохранения.
 */
#define SETTINGS_FILE "settings.dat"
#define SETTINGS_MODS_MAX 6
#define SETTINGS_NAME_MAX 64

typedef struct {
    DSMutex lock;
    int loaded, has_file;
    int language, hitboxes;
    int mods_count;
    char mods[SETTINGS_MODS_MAX][SETTINGS_NAME_MAX];
} Settings;
static Settings st = { .lock = DS_MUTEX_INIT };
/* Свой буфер на каждый слот: settings_from_storage() читает все шесть имён
 * подряд и держит их в mod_1..mod_6, поэтому один общий буфер (как у
 * s_chat_text_ret) здесь не годится — все шесть переменных показали бы
 * последнее прочитанное имя. */
static char s_mod_ret[SETTINGS_MODS_MAX][SETTINGS_NAME_MAX];

static void st_lock(void) { ds_mutex_lock(st.lock); }
static void st_unlock(void) { ds_mutex_unlock(st.lock); }

/* Имя мода приходит из поля ввода, поэтому в файл и в JSON оно уходит
 * очищенным: без разделителя '|', без кавычек/обратных слэшей (они сломали бы
 * JSON) и без управляющих символов. */
static void settings_clean_name(const char *src, char *dst, size_t cap) {
    size_t n = 0;
    if (!src) src = "";
    for (; *src && n + 1 < cap; src++) {
        unsigned char c = (unsigned char)*src;
        if (c < 0x20 || c == '|' || c == '"' || c == '\\' || c == '\n' || c == '\r') continue;
        dst[n++] = (char)c;
    }
    dst[n] = '\0';
}

static void settings_read(void) {
    char path[320], line[SETTINGS_NAME_MAX + 32];
    FILE *f;
    int language = 0, hitboxes = 1, mods_count = 0, has_file = 0;
    char mods[SETTINGS_MODS_MAX][SETTINGS_NAME_MAX];
    st_lock();
    if (st.loaded) { st_unlock(); return; }
    st_unlock();
    memset(mods, 0, sizeof(mods));
    data_file_path(path, sizeof(path), SETTINGS_FILE);
    f = fopen(path, "r");
    if (f) {
        has_file = 1;
        while (fgets(line, sizeof(line), f)) {
            char name[SETTINGS_NAME_MAX];
            int value = 0;
            if (sscanf(line, "lang %d", &value) == 1) { language = value; continue; }
            if (sscanf(line, "hitboxes %d", &value) == 1) { hitboxes = value; continue; }
            if (sscanf(line, "mods %d", &value) == 1) { continue; }
            if (sscanf(line, "mod %63[^\r\n]", name) == 1) {
                if (mods_count < SETTINGS_MODS_MAX) {
                    settings_clean_name(name, mods[mods_count], SETTINGS_NAME_MAX);
                    if (mods[mods_count][0]) mods_count++;
                }
                continue;
            }
        }
        fclose(f);
    }
    st_lock();
    st.language = language ? 1 : 0;
    st.hitboxes = hitboxes ? 1 : 0;
    st.mods_count = mods_count;
    memcpy(st.mods, mods, sizeof(mods));
    st.has_file = has_file;
    st.loaded = 1;
    st_unlock();
}

/* Атомарная запись (временный файл + rename) — как у прогресса и достижений. */
static void settings_write(void) {
    char path[320], text[1024];
    size_t off = 0;
    int i;
    settings_read();
    data_file_path(path, sizeof(path), SETTINGS_FILE);
    st_lock();
    off += (size_t)snprintf(text + off, sizeof(text) - off, "lang %d\nhitboxes %d\nmods %d\n",
                            st.language, st.hitboxes, st.mods_count);
    for (i = 0; i < st.mods_count && i < SETTINGS_MODS_MAX; i++) {
        if (off + 1 >= sizeof(text)) break;
        off += (size_t)snprintf(text + off, sizeof(text) - off, "mod %s\n", st.mods[i]);
        if (off > sizeof(text) - 1) off = sizeof(text) - 1;
    }
    st.has_file = 1;
    st_unlock();
    file_write_atomic(path, text);
}

/* Список модов одной строкой через '|' — так же его читает облако. */
static void settings_mods_joined(char *dst, size_t cap) {
    int i;
    size_t off = 0;
    dst[0] = '\0';
    st_lock();
    for (i = 0; i < st.mods_count && i < SETTINGS_MODS_MAX; i++) {
        if (off + 1 >= cap) break;
        off += (size_t)snprintf(dst + off, cap - off, "%s%s", i ? "|" : "", st.mods[i]);
        if (off > cap - 1) off = cap - 1;
    }
    st_unlock();
}

/* Настройки в облако: lang, hitboxes, mods_n и mods одним PATCH'ем. */
static void settings_push_to_cloud(void) {
    char nick[LOGIN_NICK_MAX + 1] = "";
    char mods[SETTINGS_MODS_MAX * SETTINGS_NAME_MAX + SETTINGS_MODS_MAX];
    int language, hitboxes, mods_count;
    settings_read();
    st_lock();
    language = st.language; hitboxes = st.hitboxes; mods_count = st.mods_count;
    st_unlock();
    settings_mods_joined(mods, sizeof(mods));
    lg_lock();
    if (lg.status == NET_LOGIN_OK && lg.session_nick[0]) {
        snprintf(nick, sizeof(nick), "%s", lg.session_nick);
    }
    lg_unlock();
    if (!nick[0] || !net.base[0]) return;
    char url[URL], body[BODY * 2];
    snprintf(url, sizeof(url), "%s/users/%s.json", net.base, nick);
    snprintf(body, sizeof(body),
             "{\"lang\":%d,\"hitboxes\":%d,\"mods_n\":%d,\"mods\":\"%s\"}",
             language, hitboxes, mods_count, mods);
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

double net_load_mod_count(void) {
    double v;
    settings_read();
    st_lock(); v = (double)st.mods_count; st_unlock();
    return v;
}

const char *net_load_mod(double idx) {
    int i = (int)idx;
    settings_read();
    st_lock();
    if (i >= 0 && i < st.mods_count && i < SETTINGS_MODS_MAX) {
        snprintf(s_mod_ret[i], SETTINGS_NAME_MAX, "%s", st.mods[i]);
    } else {
        s_mod_ret[0][0] = '\0';
        st_unlock();
        return s_mod_ret[0];
    }
    st_unlock();
    return s_mod_ret[i];
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

/* Список модов целиком: шесть слотов, как в core/config.ds (строковых массивов
 * в DimScript нет). Пустые имена в хвосте просто не сохраняются. */
void net_save_mods(double count, const char *m1, const char *m2, const char *m3,
                   const char *m4, const char *m5, const char *m6) {
    const char *src[SETTINGS_MODS_MAX];
    int n = (int)count, i, kept = 0;
    src[0] = m1; src[1] = m2; src[2] = m3; src[3] = m4; src[4] = m5; src[5] = m6;
    if (n < 0) n = 0;
    if (n > SETTINGS_MODS_MAX) n = SETTINGS_MODS_MAX;
    settings_read();
    st_lock();
    memset(st.mods, 0, sizeof(st.mods));
    for (i = 0; i < n; i++) {
        char clean[SETTINGS_NAME_MAX];
        settings_clean_name(src[i], clean, sizeof(clean));
        if (!clean[0]) continue;
        snprintf(st.mods[kept], SETTINGS_NAME_MAX, "%s", clean);
        kept++;
    }
    st.mods_count = kept;
    st.loaded = 1;
    st_unlock();
    settings_write();
    settings_push_to_cloud();
}

/* После входа: облако может добавить настройки туда, где их ещё нет (свежая
 * установка), но не отнимает то, что уже лежит на устройстве — как и в
 * прогрессе. Возврат 1 — локальные настройки новее облачных, их нужно поднять. */
int settings_sync_with_cloud(const char *resp) {
    int had_file, language, hitboxes, mods_n;
    char mods[SETTINGS_MODS_MAX * SETTINGS_NAME_MAX + SETTINGS_MODS_MAX];
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
    mods_n = (int)num(resp, "mods_n", -1);
    if (language < 0 && hitboxes < 0 && mods_n < 0) return 0;
    st_lock();
    if (language >= 0) st.language = language ? 1 : 0;
    if (hitboxes >= 0) st.hitboxes = hitboxes ? 1 : 0;
    st.loaded = 1;
    memset(st.mods, 0, sizeof(st.mods));
    st.mods_count = 0;
    st_unlock();
    mods[0] = '\0';
    strv(resp, "mods", mods, sizeof(mods));
    if (mods[0]) {
        /* Список «a|b|c» разбираем вручную: strtok_r здесь не нужен, а пустые
         * имена между '|' просто пропускаются. */
        int count = 0;
        const char *p = mods;
        while (*p && count < SETTINGS_MODS_MAX) {
            const char *sep = strchr(p, '|');
            size_t len = sep ? (size_t)(sep - p) : strlen(p);
            char part[SETTINGS_NAME_MAX];
            if (len >= sizeof(part)) len = sizeof(part) - 1;
            memcpy(part, p, len);
            part[len] = '\0';
            if (part[0]) {
                st_lock();
                settings_clean_name(part, st.mods[count], SETTINGS_NAME_MAX);
                if (st.mods[count][0]) count++;
                st_unlock();
            }
            if (!sep) break;
            p = sep + 1;
        }
        st_lock();
        st.mods_count = count;
        st_unlock();
    }
    settings_write();
    if (net_log_ok()) LOG("settings restored from cloud (lang=%d hitboxes=%d mods=%d)",
                          language, hitboxes, mods_n);
    return 0;
}
