/* Локальное хранилище профиля: progress.dat, achievements.dat, чтение профиля
 * скриптом и сохранение (файл + облако). Отправка в облако — profile_cloud.inc,
 * слияние с облачной записью при входе — profile_merge.inc. */
/* ── Локальные файлы профиля ───────────────────────────────────────────────
 * progress.dat: 20 чисел (18 полей прогресса, ts и lang) и ник владельца
 * второй строкой. Старые файлы короче — чего не хватило, то считается нулём, а
 * общий level/levels раскладывается по ветке выбранного класса. */

/* Привести профиль к допустимым значениям: границы валют, выбранный класс
 * только из купленных, уровни в пределах ветки, батл пасс и скин, а в конце —
 * зеркала level/levels_unlocked выбранного класса (именно их читает бой).
 * Одна функция на файл, облако и слияние: раньше одинаковые проверки жили в
 * трёх местах и успели разойтись. */
static void profile_clamp(Profile *p) {
    if (!p) return;
    if (p->cups < 0) p->cups = 0;
    if (p->candies < 0) p->candies = 0;
    if (p->cls != 1 && p->cls != 2 && p->cls != 3) p->cls = 0;
    p->azum = p->azum ? 1 : 0;
    p->santa = p->santa ? 1 : 0;
    p->ebuc = p->ebuc ? 1 : 0;
    /* Выбранный класс обязан быть купленным: иначе бой играл бы чужой веткой. */
    if (p->cls == 1 && !p->azum) p->cls = 0;
    if (p->cls == 2 && !p->santa) p->cls = 0;
    if (p->cls == 3 && !p->ebuc) p->cls = 0;
    normalize_level_pair(&p->level, &p->levels_unlocked);
    normalize_level_pair(&p->ordinary_level, &p->ordinary_levels_unlocked);
    normalize_level_pair(&p->azum_level, &p->azum_levels_unlocked);
    normalize_level_pair(&p->santa_level, &p->santa_levels_unlocked);
    normalize_level_pair(&p->ebuc_level, &p->ebuc_levels_unlocked);
    if (p->bp_level < 0) p->bp_level = 0;
    if (p->bp_level > BP_MAX) p->bp_level = BP_MAX;
    p->azum_skin = (p->azum_skin >= 1) ? SKIN_MAX : 0;
    p->lang = (p->lang == 1) ? 1 : 0;
    if (p->ts < 0) p->ts = 0;
    if (p->cls == 0) { p->level = p->ordinary_level; p->levels_unlocked = p->ordinary_levels_unlocked; }
    else if (p->cls == 1) { p->level = p->azum_level; p->levels_unlocked = p->azum_levels_unlocked; }
    else if (p->cls == 2) { p->level = p->santa_level; p->levels_unlocked = p->santa_levels_unlocked; }
    else { p->level = p->ebuc_level; p->levels_unlocked = p->ebuc_levels_unlocked; }
}

static void progress_write(const Profile *p) {
    char path[320]; FILE *f;
    if (!p) return;
    data_file_path(path, sizeof(path), PROGRESS_FILE);
    f = fopen(path, "w");
    if (!f) {
        /* Молча потерять прогресс хуже любого лога: на Android это обычно
         * значит, что каталог данных пришёл пустым (net_set_data_path). */
        if (net_log_ok()) LOGERR("progress: cannot write %s", path);
        return;
    }
    fprintf(f, "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %lld %d\n%s\n",
            p->cups, p->cls, p->azum, p->santa, p->candies, p->level, p->levels_unlocked,
            p->ordinary_level, p->ordinary_levels_unlocked,
            p->azum_level, p->azum_levels_unlocked,
            p->santa_level, p->santa_levels_unlocked,
            p->ebuc_level, p->ebuc_levels_unlocked, p->ebuc,
            p->bp_level, p->azum_skin, p->ts, p->lang, p->owner);
    fclose(f);
}

static void progress_read(void) {
    char path[320]; FILE *f;
    Profile p;
    pg_lock();
    if (pg.loaded) { pg_unlock(); return; }
    pg_unlock();
    memset(&p, 0, sizeof(p));
    data_file_path(path, sizeof(path), PROGRESS_FILE);
    f = fopen(path, "r");
    if (f) {
        int n = fscanf(f, "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %lld %d",
                       &p.cups, &p.cls, &p.azum, &p.santa, &p.candies,
                       &p.level, &p.levels_unlocked,
                       &p.ordinary_level, &p.ordinary_levels_unlocked,
                       &p.azum_level, &p.azum_levels_unlocked,
                       &p.santa_level, &p.santa_levels_unlocked,
                       &p.ebuc_level, &p.ebuc_levels_unlocked, &p.ebuc,
                       &p.bp_level, &p.azum_skin, &p.ts, &p.lang);
        if (n < 3) {
            memset(&p, 0, sizeof(p));
        } else {
            if (n < 6) { p.level = 0; p.levels_unlocked = 0; }
            else if (n < 7) { p.levels_unlocked = p.level; }
            if (n < 13) {
                p.ordinary_level = p.ordinary_levels_unlocked = 0;
                p.azum_level = p.azum_levels_unlocked = 0;
                p.santa_level = p.santa_levels_unlocked = 0;
                p.ebuc_level = p.ebuc_levels_unlocked = 0;
                if (p.cls == 1 && p.azum) { p.azum_level = p.level; p.azum_levels_unlocked = p.levels_unlocked; }
                else if (p.cls == 2 && p.santa) { p.santa_level = p.level; p.santa_levels_unlocked = p.levels_unlocked; }
                else { p.ordinary_level = p.level; p.ordinary_levels_unlocked = p.levels_unlocked; }
            }
            if (n < 16) { p.ebuc_level = p.ebuc_levels_unlocked = 0; p.ebuc = 0; }
            if (n < 17) { p.bp_level = 0; p.azum_skin = 0; }
            else if (n < 18) { p.azum_skin = 0; }
            if (n < 19) p.ts = 0;
            if (n < PROFILE_NUMBERS) p.lang = 0;
        }
        /* Ник владельца файла: у старых сохранений его нет, тогда копия
         * считается «ничейной» и сливается с тем аккаунтом, который вошёл. */
        if (fscanf(f, "%16s", p.owner) != 1) p.owner[0] = 0;
        fclose(f);
    }
    p.ach = 0;
    profile_clamp(&p);
    pg_lock();
    pg.p = p;
    pg.loaded = 1;
    pg_unlock();
}

/* ── Достижения (achievements.dat) ─────────────────────────────────────────
 * Отдельный маленький файл с единым битмаском: ACH_FLAG_* задают отдельные
 * награды, добавление нового бита не ломает старые записи. В облако маска
 * уходит полем ach записи игрока (см. profile_push_cloud), поэтому
 * достижения больше не живут только на одном устройстве. */
static void achievements_read(void) {
    char path[320];
    FILE *f;
    unsigned int flags = 0;
    ds_mutex_lock(ach_lock);
    if (ach_loaded) { ds_mutex_unlock(ach_lock); return; }
    ds_mutex_unlock(ach_lock);
    data_file_path(path, sizeof(path), ACHIEVEMENTS_FILE);
    f = fopen(path, "r");
    if (f) {
        /* Современный формат — одно целое число-битмаск. Старые сборки писали
         * «welcome all_characters» (0/1 0/1): такой файл тоже читаем и
         * перекладываем в биты. */
        char line[64];
        if (fgets(line, sizeof(line), f)) {
            int a = 0, b = 0;
            if (sscanf(line, "%d %d", &a, &b) == 2) {
                flags = (unsigned int)((a ? ACH_FLAG_WELCOME : 0)
                                       | (b ? ACH_FLAG_ALL_CHARACTERS : 0));
            } else if (sscanf(line, "%u", &flags) != 1) {
                flags = 0;
            }
        } else {
            flags = 0;
        }
        fclose(f);
    }
    ds_mutex_lock(ach_lock);
    ach_flags = flags;
    ach_loaded = 1;
    ds_mutex_unlock(ach_lock);
}

static void achievements_write(unsigned int flags) {
    char path[320];
    FILE *f;
    data_file_path(path, sizeof(path), ACHIEVEMENTS_FILE);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "%u\n", flags);
        fclose(f);
    }
}

/* ── Чтение профиля скриптом ─────────────────────────────────────────────── */
#define LOADER(name, field) double name(void) { double v; progress_read(); pg_lock(); v = (double)pg.p.field; pg_unlock(); return v; }
LOADER(net_load_cups, cups)
LOADER(net_load_candies, candies)
LOADER(net_load_class, cls)
LOADER(net_load_azum, azum)
LOADER(net_load_santa, santa)
LOADER(net_load_ebuc, ebuc)
LOADER(net_load_level, level)
LOADER(net_load_levels_unlocked, levels_unlocked)
LOADER(net_load_ordinary_level, ordinary_level)
LOADER(net_load_ordinary_levels_unlocked, ordinary_levels_unlocked)
LOADER(net_load_azum_level, azum_level)
LOADER(net_load_azum_levels_unlocked, azum_levels_unlocked)
LOADER(net_load_santa_level, santa_level)
LOADER(net_load_santa_levels_unlocked, santa_levels_unlocked)
LOADER(net_load_ebuc_level, ebuc_level)
LOADER(net_load_ebuc_levels_unlocked, ebuc_levels_unlocked)
LOADER(net_load_bp_level, bp_level)
LOADER(net_load_azum_skin, azum_skin)
LOADER(net_load_language, lang)
#undef LOADER

/* Определена в profile_cloud.inc (включается следом). */
static void profile_push_cloud(const Profile *p);

/* Сохранить профиль целиком: нормализовать, поставить ts и владельца,
 * подтянуть достижения, записать файл и отправить в облако. */
static void profile_save(Profile *p) {
    char nick[LOGIN_NICK_MAX + 1] = "";
    if (!p) return;
    lg_lock();
    if (lg.status == NET_LOGIN_OK && lg.session_nick[0])
        snprintf(nick, sizeof(nick), "%s", lg.session_nick);
    lg_unlock();
    profile_clamp(p);
    achievements_read();
    ds_mutex_lock(ach_lock); p->ach = ach_flags; ds_mutex_unlock(ach_lock);
    p->ts = wall_ms();
    if (nick[0]) snprintf(p->owner, sizeof(p->owner), "%s", nick);
    else {
        /* Сессии нет: оставляем прежнего владельца, чтобы этот прогресс не
         * слили с чужим аккаунтом после входа под другим ником. */
        pg_lock();
        if (pg.loaded && pg.p.owner[0]) snprintf(p->owner, sizeof(p->owner), "%s", pg.p.owner);
        pg_unlock();
    }
    pg_lock();
    pg.p = *p;
    pg.loaded = 1;
    pg_unlock();
    pending_cls = p->cls;
    pending_level = p->level;
    pending_skin = p->azum_skin;
    progress_write(p);
    profile_push_cloud(p);
}

void net_save_progress(double cups, double candies, double cls, double azum, double santa, double ebuc,
                       double level, double levels_unlocked) {
    Profile p;
    progress_read();
    pg_lock();
    p = pg.p;
    pg_unlock();
    p.cups = (int)cups; p.candies = (int)candies; p.cls = (int)cls;
    p.azum = azum ? 1 : 0; p.santa = santa ? 1 : 0; p.ebuc = ebuc ? 1 : 0;
    p.level = (int)level; p.levels_unlocked = (int)levels_unlocked;
    /* Уровни выбранного класса пишем в его ветку, остальные не трогаем. */
    if (p.cls == 0) { p.ordinary_level = p.level; p.ordinary_levels_unlocked = p.levels_unlocked; }
    else if (p.cls == 1) { p.azum_level = p.level; p.azum_levels_unlocked = p.levels_unlocked; }
    else if (p.cls == 2) { p.santa_level = p.level; p.santa_levels_unlocked = p.levels_unlocked; }
    else if (p.cls == 3) { p.ebuc_level = p.level; p.ebuc_levels_unlocked = p.levels_unlocked; }
    profile_save(&p);
}

void net_save_progress_all(double cups, double candies, double cls, double azum, double santa, double ebuc,
                           double level, double levels_unlocked,
                           double ordinary_level, double ordinary_levels_unlocked,
                           double azum_level, double azum_levels_unlocked,
                           double santa_level, double santa_levels_unlocked,
                           double ebuc_level, double ebuc_levels_unlocked,
                           double bp_level, double azum_skin) {
    Profile p;
    /* Начинаем с того, что уже в памяти: язык и владелец профиля в аргументах
     * не приходят, а терять их при каждом сохранении прогресса нельзя. */
    progress_read();
    pg_lock();
    p = pg.p;
    pg_unlock();
    p.cups = (int)cups; p.candies = (int)candies; p.cls = (int)cls;
    p.azum = azum ? 1 : 0; p.santa = santa ? 1 : 0; p.ebuc = ebuc ? 1 : 0;
    p.level = (int)level; p.levels_unlocked = (int)levels_unlocked;
    p.ordinary_level = (int)ordinary_level; p.ordinary_levels_unlocked = (int)ordinary_levels_unlocked;
    p.azum_level = (int)azum_level; p.azum_levels_unlocked = (int)azum_levels_unlocked;
    p.santa_level = (int)santa_level; p.santa_levels_unlocked = (int)santa_levels_unlocked;
    p.ebuc_level = (int)ebuc_level; p.ebuc_levels_unlocked = (int)ebuc_levels_unlocked;
    p.bp_level = (int)bp_level; p.azum_skin = (int)azum_skin;
    profile_save(&p);
}

/* Язык интерфейса: хранится вместе с профилем (файл + облако), поэтому выбор
 * «Русский» переживает перезапуск игры и вход с другого устройства. */
void net_save_language(double lang) {
    Profile p;
    progress_read();
    pg_lock();
    p = pg.p;
    pg_unlock();
    p.lang = ((int)lang == 1) ? 1 : 0;
    profile_save(&p);
}
