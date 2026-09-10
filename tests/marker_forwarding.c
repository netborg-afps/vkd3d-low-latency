/*
 * Copyright 2026 The vkd3d-proton authors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define COBJMACROS
#include <vkd3d.h>
#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_API
#include "vkd3d_private.h"

#ifdef _WIN32
static HMODULE load_vulkan_loader(PFN_vkGetInstanceProcAddr *vkGetInstanceProcAddr)
{
    HMODULE module;

    if (!(module = LoadLibraryA("vulkan-1.dll")))
    {
        fprintf(stderr, "Failed to load vulkan-1.dll, error %lu.\n", GetLastError());
        return NULL;
    }

    if (!(*vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(module,
            "vkGetInstanceProcAddr")))
    {
        fprintf(stderr, "Failed to get vkGetInstanceProcAddr from vulkan-1.dll, error %lu.\n", GetLastError());
        FreeLibrary(module);
        return NULL;
    }

    return module;
}
#endif

struct marker_test
{
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool acquired;
    bool resume;
    unsigned int calls;
    uint64_t frame_id;
    VkLatencyMarkerNV marker;
    IDXGIVkSwapChain *chain;
    struct d3d12_device *device;
};

static struct marker_test *marker_test;

bool vkd3d_debug_control_is_test_suite(void)
{
    return true;
}

bool vkd3d_debug_control_explode_on_vvl_error(void)
{
    return false;
}

bool vkd3d_debug_control_has_out_of_spec_test_behavior(unsigned int behavior)
{
    (void)behavior;
    return false;
}

unsigned int vkd3d_debug_control_get_behavior_flags(void)
{
    return 0;
}

bool vkd3d_debug_control_mute_message_id(const char *vuid)
{
    (void)vuid;
    return false;
}

static void marker_test_pause(void *userdata)
{
    struct marker_test *test = userdata;

    pthread_mutex_lock(&test->mutex);
    /* The marker path has retained both objects and released the selection lock. */
    test->acquired = true;
    pthread_cond_broadcast(&test->cond);
    while (!test->resume)
        pthread_cond_wait(&test->cond, &test->mutex);
    pthread_mutex_unlock(&test->mutex);
}

static VKAPI_ATTR void VKAPI_CALL marker_test_set_latency_marker(VkDevice device,
        VkSwapchainKHR swapchain, const VkSetLatencyMarkerInfoNV *info)
{
    struct marker_test *test = marker_test;

    (void)device;
    (void)swapchain;
    pthread_mutex_lock(&test->mutex);
    ++test->calls;
    test->frame_id = info->presentID;
    test->marker = info->marker;
    dxgi_vk_swap_chain_test_marker_set_swapchain(test->chain, VK_NULL_HANDLE);
    pthread_mutex_unlock(&test->mutex);
}

static void *marker_test_thread(void *userdata)
{
    struct marker_test *test = userdata;

    ID3DLowLatencyDevice_SetLatencyMarker(&test->device->ID3DLowLatencyDevice_iface,
            UINT64_C(0x123456789abcdef0), VK_LATENCY_MARKER_RENDERSUBMIT_START_NV);
    return NULL;
}

static bool marker_forwarding_test(void)
{
    static const char * const device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    static const char * const instance_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME};
    struct vkd3d_instance_create_info instance_info =
    {
        .instance_extensions = instance_extensions,
        .instance_extension_count = ARRAY_SIZE(instance_extensions),
    };
    const struct vkd3d_device_create_info device_info =
    {
        .minimum_feature_level = D3D_FEATURE_LEVEL_11_0,
        .instance_create_info = &instance_info,
        .device_extensions = device_extensions,
        .device_extension_count = ARRAY_SIZE(device_extensions),
    };
    struct marker_test test = {0};
    struct d3d12_command_queue *queue = NULL;
    ID3D12CommandQueue *queue_iface = NULL;
    ID3D12Device *device = NULL;
    D3D12_COMMAND_QUEUE_DESC desc = {0};
    PFN_vkSetLatencyMarkerNV saved_marker = NULL;
    pthread_t thread;
    HRESULT hr;
    bool result = false;
    bool mutex_initialized = false;
    bool cond_initialized = false;
    bool marker_test_registered = false;
    bool marker_callback_set = false;
    bool thread_created = false;
#ifdef _WIN32
    HMODULE vulkan_module = NULL;

    if (!(vulkan_module = load_vulkan_loader(&instance_info.pfn_vkGetInstanceProcAddr)))
        return false;
#endif

    if (FAILED(vkd3d_create_device(&device_info, &IID_ID3D12Device, (void **)&device)))
        goto cleanup;
    if (FAILED(ID3D12Device_CreateCommandQueue(device, &desc,
            &IID_ID3D12CommandQueue, (void **)&queue_iface)))
        goto cleanup;

    test.device = impl_from_ID3D12Device((d3d12_device_iface *)device);
    queue = CONTAINING_RECORD(queue_iface, struct d3d12_command_queue, ID3D12CommandQueue_iface);
    if (!dxgi_vk_swap_chain_test_marker_init(queue, &test.chain))
        goto cleanup;

    if (pthread_mutex_init(&test.mutex, NULL))
        goto cleanup;
    mutex_initialized = true;
    if (pthread_cond_init(&test.cond, NULL))
        goto cleanup;
    cond_initialized = true;
    marker_test = &test;
    marker_test_registered = true;
    saved_marker = test.device->vk_procs.vkSetLatencyMarkerNV;
    dxgi_vk_swap_chain_test_marker_set_swapchain(test.chain, (VkSwapchainKHR)(uintptr_t)1);
    dxgi_vk_swap_chain_test_marker_register(test.device, test.chain);

    test.device->vk_info.NV_low_latency2 = false;
    hr = ID3DLowLatencyDevice_SetLatencyMarker(&test.device->ID3DLowLatencyDevice_iface,
            1, VK_LATENCY_MARKER_PRESENT_START_NV);
    result = hr == E_NOTIMPL && !test.calls;

    test.device->vk_info.NV_low_latency2 = true;
    test.device->vk_procs.vkSetLatencyMarkerNV = NULL;
    hr = ID3DLowLatencyDevice_SetLatencyMarker(&test.device->ID3DLowLatencyDevice_iface,
            2, VK_LATENCY_MARKER_PRESENT_START_NV);
    if (hr != S_OK || test.calls)
        result = false;

    test.device->vk_procs.vkSetLatencyMarkerNV = marker_test_set_latency_marker;
    dxgi_vk_swap_chain_test_marker_set_callback(marker_test_pause, &test);
    marker_callback_set = true;
    if (pthread_create(&thread, NULL, marker_test_thread, &test))
    {
        result = false;
        goto cleanup;
    }
    thread_created = true;

    pthread_mutex_lock(&test.mutex);
    while (!test.acquired)
        pthread_cond_wait(&test.cond, &test.mutex);
    pthread_mutex_unlock(&test.mutex);

cleanup:
    if (test.chain)
    {
        if (IDXGIVkSwapChain_Release(test.chain))
            result = false;
    }
    if (queue_iface)
    {
        if (ID3D12CommandQueue_Release(queue_iface) != (thread_created ? 1 : 0))
            result = false;
        queue_iface = NULL;
    }
    if (thread_created)
    {
        pthread_mutex_lock(&test.mutex);
        test.resume = true;
        pthread_cond_broadcast(&test.cond);
        pthread_mutex_unlock(&test.mutex);
        if (pthread_join(thread, NULL))
            result = false;
        if (test.calls != 1 || test.frame_id != UINT64_C(0x123456789abcdef0)
                || test.marker != VK_LATENCY_MARKER_RENDERSUBMIT_START_NV)
            result = false;
    }
    test.chain = NULL;
    if (marker_callback_set)
        dxgi_vk_swap_chain_test_marker_set_callback(NULL, NULL);
    if (marker_test_registered)
    {
        test.device->vk_procs.vkSetLatencyMarkerNV = saved_marker;
        marker_test = NULL;
    }
    if (cond_initialized && pthread_cond_destroy(&test.cond))
        result = false;
    if (mutex_initialized && pthread_mutex_destroy(&test.mutex))
        result = false;
    if (device && ID3D12Device_Release(device))
        result = false;
#ifdef _WIN32
    if (vulkan_module && !FreeLibrary(vulkan_module))
        result = false;
#endif
    return result;
}

int main(void)
{
    return marker_forwarding_test() ? 0 : 1;
}
