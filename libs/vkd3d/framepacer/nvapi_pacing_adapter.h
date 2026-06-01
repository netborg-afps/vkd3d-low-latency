#pragma once

#include "framepacer/framepacer.h"
#include "util/util_log.h"

#define CHECK_INVALID_ID(id) \
    do { if (unlikely((id) == INVALID_ID)) \
        WARN("detected invalid id\n"); \
    } while (0)


class NvApi_FrameId {
public:
    uint64_t getId( uint64_t nvId ) {
        uint64_t seq1, seq2;
        uint64_t res;

        do {
            res = 0;
            seq1 = m_seq.load();
            for (Mapping& mapping : m_mapping) {
                if (mapping.nvId == nvId) {
                    res = mapping.pacerId;
                    break;
                }
            }
            seq2 = m_seq.load();
        } while ((seq1 & 1) == 1 || seq1 != seq2);

        return res;
    }

    void pushMapping( uint64_t nvId, uint64_t pacerId ) {
        // making seq odd, preventing reads
        ++m_seq;

        for (Mapping& mapping : m_mapping) {
            if (mapping.pacerId == pacerId) {
                WARN( "trying to push pacer-id %" PRIu64
                      " to nvApi frame-id mapping, but is already registered \n", pacerId );
                ++m_seq;
                return;
            }
            if (mapping.nvId == nvId) {
                WARN( "trying to push nv-id %" PRIu64
                      " to nvApi frame-id mapping, but is already registered \n", nvId );
                ++m_seq;
                return;
            }
        }
        uint64_t index = m_curIndex % m_mapping.size();
        ++m_curIndex;
        m_mapping[index].pacerId = pacerId;
        m_mapping[index].nvId = nvId;
        ++m_seq;

        INFO( "registered nv-id %" PRIu64
              " to pacer-frame-id %" PRIu64 " in nvApi mapping \n", nvId, pacerId );
    }

private:
    struct Mapping {
        std::atomic<uint64_t> pacerId;
        std::atomic<uint64_t> nvId;
    };

    std::array< Mapping, 8 > m_mapping = { };
    int64_t m_curIndex = { 0 };

    // sequence lock - may be able to optimize later
    // using thread fences and defining other memory orderings
    std::atomic< uint64_t > m_seq = { 0 };
};


class NvApi_PacingAdapter {

public:
    NvApi_PacingAdapter( PacerDevice* device )
    : m_pacer(device, 1) {}

    ~NvApi_PacingAdapter() {}

    void sleepAndBeginFrame() {
        // commiting a frame start to the pacer here
        // might cause some issues if this is called many times before
        // the game actually commits its simulation step to this, but
        // we made the ringbuffer in the pacer large enough to account for this
        // may need rework in the future to really make this bullet-proof
        uint64_t pacerId = ++m_pacerState.simulation;

        INFO( "Sleeping with pacer-id %" PRIu64 "\n", pacerId );
        m_pacer.sleepAndBeginFrame( pacerId );
        INFO( "Sleeping with pacer-id %" PRIu64 " finished \n", pacerId );
    }

    uint64_t notifySubmit() {
        uint64_t nvId = m_frameIds.submit;
        uint64_t pacerId = m_mapping.getId( nvId );
        CHECK_INVALID_ID( pacerId );

        // in case Reflex is turned off, we don't want to track new frames anymore
        // if we stop these here, vkd3d doesn't call notifyQueueSubmit and notifyGpuExecutionEnd either
        if (m_pacer.m_latencyMarkersStorage.m_timeline.cpuFinished >= pacerId)
            pacerId = INVALID_ID;

        if (pacerId == INVALID_ID)
            return INVALID_ID;

        m_pacer.notifySubmit( pacerId );
        return pacerId;
    }

    uint64_t notifyPresent() {
        uint64_t nvId = m_frameIds.submit;
        uint64_t pacerId = m_mapping.getId( nvId );
        CHECK_INVALID_ID( pacerId );

        // in case Reflex is turned off, we don't want to track new frames anymore
        // if we stop these here, vkd3d doesn't call notifyQueueSubmit and notifyGpuExecutionEnd either
        if (m_pacer.m_latencyMarkersStorage.m_timeline.cpuFinished >= pacerId)
            pacerId = INVALID_ID;

        if (pacerId == INVALID_ID)
            return INVALID_ID;

        uint64_t frameId;
        while ((frameId = ++m_pacerState.submit) < pacerId) {
            INFO( "fast forwarding submit state to frameId %" PRIu64 "\n", frameId);
            m_pacer.notifyPresent( frameId );
        }

        int numSubmits = m_pacer.m_latencyMarkersStorage.getConstMarkers(pacerId)->gpuSubmit.size();
        INFO( "notifyPresent with pacer_id %" PRIu64 " has %i submissions\n", pacerId, numSubmits );
        m_pacer.notifyPresent( pacerId );

        return pacerId;
    }

    void notifyQueueSubmit( uint64_t pacerId ) {
        // if we're getting an invalid id here, it's probably because we have returned it
        // during a previous notifySubmit to stop forwarding it to the frame pacer
        if (pacerId == INVALID_ID)
            return;
        m_pacer.notifyQueueSubmit( pacerId );
    }

    void notifyQueuePresent( uint64_t pacerId ) {
        // if we're getting an invalid id here, it's probably because we have returned it
        // during a previous notifySubmit to stop forwarding it to the frame pacer
        if (pacerId == INVALID_ID)
            return;

        uint64_t frameId;
        while ((frameId = ++m_pacerState.queueSubmit) < pacerId) {
            INFO( "fast forwarding queue-submit state to frameId %" PRIu64 "\n", frameId);
            m_pacer.notifyQueuePresentBegin( frameId );
        }

        int numSubmits = m_pacer.m_latencyMarkersStorage.getConstMarkers(pacerId)->gpuQueueSubmit.size();
        INFO( "notifyQueuePresent with pacer_id %" PRIu64 " has %i submissions\n", pacerId, numSubmits );
        m_pacer.notifyQueuePresentBegin( pacerId );
    }

    void notifyGpuExecutionEnd( uint64_t pacerId, PacerQueryPool* queryPool ) {
        // if we're getting an invalid id here, it's probably because we have returned it
        // during a previous notifySubmit to stop forwarding it to the frame pacer
        if (pacerId == INVALID_ID)
            return;

        uint64_t frameId;
        while ((frameId = ++m_pacerState.gpuFinished) < pacerId) {
            INFO( "fast forwarding gpuFinished state to frameId %" PRIu64 "\n", frameId);
            m_pacer.finishRender(frameId);
        }

        m_pacer.notifyGpuExecutionEnd( pacerId, queryPool );
    }

    void setLatencyMarker( uint64_t nvId, VkLatencyMarkerNV marker ) {
        using namespace std::chrono;
        switch (marker) {
            case VK_LATENCY_MARKER_SIMULATION_START_NV: {
                INFO( "VK_LATENCY_MARKER_SIMULATION_START_NV %" PRIu64 "\n", nvId );
                m_frameIds.simulation = nvId;

                uint64_t pacerId = m_pacerState.simulation;
                m_mapping.pushMapping( nvId, pacerId );
                break;
            }
            case VK_LATENCY_MARKER_SIMULATION_END_NV: {
                INFO( "VK_LATENCY_MARKER_SIMULATION_END_NV %" PRIu64 "\n", nvId );
                uint64_t pacerId = m_mapping.getId( nvId );
                if (pacerId != INVALID_ID) {
                    auto now = dxvk::high_resolution_clock::now();
                    dxvk::LatencyMarkers* m = m_pacer.m_latencyMarkersStorage.getMarkers(pacerId);
                    m->simulationFinished = duration_cast<microseconds>(now - m->start).count();
                }
                break;
            }
            case VK_LATENCY_MARKER_RENDERSUBMIT_START_NV: {
                INFO( "VK_LATENCY_MARKER_RENDERSUBMIT_START_NV %" PRIu64 "\n", nvId );
                m_frameIds.submit = nvId;
                uint64_t pacerId = m_mapping.getId( nvId );
                if (pacerId != INVALID_ID) {
                    auto now = dxvk::high_resolution_clock::now();
                    dxvk::LatencyMarkers* m = m_pacer.m_latencyMarkersStorage.getMarkers(pacerId);
                    m->renderStart = duration_cast<microseconds>(now - m->start).count();
                }
                break;
            }

            case VK_LATENCY_MARKER_PRESENT_START_NV:
                INFO( "VK_LATENCY_MARKER_PRESENT_START_NV %" PRIu64 "\n", nvId );
                m_frameIds.present = nvId;
                break;

            default:
                break;
        }
    }

    dxvk::FramePacer m_pacer;

private:

    static constexpr uint64_t INVALID_ID = { 0 };
    struct FrameIds {
        std::atomic<uint64_t> simulation = { INVALID_ID };
        std::atomic<uint64_t> submit     = { INVALID_ID };
        std::atomic<uint64_t> present    = { INVALID_ID };
    };

    struct PacerState {
        uint64_t simulation               = { 0 };
        std::atomic<uint64_t> submit      = { 0 };
        std::atomic<uint64_t> queueSubmit = { 0 };
        std::atomic<uint64_t> gpuFinished = { 0 };
    };

    FrameIds m_frameIds;
    PacerState m_pacerState;
    NvApi_FrameId m_mapping;

};
