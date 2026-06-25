#pragma once

#include <atomic>
#include <vector>
#include <array>

#include "util/util_time.h"
#include "util/util_likely.h"


namespace pacer {

    class FramePacer;
    class NvApi_PacingAdapter;


    struct LatencyMarkers {

        using time_point = dxvk::high_resolution_clock::time_point;

        time_point start             = { };
        time_point end               = { };

        time_point cpuFinished       = { };
        time_point gpuFinished       = { };
        int32_t presentFinished      = { 0 };

        int32_t renderStart          = { 0 };
        int32_t renderEnd            = { 0 };

    };


    class LatencyMarkersStorage {

        friend class ::pacer::FramePacer;

    public:

        LatencyMarkersStorage() { }
        ~LatencyMarkersStorage() { }

        const LatencyMarkers* getConstMarkers( uint64_t frameId ) const {
            return &m_markers[frameId % m_numMarkers];
        }

    private:

        LatencyMarkers* getMarkers( uint64_t frameId ) {
            return &m_markers[frameId % m_numMarkers];
        }

        // simple modulo hash mapping is used for frameIds. They are expected to monotonically increase by one.
        // only store a small number of past frames to keep the memory footprint low.
        static constexpr uint16_t m_numMarkers = 8;
        std::array<LatencyMarkers, m_numMarkers> m_markers = { };

    };

}
