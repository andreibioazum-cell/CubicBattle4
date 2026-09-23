/* Swapchain (создание/пересоздание/уничтожение) и кадры в полёте.
 * Часть Vulkan-бэкенда: включается из vulkan_backend.inc, всё собирается
 * одним translation unit'ом (graphics.c), поэтому состояние общее. */

/* Контроллер автомасштаба живёт в lifecycle.inc (подключается позже); новое
 * окно = устаревшая история кадров, решения на ней принимать нельзя. */
static void gfx_autoscale_new_window(void);

/* --- swapchain --- */

static const char *ds_vk_present_mode_name(VkPresentModeKHR mode) {
    switch (mode) {
        case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
        case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
        case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
        default: return "unknown";
    }
}

/* Режим презентации выбирается один раз на swapchain, но доступный список
 * печатается всегда: без этой строки в логе невозможно понять, откуда взялись
 * ступеньки 30-45 fps (ступеньки бывают только у FIFO). */
static VkPresentModeKHR ds_vk_choose_present_mode(char *listed, size_t listed_cap) {
    /* Порядок предпочтений:
     *   MAILBOX      - композитор забирает самый свежий готовый кадр, ступеньки
     *                  «промах = целый лишний интервал» нет вовсе;
     *   FIFO_RELAXED - вертикальная синхронизация без штрафного интервала: если
     *                  кадр опоздал, он показывается сразу. Это страховка для
     *                  драйверов, где MAILBOX не поддержан (там игра попадала
     *                  в жёсткие ступеньки 60/40/30 fps на панели 120 Гц);
     *   FIFO         - последний вариант: ступеньки неизбежны, но картинка целая. */
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    int have_mailbox = 0, have_relaxed = 0;
    if (listed && listed_cap) listed[0] = '\0';
    uint32_t npm = 0;
    if (vkGetPhysicalDeviceSurfacePresentModesKHR(vk_phys, vk_surface, &npm, NULL) == VK_SUCCESS && npm > 0) {
        VkPresentModeKHR *pm = (VkPresentModeKHR *)malloc(npm * sizeof(*pm));
        if (pm && vkGetPhysicalDeviceSurfacePresentModesKHR(vk_phys, vk_surface, &npm, pm) == VK_SUCCESS) {
            for (uint32_t i = 0; i < npm; i++) {
                if (pm[i] == VK_PRESENT_MODE_MAILBOX_KHR) have_mailbox = 1;
                if (pm[i] == VK_PRESENT_MODE_FIFO_RELAXED_KHR) have_relaxed = 1;
                if (listed && listed_cap > 1) {
                    size_t used = strlen(listed);
                    if (used && used + 1 < listed_cap) {
                        listed[used++] = ' ';
                        listed[used] = '\0';
                    }
                    const char *name = ds_vk_present_mode_name(pm[i]);
                    if (used + strlen(name) < listed_cap) {
                        strcpy(listed + used, name);
                    }
                }
            }
        }
        free(pm);
    }
    if (have_mailbox) mode = VK_PRESENT_MODE_MAILBOX_KHR;
    else if (have_relaxed) mode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    return mode;
}

static void ds_vk_destroy_swapchain(void) {
    if (!vk_device) return;
    for (uint32_t i = 0; i < DS_VK_MAX_SW_IMAGES; i++) {
        if (vk_sw_views[i] != VK_NULL_HANDLE) vkDestroyImageView(vk_device, vk_sw_views[i], NULL);
        if (vk_sw_present[i] != VK_NULL_HANDLE) vkDestroySemaphore(vk_device, vk_sw_present[i], NULL);
        vk_sw_views[i] = VK_NULL_HANDLE; vk_sw_present[i] = VK_NULL_HANDLE; vk_sw_images[i] = VK_NULL_HANDLE;
    }
    if (vk_swap != VK_NULL_HANDLE) vkDestroySwapchainKHR(vk_device, vk_swap, NULL);
    vk_swap = VK_NULL_HANDLE;
    vk_sw_count = 0;
    vk_image_held = 0;
}

/* Параметры поверхности, которые требуют новой swapchain. Спрашивать их
 * обязательно перед пересборкой: сам по себе SUBOPTIMAL ничего не значит. */
typedef struct {
    VkExtent2D extent;
    VkFormat format;
    VkColorSpaceKHR colorspace;
    VkSurfaceTransformFlagBitsKHR transform;
    uint32_t images;
} DsVkSurfaceState;

static int ds_vk_query_surface(DsVkSurfaceState *out) {
    if (!out || !vk_surface || !vk_phys) return 0;
    memset(out, 0, sizeof(*out));
    VkSurfaceCapabilitiesKHR caps;
    if (!ds_vk_check_result(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk_phys, vk_surface, &caps),
                            "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) return 0;
    if (caps.currentExtent.width == UINT32_MAX || caps.currentExtent.width == 0 || caps.currentExtent.height == 0)
        return 0; /* окно схлопнулось - попробуем в следующем кадре */
    uint32_t want = caps.minImageCount + 1;
    if (want < 3) want = 3;
    if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;

    /* Формат: обязательно с поддержкой blit (нужен автоматическому
     * внутреннему масштабу); для R8G8B8A8/B8G8R8A8_UNORM это гарантия
     * спецификации. */
    uint32_t nf = 0;
    if (!ds_vk_check_result(vkGetPhysicalDeviceSurfaceFormatsKHR(vk_phys, vk_surface, &nf, NULL),
                            "vkGetPhysicalDeviceSurfaceFormatsKHR")) return 0;
    if (nf == 0) { ds_log_err("vulkan: surface has no formats"); return 0; }
    VkSurfaceFormatKHR *formats = (VkSurfaceFormatKHR *)malloc(nf * sizeof(*formats));
    if (!formats) return 0;
    if (!ds_vk_check_result(vkGetPhysicalDeviceSurfaceFormatsKHR(vk_phys, vk_surface, &nf, formats),
                            "vkGetPhysicalDeviceSurfaceFormatsKHR")) { free(formats); return 0; }
    VkSurfaceFormatKHR chosen = formats[0];
    for (uint32_t i = 0; i < nf; i++) {
        VkFormat f = formats[i].format;
        if (formats[i].colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) continue;
        if (f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_B8G8R8A8_UNORM) { chosen = formats[i]; break; }
    }
    free(formats);
    out->format = chosen.format == VK_FORMAT_UNDEFINED ? VK_FORMAT_R8G8B8A8_UNORM : chosen.format;
    out->colorspace = chosen.colorSpace;
    out->extent = caps.currentExtent;
    out->images = want;
    /* preTransform = IDENTITY — обязательно на Android. Изображения swapchain
     * живут в системе координат ОКНА (ландшафт, как видит пользователь), а
     * поворот окна относительно дисплея уже применяет системный композитор —
     * этот поворот и есть caps.currentTransform (ROTATE_90 в ландшафте).
     * Если задать preTransform = currentTransform (как в некоторых примерах),
     * движок презентации проворачивает изображение ЕЩЁ на 90°: в ландшафте
     * игра оказывается «вертикальной» и растянутой. */
    out->transform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
        ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
        : caps.currentTransform;
    return 1;
}

/* Что именно изменилось в поверхности (NULL - ничего). */
static const char *ds_vk_surface_diff(const DsVkSurfaceState *s) {
    if (vk_swap == VK_NULL_HANDLE) return "нет swapchain";
    if (s->extent.width != vk_sw_extent.width || s->extent.height != vk_sw_extent.height) return "размер окна";
    if (s->format != vk_sw_format) return "формат поверхности";
    if (s->colorspace != vk_sw_colorspace) return "цветовое пространство";
    if (s->transform != vk_sw_transform) return "поворот поверхности";
    if (s->images != vk_sw_want_images) return "число изображений";
    return NULL;
}

/* Нужно ли пересобирать swapchain. Драйвер сообщает SUBOPTIMAL_KHR и когда
 * поверхность реально не изменилась (типовой случай: preTransform=IDENTITY при
 * currentTransform=ROTATE_90), поэтому параметры поверхности сверяются, и не
 * чаще, чем раз в DS_VK_SW_CHECK_FRAMES кадров. Иначе swapchain пересобирался
 * бы каждый кадр: vkQueueWaitIdle, заново изображения/семафоры, сброс истории
 * автомасштаба - и частота кадров встаёт в ступеньки 30-45 fps, которые не
 * лечатся оптимизацией отрисовки. OUT_OF_DATE_KHR (vk_need_recreate) считается
 * без всех этих проверок: там поверхность действительно устарела. */
#define DS_VK_SW_CHECK_FRAMES 30

static int ds_vk_swapchain_stale(void) {
    if (vk_swap == VK_NULL_HANDLE) return 1;
    if (!vk_sw_stale) return 0;
    if (vk_sw_check_timer > 0) { vk_sw_check_timer--; return 0; }
    vk_sw_check_timer = DS_VK_SW_CHECK_FRAMES;
    DsVkSurfaceState now;
    if (!ds_vk_query_surface(&now)) return 0;
    const char *why = ds_vk_surface_diff(&now);
    if (!why) {
        /* Не чаще первой строки, потом раз в 300 проверок: лог не должен
         * превращаться в поток на каждом кадре. */
        if (vk_sw_ignored++ == 0 || (vk_sw_ignored % 300) == 0)
            ds_log("vulkan: SUBOPTIMAL ignored (%u), surface unchanged", vk_sw_ignored);
        return 0;
    }
    ds_log("vulkan: swapchain stale: %s", why);
    vk_sw_stale = 0;
    vk_sw_check_timer = 0;
    return 1;
}

static int ds_vk_recreate_swapchain(void) {
    if (!vk_surface || !vk_phys) return 0;
    DsVkSurfaceState s;
    if (!ds_vk_query_surface(&s)) return 0;
    if (vk_device) vkQueueWaitIdle(vk_queue);
    ds_vk_destroy_swapchain();

    vk_sw_format = s.format;
    vk_sw_colorspace = s.colorspace;
    vk_sw_transform = s.transform;
    vk_sw_want_images = s.images;

    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(vk_phys, vk_sw_format, &fp);
    vk_blit_ok = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
                 (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT);
    vk_blit_linear = vk_blit_ok &&
        (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    if (!vk_blit_ok)
        ds_log_err("vulkan: format %d lacks blit support, auto-scale degrades to copy", (int)vk_sw_format);

    char listed[128];
    vk_sw_present_mode = ds_vk_choose_present_mode(listed, sizeof(listed));

    VkSwapchainCreateInfoKHR si = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    si.surface = vk_surface;
    si.minImageCount = s.images;
    si.imageFormat = vk_sw_format;
    si.imageColorSpace = vk_sw_colorspace;
    si.imageExtent = s.extent;
    si.imageArrayLayers = 1;
    si.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    si.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    si.preTransform = s.transform;
    si.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    si.presentMode = vk_sw_present_mode;
    si.clipped = VK_TRUE;
    si.oldSwapchain = VK_NULL_HANDLE;
    if (!ds_vk_check_result(vkCreateSwapchainKHR(vk_device, &si, NULL, &vk_swap), "vkCreateSwapchainKHR")) return 0;
    vk_sw_count = DS_VK_MAX_SW_IMAGES;
    if (!ds_vk_check_result(vkGetSwapchainImagesKHR(vk_device, vk_swap, &vk_sw_count, vk_sw_images),
                            "vkGetSwapchainImagesKHR")) return 0;
    for (uint32_t i = 0; i < vk_sw_count; i++) {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = vk_sw_images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = vk_sw_format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
        if (!ds_vk_check_result(vkCreateImageView(vk_device, &vi, NULL, &vk_sw_views[i]), "vkCreateImageView(swapchain)")) return 0;
        VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (!ds_vk_check_result(vkCreateSemaphore(vk_device, &sci, NULL, &vk_sw_present[i]), "vkCreateSemaphore(present)")) return 0;
    }
    vk_sw_extent = s.extent;
    vk_need_recreate = 0;
    vk_sw_stale = 0;
    vk_sw_check_timer = 0;
    vk_image_held = 0;
    vk_sw_recreates++;
    /* Первое окно, возврат в приложение, поворот/ресайз: накопленные промахи
     * относятся к прошлому окну, а первые кадры нового всегда медленные —
     * контроллер автомасштаба начинает считать заново через секунду. */
    gfx_autoscale_new_window();
    ds_log("vulkan: swapchain #%u %ux%u, %u images, format %d, present %s (available: %s)",
           vk_sw_recreates, vk_sw_extent.width, vk_sw_extent.height, (unsigned)vk_sw_count,
           (int)vk_sw_format, ds_vk_present_mode_name(vk_sw_present_mode), listed);
    return 1;
}

/* --- кадры в полёте --- */

static void ds_vk_destroy_frame_resources(void) {
    for (int i = 0; i < DS_VK_FRAMES_IN_FLIGHT; i++) {
        DsVkFrame *f = &vk_frames[i];
        if (!vk_device) break;
        if (f->pool != VK_NULL_HANDLE) vkDestroyCommandPool(vk_device, f->pool, NULL);
        if (f->fence != VK_NULL_HANDLE) vkDestroyFence(vk_device, f->fence, NULL);
        if (f->image_available != VK_NULL_HANDLE) vkDestroySemaphore(vk_device, f->image_available, NULL);
        ds_vk_destroy_buffer(&f->vbuf, &f->vmem);
        ds_vk_destroy_buffer(&f->ibuf, &f->imem);
        f->cmd = VK_NULL_HANDLE; f->fence = VK_NULL_HANDLE; f->image_available = VK_NULL_HANDLE;
        f->vmap = NULL; f->imap = NULL; f->vcap = 0; f->icap = 0;
    }
    vk_frame_cur = 0;
}

static int ds_vk_create_frame_resources(void) {
    ds_vk_destroy_frame_resources();
    for (int i = 0; i < DS_VK_FRAMES_IN_FLIGHT; i++) {
        DsVkFrame *f = &vk_frames[i];
        VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = vk_queue_family;
        if (!ds_vk_check_result(vkCreateCommandPool(vk_device, &pi, NULL, &f->pool), "vkCreateCommandPool(frame)")) return 0;
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = f->pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (!ds_vk_check_result(vkAllocateCommandBuffers(vk_device, &ai, &f->cmd), "vkAllocateCommandBuffers")) return 0;
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT; /* первый проход не ждёт */
        if (!ds_vk_check_result(vkCreateFence(vk_device, &fi, NULL, &f->fence), "vkCreateFence(frame)")) return 0;
        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (!ds_vk_check_result(vkCreateSemaphore(vk_device, &si, NULL, &f->image_available), "vkCreateSemaphore(frame)")) return 0;
        /* Буферы геометрии растут по мере надобности; стартуем с разумного. */
        if (!ds_vk_create_buffer(256 * 1024, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                 &f->vbuf, &f->vmem, (void **)&f->vmap)) return 0;
        f->vcap = 256 * 1024 / sizeof(GeoVert);
        if (!ds_vk_create_buffer(384 * 1024, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                 &f->ibuf, &f->imem, (void **)&f->imap)) return 0;
        f->icap = 384 * 1024 / sizeof(uint16_t);
    }
    return 1;
}

static int ds_vk_frame_ensure_capacity(DsVkFrame *f, size_t verts, size_t idx) {
    if (verts > f->vcap) {
        size_t cap = f->vcap * 2;
        if (cap < verts) cap = verts;
        if (cap > SIZE_MAX / sizeof(GeoVert)) return 0;
        VkBuffer nb; VkDeviceMemory nm; GeoVert *map;
        if (!ds_vk_create_buffer((VkDeviceSize)cap * sizeof(GeoVert), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                 &nb, &nm, (void **)&map)) return 0;
        ds_vk_destroy_buffer(&f->vbuf, &f->vmem);
        f->vbuf = nb; f->vmem = nm; f->vmap = map; f->vcap = cap;
    }
    if (idx > f->icap) {
        size_t cap = f->icap * 2;
        if (cap < idx) cap = idx;
        if (cap > SIZE_MAX / sizeof(uint16_t)) return 0;
        VkBuffer nb; VkDeviceMemory nm; uint16_t *map;
        if (!ds_vk_create_buffer((VkDeviceSize)cap * sizeof(uint16_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                 &nb, &nm, (void **)&map)) return 0;
        ds_vk_destroy_buffer(&f->ibuf, &f->imem);
        f->ibuf = nb; f->imem = nm; f->imap = map; f->icap = cap;
    }
    return 1;
}
