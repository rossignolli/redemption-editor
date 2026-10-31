#pragma once

// SHA-256 (FIPS 180-4) in plain C++, for the build and file hashes of the log
// header and the QA report. It replaces the CryptoAPI of advapi32: hashing whole
// files through the system crypto provider is a pattern antivirus heuristics
// weigh as ransomware, and these hashes only identify files, they protect nothing.

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace replay::sha256
{
    class Hasher
    {
    public:
        Hasher() { Reset(); }

        void Reset()
        {
            static constexpr std::uint32_t kInit[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
            std::memcpy(m_state, kInit, sizeof(m_state));
            m_length = 0;
            m_used = 0;
        }

        void Update(const void* data, std::size_t size)
        {
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            m_length += size;
            if (m_used)
            {
                const std::size_t take = (64 - m_used) < size ? (64 - m_used) : size;
                std::memcpy(m_block + m_used, bytes, take);
                m_used += take;
                bytes += take;
                size -= take;
                if (m_used < 64)
                    return;
                Compress(m_block);
                m_used = 0;
            }
            while (size >= 64)
            {
                Compress(bytes);
                bytes += 64;
                size -= 64;
            }
            if (size)
            {
                std::memcpy(m_block, bytes, size);
                m_used = size;
            }
        }

        void Final(std::uint8_t digest[32])
        {
            const std::uint64_t bits = m_length * 8;
            const std::uint8_t one = 0x80;
            Update(&one, 1);
            const std::uint8_t zero = 0;
            while (m_used != 56)
                Update(&zero, 1);
            std::uint8_t tail[8];
            for (int n = 0; n < 8; ++n)
                tail[n] = static_cast<std::uint8_t>(bits >> (56 - 8 * n));
            Update(tail, 8);
            for (int n = 0; n < 8; ++n)
            {
                digest[n * 4 + 0] = static_cast<std::uint8_t>(m_state[n] >> 24);
                digest[n * 4 + 1] = static_cast<std::uint8_t>(m_state[n] >> 16);
                digest[n * 4 + 2] = static_cast<std::uint8_t>(m_state[n] >> 8);
                digest[n * 4 + 3] = static_cast<std::uint8_t>(m_state[n]);
            }
            Reset();
        }

    private:
        static std::uint32_t Rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

        void Compress(const std::uint8_t* block)
        {
            static constexpr std::uint32_t kRound[64] = {0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
                0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u,
                0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
                0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u,
                0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u,
                0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
                0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u,
                0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau,
                0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
            std::uint32_t w[64];
            for (int n = 0; n < 16; ++n)
                w[n] = (std::uint32_t(block[n * 4]) << 24) | (std::uint32_t(block[n * 4 + 1]) << 16) |
                       (std::uint32_t(block[n * 4 + 2]) << 8) | std::uint32_t(block[n * 4 + 3]);
            for (int n = 16; n < 64; ++n)
            {
                const std::uint32_t s0 = Rotr(w[n - 15], 7) ^ Rotr(w[n - 15], 18) ^ (w[n - 15] >> 3);
                const std::uint32_t s1 = Rotr(w[n - 2], 17) ^ Rotr(w[n - 2], 19) ^ (w[n - 2] >> 10);
                w[n] = w[n - 16] + s0 + w[n - 7] + s1;
            }
            std::uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
            std::uint32_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];
            for (int n = 0; n < 64; ++n)
            {
                const std::uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
                const std::uint32_t ch = (e & f) ^ (~e & g);
                const std::uint32_t t1 = h + s1 + ch + kRound[n] + w[n];
                const std::uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
                const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                const std::uint32_t t2 = s0 + maj;
                h = g;
                g = f;
                f = e;
                e = d + t1;
                d = c;
                c = b;
                b = a;
                a = t1 + t2;
            }
            m_state[0] += a;
            m_state[1] += b;
            m_state[2] += c;
            m_state[3] += d;
            m_state[4] += e;
            m_state[5] += f;
            m_state[6] += g;
            m_state[7] += h;
        }

        std::uint32_t m_state[8]{};
        std::uint64_t m_length = 0;
        std::uint8_t m_block[64]{};
        std::size_t m_used = 0;
    };

    inline std::string Hex(const std::uint8_t digest[32])
    {
        char hex[65]{};
        for (int n = 0; n < 32; ++n)
            std::snprintf(hex + n * 2, 3, "%02x", digest[n]);
        return hex;
    }

    inline std::string Hex(const void* data, std::size_t size)
    {
        Hasher hasher;
        hasher.Update(data, size);
        std::uint8_t digest[32];
        hasher.Final(digest);
        return Hex(digest);
    }

    // Reads an open file to its end and returns the lowercase hex digest, or
    // "unavailable" when a read fails. bytes is how much was read.
    inline std::string HashOpenFile(HANDLE file, std::uint64_t& bytes, std::size_t bufferSize)
    {
        bytes = 0;
        Hasher hasher;
        std::vector<std::uint8_t> buffer(bufferSize);
        DWORD read = 0;
        for (;;)
        {
            if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
                return "unavailable";
            if (read == 0)
                break;
            hasher.Update(buffer.data(), read);
            bytes += read;
        }
        std::uint8_t digest[32];
        hasher.Final(digest);
        return Hex(digest);
    }
}
