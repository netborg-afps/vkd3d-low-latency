#pragma once

#include "framepacer_mode.h"
#include "util/util_sleep.h"

namespace pacer {

  /*
   * Minimal latency is achieved here by waiting for the previous
   * frame to complete, which results in very much reduced fps.
   * Generally not recommended, but helpful to get insights to fine-tune
   * the low-latency mode, and possibly is useful for running games
   * in the cpu limit.
   */

  class MinLatencyMode : public FramePacerMode {

  public:

    MinLatencyMode(Mode mode, LatencyMarkersStorage* storage, FrameSync* frameSync, uint64_t firstFrameId)
    : FramePacerMode(mode, "min-latency", storage, frameSync, firstFrameId) {}

    ~MinLatencyMode() {}

    void startFrame( uint64_t frameId, dxvk::high_resolution_clock::time_point ) override {

      dxvk::Sleep::TimePoint now = dxvk::high_resolution_clock::now();
      int32_t frametime = std::chrono::duration_cast<std::chrono::microseconds>(
        now - m_lastStart ).count();
      int32_t frametimeDiff = std::max( 0, m_fpsLimitFrametime.load() - frametime );
      int32_t delay = std::max( 0, frametimeDiff );
      int32_t maxDelay = std::max( m_fpsLimitFrametime.load(), 20000 );
      delay = std::min( delay, maxDelay );

      dxvk::Sleep::TimePoint nextStart = now + std::chrono::microseconds(delay);
      dxvk::Sleep::sleepUntil( now, nextStart );
      m_lastStart = nextStart;

    }

  private:

    dxvk::Sleep::TimePoint m_lastStart = { dxvk::high_resolution_clock::now() };

  };

}
