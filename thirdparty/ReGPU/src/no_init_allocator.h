#pragma once
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

// Elements that are written right after the buffer grows: growing must not zero them first (resize's
// value-initialisation was a memset per recorded call, ~3% of the command processor; per segment log
// record, ~1.3% on an FX-8350).
template <typename T>
struct NoInitAllocator : std::allocator<T>
{
    template <typename U>
    struct rebind
    {
        using other = NoInitAllocator<U>;
    };
    NoInitAllocator() = default;
    template <typename U>
    NoInitAllocator(const NoInitAllocator<U>&) noexcept {}
    template <typename U>
    void construct(U* p) noexcept(std::is_nothrow_default_constructible_v<U>) { ::new (static_cast<void*>(p)) U; }
    template <typename U, typename... A>
    void construct(U* p, A&&... a) { ::new (static_cast<void*>(p)) U(std::forward<A>(a)...); }
};
