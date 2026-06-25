#ifndef __FRAMEPACER_BRIDGE_H
#define __FRAMEPACER_BRIDGE_H

#include <stdint.h>
#include <vulkan/vulkan_core.h>
#include "vkd3d_d3d12.h"

#ifdef __cplusplus
extern "C" {
#endif

struct DXGI_SWAP_CHAIN_DESC1;

struct pacer_device_vk_procs {
    PFN_vkCreateQueryPool vkCreateQueryPool;
    PFN_vkDestroyQueryPool vkDestroyQueryPool;
    PFN_vkResetQueryPool vkResetQueryPool;
    PFN_vkCmdResetQueryPool vkCmdResetQueryPool;
    PFN_vkGetQueryPoolResults vkGetQueryPoolResults;
    PFN_vkCreateCommandPool vkCreateCommandPool;
    PFN_vkDestroyCommandPool vkDestroyCommandPool;
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
    PFN_vkFreeCommandBuffers vkFreeCommandBuffers;
    PFN_vkBeginCommandBuffer vkBeginCommandBuffer;
    PFN_vkEndCommandBuffer vkEndCommandBuffer;
    PFN_vkCmdWriteTimestamp2 vkCmdWriteTimestamp2;
    PFN_vkGetCalibratedTimestampsKHR vkGetCalibratedTimestampsKHR;
    PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR vkGetPhysicalDeviceCalibrateableTimeDomainsKHR;
};

struct pacer_device_properties {
    VkDevice vk_device;
    float timestamp_period;
    bool khrCalibratedTimestamps;
};

struct pacer_query_pool {
    VkQueryPool     pool;
    VkCommandBuffer buffer;
};

typedef struct pacer_device_opaque* pacer_device_handle;
typedef struct pacer_command_queue_opaque* pacer_command_queue_handle;
typedef struct pacer_vulkan_queue_opaque* pacer_vulkan_queue_handle;

struct pacer_vulkan_queue_info {
    uint32_t family_index;
    uint32_t queue_flags;
    uint32_t timestamp_valid_bits;
};

struct pacer_queues {
    pacer_command_queue_handle command_queue;
    pacer_vulkan_queue_handle vulkan_queue;
};

bool pacer_is_running();
pacer_device_handle pacer_create_device( struct pacer_device_properties* properties, struct pacer_device_vk_procs* vk_procs );
void pacer_destroy_device( pacer_device_handle handle );

struct pacer_queues pacer_register_queues( pacer_device_handle handle,
    void* command_queue, D3D12_COMMAND_LIST_TYPE type,
    void* vkd3d_queue, struct pacer_vulkan_queue_info vulkan_queue_info);

void pacer_register_swapchain( pacer_device_handle handle, void* vkd3d_swapchain, void* vkd3d_command_queue, DXGI_SWAP_CHAIN_DESC1 desc );
void pacer_unregister_swapchain( pacer_device_handle handle, void* vkd3d_swapchain );

void NvAPI_setSleepMode( pacer_device_handle handle, bool enable, UINT32 minimum_interval_us );
void NvAPI_setLatencyMarker( pacer_device_handle handle, uint64_t frameId, VkLatencyMarkerNV marker );
void NvAPI_sleep( pacer_device_handle handle );

uint64_t pacer_command_queue_notify_submit( pacer_command_queue_handle command_queue );
void     pacer_command_queue_notify_vulkan_submit( pacer_command_queue_handle command_queue, uint64_t command_submit_id, uint64_t vulkan_submit_id );

void     pacer_queue_notify_gpu_execution_end( struct pacer_queues pacer_queues, uint64_t command_submit_id, uint64_t vulkan_submit_id, struct pacer_query_pool* query_pool );

uint64_t pacer_vulkan_queue_notify_submit( pacer_vulkan_queue_handle vulkan_queue );
struct pacer_query_pool* pacer_vulkan_queue_alloc_query_pool( pacer_vulkan_queue_handle vulkan_queue );
struct pacer_query_pool* pacer_vulkan_queue_alloc_query_pool_top_of_pipe( pacer_vulkan_queue_handle vulkan_queue );
void pacer_vulkan_queue_free_query_pool( pacer_vulkan_queue_handle vulkan_queue,  struct pacer_query_pool* query_pool);
void pacer_vulkan_queue_push_query_pool_top_of_pipe( pacer_vulkan_queue_handle vulkan_queue, struct pacer_query_pool* query_pool, uint64_t vulkan_submit_id, bool push_into_queue );

uint64_t pacer_notify_present( pacer_device_handle device, void* vkd3d_swapchain );

//void pacer_notify_gpu_present_end( pacer_device_handle handle, uint64_t frameId );


#ifdef __cplusplus
}
#endif

#endif
