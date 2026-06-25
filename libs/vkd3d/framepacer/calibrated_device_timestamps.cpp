#include "calibrated_device_timestamps.h"
#include "device.h"
#include "util/util_log.h"
#include "util/util_likely.h"
#include <vector>

namespace pacer {

  CalibratedDeviceTimestamps::CalibratedDeviceTimestamps( Device* device )
  : m_device(device),
    m_enabled(false),
    m_timestampPeriod(device->m_properties.timestamp_period),
    m_canEnable( device->m_properties.khrCalibratedTimestamps ) {

    assert( m_canEnable );

    // if (!m_device->features().khrCalibratedTimestamps && !m_device->features().extCalibratedTimestamps) {
    //   Logger::warn( "Neither VK_KHR_calibrated_timestamps nor VK_EXT_calibrated_timestamps enabled. "
    //                 "Frame pacing will be suboptimal." );
    //   return;
    // }
    //
    // if (m_timestampValidBits != 64) {
    //   Logger::warn( str::format("Calibrated device timestamps are not enabled due to the device queue reporting ",
    //     m_timestampValidBits, " bit timestamps. Currently only implemented support for 64 bit timestamps. ",
    //     "Frame pacing will be suboptimal."));
    //   return;
    // }
    //
    // PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR fpGetPhysicalDeviceCalibrateableTimeDomains = nullptr;
    //
    // if (m_device->features().khrCalibratedTimestamps) {
    //   m_fpGetCalibratedTimestamps = m_device->vkd()->vkGetCalibratedTimestampsKHR;
    //   fpGetPhysicalDeviceCalibrateableTimeDomains = m_device->vki()->vkGetPhysicalDeviceCalibrateableTimeDomainsKHR;
    // }
    // else if (m_device->features().extCalibratedTimestamps) {
    //   m_fpGetCalibratedTimestamps = m_device->vkd()->vkGetCalibratedTimestampsEXT;
    //   fpGetPhysicalDeviceCalibrateableTimeDomains = m_device->vki()->vkGetPhysicalDeviceCalibrateableTimeDomainsEXT;
    // }
    //
    // uint32_t count;
    // fpGetPhysicalDeviceCalibrateableTimeDomains(m_device->adapter()->handle(), &count, nullptr);
    // std::vector<VkTimeDomainKHR> timeDomains( count );
    // fpGetPhysicalDeviceCalibrateableTimeDomains(m_device->adapter()->handle(), &count, timeDomains.data());
    // bool foundDeviceTimeDomain = false;
    // for (uint32_t i=0; i<count; ++i ) {
    //   foundDeviceTimeDomain |= timeDomains[i] == VK_TIME_DOMAIN_DEVICE_KHR;
    // }
    //
    // if (!foundDeviceTimeDomain)
    //   Logger::err( "VK_TIME_DOMAIN_DEVICE_KHR is not reported by vkGetPhysicalDeviceCalibrateableTimeDomains, "
    //     "possibly a Vulkan driver bug" );

    // calibrate();

  }


  CalibratedDeviceTimestamps::Calibration CalibratedDeviceTimestamps::calibrate() {

    if (!m_enabled)
      return Calibration {};

    Calibration nextCalibration;

    // we are only interested in the device "now" timestamp via Vulkan
    // since getting the host timestamp directly proved to be more reliable
    // possibly because of how the GPU driver interacts with Wine
    VkCalibratedTimestampInfoKHR calibratedTimestampInfo;
    calibratedTimestampInfo.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
    calibratedTimestampInfo.pNext = nullptr;
    calibratedTimestampInfo.timeDomain = VK_TIME_DOMAIN_DEVICE_KHR;

    VkResult res = m_device->m_vkProcs.vkGetCalibratedTimestampsKHR(
      m_device->m_properties.vk_device, 1,
      &calibratedTimestampInfo,
      &nextCalibration.deviceTimestamp,
      &nextCalibration.maxDeviation
    );

    nextCalibration.hostTimestamp = high_resolution_clock::now();

    if (unlikely(res != VK_SUCCESS)) {
      ERR( "Failed to calibrate timestamp \n" );
      return Calibration {};
    }

    // todo: make this thread-safe or remove the member variable
    m_calibration = nextCalibration;
    return nextCalibration;
  }


  CalibratedDeviceTimestamps::time_point CalibratedDeviceTimestamps::getHostTimestamp( uint64_t deviceTimestamp, const Calibration& calibration ) const {

    if (unlikely(calibration.deviceTimestamp == 0))
      return time_point{};

    int64_t deltaDeviceTicks = deviceTimestamp - calibration.deviceTimestamp;
    int64_t deltaDeviceNanoseconds = deltaDeviceTicks * m_timestampPeriod;

    return calibration.hostTimestamp + high_resolution_clock::nanoseconds( deltaDeviceNanoseconds );

  }


}
