/* The frame rhythm: asking the platform for 60 fps (ANativeWindow_setFrameRate)
 * and sleeping out the rest of the 60 fps budget after a fast frame. Part of the
 * Vulkan backend, included by vulkan_backend.inc before the frame loop and
 * compiled into one translation unit (graphics.c), so the state is shared. */

/* --- asking the system for a frame rhythm ---
 * See the DS_VK_TARGET_FPS comment in vulkan_state.inc for why 60 fps is asked
 * for. ANativeWindow_setFrameRate arrived in API 30 and is not always present
 * in libandroid, so it is resolved dynamically and simply skipped if missing. */
static void ds_vk_lookup_frame_rate_api(void) {
#ifdef __ANDROID__
    if (!vk_set_frame_rate)
        vk_set_frame_rate = (DsVkSetFrameRateFn)dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate");
    if (!vk_set_frame_rate_strategy)
        vk_set_frame_rate_strategy = (DsVkSetFrameRateStrategyFn)dlsym(RTLD_DEFAULT,
                                        "ANativeWindow_setFrameRateWithChangeStrategy");
    if (!vk_clear_frame_rate)
        vk_clear_frame_rate = (DsVkClearFrameRateFn)dlsym(RTLD_DEFAULT, "ANativeWindow_clearFrameRate");
#endif
}

static int ds_vk_request_frame_rate(ANativeWindow *window) {
    if (!window) return 0;
    ds_vk_lookup_frame_rate_api();
    /* ALWAYS instead of ONLY_IF_SEAMLESS: a 120 to 60 Hz switch usually has no
     * black frame, but a system that calls it not seamless would silently
     * ignore the seamless-only variant and leave the fps steps in place. */
    int32_t rc = -1;
    if (vk_set_frame_rate_strategy)
        rc = vk_set_frame_rate_strategy(window, (float)DS_VK_TARGET_FPS,
                                        (int8_t)DS_VK_FRAME_RATE_FIXED_SOURCE,
                                        (int8_t)DS_VK_FRAME_RATE_ALWAYS);
    if (rc != 0 && vk_set_frame_rate)
        rc = vk_set_frame_rate(window, (float)DS_VK_TARGET_FPS, (int8_t)DS_VK_FRAME_RATE_FIXED_SOURCE);
    if (rc == 0)
        ds_log("vulkan: asked the platform for %d fps (panel mode vsync interval 16.7 ms)", DS_VK_TARGET_FPS);
    else if (vk_set_frame_rate || vk_set_frame_rate_strategy)
        ds_log_err("vulkan: ANativeWindow_setFrameRate(%d) failed (%d)", DS_VK_TARGET_FPS, (int)rc);
    else
        ds_log("vulkan: ANativeWindow_setFrameRate unavailable (API < 30), panel mode left to the system");
    return rc == 0;
}

/* Drop the request when the window closes, otherwise the system would keep the
 * panel mode for a game that is gone. */
static void ds_vk_release_frame_rate(void) {
    if (vk_window && vk_clear_frame_rate) vk_clear_frame_rate(vk_window);
}

/* --- keeping the frame rate at DS_VK_TARGET_FPS ---
 * A present mode without vsync wait (MAILBOX) lets the loop run as fast as the
 * device allows: the frame counter showed 350+ fps, which wastes battery and
 * makes the speed of the game depend on the device. The remaining time of the
 * 60 fps budget is therefore slept out at the end of the frame. A frame that
 * took longer than the budget goes out at once. */
static void ds_vk_pace_frame(void) {
    if (!vk_pace_on) return;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return;
    uint64_t now_ns = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
    uint64_t period = 1000000000ull / DS_VK_TARGET_FPS;
    /* Resync after a long frame (loading, a hitch) instead of catching up. */
    if (vk_pace_next_ns == 0 || now_ns > vk_pace_next_ns + period) vk_pace_next_ns = now_ns;
    vk_pace_next_ns += period;
    if (vk_pace_next_ns <= now_ns) return;
    struct timespec left;
    left.tv_sec = (time_t)((vk_pace_next_ns - now_ns) / 1000000000ull);
    left.tv_nsec = (long)((vk_pace_next_ns - now_ns) % 1000000000ull);
    nanosleep(&left, NULL);
}
