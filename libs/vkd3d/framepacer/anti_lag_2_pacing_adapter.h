#pragma once

#include "framepacer.h"
#include "device.h"
#include "frameid_mapping.h"
#include "util/util_log.h"

#include <winnt.h>
#include <winerror.h>

extern "C" {
#include "vkd3d_device_vkd3d_ext.h"
}

namespace pacer {

    class AMD_AntiLag2_PacingAdapter {
        using time_point = dxvk::high_resolution_clock::time_point;

    public:

        AMD_AntiLag2_PacingAdapter( Device* device )
            : m_device(device) {}

        ~AMD_AntiLag2_PacingAdapter() { }

        HRESULT updateAntiLagState( void* pData ) {

            const AmdAntiLagAPIData_v1 *v1 = static_cast<const AmdAntiLagAPIData_v1*> (pData);
            const AmdAntiLagAPIData_v2 *v2 = static_cast<const AmdAntiLagAPIData_v2*> (pData);

            if (!pData) {
                _INFO( "updateAntiLagState with pData == NULL \n" );
                // probably this isn't called in parallel, inherit atomic logic from NvAPI pacing adapter

                if (m_device->m_activeType.load(std::memory_order_acquire) != Device::AMD_AntiLag2)
                    return S_OK;

                uint64_t frameId;
                if (m_pendingFrameId.load(std::memory_order_acquire)
                    && (frameId = m_pendingFrameId.exchange(0, std::memory_order_acq_rel))) {

                    auto now = dxvk::high_resolution_clock::now();
                    uint64_t pacerId = m_mapping.registerNvId( frameId, now );

                    m_device->m_pacer->sleep(
                        pacerId + m_drift.load( std::memory_order_acquire ),
                        m_lastEndSleep.load( std::memory_order_acquire ));

                    now = dxvk::high_resolution_clock::now();
                    m_lastEndSleep.store( now, std::memory_order_release );
                }

                return S_OK;
            }

            if (v1->uiVersion == 1) {
                bool enable = (v1->eMode == 1);
                _INFO( "updateAntiLagState v1 config: enable = %d, maxFPS = %u \n", enable, v1->maxFPS );

                m_device->m_activeType.store(
                    enable ? Device::AMD_AntiLag2 : Device::WaitableDXGISwapchain,
                    std::memory_order_release );

                if (enable) {
                    if (!v1->maxFPS)
                        m_device->m_pacer->setFpsLimit(0);
                    else {
                        uint32_t minInterval = 1'000'000 / v1->maxFPS;
                        m_device->m_pacer->setFpsLimit(minInterval);
                    }
                }

                return S_OK;
            }

            if (v2->uiVersion == 2) {
                if (m_device->m_activeType.load(std::memory_order_acquire) != Device::AMD_AntiLag2)
                    return S_OK;

                if (v2->flags.signalGetUserInputIdx) {
                    _INFO( "AMD Anti-Lag 2 Frame Start id %" PRIu64 "\n", v2->iiFrameIdx );
                    m_pendingFrameId.store( v2->iiFrameIdx, std::memory_order_release );
                    return S_OK;
                }
                if (v2->flags.signalEndOfFrameIdx && !v2->flags.isInterpolatedFrame) {
                    FramePacer::FrameInfo frameInfo = m_mapping.getFrameInfo( v2->iiFrameIdx );
                    const uint64_t& pacerId = frameInfo.externalId;
                    if (pacerId != FrameIdMap::INVALID_PACER_ID) {
                        int64_t drift = m_device->m_pacer->registerExternalId( pacerId, frameInfo );
                        _INFO( " present resolve: \n"
                               "   pacerId = %" PRIu64 " \n"
                               "   drift = %" PRIi64 " \n"
                               "   projected cpuId = %" PRIu64 " \n",
                               pacerId, drift, pacerId + drift );
                        if (drift != std::numeric_limits<int64_t>::max())
                            m_drift.store( drift, std::memory_order_release );
                    }
                }
                if (v2->flags.signalEndOfFrameIdx && v2->flags.isInterpolatedFrame) {
                    WARN( "framegen currently not supported! \n" );
                }
                return S_OK;
            }

            ERR( "updateAntiLagState with invalid uiVersion %u.\n", v1->uiVersion);
            return E_INVALIDARG;
        }

    private:

        Device* m_device;

        std::atomic<uint64_t> m_pendingFrameId = { 0 };
        FrameIdMap m_mapping;

        std::atomic<time_point> m_lastEndSleep = { };
        std::atomic<int64_t> m_drift = { 0 };

    };

}
