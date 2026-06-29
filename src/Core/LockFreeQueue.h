/*
 * More-Phi — Core/LockFreeQueue.h
 * Multi-producer single-consumer lock-free ring buffer.
 * Producers (UI/MCP threads) are serialized via SpinLock. Consumer (audio thread) is lock-free.
 *
 * MPMC SAFETY: push() has an internal mutex so that multiple producer
 * threads (UI thread + MCP thread) can enqueue without corrupting the
 * ring-buffer indices.  pop() remains lock-free because there is only
 * ever one consumer (the audio thread).
 */
#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <iterator>
#include <type_traits>
#include <juce_core/juce_core.h>

namespace more_phi {

#if defined(_MSC_VER)
#pragma warning(push)
// Intentional cache-line alignment of indices can require padding in this type.
#pragma warning(disable: 4324)
#endif

template <typename T, size_t Capacity>
class LockFreeQueue
{
    static_assert(std::is_trivially_copyable_v<T>,
                  "LockFreeQueue requires trivially copyable types");
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of 2");
public:
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

    // MULTI-PRODUCER SAFETY: internal mutex serializes concurrent push() calls.
    // pop() remains lock-free (single consumer = audio thread only).
    [[nodiscard]] bool push(const T& item)
    {
        const juce::SpinLock::ScopedLockType guard(pushMutex_);
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) & mask_;
        if (next == tail_.load(std::memory_order_acquire))
            return false;  // full
        buffer_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // C-2 FIX: Atomic batch write under a single lock acquisition.
    // The old chunked implementation released and re-acquired pushMutex_
    // between 512-element fragments, leaving the queue in a partial-write
    // state on overflow (first N elements committed, rest silently dropped).
    // The new path checks that the FULL range fits, then copies all elements
    // atomically under one lock scope — either the entire batch lands or
    // nothing does. This also eliminates the chunk-loop overhead.

    template <typename Range>
    [[nodiscard]] bool pushRange(const Range& items)
    {
        static_assert(std::is_base_of_v<std::random_access_iterator_tag,
                      typename std::iterator_traits<decltype(std::begin(items))>::iterator_category>,
                      "LockFreeQueue::pushRange requires random-access iterators");
        const size_t count = static_cast<size_t>(std::distance(std::begin(items), std::end(items)));
        if (count == 0)
            return true;

        const juce::SpinLock::ScopedLockType guard(pushMutex_);
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_acquire);
        const size_t used = (head - tail) & mask_;
        const size_t available = usableCapacity() - used;

        if (count > available)
            return false;

        size_t write = head;
        for (auto it = std::begin(items); it != std::end(items); ++it)
        {
            buffer_[write] = *it;
            write = (write + 1) & mask_;
        }

        head_.store(write, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool pushRange(const T* items, std::size_t count)
    {
        if (count == 0)
            return true;

        const juce::SpinLock::ScopedLockType guard(pushMutex_);
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_acquire);
        const size_t used = (head - tail) & mask_;
        const size_t available = usableCapacity() - used;

        if (count > available)
            return false;

        size_t write = head;
        for (std::size_t i = 0; i < count; ++i)
        {
            buffer_[write] = items[i];
            write = (write + 1) & mask_;
        }

        head_.store(write, std::memory_order_release);
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
    mutable juce::SpinLock pushMutex_;  // H-3 FIX: SpinLock avoids kernel transition (priority inversion)
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace more_phi
