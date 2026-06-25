#include "framepacer.h"
#include "framepacer_mode_low_latency.h"
#include "framepacer_mode_min_latency.h"
#include "command_queue.h"
#include "vulkan_queue.h"
#include <string>
#include <stdint.h>

extern "C" {
#include "vkd3d_platform.h"
}

namespace pacer {

    int getRefreshRate( std::string s ) {
        return std::abs( std::atoi( s.substr(16).c_str() ) );
    }


    FramePacer::FramePacer( Device* device, uint64_t firstFrameId )
    : m_device(device) {
        // We'll default to LOW_LATENCY, which generally provides the best "input lag"
        // along with time consistency and often appears the smoothest too.
        // MAX_FRAME_LATENCY can have advantages in some games like God of War that provide inconsistent
        // cpu frametimes. Also, it's tuned for highest fps which can be relevant in benchmarks.
        FramePacerMode::Mode mode = FramePacerMode::LOW_LATENCY;
        char env[8];

        // dxvk-low-latency is using default wait-latency 2 here, which pretty much can
        // max out fps in any game. For vkd3d however, we need to default it to 3
        // to prevent fps throttling in a lot of games. We want still give the user
        // the option to manually set it to 2 though.
        if (vkd3d_get_env_var("VKD3D_SWAPCHAIN_LATENCY_FRAMES", env, sizeof(env))) {
            unsigned long latency_override = strtoul(env, NULL, 0);
            if (latency_override >= 1 && latency_override <= 3)
                m_frameSync.m_waitLatency = latency_override;
        }

        // int refreshRate = 0;

        // std::string configStr = "min-latency";
        //    env::getEnvVar("DXVK_FRAME_PACE");

        // if (configStr.find("max-frame-latency") != std::string::npos) {
        //   mode = FramePacerMode::MAX_FRAME_LATENCY;
        // } else if (configStr.find("low-latency-vrr-") != std::string::npos) {
        //   mode = FramePacerMode::LOW_LATENCY_VRR;
        //   refreshRate = getRefreshRate(configStr);
        // } else if (configStr.find("low-latency") != std::string::npos) {
        //   mode = FramePacerMode::LOW_LATENCY;
        // } else if (configStr.find("min-latency") != std::string::npos) {
        //   mode = FramePacerMode::MIN_LATENCY;
        // }
        //    else if (options.framePace.find("max-frame-latency") != std::string::npos) {
        //      mode = FramePacerMode::MAX_FRAME_LATENCY;
        //    } else if (options.framePace.find("low-latency-vrr-") != std::string::npos) {
        //      mode = FramePacerMode::LOW_LATENCY_VRR;
        //      refreshRate = getRefreshRate(options.framePace);
        //    } else if (options.framePace.find("low-latency") != std::string::npos) {
        //      mode = FramePacerMode::LOW_LATENCY;
        //    } else if (options.framePace.find("min-latency") != std::string::npos) {
        //      mode = FramePacerMode::MIN_LATENCY;
        //    }
        //    else if (!configStr.empty()) {
        //      Logger::warn( str::format( "DXVK_FRAME_PACE=", configStr, " unknown" ));
        //    } else if (!options.framePace.empty()) {
        //      Logger::warn( str::format( "dxvk.framePace = ", options.framePace, " unknown" ));
        //    }

        if (!m_device->m_calibratedDeviceTimestamps.canEnable() && mode != FramePacerMode::MIN_LATENCY) {
            WARN( "cannot enable low-latency frame pacing due to missing VK_KHR_calibrated_timestamps \n" );
            mode = FramePacerMode::MAX_FRAME_LATENCY;
        }

        switch (mode) {
        case FramePacerMode::MAX_FRAME_LATENCY:
            INFO( "Frame pace: max-frame-latency \n" );
            m_mode = std::make_unique<FramePacerMode>(FramePacerMode::MAX_FRAME_LATENCY, "max-frame-latency", &m_latencyMarkers, &m_frameSync, firstFrameId);
            break;

        case FramePacerMode::LOW_LATENCY:
            INFO( "Frame pace: low-latency \n" );
            INFO( "  m_frameSync.m_waitLatency = %i \n", m_frameSync.m_waitLatency.load() );
            m_device->m_calibratedDeviceTimestamps.enable();
            m_mode = std::make_unique<LowLatencyMode>(mode, &m_latencyMarkers, &m_frameSync, firstFrameId, m_device);
            break;

        case FramePacerMode::LOW_LATENCY_VRR:
        ////        Logger::info( "Frame pace: low-latency-vrr" );
        //        m_calibratedDeviceTimestamps.enable();
        //        m_mode = std::make_unique<LowLatencyMode>(mode, &m_latencyMarkers, &m_frameSync, options, firstFrameId, refreshRate);
        //        break;

        case FramePacerMode::MIN_LATENCY:
            INFO( "Frame pace: min-latency \n" );
            m_frameSync.m_waitLatency = 1;
            m_mode = std::make_unique<MinLatencyMode>(mode, &m_latencyMarkers, &m_frameSync, firstFrameId);
            break;
        }

        m_frameSync.cpuFinished   = firstFrameId-1;
        m_frameSync.gpuFinished   = firstFrameId-1;
        m_frameSync.frameFinished = firstFrameId-1;

    }


    FramePacer::~FramePacer() {

        delete m_presentationStats.load();
        delete m_gpuBufferStats.load();

    }

}
