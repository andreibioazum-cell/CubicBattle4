/* The Vulkan rendering backend of Cubic Battle: instance, surface and device,
 * swapchain, offscreen target, pipelines with prebaked SPIR-V shaders, texture
 * uploads and the per-frame begin, end and cancel cycle.
 *
 * A frame is built like this: the script collects commands (lifecycle.inc), at the
 * end of the frame geometry.inc turns them into triangles, the backend draws them
 * into an offscreen RGBA8 target of window / scale, and the result is blitted
 * nearest into the swapchain image covering the whole window. Script coordinates
 * stay in physical window pixels, since the vertex shader gets the logical size,
 * which means the offscreen resolution shrinks the raster without scaling the
 * interface up.
 *
 * The implementation is split by responsibility, with a 450 line limit per file,
 * and the include order matters: state is declared before it is used. */
#include "native/graphics/vulkan_state.inc"
#include "native/graphics/vulkan_upload.inc"
#include "native/graphics/vulkan_pipelines.inc"
#include "native/graphics/vulkan_swapchain.inc"
#include "native/graphics/vulkan_record.inc"
#include "native/graphics/vulkan_frame_loop.inc"
