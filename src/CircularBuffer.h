//
// Created by awalol on 2026/4/23.
//

#ifndef DS5_BRIDGE_CIRCULAR_BUFFER_H
#define DS5_BRIDGE_CIRCULAR_BUFFER_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "pico/sync.h"

class CircularBuffer {
public:
    explicit CircularBuffer(size_t size);
    ~CircularBuffer();

    CircularBuffer(const CircularBuffer&) = delete;
    CircularBuffer& operator=(const CircularBuffer&) = delete;

    size_t Write(const uint8_t* data, size_t offset, size_t count);
    size_t Read(uint8_t* data, size_t offset, size_t count);

    size_t MaxLength() const;
    size_t Count() const;

    void Reset();
    void Advance(size_t count);

private:
    std::vector<uint8_t> buffer;
    mutable critical_section_t lockObject{};
    size_t writePosition = 0;
    size_t readPosition = 0;
    size_t byteCount = 0;

    void ResetInner();
};

#endif //DS5_BRIDGE_CIRCULAR_BUFFER_H
