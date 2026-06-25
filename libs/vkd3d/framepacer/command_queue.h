#pragma once
#include "vulkan_queue.h"
#include "device.h"
#include "nvapi_pacing_adapter.h"
#include "util/util_time.h"

namespace pacer {

    class SubmitIterator;

    class CommandQueue {
        using time_point = dxvk::high_resolution_clock::time_point;
        using high_resulution_clock = dxvk::high_resolution_clock;
        friend class SubmitIterator;
    public:

        struct Properties {
            uint16_t id;
            D3D12_COMMAND_LIST_TYPE type;
            void* vkd3d_command_queue;
            void* vkd3d_queue;
        };

        const Properties m_properties;

        CommandQueue( Device* device, const Properties& properties, VulkanQueue* vulkanQueue )
            : m_properties(properties), m_vulkanQueue(vulkanQueue), m_device(device) {}

        ~CommandQueue() {}

        // we need to support multithreaded access here
        uint64_t notifySubmit() {
            uint64_t id = m_submitCounter++;
            m_submits[id % NUM_SUBMITS] = high_resulution_clock::now();
            m_vulkanQueueIds[id % NUM_SUBMITS] = INVALID_ID;
            return id;
        }

        // this method is accessed single threaded
        void notifyVulkanSubmit( uint64_t commandId, uint64_t vulkanId ) {
            m_vulkanQueueIds[commandId % NUM_SUBMITS] = vulkanId % VulkanQueue::NUM_SUBMITS;
        }

        void notifyPresent( uint64_t cpuId ) {
            uint16_t index = (m_submitCounter-1) % NUM_SUBMITS;
            m_device->m_nvApi_pacingAdapter->m_pacer.finishCpu();
            m_presentFlags[ index ].store( true, std::memory_order_release );

            uint64_t vulkanId = m_vulkanQueueIds[index].load(std::memory_order_acquire);
            uint16_t vulkanIndex = vulkanId % VulkanQueue::NUM_SUBMITS;
            uint64_t t_gpu;
            if ( vulkanId != INVALID_ID
                && (t_gpu = m_vulkanQueue->m_gpuExecutionEnd[vulkanIndex].load(std::memory_order_acquire))
                && m_presentFlags[index].exchange(false)) {
                m_device->m_nvApi_pacingAdapter->m_pacer.finishRender(t_gpu);
            }
        }

        void notifyVulkanGpuExecutionEnd( uint64_t commandId, uint64_t vulkanId ) {
            uint16_t index = commandId % NUM_SUBMITS;
            uint64_t t_gpu;
            if (m_presentFlags[index].load(std::memory_order_acquire)
                && (t_gpu = m_vulkanQueue->m_gpuExecutionEnd[vulkanId % VulkanQueue::NUM_SUBMITS].load(std::memory_order_acquire))
                && m_presentFlags[index].exchange(false)) {
                m_device->m_nvApi_pacingAdapter->m_pacer.finishRender(t_gpu);
            }
        }

        VulkanQueue* m_vulkanQueue;
        Device* m_device;

    private:

        static constexpr uint16_t INVALID_ID = 0xFFFF;
        static constexpr uint16_t NUM_SUBMITS = 2048;

        std::array<std::atomic<time_point>, NUM_SUBMITS> m_submits;
        std::array<std::atomic<uint16_t>, NUM_SUBMITS>   m_vulkanQueueIds;
        std::array<std::atomic<bool>, NUM_SUBMITS>       m_presentFlags = { };
        std::atomic<uint64_t> m_submitCounter = { 1 };

    };

}
