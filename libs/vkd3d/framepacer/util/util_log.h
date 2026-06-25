#pragma once

#include <inttypes.h>
#include <assert.h>
extern "C" {
    #define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_API
    #include "vkd3d_debug.h"
}

namespace pacer {
    inline std::atomic< bool > enableLog = { false };
}

#define _INFO(...) \
    do { \
        if (pacer::enableLog.load(std::memory_order_relaxed)) { \
            INFO(__VA_ARGS__); \
        } \
    } while(0)
