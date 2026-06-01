#ifndef __FRAMEPACER_BRIDGE_H
#define __FRAMEPACER_BRIDGE_H

#include <stdint.h>
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

struct PacerDevice {
    VkDevice device;
    uint32_t graphicsQueueFamilyIndex;
    float timestampPeriod;
    uint32_t timestampValidBits;
    bool khrCalibratedTimestamps;
    PFN_vkCreateQueryPool vkCreateQueryPool;
    PFN_vkGetQueryPoolResults vkGetQueryPoolResults;
    PFN_vkCreateCommandPool vkCreateCommandPool;
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
    PFN_vkBeginCommandBuffer vkBeginCommandBuffer;
    PFN_vkCmdResetQueryPool vkCmdResetQueryPool;
    PFN_vkCmdWriteTimestamp2 vkCmdWriteTimestamp2;
    PFN_vkEndCommandBuffer vkEndCommandBuffer;
    PFN_vkGetCalibratedTimestampsKHR vkGetCalibratedTimestampsKHR;
    PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR vkGetPhysicalDeviceCalibrateableTimeDomainsKHR;
};

struct PacerQueryPool {
    VkQueryPool     pool;
    VkCommandBuffer buffer;
};

typedef void* PacerHandle;
typedef uint64_t pacer_frame_id_t;

PacerHandle pacer_create( struct PacerDevice* device );
void pacer_destroy( PacerHandle handle );

void NvAPI_setSleepMode( bool enable );
void NvAPI_setLatencyMarker( PacerHandle handle, uint64_t frameID, VkLatencyMarkerNV marker );
void NvAPI_sleep( PacerHandle handle );

pacer_frame_id_t
     pacer_notify_submit( PacerHandle handle );
pacer_frame_id_t
     pacer_notify_present( PacerHandle handle );

void pacer_notify_queue_submit( PacerHandle handle, pacer_frame_id_t frameId );
void pacer_notify_queue_present( PacerHandle handle, pacer_frame_id_t frameId );

void pacer_notify_gpu_execution_end( PacerHandle handle, pacer_frame_id_t frameId, struct PacerQueryPool* queryPool );

// assumes presentation to single swapchain for now
void pacer_notify_gpu_present_end( PacerHandle handle, pacer_frame_id_t frameId );

struct PacerQueryPool* pacer_alloc_submit_query_pool( PacerHandle handle );


#ifdef __cplusplus
}
#endif

#endif
