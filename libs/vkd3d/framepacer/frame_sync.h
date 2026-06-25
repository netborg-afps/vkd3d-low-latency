#pragma once

#include "util/sync/sync_timeline_semaphore.h"

namespace pacer {

    /*
    * stores which information is accessible for which frame and is also used for signalling
    */

    class FrameSync {

    public:

        TimelineSemaphore cpuFinished;
        TimelineSemaphore gpuFinished;
        TimelineSemaphore frameFinished;

        std::atomic< int32_t > m_waitLatency = { 3 };

    };

}
