#pragma once

#include "framepacer_bridge.h"
#include "util/util_time.h"

namespace pacer {

  class Device;

  /*
   * Clock synchronization for GPU/CPU via the VK_KHR_calibrated_timestamps
   * (fallback VK_EXT_calibrated_timestamps) device extension. The clocks need
   * to get calibrated regularly, for example once every frame, to account for
   * clock drift.
   *
   * Assumes 64 bit timestamps are supported for now, as lower bit timestamps
   * would need overflow checks and are impractical since 32 bit timestamps
   * would wrap back to zero every 4 seconds at nanosecond precision.
   *
   * Intended to be used within a single thread, not thread-safe.
   */

  class CalibratedDeviceTimestamps {
  public:
    using time_point = dxvk::high_resolution_clock::time_point;
    using high_resolution_clock = dxvk::high_resolution_clock;

    CalibratedDeviceTimestamps( Device* device );
    ~CalibratedDeviceTimestamps() { }

    struct Calibration {
      using time_point = dxvk::high_resolution_clock::time_point;
      uint64_t deviceTimestamp  = { 0 };
      uint64_t maxDeviation     = { 0 };
      time_point hostTimestamp  = { time_point{} };
    };

    void enable() { m_enabled = m_canEnable; }
    bool canEnable() const { return m_canEnable; }
    bool isEnabled() const { return m_enabled; }

    Calibration calibrate();
    time_point getHostTimestamp( uint64_t deviceTimestamp, const Calibration& calibration) const;
    time_point getHostTimestamp( uint64_t deviceTimestamp ) const
        { return getHostTimestamp(deviceTimestamp, m_calibration); }

  private:

    Device*       m_device;
    Calibration   m_calibration;
    bool          m_enabled;

    // PFN_vkGetCalibratedTimestampsKHR m_fpGetCalibratedTimestamps = nullptr;

    const float    m_timestampPeriod;
    const bool     m_canEnable;

  };

}
