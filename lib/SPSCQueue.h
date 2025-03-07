#pragma once

// SPSCQueue.h
// -----------
// SPSCQueue defines a single producer single consumer lock free queue. The implementation is
// inspired by https://github.com/rigtorp/SPSCQueue.

#include <atomic>
#include <cassert>
#include <iostream>
#include <memory>           // std::allocator
#include <new>              // std::hardware_destructive_interference_size
#include <stdexcept>
#include <type_traits>      // std::enable_if, std::is_*_constructible
#include <vector>


template <typename T, typename Allocator = std::allocator<T>>
class SPSCQueue
{

#if defined(__cpp_if_constexpr) && defined(__cpp_lib_void_t)
    template <typename Alloc2, typename = void>
    struct has_allocate_at_least : std::false_type {};

    template <typename Alloc2> 
    struct has_allocate_at_least<
        Alloc2,
        std::void_t<
            typename Alloc2::value_type, 
            decltype(std::declval<Alloc2 &>().allocate_at_least(size_t{}))>> : std::true_type {};
#endif

#ifdef __cpp_lib_hardware_interference_size
    static constexpr size_t _cacheLineSize = std::hardware_destructive_interference_size;
#else
    static constexpr size_t _cacheLineSize = 64;
#endif

#if defined(__has_cpp_attribute) && __has_cpp_attribute(no_unique_address)
    Allocator _allocator [[no_unique_address]];
#else
    Allocator _allocator;
#endif 

    // Pad to avoid false sharing between slots and adjacent allocations
    static constexpr size_t _padding = (_cacheLineSize - 1) / sizeof(T) + 1;

    size_t _capacity;
    T *_slots; 

    // Align to cache line size in order to avoid false sharing. 
    alignas(_cacheLineSize) std::atomic<size_t> _writeIndex{0};
    alignas(_cacheLineSize) std::atomic<size_t> _readIndex{0};
    // Use to reduce the amount of cache coherenecy traffic 
    alignas(_cacheLineSize) size_t _writeIndexCache{0};
    alignas(_cacheLineSize) size_t _readIndexCache{0};

public:
    explicit SPSCQueue(const size_t capacity): _capacity(capacity) 
    {
        if (_capacity < 1)
        {
            _capacity = 1;
        }
        // Add one for slack
        _capacity++;
        // Prevent overflowing size_t
        if (_capacity > SIZE_MAX - 2 * _padding)
        {
            _capacity = SIZE_MAX - 2 * _padding;
        }

#if defined(__cpp_if_constexpr) && defined(__cpp_lib_void_t)
        if constexpr (has_allocate_at_least<Allocator>::value)
        {
            auto res = _allocator.allocate_at_least(_capacity + 2 * _padding);
            _slots = res.ptr; 
            _capacity = res.count - 2 * _padding; 
        }
        else
        {
            _slots = std::allocator_traits<Allocator>::allocate(_allocator, _capacity + 2 * _padding);
        }
#else
        _slots = std::allocator_traits<Allocator>::allocate(_allocator, _capacity + 2 * _padding);
#endif

        static_assert(alignof(SPSCQueue<T>) == _cacheLineSize);
        static_assert(sizeof(SPSCQueue<T>) >= 3 * _cacheLineSize);
        assert(reinterpret_cast<char *>(&_readIndex) - reinterpret_cast<char *>(&_writeIndex) >= static_cast<std::ptrdiff_t>(_cacheLineSize));
    }

    ~SPSCQueue()
    {
        while (front())
        {
            pop();
        }
    }

    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue &operator=(const SPSCQueue &) = delete; 

    template <typename... Args>
    void emplace(Args &&...args) 
    {
        static_assert(std::is_constructible<T, Args &&...>::value, "T is not constructible with Args");
        size_t const writeIndex = _writeIndex.load(std::memory_order_relaxed);
        size_t nextWriteIndex = writeIndex + 1; 
        if (nextWriteIndex == _capacity)
        {
            nextWriteIndex = 0;
        }

        while (nextWriteIndex == _readIndexCache)
        {
            _readIndexCache = _readIndex.load(std::memory_order_acquire);
        }

        new (&_slots[writeIndex + _padding]) T(std::forward<Args>(args)...);
        _writeIndex.store(nextWriteIndex, std::memory_order_release);
    }

    template <typename... Args>
    bool try_emplace(Args &&...args) noexcept(std::is_nothrow_constructible<T, Args &&...>::value)
    {
        static_assert(std::is_constructible<T, Args &&...>::value, "T is not constructible with Args");
        auto const writeIndex = _writeIndex.load(std::memory_order_relaxed);
        auto nextWriteIndex = writeIndex + 1;
        if (nextWriteIndex == _capacity)
        {
            nextWriteIndex = 0;
        }
        if (nextWriteIndex == _readIndexCache)
        {
            _readIndexCache = _readIndex.load(std::memory_order_acquire);
            if (nextWriteIndex == _readIndexCache)
            {
                return false;
            }
        }
        new (&_slots[writeIndex + _padding]) T(std::forward<Args>(args)...);
        _writeIndex.store(nextWriteIndex, std::memory_order_release);
        return true; 
    }

    void push(const T &v) noexcept(std::is_nothrow_copy_constructible<T>::value)
    {
        static_assert(std::is_copy_constructible<T>::value, "T must be copy constructible");
        emplace(v);
    }

    template <typename P, typename = typename std::enable_if<std::is_constructible<T, P &&>::value>::type>
    void push(P &&v) noexcept(std::is_nothrow_constructible<T, P &&>::value)
    {
        emplace(std::forward<P>(v));
    }

    bool try_push(const T &v) noexcept(std::is_nothrow_copy_constructible<T>::value)
    {
        static_assert(std::is_copy_constructible<T>::value, "T must be copy constructible");
        return try_emplace(v);   
    }

    T *front() noexcept
    {
        auto const readIndex = _readIndex.load(std::memory_order_relaxed);
        if (readIndex == _writeIndexCache)
        {
            _writeIndexCache = _writeIndex.load(std::memory_order_acquire);
            if (_writeIndexCache == readIndex)
            {
                return nullptr;
            }
        }
        return &_slots[readIndex + _padding];
    }

    void pop() noexcept
    {
        static_assert(std::is_nothrow_destructible<T>::value, "T must be nothrow destructible");
        auto const readIndex = _readIndex.load(std::memory_order_relaxed);
        assert(_writeIndex.load(std::memory_order_acquire) != readIndex && "Call pop() only after front() returns a valid pointer");
        _slots[readIndex + _padding].~T();
        auto nextReadIndex = readIndex + 1;
        if (nextReadIndex == _capacity)
        {
            nextReadIndex = 0;
        }
        _readIndex.store(nextReadIndex, std::memory_order_release);
    }

    size_t size() const noexcept
    {
        std::ptrdiff_t diff = _writeIndex.load(std::memory_order_acquire) - _readIndex.load(std::memory_order_acquire);
        if (diff < 0)
        {
            diff += _capacity;
        }
        return static_cast<size_t>(diff);
    }

    bool empty() const noexcept
    {
        return _writeIndex.load(std::memory_order_acquire) == _readIndex.load(std::memory_order_acquire);
    }

    size_t capacity() const noexcept { return _capacity - 1; }
};

