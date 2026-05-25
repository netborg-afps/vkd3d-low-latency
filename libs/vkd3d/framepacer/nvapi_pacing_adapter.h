#pragma once

#include "framepacer/framepacer.h"
#include "util/util_log.h"


class NvApi_FrameId {
public:
    uint64_t getId( uint64_t nvId ) {
        for (Mapping& mapping : m_mapping)
            if (mapping.nvId == nvId)
                return mapping.pacerId;
        return 0;
    }

    void pushMapping( uint64_t nvId, uint64_t pacerId ) {
        uint64_t index = m_curIndex.load() % m_mapping.size();
        ++m_curIndex;
        m_mapping[index].pacerId = pacerId;
        m_mapping[index].nvId = nvId;
    }

private:
    struct Mapping {
        uint64_t pacerId;
        uint64_t nvId;
    };

    // todo: somehow make this safe in a lockfree way
    std::array< Mapping, 8 > m_mapping = { };
    std::atomic< int64_t > m_curIndex = { 0 };
    // std::atomic< uint64_t > m_curPacerId = { 1 };
};


class NvApi_PacingAdapter {

public:
    NvApi_PacingAdapter()
    : m_pacer(nullptr, 1) {}

    ~NvApi_PacingAdapter() {}

    void sleepAndBeginFrame() {
        ++m_frameIds.pacer_app;
        uint64_t pacerId = m_frameIds.pacer_app;

        INFO( "Sleeping with pacer-id %" PRIu64 "\n", pacerId );
        m_pacer.sleepAndBeginFrame( pacerId );
        INFO( "Sleeping with pacer-id %" PRIu64 " finished \n", pacerId );
    }

    void endFrame( uint64_t nvId ) {
        if (nvId == INVALID_ID) {
            INFO( "INVALID_ID, cannot end frame \n" );
            return;
        }

        uint64_t pacerId = m_mapping.getId( nvId );
        if (pacerId == INVALID_ID) {
            INFO( "cannot end frame, nv-id %" PRIu64 " was not seen at frame start \n", nvId );
            return;
        }
        INFO( "end_frame with nv-id %" PRIu64 ", pacer-id %" PRIu64 "\n", nvId, pacerId );

        m_pacer.notifyGpuExecutionEnd( pacerId, nullptr );
        m_pacer.notifyGpuPresentBegin( pacerId );
        m_pacer.notifyGpuPresentEnd( pacerId );
    }

    void setLatencyMarker( uint64_t frameId, VkLatencyMarkerNV marker ) {
        switch (marker) {
            case VK_LATENCY_MARKER_SIMULATION_START_NV: {
                INFO( "VK_LATENCY_MARKER_SIMULATION_START_NV %" PRIu64 "\n", frameId );
                m_frameIds.simulation = frameId;

                // todo: make this bullet-proof
                uint64_t pacerId = m_frameIds.pacer_app;
                m_mapping.pushMapping( frameId, pacerId );
                break;
            }
            case VK_LATENCY_MARKER_PRESENT_START_NV:
                if (frameId <= m_frameIds.present) {
                    WARN( "VK_LATENCY_MARKER_PRESENT_START_NV %" PRIu64 " not correct\n", frameId);
                    m_frameIds.present = INVALID_ID;
                    break;
                }
                INFO( "VK_LATENCY_MARKER_PRESENT_START_NV %" PRIu64 "\n", frameId );
                m_frameIds.present = frameId;
                break;
            default:
                break;
        }
    }

private:

    static constexpr uint64_t INVALID_ID = { 0 };
    struct FrameIds {
        std::atomic<uint64_t> simulation = { INVALID_ID };
        std::atomic<uint64_t> submit = { INVALID_ID };
        std::atomic<uint64_t> present = { INVALID_ID };

        std::atomic<uint64_t> pacer_app = { 0 };
    };

    FrameIds m_frameIds;
    NvApi_FrameId m_mapping;
    dxvk::FramePacer m_pacer;

};