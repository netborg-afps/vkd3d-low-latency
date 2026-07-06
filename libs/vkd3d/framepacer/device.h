#pragma once
#include "framepacer_bridge.h"
#include "calibrated_device_timestamps.h"
#include "vkd3d_dxgi1_2.h"
#include "util/thread.h"
#include <vector>

namespace pacer {

    class CommandQueue;
    class VulkanQueue;
    class NvApi_PacingAdapter;
    class FramePacer;

    struct Swapchain {
        void* vkd3d_swapchain;
        void* vkd3d_command_queue;
        DXGI_SWAP_CHAIN_DESC1 desc;
        vkd3d_native_sync_handle* latency_event;
    };

    class Device {
    public:

        Device( pacer_device_properties* properties, const pacer_device_vk_procs* vkProcs );
        ~Device( );

        pacer_queues registerQueues( void* command_queue,
            D3D12_COMMAND_LIST_TYPE type, void* vkd3d_queue,
            pacer_vulkan_queue_info vulkan_queue_info);

        void registerSwapchain( void* vkd3d_swapchain, void* vkd3d_command_queue, DXGI_SWAP_CHAIN_DESC1 desc, vkd3d_native_sync_handle* latency_event );
        void unregisterSwapchain( void* vkd3d_swapchain );
        vkd3d_native_sync_handle* getLatencyEvent( void* vkd3d_swapchain );

        void getCommandQueues( std::vector<CommandQueue*>& outQueues );
        void getVulkanQueues( std::vector<VulkanQueue*>& outQueues );

        enum type_t : uint16_t {
            WaitableDXGISwapchain = 0,
            NVIDIA_Reflex = 1,
            AMD_AntiLag2 = 2
        };

        std::atomic< type_t > m_activeType = { WaitableDXGISwapchain };

        pacer_device_properties m_properties;
        pacer_device_vk_procs   m_vkProcs;
        CalibratedDeviceTimestamps m_calibratedDeviceTimestamps;

        NvApi_PacingAdapter* m_nvApi_pacingAdapter;

        // todo: make those two atomic together
        std::atomic< void* > m_activeSwapchain = { nullptr };
        std::atomic< CommandQueue* > m_primaryCommandQueue = { nullptr };

        FramePacer* m_pacer;

    private:

        typedef std::vector< Swapchain >::iterator swapchain_iter;
        void selectBestSwapchain_locked();
        swapchain_iter findSwapchain_locked( void* vkd3d_swapchain );
        void assignPrimaryCommandQueue( void* vkd3d_command_queue );

        std::vector< Swapchain > m_swapchains;
        std::vector< VulkanQueue* > m_vulkanQueues;
        std::vector< CommandQueue* > m_commandQueues;
        dxvk::mutex m_queueMutex;
        dxvk::mutex m_swapchainMutex;
    };
}
