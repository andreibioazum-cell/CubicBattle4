/* Слияние облачной записи игрока и локальной копии при входе.
 *
 * /users/<ник> — источник правды для аккаунта, progress.dat на устройстве —
 * его копия. Копия и облако сливаются, а не перезаписывают друг друга: раньше
 * облачная запись просто затирала локальную, и купленный класс исчезал, если
 * его покупка не доехала до базы (старые правила, протухший токен, обрыв
 * сети). Разбор JSON опирается на num/strv из auth_json.inc, поэтому файл
 * включается после него. */
/* Разбор записи /users/<ник> в профиль. Пароль не читаем: в защищённом режиме
 * его проверяет Firebase Auth, а в базе его нет. */
static void profile_from_json(const char *resp, Profile *out) {
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
    memset(out, 0, sizeof(*out));
    out->cups = (int)num(resp, "cups", 0);
    out->candies = (int)num(resp, "candies", 0);
    out->bp_level = (int)num(resp, "bp", 0);
    out->azum_skin = (int)num(resp, "azum_skin", 0);
    out->ach = (unsigned int)num(resp, "ach", 0);
    out->ts = (long long)num(resp, "ts", 0);
    out->lang = (int)num(resp, "lang", 0);
    strv(resp, "nick", out->owner, sizeof(out->owner));
    azum = azum ? 1 : 0;
    santa = santa ? 1 : 0;
    ebuc = ebuc ? 1 : 0;
    if (cls != 1 && cls != 2 && cls != 3) cls = 0;
    if (cls == 1 && !azum) cls = 0;
    if (cls == 2 && !santa) cls = 0;
    if (cls == 3 && !ebuc) cls = 0;
    if (levels_unlocked < 0) levels_unlocked = level;
    /* Ветки уровней появились позже общих level/levels: если их в записи нет,
     * раскладываем общий прогресс по ветке выбранного класса. */
    if (ordinary_level < 0 && ordinary_levels_unlocked < 0 &&
        azum_level < 0 && azum_levels_unlocked < 0 &&
        santa_level < 0 && santa_levels_unlocked < 0 &&
        ebuc_level < 0 && ebuc_levels_unlocked < 0) {
        ordinary_level = ordinary_levels_unlocked = 0;
        azum_level = azum_levels_unlocked = 0;
        santa_level = santa_levels_unlocked = 0;
        ebuc_level = ebuc_levels_unlocked = 0;
        if (cls == 1) { azum_level = level; azum_levels_unlocked = levels_unlocked; }
        else if (cls == 2) { santa_level = level; santa_levels_unlocked = levels_unlocked; }
        else if (cls == 3) { ebuc_level = level; ebuc_levels_unlocked = levels_unlocked; }
        else { ordinary_level = level; ordinary_levels_unlocked = levels_unlocked; }
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
    out->cls = cls; out->azum = azum; out->santa = santa; out->ebuc = ebuc;
    out->level = level; out->levels_unlocked = levels_unlocked;
    out->ordinary_level = ordinary_level; out->ordinary_levels_unlocked = ordinary_levels_unlocked;
    out->azum_level = azum_level; out->azum_levels_unlocked = azum_levels_unlocked;
    out->santa_level = santa_level; out->santa_levels_unlocked = santa_levels_unlocked;
    out->ebuc_level = ebuc_level; out->ebuc_levels_unlocked = ebuc_levels_unlocked;
    profile_clamp(out);
}

/* Уровни одной ветки при слиянии: открыто — максимум из двух копий, выбранный
 * уровень — не выше открытого. */
static void merge_level_pair(int cloud_lv, int cloud_un, int local_lv, int local_un,
                             int *lv, int *un) {
    *un = imax(cloud_un, local_un);
    *lv = imax(cloud_lv, local_lv);
    if (*lv > *un) *lv = *un;
}

/* Куплен ли класс в этом профиле (обычный доступен всегда). */
static int profile_owns_class(const Profile *p, int cls) {
    if (cls == 1) return p->azum ? 1 : 0;
    if (cls == 2) return p->santa ? 1 : 0;
    if (cls == 3) return p->ebuc ? 1 : 0;
    return cls == 0;
}

/* Слияние облака и локальной копии. Правило: покупки не теряются никогда,
 * траты берутся из более свежей копии.
 *   • владение классами (azum/santa/ebuc), скин — ИЛИ;
 *   • уровни веток и батл пасс — максимум;
 *   • достижения — объединение битов;
 *   • кубки, леденцы и выбранный класс — из той копии, что сохранена позже (ts).
 * local_trusted — локальную копию вообще можно сливать: она принадлежит этому
 * нику (или досталась от версии, которая владельца не писала). */
static void profile_merge(const Profile *cloud, const Profile *local, int local_trusted, Profile *out) {
    *out = *cloud;
    if (!local_trusted) { profile_clamp(out); return; }
    out->azum = (cloud->azum || local->azum) ? 1 : 0;
    out->santa = (cloud->santa || local->santa) ? 1 : 0;
    out->ebuc = (cloud->ebuc || local->ebuc) ? 1 : 0;
    merge_level_pair(cloud->ordinary_level, cloud->ordinary_levels_unlocked,
                     local->ordinary_level, local->ordinary_levels_unlocked,
                     &out->ordinary_level, &out->ordinary_levels_unlocked);
    merge_level_pair(cloud->azum_level, cloud->azum_levels_unlocked,
                     local->azum_level, local->azum_levels_unlocked,
                     &out->azum_level, &out->azum_levels_unlocked);
    merge_level_pair(cloud->santa_level, cloud->santa_levels_unlocked,
                     local->santa_level, local->santa_levels_unlocked,
                     &out->santa_level, &out->santa_levels_unlocked);
    merge_level_pair(cloud->ebuc_level, cloud->ebuc_levels_unlocked,
                     local->ebuc_level, local->ebuc_levels_unlocked,
                     &out->ebuc_level, &out->ebuc_levels_unlocked);
    out->bp_level = imax(cloud->bp_level, local->bp_level);
    out->azum_skin = (cloud->azum_skin || local->azum_skin) ? 1 : 0;
    out->ach = cloud->ach | local->ach;
    out->ts = llmax(cloud->ts, local->ts);
    if (local->ts > cloud->ts) {
        /* Локальная копия новее: значит, последнее сохранение было здесь, а
         * до облака оно не доехало (или там ещё старая запись). */
        out->cups = local->cups;
        out->candies = local->candies;
        out->cls = local->cls;
        out->lang = local->lang;
    } else if (local->ts == cloud->ts && local->cls != cloud->cls &&
               !profile_owns_class(cloud, local->cls)) {
        /* Меток времени нет (сохранения старых версий), но локально выбран
         * класс, которого облако не знает: это и есть покупка, которая не
         * доехала. Оставляем игрока в его классе, а не в обычном. */
        out->cls = local->cls;
    }
    profile_clamp(out);
}

/* Отличается ли профиль от облачного (нужен ли PATCH после входа). */
static int profile_differs(const Profile *a, const Profile *b) {
    return a->cups != b->cups || a->candies != b->candies || a->cls != b->cls
        || a->azum != b->azum || a->santa != b->santa || a->ebuc != b->ebuc
        || a->level != b->level || a->levels_unlocked != b->levels_unlocked
        || a->ordinary_level != b->ordinary_level || a->ordinary_levels_unlocked != b->ordinary_levels_unlocked
        || a->azum_level != b->azum_level || a->azum_levels_unlocked != b->azum_levels_unlocked
        || a->santa_level != b->santa_level || a->santa_levels_unlocked != b->santa_levels_unlocked
        || a->ebuc_level != b->ebuc_level || a->ebuc_levels_unlocked != b->ebuc_levels_unlocked
        || a->bp_level != b->bp_level || a->azum_skin != b->azum_skin
        || a->lang != b->lang || a->ach != b->ach || a->ts != b->ts;
}

/* Применить облачную запись при входе: слить с локальной копией, записать
 * progress.dat и achievements.dat. Возврат 1 — облако отстало от слияния,
 * после установки сессии его нужно обновить (так покупка, которая не доехала
 * в прошлый раз, доезжает сейчас). */
static int apply_user_json_merge(const char *resp, const char *nick) {
    Profile cloud, local, merged;
    unsigned int local_ach;
    int have_local = 0, trusted, changed;
    profile_from_json(resp, &cloud);
    progress_read();
    achievements_read();
    ds_mutex_lock(ach_lock); local_ach = ach_flags; ds_mutex_unlock(ach_lock);
    memset(&local, 0, sizeof(local));
    pg_lock();
    if (pg.loaded) { local = pg.p; have_local = 1; }
    pg_unlock();
    local.ach = local_ach;
    trusted = have_local && (local.owner[0] == 0 || nick_same(local.owner, nick));
    profile_merge(&cloud, &local, trusted, &merged);
    snprintf(merged.owner, sizeof(merged.owner), "%s", nick ? nick : "");
    changed = profile_differs(&cloud, &merged);
    pg_lock();
    pg.p = merged;
    pg.loaded = 1;
    pg_unlock();
    pending_cls = merged.cls;
    pending_level = merged.level;
    pending_skin = merged.azum_skin;
    progress_write(&merged);
    if (merged.ach != local_ach) {
        ds_mutex_lock(ach_lock);
        ach_flags = merged.ach;
        ach_loaded = 1;
        ds_mutex_unlock(ach_lock);
        achievements_write(merged.ach);
    }
    if (changed)
        LOG("profile '%s': merged with local copy (cloud was behind), pushing back", nick ? nick : "?");
    return changed;
}
