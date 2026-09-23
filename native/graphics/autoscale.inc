/* Internal render scale of the frame (no button, no setting, no saved value:
 * the scale only follows the measured frame time) plus a rare log summary. Part
 * of the Vulkan backend: included by graphics.c after vulkan_backend.inc, so the
 * state is shared and the summary can print the present mode and the number of
 * swapchain rebuilds.
 *
 * Vsync (FIFO) quantises the rate: a frame only slightly longer than 16.6 ms
 * lands in 30 instead of 60. On weak GPUs the full window size does not fit
 * into that budget in battle, so a run of missed frames moves the rendering
 * into an offscreen scale times smaller, stretched back onto the window; when
 * there is headroom again the scale returns to 1:1. The image stays sharp while
 * the device holds 60 fps and gets softer only instead of dropping to 30.
 *
 * The hysteresis is deliberately asymmetric so the scale does not breathe: it
 * gets coarser fast (4 misses in a row or 4 of 24 frames) so a weak phone does
 * not stay at 20-35 fps for seconds, while a sharper probe is allowed only
 * after 300 clean frames and not more often than every 15 s. Two failed probes
 * pin the current scale for the session: steady softness beats visible
 * sharpness swings and a recreated offscreen every few seconds. History is
 * dropped for every new window, since the first frames after returning to the
 * app are always slow. */
static int gfx_auto_scale = 1;
static int gfx_cooldown = 0;        /* frames before any decision after a change */
static int gfx_finer_cooldown = 0;  /* frames before the next sharper probe */
static int gfx_probe_finer = 0;     /* sharper probe running: a miss rolls back */
static int gfx_probe_left = 0;      /* frames left in the probe verdict */
static int gfx_probe_fails = 0;     /* failed probes this session */
static int gfx_finer_off = 0;       /* probes off after two failures */
static int gfx_clean = 0;           /* frames in a row without a miss */
static int gfx_miss_streak = 0, gfx_win_frames = 0, gfx_win_misses = 0;

int ds_graphics_pixel_scale(void) { return gfx_auto_scale; }

/* New window (start, return to the app, resize): the old timings are stale. */
static void gfx_autoscale_new_window(void) {
    /* Judge performance early: on a weak GPU a second of full size frames feels
     * like a frozen interface. */
    gfx_cooldown = 15;
    gfx_probe_finer = 0;
    gfx_probe_left = 0;
    gfx_miss_streak = 0; gfx_win_frames = 0; gfx_win_misses = 0; gfx_clean = 0;
}

static void gfx_set_scale(int s, int cooldown) {
    if (s < 1) s = 1;
    /* On weak 1080p devices 1/4 can still miss the 30 fps step, so 1/5 and 1/6
     * are allowed: the image gets softer, the controls stay responsive. */
    if (s > 6) s = 6;
    if (s == gfx_auto_scale) return;
    gfx_auto_scale = s;
    gfx_cooldown = cooldown;
    gfx_miss_streak = 0; gfx_win_frames = 0; gfx_win_misses = 0;
    ds_log("graphics: internal resolution 1/%d (auto)", s);
}

/* Log summary every 5 seconds: the only way to see on the phone what the frame
 * rhythm does - vsync steps, present mode, swapchain rebuilds, chosen scale.
 * Without it a frame rate complaint cannot be told apart from a GPU limit. */
static double gfx_report_acc;
static int gfx_report_frames;
#define GFX_REPORT_SEC 5.0

/* Frame interval of the main loop in seconds, reported after every frame. */
void ds_graphics_report_frame_interval(double sec) {
    if (!(sec > 0.0) || sec > 0.25) return;   /* pause, background, debugger */
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
    /* Vsync miss at 60 Hz: the frame came at the second interval (33 ms) or
     * later. 20 ms leaves room for jitter around 16.7. */
    int missed = sec > 0.020;
    gfx_clean = missed ? 0 : gfx_clean + 1;
    if (gfx_cooldown > 0) { gfx_cooldown--; return; }
    /* Verdict of the sharper probe: the frames after its cooldown. A single
     * late miss a minute later must not roll back a scale that settled. */
    if (gfx_probe_finer && gfx_probe_left > 0) {
        gfx_probe_left--;
        if (gfx_probe_left == 0) gfx_probe_finer = 0;  /* probe held up */
    }
    if (missed) {
        if (gfx_probe_finer) {   /* sharper probe failed: roll back for good */
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
            /* A recreated offscreen waits for the GPU and hitches by itself, so
             * two steps are skipped at once (1-3-5-6): at most three hitches
             * instead of five while the scale settles. */
            if (gfx_auto_scale < 6) gfx_set_scale(gfx_auto_scale + 2, 12);
        } else if (gfx_win_misses == 0 && gfx_clean >= 300 && gfx_auto_scale > 1 &&
                   gfx_finer_cooldown == 0 && !gfx_finer_off) {
            gfx_probe_finer = 1;
            gfx_finer_cooldown = 900;
            gfx_set_scale(gfx_auto_scale - 1, 45);
            gfx_probe_left = 45 + 60;  /* probe cooldown plus verdict window */
        }
        gfx_win_frames = 0; gfx_win_misses = 0;
    }
}
