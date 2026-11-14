// Ported from nxnative 1f01e28c uniform_table.inc / resource_pools.inc.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
    uint64_t *handles;
    uint32_t *map, count, handle_capacity, map_capacity;
    uint64_t registration_generation, replay_job;
    int pool_base, pool_kind;
} vk_resource_table;
int vk_resource_pool_build(vk_resource_table *table, int sampler, int base);
