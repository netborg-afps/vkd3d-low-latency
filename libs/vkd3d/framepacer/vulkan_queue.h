#pragma once

#include "device.h"
#include "framepacer.h"
#include "nvapi_pacing_adapter.h"
#include "util/sync/sync_ringbuffer_allocator.h"
#include "util/util_time.h"
#include "util/util_likely.h"
#include "util/util_small_vector.h"
#include <queue>

namespace pacer {

    class SubmitIterator;
    class CommandQueue;

    class VulkanQueue {
        using time_point = dxvk::high_resolution_clock::time_point;
        using high_resolution_clock = dxvk::high_resolution_clock;
        friend class SubmitIterator;
        friend class CommandQueue;
    public:

        struct Properties {
            uint16_t id;
            void* vkd3d_queue;
            pacer_vulkan_queue_info queueInfo;
        };

        const Properties m_properties;
        static constexpr uint16_t NUM_SUBMITS = 2048;

        VulkanQueue( Device* device, const Properties& properties )
            : m_properties( properties ), m_device( device ) {
            initVulkanObjects();
        }

        ~VulkanQueue() {
            destroyVulkanObjects();
        }

        // we expect to handle one thread only here
        uint64_t notifySubmit( ) {

            uint64_t id = m_submitCounter.load( std::memory_order_relaxed );
            uint16_t index = id % NUM_SUBMITS;
            m_submits[index] = high_resolution_clock::now();
            m_gpuExecutionStart[index].store( UINT64_MAX, std::memory_order_release );
            m_gpuExecutionEnd[index].store( UINT64_MAX, std::memory_order_release );
            m_submitCounter.store( id + 1, std::memory_order_release );
            return id;

        }

        void notifyGpuExecutionEnd( uint64_t vulkanId, pacer_query_pool* queryPoolTopOfPipe, pacer_query_pool* queryPool ) {

            uint64_t index = vulkanId % NUM_SUBMITS;
            uint64_t timestamp;

            if (queryPoolTopOfPipe) {
                getQueryPoolResult(queryPoolTopOfPipe, &timestamp);
                m_gpuExecutionStart[index].store( timestamp, std::memory_order_release );
                freeQueryPoolTopOfPipe(queryPoolTopOfPipe);
            }

            if (queryPool) {
                getQueryPoolResult(queryPool, &timestamp);
                m_gpuExecutionEnd[index].store( timestamp, std::memory_order_release );
                freeQueryPool(queryPool);
            }

        }

        pacer_query_pool* allocQueryPool() {

            return m_queryPoolsBottomOfPipe.alloc();

        }

        pacer_query_pool* allocQueryPoolTopOfPipe() {

            pacer_query_pool* res = m_queryPoolsTopOfPipe.alloc();
            m_device->m_vkProcs.vkResetQueryPool( m_device->m_properties.vk_device,
                res->pool, 0, 1);
            return res;

        }

        void pushQueryPoolTopOfPipe( pacer_query_pool* queryPool, uint64_t submitId, VkSemaphore waitSemaphore, uint64_t waitValue, bool pushIntoQueue ) {
            /*
             * todo: Reading top-of-pipe timestamps in a separate thread currently doesn't work correctly on RADV
             *   because its vkWaitSemaphores() implementation wakes us up way too late. Delays of more than 10 ms
             *   have been observed. Some kind of reactive wait is needed though for this thread to work efficiently.
             *   Will be reactivated once we work on the GPU Progress feature. By then we have to figure out
             *   how to deal with the Mesa/RADV path. Was working without such delays on the NVIDIA driver (615.78.08).
             */
        }

        void pushQueryPoolWaitSemaphores( const VkSemaphoreSubmitInfo* pWaitSemaphoreInfos, uint32_t waitSemaphoreInfoCount ) { }

        void freeQueryPoolTopOfPipe(pacer_query_pool* queryPool) {

            if (queryPool == nullptr)
                return;
            m_queryPoolsTopOfPipe.free( queryPool );

        }

        void freeQueryPool(pacer_query_pool* queryPool) {

            if (queryPool == nullptr)
                return;
            m_queryPoolsBottomOfPipe.free( queryPool );

        }

    private:

        VkResult getQueryPoolResult( pacer_query_pool* queryPool, uint64_t* timestamp ) const {

            assert( queryPool );
            VkQueryResultFlags flags = VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT;
            VkResult res = m_device->m_vkProcs.vkGetQueryPoolResults(
                m_device->m_properties.vk_device, queryPool->pool, 0, 1, sizeof(uint64_t),
                timestamp, sizeof(uint64_t), flags
            );

            if (unlikely(res != VK_SUCCESS)) {
                ERR( "FramePacer: vkGetQueryPoolResults returned %u \n", res);
                *timestamp = UINT64_MAX;
            }

            return res;

        }

        void initVulkanObjects() {

            pacer_device_vk_procs* vk_procs = &m_device->m_vkProcs;
            VkDevice& device = m_device->m_properties.vk_device;

            VkQueryPoolCreateInfo queryPoolInfo = {};
            queryPoolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            queryPoolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            queryPoolInfo.queryCount = 1;

            // query pools for submit-start

            pacer_query_pool* queryPools = m_queryPoolsTopOfPipe.getDataUnsafe();
            for (int i=0; i<256; ++i) {
                VkResult res = vk_procs->vkCreateQueryPool(device, &queryPoolInfo, nullptr, &queryPools[i].pool);

                if (res != VK_SUCCESS) {
                    ERR("FramePacer: Failed to create submit query pool \n");
                    exit(-1);
                }
            }

            // query pools for submit-end

            queryPools = m_queryPoolsBottomOfPipe.getDataUnsafe();
            for (int i=0; i<256; ++i) {
                VkResult res = vk_procs->vkCreateQueryPool(device, &queryPoolInfo, nullptr, &queryPools[i].pool);

                if (res != VK_SUCCESS) {
                    ERR("FramePacer: Failed to create submit query pool \n");
                    exit(-1);
                }
            }

            // command pool

            VkCommandPoolCreateInfo commandPoolInfo = {};
            commandPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            commandPoolInfo.queueFamilyIndex = m_properties.queueInfo.family_index;

            if (vk_procs->vkCreateCommandPool(device, &commandPoolInfo, nullptr, &m_commandPool) != VK_SUCCESS) {
                ERR("FramePacer: Failed to create command pool \n");
                exit(-1);
            }

            VkCommandBufferAllocateInfo allocInfo = {};
            allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocInfo.commandPool = m_commandPool;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandBufferCount = 1;

            VkCommandBufferBeginInfo beginInfo = {};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = 0;

            // command buffers for submit-start

            queryPools = m_queryPoolsTopOfPipe.getDataUnsafe();

            for (int i=0; i<256; ++i) {
                VkResult res = vk_procs->vkAllocateCommandBuffers(
                    device, &allocInfo, &queryPools[i].buffer);

                if (res != VK_SUCCESS) {
                    ERR("FramePacer: Failed to create submit query pool \n");
                    exit(-1);
                }

                vk_procs->vkBeginCommandBuffer(queryPools[i].buffer, &beginInfo);
                // we couldn't bake the reset in like that for the beginning timestamp
                // because we would get validation error messages that we tried to access a pool
                // that wasn't reset. wondering though why this is working for the other one
//                vk_procs->vkCmdResetQueryPool(queryPools[i].buffer, queryPools[i].pool, 0, 1);
                vk_procs->vkCmdWriteTimestamp2(queryPools[i].buffer,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                queryPools[i].pool, 0);
                vk_procs->vkEndCommandBuffer(queryPools[i].buffer);
            }

            // command buffers for submit-end

            queryPools = m_queryPoolsBottomOfPipe.getDataUnsafe();

            for (int i=0; i<256; ++i) {
                VkResult res = vk_procs->vkAllocateCommandBuffers(
                    device, &allocInfo, &queryPools[i].buffer);

                if (res != VK_SUCCESS) {
                    ERR("FramePacer: Failed to create submit query pool \n");
                    exit(-1);
                }

                vk_procs->vkBeginCommandBuffer(queryPools[i].buffer, &beginInfo);
                vk_procs->vkCmdResetQueryPool(queryPools[i].buffer, queryPools[i].pool, 0, 1);
                vk_procs->vkCmdWriteTimestamp2(queryPools[i].buffer,
                VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                queryPools[i].pool, 0);
                vk_procs->vkEndCommandBuffer(queryPools[i].buffer);
            }
        }

        void destroyVulkanObjects() {

            pacer_device_vk_procs* vk_procs = &m_device->m_vkProcs;
            VkDevice& device = m_device->m_properties.vk_device;

            pacer_query_pool* queryPools = m_queryPoolsTopOfPipe.getDataUnsafe();
            for (int i=0; i<256; ++i) {
                vk_procs->vkFreeCommandBuffers(device, m_commandPool, 1, &queryPools[i].buffer);
            }

            queryPools = m_queryPoolsBottomOfPipe.getDataUnsafe();
            for (int i=0; i<256; ++i) {
                vk_procs->vkFreeCommandBuffers(device, m_commandPool, 1, &queryPools[i].buffer);
            }

            vk_procs->vkDestroyCommandPool(device, m_commandPool, nullptr);

            queryPools = m_queryPoolsTopOfPipe.getDataUnsafe();
            for (int i=0; i<256; ++i) {
                vk_procs->vkDestroyQueryPool(device, queryPools[i].pool, nullptr );
            }

            queryPools = m_queryPoolsBottomOfPipe.getDataUnsafe();
            for (int i=0; i<256; ++i) {
                vk_procs->vkDestroyQueryPool(device, queryPools[i].pool, nullptr );
            }

        }


        Device* m_device;

        // is accessed from multiple threads

        std::array<time_point, NUM_SUBMITS> m_submits = { };
        std::array<std::atomic<uint64_t>, NUM_SUBMITS> m_gpuExecutionStart = { };
        std::array<std::atomic<uint64_t>, NUM_SUBMITS> m_gpuExecutionEnd = { };
        std::atomic<uint64_t> m_submitCounter = { 1 };

        VkCommandPool m_commandPool = { VK_NULL_HANDLE };

        // holding the elements in a lockfree ringbuffer has the advantage
        // that we can check if we truely cycle through the pools in order

        sync::RingbufferAllocator<pacer_query_pool, 256> m_queryPoolsTopOfPipe;
        sync::RingbufferAllocator<pacer_query_pool, 256> m_queryPoolsBottomOfPipe;

    };
}
