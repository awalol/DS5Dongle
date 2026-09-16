#pragma once

#include <cstddef>
#include <cstdint>

#include "pico/util/queue.h"

namespace port {

class Queue {
public:
    void init(size_t element_size, size_t count) {
        queue_init(&q_, element_size, static_cast<uint>(count));
    }

    bool try_add(const void *src) { return queue_try_add(&q_, src); }
    bool try_remove(void *dst) { return queue_try_remove(&q_, dst); }

    size_t level() { return queue_get_level(&q_); }
    bool is_full() { return queue_is_full(&q_); }
    bool is_empty() { return queue_is_empty(&q_); }

private:
    queue_t q_{};
};

} // namespace port
