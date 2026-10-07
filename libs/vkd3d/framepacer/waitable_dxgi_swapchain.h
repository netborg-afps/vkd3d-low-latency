#pragma once

#include "util/sync/sync_atomic_signal.h"
#include "util/util_sleep.h"
#include <stdint.h>

namespace pacer {

    class WaitableDXGISwapchain {
    public:

        WaitableDXGISwapchain( Device* device, LatencyMarkersStorage& latencyMarkers, FrameSync& frameSync,
            std::function<void(uint64_t, dxvk::high_resolution_clock::time_point)> sleep )
        : m_device(device), m_latencyMarkers(latencyMarkers), m_frameSync(frameSync), m_sleep(std::move(sleep)),
          m_thread([this] { threadFunc(); }) { }

        ~WaitableDXGISwapchain() {

            m_stopped.store(true);
            m_signal.signal_one();
            m_thread.join();

            if (m_watchdogThread.joinable())
                m_watchdogThread.join();

        }

        void releaseSemaphore( void* vkd3d_swapchain, int count ) {

            vkd3d_native_sync_handle* latencyEvent = m_device->getLatencyEvent(vkd3d_swapchain);
            if (latencyEvent && vkd3d_native_sync_handle_is_valid(*latencyEvent))
                vkd3d_native_sync_handle_release(*latencyEvent, count);

        }

        void sleep( void* vkd3d_swapchain ) {

            if (m_device->m_activeType != Device::WaitableDXGISwapchain) {
                releaseSemaphore( vkd3d_swapchain, 3 );
                return;
            }

            if (!m_watchdogSpawned.load(std::memory_order_relaxed) && !m_watchdogSpawned.exchange(true))
                m_watchdogThread = dxvk::thread([this] { watchdogThreadFunc(); });

            ++m_presentCounter;

            vkd3d_native_sync_handle* latencyEvent = m_device->getLatencyEvent(vkd3d_swapchain);
            if (latencyEvent && vkd3d_native_sync_handle_is_valid(*latencyEvent)) {

                // we don't block here - in dxgi.present() - other than for some reason
                // the game isn't using its waitable swapchain handle such that it calls
                // two or more dxgi.present() in a row without attempting to decrement
                // the semaphore
                m_ready.wait();

                m_frameInfo.cpuId = m_frameSync.cpuFinished;
                m_frameInfo.latencyEvent = latencyEvent;

                m_signal.signal_one();

            }

            if ((m_presentCounter & (m_presentCounterPrint-1)) == 0) {

                m_presentCounterPrint <<= 1;
                uint64_t percentage = (m_presentCounterWaitableObject*100)/m_presentCounter;
                uint64_t percentage_decimal = ((m_presentCounterWaitableObject*1000)/m_presentCounter)%10;
                INFO( "waitable dxgi swapchain present percentage: %" PRIu64 ".%" PRIu64 "%%, total frames %" PRIu64 " \n",
                    percentage, percentage_decimal, m_presentCounter.load(std::memory_order_acquire) );

            }

        }

        bool isActive() {

            if (m_presentCounter < 32)
                return false;

            uint64_t percentage = (m_presentCounterWaitableObject*100)/m_presentCounter;
            if (percentage > 90)
                return true;

            return false;

        }


    private:

        void threadFunc() {

            while (!m_stopped.load(std::memory_order_acquire)) {

                m_signal.wait();
                if (m_stopped.load(std::memory_order_acquire))
                    return;

                vkd3d_native_sync_handle* latencyEvent = m_frameInfo.latencyEvent;

                uint64_t cpuId = m_frameInfo.cpuId;
                uint64_t newId = cpuId + 1;

                LatencyMarkers* m_new = m_latencyMarkers.getMarkers( newId );
                LatencyMarkers* m_prev = m_latencyMarkers.getMarkers( cpuId-1 );
                m_sleep( newId, m_prev->cpuFinished );

                bool usingWaitableSwapchain = true;
                ++m_presentCounterWaitableObject;
                if (WaitForSingleObject((*latencyEvent).handle, 0) == WAIT_OBJECT_0) {
                    usingWaitableSwapchain = false;
                    --m_presentCounterWaitableObject;
                }
                while (WaitForSingleObject((*latencyEvent).handle, 0) == WAIT_OBJECT_0)
                    { }
                vkd3d_native_sync_handle_release(*latencyEvent, 1);

                WatchdogContext context;
                context.latencyEvent = latencyEvent;
                context.t = dxvk::high_resolution_clock::now();
                {   std::lock_guard<dxvk::mutex> lock(m_watchdogMutex);
                    m_watchdogContext = context;
                }

                if (usingWaitableSwapchain) {
                    _INFO( "setting m_new->start \n" );
                    m_new->start = dxvk::high_resolution_clock::now();
                }

                m_ready.signal_one();

            }

        }

        void watchdogThreadFunc() {

            while (!m_stopped.load(std::memory_order_acquire)) {

                WatchdogContext context;
                {   std::lock_guard<dxvk::mutex> lock(m_watchdogMutex);
                    context = m_watchdogContext;
                }

                auto now = dxvk::high_resolution_clock::now();

                if (context.latencyEvent && now > context.t + std::chrono::milliseconds( 500 )) {
                    // Had been an issue when booting up Dead Space (remake) where shader compilation
                    // made some frames take 10+ seconds to complete. The Frostbite engine detects a timeout
                    // with a made-up "DXGI_ERROR_DEVICE_HUNG" when we don't give it a second cpu-sided in-flight frame.
                    WARN("detected slow frame, releasing waitable swapchain object temporarily to make game watchdogs happy \n");
                    vkd3d_native_sync_handle_release(*context.latencyEvent, 1);
                    std::lock_guard<dxvk::mutex> lock(m_watchdogMutex);
                    if (m_watchdogContext.t == context.t)
                        m_watchdogContext = WatchdogContext{};
                }

                dxvk::Sleep::sleepForCoarse( now, std::chrono::milliseconds( 200 ) );

            }

        }

        Device* m_device;
        LatencyMarkersStorage& m_latencyMarkers;
        FrameSync& m_frameSync;

        std::function<void(uint64_t, dxvk::high_resolution_clock::time_point)> m_sleep;

        struct FrameInfo {
            uint64_t cpuId;
            vkd3d_native_sync_handle* latencyEvent;
        };

        FrameInfo m_frameInfo;
        std::atomic<bool> m_stopped = { false };
        sync::AtomicSignal m_signal = { "dxgi::m_signal", false };
        sync::AtomicSignal m_ready  = { "dxgi::m_ready", true };

        std::atomic<uint64_t> m_presentCounter = { 0 };
        std::atomic<uint64_t> m_presentCounterWaitableObject = { 0 };
        uint64_t m_presentCounterPrint = { 32 };

        dxvk::thread m_thread;

        // watchdog

        using time_point = dxvk::high_resolution_clock::time_point;
        struct WatchdogContext {
            vkd3d_native_sync_handle* latencyEvent = { nullptr };
            time_point t = { };
        };

        WatchdogContext     m_watchdogContext;
        dxvk::mutex         m_watchdogMutex;
        dxvk::thread        m_watchdogThread;
        std::atomic<bool>   m_watchdogSpawned = { false };

    };


}
