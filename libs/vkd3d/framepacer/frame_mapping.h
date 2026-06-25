#pragma once

namespace pacer {

    /*
     * internal-id (cpuId, gpuId) to frame-id
     */
    class FrameMapping {
    public:
        struct Mapping {
            std::atomic<uint64_t> internalId = { 0 };
            std::atomic<uint64_t> frameId    = { 0 };
        };

        uint64_t getFrameId( uint64_t internalId ) {
            uint16_t index = internalId % NUM_MAPPINGS;
            Mapping& mapping = m_frameIds[index];
            if (mapping.internalId != internalId)
                return 0;
            return mapping.frameId;
        }

        void registerMapping( uint64_t internalId, uint64_t frameId ) {
            _INFO( "register internal-id %" PRIu64 " to external-id %" PRIu64 " \n",
                internalId, frameId );
            uint16_t index = internalId % NUM_MAPPINGS;
            Mapping& mapping = m_frameIds[index];
            mapping.internalId = internalId;
            mapping.frameId = frameId;
        }

    private:
        static constexpr uint16_t NUM_MAPPINGS = 8;
        std::array< Mapping, NUM_MAPPINGS> m_frameIds = { };
    };


}
