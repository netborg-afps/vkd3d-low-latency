#include "framepacer_bridge.h"
#include "device.h"
#include "command_queue.h"
#include "vulkan_queue.h"
#include "sleep_value_filter.h"
#include "util/util_log.h"
#include "util/util_debug.h"
#include "vkd3d_dxgi1_2.h"

using namespace pacer;

#define DEVICE(x) ((Device*) x)
#define COMMAND_QUEUE(x) ((CommandQueue*) x)
#define VULKAN_QUEUE(x) ((VulkanQueue*) x)
static std::atomic<bool> g_NvApi_sleepEnabled;
static SleepValueFilter g_sleepValueFilter;

bool pacer_is_running( pacer_device_handle device ) {
    assert(device);

    Device::type_t activeType = DEVICE(device)->m_activeType.load(std::memory_order_acquire);
    if (activeType == Device::NVIDIA_Reflex)
        return true;

    assert (activeType == Device::WaitableDXGISwapchain);
    return DEVICE(device)->m_pacer->m_waitableDxgiSwapchain.isActive();
}

pacer_device_handle pacer_create_device( struct pacer_device_properties* properties, struct pacer_device_vk_procs* vk_procs ) {
    Debug debug("pacer_create_device");
    if (!properties->khrCalibratedTimestamps) {
        ERR( "VK_KHR_calibrated_timestamps extension is required for frame-pacer device \n" );
        return nullptr;
    }
    return (pacer_device_handle) new Device(properties, vk_procs);
}

void pacer_destroy_device( pacer_device_handle handle ) {
    Debug debug("pacer_destroy_device");
    delete DEVICE(handle);
}

pacer_queues pacer_register_queues( pacer_device_handle handle,
        void* command_queue, D3D12_COMMAND_LIST_TYPE type,
        void* vkd3d_queue, struct pacer_vulkan_queue_info vulkan_queue_info) {
    assert(handle);
    return DEVICE(handle)->registerQueues(command_queue, type, vkd3d_queue, vulkan_queue_info);
}

void pacer_register_swapchain( pacer_device_handle handle, void* vkd3d_swapchain, void* vkd3d_command_queue, DXGI_SWAP_CHAIN_DESC1 desc, vkd3d_native_sync_handle* latency_event) {
    assert(handle);
    DEVICE(handle)->registerSwapchain(vkd3d_swapchain, vkd3d_command_queue, desc, latency_event);
}

void pacer_unregister_swapchain( pacer_device_handle handle, void* vkd3d_swapchain ) {
    assert(handle);
    DEVICE(handle)->unregisterSwapchain(vkd3d_swapchain);
}

uint64_t pacer_command_queue_notify_submit( pacer_command_queue_handle command_queue ) {
    assert(command_queue);
    return COMMAND_QUEUE(command_queue)->notifySubmit();
}

void pacer_command_queue_notify_vulkan_submit( pacer_command_queue_handle command_queue, uint64_t command_submit_id, uint64_t vulkan_submit_id ) {
    assert(command_queue);
    COMMAND_QUEUE(command_queue)->notifyVulkanSubmit(command_submit_id, vulkan_submit_id);
}


void pacer_queue_notify_gpu_execution_end( struct pacer_queues pacer_queues, uint64_t command_submit_id, uint64_t vulkan_submit_id, struct pacer_query_pool* query_pool ) {
    CommandQueue* commandQueue = COMMAND_QUEUE(pacer_queues.command_queue);
    VulkanQueue* vulkanQueue = VULKAN_QUEUE(pacer_queues.vulkan_queue);
    assert( commandQueue );
    assert( vulkanQueue );
    vulkanQueue->notifyGpuExecutionEnd(vulkan_submit_id, query_pool);
    commandQueue->notifyVulkanGpuExecutionEnd(command_submit_id, vulkan_submit_id);
}


uint64_t pacer_vulkan_queue_notify_submit( pacer_vulkan_queue_handle vulkan_queue ) {
    assert(vulkan_queue);
    return VULKAN_QUEUE(vulkan_queue)->notifySubmit();
}

struct pacer_query_pool* pacer_vulkan_queue_alloc_query_pool( pacer_vulkan_queue_handle vulkan_queue ) {
    assert(vulkan_queue);
    return VULKAN_QUEUE(vulkan_queue)->allocQueryPool();
}

void pacer_vulkan_queue_free_query_pool( pacer_vulkan_queue_handle vulkan_queue,  struct pacer_query_pool* query_pool) {
    assert(vulkan_queue);
    VULKAN_QUEUE(vulkan_queue)->freeQueryPool( query_pool );
}

struct pacer_query_pool* pacer_vulkan_queue_alloc_query_pool_top_of_pipe( pacer_vulkan_queue_handle vulkan_queue ) {
    assert(vulkan_queue);
    return VULKAN_QUEUE(vulkan_queue)->allocQueryPoolTopOfPipe();
}

void pacer_vulkan_queue_push_query_pool_top_of_pipe( pacer_vulkan_queue_handle vulkan_queue, struct pacer_query_pool* query_pool, uint64_t vulkan_submit_id, bool push_into_queue ) {
    assert(vulkan_queue);
    VULKAN_QUEUE(vulkan_queue)->pushQueryPoolTopOfPipe(query_pool, vulkan_submit_id, push_into_queue);
}

void NvAPI_setSleepMode( pacer_device_handle handle, bool enable, UINT32 minimum_interval_us ) {
    g_NvApi_sleepEnabled.store( enable, std::memory_order_release );
    DEVICE(handle)->m_activeType.store(
        enable ? Device::NVIDIA_Reflex : Device::WaitableDXGISwapchain,
        std::memory_order_release );

    // games (UE5!) seem to spam garbage which we need to filter
    g_sleepValueFilter.push(minimum_interval_us);
    uint32_t minInterval = g_sleepValueFilter.getMinInterval();

    if (handle)
        DEVICE(handle)->m_pacer->setFpsLimit(minInterval);
}

void NvAPI_setLatencyMarker( pacer_device_handle handle, uint64_t frameId, VkLatencyMarkerNV marker ) {
    assert(handle);
    DEVICE(handle)->m_nvApi_pacingAdapter->setLatencyMarker(frameId, marker);
}

void NvAPI_sleep( pacer_device_handle handle ) {
    assert(handle);
    if (g_NvApi_sleepEnabled)
        DEVICE(handle)->m_nvApi_pacingAdapter->sleepAndBeginFrame();
}

uint64_t pacer_notify_present( pacer_device_handle device, void* vkd3d_swapchain ) {
    assert(device);
    assert(vkd3d_swapchain);
    CommandQueue* commandQueue = DEVICE(device)->m_primaryCommandQueue;
    if (commandQueue && DEVICE(device)->m_activeSwapchain == vkd3d_swapchain) {
        commandQueue->notifyPresent(vkd3d_swapchain);
    }

    // return a frame_id when we'll make use of it (present_timing, present_wait, etc.)
    return 0;
}

void pacer_notify_aborted_present( pacer_device_handle device, void* vkd3d_swapchain ) {
    assert(device);
    assert(vkd3d_swapchain);

    DEVICE(device)->m_pacer->m_waitableDxgiSwapchain.releaseSemaphore( vkd3d_swapchain, 1 );
}
