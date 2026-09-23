/* Инстанс/устройство/swapchain/оффскрин: константы, всё состояние бэкенда и forward-объявления.
 * Часть Vulkan-бэкенда: включается из vulkan_backend.inc, всё собирается
 * одним translation unit'ом (graphics.c), поэтому состояние общее. */

#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <time.h>
#ifdef __ANDROID__
#include <dlfcn.h> /* ANativeWindow_setFrameRate появился после API 30: берём его динамически */
#endif
#include "native/graphics/shaders_spirv.inc"

#define DS_VK_MAX_SW_IMAGES 8
#define DS_VK_FRAMES_IN_FLIGHT 2
#define DS_VK_DESC_SETS_PER_POOL 96

/* Логирование результата Vulkan: 1 - успех, 0 - ошибка (с сообщением в лог). */
static int ds_vk_check_result(VkResult r, const char *what) {
    if (r == VK_SUCCESS) return 1;
    ds_log_err("vulkan: %s failed (%d)", what, (int)r);
    return 0;
}

/* --- состояние бэкенда --- */

static VkInstance vk_instance;
static VkSurfaceKHR vk_surface;
static ANativeWindow *vk_window;
static VkPhysicalDevice vk_phys;
static VkDevice vk_device;
static VkQueue vk_queue;
static uint32_t vk_queue_family;
static int vk_ready;
static int vk_need_recreate;
static int vk_blit_ok = 1;
/* Есть ли у формата swapchain линейная фильтрация для blit: растягивание
 * уменьшенного оффскрина с LINEAR выглядит мягко, а не «пикселями». */
static int vk_blit_linear = 0;

static VkSwapchainKHR vk_swap;
static uint32_t vk_sw_count;
static VkImage vk_sw_images[DS_VK_MAX_SW_IMAGES];
static VkImageView vk_sw_views[DS_VK_MAX_SW_IMAGES];
static VkSemaphore vk_sw_present[DS_VK_MAX_SW_IMAGES]; /* "кадр отрисован" */
static VkFormat vk_sw_format = VK_FORMAT_R8G8B8A8_UNORM;
static VkExtent2D vk_sw_extent;
static uint32_t vk_sw_index;
static int vk_image_held; /* кадр захвачен, но ещё не показан (cancel_frame) */

/* --- параметры поверхности, от которых зависит swapchain ---
 * Пересобирать swapchain можно ТОЛЬКО когда реально меняется что-то из этого
 * списка. VK_SUBOPTIMAL_KHR драйвер умеет отдавать «просто так» (типовой случай
 * на Android: preTransform=IDENTITY при currentTransform=ROTATE_90 у поверхности),
 * и если верить каждому такому ответу, swapchain пересобирается КАЖДЫЙ кадр:
 * vkQueueWaitIdle, заново все image view и семафоры, сброс истории автомасштаба.
 * Частота кадров от этого садится в ступеньки 30-45 fps, которых не видно ни в
 * одной оптимизации отрисовки - дело не в нагрузке, а в пересборке. */
static VkPresentModeKHR vk_sw_present_mode = VK_PRESENT_MODE_FIFO_KHR;
static VkSurfaceTransformFlagBitsKHR vk_sw_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
static VkColorSpaceKHR vk_sw_colorspace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
static uint32_t vk_sw_want_images;
static int vk_sw_stale;         /* драйвер сказал SUBOPTIMAL: надо проверить поверхность */
static int vk_sw_check_timer;   /* кадров до следующей проверки поверхности */
static unsigned vk_sw_recreates;/* сколько раз пересобирали swapchain (диагностика) */
static unsigned vk_sw_ignored;  /* SUBOPTIMAL без изменений поверхности (диагностика) */

/* --- кадровый ритм, который игра просит у системы ---
 * Панель телефона часто умеет 90/120 Гц, и система по умолчанию отдаёт игре
 * самый быстрый режим: на кадр остаётся 8.3 мс, полноразмерный кадр слабого GPU
 * в него не укладывается, композитор сдвигает его на следующий вертикальный
 * интервал - и счётчик показывает ровно ступеньки 60/40/30 fps (те самые
 * «30-45 fps в бою»). Игра посчитана на 60 fps, поэтому сообщаем это системе
 * через ANativeWindow_setFrameRate (API 30+): она подбирает режим панели с
 * интервалом 16.7 мс, и тот же кадр укладывается целиком.
 * Функции может не быть (Android < 11, урезанный libandroid) - тогда берём её
 * dlsym'ом и молча работаем как раньше. Значения enum'ов из android/native_window.h:
 * ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE = 1,
 * ANATIVEWINDOW_CHANGE_FRAME_RATE_ALWAYS = 1. */
#define DS_VK_TARGET_FPS 60
#define DS_VK_FRAME_RATE_FIXED_SOURCE 1
#define DS_VK_FRAME_RATE_ALWAYS 1
typedef int32_t (*DsVkSetFrameRateFn)(ANativeWindow *window, float frameRate, int8_t compatibility);
typedef int32_t (*DsVkSetFrameRateStrategyFn)(ANativeWindow *window, float frameRate, int8_t compatibility,
                                              int8_t changeFrameRateStrategy);
typedef void (*DsVkClearFrameRateFn)(ANativeWindow *window);
static DsVkSetFrameRateFn vk_set_frame_rate;
static DsVkSetFrameRateStrategyFn vk_set_frame_rate_strategy;
static DsVkClearFrameRateFn vk_clear_frame_rate;

static VkRenderPass vk_renderpass;
static VkFormat vk_rp_format; /* формат swapchain, под который собраны render pass и конвейеры */
static VkPipelineLayout vk_layout_solid;
static VkPipelineLayout vk_layout_image;
static VkPipelineLayout vk_layout_tint;
static VkPipeline vk_pipe_solid;
static VkPipeline vk_pipe_image;
static VkPipeline vk_pipe_tint;
/* roundrect в софтрендере рисуется fill_span'ом - заменой пикселей без
 * смешивания (альфа примитива пишется в кадр как есть). Отдельный конвейер
 * с отключенным смешиванием повторяет это поведение один в один. */
static VkPipeline vk_pipe_replace;
static VkDescriptorSetLayout vk_desc_layout;
static VkSampler vk_sampler;

/* Оффскрин = виртуальный экран; очищается load-op'ом каждый кадр. */
static VkImage vk_off_image;
static VkDeviceMemory vk_off_mem;
static VkImageView vk_off_view;
static VkFramebuffer vk_off_fb;
static VkFormat vk_off_format = VK_FORMAT_R8G8B8A8_UNORM;
static uint32_t vk_off_w, vk_off_h;
/* Логический размер кадра (физические пиксели окна): в этих координатах живут
 * скрипты, поэтому вершинный шейдер делит на них, а не на размер оффскрина —
 * апскейл уменьшает число пикселей растра, но не увеличивает интерфейс. */
static uint32_t vk_log_w, vk_log_h;

typedef struct {
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore image_available; /* "кадр захвачен" */
    VkBuffer vbuf, ibuf;
    VkDeviceMemory vmem, imem;
    GeoVert *vmap;
    uint16_t *imap;
    size_t vcap, icap; /* ёмкость в элементах */
    int submitted;     /* есть незадождаленный submit: фенс надо дождаться */
} DsVkFrame;
static DsVkFrame vk_frames[DS_VK_FRAMES_IN_FLIGHT];
static int vk_frame_cur;
/* Кадр начат (изображение захвачено), но ещё не показан: cancel_frame или
 * ошибка submit оставляют слот в этом состоянии - следующий begin обязан
 * переиспользовать ТОТ ЖЕ слот и то же изображение, иначе submit будет ждать
 * семафор, сигнал от которого уже потерян. */
static int vk_begun;

/* Пулы дескрипторов: по набору на текстуру и один на шрифтовой атлас. */
static VkDescriptorPool *vk_desc_pools;
static size_t vk_desc_pool_n, vk_desc_pool_cap, vk_desc_pool_used;

/* Разовые загрузки текстур (ленивые, редкие) — отдельный пул команд. */
static VkCommandPool vk_upload_pool;
static VkCommandBuffer vk_upload_cmd;
static VkFence vk_upload_fence;

/* GPU-ресурсы шрифтового атласа. */
static VkImage vk_font_image;
static VkDeviceMemory vk_font_memory;
static VkImageView vk_font_view;
static VkDescriptorSet vk_font_desc;

/* Отложенные загрузки: текстуры, созданные скриптом прямо во время кадра,
 * заливаются на GPU в начале end_frame. */
#define DS_VK_PENDING_CAP 64
static Texture *vk_pending_tex[DS_VK_PENDING_CAP];
static int vk_pending_n;
static int vk_pending_font;

/* Пачки треугольников одного конвейера/текстуры подряд — чтобы сотни квадов
 * текста не превращались в сотни draw call'ов. */
typedef struct { int pipeline; const void *desc; uint32_t first; uint32_t count; } GeoBatch;
static GeoBatch *vk_batches;
static size_t vk_batch_n, vk_batch_cap;

static int ds_vk_create_frame_resources(void);
static void ds_vk_destroy_frame_resources(void);
static int ds_vk_recreate_swapchain(void);
static void ds_vk_destroy_swapchain(void);
static int ds_vk_swapchain_stale(void);
static const char *ds_vk_present_mode_name(VkPresentModeKHR mode);
static void ds_vk_lookup_frame_rate_api(void);
static int ds_vk_request_frame_rate(ANativeWindow *window);
static void ds_vk_release_frame_rate(void);
static int ds_vk_ensure_offscreen(uint32_t w, uint32_t h, VkFormat format);
static void ds_vk_destroy_offscreen(void);
