#pragma once

#include "framepacer_bridge.h"
#include "framepacer_mode.h"
#include "frame_sync.h"
#include "device.h"
#include "frame_mapping.h"
#include "latency_markers.h"
#include "latency_stats.h"
#include "jitter_stats.h"
#include "util/sync/sync_ringbuffer_allocator.h"
#include "util/util_log.h"


/* \brief Frame pacer interface managing the CPU - GPU synchronization.
 *
 * GPUs render frames asynchronously to the game's and dxvk's CPU-side work
 * in order to improve fps-throughput. Aligning the cpu work to chosen time-
 * points allows to tune certain characteristics of the video presentation,
 * like smoothness and latency.
 */

namespace pacer {

    class CommandQueue;
    class VulkanQueue;


    class FramePacer {

        using microseconds = std::chrono::microseconds;
        using high_resolution_clock = dxvk::high_resolution_clock;
        using time_point   = dxvk::high_resolution_clock::time_point;

    public:

        struct FrameInfo {
            uint64_t externalId;
            time_point start_t;
            int32_t renderStart;
            int32_t renderEnd;
        };

        FramePacer( Device* device, uint64_t firstFrameId );
        ~FramePacer();

        void sleep( uint64_t frameId, time_point lastSimulationStart ) {
            _INFO( "sleep - frameId: %" PRIu64 ", m_frameSync.cpuFinished: %"
                PRIu64 " m_frameSync.gpuFinished: %" PRIu64 " \n",
                frameId, m_frameSync.cpuFinished.load(), m_frameSync.gpuFinished.load() );

            // wait for finished rendering of a previous frame, typically the one before last
            uint64_t waitId = frameId-m_frameSync.m_waitLatency;
            if (!m_frameSync.gpuFinished.wait(waitId, 200)) {
                WARN( "timeout on waiting for gpu finish id %" PRIu64 " reached, resulting in stutter \n", waitId );
                return;
            }
            // potentially wait some more if the cpu gets too much ahead
            m_mode->startFrame(frameId, lastSimulationStart);
        }

        int64_t registerExternalId( uint64_t externalId, const FrameInfo& frameInfo ) {
            uint64_t cpuId = m_frameSync.cpuFinished + 1;

            uint64_t id = m_frameMapping.getFrameId(cpuId);
            if (id && id != externalId) {
                ERR( "Application reports different external-ids mapped to the same internal id %" PRIu64 "\n",
                    cpuId );
                return std::numeric_limits<int64_t>::max();
            }

            if (!id) {
                m_frameMapping.registerMapping( cpuId, externalId );
                LatencyMarkers* m = m_latencyMarkers.getMarkers(cpuId);
                m->start = frameInfo.start_t;
                m->renderStart = frameInfo.renderStart;
                m->renderEnd = frameInfo.renderEnd;
            }

            return cpuId - externalId;
        }

        void finishCpu() {
            uint64_t cpuId = 1 + m_frameSync.cpuFinished++;

            uint64_t newId = cpuId + 1;
            m_frameMapping.registerMapping( newId, 0 );
            LatencyMarkers* m = m_latencyMarkers.getMarkers( cpuId );
            LatencyMarkers* m_next = m_latencyMarkers.getMarkers( newId+1 );
            *m_next = LatencyMarkers {};

            _INFO( " reset markers for frame %" PRIu64 " \n", newId+1 );

            m->cpuFinished = high_resolution_clock::now();
        }

        void finishRender( uint64_t gpuDeviceTimestamp ) {
            // this is incredibly rare that finish render is triggered
            // for two consecutive frames from two threads simultaneously
            // but we've had that happen when logging was enabled
            std::lock_guard<dxvk::mutex> lock(m_finishMutex);

            uint64_t gpuId = 1 + m_frameSync.gpuFinished;
            LatencyMarkers* m = m_latencyMarkers.getMarkers( gpuId );
            time_point t = m_device->m_calibratedDeviceTimestamps.getHostTimestamp(gpuDeviceTimestamp);
            m->gpuFinished = t;

            uint64_t frameId = m_frameMapping.getFrameId( gpuId );
            m_frameSync.gpuFinished++;

            if (frameId == 0) {
                WARN( "frame with gpuId %" PRIu64 " not tracked correctly, ignoring. Missing marker? \n", gpuId );
                return;
            }

            int32_t latency = std::chrono::duration_cast<microseconds> (t - m->start ).count();
            _INFO( "latency = %" PRIi32 "\n", latency );
//            auto now = high_resolution_clock::now();
//            int32_t t_vs_now = std::chrono::duration_cast<microseconds> (t - now ).count();
//            INFO( "t_vs_now = %" PRIi32 "\n", t_vs_now );
//            INFO( "t = %" PRIu64 " \n", gpuDeviceTimestamp );
//            m_latencyAverage.push( m->gpuFinished );
            m_mode->finishRender( gpuId );
        }

        // todo: implement
        void notifyGpuPresentEnd( uint64_t frameId ) {
            // // the frame has been displayed to the screen
            // m_latencyMarkers.registerFrameEnd(frameId);
            // m_mode->endFrame(frameId);
            // m_frameSync.frameFinished.signal(frameId);
            //
            // trackStats(frameId);
        }

        FramePacerMode::Mode getMode() const {
            return m_mode->m_mode;
        }

        FramePacerMode* getFramePacerMode() {
            return m_mode.get();
        }

        void setFpsLimit( uint32_t minInterval ) {
            m_mode->setFpsLimit(minInterval);
        }

        LatencyMarkersStorage m_latencyMarkers;
        FrameMapping m_frameMapping;
        FrameSync m_frameSync;

    //

        int32_t getLatencyAverage() const
          { return m_latencyAverage.getAverage(); }

        JitterTotal getJitterStats() const
          { return m_jitterStats.getJitterTotal(); }

        const LatencyStats* getGpuBufferStats() const
          { return m_gpuBufferStats.load(); }

        const LatencyStats* getPresentStats() const
          { return m_presentationStats.load(); }

        std::atomic< bool > m_enabledGpuBufferTracking = { false };
        std::atomic< bool > m_enabledVSyncBufferTracking = { false };
        std::atomic< bool > m_enabledJitterTracking = { false };


    private:


        void trackStats( uint64_t frameId ) {
            using std::chrono::duration_cast;
            const LatencyMarkers* m_prev2 = m_latencyMarkers.getConstMarkers(frameId-2);
            const LatencyMarkers* m_prev = m_latencyMarkers.getConstMarkers(frameId-1);
            const LatencyMarkers* m = m_latencyMarkers.getConstMarkers(frameId);

            if (m_enabledJitterTracking && frameId > m_mode->getFirstFrameId()+2) {
                JitterEntry e;
                e.t = m->start;
                e.frametime  = std::abs( duration_cast<microseconds>( m->start - m_prev->start ).count()
                    - duration_cast<microseconds>( m_prev->start - m_prev2->start ).count() );
                e.frametime += std::abs( duration_cast<microseconds>( m->end - m_prev->end ).count()
                    - duration_cast<microseconds>( m_prev->end - m_prev2->end ).count() );
                e.frametime >>= 1;

//                e.latency = std::abs( m->gpuFinished - m_prev->gpuFinished );

                m_jitterStats.push( std::move(e) );
            }

            // will be re-enabled for VK_EXT_present_timing, but for now this isn't accurate enough
//            if (false && m_enabledVSyncBufferTracking) {
//                if (!m_presentationStats)
//                    m_presentationStats.store( new LatencyStats(3000) );
//                m_presentationStats.load()->push( m->end, m->presentFinished - m->gpuFinished );
//            }

            //    todo: adapt for vkd3d pacing code base
            //
            //      if (m_enabledGpuBufferTracking) {
            //        if (!m_gpuBufferStats)
            //          m_gpuBufferStats.store( new LatencyStats(3000) );
            //
            //        int64_t minDiff = std::numeric_limits<int64_t>::max();
            //        size_t i = 0;
            //        while (m->numAppSubmits > i && m->numGpuReadySubmits > i) {
            //          int64_t diff = std::chrono::duration_cast<microseconds>(
            //            m->gpuReady[i] - m->gpuAppSubmit[i]).count();
            //          diff = std::max( (int64_t) 0, diff );
            //          minDiff = std::min( minDiff, diff );
            //          ++i;
            //        }
            //
            //        if (minDiff != std::numeric_limits<int64_t>::max())
            //          m_gpuBufferStats.load()->push( m->end, minDiff );
            //      }
        }

        Device* m_device;
        std::unique_ptr<FramePacerMode> m_mode;

        dxvk::mutex m_finishMutex;

        std::atomic<LatencyStats*> m_gpuBufferStats = { nullptr };
        std::atomic<LatencyStats*> m_presentationStats = { nullptr };
        LatencyAverage m_latencyAverage;
        JitterStats m_jitterStats;

    };

}
