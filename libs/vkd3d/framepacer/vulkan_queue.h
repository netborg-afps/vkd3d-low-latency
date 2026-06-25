#pragma once

#include "device.h"
#include "framepacer.h"
#include "nvapi_pacing_adapter.h"
#include "util/sync/sync_ringbuffer_allocator.h"
#include "util/util_time.h"
#include "util/util_likely.h"
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
            : m_properties( properties ), m_device( device ),
              m_thread([this] { threadFunc(); }) {
            initVulkanObjects();
        }

        ~VulkanQueue() {
            {   std::lock_guard<dxvk::mutex> lock(m_mutex);
                m_stopped.store( true );
                m_cond.notify_one();
            }
            m_thread.join();
            destroyVulkanObjects();
        }

        // we expect to handle one thread only here
        uint64_t notifySubmit( ) {

            uint64_t id = m_submitCounter.load();
            uint16_t index = id % NUM_SUBMITS;
            m_submits[index] = high_resolution_clock::now();
            m_gpuExecutionStart[index] = 0;
            m_gpuExecutionEnd[index] = 0;
            ++m_submitCounter;
            return id;

        }

        uint64_t notifyGpuExecutionEnd( uint64_t vulkanId, pacer_query_pool* queryPool ) {

            if (queryPool == nullptr)
                return 0;

            uint64_t index = vulkanId % NUM_SUBMITS;
            uint64_t timestamp;
            getQueryPoolResult(queryPool, &timestamp);
            m_gpuExecutionEnd[index].store( timestamp, std::memory_order_release );
            freeQueryPool(queryPool);
            return timestamp;

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

        void pushQueryPoolTopOfPipe( pacer_query_pool* queryPool, uint64_t submitId, bool pushIntoQueue ) {

            if (unlikely(!pushIntoQueue)) {
                // might trigger an out-of-order return log error, should be harmless / todo
                freeQueryPoolTopOfPipe( queryPool );
                return;
            }
            std::lock_guard<dxvk::mutex> lock(m_mutex);
            m_queryQueue.push( {queryPool, submitId} );
            m_cond.notify_one();

        }

        void freeQueryPool(pacer_query_pool* queryPool) {

            assert( queryPool );
            m_queryPoolsBottomOfPipe.free( queryPool );

        }

    private:

        void freeQueryPoolTopOfPipe(pacer_query_pool* queryPool) {

            assert( queryPool );
            m_queryPoolsTopOfPipe.free( queryPool );

        }

        VkResult getQueryPoolResult( pacer_query_pool* queryPool, uint64_t* timestamp ) const {

            assert( queryPool );
            VkQueryResultFlags flags = VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT;
            VkResult res = m_device->m_vkProcs.vkGetQueryPoolResults(
              m_device->m_properties.vk_device, queryPool->pool, 0, 1, sizeof(uint64_t),
              timestamp, sizeof(uint64_t), flags
            );

            if (unlikely(res != VK_SUCCESS))
                ERR( "FramePacer: vkGetQueryPoolResults returned %u \n", res);

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

        void threadFunc() {

            while (!m_stopped.load( std::memory_order_acquire)) {

                QueueItem item;
                {   std::unique_lock<dxvk::mutex> lock(m_mutex);
                    m_cond.wait( lock, [this] {
                        return m_stopped.load() || !m_queryQueue.empty();
                    });

                    if (m_stopped.load())
                        return;

                    item = std::move(m_queryQueue.front());
                    m_queryQueue.pop();
                }

                assert( item.queryPool );
                uint64_t gpuTimestamp;
                getQueryPoolResult( item.queryPool, &gpuTimestamp );
                m_gpuExecutionStart[ item.submitId % NUM_SUBMITS ].store(gpuTimestamp);
                freeQueryPoolTopOfPipe( item.queryPool );

            }
        }

        static_assert(std::atomic<time_point>::is_always_lock_free);

        Device* m_device;

        // is accessed from multiple threads
        std::array<time_point, NUM_SUBMITS> m_submits = { };
        std::array<std::atomic<uint64_t>, NUM_SUBMITS> m_gpuExecutionStart = { };
        std::array<std::atomic<uint64_t>, NUM_SUBMITS> m_gpuExecutionEnd = { };
        std::atomic<uint64_t> m_submitCounter = { 1 };

        // holding the elements in a lockfree ringbuffer has the advantage
        // that we can check if we truely cycle through the pools in order
        // alternative would be a lockfree stack, which potentially could be
        // faster but we would miss this assertive checking
        VkCommandPool m_commandPool = { VK_NULL_HANDLE };

        sync::RingbufferAllocator<pacer_query_pool, 256> m_queryPoolsTopOfPipe;
        sync::RingbufferAllocator<pacer_query_pool, 256> m_queryPoolsBottomOfPipe;

        // threading for top of pipe timestamps

        struct QueueItem {
            pacer_query_pool* queryPool;
            uint64_t submitId;
        };

        std::atomic<bool> m_stopped = { false };
        dxvk::thread m_thread;
        dxvk::condition_variable m_cond;
        dxvk::mutex m_mutex;
        std::queue<QueueItem> m_queryQueue;

    };
}
