// Vulkan backend for the NVN layer (Windows; macOS uses runtime/nvn/metal.m + renderer.m).
// Files: device.c (instance, window, swapchain, present, screenshots, ImGui hooks), textures.c (images, uploads,
// views), exec.c (command execution: passes, draws, clears, copies, compute), shaders.c (GLSL -> SPIR-V, programs,
// pipelines). Mirrors the Metal backend's behaviour; differences are noted where they happen.
//
// Synchronisation model: every image lives in VK_IMAGE_LAYOUT_GENERAL, and a full memory barrier separates
// render passes, copies and dispatches (Metal tracks these hazards itself). Submissions are numbered; a timeline
// semaphore signals each number, which is what NVN fences (renderer_fence_seq / renderer_wait_seq) wait for.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#if __has_include(<vma/vk_mem_alloc.h>)
#include <vma/vk_mem_alloc.h>
#else
#include <vk_mem_alloc.h>  // (Debian / Ubuntu libvulkan-memory-allocator-dev)
#endif

// The Vulkan 1.2 / 1.3 commands come from the driver (device.c, after vkCreateDevice), not from the loader's exports:
// a vulkan-1.dll older than 1.3 doesn't have them, and Windows then refuses to start the program ("The procedure entry
// point vkCmdBeginRendering could not be located") before it can say what is missing.
#define VK_DEVICE_COMMANDS_12(X) X(vkGetSemaphoreCounterValue) X(vkWaitSemaphores)
// Vulkan 1.3 commands: core, or the same command from its KHR / EXT extension on a Vulkan 1.2 driver; NULL when the
// driver has neither, and compat.c does without (g_vkc)
#define VK_DEVICE_COMMANDS_13(X)                                                                                      \
    X(vkCmdBeginRendering, KHR) X(vkCmdEndRendering, KHR) X(vkCmdPipelineBarrier2, KHR) X(vkCmdWriteTimestamp2, KHR)  \
    X(vkQueueSubmit2, KHR) X(vkCmdSetCullMode, EXT) X(vkCmdSetDepthCompareOp, EXT) X(vkCmdSetDepthTestEnable, EXT)    \
    X(vkCmdSetDepthWriteEnable, EXT) X(vkCmdSetFrontFace, EXT) X(vkCmdSetPrimitiveTopology, EXT)                      \
    X(vkCmdSetStencilOp, EXT) X(vkCmdSetStencilTestEnable, EXT) X(vkCmdSetDepthBiasEnable, EXT)                       \
    X(vkCmdSetPrimitiveRestartEnable, EXT)
#define VK_DECLARE_COMMAND(f) extern PFN_##f vkp_##f;
#define VK_DECLARE_COMMAND_13(f, ext) extern PFN_##f vkp_##f;
VK_DEVICE_COMMANDS_12(VK_DECLARE_COMMAND)
VK_DEVICE_COMMANDS_13(VK_DECLARE_COMMAND_13)
#undef VK_DECLARE_COMMAND
#undef VK_DECLARE_COMMAND_13
#define vkGetSemaphoreCounterValue vkp_vkGetSemaphoreCounterValue
#define vkWaitSemaphores vkp_vkWaitSemaphores
#define vkCmdSetCullMode vkp_vkCmdSetCullMode
#define vkCmdSetDepthCompareOp vkp_vkCmdSetDepthCompareOp
#define vkCmdSetDepthTestEnable vkp_vkCmdSetDepthTestEnable
#define vkCmdSetDepthWriteEnable vkp_vkCmdSetDepthWriteEnable
#define vkCmdSetFrontFace vkp_vkCmdSetFrontFace
#define vkCmdSetPrimitiveTopology vkp_vkCmdSetPrimitiveTopology
#define vkCmdSetStencilOp vkp_vkCmdSetStencilOp
#define vkCmdSetStencilTestEnable vkp_vkCmdSetStencilTestEnable

// ---- Vulkan 1.2 GPUs (compat.c): what the driver lacks of Vulkan 1.3 is done another way. LARECOMP_VK_COMPAT=1 takes
// those ways on a Vulkan 1.3 GPU too (testing them).
typedef struct {
    uint32_t api;  // the Vulkan version used: the device's, at most 1.3
    bool sync1;    // no synchronization2: barriers, submits and timestamps through the Vulkan 1.0 commands
    bool render_pass;  // no dynamic rendering: render passes and framebuffers (compat_rp.c)
    bool no_eds2;      // no extended dynamic state 2: primitive restart in the pipeline key, depth bias always on in
                       // the pipeline (its values are dynamic and zero while the game has it off: the same)
} VkCompat;
extern VkCompat g_vkc;
void vkc_CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *d);
VkResult vkc_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *si, VkFence fence);
void vkc_CmdWriteTimestamp2(VkCommandBuffer cb, VkPipelineStageFlags2 stage, VkQueryPool pool, uint32_t query);
// (compat_rp.c)
void vkc_CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri);
void vkc_CmdEndRendering(VkCommandBuffer cb);
VkResult vkc_CreateGraphicsPipelines(VkDevice dev, VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
                                     const VkAllocationCallbacks *al, VkPipeline *out);
VkResult vkc_CreateImageView(VkDevice dev, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *al, VkImageView *v);
void vkc_DestroyImageView(VkDevice dev, VkImageView v, const VkAllocationCallbacks *al);
VkResult vkc_vmaCreateImage(VmaAllocator a, const VkImageCreateInfo *ci, const VmaAllocationCreateInfo *ai, VkImage *img,
                            VmaAllocation *mem, VmaAllocationInfo *info);
VkRenderPass vkc_render_pass_for(uint32_t ncolor, const VkFormat *color, VkFormat depth, VkFormat stencil,
                                 VkSampleCountFlagBits samples, VkImageLayout layout);
// (no extended dynamic state 2: exec.c bakes these into the pipeline instead)
static inline void vkc_CmdSetDepthBiasEnable(VkCommandBuffer cb, VkBool32 on) {
    if (vkp_vkCmdSetDepthBiasEnable) vkp_vkCmdSetDepthBiasEnable(cb, on);
}
static inline void vkc_CmdSetPrimitiveRestartEnable(VkCommandBuffer cb, VkBool32 on) {
    if (vkp_vkCmdSetPrimitiveRestartEnable) vkp_vkCmdSetPrimitiveRestartEnable(cb, on);
}
#define vkCmdSetDepthBiasEnable vkc_CmdSetDepthBiasEnable
#define vkCmdSetPrimitiveRestartEnable vkc_CmdSetPrimitiveRestartEnable
#define vkCmdBeginRendering vkc_CmdBeginRendering
#define vkCmdEndRendering vkc_CmdEndRendering
#define vkCreateGraphicsPipelines vkc_CreateGraphicsPipelines
#define vkCreateImageView vkc_CreateImageView
#define vkDestroyImageView vkc_DestroyImageView
#define vmaCreateImage vkc_vmaCreateImage
#define vkCmdPipelineBarrier2 vkc_CmdPipelineBarrier2
#define vkQueueSubmit2 vkc_QueueSubmit2
#define vkCmdWriteTimestamp2 vkc_CmdWriteTimestamp2

#include "../nvn.h"
#include "../maxwell/shader.h"

void vk_device_fault_report(void);
void vk_checkpoint(VkCommandBuffer cb, uint64_t mark);
#define VK_CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { if (r_ == VK_ERROR_DEVICE_LOST) vk_device_fault_report(); \
    FATAL("%s failed: %d (%s:%d)\n", #x, r_, __FILE__, __LINE__); } } while (0)

typedef struct {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t qfam;
    VmaAllocator vma;
    VkPhysicalDeviceProperties props;
    VkPipelineCache pcache;
    VkSemaphore timeline;      // value = last completed submission number
    VkCommandPool pool;        // render thread only
    bool custom_border, divisor, host_import;
    // window
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat sc_format;
    VkExtent2D sc_extent;
    uint32_t sc_count;
    VkImage sc_images[8];
    VkImageView sc_views[8];
} VkG;
extern VkG g_vk;

// ---- submissions (exec.c)
// Work is recorded into the current command buffer; uploads go into a separate one that runs first.
VkCommandBuffer vk_cmd(void);         // current command buffer (outside any rendering unless exec.c began one)
VkCommandBuffer vk_upload_cmd(void);  // upload command buffer of the pending submission
uint64_t vk_pending_seq(void);        // number the pending submission will get
uint64_t vk_completed_seq(void);
void vk_wait_seq(uint64_t seq);
void vk_full_barrier(VkCommandBuffer cb);
void vk_end_pass(void);
void vk_submit_extra(VkSemaphore wait, VkSemaphore signal, VkCommandBuffer extra);  // submit + one more cb (present)

// Transient GPU-visible memory that lives until the submission it was allocated for completes.
typedef struct { VkBuffer buf; uint64_t off; void *ptr; } VkSpan;
VkSpan vk_transient(uint64_t size, uint64_t align);
VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align);
VkSpan vk_readback(uint64_t size);  // host-cached GPU -> CPU staging (read right after the copy completes)
VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout);

// ---- textures (textures.c)
typedef struct TexBackend {
    VkImage image;
    VmaAllocation mem;
    VkFormat format;
    VkImageViewType base_type;
    uint32_t width, height, depth, layers, levels, samples;
    VkImageAspectFlags aspect;
    bool gpu_owned, uploaded, dynamic;
    uint64_t data_hash, sample_hash, checked_frame, full_frame;
    uint32_t wgen;  // page write tracking generation at the last upload check (textures.c)
    bool scalable, presented;
    uint64_t touched_frame, frame_cleared;
    uint32_t level_gen;
    float sx, sy;
    void *pending_rgba;  // host-produced pixels (video) waiting to be uploaded on the render thread
    uint32_t pending_w, pending_h, pending_stride;
    uint8_t astc_bw, astc_bh;  // ASTC without GPU support: decoded to RGBA8 on upload (runtime/nvn/astc.c)
    bool astc_srgb;
    int tp;              // runtime/nvn/texpatch.c: 0 not checked yet, -1 none, else known UI texture + 1
    uint32_t tp_gen;     // texpatch_generation() it was last uploaded with
    int reg;  // slot in textures.c's live list + 1 (a reboot destroys them all)
    bool hd;  // replaced by an HD texture pack's image (textures.c): sx / sy = its size over the game's
} TexBackend;

static inline bool vk_format_is_srgb(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK: return true;
    default: return f >= VK_FORMAT_ASTC_4x4_SRGB_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK && (f - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) % 2 == 1;
    }
}

TexBackend *vk_tex(NvnTexture *t, bool for_render);  // backend image (uploads CPU contents unless for_render)
VkFormat vk_format(uint32_t nvn_format);
bool vk_format_alpha_one(uint32_t nvn_format);
// View of t (optionally through an NVN texture view) for rendering or sampling.
VkImageView vk_view(NvnTexture *t, const NvnTextureView *v, bool has_view, bool render, int want_textype);
// Attachment view of every layer in range (3D targets: slices, as a 2D array); *layers = its layer count.
VkImageView vk_render_view(NvnTexture *t, const NvnTextureView *v, bool has_view, uint32_t *layers);
VkImageView vk_image_view(TexBackend *b, VkImageViewType type, VkFormat fmt, uint32_t lv0, uint32_t nlv, uint32_t ly0,
                          uint32_t nly, VkComponentMapping sw, VkImageAspectFlags aspect);
VkImageView vk_dummy_view(int textype);
VkSampler vk_sampler(NvnSamplerDesc *d);
VkSampler vk_default_sampler(bool linear);
VkSampler vk_sampler_nearest(const NvnSamplerDesc *d);  // (integer formats: unfiltered)
int vk_view_int_kind(NvnTexture *t, const NvnTextureView *v, bool has_view);  // 0 float, 1 uint, 2 sint
void vk_image_init_layout(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect);
// Destruction once every submission that may use the object has completed.
void vk_defer_image(VkImage img, VmaAllocation mem);
void vk_defer_view(VkImageView v);
void vk_views_forget(TexBackend *b);  // drop (deferred) the cached views of an image being replaced
uint64_t vk_frame_counter(void);

// ---- shaders / pipelines (shaders.c)
VkShaderModule vk_compile_glsl(const char *src, int stage, const char *name);  // NULL on failure (logged)
VkShaderModule vk_builtin_shader(const char *name);  // internal shaders (clear, copy, present)

// ---- device / window (device.c)
void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);
