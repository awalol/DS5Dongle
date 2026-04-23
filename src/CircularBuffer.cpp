//
// Created by awalol on 2026/4/23.
// NAudio CircularBuffer convert to the C++ version by GPT5.4
//

#include "CircularBuffer.h"

#include <algorithm>
#include <cstring>

namespace {
class CriticalSectionLock {
public:
    explicit CriticalSectionLock(critical_section_t* lock) : lock(lock) {
        critical_section_enter_blocking(lock);
    }

    ~CriticalSectionLock() {
        critical_section_exit(lock);
    }

    CriticalSectionLock(const CriticalSectionLock&) = delete;
    CriticalSectionLock& operator=(const CriticalSectionLock&) = delete;

private:
    critical_section_t* lock;
};
}

CircularBuffer::CircularBuffer(size_t size) : buffer(size) {
    critical_section_init(&lockObject);
}

CircularBuffer::~CircularBuffer() {
    critical_section_deinit(&lockObject);
}

size_t CircularBuffer::Write(const uint8_t* data, size_t offset, size_t count) {
    if (data == nullptr || buffer.empty() || count == 0) {
        return 0;
    }

    CriticalSectionLock lock(&lockObject);

    count = std::min(count, buffer.size() - byteCount);
    const size_t firstLength = std::min(buffer.size() - writePosition, count);
    memcpy(buffer.data() + writePosition, data + offset, firstLength);
    writePosition = (writePosition + firstLength) % buffer.size();

    size_t written = firstLength;
    if (written < count) {
        const size_t secondLength = count - written;
        memcpy(buffer.data() + writePosition, data + offset + written, secondLength);
        writePosition += secondLength;
        written = count;
    }

    byteCount += written;
    return written;
}

size_t CircularBuffer::Read(uint8_t* data, size_t offset, size_t count) {
    if (data == nullptr || buffer.empty() || count == 0) {
        return 0;
    }

    CriticalSectionLock lock(&lockObject);

    count = std::min(count, byteCount);
    const size_t firstLength = std::min(buffer.size() - readPosition, count);
    memcpy(data + offset, buffer.data() + readPosition, firstLength);
    readPosition = (readPosition + firstLength) % buffer.size();

    size_t read = firstLength;
    if (read < count) {
        const size_t secondLength = count - read;
        memcpy(data + offset + read, buffer.data() + readPosition, secondLength);
        readPosition += secondLength;
        read = count;
    }

    byteCount -= read;
    return read;
}

size_t CircularBuffer::MaxLength() const {
    return buffer.size();
}

size_t CircularBuffer::Count() const {
    CriticalSectionLock lock(&lockObject);
    return byteCount;
}

void CircularBuffer::Reset() {
    CriticalSectionLock lock(&lockObject);
    ResetInner();
}

void CircularBuffer::ResetInner() {
    byteCount = 0;
    readPosition = 0;
    writePosition = 0;
}

void CircularBuffer::Advance(size_t count) {
    CriticalSectionLock lock(&lockObject);

    if (count >= byteCount) {
        ResetInner();
        return;
    }

    byteCount -= count;
    readPosition = (readPosition + count) % MaxLength();
}
