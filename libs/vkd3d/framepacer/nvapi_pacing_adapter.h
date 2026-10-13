#pragma once

#include "framepacer/framepacer.h"
#include "framepacer/device.h"
#include "util/util_log.h"


namespace pacer {

    /*
     * Synchronization philosophy:
     * Use a buffer of sufficient size to store all marker timestamps,
     * such that well-behaving games can operate at maximum performance.
     *
     * The buffer size will act as a safety net for wild use of nv ids,
     * but there may be games out there falling through. Teared data is
     * possible in extreme cases.
     *
     * Microbenchmarks showed a setLatencyMarker call takes less than
     * 0.2 microseconds on average and almost never over 2 microseconds,
     * which makes it fulfill the role of not contributing to jitter.
     */

    class NvApi_FrameIdMap {
        using time_point = dxvk::high_resolution_clock::time_point;
        using microseconds = std::chrono::microseconds;
    public:

        struct alignas(64) PacerFrame {
            std::atomic<uint64_t> pacerId;
            std::atomic<time_point> start_t;
            std::atomic<int32_t> renderStart;
            std::atomic<int32_t> renderEnd;
        };

         /* Seqlock, and memory orderings based on:
            * Wait-Free Thread Synchronisation With the SeqLock - Timur Doumler - ADC 2024
            * https://www.youtube.com/watch?v=uiBMczcm69A
            * Can Seqlocks Get Along with Programming Language Memory Models? - Hans-J. Boehm
            * https://dl.acm.org/doi/epdf/10.1145/2247684.2247688
          */

        PacerFrame* getFrame( uint64_t nvId ) {
            size_t seq1, seq2;
            PacerFrame* res;

            do {
                res = nullptr;
                seq1 = m_seq.load( std::memory_order_acquire );
                for (size_t i=0; i<NUM_FRAMES; ++i) {
                    if (m_nvIds[i].load( std::memory_order_relaxed ) == nvId) {
                        res = &m_frames[i];
                        break;
                    }
                }
                std::atomic_thread_fence( std::memory_order_acquire );
                seq2 = m_seq.load( std::memory_order_relaxed );
            } while (seq1 & 1 || seq1 != seq2);

            return res;
        }

        uint64_t registerNvId( uint64_t nvId, time_point t ) {
            // we can only have one writer simultaneously
            std::lock_guard<dxvk::mutex> lock(m_nvIdsMutex);
            // check if the id really isn't registered yet
            for (size_t i=0; i<NUM_FRAMES; ++i) {
                if (m_nvIds[i].load( std::memory_order_relaxed ) == nvId)
                    return INVALID_PACER_ID;
            }

            // making seq odd: preventing reads
            size_t oldSeq = m_seq.load( std::memory_order_relaxed );
            m_seq.store( oldSeq + 1, std::memory_order_relaxed );
            std::atomic_thread_fence( std::memory_order_release );

            uint16_t index = m_curIndex++ % NUM_FRAMES;
            uint64_t pacerId = m_pacerSimulationId++;

            m_frames[index].pacerId.store( pacerId, std::memory_order_relaxed );
            m_frames[index].start_t.store( t, std::memory_order_relaxed );
            m_frames[index].renderStart.store( 0, std::memory_order_relaxed );
            m_frames[index].renderEnd.store( 0, std::memory_order_relaxed );

            m_nvIds[index].store( nvId, std::memory_order_relaxed );

            // make seq even again: allowing reads
            m_seq.store( oldSeq + 2, std::memory_order_release );

            return pacerId;
        }


        FramePacer::FrameInfo getFrameInfo( uint64_t nvId ) {
            PacerFrame* frame = getFrame(nvId);
            if (!frame)
                return FramePacer::FrameInfo{};

            FramePacer::FrameInfo res;
            res.externalId = frame->pacerId.load( std::memory_order_relaxed );
            res.start_t = frame->start_t.load( std::memory_order_relaxed );
            res.renderStart = frame->renderStart.load( std::memory_order_relaxed );
            res.renderEnd =  frame->renderEnd.load( std::memory_order_relaxed );

            return res;
        }

        void updateRenderStart( uint64_t nvId ) {
            updateTimestamp<&PacerFrame::renderStart>( nvId );
        }

        void updateRenderEnd( uint64_t nvId ) {
            updateTimestamp<&PacerFrame::renderEnd>( nvId );
        }

        constexpr static uint64_t INVALID_PACER_ID = 0;

    private:

        template <auto Timestamp>
        void updateTimestamp( uint64_t nvId ) {
            auto now = dxvk::high_resolution_clock::now();
            PacerFrame* frame = getFrame(nvId);
            if (!frame)
                return;

            int32_t old = (frame->*Timestamp).load( std::memory_order_relaxed );
            int32_t value = std::chrono::duration_cast<microseconds>(
                        now - frame->start_t.load( std::memory_order_relaxed )).count();

            // we aren't using compare-exchange here because of performance,
            // still this should give us the same data in 99.99% of cases
            if (!old || value < old)
                (frame->*Timestamp).store( value, std::memory_order_relaxed );
        }

        static constexpr size_t NUM_FRAMES = 16;

        // nv id lookup protected by a seq lock, mutex is used to serialize the writers.
        // seq lock guarantees maximum performance on reader paths

        alignas(64) dxvk::mutex m_nvIdsMutex;
        alignas(64) std::array< std::atomic<uint64_t>, NUM_FRAMES > m_nvIds = { };
        alignas(64) std::atomic< size_t > m_seq = { 0 };

        // once the pacer-frame is found for nvId, we access this data without any lock

        std::array< PacerFrame, NUM_FRAMES > m_frames = { };
        size_t m_curIndex = { 0 };
        uint64_t m_pacerSimulationId = { 2 };

    };


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

            auto t = dxvk::high_resolution_clock::now();
            m_lastEndSleep.store( t, std::memory_order_release );
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

                    if (pacerId == NvApi_FrameIdMap::INVALID_PACER_ID) {
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
                    uint64_t& pacerId = frameInfo.externalId;
                    if (pacerId != NvApi_FrameIdMap::INVALID_PACER_ID) {
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
        NvApi_FrameIdMap m_mapping;

    };

}
