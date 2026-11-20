#pragma once
#include "gfx_vk.h"

int vk_lsfg_validate_dll(const void *data, size_t size);

// Poll and record on the present thread. The front end reads availability atomically.
int vk_lsfg_poll(uint64_t now_ns);
int vk_lsfg_available(void);
int vk_lsfg_prepare(VkExtent2D size, const VkOffset3D offsets[2]);
int vk_lsfg_has_history(void);
void vk_lsfg_forget(void);
void vk_lsfg_destroy(void);   // releases size-dependent resources; the queue must be idle
VkImage vk_lsfg_record(VkCommandBuffer cb, VkImage source, int interpolate);
void vk_lsfg_submitted(void);
