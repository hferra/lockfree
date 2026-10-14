#pragma once

#include <sys/mman.h>
#include <stdexcept>
#include <system_error>

class ScopedMemoryLock {
private:
    const void* m_addr;
    std::size_t m_size;
    bool m_locked;

public:
    // Acquire resource upon construction
    ScopedMemoryLock(const void* addr, std::size_t size, bool fail_hard = true) 
        : m_addr(addr)
        , m_size(size)
        , m_locked(false) 
    {
        if (::mlock(m_addr, m_size) == 0) {
            m_locked = true;
        } else {
            // Capture the exact OS kernel error code (e.g., EPERM for lack of privileges)
            int err = errno; 
            std::cerr << "CRITICAL ERROR: RAII mlock failed to pin " << m_size 
                      << " bytes at " << m_addr << ". OS Error: " 
                      << std::generic_category().message(err) << std::endl;

            if (fail_hard) {
                // Instantly crash the app with a clean core dump for debugging.
                // In HFT, running unpinned is worse than not running at all.
                std::terminate(); 
            }
        }
    }

    // Automatically release resource upon destruction (RAII)
    ~ScopedMemoryLock() noexcept {
        if (m_locked) {
            ::munlock(m_addr, m_size);
        }
    }

    // Prevent copying to avoid double-unlocking bugs
    ScopedMemoryLock(const ScopedMemoryLock&) = delete;
    ScopedMemoryLock& operator=(const ScopedMemoryLock&) = delete;

    // Allow moving if you need to transfer ownership across scopes
    ScopedMemoryLock(ScopedMemoryLock&& other) noexcept 
        : m_addr(other.m_addr)
        , m_size(other.m_size)
        , m_locked(other.m_locked) 
    {
        other.m_locked = false; // Nullify the old instance
    }

    [[nodiscard]] bool is_locked() const noexcept { return m_locked; }
};
