#ifndef __FRAMEPACER_BRIDGE_H
#define __FRAMEPACER_BRIDGE_H

#include <stdint.h>
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* PacerHandle;

PacerHandle* pacer_create();
void pacer_destroy( PacerHandle* handle );

void NvAPI_setLatencyMarker( PacerHandle* handle, uint64_t frameID, VkLatencyMarkerNV marker );
void NvAPI_sleep( PacerHandle* handle );

void pacer_spawn_frame( PacerHandle* handle );
void pacer_start_frame( PacerHandle* handle );
void pacer_end_frame( PacerHandle* handle, uint64_t nvId );

void pacer_notify_present( PacerHandle* handle, uint64_t frameId );
void pacer_notify_queue_present( PacerHandle* handle, uint64_t frameId );
void pacer_notify_gpu_execution_end( PacerHandle* handle, uint64_t frameId );

#ifdef __cplusplus
}
#endif

#endif
