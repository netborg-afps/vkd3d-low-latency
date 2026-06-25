#include "command_queue.h"
#include "vulkan_queue.h"
#include "util/util_time.h"
#include "util/util_log.h"


namespace pacer {

    class SubmitIterator {

        using time_point = dxvk::high_resolution_clock::time_point;

    public:

        explicit SubmitIterator( time_point start, time_point end, CommandQueue* q )
        : m_commandQueue(q), m_vulkanQueue(q->m_vulkanQueue) {
            // we may be able to make this faster with caching, but let's first care about correctness
            // also because this is not called from a performance critical thread
            auto q_submits = &q->m_submits;
            auto c_submit = [q_submits]( uint64_t id ) {
                return (*q_submits)[ id % CommandQueue::NUM_SUBMITS ].load(std::memory_order_acquire);
            };

            m_lastIndex = q->m_submitCounter - 1;
            if (q->m_submitCounter == 0)
                m_lastIndex = 0;

            uint64_t stopIndex = (m_lastIndex > CommandQueue::NUM_SUBMITS/2)
                ? m_lastIndex - CommandQueue::NUM_SUBMITS/2 : 0;

            while (m_lastIndex > stopIndex && c_submit(m_lastIndex) > end)
                --m_lastIndex;

            m_curIndex = m_lastIndex;
            while (m_curIndex > stopIndex && c_submit(m_curIndex) > start)
                --m_curIndex;

            m_curIndex++;
            cacheVulkanQueueId();

            _INFO( "iter spans from %" PRIu64 " to %" PRIu64 "\n", m_curIndex, m_lastIndex );
        }

        bool isAtEnd() {
            return m_curIndex > m_lastIndex;
        }

        void operator++() {
            ++m_curIndex;
            cacheVulkanQueueId();
        }

        void cacheVulkanQueueId() {
            if (isAtEnd())
                return;
            uint64_t vulkanId = m_commandQueue->m_vulkanQueueIds[ m_curIndex % CommandQueue::NUM_SUBMITS ].load(std::memory_order_acquire);
            m_cachedVulkanQueueId = vulkanId % VulkanQueue::NUM_SUBMITS;
        }

        time_point getAppSubmit() {
            return m_commandQueue->m_submits[ m_curIndex % CommandQueue::NUM_SUBMITS ].load(std::memory_order_acquire);
        }

        time_point getVulkanSubmit() {
            return m_vulkanQueue->m_submits[ m_cachedVulkanQueueId ];
        }

        uint64_t getVulkanGpuExecutionStart() {
            return m_vulkanQueue->m_gpuExecutionStart[ m_cachedVulkanQueueId ].load(std::memory_order_acquire);
        }

        uint64_t getVulkanGpuExecutionEnd() {
            return m_vulkanQueue->m_gpuExecutionEnd[ m_cachedVulkanQueueId ].load(std::memory_order_acquire);
        }

        uint16_t getVulkanQueueId() {
            return m_vulkanQueue->m_properties.id;
        }

    private:

        uint64_t m_curIndex;
        uint64_t m_lastIndex;
        CommandQueue* m_commandQueue;
        VulkanQueue* m_vulkanQueue;
        uint16_t m_cachedVulkanQueueId;
    };

}
