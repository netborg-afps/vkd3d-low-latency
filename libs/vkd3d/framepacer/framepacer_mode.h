#pragma once

#include "latency_markers.h"
#include "frame_sync.h"

extern "C" {
#include <vkd3d_platform.h>
}

namespace pacer {

  /*
   * /brief Abstract frame pacer mode in order to support different strategies of synchronization.
   */

    class FramePacerMode {

        using time_point = dxvk::high_resolution_clock::time_point;

    public:

        enum Mode {
            MAX_FRAME_LATENCY = 0,
            LOW_LATENCY,
            LOW_LATENCY_VRR,
            MIN_LATENCY
        };

        FramePacerMode( Mode mode, const char* name, LatencyMarkersStorage* markerStorage, FrameSync* frameSync, uint64_t firstFrameId )
        :   m_mode( mode ),
            m_name( name ),
            m_firstFrameId( firstFrameId ),
            m_latencyMarkers( markerStorage ),
            m_frameSync( frameSync ) {
            setFpsLimitFrametimeFromEnv();
        }

        virtual ~FramePacerMode() { }

        virtual void startFrame( uint64_t frameId, time_point lastSimulationStart ) { }
        virtual void endFrame( uint64_t frameId ) { }

        virtual void finishRender( uint64_t frameId ) { }

        virtual void notifyQueueSubmit( uint64_t frameId, time_point t ) { }
        virtual void notifyGpuReady( uint64_t frameId, time_point t ) { }

        void setPresentMode( uint32_t presentMode )
          { m_presentMode = presentMode; }

        uint32_t getPresentMode()
          { return m_presentMode; }

        void setFpsLimit( uint32_t frametime ) {
            if (!frametime && m_fpsLimitFrametimeEnv) {
                m_fpsLimitFrametime.store( m_fpsLimitFrametimeEnv );
                return;
            }

            m_fpsLimitFrametime.store( frametime );
        }

        const Mode m_mode;

        const char* getName() const
        { return m_name; }

        uint64_t getFirstFrameId() const
        { return m_firstFrameId; }

    protected:

        void setFpsLimitFrametimeFromEnv();

        const char*             m_name;
        const uint64_t          m_firstFrameId;
        LatencyMarkersStorage*  m_latencyMarkers;
        FrameSync*              m_frameSync;

        std::atomic<uint32_t>   m_presentMode;
        std::atomic<int32_t>    m_fpsLimitFrametime    = { 0 };
        int32_t                 m_fpsLimitFrametimeEnv = { 0 };

    };

    inline void FramePacerMode::setFpsLimitFrametimeFromEnv() {

        double target_frame_rate = 0;
        char env[16];
        if (vkd3d_get_env_var("VKD3D_FRAME_RATE", env, sizeof(env))) {
            target_frame_rate = strtod(env, NULL);
        }

        if (target_frame_rate < 1.0)
            return;

        m_fpsLimitFrametimeEnv = (int) (1'000'000/target_frame_rate);
        m_fpsLimitFrametime.store( m_fpsLimitFrametimeEnv );
        INFO( "set fps limit frametime to: %" PRIi32 " us\n", m_fpsLimitFrametime.load() );

    }

}
