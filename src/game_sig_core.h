#pragma once

// Code signatures: the bytes of the game code the mod relies on, with the
// bytes that hold an address (a call's rel32, a RIP-relative displacement)
// as wildcards: "48 8B 05 ?? ?? ?? ?? 81 E1". Between game builds (and
// between store builds) a function keeps its bytes and only its address and
// the addresses inside it change, so the signature finds it again. A
// function that really changed (another layout, other offsets) does not
// match, and the feature that needs it stays off.
//
// This header is the pure part: the pattern text, the match at a buffer, the
// one-pass search of many patterns, and the list every Signature joins when
// it is constructed (unit tested in tests/game_sig_tests.cpp). game_sig.h
// resolves them in the running game.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace replay::sig
{
    inline constexpr std::size_t kMaxPatternBytes = 256;
    // The bytes another module's detour writes at a function's start (a
    // 14-byte jmp [rip]; MinHook's is 5): the signature of a function the mod
    // hooks leaves them out, so the function is still found when another
    // mod hooked it first.
    inline constexpr std::size_t kHookBytes = 14;
    inline constexpr std::uint32_t kNoEntry = 0xFFFFFFFFu;

    struct Pattern
    {
        std::uint8_t bytes[kMaxPatternBytes] = {};
        bool wild[kMaxPatternBytes] = {};
        std::size_t size = 0;
        // Two adjacent fixed bytes at `key`: what the search indexes. The
        // pair least likely in x64 code, so few places get a full compare.
        // A pattern without such a pair is only checked at an address.
        std::size_t key = 0;
        bool hasKey = false;
    };

    // How common a byte is in the game's code, in hundredths of a percent
    // (measured on the 1491.50 .text: 00 is 8.7%, 48 7.7%, 8B 5.1%...); 16
    // (the median) for every byte not listed.
    inline std::uint32_t ByteWeight(std::uint8_t b)
    {
        switch (b)
        {
        case 0x00: return 871;
        case 0x48: return 768;
        case 0x8B: return 508;
        case 0x0F: return 450;
        case 0xFF: return 199;
        case 0x01: return 191;
        case 0xE8: return 186;
        case 0x24: return 186;
        case 0x89: return 176;
        case 0x8D: return 169;
        case 0x41: return 165;
        case 0x83: return 160;
        case 0x44: return 135;
        case 0xC0: return 129;
        case 0x74: return 122;
        case 0x05: return 114;
        case 0x10: return 113;
        case 0x03: return 112;
        case 0x02: return 109;
        case 0x4C: return 102;
        case 0x20: return 102;
        case 0x28: return 98;
        case 0x85: return 89;
        case 0x45: return 86;
        case 0x49: return 84;
        case 0xF3: return 83;
        case 0x08: return 82;
        case 0x40: return 77;
        case 0xC3: return 74;
        default: return 16;
        }
    }

    // "48 8B ?? 05" (a wildcard is "?" or "??", hex in either case, spaces
    // between bytes). False on a malformed text, more than kMaxPatternBytes
    // bytes, or no fixed byte at all.
    inline bool Parse(const char* text, Pattern& out)
    {
        out.size = 0;
        out.key = 0;
        out.hasKey = false;
        if (!text)
            return false;
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            return -1;
        };
        const char* p = text;
        while (*p)
        {
            if (*p == ' ')
            {
                ++p;
                continue;
            }
            if (out.size >= kMaxPatternBytes)
                return false;
            if (p[0] == '?')
            {
                p += p[1] == '?' ? 2 : 1;
                out.wild[out.size] = true;
                out.bytes[out.size] = 0;
            }
            else
            {
                const int hi = nibble(p[0]);
                const int lo = hi < 0 ? -1 : nibble(p[1]);
                if (lo < 0)
                    return false;
                out.wild[out.size] = false;
                out.bytes[out.size] = static_cast<std::uint8_t>(hi * 16 + lo);
                p += 2;
            }
            ++out.size;
            if (*p && *p != ' ')
                return false;
        }
        bool anyFixed = false;
        std::uint64_t best = ~0ull;
        for (std::size_t n = 0; n < out.size; ++n)
        {
            anyFixed = anyFixed || !out.wild[n];
            if (n + 1 >= out.size || out.wild[n] || out.wild[n + 1])
                continue;
            const std::uint64_t score = static_cast<std::uint64_t>(ByteWeight(out.bytes[n])) * ByteWeight(out.bytes[n + 1]);
            if (score < best)
            {
                best = score;
                out.key = n;
                out.hasKey = true;
            }
        }
        return anyFixed;
    }

    // A pattern whose first kHookBytes bytes are all wildcards: the
    // signature of a function the mod hooks.
    inline bool HookTolerant(const char* text)
    {
        Pattern pattern;
        if (!Parse(text, pattern) || pattern.size <= kHookBytes)
            return false;
        for (std::size_t n = 0; n < kHookBytes; ++n)
            if (!pattern.wild[n])
                return false;
        return true;
    }

    // The code starts with a jump another module's detour writes: jmp rel32
    // (E9) or jmp [rip + x] (FF 25).
    inline bool StartsWithJump(const std::uint8_t* code, std::size_t available)
    {
        return code && available >= 2 && (code[0] == 0xE9 || (code[0] == 0xFF && code[1] == 0x25));
    }

    inline bool MatchesAt(const Pattern& pattern, const std::uint8_t* code, std::size_t available)
    {
        if (!code || pattern.size == 0 || pattern.size > available)
            return false;
        for (std::size_t n = 0; n < pattern.size; ++n)
            if (!pattern.wild[n] && code[n] != pattern.bytes[n])
                return false;
        return true;
    }

    // Where a pattern matched: how many times (counting stops at 2, which is
    // already "not unique") and the offset of the first match.
    struct Hits
    {
        std::uint32_t count = 0;
        std::uint64_t first = 0;
    };

    // The patterns of one search, indexed by their key pair: one pass over
    // the code compares each pattern only where its pair is.
    class ScanIndex
    {
    public:
        explicit ScanIndex(const Pattern* const* patterns, std::size_t count)
            : m_patterns(patterns, patterns + count), m_head(65536, kNoEntry)
        {
            m_entries.reserve(count);
            for (std::size_t n = 0; n < count; ++n)
            {
                const Pattern& p = *patterns[n];
                if (!p.hasKey)
                    continue;
                const std::uint32_t pair = p.bytes[p.key] | (static_cast<std::uint32_t>(p.bytes[p.key + 1]) << 8);
                m_entries.push_back({static_cast<std::uint32_t>(n), m_head[pair]});
                m_head[pair] = static_cast<std::uint32_t>(m_entries.size() - 1);
            }
        }

        std::size_t Count() const { return m_patterns.size(); }

        // Adds the matches in [data, data + size) to `hits` (one per pattern,
        // in the index's order); `origin` is the offset reported for data[0],
        // so the code sections of one image can be searched one after another.
        // No allocation: the caller may run it under SEH.
        void Scan(const std::uint8_t* data, std::size_t size, std::uint64_t origin, Hits* hits) const
        {
            if (!data || size < 2)
                return;
            for (std::size_t i = 0; i + 1 < size; ++i)
            {
                std::uint32_t e = m_head[data[i] | (static_cast<std::uint32_t>(data[i + 1]) << 8)];
                while (e != kNoEntry)
                {
                    const Entry& entry = m_entries[e];
                    e = entry.next;
                    const Pattern& p = *m_patterns[entry.pattern];
                    if (i < p.key)
                        continue;
                    const std::size_t start = i - p.key;
                    if (!MatchesAt(p, data + start, size - start))
                        continue;
                    Hits& h = hits[entry.pattern];
                    if (h.count == 0)
                        h.first = origin + start;
                    if (h.count < 2)
                        ++h.count;
                }
            }
        }

    private:
        struct Entry
        {
            std::uint32_t pattern;
            std::uint32_t next;
        };
        std::vector<const Pattern*> m_patterns;
        std::vector<std::uint32_t> m_head;
        std::vector<Entry> m_entries;
    };

    // The target of a rel32/disp32: the instruction at `instruction` is
    // `length` bytes long and its displacement counts from its end.
    inline std::int64_t Rel32Target(std::int64_t instruction, std::uint32_t length, std::int32_t displacement)
    {
        return instruction + static_cast<std::int64_t>(length) + displacement;
    }

    // ------------------------------------------------------------ signatures

    enum class Result : std::uint8_t
    {
        Pending,   // not resolved yet
        Hint,      // the known exe: matched at its 1491.50 address
        Scan,      // found once by the search
        Derived,   // reached from its parent and checked
        Missing,   // not found (or its parent was not): the feature stays off
        Ambiguous, // found more than once: the feature stays off
        Bad,       // the pattern text does not parse (a bug of the mod)
    };

    inline const char* ResultName(Result r)
    {
        switch (r)
        {
        case Result::Pending: return "pending";
        case Result::Hint: return "hint";
        case Result::Scan: return "scan";
        case Result::Derived: return "derived";
        case Result::Missing: return "missing";
        case Result::Ambiguous: return "ambiguous";
        case Result::Bad: return "bad_pattern";
        }
        return "?";
    }

    // How a signature finds its address.
    enum class Via : std::uint8_t
    {
        Search, // its own pattern, searched in the code
        Rel32,  // the rel32/disp32 of an instruction of its parent points to it
        Offset,   // a fixed distance inside its parent
        Pointer,  // the pointer stored at a distance inside its parent (a vtable slot)
        Instance, // the static object whose first 8 bytes are its parent's address (its vtable)
    };

    // The displacement at parent + dispAt, counted from parent + end (the end
    // of its instruction): "call x" is {1, 5} from the call, "mov rax,
    // [rip + x]" {3, 7}.
    struct Rel32
    {
        std::uint32_t dispAt;
        std::uint32_t end;
    };

    struct Offset
    {
        std::uint32_t bytes;
    };

    // The 8-byte pointer at parent + bytes, e.g. a method in a vtable slot.
    struct Pointer
    {
        std::uint32_t bytes;
    };

    // The one static object of the image's writable data that starts with
    // its parent's address: an object found by its vtable. `fromFile`: only
    // an object the exe file already holds (constant initialized, in the part
    // of the section the file has), not one a static constructor builds at
    // start in the zero-filled tail.
    struct Instance
    {
        bool fromFile = false;
    };

    struct Signature;

    namespace detail
    {
        // Every Signature of the module, newest first (the constructor links
        // it in at load, before any code asks for one).
        inline Signature* g_first = nullptr;
    }

    // A static object per signature, next to the code that needs it. `rva`
    // is where the 1491.50 exe (Steam) has it: on that exe an anchor is
    // checked there with no search; for every signature it is what the
    // offline check (tools/sigs/sigtool.py) and the forced search compare to.
    struct Signature
    {
        // An anchor: found by its own pattern.
        Signature(const char* name, const char* pattern, std::uint32_t rva) noexcept
            : Signature(name, pattern, rva, Via::Search, nullptr, 0, 0)
        {
        }
        // Reached from `parent` by a call, a jump or a RIP-relative operand. A
        // code target has its pattern checked there; a global has none ("")
        // and only has to land inside the image.
        Signature(const char* name, const Signature& parent, Rel32 at, const char* pattern, std::uint32_t rva) noexcept
            : Signature(name, pattern, rva, Via::Rel32, &parent, at.dispAt, at.end)
        {
        }
        // A place inside `parent` (an instruction the mod reads, calls or
        // patches), checked with its pattern.
        Signature(const char* name, const Signature& parent, Offset at, const char* pattern, std::uint32_t rva) noexcept
            : Signature(name, pattern, rva, Via::Offset, &parent, at.bytes, 0)
        {
        }
        // What `parent` (a vtable) holds at a slot: a method, its pattern
        // checked where the pointer leads. The class's slot order holds across
        // builds, where the order of functions in the exe does not.
        Signature(const char* name, const Signature& parent, Pointer at, const char* pattern, std::uint32_t rva) noexcept
            : Signature(name, pattern, rva, Via::Pointer, &parent, at.bytes, 0)
        {
        }
        // A static object no code points at, found by its vtable (`parent`):
        // the pattern is checked on the object's bytes, its first 8 (the
        // vtable pointer) as ??. On the known exe it is checked at `rva`; on
        // another it is searched in the writable data and used when found once.
        Signature(const char* name, const Signature& parent, Instance at, const char* pattern, std::uint32_t rva) noexcept
            : Signature(name, pattern, rva, Via::Instance, &parent, at.fromFile ? 1u : 0u, 0)
        {
        }
        Signature(const Signature&) = delete;
        Signature& operator=(const Signature&) = delete;

        const char* const name;
        const char* const pattern;
        const std::uint32_t rva;
        const Via via;
        const Signature* const parent;
        const std::uint32_t at;  // Rel32: dispAt; Offset, Pointer: the distance; Instance: 1 from the file
        const std::uint32_t end; // Rel32: the end of the instruction
        Signature* const link;   // the list
        // Written once by the resolver (game_sig.cpp), read by anyone.
        mutable std::atomic<std::uintptr_t> address{0};
        mutable std::atomic<Result> result{Result::Pending};

    private:
        Signature(const char* name, const char* pattern, std::uint32_t rva, Via via, const Signature* parent, std::uint32_t at,
            std::uint32_t end) noexcept
            : name(name), pattern(pattern ? pattern : ""), rva(rva), via(via), parent(parent), at(at), end(end),
              link(detail::g_first)
        {
            detail::g_first = this;
        }
    };

    inline Signature* FirstSignature() { return detail::g_first; }

    // Where a Rel32 or Offset signature lands from its parent's address;
    // `disp` is the displacement read at parent + at (Rel32 only). Pointer and
    // Instance read the game's memory (game_sig.cpp).
    inline std::int64_t DerivedAddress(const Signature& s, std::int64_t parent, std::int32_t disp)
    {
        return s.via == Via::Rel32 ? Rel32Target(parent, s.end, disp) : parent + static_cast<std::int64_t>(s.at);
    }
}
