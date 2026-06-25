#pragma once

#include "framepacer_mode.h"
#include "submit_iterator.h"
#include "threaded_sleep.h"
#include "util/util_sleep.h"
#include "util/util_log.h"
#include <algorithm>

namespace pacer {

  /*
   * This low-latency mode aims to reduce latency with minimal impact in fps.
   * Effective when operating in the GPU-limit. Efficient to be used in the CPU-limit as well.
   *
   * Greatly reduces input lag variations when switching between CPU- and GPU-limit, and
   * compared to the max-frame-latency approach, it has a much more stable input lag when
   * GPU running times change dramatically, which can happen for example when rotating within a scene.
   *
   * The current implementation rather generates fluctuations alternating frame-by-frame
   * depending on the game's and dxvk's CPU-time variations. This might be visible as a loss
   * in smoothness in games which provide strongly varying frame times. Most games should be fine,
   * but for example in 'God of War', we measured a prediction error of +/- 3 ms for the
   * 99% percentiles and up to +/- 2 ms for the 95% percentiles, which is a bit too much.
   * Smoothing may be a consideration in such a case, but unsuitable smoothing will degrade input feel,
   * so it's not implemented for now, but more advanced smoothing techniques will be investigated
   * in the future.
   *
   * In some situations however, this low-latency pacing actually presents smoother visuals
   * than max-frame-latency, because when the prediction error is small, this jitter effect
   * gets negligible, and it becomes apparent that the generated video progresses more cleanly
   * in time with regards to medium-term time consistency. In other words, the video playback speed
   * is more accurate and steady - for the same reasons why input lag consistency is improved.
   *
   * Fps limiting is tightly integrated into the frame pacing logic and is highly recommended
   * to be used in place of most ingame limiters.
   *
   * A VRR mode is also provided to combine low latency with image clarity. It's achieved by
   * predictively limiting at the present timeline, respecting (implicitly derived) v-blanks to
   * avoid going into V-Sync buffering. This can further be tuned by using the fps limiter at the
   * same time, which means there are basically two limiters active. The advantage of doing so
   * is that the "normal" fps limiter isn't affected by prediction errors. Selecting a VRR refresh
   * rate smaller than the monitor's refresh rate will lower the chance and/or lower the duration
   * frames will go into V-Sync buffering.
   *
   * It further can be fine-tuned via the dxvk.lowLatencyOffset and
   * dxvk.lowLatencyAllowCpuFramesOverlap config variables.
   * Compared to maxFrameLatency = 3, render-latency reductions of up to 67% are achieved.
   */

    class LowLatencyMode : public FramePacerMode {

        using microseconds = std::chrono::microseconds;
        using time_point = dxvk::high_resolution_clock::time_point;
        using high_resolution_clock = dxvk::high_resolution_clock;

    public:

        LowLatencyMode(Mode mode, LatencyMarkersStorage* storage, FrameSync* frameSync, uint64_t firstFrameId, Device* device, int refreshRate = 0)
        : FramePacerMode(mode, mode == LOW_LATENCY ? "low-latency" : "low-latency-vrr", storage, frameSync, firstFrameId),
            m_lowLatencyOffset(0), m_device(device) {
            // Logger::info( str::format("  lowLatencyOffset: ", m_lowLatencyOffset) );

            if (refreshRate > 0) {
                m_vrrRefreshInterval = 1'000'000 / refreshRate;
                // Logger::info( str::format("  vrr refresh rate: ", refreshRate) );
            }

        }

        ~LowLatencyMode() {}

        void startFrame( uint64_t frameId, time_point lastStart ) override {

            using std::chrono::duration_cast;

            uint64_t gpuFinishedId = m_frameSync->gpuFinished.load();
            if (gpuFinishedId <= m_firstFrameId+2) // do we still need this?
                return;

            InFlight inFlight;
            inFlight.numInFlight = frameId - gpuFinishedId;
            inFlight.earliestFrameId = gpuFinishedId+1;
            inFlight.m = m_latencyMarkers->getConstMarkers(frameId-1);
            inFlight.m_prev = m_latencyMarkers->getConstMarkers(frameId-2);
            inFlight.m_prev_prev = m_latencyMarkers->getConstMarkers(frameId-3);

            _INFO( "num inflight frames : %" PRIu16 " \n", inFlight.numInFlight );

            const LatencyMarkers*& m = inFlight.m;
            time_point now = dxvk::high_resolution_clock::now();

            if (m->start != time_point {})
                lastStart = m->start;

            if (inFlight.numInFlight == 1) {
                int32_t delay = getFpsLimiterDelay( lastStart, now );
                sleepFor( now, delay );
                _INFO( "we are the only in-flight frame, no sleep \n" );
                return;
            }

            SyncProps props = getSyncPrediction();

            now = dxvk::high_resolution_clock::now();
            int32_t cpuDelay = getCpuDelay( props, inFlight, now, lastStart );
            int32_t gpuDelay = getGpuDelay( props, inFlight, now, lastStart );
            int32_t delay = std::max( cpuDelay, getFpsLimiterDelay( lastStart, now ) );
            delay = std::max( delay, gpuDelay );
//            delay = std::max( delay, getVrrDelay( frameId, props, now, lastFrameFinishPrediction ) );
            _INFO (" sleeping for %" PRIi32 " us \n", delay );
            sleepFor( now, delay );

        }


//        void notifyGpuReady( uint64_t frameId, time_point t ) override
//          { m_gpuProgress.notifyGpuReady( frameId, t ); }
//
//        void notifyQueueSubmit( uint64_t frameId, time_point t ) override
//          { m_gpuProgress.notifyQueueSubmit( frameId, t ); }


        void finishRender( uint64_t frameId ) override {

            using std::chrono::duration_cast;

            if (frameId < 2)
                return;

            const LatencyMarkers* m = m_latencyMarkers->getConstMarkers(frameId);
            const LatencyMarkers* m_prev = m_latencyMarkers->getConstMarkers(frameId-1);

            // estimates the optimal overlap for cpu/gpu work by optimizing gpu scheduling first
            // such that the gpu doesn't go into idle for this frame, and then aligning cpu submits
            // where gpuSubmit[i] <= gpuRun[i] for all i

            std::vector< CommandQueue* >& commandQueues = m_tempCommandQueues;
            std::vector< SubmitIterator >& submitIters = m_tempSubmitIters;
            submitIters.clear();
            m_device->getCommandQueues(commandQueues); // we're doing a clear() here
            for (CommandQueue* q : commandQueues) {
                SubmitIterator iter( m_prev->cpuFinished, m->cpuFinished, q );
                if (!iter.isAtEnd())
                    submitIters.push_back(iter);
            }

            int32_t cpuFinished = duration_cast<microseconds>(m->cpuFinished - m->start).count();
            _INFO( "m->cpuFinished: %" PRIi32 " \n", cpuFinished );

            uint32_t size = submitIters.size();
            _INFO( "num submit-iters: %" PRIu32 " \n", size );

            CalibratedDeviceTimestamps::Calibration calibration = m_device->m_calibratedDeviceTimestamps.calibrate();

            std::vector< Submit >& submits = m_tempSubmits;
            submits.clear();
            for (SubmitIterator& iter : submitIters) {
                while (!iter.isAtEnd()) {
                    uint64_t gpuTimestampStart = iter.getVulkanGpuExecutionStart();
                    uint64_t gpuTimestamp = iter.getVulkanGpuExecutionEnd();
                    if (!gpuTimestamp || !gpuTimestampStart) { // frame-overlapping submit
                        ++iter;
                        continue;
                    }
                    Submit s;
                    s.command_t = iter.getAppSubmit();
                    s.vulkan_t  = iter.getVulkanSubmit();
                    s.gpuStart_t = m_device->m_calibratedDeviceTimestamps.getHostTimestamp(
                        gpuTimestampStart, calibration);
                    s.gpuFinish_t = m_device->m_calibratedDeviceTimestamps.getHostTimestamp(
                        gpuTimestamp, calibration);
                    s.vulkanQueueId = iter.getVulkanQueueId();
                    if (unlikely(s.vulkanQueueId > 63)) {
                        ERR( "more than 63 Vulkan queues is absolutely not expected here, cannot perform frame pacing! \n" );
                        return;
                    }
                    submits.push_back(s);
                    ++iter;
                }
            }

            int32_t numSubmits = (int32_t) submits.size();
            if (numSubmits == 0)
                return;

            _INFO( " NUMBER OF SUBMITS : %" PRIi32 " \n", numSubmits );
            std::sort( submits.begin(), submits.end() );

            int32_t excessLatency = std::numeric_limits<int32_t>::max();
            for (Submit& s : submits) {
                int32_t el = duration_cast<microseconds>(s.gpuStart_t - s.command_t).count();
                excessLatency = std::min( el, excessLatency );
            }

            _INFO( "excessLatency: %" PRIi32 " \n", excessLatency );

            // todo: think about potentially frame-independent submits we may have missed
            //       we do filter out submits which end after frame-finished, possibly that's all we need

            std::vector< GpuSubmit >& gpuSubmits = m_tempGpuSubmits;
            gpuSubmits.clear();
            for (size_t i=0; i<submits.size(); ++i) {
                Submit& submit = submits[i];
                GpuSubmit s1;
                s1.t = submit.gpuStart_t;
                s1.submitId = i;
                s1.id = submit.vulkanQueueId;
                s1.type = GpuSubmit::START;

                GpuSubmit s2;
                s2.t = submit.gpuFinish_t;
                s2.submitId = i;
                s2.id = submit.vulkanQueueId;
                s2.type = GpuSubmit::END;

                gpuSubmits.push_back(s1);
                gpuSubmits.push_back(s2);
            }

            if (gpuSubmits.empty())
                return;

            std::sort( gpuSubmits.begin(), gpuSubmits.end() );

            uint64_t gpuState = 0;
            int16_t queueState[64] = {};
            time_point lastTimestamp = gpuSubmits[0].t;
            int64_t gpuTime = 0;

            for (GpuSubmit& s : gpuSubmits) {
                int64_t diff = duration_cast<microseconds> (s.t - lastTimestamp).count();
                lastTimestamp = s.t;
                if (gpuState)
                    gpuTime += diff;

                if (s.type == GpuSubmit::START) {
                    submits[s.submitId].gpuRun = gpuTime;
                    ++queueState[s.id];
                    gpuState |= 1ULL << s.id;
                } else { // == GpuSubmit::END
                    if (--queueState[s.id] == 0)
                        gpuState &= ~(1ULL << s.id);
                    assert( queueState[s.id] >= 0 );
                }
            }

            int32_t alignment = duration_cast<microseconds>( submits[numSubmits-1].command_t - submits[0].command_t ).count()
                - submits[numSubmits-1].gpuRun;

            int32_t offset = 0;
            for (int i=numSubmits-2; i>=0; --i) {
                int32_t curSubmit = duration_cast<microseconds>( submits[i].command_t - submits[0].command_t ).count();
                int32_t diff = curSubmit - submits[i].gpuRun - alignment;
                diff = std::max( 0, diff );
                offset += diff;
                alignment += diff;
            }

            SyncProps& props = m_props[frameId % m_props.size()];
            props.gpuSync = submits[numSubmits-1].gpuRun;
            props.cpuUntilGpuSync = offset + duration_cast<microseconds>( submits[numSubmits-1].command_t - m->start ).count();
            props.cpuUntilGpuStart = props.cpuUntilGpuSync - props.gpuSync;
            props.optimizedGpuTime = gpuTime;
            props.isOutlier = isOutlier(frameId);
            props.renderStart = m->renderStart;
            props.renderEnd = m->renderEnd;

            _INFO( "props.gpuSync = %" PRIi32 " \n", props.gpuSync );
            _INFO( "props.cpuUntilGpuStart = %" PRIi32 " \n", props.cpuUntilGpuStart );
            _INFO( "props.optimizedGpuTime = %" PRIi32 " \n", props.optimizedGpuTime );
            _INFO( "offset = %" PRIi32 " \n", offset );
            _INFO( "props.isOutlier = %d \n", props.isOutlier );

//            if (m_mode == LOW_LATENCY_VRR) {
//                // implicitly derive v-blank timings
//                const LatencyMarkers* mPrev = m_latencyMarkers->getConstMarkers(frameId-1);
//                int32_t frametime = duration_cast<microseconds>(
//                      (m->start + microseconds(m->gpuFinished))
//                    - (mPrev->start + microseconds(mPrev->gpuFinished) )).count();
//
//                int32_t curVSyncBuffer = m_vrrVSyncBuffer.load();
//                int32_t diff = (m_vrrRefreshInterval + curVSyncBuffer) - frametime;
//
//                int32_t newVSyncBuffer = std::max( 0, diff );
//                m_vrrVSyncBuffer.store( newVSyncBuffer );
//            }

            m_propsFinished.store( frameId );

        }


        void endFrame( uint64_t frameId ) override { }



    private:

        struct SyncProps {
            int32_t optimizedGpuTime;   // gpu executing packed submits in one go
            int32_t gpuSync;            // gpuStart to this sync point, in microseconds
            int32_t cpuUntilGpuSync;
            int32_t cpuUntilGpuStart;
            int32_t renderStart;
            int32_t renderEnd;
            bool    isOutlier;
        };

        struct InFlight {
            uint16_t numInFlight = { 1 };
            uint64_t earliestFrameId = { 0 };
            const LatencyMarkers* m = { nullptr };
            const LatencyMarkers* m_prev = { nullptr };
            const LatencyMarkers* m_prev_prev = { nullptr };
        };

        SyncProps getSyncPrediction() const {
            // In the future we might use more samples to get a prediction.
            // Possibly this will be optional, as until now, basing it on
            // just the previous frame gave us the best mouse input feel.
            // Simple averaging or median filtering is surely not the way
            // to go, but more advanced methods will be investigated.
            // Outlier removal has worked out really well though, so that's
            // what we are using here.

            SyncProps res = {};
            uint64_t id = m_propsFinished;
            if (id < m_firstFrameId+7)
                return res;

            for (size_t i=0; i<7; ++i) {
                const SyncProps& props = m_props[ (id-i) % m_props.size() ];
                if (!props.isOutlier) {
                    id = id-i;
                    break;
                }
            }

            return m_props[ id % m_props.size() ];

        };


        bool isOutlier( uint64_t frameId ) const {

            constexpr int32_t numLoop = 7;
            int32_t totalCpuTime = 0;
            for (int32_t i=1; i<numLoop; ++i) {
                const SyncProps& props = m_props[ (frameId-i) % m_props.size() ];
                totalCpuTime += props.cpuUntilGpuStart;
            }

            int32_t avgCpuTime = totalCpuTime / (numLoop-1);
            const SyncProps& props = m_props[ frameId % m_props.size() ];
            if (props.cpuUntilGpuStart > 1.3*avgCpuTime)
                return true;

            return false;

        }


        int32_t getGpuDelay( const SyncProps& props, const InFlight& inFlight, time_point now, time_point lastStart ) const {

            // should not happen
            if (inFlight.numInFlight == 2 && inFlight.m_prev->gpuFinished == time_point{}) {
                ERR( "m_prev->gpuFinished == time_point{} \n " );
                return 0;
            }

            if (inFlight.numInFlight == 3 && inFlight.m_prev_prev->gpuFinished == time_point{}) {
                ERR( "m_prev_prev->gpuFinished == time_point{} \n " );
                return 0;
            }

            int32_t lastFrameFinish = std::chrono::duration_cast<microseconds>( inFlight.m_prev->gpuFinished - now ).count();
            int32_t gpuReadyPrediction = lastFrameFinish + props.optimizedGpuTime;

            if (inFlight.numInFlight == 3) {
                lastFrameFinish = std::chrono::duration_cast<microseconds>( inFlight.m_prev_prev->gpuFinished - now ).count();
                gpuReadyPrediction = lastFrameFinish + props.optimizedGpuTime + props.optimizedGpuTime;
            }

            int32_t gpuDelay = gpuReadyPrediction - props.cpuUntilGpuStart;
            return gpuDelay + m_lowLatencyOffset;

        }


        int32_t getCpuDelay( const SyncProps& props, const InFlight& inFlight, time_point now, time_point lastStart ) const {

            // todo: create our own markers, we don't have to rely on NvAPI markers
            if (props.renderStart == 0 || props.renderEnd == 0) {
                _INFO( "render markers incomplete, skipping cpuDelay \n" );
                return 0;
            }

            // in case the rendering is threaded in its own loop, make sure we don't
            // run into buffering here which would create additional latency.
            int32_t cpuReadyPrediction = std::chrono::duration_cast<microseconds>(
                lastStart + microseconds(props.renderEnd) - now).count();
            int32_t cpuDelay = cpuReadyPrediction - props.renderStart;
            _INFO( "cpuDelay = %" PRIi32 " \n", cpuDelay );
            return cpuDelay + m_lowLatencyOffset;

            // todo: may be able to give a better prediction for numInFlight == 3
        }


        int32_t getFpsLimiterDelay( time_point lastStart, time_point now ) const {

            int32_t frametime = std::chrono::duration_cast<microseconds>( now - lastStart ).count();
            return std::max( 0, m_fpsLimitFrametime.load() - frametime );

        }


        int32_t getVrrDelay( uint64_t frameId, const SyncProps& props, const time_point& now, const time_point& lastFrameFinishPrediction = time_point{} ) {

            // todo
            return 0;

//            if (m_mode != LOW_LATENCY_VRR)
//                return 0;
//
//            uint64_t renderFinishedId = m_frameSync->gpuFinished.load();
//            const LatencyMarkers* m = m_latencyMarkers->getConstMarkers(renderFinishedId);
//            int32_t lastVBlank = std::chrono::duration_cast<microseconds> (
//                m->start + microseconds(m->gpuFinished) - now + microseconds(m_vrrVSyncBuffer.load())).count();
//
//            int32_t targetVBlank = lastVBlank + (frameId-renderFinishedId) * m_vrrRefreshInterval;
//
//            // set last v-blank if we have more information about the last frame
//            if (renderFinishedId == frameId-2 && lastFrameFinishPrediction != time_point{} ) {
//                int32_t vBlank = std::chrono::duration_cast<microseconds> (
//                lastFrameFinishPrediction - now).count() + m_vrrVSyncBuffer.load();
//                lastVBlank += m_vrrRefreshInterval;
//                lastVBlank = std::max( lastVBlank, vBlank );
//                targetVBlank = lastVBlank + m_vrrRefreshInterval;
//            }
//
//            int32_t expectedFrameLatency = props.cpuUntilGpuStart + props.optimizedGpuTime;
//            return targetVBlank - expectedFrameLatency;

        }


        void sleepFor( const dxvk::Sleep::TimePoint t, int32_t delay ) {

            if (delay <= 0)
                return;

            int32_t maxDelay = std::max( m_fpsLimitFrametime.load(), 20000 );
            delay = std::min( delay, maxDelay );

            dxvk::Sleep::TimePoint t2 = t + microseconds(delay);
            m_threadedSleep.sleepUntil(t2);

        }


        // int32_t getLowLatencyOffset( const DxvkOptions& options );

        const int32_t m_lowLatencyOffset;
        Device* m_device;

        int32_t m_vrrRefreshInterval = { 0 };
        std::atomic<int32_t> m_vrrVSyncBuffer = { 0 };

        std::array<SyncProps, 16> m_props = { };
        std::atomic<uint64_t> m_propsFinished = { 0 };

        struct GpuSubmit {

            using time_point = dxvk::high_resolution_clock::time_point;
            enum type_t : uint8_t {
                START = 0,
                END = 1
            };

            time_point t;
            uint16_t submitId;
            uint8_t id;
            type_t type;

            bool operator<( const GpuSubmit& b ) const {
                if (this->t != b.t)
                    return this->t < b.t;
                return this->type < b.type;
            }

        };

        struct Submit {

            time_point command_t;
            time_point vulkan_t;
            time_point gpuStart_t;
            time_point gpuFinish_t;
            uint16_t vulkanQueueId;

            int32_t gpuRun; // filled out at a later stage
            bool operator<( const Submit& b ) const {
                return this->command_t < b.command_t;
            }

        };

        std::vector< Submit > m_tempSubmits;
        std::vector< SubmitIterator > m_tempSubmitIters;
        std::vector< CommandQueue* > m_tempCommandQueues;
        std::vector< GpuSubmit > m_tempGpuSubmits;

        ThreadedSleep m_threadedSleep;

    };

}
