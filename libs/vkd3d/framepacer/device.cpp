#include "device.h"

#include <algorithm>
#include "command_queue.h"
#include "vulkan_queue.h"
#include "nvapi_pacing_adapter.h"
extern "C" {
#include "config_flags.h"
#include <vkd3d_d3d12.h>
}


namespace pacer {

    Device::Device( pacer_device_properties* properties, const pacer_device_vk_procs* vkProcs )
    : m_properties(*properties), m_vkProcs(*vkProcs), m_calibratedDeviceTimestamps(this) {
        m_nvApi_pacingAdapter = new NvApi_PacingAdapter(this);
        if (VKD3D_CONFIG_FLAG_IS_SET(PACER_DEBUG))
            enableLog = true;
    }

    Device::~Device( ) {
        for (CommandQueue* commandQueue : m_commandQueues)
            delete commandQueue;
        for (VulkanQueue* vulkanQueue : m_vulkanQueues)
            delete vulkanQueue;
        delete m_nvApi_pacingAdapter;
    }

    pacer_queues Device::registerQueues( void* vkd3d_command_queue,
        D3D12_COMMAND_LIST_TYPE type, void* vkd3d_queue,
        pacer_vulkan_queue_info vulkan_queue_info) {

        INFO( "command_queue register with type %u \n", type );

        pacer_queues queues;
        if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE
            && type != D3D12_COMMAND_LIST_TYPE_COPY) {
            queues.command_queue = nullptr;
            queues.vulkan_queue = nullptr;
            return queues;
        }

        unsigned int id;
        {   std::lock_guard<dxvk::mutex> lock(m_queueMutex);

            queues.vulkan_queue = nullptr;
            for (VulkanQueue* vulkanQueue : m_vulkanQueues) {
                if (vulkanQueue->m_properties.vkd3d_queue == vkd3d_queue) {
                    queues.vulkan_queue = (pacer_vulkan_queue_handle) vulkanQueue;
                    break;
                }
            }

            if (queues.vulkan_queue == nullptr) {
                VulkanQueue::Properties properties;
                properties.id = m_vulkanQueues.size();
                properties.vkd3d_queue = vkd3d_queue;
                properties.queueInfo = vulkan_queue_info;
                VulkanQueue* vulkanQueue = new VulkanQueue( this, properties );
                queues.vulkan_queue = (pacer_vulkan_queue_handle) vulkanQueue;
                m_vulkanQueues.push_back( vulkanQueue );
            }

            CommandQueue::Properties properties;
            properties.id = m_commandQueues.size(); id = properties.id;
            properties.type = type;
            properties.vkd3d_command_queue = vkd3d_command_queue;
            properties.vkd3d_queue = vkd3d_queue;
            CommandQueue* commandQueue = new CommandQueue( this, properties, (VulkanQueue*) queues.vulkan_queue );
            queues.command_queue = (pacer_command_queue_handle) commandQueue;
            m_commandQueues.push_back( commandQueue );
        }

        INFO( "command_queue %u (%" PRIu64 ") registered to pacer [%" PRIu64
            ", %" PRIu64 "] with type %u, vk_family_index %u and vk_queue_flags %u \n",
            id, (uintptr_t) queues.command_queue,
            (uintptr_t) vkd3d_command_queue, (uintptr_t) vkd3d_queue, type,
            vulkan_queue_info.family_index, vulkan_queue_info.queue_flags );

        return queues;
    }


    void Device::registerSwapchain( void* vkd3d_swapchain, void* vkd3d_command_queue, DXGI_SWAP_CHAIN_DESC1 desc ) {
        {   std::lock_guard<dxvk::mutex> lock(m_swapchainMutex);
            m_swapchains.push_back({vkd3d_swapchain, vkd3d_command_queue, desc});
            selectBestSwapchain_locked(); }
        INFO( "swapchain (%" PRIu64 ") registered to pacer \n", (uintptr_t) vkd3d_swapchain );
    }

    void Device::unregisterSwapchain( void* vkd3d_swapchain ) {
         {   std::lock_guard<dxvk::mutex> lock(m_swapchainMutex);
             auto it = findSwapchain_locked(vkd3d_swapchain);
             if (it != m_swapchains.end())
                 m_swapchains.erase(it);
             else { ERR( "pacer-swapchain not found \n" ); return; }
             if (!m_swapchains.empty()) selectBestSwapchain_locked();
             else { m_primaryCommandQueue = nullptr; m_activeSwapchain = nullptr; }
         }
         INFO( "swapchain (%" PRIu64 ") unregistered to pacer \n", (uintptr_t) vkd3d_swapchain );
    }

    void Device::getCommandQueues( std::vector<CommandQueue*>& outQueues ) {
        std::lock_guard<dxvk::mutex> lock(m_queueMutex);
        outQueues.clear();
        outQueues.insert(outQueues.end(), m_commandQueues.begin(), m_commandQueues.end());
    }

    void Device::getVulkanQueues( std::vector<VulkanQueue*>& outQueues ) {
        std::lock_guard<dxvk::mutex> lock(m_queueMutex);
        outQueues.clear();
        outQueues.insert(outQueues.end(), m_vulkanQueues.begin(), m_vulkanQueues.end());
    }

    void Device::selectBestSwapchain_locked() {
        for (auto it = m_swapchains.rbegin(); it != m_swapchains.rend(); ++it) {
            Swapchain& chain = *it;
            // filter out small ones which games uses for overlays
            // todo: to be robust, we need usage statistics though
            if (chain.desc.Width >= 800 && chain.desc.Height >= 600) {
                m_activeSwapchain = chain.vkd3d_swapchain;
                assignPrimaryCommandQueue( chain.vkd3d_command_queue );
                return;
            }
        }

        if (!m_swapchains.empty()) {
            m_activeSwapchain = m_swapchains.back().vkd3d_swapchain;
            assignPrimaryCommandQueue( m_swapchains.back().vkd3d_command_queue );
            return;
        }

        m_activeSwapchain = nullptr;
        m_primaryCommandQueue = nullptr;
    }

    void Device::assignPrimaryCommandQueue( void* vkd3d_command_queue ) {
        {   std::lock_guard<dxvk::mutex> lock(m_queueMutex);
            for (CommandQueue* commandQueue : m_commandQueues) {
                if (commandQueue->m_properties.vkd3d_command_queue == vkd3d_command_queue) {
                    m_primaryCommandQueue = commandQueue;
                    return;
                }
            }
        }
        m_primaryCommandQueue = nullptr;
        ERR( "could not assign the primary command queue! \n" );
    }

    Device::swapchain_iter Device::findSwapchain_locked( void* vkd3d_swapchain ) {
        auto targetSwapchain = [vkd3d_swapchain] (const Swapchain& chain) {
            return chain.vkd3d_swapchain == vkd3d_swapchain;
        };

        return std::find_if(m_swapchains.begin(), m_swapchains.end(), targetSwapchain);
    }
}
