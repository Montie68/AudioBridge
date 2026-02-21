#pragma once
// ring_buffer.h - Lock-free single-producer / single-consumer ring buffer.
//
// Template parameter T is the element type (typically float for audio samples).
// The capacity is rounded up to the next power of two so that index masking is
// a simple bitwise AND -- no modulo required.

#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <type_traits>

template <typename T>
class RingBuffer {
    static_assert(std::is_trivially_copyable_v<T>,
                  "RingBuffer only supports trivially-copyable types");

public:
    // Construct a ring buffer that can hold *at least* `min_capacity` elements.
    // The actual capacity will be the next power of two >= min_capacity.
    explicit RingBuffer(size_t min_capacity)
        : mask_(next_power_of_two(min_capacity) - 1)
        , buffer_(std::make_unique<T[]>(mask_ + 1))
        , head_(0)
        , tail_(0)
    {
    }

    // Non-copyable, non-movable (atomic members).
    RingBuffer(const RingBuffer&)            = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&&)                 = delete;
    RingBuffer& operator=(RingBuffer&&)      = delete;

    // ---- Producer API (single writer thread) ----

    // Push up to `count` elements from `data` into the buffer.
    // Returns the number of elements actually written (may be less than
    // `count` if the buffer is full).
    size_t push(const T* data, size_t count) {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t t = tail_.load(std::memory_order_acquire);
        const size_t avail = available_write_impl(h, t);
        if (avail == 0) return 0;

        const size_t to_write = (count < avail) ? count : avail;
        const size_t capacity = mask_ + 1;
        const size_t head_index = h & mask_;

        // How many elements fit before wrapping?
        const size_t first = capacity - head_index;
        if (to_write <= first) {
            std::memcpy(buffer_.get() + head_index, data, to_write * sizeof(T));
        } else {
            std::memcpy(buffer_.get() + head_index, data,          first * sizeof(T));
            std::memcpy(buffer_.get(),               data + first, (to_write - first) * sizeof(T));
        }

        // Publish the new head.  Release so the consumer sees the written data.
        head_.store(h + to_write, std::memory_order_release);
        return to_write;
    }

    // ---- Consumer API (single reader thread) ----

    // Pop up to `count` elements into `data`.
    // Returns the number of elements actually read (may be less than `count`
    // if the buffer doesn't hold that many).
    size_t pop(T* data, size_t count) {
        const size_t t = tail_.load(std::memory_order_relaxed);
        const size_t h = head_.load(std::memory_order_acquire);
        const size_t avail = available_read_impl(h, t);
        if (avail == 0) return 0;

        const size_t to_read = (count < avail) ? count : avail;
        const size_t capacity = mask_ + 1;
        const size_t tail_index = t & mask_;

        const size_t first = capacity - tail_index;
        if (to_read <= first) {
            std::memcpy(data, buffer_.get() + tail_index, to_read * sizeof(T));
        } else {
            std::memcpy(data,         buffer_.get() + tail_index, first * sizeof(T));
            std::memcpy(data + first, buffer_.get(),              (to_read - first) * sizeof(T));
        }

        // Publish the new tail.
        tail_.store(t + to_read, std::memory_order_release);
        return to_read;
    }

    // ---- Query (safe to call from either side) ----

    size_t available_read() const {
        const size_t h = head_.load(std::memory_order_acquire);
        const size_t t = tail_.load(std::memory_order_acquire);
        return available_read_impl(h, t);
    }

    size_t available_write() const {
        const size_t h = head_.load(std::memory_order_acquire);
        const size_t t = tail_.load(std::memory_order_acquire);
        return available_write_impl(h, t);
    }

    size_t capacity() const { return mask_ + 1; }

private:
    static size_t next_power_of_two(size_t v) {
        if (v == 0) return 1;
        v--;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        v |= v >> 32;
        return v + 1;
    }

    size_t available_read_impl(size_t head, size_t tail) const {
        return head - tail; // works with unsigned wrap-around
    }

    size_t available_write_impl(size_t head, size_t tail) const {
        return (mask_ + 1) - (head - tail);
    }

    const size_t                mask_;
    std::unique_ptr<T[]>        buffer_;
    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;
};
