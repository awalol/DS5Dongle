//
// Created by Codex on 2026/7/7.
//

#include "port/port.h"
#include "debug.h"

#if ENABLE_DEBUG

#include <cstdio>


static constexpr uint32_t DEBUG_STACK_CANARY = 0xA5A5A5A5u;
static constexpr uint64_t DEBUG_STACK_LOG_PERIOD_US = 5'000'000;

static uint32_t *debug_core1_stack = nullptr;
static uint32_t debug_core1_stack_words = 0;
static bool debug_core1_stack_watermark_active = false;

void debug_fill_core1_stack_watermark(uint32_t *stack, uint32_t word_count) {
    if (stack == nullptr || word_count == 0) {
        return;
    }

    for (uint32_t i = 0; i < word_count; i++) {
        stack[i] = DEBUG_STACK_CANARY;
    }

    debug_core1_stack = stack;
    debug_core1_stack_words = word_count;
    debug_core1_stack_watermark_active = true;
}

static uint32_t debug_core1_stack_used_bytes() {
    if (!debug_core1_stack_watermark_active) {
        return 0;
    }

    const volatile uint32_t *stack = debug_core1_stack;
    uint32_t unused_words = 0;
    while (unused_words < debug_core1_stack_words && stack[unused_words] == DEBUG_STACK_CANARY) {
        unused_words++;
    }

    return (debug_core1_stack_words - unused_words) * sizeof(debug_core1_stack[0]);
}

void debug_log_core1_stack_usage() {
    if (!debug_core1_stack_watermark_active) {
        return;
    }

    static uint64_t next_log_us = 0;
    const uint64_t now = port::now_us();
    if (now < next_log_us) {
        return;
    }
    next_log_us = now + DEBUG_STACK_LOG_PERIOD_US;

    const uint32_t used = debug_core1_stack_used_bytes();
    const uint32_t total = debug_core1_stack_words * sizeof(debug_core1_stack[0]);
    printf("[Audio] core1 stack used %lu / %lu bytes, free %lu bytes\n",
           static_cast<unsigned long>(used),
           static_cast<unsigned long>(total),
           static_cast<unsigned long>(total - used));
}

#endif
