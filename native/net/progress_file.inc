/* progress.dat — файл прогресса на устройстве.
 *
 * Вынесено из native/net/state_storage.inc: здесь только работа с файлом и
 * геттеры для скриптов (net_load_*), а облачная часть сохранения осталась там.
 * Формат позиционный, старые файлы дочитываются по числу прочитанных полей:
 *   кубки класс азум санти леденцы уровень открыто_уровней
 *   ordinary_level ordinary_levels azum_level azum_levels
 *   santa_level santa_levels ebuc_level ebuc_levels ebuc bp azum_skin
 * Запись атомарная (file_write_atomic из state_storage.inc).
 */
static void progress_write(int cups, int candies, int cls, int azum, int santa, int ebuc,
                           int level, int levels_unlocked,
                           int ordinary_level, int ordinary_levels_unlocked,
                           int azum_level, int azum_levels_unlocked,
                           int santa_level, int santa_levels_unlocked,
                           int ebuc_level, int ebuc_levels_unlocked,
                           int bp_level, int azum_skin) {
    char path[320], text[512];
    data_file_path(path, sizeof(path), PROGRESS_FILE);
    snprintf(text, sizeof(text), "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n",
             cups, cls, azum, santa, candies, level, levels_unlocked,
             ordinary_level, ordinary_levels_unlocked,
             azum_level, azum_levels_unlocked,
             santa_level, santa_levels_unlocked,
             ebuc_level, ebuc_levels_unlocked, ebuc,
             bp_level, azum_skin);
    file_write_atomic(path, text);
}
static void progress_read(void) {
    char path[320]; FILE *f;
    int cups=0, candies=0, cls=0, azum=0, santa=0, ebuc=0, level=0, levels_unlocked=0;
    int ordinary_level=-1, ordinary_levels_unlocked=-1;
    int azum_level=-1, azum_levels_unlocked=-1;
    int santa_level=-1, santa_levels_unlocked=-1;
    int ebuc_level=-1, ebuc_levels_unlocked=-1;
    int bp_level=0, azum_skin=0;
    pg_lock();
    if (pg.loaded) { pg_unlock(); return; }
    pg_unlock();
    data_file_path(path, sizeof(path), PROGRESS_FILE);
    f = fopen(path, "r");
    if (f) {
        int n = fscanf(f, "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
                       &cups, &cls, &azum, &santa, &candies,
                       &level, &levels_unlocked,
                       &ordinary_level, &ordinary_levels_unlocked,
                       &azum_level, &azum_levels_unlocked,
                       &santa_level, &santa_levels_unlocked,
                       &ebuc_level, &ebuc_levels_unlocked, &ebuc,
                       &bp_level, &azum_skin);
        if (n < 3) {
            cups=0; candies=0; cls=0; azum=0; santa=0; ebuc=0; level=0; levels_unlocked=0;
            ordinary_level=ordinary_levels_unlocked=0;
            azum_level=azum_levels_unlocked=0;
            santa_level=santa_levels_unlocked=0;
            ebuc_level=ebuc_levels_unlocked=0;
        } else {
            if (n < 6) { level=0; levels_unlocked=0; }
            else if (n < 7) {
                levels_unlocked=level;
            }
            if (n < 13) {
                ordinary_level=ordinary_levels_unlocked=0;
                azum_level=azum_levels_unlocked=0;
                santa_level=santa_levels_unlocked=0;
                ebuc_level=ebuc_levels_unlocked=0;
                if (cls==1 && azum) { azum_level=level; azum_levels_unlocked=levels_unlocked; }
                else if (cls==2 && santa) { santa_level=level; santa_levels_unlocked=levels_unlocked; }
                else { ordinary_level=level; ordinary_levels_unlocked=levels_unlocked; }
            }
            if (n < 16) {
                ebuc_level=ebuc_levels_unlocked=0;
                ebuc=0;
            }
            if (n < 17) { bp_level=0; azum_skin=0; }
            else if (n < 18) { azum_skin=0; }
        }
        fclose(f);
    }
    if (cups < 0) cups = 0;
    if (candies < 0) candies = 0;
    if (cls != 1 && cls != 2 && cls != 3) cls = 0;
    azum = azum ? 1 : 0;
    santa = santa ? 1 : 0;
    ebuc = ebuc ? 1 : 0;
    if (cls == 1 && !azum) cls = 0;
    if (cls == 2 && !santa) cls = 0;
    if (cls == 3 && !ebuc) cls = 0;
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
}
double net_load_cups(void) { double v; progress_read(); pg_lock(); v = (double)pg.cups; pg_unlock(); return v; }
double net_load_candies(void) { double v; progress_read(); pg_lock(); v = (double)pg.candies; pg_unlock(); return v; }
double net_load_class(void) { double v; progress_read(); pg_lock(); v = (double)pg.cls; pg_unlock(); return v; }
double net_load_azum(void) { double v; progress_read(); pg_lock(); v = (double)pg.azum; pg_unlock(); return v; }
double net_load_santa(void) { double v; progress_read(); pg_lock(); v = (double)pg.santa; pg_unlock(); return v; }
double net_load_ebuc(void) { double v; progress_read(); pg_lock(); v = (double)pg.ebuc; pg_unlock(); return v; }
double net_load_level(void) { double v; progress_read(); pg_lock(); v = (double)pg.level; pg_unlock(); return v; }
double net_load_levels_unlocked(void) { double v; progress_read(); pg_lock(); v = (double)pg.levels_unlocked; pg_unlock(); return v; }
double net_load_ordinary_level(void) { double v; progress_read(); pg_lock(); v = (double)pg.ordinary_level; pg_unlock(); return v; }
double net_load_ordinary_levels_unlocked(void) { double v; progress_read(); pg_lock(); v = (double)pg.ordinary_levels_unlocked; pg_unlock(); return v; }
double net_load_azum_level(void) { double v; progress_read(); pg_lock(); v = (double)pg.azum_level; pg_unlock(); return v; }
double net_load_azum_levels_unlocked(void) { double v; progress_read(); pg_lock(); v = (double)pg.azum_levels_unlocked; pg_unlock(); return v; }
double net_load_santa_level(void) { double v; progress_read(); pg_lock(); v = (double)pg.santa_level; pg_unlock(); return v; }
double net_load_santa_levels_unlocked(void) { double v; progress_read(); pg_lock(); v = (double)pg.santa_levels_unlocked; pg_unlock(); return v; }
double net_load_ebuc_level(void) { double v; progress_read(); pg_lock(); v = (double)pg.ebuc_level; pg_unlock(); return v; }
double net_load_ebuc_levels_unlocked(void) { double v; progress_read(); pg_lock(); v = (double)pg.ebuc_levels_unlocked; pg_unlock(); return v; }
double net_load_bp_level(void) { double v; progress_read(); pg_lock(); v = (double)pg.bp_level; pg_unlock(); return v; }
double net_load_azum_skin(void) { double v; progress_read(); pg_lock(); v = (double)pg.azum_skin; pg_unlock(); return v; }
