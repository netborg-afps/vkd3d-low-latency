#pragma once

#include "framepacer_bridge.h"
#include <vulkan/vulkan_core.h>
#include "framepacer_mode.h"
#include "frame_sync.h"
#include "latency_markers.h"
#include "latency_stats.h"
#include "jitter_stats.h"
#include "calibrated_device_timestamps.h"
#include "util/sync/sync_ringbuffer_allocator.h"
#include "util/util_log.h"
#include <cinttypes>


/* \brief Frame pacer interface managing the CPU - GPU synchronization.
 *
 * GPUs render frames asynchronously to the game's and dxvk's CPU-side work
 * in order to improve fps-throughput. Aligning the cpu work to chosen time-
 * points allows to tune certain characteristics of the video presentation,
 * like smoothness and latency.
 */

struct PacerDevice;

namespace dxvk {


  class FramePacer {
    using microseconds = std::chrono::microseconds;
    using time_point   = high_resolution_clock::time_point;
  public:

    FramePacer( PacerDevice* device, uint64_t firstFrameId );
    ~FramePacer();

    void sleepAndBeginFrame( uint64_t frameId ) {
      LatencyMarkers* m = m_latencyMarkersStorage.getMarkers(frameId-1);
      m->appThreadFinished = std::chrono::duration_cast<microseconds> (
        high_resolution_clock::now() - m->start).count();

      // wait for finished rendering of a previous frame, typically the one before last
      m_frameSync.waitRenderFinished(frameId);
      // potentially wait some more if the cpu gets too much ahead
      m_mode->startFrame(frameId);
      m_latencyMarkersStorage.registerFrameStart(frameId);
    }

    void notifyGpuPresentEnd( uint64_t frameId ) {
      // the frame has been displayed to the screen
      m_latencyMarkersStorage.registerFrameEnd(frameId);
      m_mode->endFrame(frameId);
      m_frameSync.signalFrameFinished(frameId);
      trackStats(frameId);
    }

    void notifyCsRenderBegin( uint64_t frameId ) {
      auto now = high_resolution_clock::now();
      LatencyMarkers* m = m_latencyMarkersStorage.getMarkers(frameId);
      m->csStart = std::chrono::duration_cast<microseconds>(now - m->start).count();
    }

    void notifyCsRenderEnd( uint64_t frameId ) {
      auto now = high_resolution_clock::now();
      LatencyMarkers* m = m_latencyMarkersStorage.getMarkers(frameId);
      m->csFinished = std::chrono::duration_cast<microseconds>(now - m->start).count();
      m_frameSync.signalCsFinished( frameId );
    }

    void notifySubmit( uint64_t frameId ) {
      LatencyMarkers* m = m_latencyMarkersStorage.getMarkers(frameId);
      m->gpuSubmit.push_back(high_resolution_clock::now());
      submitCheckGpuFinished(frameId);
    }

    void notifyPresent( uint64_t frameId ) {
      // dx to vk translation is finished
      // in case of dx12 this is the dxgi.present
      if (frameId != 0) {
        auto now = high_resolution_clock::now();
        LatencyMarkers* m = m_latencyMarkersStorage.getMarkers(frameId);
        LatencyMarkers* next = m_latencyMarkersStorage.getMarkers(frameId+1);
        m->cpuFinished = std::chrono::duration_cast<microseconds>(now - m->start).count();
        next->gpuSubmit.clear();

        dxgiPresentCheckGpuFinished(frameId);
        m_latencyMarkersStorage.m_timeline.cpuFinished.store(frameId);
      }
    }

    void notifyQueueSubmit( uint64_t frameId ) {
      auto now = high_resolution_clock::now();
      LatencyMarkers* m = m_latencyMarkersStorage.getMarkers(frameId);
      m->gpuQueueSubmit.push_back(now);
      queueSubmitCheckGpuStart(frameId, m, now);
      m_mode->notifyQueueSubmit(frameId, now);
    }

    void notifyQueuePresentBegin( uint64_t frameId ) {
      if (frameId != 0) {
        auto now = high_resolution_clock::now();
        LatencyMarkers* m = m_latencyMarkersStorage.getMarkers(frameId);
        LatencyMarkers* next = m_latencyMarkersStorage.getMarkers(frameId+1);
        next->gpuQueueSubmit.clear();
        queueSubmitCheckGpuStart(frameId, m, now);
      }
    }

    void notifyGpuExecutionEnd( uint64_t frameId, VkQueryPool* queryPool ) {
      LatencyMarkers* m = m_latencyMarkersStorage.getMarkers(frameId);

      if (queryPool == nullptr) {
        auto now = high_resolution_clock::now();
        m->gpuReady.push_back(now);
        m_mode->notifyGpuReady(frameId, now);
        gpuExecutionCheckGpuFinished(frameId);
        return;
      }

      uint64_t timestamp;
      VkResult res = getSubmitQueryPoolResult( queryPool, &timestamp );

      if (unlikely(res != VK_SUCCESS)) {
        auto now = high_resolution_clock::now();
        m->gpuReady.push_back(now);
        m_mode->notifyGpuReady(frameId, now);
        m_queryPools.free(queryPool);
        gpuExecutionCheckGpuFinished(frameId);
        return;
      }

      auto t = m_calibratedDeviceTimestamps.getHostTimestamp(timestamp);
      if (unlikely(t == high_resolution_clock::time_point{}))
        t = high_resolution_clock::now();

      m->gpuReady.push_back(t);
      m_mode->notifyGpuReady(frameId, t);

      m_queryPools.free(queryPool);
      gpuExecutionCheckGpuFinished(frameId);
    }

    void finishRender( uint64_t frameId ) {
      // we get frameId == 0 for repeated presents (SyncInterval)
      assert( frameId > 0 );

      LatencyMarkers* m = m_latencyMarkersStorage.getMarkers(frameId);
      LatencyMarkers* next = m_latencyMarkersStorage.getMarkers(frameId+1);
      assert( !m->gpuReady.empty() );
      auto t = m->gpuReady.back();
      m->gpuFinished = std::chrono::duration_cast<microseconds>(t - m->start).count();
      next->gpuReady.clear();
      next->gpuReady.push_back(t);
      m_mode->notifyGpuReady(frameId+1, t);
      m_calibratedDeviceTimestamps.calibrate();

      gpuExecutionCheckGpuStart(frameId+1, next, t);

      m_latencyAverage.push(m->gpuFinished);
      m_latencyMarkersStorage.m_timeline.gpuFinished.store(frameId);
      m_mode->finishRender(frameId);
      m_frameSync.signalRenderFinished(frameId);

      GpuFinishedState initState = {};
      size_t initIndex = (frameId-1) % m_gpuFinishedState.size();
      m_gpuFinishedState[initIndex].store(initState);
      m_gpuStarts[ (frameId-1) % m_gpuStarts.size() ].store(0);

      size_t index = frameId % m_gpuFinishedState.size();
      INFO( "gpu finished frameId %" PRIu64 " has %" PRIu16 " submissions\n",
        frameId, m_gpuFinishedState[index].load().numGpuSubmits );
    }

    VkQueryPool* allocSubmitQueryPool()
    { return m_calibratedDeviceTimestamps.isEnabled() ? m_queryPools.alloc() : nullptr; }

    void freeSubmitQueryPool( VkQueryPool* queryPool )
    { m_queryPools.free( queryPool ); }

    FramePacerMode::Mode getMode() const {
      return m_mode->m_mode;
    }

    FramePacerMode* getFramePacerMode() {
      return m_mode.get();
    }

    void setTargetFrameRate( double frameRate ) {
      m_mode->setTargetFrameRate(frameRate);
    }

    LatencyMarkersStorage m_latencyMarkersStorage;
    FrameSync m_frameSync;


    //


    VkResult getSubmitQueryPoolResult( VkQueryPool* queryPool, uint64_t* timestamp ) { return VK_ERROR_UNKNOWN; }

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

    void signalGpuStart( uint64_t frameId, LatencyMarkers* m, const time_point& t ) {
      m->gpuStart = std::chrono::duration_cast<microseconds>(t - m->start).count();
      m_latencyMarkersStorage.m_timeline.gpuStart.store(frameId);
      m_frameSync.signalGpuStart(frameId);
    }

    void queueSubmitCheckGpuStart( uint64_t frameId, LatencyMarkers* m, const time_point& t ) {
      auto& gpuStart = m_gpuStarts[ frameId % m_gpuStarts.size() ];
      uint16_t val = gpuStart.fetch_or(queueSubmitBit);
      if (val == gpuReadyBit)
        signalGpuStart( frameId, m, t );
    }

    void gpuExecutionCheckGpuStart( uint64_t frameId, LatencyMarkers* m, const time_point& t ) {
      auto& gpuStart = m_gpuStarts[ frameId % m_gpuStarts.size() ];
      uint16_t val = gpuStart.fetch_or(gpuReadyBit);
      if (val == queueSubmitBit)
        signalGpuStart( frameId, m, t );
    }

    void submitCheckGpuFinished( uint64_t frameId ) {
        uint32_t index = frameId % m_gpuFinishedState.size();
        GpuFinishedState expected = m_gpuFinishedState[ index ].load();
        GpuFinishedState desiredState;

        do {
            desiredState = expected;
            desiredState.numSubmits++;
        } while ( !m_gpuFinishedState[index].compare_exchange_weak( expected, desiredState ) );
    }

    void dxgiPresentCheckGpuFinished( uint64_t frameId ) {
      uint32_t index = frameId % m_gpuFinishedState.size();
      GpuFinishedState expected = m_gpuFinishedState[ index ].load();
      GpuFinishedState desiredState;

      do {
        desiredState = expected;
        desiredState.presentIssued = true;
      } while ( !m_gpuFinishedState[ index ].compare_exchange_weak( expected, desiredState ) );

      if (desiredState.numSubmits == desiredState.numGpuSubmits && desiredState.presentIssued)
        finishRender(frameId);
    }

    void gpuExecutionCheckGpuFinished( uint64_t frameId ) {
      uint32_t index = frameId % m_gpuFinishedState.size();
      GpuFinishedState expected = m_gpuFinishedState[ index ].load();
      GpuFinishedState desiredState;

      do {
        desiredState = expected;
        desiredState.numGpuSubmits++;
      } while ( !m_gpuFinishedState[ index ].compare_exchange_weak( expected, desiredState ) );

      if (desiredState.numSubmits == desiredState.numGpuSubmits && desiredState.presentIssued)
        finishRender(frameId);
    }

    void trackStats( uint64_t frameId ) {
      using std::chrono::duration_cast;
      const LatencyMarkers* m_prev2 = m_latencyMarkersStorage.getConstMarkers(frameId-2);
      const LatencyMarkers* m_prev = m_latencyMarkersStorage.getConstMarkers(frameId-1);
      const LatencyMarkers* m = m_latencyMarkersStorage.getConstMarkers(frameId);

      if (m_enabledJitterTracking && frameId > m_mode->getFirstFrameId()+2) {
        JitterEntry e;
        e.t = m->start;
        e.frametime  = std::abs( duration_cast<microseconds>( m->start - m_prev->start ).count()
                               - duration_cast<microseconds>( m_prev->start - m_prev2->start ).count() );
        e.frametime += std::abs( duration_cast<microseconds>( m->end - m_prev->end ).count()
                               - duration_cast<microseconds>( m_prev->end - m_prev2->end ).count() );
        e.frametime >>= 1;

        e.latency = std::abs( m->gpuFinished - m_prev->gpuFinished );
        e.appThreadLatency = std::abs( m->appThreadFinished - m_prev->appThreadFinished );

        m_jitterStats.push( std::move(e) );
      }

      // will be re-enabled for VK_EXT_present_timing, but for now this isn't accurate enough
      if (false && m_enabledVSyncBufferTracking) {
        if (!m_presentationStats)
          m_presentationStats.store( new LatencyStats(3000) );
        m_presentationStats.load()->push( m->end, m->presentFinished - m->gpuFinished );
      }

      if (m_enabledGpuBufferTracking) {
        if (!m_gpuBufferStats)
          m_gpuBufferStats.store( new LatencyStats(3000) );

        int64_t minDiff = std::numeric_limits<int64_t>::max();
        size_t i = 0;
        while (m->gpuSubmit.size() > i && m->gpuReady.size() > i) {
          int64_t diff = std::chrono::duration_cast<microseconds>(
            m->gpuReady[i] - m->gpuSubmit[i]).count();
          diff = std::max( (int64_t) 0, diff );
          minDiff = std::min( minDiff, diff );
          ++i;
        }

        if (minDiff != std::numeric_limits<int64_t>::max())
          m_gpuBufferStats.load()->push( m->end, minDiff );
      }
    }


    struct alignas(8) GpuFinishedState {
      uint16_t numSubmits    = { 0 };
      uint16_t numGpuSubmits = { 0 };
      bool presentIssued     = { false };
      uint8_t __padding[3]   = { 0 };
    };

    PacerDevice m_device;
    std::unique_ptr<FramePacerMode> m_mode;

    std::array< std::atomic<GpuFinishedState>, 256 > m_gpuFinishedState = { };
    std::array< std::atomic< uint16_t >, 256 > m_gpuStarts = { };
    static constexpr uint16_t queueSubmitBit = 1;
    static constexpr uint16_t gpuReadyBit    = 2;

    std::atomic<LatencyStats*> m_gpuBufferStats = { nullptr };
    std::atomic<LatencyStats*> m_presentationStats = { nullptr };
    LatencyAverage m_latencyAverage;
    JitterStats m_jitterStats;

    CalibratedDeviceTimestamps m_calibratedDeviceTimestamps;
    sync::RingbufferAllocator<VkQueryPool, 256> m_queryPools;

  };

}
