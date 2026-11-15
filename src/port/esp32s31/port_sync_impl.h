#pragma once

#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace port {

class CriticalSection {
public:
    void init() { mux_ = (portMUX_TYPE) portMUX_INITIALIZER_UNLOCKED; }
    void enter() { portENTER_CRITICAL_SAFE(&mux_); }
    void exit() { portEXIT_CRITICAL_SAFE(&mux_); }

private:
    // _SAFE variants pick the ISR-correct path at runtime, so a section may be
    // shared between task and ISR context the way the pico-sdk one was.
    portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};

using irq_state_t = uint32_t;

inline irq_state_t irq_save() { return portSET_INTERRUPT_MASK_FROM_ISR(); }
inline void irq_restore(irq_state_t state) { portCLEAR_INTERRUPT_MASK_FROM_ISR(state); }

} // namespace port
