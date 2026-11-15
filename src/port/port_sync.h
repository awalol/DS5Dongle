#pragma once

// Cross-core mutual exclusion.
//
// port::CriticalSection is an SMP-safe spinlock that also masks interrupts on
// the calling core — matching pico-sdk critical_section_t semantics, which the
// audio and BT paths rely on to guard state shared with the second core.
//
// It is NOT recursive on either platform: taking the same section twice on one
// core deadlocks.
//
// port::irq_save()/irq_restore() mask interrupts on the calling core only, with
// no cross-core exclusion. Use a CriticalSection unless you specifically need
// the bare mask (flash writes do).

#if defined(PORT_PLATFORM_PICO)
#include "pico/port_sync_impl.h"
#elif defined(PORT_PLATFORM_ESP32S31)
#include "esp32s31/port_sync_impl.h"
#endif

namespace port {

// RAII guard; prefer this over manual enter/exit at new call sites.
class CriticalGuard {
public:
    explicit CriticalGuard(CriticalSection &cs) : cs_(cs) { cs_.enter(); }
    ~CriticalGuard() { cs_.exit(); }
    CriticalGuard(const CriticalGuard &) = delete;
    CriticalGuard &operator=(const CriticalGuard &) = delete;

private:
    CriticalSection &cs_;
};

} // namespace port
