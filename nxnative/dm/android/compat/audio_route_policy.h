#pragma once
#include <stdatomic.h>
#include <stdint.h>

static inline int audio_route_fallback(_Atomic int32_t *selected, int32_t disconnected) {
    return disconnected && atomic_compare_exchange_strong(selected, &disconnected, 0);
}
