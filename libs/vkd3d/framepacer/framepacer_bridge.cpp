#include "framepacer_bridge.h"
#include "framepacer.h"
#include "nvapi_pacing_adapter.h"
#include "util/util_log.h"

#define PACER(x) ((NvApi_PacingAdapter*) x)
static std::atomic<bool> NvApi_sleepEnabled;

PacerHandle pacer_create( struct PacerDevice* device ) {
    INFO( "pacer_create start \n" );
    PacerHandle res = new NvApi_PacingAdapter( nullptr );
    INFO( "pacer_create done \n" );
    return res;
}

void pacer_destroy(PacerHandle handle) {
    INFO( "pacer_destroy start \n" );
    delete PACER(handle);
    INFO( "pacer_destroy done \n" );
}

void NvAPI_setSleepMode( bool enable ) {
    NvApi_sleepEnabled.store( enable );
}

void NvAPI_setLatencyMarker( PacerHandle handle, uint64_t frameID, VkLatencyMarkerNV marker ) {
    PACER(handle)->setLatencyMarker( frameID, marker );
}

void NvAPI_sleep( PacerHandle handle ) {
    if (NvApi_sleepEnabled)
        PACER(handle)->sleepAndBeginFrame();
}

pacer_frame_id_t pacer_notify_submit( PacerHandle handle ) {
    return PACER(handle)->notifySubmit();
}

pacer_frame_id_t pacer_notify_present( PacerHandle handle ) {
    return PACER(handle)->notifyPresent();
}

void pacer_notify_queue_submit( PacerHandle handle, pacer_frame_id_t pacerId ) {
    PACER(handle)->notifyQueueSubmit( pacerId );
}

void pacer_notify_queue_present( PacerHandle handle, pacer_frame_id_t pacerId ) {
    PACER(handle)->notifyQueuePresent( pacerId );
}

void pacer_notify_gpu_execution_end( PacerHandle handle, pacer_frame_id_t frameId, struct PacerQueryPool* queryPool ) {
    PACER(handle)->notifyGpuExecutionEnd( frameId, queryPool );
}

void pacer_notify_gpu_present_end( PacerHandle handle, pacer_frame_id_t frameId ) {
    // do nothing for now
}