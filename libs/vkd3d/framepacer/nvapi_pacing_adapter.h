#pragma once

#include "framepacer.h"
#include "device.h"
#include "frameid_mapping.h"
#include "util/util_log.h"


namespace pacer {

    class NvApi_PacingAdapter {
        using time_point = dxvk::high_resolution_clock::time_point;

    public:
        NvApi_PacingAdapter( Device* device )
        : m_device(device) {}

        ~NvApi_PacingAdapter() {}

        void sleepAndBeginFrame() {
            // only sleep once before seeing a simulation marker
            uint64_t pacerId = m_pacerState.simulation.load( std::memory_order_acquire ) + 1;

            if (!m_pendingSleep.load( std::memory_order_acquire )) {
                _INFO( "sleeping for pacerId %" PRIu64 " \n", pacerId );
                m_device->m_pacer->sleep(
                    pacerId + m_pacerState.drift.load( std::memory_order_acquire ),
                    m_lastEndSleep.load( std::memory_order_acquire ));
                m_pendingSleep.store( true, std::memory_order_release );
            }

            auto now = dxvk::high_resolution_clock::now();
            m_lastEndSleep.store( now, std::memory_order_release );
        }

        void setLatencyMarker( uint64_t nvId, VkLatencyMarkerNV marker ) {
            if (m_device->m_activeType.load( std::memory_order_relaxed ) != Device::NVIDIA_Reflex)
                return;

            using namespace std::chrono;
            switch (marker) {

                case VK_LATENCY_MARKER_SIMULATION_START_NV: {
                    // commiting a frame start to the pacer here, which means the last sleep
                    // gets connected to the simulation marker here, and the sleep is
                    // connected to the internal pacer frames.
                    // the pacer might have different internal ids which we need to know
                    // when we perform the sleep, which is accounted for with the "drift" variable
                    _INFO( "VK_LATENCY_MARKER_SIMULATION_START_NV %" PRIu64 "\n", nvId );

                    auto t = m_lastEndSleep.load( std::memory_order_acquire );
                    uint64_t pacerId = m_mapping.registerNvId( nvId, t );

                    if (pacerId == FrameIdMap::INVALID_PACER_ID) {
                        // we've seen this id already before, so ignore it
                        // some games like Hitman WOA have multiple parallel simulation threads
                        break;
                    }

                    if (unlikely( !m_pendingSleep.load( std::memory_order_acquire ) ))
                        WARN( "Simulation marker without prior sleep. Game doesn't want to get paced? \n" );

                    _INFO( "timestamp stored for pacerId %" PRIu64 " \n", pacerId );
                    m_pacerState.simulation.store( pacerId, std::memory_order_release );
                    m_pendingSleep.store( false, std::memory_order_release );
                    break;
                }

                case VK_LATENCY_MARKER_SIMULATION_END_NV:
                    _INFO( "VK_LATENCY_MARKER_SIMULATION_END_NV %" PRIu64 "\n", nvId );
                    break;

                case VK_LATENCY_MARKER_RENDERSUBMIT_START_NV:
                    _INFO( "VK_LATENCY_MARKER_RENDERSUBMIT_START_NV %" PRIu64 "\n", nvId );
                    m_mapping.updateRenderStart( nvId );
                    break;

                case VK_LATENCY_MARKER_RENDERSUBMIT_END_NV:
                    _INFO( "VK_LATENCY_MARKER_RENDERSUBMIT_END_NV %" PRIu64 "\n", nvId );
                    m_mapping.updateRenderEnd( nvId );
                    break;

                case VK_LATENCY_MARKER_PRESENT_START_NV: {
                    _INFO( "VK_LATENCY_MARKER_PRESENT_START_NV %" PRIu64 "\n", nvId );
                    FramePacer::FrameInfo frameInfo = m_mapping.getFrameInfo( nvId );
                    const uint64_t& pacerId = frameInfo.externalId;
                    if (pacerId != FrameIdMap::INVALID_PACER_ID) {
                        int64_t drift = m_device->m_pacer->registerExternalId( pacerId, frameInfo );
                        _INFO( " present resolve: \n"
                               "   pacerId = %" PRIu64 " \n"
                               "   drift = %" PRIi64 " \n"
                               "   projected cpuId = %" PRIu64 " \n",
                               pacerId, drift, pacerId + drift );
                        if (drift != std::numeric_limits<int64_t>::max())
                            m_pacerState.drift.store( drift, std::memory_order_release );
                    }
                    break;
                }

                case VK_LATENCY_MARKER_PRESENT_END_NV:
                    _INFO( "VK_LATENCY_MARKER_PRESENT_END_NV %" PRIu64 "\n", nvId );
                    break;

                default:
                    _INFO( "unhandled VK_LATENCY_MARKER %" PRIu32 " for nv-id %" PRIu64 "\n", marker, nvId );
                    break;
            }
        }

    private:

        Device* m_device;

        std::atomic<bool> m_pendingSleep = { false };
        std::atomic<time_point> m_lastEndSleep = { };

        struct PacerState {
            std::atomic<uint64_t> simulation  = { 1 };
            std::atomic<int64_t> drift        = { 0 };
        };

        PacerState m_pacerState;
        FrameIdMap m_mapping;

    };

}
