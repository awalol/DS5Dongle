#pragma once

#include <cstdint>

namespace port {

// One-shot deferred callback.
//
// The callback runs off a timer context on both platforms (pico-sdk alarm pool
// / esp_timer), so it must be short and must not block. kNoTimer is the "not
// scheduled" value, chosen as 0 to match how the existing call sites test it.

using TimerId = int32_t;
constexpr TimerId kNoTimer = 0;

// Schedule `cb(ctx)` to run once after `delay_ms`. Returns kNoTimer on failure.
TimerId timer_once_ms(uint32_t delay_ms, void (*cb)(void *ctx), void *ctx);

// Cancel a pending timer. Safe to call with kNoTimer, and safe to call after
// the callback has already run.
void timer_cancel(TimerId id);

} // namespace port
