/* The window side of the renderer: losing the window when the app goes to the
 * background, getting a new one on return, and repairing a lost surface or
 * device. Part of the Vulkan backend, included by vulkan_backend.inc and
 * compiled into one translation unit (graphics.c), so the state is shared.
 *
 * Android takes the window away every time the game is minimised
 * (APP_CMD_TERM_WINDOW) and gives a new one on return (APP_CMD_INIT_WINDOW).
 * The renderer used to be torn down completely on the way out - instance,
 * device, pipelines, every texture - and built again on the way in. On a
 * TECNO Spark Go 1 (Android 14 Go, Mali) the game crashed right after coming
 * back. Only the surface and the swapchain belong to a window, so only they are
 * dropped now; the device keeps running and the next window gets a new surface
 * on it, which is also how the Android Vulkan samples handle it. */

/* New "image acquired" semaphores for both frames in flight. The swapchain
 * that could still signal the old ones is already gone and the queue is idle,
 * so nothing refers to them any more. */
static int ds_vk_reset_acquire_semaphores(void) {
    if (!vk_device) return 0;
    for (int i = 0; i < DS_VK_FRAMES_IN_FLIGHT; i++) {
        DsVkFrame *f = &vk_frames[i];
        if (f->image_available == VK_NULL_HANDLE) continue; /* frames not built yet */
        vkDestroySemaphore(vk_device, f->image_available, NULL);
        f->image_available = VK_NULL_HANDLE;
        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (!ds_vk_check_result(vkCreateSemaphore(vk_device, &si, NULL, &f->image_available),
                                "vkCreateSemaphore(acquire reset)")) {
            f->image_available = VK_NULL_HANDLE;
            vk_device_lost = 1; /* the frame cannot run without it: rebuild everything */
            return 0;
        }
    }
    return 1;
}

/* The window is going away (or already went): wait for the GPU, then drop the
 * swapchain and the surface while the ANativeWindow is still valid - the glue
 * holds the UI thread in onSurfaceDestroyed until this returns. */
static void ds_vk_surface_lost(void) {
    if (!vk_device) { vk_window = NULL; vk_surface_lost = 0; return; }
    vkDeviceWaitIdle(vk_device);
    ds_vk_release_frame_rate();
    ds_vk_destroy_swapchain();
    (void)ds_vk_reset_acquire_semaphores();
    for (int i = 0; i < DS_VK_FRAMES_IN_FLIGHT; i++) vk_frames[i].submitted = 0; /* idle: all fences signalled */
    if (vk_surface != VK_NULL_HANDLE && vk_instance != VK_NULL_HANDLE)
        vkDestroySurfaceKHR(vk_instance, vk_surface, NULL);
    vk_surface = VK_NULL_HANDLE;
    vk_window = NULL;
    vk_begun = 0;
    vk_image_held = 0;
    vk_need_recreate = 0;
    vk_sw_stale = 0;
    vk_sw_check_timer = 0;
    vk_surface_lost = 0;
    vk_pace_next_ns = 0;
}

/* A surface for a new window on the device that is already running. Returns 0
 * when the device cannot present to it; the caller then rebuilds from zero. */
static int ds_vk_attach_window(ANativeWindow *window) {
    if (!window || !vk_device || !vk_instance || !vk_phys) return 0;
    VkAndroidSurfaceCreateInfoKHR sci = { VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR };
    sci.window = window;
    if (!ds_vk_check_result(vkCreateAndroidSurfaceKHR(vk_instance, &sci, NULL, &vk_surface),
                            "vkCreateAndroidSurfaceKHR(reattach)")) {
        vk_surface = VK_NULL_HANDLE;
        return 0;
    }
    VkBool32 present = VK_FALSE;
    if (vkGetPhysicalDeviceSurfaceSupportKHR(vk_phys, vk_queue_family, vk_surface, &present) != VK_SUCCESS ||
        !present) {
        vkDestroySurfaceKHR(vk_instance, vk_surface, NULL);
        vk_surface = VK_NULL_HANDLE;
        return 0;
    }
    vk_window = window;
    ds_vk_request_frame_rate(window);
    /* The swapchain is built by the first frame (vk_swap is NULL). */
    vk_need_recreate = 1;
    vk_begun = 0;
    vk_image_held = 0;
    vk_pace_next_ns = 0;
    vk_reattaches++;
    ds_log("vulkan: window #%u attached to the kept device", vk_reattaches);
    return 1;
}

/* Called at the start of a frame when acquire or present reported a lost
 * surface or device, or when an earlier rebuild failed. Textures keep their CPU
 * pixels through a device rebuild and go up again on their next tex() call;
 * the font atlas is queued again by ds_vk_process_pending_uploads. */
static int ds_vk_recover(void) {
    ANativeWindow *window = vk_window ? vk_window : vk_rebuild_window;
    if (vk_device_lost || !vk_ready) {
        vk_device_lost = 0;
        vk_surface_lost = 0;
        if (!window) return 0;
        /* A rebuild that keeps failing is retried about twice a second, not
         * on every frame, so the log and the driver are not hammered. */
        if (vk_rebuild_window && (vk_rebuild_tries++ % 30) != 0) return 0;
        ds_log_err("vulkan: rebuilding the renderer (device lost or earlier failure)");
        ds_vk_shutdown_backend();
        if (!ds_vk_init_backend(window)) { vk_rebuild_window = window; return 0; }
        vk_rebuild_window = NULL;
        vk_rebuild_tries = 0;
        return 1;
    }
    if (vk_surface_lost) {
        ds_vk_surface_lost();
        if (window && ds_vk_attach_window(window)) return 1;
        vk_device_lost = 1; /* no surface on the kept device: full rebuild next frame */
        vk_rebuild_window = window;
        return 0;
    }
    return 1;
}
