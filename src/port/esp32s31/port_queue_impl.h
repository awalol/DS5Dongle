#pragma once

#include <cstddef>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace port {

class Queue {
public:
    void init(size_t element_size, size_t count) {
        q_ = xQueueCreate(count, element_size);
        // A null handle here means the queue silently swallows every element,
        // which on the audio path looks like glitching rather than a fault.
        configASSERT(q_ != nullptr);

        // Scratch destination for discard-oldest removals; see try_remove().
        scratch_ = heap_caps_malloc(element_size, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        configASSERT(scratch_ != nullptr);
    }

    // pico_util queues are callable from either context, so the ISR-safe
    // variant is selected at runtime rather than pushed onto callers.
    bool IRAM_ATTR try_add(const void *src) {
        if (xPortInIsrContext()) {
            BaseType_t woken = pdFALSE;
            const BaseType_t ok = xQueueSendFromISR(q_, src, &woken);
            if (woken) portYIELD_FROM_ISR();
            return ok == pdTRUE;
        }
        return xQueueSend(q_, src, 0) == pdTRUE;
    }

    // A null destination means "discard the oldest element". pico_util supports
    // that directly -- queue_remove_internal() guards its memcpy with
    // `if (data)` -- and seven call sites rely on it to drop samples when a
    // FIFO backs up (audio.cpp and bt.cpp's send_fifo).
    //
    // FreeRTOS has no discard primitive and instead asserts
    // `!(pvBuffer == NULL && uxItemSize != 0)`, so forwarding a null through
    // would abort on the first dropped frame -- or, with asserts compiled out,
    // memcpy to address 0. Drain into per-queue scratch instead.
    bool IRAM_ATTR try_remove(void *dst) {
        void *dest = (dst != nullptr) ? dst : scratch_;
        if (xPortInIsrContext()) {
            BaseType_t woken = pdFALSE;
            const BaseType_t ok = xQueueReceiveFromISR(q_, dest, &woken);
            if (woken) portYIELD_FROM_ISR();
            return ok == pdTRUE;
        }
        return xQueueReceive(q_, dest, 0) == pdTRUE;
    }

    size_t level() {
        return xPortInIsrContext() ? uxQueueMessagesWaitingFromISR(q_)
                                   : uxQueueMessagesWaiting(q_);
    }

    bool is_full() { return uxQueueSpacesAvailable(q_) == 0; }
    bool is_empty() { return level() == 0; }

private:
    QueueHandle_t q_ = nullptr;
    void *scratch_ = nullptr;
};

} // namespace port
