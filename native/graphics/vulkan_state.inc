/* Instance, device, swapchain and offscreen: constants, all backend state and
 * forward declarations. Part of the Vulkan backend, included by
 * vulkan_backend.inc and compiled into one translation unit (graphics.c), so the
 * state is shared between the parts. */

#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <time.h>
#ifdef __ANDROID__
#include <dlfcn.h> /* ANativeWindow_setFrameRate needs API 30, so it is resolved at runtime */
#endif
#include "native/graphics/shaders_spirv.inc"

#define DS_VK_MAX_SW_IMAGES 8
#define DS_VK_FRAMES_IN_FLIGHT 2
#define DS_VK_DESC_SETS_PER_POOL 96

/* Logs a Vulkan result: 1 on success, 0 on error (with a message in the log). */
/* The first thing that failed in the current start attempt and the GPU it ran
 * on. When Vulkan cannot start at all, fallback_screen.inc shows these on the
 * window so a player can send a screenshot instead of a black screen. */
static char vk_fail_step[96];
static int vk_fail_code;
static char vk_diag_gpu[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
static uint32_t vk_diag_api, vk_diag_driver, vk_diag_vendor;

static void ds_vk_note_failure(const char *what, int code) {
    if (vk_fail_step[0]) return; /* the first failure is the cause, the rest follow from it */
    snprintf(vk_fail_step, sizeof(vk_fail_step), "%s", what ? what : "?");
    vk_fail_code = code;
}

static int ds_vk_check_result(VkResult r, const char *what) {
    if (r == VK_SUCCESS) return 1;
    ds_log_err("vulkan: %s failed (%d)", what, (int)r);
    ds_vk_note_failure(what, (int)r);
    return 0;
}

/* --- backend state --- */

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
/* Whether the swapchain format can be blitted with linear filtering: that is
 * what makes the stretched offscreen look soft instead of blocky. */
static int vk_blit_linear = 0;

static VkSwapchainKHR vk_swap;
static uint32_t vk_sw_count;
static VkImage vk_sw_images[DS_VK_MAX_SW_IMAGES];
static VkImageView vk_sw_views[DS_VK_MAX_SW_IMAGES];
static VkSemaphore vk_sw_present[DS_VK_MAX_SW_IMAGES]; /* "frame is drawn" */
static VkFormat vk_sw_format = VK_FORMAT_R8G8B8A8_UNORM;
static VkExtent2D vk_sw_extent;
static uint32_t vk_sw_index;
static int vk_image_held; /* image acquired but not presented (cancel_frame) */

/* Surface parameters the swapchain is built from. It may only be rebuilt when
 * one of them really changed: Android drivers also report VK_SUBOPTIMAL_KHR
 * when nothing happened (preTransform IDENTITY against a ROTATE_90 surface is
 * the usual case), and trusting every such answer rebuilds the swapchain each
 * frame: vkQueueWaitIdle, all image views and semaphores again, autoscale
 * history dropped. That alone costs the 30-45 fps steps, so no draw call
 * optimisation can show any effect. */
static VkPresentModeKHR vk_sw_present_mode = VK_PRESENT_MODE_FIFO_KHR;
static VkSurfaceTransformFlagBitsKHR vk_sw_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
static VkColorSpaceKHR vk_sw_colorspace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
static uint32_t vk_sw_want_images;
static int vk_sw_stale;         /* driver said SUBOPTIMAL: check the surface */
static int vk_sw_check_timer;   /* frames until the surface is checked again */
static unsigned vk_sw_recreates;/* swapchain rebuilds, for diagnostics */
static unsigned vk_sw_ignored;  /* SUBOPTIMAL with an unchanged surface, for diagnostics */

/* Frame rate the game runs at. The panel is often 90 or 120 Hz, and the system
 * hands out its fastest mode by default: a full size frame of a weak GPU does
 * not fit into 8.3 ms, the compositor moves it to the next vsync slot, and the
 * counter shows the 60/40/30 steps players report as "30-45 fps". The game is
 * built for 60 fps, so it asks for that through ANativeWindow_setFrameRate
 * (API 30+), which picks a panel mode with a 16.7 ms interval.
 * The call may be missing (Android < 11, trimmed libandroid): it is resolved
 * with dlsym, and without it the game keeps working as before. Enum values from
 * android/native_window.h: FRAME_RATE_COMPATIBILITY_FIXED_SOURCE = 1,
 * CHANGE_FRAME_RATE_ALWAYS = 1. */
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
/* Deadline of the next frame, filled by ds_vk_pace_frame. */
static uint64_t vk_pace_next_ns;
static int vk_pace_on = 1; /* the host test turns pacing off to run frames fast */

static VkRenderPass vk_renderpass;
static VkFormat vk_rp_format; /* swapchain format the render pass and pipelines were built for */
static VkPipelineLayout vk_layout_solid;
static VkPipelineLayout vk_layout_image;
static VkPipelineLayout vk_layout_tint;
static VkPipeline vk_pipe_solid;
static VkPipeline vk_pipe_image;
static VkPipeline vk_pipe_tint;
/* The software renderer drew roundrect with fill_span, which replaced pixels
 * without blending (the alpha of the primitive went into the frame as is). A
 * separate pipeline with blending disabled copies that behaviour exactly. */
static VkPipeline vk_pipe_replace;
static VkDescriptorSetLayout vk_desc_layout;
static VkSampler vk_sampler;

/* The offscreen is the virtual screen and is cleared by a load op every frame. */
static VkImage vk_off_image;
static VkDeviceMemory vk_off_mem;
static VkImageView vk_off_view;
static VkFramebuffer vk_off_fb;
static VkFormat vk_off_format = VK_FORMAT_R8G8B8A8_UNORM;
static uint32_t vk_off_w, vk_off_h;
/* Logical frame size in window pixels: the scripts live in those coordinates,
 * so the vertex shader divides by them and not by the offscreen size. The
 * internal scale shrinks the raster but never scales the interface up. */
static uint32_t vk_log_w, vk_log_h;

typedef struct {
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore image_available; /* "image acquired" */
    VkBuffer vbuf, ibuf;
    VkDeviceMemory vmem, imem;
    GeoVert *vmap;
    uint16_t *imap;
    size_t vcap, icap; /* capacity in elements */
    int submitted;     /* an unwaited submit: the fence has to be waited for */
} DsVkFrame;
static DsVkFrame vk_frames[DS_VK_FRAMES_IN_FLIGHT];
static int vk_frame_cur;
/* A frame was started (an image was acquired) but not presented yet:
 * cancel_frame or a failed submit leaves the slot in that state, and the next
 * begin must reuse the same slot and the same image, otherwise the submit would
 * wait for a semaphore whose signal is already lost. */
static int vk_begun;

/* Descriptor pools: one set per texture and one for the font atlas. */
static VkDescriptorPool *vk_desc_pools;
static size_t vk_desc_pool_n, vk_desc_pool_cap, vk_desc_pool_used;

/* One-off texture uploads (lazy and rare) get their own command pool. */
static VkCommandPool vk_upload_pool;
static VkCommandBuffer vk_upload_cmd;
static VkFence vk_upload_fence;

/* GPU resources of the font atlas. */
static VkImage vk_font_image;
static VkDeviceMemory vk_font_memory;
static VkImageView vk_font_view;
static VkDescriptorSet vk_font_desc;

/* Deferred uploads: textures created by a script during a frame are copied to
 * the GPU at the start of end_frame. */
#define DS_VK_PENDING_CAP 64
static Texture *vk_pending_tex[DS_VK_PENDING_CAP];
static int vk_pending_n;
static int vk_pending_font;

/* Runs of triangles sharing a pipeline and texture, so that hundreds of text
 * quads do not become hundreds of draw calls. */
typedef struct { int pipeline; const void *desc; uint32_t first; uint32_t count; } GeoBatch;
static GeoBatch *vk_batches;
static size_t vk_batch_n, vk_batch_cap;

/* Going to the background (vulkan_window.inc). Only the window half of the
 * renderer - surface and swapchain - dies with the window; instance, device,
 * pipelines and textures stay, and the next window gets a new surface on the
 * same device. vk_surface_lost / vk_device_lost are set by acquire or present
 * and handled at the start of the next frame; vk_rebuild_window remembers the
 * window when a rebuild failed, so it is retried instead of a black screen. */
static int vk_surface_lost;
static int vk_device_lost;
static ANativeWindow *vk_rebuild_window;
static unsigned vk_rebuild_tries;
static unsigned vk_reattaches; /* windows served by a kept device, for diagnostics */
/* Image acquire never waits forever: with UINT64_MAX a compositor that stops
 * handing out buffers while the app goes to the background parks the game
 * thread inside vkAcquireNextImageKHR, APP_CMD_TERM_WINDOW is never handled,
 * the UI thread hangs in onSurfaceDestroyed and the system kills the app. */
#define DS_VK_ACQUIRE_TIMEOUT_NS 250000000ull
static int ds_vk_attach_window(ANativeWindow *window);
static void ds_vk_surface_lost(void);
static int ds_vk_recover(void);
static int ds_vk_reset_acquire_semaphores(void);

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
