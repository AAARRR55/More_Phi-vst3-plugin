/*
 * MorphSnap — Core/LockFreeQueue.h
 * Single-producer single-consumer lock-free ring buffer.
 */
#pragma once

#include <atomic>
#include <array>
#include <cstddef>

namespace morphsnap {

#if defined(_MSC_VER)
#pragma warning(push)
// Intentional cache-line alignment of indices can require padding in this type.
#pragma warning(disable: 4324)
#endif

template <typename T, size_t Capacity>
class LockFreeQueue
{
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of 2");
public:
    // SINGLE-PRODUCER SINGLE-CONSUMER ONLY.
    // Do NOT call push() from multiple threads simultaneously, or pop() from
    // multiple threads simultaneously. The ABA problem does not apply here
    // because only one thread ever writes head_ and one ever reads tail_.
    static constexpr size_t usableCapacity() noexcept
    {
        // Ring buffers that reserve one slot can store Capacity - 1 elements.
        return Capacity - 1;
    }

    size_t sizeApprox() const noexcept
    {
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t tail = tail_.load(std::memory_order_acquire);
        return (head - tail) & mask_;
    }

    size_t freeSpaceApprox() const noexcept
    {
        return usableCapacity() - sizeApprox();
    }

    [[nodiscard]] bool push(const T& item)
    {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) & mask_;
        if (next == tail_.load(std::memory_order_acquire))
            return false;  // full
        buffer_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool pop(T& item)
    {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return false;  // empty
        item = buffer_[tail];
        tail_.store((tail + 1) & mask_, std::memory_order_release);
        return true;
    }

    bool empty() const
    {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

private:
    static constexpr size_t mask_ = Capacity - 1;
    std::array<T, Capacity> buffer_{};
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace morphsnap
