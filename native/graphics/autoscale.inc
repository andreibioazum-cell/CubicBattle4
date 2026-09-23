/* Автоматическое внутреннее разрешение (внутренний масштаб кадра) и редкая
 * сводка в лог. Часть Vulkan-бэкенда: подключается из graphics.c после
 * vulkan_backend.inc и собирается с ним в один translation unit, поэтому
 * состояние общее — сводка печатает выбранный режим презентации и число
 * пересборок swapchain.
 *
 * Ни кнопки, ни настройки, ни сохранённого значения у масштаба нет: только
 * реакция на фактическую скорость кадров. Vsync (FIFO) квантует частоту:
 * стоит кадру стать чуть дольше 16.6 мс, и вместо 60 будет 30. На слабых GPU
 * в бою (много слоёв смешивания на весь экран) полный размер окна в этот бюджет
 * не укладывается, поэтому при накоплении промахов кадр рисуется
 * в оффскрин в scale раз меньше и растягивается на окно; как только запас
 * появляется — возвращаемся к 1:1. Картинка всегда резкая, пока устройство
 * держит 60 fps; мягче она становится только вместо провала в 30.
 *
 * Гистерезис здесь принципиально несимметричный, чтобы масштаб не «дышал»:
 * грубее уходим быстро (4 промаха подряд или 4 из 24 кадров), чтобы слабый
 * телефон не оставался на 20-35 fps несколько секунд, а пробу «резче»
 * допускаем только после 300 кадров (~5 с) вообще без промахов и не
 * чаще раза в 15 с; две провалившиеся пробы за сеанс навсегда фиксируют
 * текущий масштаб (gfx_finer_off) — лучше стабильная мягкость, чем видимая
 * пульсация резкости и пересоздание оффскрина каждые несколько секунд.
 * История промахов сбрасывается на каждом новом окне (gfx_autoscale_new_window):
 * первые кадры после возврата в приложение всегда медленные и не должны
 * сразу тянуть масштаб вниз. */
static int gfx_auto_scale = 1;
static int gfx_cooldown = 0;        /* кадров до любых решений после смены */
static int gfx_finer_cooldown = 0;  /* кадров до следующей пробы «резче» */
static int gfx_probe_finer = 0;     /* идёт проба «резче»: промах откатит */
static int gfx_probe_left = 0;      /* кадров вердикта пробы; дожил до 0 - успех */
static int gfx_probe_fails = 0;     /* сколько проб провалилось за сеанс */
static int gfx_finer_off = 0;       /* после двух провалов пробы выключены */
static int gfx_clean = 0;           /* кадров подряд без промаха */
static int gfx_miss_streak = 0, gfx_win_frames = 0, gfx_win_misses = 0;

int ds_graphics_pixel_scale(void) { return gfx_auto_scale; }

/* Старт/возврат в приложение/смена размера окна: история скорости устарела. */
static void gfx_autoscale_new_window(void) {
    /* Первую оценку производительности делаем быстро: на слабом GPU секунда
     * кадров в полном разрешении визуально ощущается как зависание интерфейса. */
    gfx_cooldown = 15;
    gfx_probe_finer = 0;
    gfx_probe_left = 0;
    gfx_miss_streak = 0; gfx_win_frames = 0; gfx_win_misses = 0; gfx_clean = 0;
}

static void gfx_set_scale(int s, int cooldown) {
    if (s < 1) s = 1;
    /* На слабых 1080p-устройствах (включая Unisoc T615) 1/4 всё ещё может
     * упираться в 30 fps. Разрешаем аварийные 1/5 и 1/6: интерфейс станет
     * мягче, зато управление и анимации останутся отзывчивыми. */
    if (s > 6) s = 6;
    if (s == gfx_auto_scale) return;
    gfx_auto_scale = s;
    gfx_cooldown = cooldown;
    gfx_miss_streak = 0; gfx_win_frames = 0; gfx_win_misses = 0;
    ds_log("graphics: internal resolution 1/%d (auto)", s);
}

/* Редкая сводка в лог (раз в 5 с): единственный способ увидеть на телефоне,
 * что реально происходит с кадровым ритмом — ступеньки вертикальной
 * синхронизации, режим презентации, число пересборок swapchain и выбранный
 * автомасштаб. Без неё «в бою 30-45 fps» невозможно отличить от «уперлись в
 * GPU»: строки в логе видно в logcat и в отчёте о падении. */
static double gfx_report_acc;
static int gfx_report_frames;
#define GFX_REPORT_SEC 5.0

/* Кадровый интервал главного цикла (секунды): вызывается после каждого кадра. */
void ds_graphics_report_frame_interval(double sec) {
    if (!(sec > 0.0) || sec > 0.25) return;   /* пауза, сворачивание, отладчик */
    gfx_report_acc += sec;
    gfx_report_frames++;
    if (gfx_report_acc >= GFX_REPORT_SEC) {
        ds_log("graphics: %.1f fps, frame %.1f ms, internal 1/%d, present %s, swapchain %ux%u, recreates %u",
               (double)gfx_report_frames / gfx_report_acc,
               1000.0 * gfx_report_acc / (double)gfx_report_frames,
               gfx_auto_scale, ds_vk_present_mode_name(vk_sw_present_mode),
               (unsigned)vk_sw_extent.width, (unsigned)vk_sw_extent.height, vk_sw_recreates);
        gfx_report_acc = 0;
        gfx_report_frames = 0;
    }
    if (gfx_finer_cooldown > 0) gfx_finer_cooldown--;
    /* Промах по vsync 60 Гц: кадр пришёл ко второму вертикальному интервалу
     * (33 мс) или позже. 20 мс — граница с запасом против дрожания 16.7. */
    int missed = sec > 0.020;
    gfx_clean = missed ? 0 : gfx_clean + 1;
    if (gfx_cooldown > 0) { gfx_cooldown--; return; }
    /* Вердикт пробы «резче»: окно кадров после её кулдауна. Одиночный поздний
     * промах спустя минуту не должен откатывать уже прижившийся масштаб. */
    if (gfx_probe_finer && gfx_probe_left > 0) {
        gfx_probe_left--;
        if (gfx_probe_left == 0) gfx_probe_finer = 0;  /* проба прижилась */
    }
    if (missed) {
        if (gfx_probe_finer) {   /* проба «резче» провалилась — откат и надолго */
            gfx_probe_finer = 0;
            gfx_probe_left = 0;
            gfx_probe_fails++;
            if (gfx_probe_fails >= 2) gfx_finer_off = 1;
            gfx_finer_cooldown = gfx_finer_off ? 1000000000 : 1800;
            gfx_set_scale(gfx_auto_scale + 1, 90);
            return;
        }
        gfx_miss_streak++;
    } else {
        gfx_miss_streak = 0;
    }
    gfx_win_frames++;
    if (missed) gfx_win_misses++;
    if (gfx_miss_streak >= 4 || gfx_win_frames >= 24) {
        int bad = gfx_miss_streak >= 4 || gfx_win_misses >= 4;
        if (bad) {
            /* Пересоздание оффскрина ждёт GPU и само даёт заметный рывок.
             * Перепрыгиваем сразу две ступени (1→3→5→6), чтобы вместо пяти
             * подвисаний при подборе масштаба было максимум три. */
            if (gfx_auto_scale < 6) gfx_set_scale(gfx_auto_scale + 2, 12);
        } else if (gfx_win_misses == 0 && gfx_clean >= 300 && gfx_auto_scale > 1 &&
                   gfx_finer_cooldown == 0 && !gfx_finer_off) {
            gfx_probe_finer = 1;
            gfx_finer_cooldown = 900;
            gfx_set_scale(gfx_auto_scale - 1, 45);
            gfx_probe_left = 45 + 60;  /* кулдаун пробы плюс окно вердикта */
        }
        gfx_win_frames = 0; gfx_win_misses = 0;
    }
}
