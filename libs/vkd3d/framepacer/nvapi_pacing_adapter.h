#pragma once

#include "framepacer/framepacer.h"
#include "util/util_log.h"


namespace pacer {

    class NvApi_FrameId {
        using time_point = dxvk::high_resolution_clock::time_point;
        using microseconds = std::chrono::microseconds;
    public:

        uint64_t getId( uint64_t nvId ) {
            uint64_t seq1, seq2;
            uint64_t res;

            do {
                res = 0;
                seq1 = m_seq.load(std::memory_order_acquire);
                for (Mapping& mapping : m_mapping) {
                    if (mapping.nvId.load(std::memory_order_relaxed) == nvId) {
                        res = mapping.pacerId.load(std::memory_order_relaxed);
                        break;
                    }
                }
                seq2 = m_seq.load(std::memory_order_acquire);
            } while ((seq1 & 1) == 1 || seq1 != seq2);

            return res;
        }

        FramePacer::FrameInfo getFrameInfo( uint64_t nvId ) {
            uint64_t seq1, seq2;
            FramePacer::FrameInfo res;

            do {
                res = FramePacer::FrameInfo {};
                seq1 = m_seq.load(std::memory_order_acquire);
                for (Mapping& mapping : m_mapping) {
                    if (mapping.nvId.load(std::memory_order_relaxed) == nvId) {
                        res.externalId = mapping.pacerId.load(std::memory_order_relaxed);
                        res.start_t = mapping.start_t.load(std::memory_order_relaxed);
                        res.renderStart = mapping.renderStart.load(std::memory_order_acquire);
                        res.renderEnd =  mapping.renderEnd.load(std::memory_order_acquire);
                        break;
                    }
                }
                seq2 = m_seq.load(std::memory_order_acquire);
            } while ((seq1 & 1) == 1 || seq1 != seq2);

            return res;
        }

        void updateRenderStart( uint64_t nvId, time_point t ) {
            updateTimestamp<&Mapping::renderStart>( nvId, t );
        }

        void updateRenderEnd( uint64_t nvId, time_point t ) {
            updateTimestamp<&Mapping::renderEnd>( nvId, t );
        }

        uint64_t pushMapping( uint64_t nvId, time_point t ) {
            // making seq odd, preventing reads
            m_seq.fetch_add(1, std::memory_order_release);

            for (Mapping& mapping : m_mapping) {
                if (mapping.nvId.load(std::memory_order_relaxed) == nvId) {
                    WARN( "trying to push nv-id %" PRIu64
                          " to nvApi frame-id mapping, but is already registered \n", nvId );
                    m_seq.fetch_add(1, std::memory_order_release);
                    return 0;
                }
            }
            uint64_t index = m_curIndex % m_mapping.size();
            uint64_t pacerId = m_pacerSimulationId++;
            ++m_curIndex;
            m_mapping[index].pacerId.store( pacerId, std::memory_order_relaxed );
            m_mapping[index].start_t.store( t, std::memory_order_relaxed );
            m_mapping[index].renderStart.store( 0, std::memory_order_relaxed );
            m_mapping[index].renderEnd.store( 0, std::memory_order_relaxed );
            // release
            m_mapping[index].nvId.store( nvId, std::memory_order_release );
            m_seq.fetch_add(1, std::memory_order_release); // make seq even again

            _INFO( "registered nv-id %" PRIu64
                  " to pacer-frame-id %" PRIu64 " in nvApi mapping \n", nvId, pacerId );

            return pacerId;
        }

    private:

        template <auto Timestamp>
        void updateTimestamp( uint64_t nvId, time_point t ) {
            // don't check the seq lock here, trust the ringbuffer

            for (Mapping& mapping : m_mapping) {
                if (mapping.nvId.load(std::memory_order_acquire) == nvId) {
                    int32_t value = std::chrono::duration_cast<microseconds>(
                        t - mapping.start_t.load(std::memory_order_relaxed)).count();
                    int32_t expected = 0;
                    (mapping.*Timestamp).compare_exchange_strong( expected, value,
                        std::memory_order_release, std::memory_order_relaxed );
                    return;
                }
            }
        }

        struct alignas(64) Mapping {
            std::atomic<uint64_t> pacerId;
            std::atomic<uint64_t> nvId;
            std::atomic<time_point> start_t;
            std::atomic<int32_t> renderStart;
            std::atomic<int32_t> renderEnd;
        };

        std::array< Mapping, 16 > m_mapping = { };
        int64_t m_curIndex = { 0 };
        uint64_t m_pacerSimulationId = { 1 };

        alignas(64) std::atomic< uint64_t > m_seq = { 0 };
    };


    class NvApi_PacingAdapter {
        using time_point = dxvk::high_resolution_clock::time_point;

    public:
        NvApi_PacingAdapter( pacer::Device* device )
        : m_pacer(device, 1) {}

        ~NvApi_PacingAdapter() {}

        void sleepAndBeginFrame() {
            // only sleep once before seeing a simulation marker
            uint64_t pacerId = m_pacerState.simulation + 1;

            if (!m_pendingSleep) {
                _INFO( "sleeping for pacerId %" PRIu64 " \n", pacerId );
                m_pacer.sleep(pacerId + m_pacerState.drift, m_lastSimulationStart.load(std::memory_order_acquire));
                m_pendingSleep = true;
            }
        }

        void setLatencyMarker( uint64_t nvId, VkLatencyMarkerNV marker ) {

            using namespace std::chrono;
            switch (marker) {

                case VK_LATENCY_MARKER_SIMULATION_START_NV: {
                    // commiting a frame start to the pacer here,
                    // the pacer might have different internal ids which we need to know
                    // when we perform the sleep, which is accounted for with the "drift" variable
                    _INFO( "VK_LATENCY_MARKER_SIMULATION_START_NV %" PRIu64 "\n", nvId );

                    if (m_mapping.getId( nvId ) != INVALID_ID) {
                        // we've seen this id already before, so ignore it
                        // some games like Hitman WOA have multiple parallel simulation threads
                        break;
                    }

                    auto t = dxvk::high_resolution_clock::now();
                    m_lastSimulationStart.store( t, std::memory_order_release );
                    if (!m_pendingSleep)
                        WARN( "Simulation marker without prior sleep. Game doesn't want to get paced? \n");

                    uint64_t pacerId = m_mapping.pushMapping( nvId, t );
                    _INFO( "timestamp stored for pacerId %" PRIu64 " \n", pacerId );
                    if (pacerId) {
                        m_pacerState.simulation = pacerId;
                        m_pendingSleep = false;
                    }
                    break;
                }

                case VK_LATENCY_MARKER_SIMULATION_END_NV:
                    _INFO( "VK_LATENCY_MARKER_SIMULATION_END_NV %" PRIu64 "\n", nvId );
                    break;

                case VK_LATENCY_MARKER_RENDERSUBMIT_START_NV: {
                    auto now = dxvk::high_resolution_clock::now();
                    _INFO( "VK_LATENCY_MARKER_RENDERSUBMIT_START_NV %" PRIu64 "\n", nvId );
                    m_mapping.updateRenderStart( nvId, now );
                    break;
                }

                case VK_LATENCY_MARKER_RENDERSUBMIT_END_NV: {
                    auto now = dxvk::high_resolution_clock::now();
                    _INFO( "VK_LATENCY_MARKER_RENDERSUBMIT_END_NV %" PRIu64 "\n", nvId );
                    m_mapping.updateRenderEnd( nvId, now );
                    break;
                }

                case VK_LATENCY_MARKER_PRESENT_START_NV: {
                    _INFO( "VK_LATENCY_MARKER_PRESENT_START_NV %" PRIu64 "\n", nvId );
                    FramePacer::FrameInfo frameInfo = m_mapping.getFrameInfo( nvId );
                    uint64_t& pacerId = frameInfo.externalId;
                    if (pacerId != INVALID_ID) {
                        int64_t drift = m_pacer.registerExternalId( pacerId, frameInfo );
                        _INFO( " present resolve: \n"
                               "   pacerId = %" PRIu64 " \n"
                               "   drift = %" PRIu64 " \n"
                               "   projected cpuId = %" PRIu64 " \n",
                               pacerId, drift, pacerId + drift );
                        if (drift != std::numeric_limits<int64_t>::max())
                            m_pacerState.drift = drift;
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

        FramePacer m_pacer;

    private:

        static constexpr uint64_t INVALID_ID = { 0 };

        std::atomic<bool> m_pendingSleep = { false };
        std::atomic<time_point> m_lastSimulationStart = { };

        struct PacerState {
            std::atomic<uint64_t> simulation  = { 0 };
            std::atomic<int64_t> drift        = { 0 };
        };

        PacerState m_pacerState;
        NvApi_FrameId m_mapping;

    };

}
