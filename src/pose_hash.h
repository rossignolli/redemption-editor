#pragma once

// Hash of a pose buffer for the override write evidence (did the engine
// overwrite our last write?). A 64-bit word hash in four independent lanes;
// a byte-wise FNV-1a costs about 1 ns per byte, ~30 us per vehicle pose, on
// every finalizer call. Streamed, so a pose kept in two arrays (locals,
// objects) hashes like the engine buffer that holds them back to back. Only
// compared with itself in this process, never stored.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace replay::pose
{
    struct PoseHasher
    {
        static constexpr std::uint64_t kPrime = 1099511628211ull;
        std::uint64_t lane[4] = {1469598103934665603ull, 0x9E3779B97F4A7C15ull, 0xC2B2AE3D27D4EB4Full,
            0x165667B19E3779F9ull};
        std::uint64_t words = 0;
        std::uint64_t bytes = 0;

        // Pieces must be whole 8-byte words except the last one (poses are
        // 48 bytes per bone).
        void Add(const void* data, std::size_t size)
        {
            const std::uint8_t* at = static_cast<const std::uint8_t*>(data);
            std::size_t index = 0;
            const auto one = [&]() {
                std::uint64_t word = 0;
                std::memcpy(&word, at + index, sizeof(word));
                std::uint64_t& target = lane[words++ & 3];
                target = (target ^ word) * kPrime;
                index += 8;
            };
            while ((words & 3) != 0 && index + 8 <= size)
                one();
            // Four words at a time in registers (the same lanes and order).
            std::uint64_t l0 = lane[0], l1 = lane[1], l2 = lane[2], l3 = lane[3];
            for (; index + 32 <= size; index += 32)
            {
                std::uint64_t w[4];
                std::memcpy(w, at + index, sizeof(w));
                l0 = (l0 ^ w[0]) * kPrime;
                l1 = (l1 ^ w[1]) * kPrime;
                l2 = (l2 ^ w[2]) * kPrime;
                l3 = (l3 ^ w[3]) * kPrime;
                words += 4;
            }
            lane[0] = l0;
            lane[1] = l1;
            lane[2] = l2;
            lane[3] = l3;
            while (index + 8 <= size)
                one();
            for (; index < size; ++index)
                lane[0] = (lane[0] ^ at[index]) * kPrime;
            bytes += size;
        }

        std::uint64_t Finish() const
        {
            std::uint64_t hash = bytes;
            for (const std::uint64_t value : lane)
                hash = (hash ^ value ^ (value >> 29)) * kPrime;
            return hash;
        }
    };
}
