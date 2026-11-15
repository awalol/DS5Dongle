#pragma once

#include <cstddef>
#include <cstdint>

namespace port {

// Run `entry` on the second core, using the caller-provided static stack.
//
// Both backends pin the work to core 1 and use the supplied buffer as its
// stack, so the audio core's memory stays statically allocated and its
// high-water mark remains measurable (see debug_fill_core1_stack_watermark).
// `entry` is not expected to return.
void launch_worker(void (*entry)(), uint32_t *stack, size_t stack_bytes);

} // namespace port
