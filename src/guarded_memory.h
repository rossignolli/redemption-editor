#pragma once

// SEH-guarded engine memory access shared by every observer.
//
// Copy refuses, without touching memory, a range that can never be user
// memory: below 64 KB (the null region) or reaching past the canonical user
// space. Such a range would always fault, and the exception's dispatch runs
// every vectored handler in the process first (another mod's CLR handler can
// overflow the script fiber's stack on one). The audio observer passes a
// stricter floor (kGameHeapLowest: the game's heaps are never below 4 GB)
// through the `lowest` argument.
//
// MSVC: no object with a destructor may live in a function with __try, so
// these stay plain functions over raw pointers.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <windows.h>

namespace replay::mem
{
    constexpr std::uintptr_t kLowest = 0x10000;
    constexpr std::uintptr_t kUserEnd = 0x800000000000ull;
    constexpr std::uintptr_t kGameHeapLowest = 0x100000000ull; // audio_observer

    constexpr bool InUserRange(std::uintptr_t address, std::size_t size, std::uintptr_t lowest = kLowest)
    {
        return address >= lowest && address < kUserEnd && size <= kUserEnd - address;
    }

    // memcpy under SEH, no range filter (callers that already checked).
    inline bool CopyUnfiltered(void* out, const void* source, std::size_t size)
    {
        __try
        {
            std::memcpy(out, source, size);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    inline bool Copy(void* out, const void* source, std::size_t size, std::uintptr_t lowest = kLowest)
    {
        if (!size)
            return true;
        if (!InUserRange(reinterpret_cast<std::uintptr_t>(source), size, lowest))
            return false;
        return CopyUnfiltered(out, source, size);
    }

    inline bool Copy(void* out, std::uintptr_t source, std::size_t size, std::uintptr_t lowest = kLowest)
    {
        return Copy(out, reinterpret_cast<const void*>(source), size, lowest);
    }

    template <typename T>
    inline bool Read(std::uintptr_t address, T& out)
    {
        return Copy(&out, address, sizeof(T));
    }

    // Guarded write.
    inline bool Put(std::uintptr_t target, const void* source, std::size_t size)
    {
        if (!size)
            return true;
        if (!InUserRange(target, size))
            return false;
        __try
        {
            std::memcpy(reinterpret_cast<void*>(target), source, size);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Signature check of engine code (bytes at `target` equal `signature`).
    inline bool Matches(const void* target, const std::uint8_t* signature, std::size_t size)
    {
        if (!InUserRange(reinterpret_cast<std::uintptr_t>(target), size))
            return false;
        __try
        {
            return std::memcmp(target, signature, size) == 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}
