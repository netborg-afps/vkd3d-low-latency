#pragma once

#ifdef _WIN32
#include <synchapi.h>
#endif

#include <chrono>
#include <cstdint>
#include <atomic>
#include "../util_likely.h"
#include "../util_log.h"

namespace pacer {

    // in practice this is used with strictly monotonic increasing values
    // however, the interface would allow to set arbitrary values
    class TimelineSemaphore {

    public:

        TimelineSemaphore()
        : m_value(0) { }

        explicit TimelineSemaphore(uint64_t value)
        : m_value(value) { }

        operator uint64_t() const {
            return m_value.load(std::memory_order_acquire);
        }

        uint64_t load() const {
            return m_value.load(std::memory_order_acquire);
        }

        uint64_t operator++( int ) {
            uint64_t res = m_value.fetch_add(1, std::memory_order_acq_rel);
            WakeByAddressAll(&m_value);
            return res;
        }

        uint64_t operator=( uint64_t value ) {
            signal( value );
            return value;
        }

        void signal( uint64_t value ) {
            m_value.store(value, std::memory_order_release);
            WakeByAddressAll(&m_value);
        }

        void wait( uint64_t value ) {
            uint64_t cur;
            while (value > (cur = m_value.load(std::memory_order_acquire))) {
                WaitOnAddress(&m_value, &cur, sizeof(uint64_t), INFINITE);
            }
        }

        bool wait( uint64_t value, DWORD timeout ) {
            using time_point = std::chrono::steady_clock::time_point;
            using namespace std::chrono;

            uint64_t cur = m_value.load(std::memory_order_acquire);
            if (cur >= value)
                return true;

            time_point t = steady_clock::now();
            time_point target = t + milliseconds(timeout);

            while (cur < value && t < target) {
                DWORD remaining = duration_cast<milliseconds>(target-t).count();
                WaitOnAddress(&m_value, &cur, sizeof(uint64_t), remaining);
                cur = m_value.load(std::memory_order_acquire);
                t = steady_clock::now();
            }

            return cur >= value;
        }

        private:

            std::atomic<uint64_t> m_value;

    };

}
