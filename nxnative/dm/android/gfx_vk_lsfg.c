// SPDX-License-Identifier: MIT
// The LSFG 3.1 Performance pass graph is adapted from lsfg-vk (Copyright 2025 lsfg-vk).
// Source: https://github.com/PancakeTAS/lsfg-vk/tree/v1.0.0/framegen
// License: apk/lsfg-vk-LICENSE.txt
#include "gfx_vk_lsfg.h"
#include "lossless_dll.h"
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    LEVELS = 7, HISTORY = 3, PLANS = 2 * HISTORY, MAX_BINDINGS = 12,
    MAX_IMAGES = 3 + LEVELS * (1 + 10 + 6) + 10 + 6,
    MAX_JOBS = 1 + LEVELS * 4 + 5 + LEVELS * 5 + 3 * 10 + 1,
};
enum { BASE, MIDPOINT, FIRST_GAMMA, FIRST_DELTA, CONSTANTS };
enum { BLACK_BORDER, WHITE_BORDER, EDGE, SAMPLERS };
typedef struct {
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkPipeline pipeline;
} lsfg_pass;

typedef struct {
    lsfg_pass *pass;
    VkDescriptorSet set;
    VkExtent2D groups;
} lsfg_job;

typedef struct {
    uint32_t offset[2], first, first_secondary, color_kind, hdr;
    float inverse_scale, timestamp, ui_threshold;
    uint32_t padding[3];
} lsfg_constants;

typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
} frame_image;

static const VkImageSubresourceRange color_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
static const VkImageSubresourceLayers color_layers = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
static const VkImageUsageFlags work_usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
static const VkFormat work_format = VK_FORMAT_R8G8B8A8_UNORM;
static const VkFormat flow_format = VK_FORMAT_R8_UNORM;
static const VkFormat vector_format = VK_FORMAT_R16G16B16A16_SFLOAT;
static struct {
    lsfg_pass passes[LSFG_SHADER_COUNT];
    unsigned epoch;
} programs;

static struct {
    VkExtent2D size;
    VkOffset3D offsets[2];
    VkDescriptorPool pool;
    VkSampler samplers[SAMPLERS];
    VkBuffer constants;
    VkDeviceMemory constant_memory;
    VkDeviceSize constant_stride;
    frame_image images[MAX_IMAGES], *input[2], *output;
    unsigned images_used, analysis_jobs, history, slot, epoch;
    lsfg_job jobs[PLANS][MAX_JOBS];   // two color frames and three feature history slots
    int initialized, ready;
} fg;

static _Atomic int available;
static struct stat observed_file;
static unsigned file_epoch;
static uint64_t last_poll;
static int poll_started, saw_file;
static void destroy_programs(void);
static VkResult wait_queue(void);

int vk_lsfg_available(void) { return atomic_load(&available); }

static int same_file(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
           a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
           a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

static int open_dll(struct stat *info) {
    const char *root = getenv("DM_ROOT");
    char path[1024];
    if (!root || snprintf(path, sizeof path, "%s/Lossless.dll", root) >= (int)sizeof path) return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    uint8_t magic[2];
    if (fd >= 0 && (fstat(fd, info) || !S_ISREG(info->st_mode) ||
                    info->st_size < 64 || info->st_size > LSFG_DLL_MAX_SIZE ||
                    pread(fd, magic, sizeof magic, 0) != sizeof magic || magic[0] != 'M' || magic[1] != 'Z')) {
        close(fd);
        return -1;
    }
    return fd;
}

static void *read_dll(int fd, const struct stat *info) {
    size_t size = (size_t)info->st_size, read_size = 0;
    uint8_t *data = malloc(size);
    if (!data) return NULL;
    while (read_size < size) {
        ssize_t n = read(fd, data + read_size, size - read_size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        read_size += (size_t)n;
    }
    struct stat after;
    if (read_size == size && !fstat(fd, &after) && same_file(info, &after)) return data;
    free(data);
    return NULL;
}

typedef struct { unsigned uniform, samplers, sampled, storage; } lsfg_layout;

// SPIR-V from Lossless.dll uses set 0 and separate register ranges for each descriptor type.
static VkDescriptorType binding_type(unsigned binding) {
    if (binding < 16) return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    if (binding < 32) return VK_DESCRIPTOR_TYPE_SAMPLER;
    if (binding < 48) return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
}

static const lsfg_layout layouts[LSFG_SHADER_COUNT] = {
    {1,1,1,7}, {1,2,5,1},                                               // 255 mipmaps, 256 output
    {1,2,5,3}, {1,2,6,1}, {0,1,3,2}, {0,1,2,2}, {0,1,2,2}, {1,2,4,1}, // 280–285 gamma and delta
    {0,1,3,2}, {0,1,2,2}, {0,1,2,2}, {1,2,4,1},                       // 286–289 delta
    {0,1,1,1}, {0,1,1,1}, {0,1,1,2}, {0,1,2,2},                       // 290–293 alpha
    {0,1,1,1}, {0,1,1,1}, {0,1,1,1}, {1,2,2,1},                       // 294–297 delta
    {0,1,6,2}, {0,1,2,2}, {0,1,2,2}, {0,1,2,2}, {1,1,2,6},             // 298–302 beta
};

static unsigned pass_index(unsigned resource) { return resource < 280 ? resource - 255 : resource - 278; }

static uint64_t binding_mask(lsfg_layout layout) {
    return ((1ull << layout.uniform) - 1) | (((1ull << layout.samplers) - 1) << 16) |
           (((1ull << layout.sampled) - 1) << 32) | (((1ull << layout.storage) - 1) << 48);
}

static int compatible_shader(lsfg_shader shader, unsigned index) {
    const uint32_t *words = shader.words;
    const size_t count = shader.size / 4;
    if (words[1] < 0x10000 || words[1] > 0x10300 || words[4] != 0) return 0;
    uint64_t mask = 0;
    uint32_t entry = 0;
    int local_size = 0;
    unsigned width = index == 0 || index == LSFG_SHADER_COUNT - 1 ? 32 : index == 1 ? 16 : 8;
    for (size_t pos = 5; pos < count;) {
        const uint32_t *ins = words + pos;
        unsigned length = ins[0] >> 16, op = ins[0] & 0xffff;
        if (!length || length > count - pos) return 0;
        if (op == 17) {   // OpCapability
            if (length != 2) return 0;
            switch (ins[1]) {
            case 1: case 49: case 50: case 56: break; // Shader, ExtendedFormats, ImageQuery, WriteWithoutFormat
            case 9: if (V.has_float16) break; // Float16
                return 0;
            default: return 0;
            }
        } else if (op == 14 && (length != 3 || ins[1] != 0 || ins[2] != 1)) {
            return 0;   // OpMemoryModel must be Logical GLSL450
        } else if (op == 15) {   // OpEntryPoint GLCompute main
            if (length < 5 || ins[1] != 5 || ins[3] != 0x6e69616du || ins[4] != 0 || entry) return 0;
            entry = ins[2];
        } else if (op == 16 && length >= 3 && ins[2] == 17) {   // OpExecutionMode LocalSize
            if (length != 6 || !entry || ins[1] != entry || ins[3] != width || ins[4] != width || ins[5] != 1) return 0;
            local_size = 1;
        } else if (op == 71 && length >= 4) {   // OpDecorate Binding or DescriptorSet
            if (ins[2] == 34 && ins[3] != 0) return 0;
            if (ins[2] == 33) {
                if (ins[3] >= 64 || (mask & (1ull << ins[3]))) return 0;
                mask |= 1ull << ins[3];
            }
        }
        pos += length;
    }
    return entry && local_size && mask == binding_mask(layouts[index]);
}

static int shader_set(const void *data, size_t size, lsfg_shader shaders[LSFG_SHADER_COUNT]) {
    const unsigned offsets[] = {49, 98};   // Native FP16, then the FP32 variant.
    for (unsigned j = V.has_float16 ? 0 : 1; j < 2; ++j) {
        if (!lsfg_dll_shaders(data, size, offsets[j], shaders)) continue;
        unsigned i = 0;
        while (i < LSFG_SHADER_COUNT && compatible_shader(shaders[i], i)) ++i;
        if (i == LSFG_SHADER_COUNT) return 1;
    }
    return 0;
}

int vk_lsfg_validate_dll(const void *data, size_t size) {
    lsfg_shader shaders[LSFG_SHADER_COUNT];
    return shader_set(data, size, shaders);
}

static int format_supports(VkFormat format, VkFormatFeatureFlags features) {
    VkFormatProperties properties;
    vkGetPhysicalDeviceFormatProperties(V.pd, format, &properties);
    return (properties.optimalTilingFeatures & features) == features;
}

static int supported(void) {
    const VkPhysicalDeviceLimits *limits = &V.props.limits;
    const VkFormatFeatureFlags sampled = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    const VkFormatFeatureFlags storage = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    return V.has_dtiming && V.has_robust2 && V.has_storage_image_extended_formats &&
           V.has_storage_image_write_without_format && limits->maxComputeWorkGroupInvocations >= 1024 &&
           limits->maxComputeWorkGroupSize[0] >= 32 && limits->maxComputeWorkGroupSize[1] >= 32 &&
           format_supports(work_format, sampled | storage | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
           format_supports(flow_format, sampled | storage) && format_supports(vector_format, sampled | storage);
}

int vk_lsfg_poll(uint64_t now_ns) {
    if (poll_started && now_ns >= last_poll && now_ns - last_poll < 1000000000ull) return 0;
    poll_started = 1;
    last_poll = now_ns;
    int before = vk_lsfg_available();
    struct stat info;
    int fd = open_dll(&info);
    if (fd < 0) {
        if (!saw_file) return 0;
        saw_file = 0;
    } else if (saw_file && same_file(&info, &observed_file)) {
        close(fd);
        return 0;
    } else {
        saw_file = 1;
        observed_file = info;
    }
    int valid = 0;
    ++file_epoch;
    if (programs.epoch && !wait_queue()) {
        vk_lsfg_destroy();
        destroy_programs();
    }
    if (fd >= 0) {
        lsfg_shader shaders[LSFG_SHADER_COUNT];
        int device_ok = supported();
        void *data = device_ok ? read_dll(fd, &info) : NULL;
        if (data) {
            valid = shader_set(data, (size_t)info.st_size, shaders);
            free(data);
        } else if (device_ok) saw_file = 0;   // Retry transient file reads on the next poll.
        close(fd);
        if (valid) VKLOG("Lossless.dll: compatible LSFG shader set found");
        else VKLOG("[RED] Lossless.dll: unreadable or incompatible with this Vulkan device");
    } else {
        VKLOG("Lossless.dll is no longer readable");
    }
    vk_lsfg_forget();
    atomic_store(&available, valid);
    return before != valid;
}

static void destroy_programs(void) {
    for (unsigned i = 0; i < LSFG_SHADER_COUNT; ++i) {
        lsfg_pass *pass = &programs.passes[i];
        if (pass->pipeline) vkDestroyPipeline(V.dev, pass->pipeline, NULL);
        if (pass->layout) vkDestroyPipelineLayout(V.dev, pass->layout, NULL);
        if (pass->set_layout) vkDestroyDescriptorSetLayout(V.dev, pass->set_layout, NULL);
    }
    memset(&programs, 0, sizeof programs);
}

void vk_lsfg_destroy(void) {
    if (fg.pool) vkDestroyDescriptorPool(V.dev, fg.pool, NULL);
    for (unsigned i = 0; i < SAMPLERS; ++i)
        if (fg.samplers[i]) vkDestroySampler(V.dev, fg.samplers[i], NULL);
    for (unsigned i = 0; i < fg.images_used; ++i) {
        frame_image *image = &fg.images[i];
        if (image->view) vkDestroyImageView(V.dev, image->view, NULL);
        if (image->image) vkDestroyImage(V.dev, image->image, NULL);
        if (image->memory) vkFreeMemory(V.dev, image->memory, NULL);
    }
    if (fg.constants) vkDestroyBuffer(V.dev, fg.constants, NULL);
    if (fg.constant_memory) vkFreeMemory(V.dev, fg.constant_memory, NULL);
    memset(&fg, 0, sizeof fg);
}

static VkResult create_pass(lsfg_pass *pass, lsfg_layout desc, lsfg_shader code) {
    uint64_t mask = binding_mask(desc);
    VkDescriptorSetLayoutBinding bindings[MAX_BINDINGS];
    unsigned used = 0;
    for (unsigned i = 0; i < 64; ++i)
        if (mask & (1ull << i)) bindings[used++] = (VkDescriptorSetLayoutBinding){i, binding_type(i), 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = used, .pBindings = bindings,
    };
    VkResult result = vkCreateDescriptorSetLayout(V.dev, &set_info, NULL, &pass->set_layout);
    if (result) return result;
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &pass->set_layout,
    };
    result = vkCreatePipelineLayout(V.dev, &layout_info, NULL, &pass->layout);
    if (result) return result;
    VkShaderModule module;
    VkShaderModuleCreateInfo module_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = code.size, .pCode = code.words,
    };
    result = vkCreateShaderModule(V.dev, &module_info, NULL, &module);
    if (result) return result;
    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pass->layout,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = module, .pName = "main"},
    };
    result = vkCreateComputePipelines(V.dev, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pass->pipeline);
    vkDestroyShaderModule(V.dev, module, NULL);
    return result;
}

static VkResult create_programs(void) {
    struct stat info;
    int fd = open_dll(&info);
    void *data = NULL;
    if (fd >= 0) {
        if (same_file(&info, &observed_file)) data = read_dll(fd, &info);
        close(fd);
    }
    if (!data) {
        saw_file = 0;   // The file may have changed between the poll and pipeline creation.
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    lsfg_shader shaders[LSFG_SHADER_COUNT];
    VkResult result = VK_ERROR_INVALID_SHADER_NV;
    if (shader_set(data, (size_t)info.st_size, shaders)) {
        result = VK_SUCCESS;
        for (unsigned i = 0; !result && i < LSFG_SHADER_COUNT; ++i)
            result = create_pass(&programs.passes[i], layouts[i], shaders[i]);
    }
    free(data);
    if (result) destroy_programs();
    else programs.epoch = file_epoch;
    return result;
}

static VkResult create_constants(void) {
    VkDeviceSize align = V.props.limits.minUniformBufferOffsetAlignment;
    fg.constant_stride = (sizeof(lsfg_constants) + align - 1) & ~(align - 1);
    VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = fg.constant_stride * CONSTANTS,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
    };
    VkResult result = vkCreateBuffer(V.dev, &info, NULL, &fg.constants);
    if (result) return result;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(V.dev, fg.constants, &req);
    uint32_t type = vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    if (type == UINT32_MAX) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type,
    };
    result = vkAllocateMemory(V.dev, &allocation, NULL, &fg.constant_memory);
    if (result) return result;
    result = vkBindBufferMemory(V.dev, fg.constants, fg.constant_memory, 0);
    if (result) return result;
    void *mapping;
    result = vkMapMemory(V.dev, fg.constant_memory, 0, VK_WHOLE_SIZE, 0, &mapping);
    if (result) return result;
    for (unsigned i = 0; i < CONSTANTS; ++i) {
        lsfg_constants cb = {.first = i == FIRST_GAMMA, .first_secondary = i == FIRST_DELTA,
                             .inverse_scale = 2.0f, .timestamp = i == BASE ? 0.0f : 0.5f, .ui_threshold = 0.5f};
        memcpy((uint8_t *)mapping + i * fg.constant_stride, &cb, sizeof cb);
    }
    vkUnmapMemory(V.dev, fg.constant_memory);
    return VK_SUCCESS;
}

static VkResult create_samplers(void) {
    for (unsigned i = 0; i < SAMPLERS; ++i) {
        VkSamplerAddressMode address = i == EDGE ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        VkSamplerCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR, .addressModeU = address, .addressModeV = address, .addressModeW = address,
            .borderColor = i == WHITE_BORDER ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
            .maxLod = VK_LOD_CLAMP_NONE,
        };
        VkResult result = vkCreateSampler(V.dev, &info, NULL, &fg.samplers[i]);
        if (result) return result;
    }
    return VK_SUCCESS;
}

typedef struct {
    frame_image *half[2], *quarter[2], *features[HISTORY][2];
} lsfg_alpha;

typedef struct {
    frame_image *a[3], *b[2], *motion, *delta[2];
} lsfg_refine;

typedef struct {
    VkExtent2D flow, features[LEVELS];
    frame_image *mips[LEVELS], *beta_work[2][2], *beta_flow[LEVELS - 1];
    lsfg_alpha alpha[LEVELS];
    lsfg_refine refine[LEVELS];
    VkResult error;
} lsfg_graph;

static VkResult image_create(frame_image *image, VkExtent2D size, VkFormat format, VkImageUsageFlags usage) {
    VkImageCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = format, .extent = {size.width, size.height, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
    };
    VkResult result = vkCreateImage(V.dev, &ci, NULL, &image->image);
    if (result) return result;

    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(V.dev, image->image, &requirements);
    uint32_t type = vk_mem_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (type == UINT32_MAX) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size, .memoryTypeIndex = type,
    };
    result = vkAllocateMemory(V.dev, &ai, NULL, &image->memory);
    if (result) return result;
    result = vkBindImageMemory(V.dev, image->image, image->memory, 0);
    if (result) return result;
    VkImageViewCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format, .subresourceRange = color_range,
    };
    return vkCreateImageView(V.dev, &vi, NULL, &image->view);
}

static frame_image *graph_image(lsfg_graph *graph, VkExtent2D size, VkFormat format, VkImageUsageFlags usage) {
    if (graph->error) return NULL;
    if (fg.images_used == MAX_IMAGES) {
        graph->error = VK_ERROR_INITIALIZATION_FAILED;
        return NULL;
    }
    frame_image *image = &fg.images[fg.images_used++];
    graph->error = image_create(image, size, format, usage);
    return image;
}

static VkExtent2D div_up(VkExtent2D size, uint32_t factor) {
    return (VkExtent2D){(size.width + factor - 1) / factor, (size.height + factor - 1) / factor};
}

static VkExtent2D mip_extent(VkExtent2D size, unsigned level) {
    return (VkExtent2D){size.width >> level, size.height >> level};
}

static void create_graph_images(lsfg_graph *graph) {
    fg.input[0] = graph_image(graph, fg.size, work_format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    fg.input[1] = graph_image(graph, fg.size, work_format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    fg.output = graph_image(graph, fg.size, work_format, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    graph->flow = mip_extent(fg.size, 1);
    for (unsigned i = 0; i < LEVELS; ++i) {
        VkExtent2D mip = mip_extent(graph->flow, i), half = div_up(mip, 2), quarter = div_up(half, 2);
        graph->mips[i] = graph_image(graph, mip, flow_format, work_usage);
        graph->features[i] = quarter;
        lsfg_alpha *alpha = &graph->alpha[i];
        for (unsigned j = 0; j < 2; ++j) {
            alpha->half[j] = graph_image(graph, half, work_format, work_usage);
            alpha->quarter[j] = graph_image(graph, quarter, work_format, work_usage);
            for (unsigned k = 0; k < HISTORY; ++k)
                alpha->features[k][j] = graph_image(graph, quarter, work_format, work_usage);
        }
        lsfg_refine *refine = &graph->refine[i];
        for (unsigned j = 0; j < 3; ++j) refine->a[j] = graph_image(graph, quarter, work_format, work_usage);
        for (unsigned j = 0; j < 2; ++j) refine->b[j] = graph_image(graph, quarter, work_format, work_usage);
        refine->motion = graph_image(graph, quarter, vector_format, work_usage);
        if (i < 3) for (unsigned j = 0; j < 2; ++j)
            refine->delta[j] = graph_image(graph, quarter, vector_format, work_usage);
    }
    VkExtent2D features = graph->features[0];
    for (unsigned i = 0; i < 2; ++i) for (unsigned j = 0; j < 2; ++j)
        graph->beta_work[i][j] = graph_image(graph, features, work_format, work_usage);
    for (unsigned i = 0; i < LEVELS - 1; ++i)
        graph->beta_flow[i] = graph_image(graph, mip_extent(features, i), flow_format, work_usage);
}

typedef struct {
    lsfg_job *jobs;
    unsigned count;
    VkResult error;
} lsfg_plan;

static void add_job(lsfg_plan *plan, unsigned resource, VkExtent2D groups, unsigned constants,
                    const unsigned *samplers, frame_image *const *sampled, frame_image *const *storage) {
    if (plan->error) return;
    if (plan->count == MAX_JOBS) {
        plan->error = VK_ERROR_INITIALIZATION_FAILED;
        return;
    }
    unsigned index = pass_index(resource);
    lsfg_pass *pass = &programs.passes[index];
    lsfg_job *job = &plan->jobs[plan->count++];
    job->pass = pass;
    job->groups = groups;
    VkDescriptorSetAllocateInfo info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = fg.pool,
        .descriptorSetCount = 1, .pSetLayouts = &pass->set_layout,
    };
    plan->error = vkAllocateDescriptorSets(V.dev, &info, &job->set);
    if (plan->error) return;

    lsfg_layout layout = layouts[index];
    uint64_t mask = binding_mask(layout);
    VkWriteDescriptorSet writes[MAX_BINDINGS];
    VkDescriptorImageInfo images[MAX_BINDINGS];
    VkDescriptorBufferInfo buffer = {fg.constants, fg.constant_stride * constants, sizeof(lsfg_constants)};
    unsigned count = 0;
    for (unsigned binding = 0; binding < 64; ++binding) {
        if (!(mask & (1ull << binding))) continue;
        VkDescriptorType type = binding_type(binding);
        VkWriteDescriptorSet *write = &writes[count];
        *write = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = job->set,
            .dstBinding = binding, .descriptorCount = 1, .descriptorType = type};
        if (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            write->pBufferInfo = &buffer;
        } else {
            VkDescriptorImageInfo *image = &images[count];
            *image = (VkDescriptorImageInfo){.imageLayout = VK_IMAGE_LAYOUT_GENERAL};
            if (type == VK_DESCRIPTOR_TYPE_SAMPLER) {
                image->sampler = fg.samplers[samplers[binding - 16]];
            } else {
                const frame_image *view = type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ? sampled[binding - 32] : storage[binding - 48];
                image->imageView = view ? view->view : VK_NULL_HANDLE;
            }
            write->pImageInfo = image;
        }
        ++count;
    }
    vkUpdateDescriptorSets(V.dev, count, writes, 0, NULL);
}

static const unsigned black[] = {BLACK_BORDER};
static const unsigned white[] = {WHITE_BORDER};
static const unsigned black_edge[] = {BLACK_BORDER, EDGE};
static const unsigned white_edge[] = {WHITE_BORDER, EDGE};

static void add_alpha(lsfg_plan *plan, lsfg_graph *graph, unsigned level, unsigned current) {
    lsfg_alpha *a = &graph->alpha[level];
    VkExtent2D half = div_up(mip_extent(graph->flow, level), 2);
    VkExtent2D h = div_up(half, 8), q = div_up(graph->features[level], 8);
    add_job(plan, 290, h, BASE, black, (frame_image *[]){graph->mips[level]}, &a->half[0]);
    add_job(plan, 291, h, BASE, black, &a->half[0], &a->half[1]);
    add_job(plan, 292, q, BASE, black, &a->half[1], a->quarter);
    add_job(plan, 293, q, BASE, black, a->quarter, a->features[current]);
}

static void add_beta(lsfg_plan *plan, lsfg_graph *graph, unsigned current, unsigned previous, unsigned older) {
    lsfg_alpha *a = &graph->alpha[0];
    frame_image **x = graph->beta_work[0], **y = graph->beta_work[1];
    VkExtent2D extent = graph->features[0], groups = div_up(extent, 8);
    frame_image *history[] = {a->features[older][0], a->features[older][1], a->features[previous][0],
                              a->features[previous][1], a->features[current][0], a->features[current][1]};
    add_job(plan, 298, groups, BASE, white, history, x);
    add_job(plan, 299, groups, BASE, black, x, y);
    add_job(plan, 300, groups, BASE, black, y, x);
    add_job(plan, 301, groups, BASE, black, x, y);
    add_job(plan, 302, div_up(extent, 32), MIDPOINT, black, y, graph->beta_flow);
}

static void add_refinement(lsfg_plan *plan, lsfg_graph *graph, unsigned level, unsigned current, unsigned previous, int delta) {
    lsfg_refine *r = &graph->refine[level];
    lsfg_refine *coarser = level + 1 < LEVELS ? &graph->refine[level + 1] : NULL;
    frame_image *prior = coarser ? coarser->motion : NULL;
    lsfg_alpha *a = &graph->alpha[level];
    VkExtent2D groups = div_up(graph->features[level], 8);
    frame_image *flow = graph->beta_flow[level < LEVELS - 1 ? level : LEVELS - 2];
    unsigned constants = delta && level == 2 ? FIRST_DELTA : !prior ? FIRST_GAMMA : MIDPOINT;
    frame_image *history[] = {a->features[previous][0], a->features[previous][1], a->features[current][0],
                              a->features[current][1], prior, delta ? coarser->delta[0] : NULL};
    frame_image *finish[] = {r->b[0], r->b[1], prior, flow};
    unsigned first_filter = delta ? 286 : 282;
    add_job(plan, 280, groups, constants, white_edge, history, r->a);
    add_job(plan, first_filter, groups, BASE, black, r->a, r->b);
    add_job(plan, first_filter + 1, groups, BASE, black, r->b, r->a);
    add_job(plan, first_filter + 2, groups, BASE, black, r->a, r->b);
    add_job(plan, first_filter + 3, groups, constants, black_edge, finish, delta ? r->delta : &r->motion);
    if (!delta) return;

    add_job(plan, 281, groups, constants, white_edge, history, r->b);
    add_job(plan, 294, groups, BASE, black, r->b, r->a);
    add_job(plan, 295, groups, BASE, black, r->a, r->b);
    add_job(plan, 296, groups, BASE, black, r->b, r->a);
    add_job(plan, 297, groups, constants, black_edge, (frame_image *[]){r->a[0], coarser->delta[1]}, &r->delta[1]);
}

static VkResult create_plans(lsfg_graph *graph) {
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, PLANS * MAX_JOBS},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 2 * PLANS * MAX_JOBS},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 6 * PLANS * MAX_JOBS},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 7 * PLANS * MAX_JOBS},
    };
    VkDescriptorPoolCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = PLANS * MAX_JOBS,
        .poolSizeCount = sizeof sizes / sizeof sizes[0], .pPoolSizes = sizes,
    };
    VkResult result = vkCreateDescriptorPool(V.dev, &info, NULL, &fg.pool);
    if (result) return result;
    for (unsigned i = 0; i < PLANS; ++i) {
        lsfg_plan plan = {.jobs = fg.jobs[i]};
        unsigned current = i % HISTORY, previous = (i + 2) % HISTORY, older = (i + 1) % HISTORY;
        add_job(&plan, 255, div_up(graph->flow, 64), BASE, black, &fg.input[i % 2], graph->mips);
        for (unsigned level = LEVELS; level-- > 0;) add_alpha(&plan, graph, level, current);
        fg.analysis_jobs = plan.count;
        add_beta(&plan, graph, current, previous, older);
        for (unsigned level = LEVELS; level-- > 0;) {
            add_refinement(&plan, graph, level, current, previous, 0);
            if (level < 3) add_refinement(&plan, graph, level, current, previous, 1);
        }
        frame_image *inputs[] = {fg.input[(i + 1) % 2], fg.input[i % 2], graph->refine[0].motion,
                                 graph->refine[0].delta[0], graph->refine[0].delta[1]};
        add_job(&plan, 256, div_up(fg.size, 16), MIDPOINT, black_edge, inputs, &fg.output);
        if (plan.error) return plan.error;
        if (plan.count != MAX_JOBS) return VK_ERROR_INITIALIZATION_FAILED;
    }
    return VK_SUCCESS;
}

static VkResult create_graph(void) {
    VkResult result = create_constants();
    if (result) return result;
    result = create_samplers();
    if (result) return result;
    lsfg_graph graph = {0};
    create_graph_images(&graph);
    return graph.error ? graph.error : create_plans(&graph);
}

static VkResult wait_queue(void) {
    extern pthread_mutex_t g_vk_queue_mu;
    pthread_mutex_lock(&g_vk_queue_mu);
    VkResult result = vkQueueWaitIdle(V.q);
    pthread_mutex_unlock(&g_vk_queue_mu);
    return result;
}

int vk_lsfg_prepare(VkExtent2D size, const VkOffset3D offsets[2]) {
    if (!vk_lsfg_available() || size.width < 256 || size.height < 256) return 0;
    if (fg.epoch == file_epoch && fg.size.width == size.width && fg.size.height == size.height) {
        if (memcmp(fg.offsets, offsets, sizeof fg.offsets)) {
            vk_lsfg_forget();
            memcpy(fg.offsets, offsets, sizeof fg.offsets);
        }
        return fg.ready;
    }
    if (fg.ready) {
        if (wait_queue()) return 0;
        vk_lsfg_destroy();
    }
    if (programs.epoch != file_epoch) {
        destroy_programs();
        VkResult result = create_programs();
        if (result) {
            VKLOG("[RED] Lossless Scaling pipelines failed (%d); frame generation unavailable", result);
            atomic_store(&available, 0);
            return 0;
        }
    }
    fg.size = size;
    VkResult result = create_graph();
    if (result) {
        VKLOG("[RED] Lossless Scaling resources failed at %ux%u (%d); frame generation unavailable",
              size.width, size.height, result);
        vk_lsfg_destroy();
    }
    fg.size = size;
    fg.epoch = file_epoch;
    memcpy(fg.offsets, offsets, sizeof fg.offsets);
    fg.ready = result == VK_SUCCESS;
    if (fg.ready) {
        VKLOG("Lossless Scaling frame generation: %ux%u, LSFG 3.1 Performance", size.width, size.height);
    } else {
        atomic_store(&available, 0);
    }
    return fg.ready;
}

int vk_lsfg_has_history(void) { return fg.history >= HISTORY - 1; }
void vk_lsfg_forget(void) { fg.history = 0; }

static void initialize_images(VkCommandBuffer cb) {
    VkImageMemoryBarrier barriers[MAX_IMAGES];
    for (unsigned i = 0; i < fg.images_used; ++i) {
        barriers[i] = (VkImageMemoryBarrier){
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = fg.images[i].image, .subresourceRange = color_range,
        };
    }
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, NULL, 0, NULL, fg.images_used, barriers);
}

VkImage vk_lsfg_record(VkCommandBuffer cb, VkImage source, int interpolate) {
    if (!fg.initialized) initialize_images(cb);
    VkImageBlit copy = {
        .srcSubresource = color_layers, .srcOffsets = {fg.offsets[0], fg.offsets[1]},
        .dstSubresource = color_layers, .dstOffsets = {{0, 0, 0}, {(int32_t)fg.size.width, (int32_t)fg.size.height, 1}},
    };
    vkCmdBlitImage(cb, source, VK_IMAGE_LAYOUT_GENERAL, fg.input[fg.slot % 2]->image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy, VK_FILTER_NEAREST);
    unsigned count = interpolate ? MAX_JOBS : fg.analysis_jobs;
    for (unsigned i = 0; i < count; ++i) {
        const lsfg_job *job = &fg.jobs[fg.slot][i];
        VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = i ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
        vkCmdPipelineBarrier(cb, i ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, job->pass->pipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, job->pass->layout, 0, 1, &job->set, 0, NULL);
        vkCmdDispatch(cb, job->groups.width, job->groups.height, 1);
    }
    if (!interpolate) return VK_NULL_HANDLE;
    VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                               .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &barrier, 0, NULL, 0, NULL);
    return fg.output->image;
}

void vk_lsfg_submitted(void) {
    fg.initialized = 1;
    if (fg.history < HISTORY - 1) ++fg.history;
    fg.slot = (fg.slot + 1) % PLANS;
}
