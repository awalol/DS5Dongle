#pragma once

#include <cstdint>

#include "pico/critical_section.h"
#include "hardware/sync.h"

namespace port {

class CriticalSection {
public:
    void init() { critical_section_init(&cs_); }
    void enter() { critical_section_enter_blocking(&cs_); }
    void exit() { critical_section_exit(&cs_); }

private:
    critical_section_t cs_{};
};

using irq_state_t = uint32_t;

inline irq_state_t irq_save() { return save_and_disable_interrupts(); }
inline void irq_restore(irq_state_t state) { restore_interrupts(state); }

} // namespace port
