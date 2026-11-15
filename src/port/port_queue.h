#pragma once

#include <cstddef>

// Fixed-capacity, fixed-element-size SPSC/MPMC queue used to hand audio and
// report buffers between the two cores.
//
// Contract (matches pico_util queue_t, which the existing call sites assume):
//   - try_add/try_remove never block and never allocate;
//   - both are safe to call from ISR context as well as task context;
//   - elements are copied by value, element_size bytes at a time.
//
// port_queue.h is on the audio hot path. cmake/relocate_to_ram.cmake already
// pulls pico_util's queue add/remove into RAM; the wrappers here are inline so
// that relocation still applies.

#if defined(PORT_PLATFORM_PICO)
#include "pico/port_queue_impl.h"
#elif defined(PORT_PLATFORM_ESP32S31)
#include "esp32s31/port_queue_impl.h"
#endif
