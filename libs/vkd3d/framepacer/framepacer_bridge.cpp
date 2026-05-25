#include "framepacer_bridge.h"
#include "framepacer.h"
#include "nvapi_pacing_adapter.h"
#include "util/util_log.h"

#define NV_PACER(x) (((Handle*) x)->nv_api_pacer)

struct Handle {
    NvApi_PacingAdapter nv_api_pacer;
};

void NvAPI_setLatencyMarker( PacerHandle* handle, uint64_t frameID, VkLatencyMarkerNV marker ) {
    if (handle) NV_PACER(handle).setLatencyMarker( frameID, marker );
}

void NvAPI_sleep( PacerHandle* handle ) {
    if (handle) NV_PACER(handle).sleepAndBeginFrame();
}

void pacer_end_frame( PacerHandle* handle, uint64_t nvId ) {
    if (handle) NV_PACER(handle).endFrame( nvId );
}

PacerHandle* pacer_create() {
    Handle* handle = new Handle();
    INFO("Create Pacer %" PRIu64 "\n", (uintptr_t) &NV_PACER(handle));
    return (PacerHandle*) handle;
}

void pacer_destroy( PacerHandle* handle ) {
    // todo
}

// void pacer_start_frame( PacerHandle* handle, uint64_t frameId ) {
//     INFO("pacer_start_frame %" PRIu64".\n", frameId);
//     pacer(handle)->sleepAndBeginFrame( frameId );
//     INFO("pacer_start_frame STARTED %" PRIu64".\n", frameId);
// }
//
// void pacer_notify_present( PacerHandle* handle, uint64_t frameId ) {
//     pacer(handle)->notifyPresent( frameId );
// }
//
// void pacer_notify_queue_present( PacerHandle* handle, uint64_t frameId ) {
//     pacer(handle)->notifyQueuePresentBegin( frameId );
// }
//
//
// void pacer_notify_gpu_execution_end( PacerHandle* handle, uint64_t frameId ) {
//     INFO("pacer_notify_gpu_execution_end %" PRIu64".\n", frameId);
//     pacer(handle)->notifyGpuExecutionEnd( frameId, nullptr );
// }


