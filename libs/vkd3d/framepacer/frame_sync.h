#pragma once

#include "util/sync/sync_timeline_semaphore.h"

namespace pacer {

    /*
     * stores which information is accessible for which frame and is also used for signalling
     * Using convention:
     * cpu start = first DX12 submit
     * cpu finished = DX12 present
     * gpu finished = Vulkan GPU processing finished
     */

    class FrameSync {

    public:

        TimelineSemaphore cpuStarted;
        TimelineSemaphore cpuFinished;
        TimelineSemaphore gpuFinished;
        TimelineSemaphore frameFinished;

        std::atomic< int32_t > m_waitLatency = { 3 };

    };

}
