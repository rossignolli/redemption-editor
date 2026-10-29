#pragma once

// The shader variables a creature animates: the moving muscles of a horse.
//
// Where they live (build 1491.50, dump rdr2-1491.50-pid18760):
//  - Every creature whose drawable has animated shader variables gets a
//    shader vars component (ctor 0x26507EC: vtable 0x3625E88, type byte
//    +0x10 = 9; built by 0x88FF58 and, for the vehicle cloth squash, by
//    0xB2DA78). Its slot 5 (0x2659AA0) answers "do I have this DOF" for the
//    tracks 0x11 (shaderSlideU), 0x12 (shaderSlideV) and 0x17
//    (animatedNormalMaps): the muscle weights are DOFs of that component.
//  - The component keeps its own frame: the DOF table (crFrameData) at +0x28
//    (+0x28 table of 6-byte DOFs: u16 value offset, u16 id, u8 track, u8 type
//    & 0xF; +0x54 count) and the values at +0x68 (crFrame, 0x265EE10 /
//    0x2660238: +0x68 buffer, +0x70 the frame data = component + 0x28, +0x9C
//    u16 buffer bytes; a value of all ones is "not set", the buffer is filled
//    with 0xFF at init).
//  - The anim director's job writes it every animation update: 0x294F068
//    chains, after the pose finalizer node (0x2952310 -> 0x295BBFC ->
//    0x266C294), a frame write node (0x29520A8 -> 0x295AFC4) whose
//    destination is the component's frame (lea rdx,[rax+68h]) and whose
//    source is the motion tree's output: the node fills the destination with
//    0xFF and copies every mapped DOF. The weights are what the horse's
//    motion tree computes from its animation (clips and expressions); a
//    puppet's motion tree plays nothing, so its weights are the rest values
//    or "not set", whatever the replay writes into its skeleton.
//  - The ped shader effect reads them in its update (vtable 0x34068B0 slot
//    24, 0xB2D128(effect, ped): ped type 4 only) through 0xB2F0D0(effect,
//    ped): ped -> [+0xA8] -> [+0x48] creature -> 0x87CBD0 (the component by
//    its type: index byte [creature + 0x74 + type], [creature][index]) ->
//    the bindings at +0x18 (16-byte rows: hash, value offset, id, track,
//    type; u16 count at +0x20) -> the value at [component + 0x68] + offset,
//    "not set" read as 0, clamped and packed into the effect's instance.
//
// What the replay does (it records the engine's results per entity and
// per frame, not the inputs the engine derived them from): the
// recording keeps each recorded horse's component values, about 30 times a
// second (section SHVR), and the playback writes the recorded values into
// the puppet's component buffer at the end of the engine's own write (a
// detour of the job's frame write node 0x295AFC4, shader_vars_hook.h): every
// reader after it, the shader included, sees them until the next update.
//
// Pure data: layout parsing, the half float codec, the recorded track (note,
// lookup with interpolation, ring, cut, validation) and the mapping of a
// recorded layout onto a puppet's. Unit tested in tests/shader_vars_tests.cpp.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace replay::shadervars
{
    // MuscleRecord and MuscleReplay (developer keys), both on by default.
    struct ReplayConfig
    {
        bool record = true;
        bool replay = true;
        // Developer tests of cause (dev keys, off by default):
        // MuscleDebugForce 1 writes every track 0x17 lane of every horse
        // puppet (and, outside the replay, of the player's horse) to 1.0, 2 to
        // 0.0; MuscleDebugSweat >= 0 gives the horse puppets SET_PED_SWEAT.
        std::uint8_t debugForce = 0;
        float debugSweat = -1.0f;
        // MuscleDebugOverwrite 1: the engine copies into the puppet's buffer
        // and the recorded values go on top (an earlier build's way); 0 (default):
        // the recorded values replace the engine's copy.
        bool debugOverwrite = false;
    };

    // Not an engine track: the ped's sweat (graphics component +0x488, which the
    // ped shader effect copies every update) kept as one more float lane of
    // the sample, DOF {kTrackSweat, 2, kSweatId}. A puppet's layout never has
    // it, so it is never written into the component (MapLayout counts it
    // missing); the playback gives it with SET_PED_SWEAT.
    constexpr std::uint8_t kTrackSweat = 0xFF;
    constexpr std::uint16_t kSweatId = 0x488;

    constexpr std::uint8_t kTrackShaderSlideU = 0x11;
    constexpr std::uint8_t kTrackShaderSlideV = 0x12;
    constexpr std::uint8_t kTrackAnimatedNormalMaps = 0x17;

    constexpr std::size_t kDofBytes = 6;          // u16 offset, u16 id, u8 track, u8 type
    constexpr std::uint32_t kMaxDofs = 256;       // DOFs of one component frame
    constexpr std::uint32_t kMaxLanes = 512;      // floats of one sample
    constexpr std::uint32_t kMaxBufferBytes = 8192;
    constexpr std::uint32_t kNotSet = 0xFFFFFFFFu;

    // The recording's rate and how far a value holds past its point.
    constexpr std::uint32_t kSamplePeriodMs = 33;
    // A value holds this long past (or before) its sample; set samples at
    // most kBridgeMs apart are interpolated.
    constexpr std::uint32_t kHoldMs = 1500;
    constexpr std::uint32_t kBridgeMs = 3000;
    constexpr std::size_t kMaxOwners = 1024;
    constexpr std::size_t kMaxBlocks = 64;              // layouts per owner
    constexpr std::size_t kMaxPoints = 1u << 18;        // per block (2.4 hours at 30 a second)

    // A DOF of the component frame. The type is the frame's value type: 2 a
    // float (4 bytes), 0, 1 and 4 a 16-byte vector (0xB2F0D0 reads type 2 as a
    // float and type 4 as four; the frame write node copies 4 and 16-byte
    // values). Other types are not kept.
    struct Dof
    {
        std::uint8_t track = 0;
        std::uint8_t type = 0;
        std::uint16_t id = 0;

        bool operator==(const Dof& o) const { return track == o.track && type == o.type && id == o.id; }
        bool operator!=(const Dof& o) const { return !(*this == o); }
    };

    inline std::uint32_t LanesOf(std::uint8_t type)
    {
        switch (type)
        {
        case 2: return 1;
        case 0:
        case 1:
        case 4: return 4;
        default: return 0;
        }
    }

    inline std::uint32_t LanesOf(const std::vector<Dof>& dofs)
    {
        std::uint32_t lanes = 0;
        for (const Dof& dof : dofs)
            lanes += LanesOf(dof.type);
        return lanes;
    }

    // The kept DOFs of a component frame, in table order, with the byte
    // offset of each value in the frame buffer.
    struct Layout
    {
        std::vector<Dof> dofs;
        std::vector<std::uint16_t> offsets;
        std::uint32_t lanes = 0;
        std::uint32_t skipped = 0; // DOFs of a type not kept

        std::uint32_t CountTrack(std::uint8_t track) const
        {
            std::uint32_t n = 0;
            for (const Dof& dof : dofs)
                n += dof.track == track ? 1u : 0u;
            return n;
        }
    };

    enum class ParseResult : std::uint8_t
    {
        Ok,
        Empty,     // no DOF (or none kept)
        TooMany,   // more DOFs or lanes than kept
        Order,     // keys not ascending (the engine binary-searches the table)
        Range,     // a value past the buffer
        Overlap,   // two values share bytes
    };

    inline const char* ParseResultText(ParseResult r)
    {
        switch (r)
        {
        case ParseResult::Ok: return "ok";
        case ParseResult::Empty: return "empty";
        case ParseResult::TooMany: return "too_many";
        case ParseResult::Order: return "order";
        case ParseResult::Range: return "range";
        case ParseResult::Overlap: return "overlap";
        default: return "?";
        }
    }

    // The DOF table (count rows of kDofBytes) of a frame whose buffer has
    // bufferBytes bytes.
    inline ParseResult ParseLayout(const std::uint8_t* table, std::uint32_t count, std::uint32_t bufferBytes, Layout& out)
    {
        out = Layout{};
        if (!table || count == 0)
            return ParseResult::Empty;
        if (count > kMaxDofs || bufferBytes > kMaxBufferBytes)
            return ParseResult::TooMany;
        struct Span
        {
            std::uint32_t begin, end;
        };
        std::vector<Span> spans;
        std::uint32_t lastKey = 0;
        for (std::uint32_t n = 0; n < count; ++n)
        {
            const std::uint8_t* at = table + static_cast<std::size_t>(n) * kDofBytes;
            std::uint16_t offset = 0, id = 0;
            std::memcpy(&offset, at, 2);
            std::memcpy(&id, at + 2, 2);
            const std::uint8_t track = at[4];
            const std::uint8_t type = static_cast<std::uint8_t>(at[5] & 0x0F);
            const std::uint32_t key = (static_cast<std::uint32_t>(track) << 16) | id;
            if (n != 0 && key <= lastKey)
                return ParseResult::Order;
            lastKey = key;
            const std::uint32_t lanes = LanesOf(type);
            if (!lanes)
            {
                ++out.skipped;
                continue;
            }
            const std::uint32_t end = static_cast<std::uint32_t>(offset) + lanes * 4u;
            if (end > bufferBytes)
                return ParseResult::Range;
            if (out.lanes + lanes > kMaxLanes)
                return ParseResult::TooMany;
            out.dofs.push_back(Dof{track, type, id});
            out.offsets.push_back(offset);
            out.lanes += lanes;
            spans.push_back(Span{offset, end});
        }
        if (out.dofs.empty())
            return ParseResult::Empty;
        std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.begin < b.begin; });
        for (std::size_t n = 1; n < spans.size(); ++n)
            if (spans[n].begin < spans[n - 1].end)
                return ParseResult::Overlap;
        return ParseResult::Ok;
    }

    // The lanes of a layout read from a copy of the frame buffer (raw bits).
    inline bool Gather(const Layout& layout, const std::uint8_t* buffer, std::size_t bytes, std::vector<std::uint32_t>& bits)
    {
        bits.clear();
        bits.reserve(layout.lanes);
        for (std::size_t n = 0; n < layout.dofs.size(); ++n)
        {
            const std::uint32_t lanes = LanesOf(layout.dofs[n].type);
            const std::size_t at = layout.offsets[n];
            if (at + lanes * 4u > bytes)
                return false;
            for (std::uint32_t k = 0; k < lanes; ++k)
            {
                std::uint32_t v = 0;
                std::memcpy(&v, buffer + at + k * 4u, 4);
                bits.push_back(v);
            }
        }
        return true;
    }

    inline float FloatOf(std::uint32_t bits)
    {
        float v = 0.0f;
        std::memcpy(&v, &bits, 4);
        return v;
    }

    inline std::uint32_t BitsOf(float v)
    {
        std::uint32_t b = 0;
        std::memcpy(&b, &v, 4);
        return b;
    }

    // Set: a finite value (the engine's "not set" is all ones, a NaN).
    inline bool IsSet(std::uint32_t bits) { return bits != kNotSet && std::isfinite(FloatOf(bits)); }

    // ---- half floats (IEEE 754 binary16) -----------------------------------
    // The values go to the shader as bytes (0xB2F0D0: clamped, x 127.5 or
    // x 255), so 11 significant bits keep what it sees. "Not set" and any
    // NaN are kHalfNotSet.
    constexpr std::uint16_t kHalfNotSet = 0xFFFF;

    inline std::uint16_t ToHalf(std::uint32_t bits)
    {
        if (!IsSet(bits))
            return kHalfNotSet;
        const std::uint32_t sign = (bits >> 16) & 0x8000u;
        const std::int32_t exponent = static_cast<std::int32_t>((bits >> 23) & 0xFF) - 127 + 15;
        std::uint32_t mantissa = bits & 0x7FFFFFu;
        if (exponent >= 31)
            return static_cast<std::uint16_t>(sign | 0x7BFFu); // the largest finite half
        if (exponent <= 0)
        {
            if (exponent < -10)
                return static_cast<std::uint16_t>(sign); // below the smallest subnormal: 0
            mantissa |= 0x800000u;
            const std::uint32_t shift = static_cast<std::uint32_t>(14 - exponent);
            std::uint32_t half = mantissa >> shift;
            const std::uint32_t rest = mantissa & ((1u << shift) - 1u);
            const std::uint32_t halfway = 1u << (shift - 1);
            if (rest > halfway || (rest == halfway && (half & 1u)))
                ++half;
            return static_cast<std::uint16_t>(sign | half);
        }
        std::uint32_t half = (static_cast<std::uint32_t>(exponent) << 10) | (mantissa >> 13);
        const std::uint32_t rest = mantissa & 0x1FFFu;
        if (rest > 0x1000u || (rest == 0x1000u && (half & 1u)))
            ++half; // a carry into the exponent is the right rounding
        if (half >= 0x7C00u)
            half = 0x7BFFu;
        return static_cast<std::uint16_t>(sign | half);
    }

    inline std::uint32_t FromHalf(std::uint16_t half)
    {
        if (half == kHalfNotSet || (half & 0x7C00u) == 0x7C00u)
            return kNotSet;
        const std::uint32_t sign = static_cast<std::uint32_t>(half & 0x8000u) << 16;
        std::uint32_t exponent = (half >> 10) & 0x1Fu;
        std::uint32_t mantissa = half & 0x3FFu;
        if (exponent == 0)
        {
            if (mantissa == 0)
                return sign;
            // Subnormal: normalize.
            exponent = 1;
            while (!(mantissa & 0x400u))
            {
                mantissa <<= 1;
                --exponent;
            }
            mantissa &= 0x3FFu;
        }
        return sign | ((exponent + 127u - 15u) << 23) | (mantissa << 13);
    }

    // ---- the recorded track (Recording::shaderVars, section SHVR) ----------

    // Points of one layout.
    struct Block
    {
        std::vector<Dof> dofs;
        std::uint32_t lanes = 0;
        std::vector<std::uint32_t> times;   // ascending
        std::vector<std::uint16_t> values;  // times.size() x lanes, half floats

        std::uint32_t Front() const { return times.front(); }
        std::uint32_t Back() const { return times.back(); }
    };

    struct Series
    {
        std::uint32_t owner = 0; // packed replay id
        std::vector<Block> blocks; // time ascending
    };

    struct Track
    {
        std::vector<Series> series; // owner ascending

        void Clear() { series.clear(); }
        bool Empty() const { return series.empty(); }

        std::size_t Points() const
        {
            std::size_t n = 0;
            for (const Series& s : series)
                for (const Block& b : s.blocks)
                    n += b.times.size();
            return n;
        }

        std::size_t ApproxBytes() const
        {
            std::size_t bytes = series.capacity() * sizeof(Series);
            for (const Series& s : series)
            {
                bytes += s.blocks.capacity() * sizeof(Block);
                for (const Block& b : s.blocks)
                    bytes += b.dofs.capacity() * sizeof(Dof) + b.times.capacity() * 4u + b.values.capacity() * 2u;
            }
            return bytes;
        }

        const Series* Find(std::uint32_t owner) const
        {
            const auto it = std::lower_bound(series.begin(), series.end(), owner,
                [](const Series& s, std::uint32_t id) { return s.owner < id; });
            return it != series.end() && it->owner == owner ? &*it : nullptr;
        }

        // A new point is due: none yet, or kSamplePeriodMs since the last
        // (a clock that went back is due too: Note drops the newer tail).
        bool Due(std::uint32_t owner, std::uint32_t timeMs) const
        {
            const Series* s = Find(owner);
            if (!s || s->blocks.empty())
                return true;
            const std::uint32_t last = s->blocks.back().Back();
            return timeMs < last || timeMs - last >= kSamplePeriodMs;
        }

        // Appends in time order (a clock that went back drops the newer tail,
        // the way a rewound recording overwrites); a layout other than the
        // last block's starts a block. False: not kept.
        bool Note(std::uint32_t owner, std::uint32_t timeMs, const std::vector<Dof>& dofs, const std::vector<std::uint32_t>& bits)
        {
            const std::uint32_t lanes = LanesOf(dofs);
            if (dofs.empty() || dofs.size() > kMaxDofs || lanes == 0 || lanes > kMaxLanes || bits.size() != lanes)
                return false;
            for (const Dof& dof : dofs)
                if (!LanesOf(dof.type))
                    return false;
            auto it = std::lower_bound(series.begin(), series.end(), owner,
                [](const Series& s, std::uint32_t id) { return s.owner < id; });
            if (it == series.end() || it->owner != owner)
            {
                if (series.size() >= kMaxOwners)
                    return false;
                Series s;
                s.owner = owner;
                it = series.insert(it, std::move(s));
            }
            std::vector<Block>& blocks = it->blocks;
            while (!blocks.empty() && blocks.back().Back() >= timeMs)
            {
                Block& b = blocks.back();
                b.times.pop_back();
                b.values.resize(b.times.size() * b.lanes);
                if (b.times.empty())
                    blocks.pop_back();
            }
            if (blocks.empty() || blocks.back().dofs != dofs)
            {
                if (blocks.size() >= kMaxBlocks)
                    return false;
                Block b;
                b.dofs = dofs;
                b.lanes = lanes;
                blocks.push_back(std::move(b));
            }
            Block& b = blocks.back();
            if (b.times.size() >= kMaxPoints)
                return false;
            b.times.push_back(timeMs);
            for (const std::uint32_t v : bits)
                b.values.push_back(ToHalf(v));
            return true;
        }

        // The recorded value at timeMs, lane by lane, in the block in force:
        // linear between the nearest set samples around timeMs when they are
        // at most kBridgeMs apart, else the nearer one within kHoldMs; one
        // side only: held within kHoldMs. A lane unset in a sample, or a hole
        // in the samples (the engine animates a far or off-camera horse a few
        // times a second, and the recorder notes only what the write node
        // produced), never drops back to the puppet's own rest values (impl/
        // horse-muscle-4: the muscles relaxed for a moment mid-clip). False:
        // no lane has a value.
        bool At(std::uint32_t owner, std::uint32_t timeMs, const std::vector<Dof>*& dofs, std::vector<std::uint32_t>& bits) const
        {
            dofs = nullptr;
            bits.clear();
            const Series* s = Find(owner);
            if (!s || s->blocks.empty())
                return false;
            // The last block starting at or before timeMs (or the first).
            const Block* block = &s->blocks.front();
            for (const Block& b : s->blocks)
                if (b.Front() <= timeMs)
                    block = &b;
            const std::vector<std::uint32_t>& times = block->times;
            const std::size_t count = times.size();
            const std::size_t next = static_cast<std::size_t>(std::upper_bound(times.begin(), times.end(), timeMs) - times.begin());
            bits.assign(block->lanes, kNotSet);
            bool any = false;
            for (std::uint32_t k = 0; k < block->lanes; ++k)
            {
                // The nearest set sample at or before timeMs, within kHoldMs + kBridgeMs.
                std::size_t a = count, b = count;
                for (std::size_t i = next; i-- > 0;)
                {
                    if (timeMs - times[i] > kHoldMs + kBridgeMs)
                        break;
                    if (IsSet(FromHalf(block->values[i * block->lanes + k])))
                    {
                        a = i;
                        break;
                    }
                }
                for (std::size_t i = next; i < count; ++i)
                {
                    if (times[i] - timeMs > kHoldMs + kBridgeMs)
                        break;
                    if (IsSet(FromHalf(block->values[i * block->lanes + k])))
                    {
                        b = i;
                        break;
                    }
                }
                const std::uint32_t va = a < count ? FromHalf(block->values[a * block->lanes + k]) : kNotSet;
                const std::uint32_t vb = b < count ? FromHalf(block->values[b * block->lanes + k]) : kNotSet;
                if (a < count && b < count && times[b] - times[a] <= kBridgeMs)
                {
                    const std::uint32_t ta = times[a], tb = times[b];
                    const float f = tb > ta ? static_cast<float>(timeMs - ta) / static_cast<float>(tb - ta) : 1.0f;
                    bits[k] = BitsOf(FloatOf(va) + (FloatOf(vb) - FloatOf(va)) * f);
                }
                else
                {
                    const std::uint32_t da = a < count ? timeMs - times[a] : 0xFFFFFFFFu;
                    const std::uint32_t db = b < count ? times[b] - timeMs : 0xFFFFFFFFu;
                    if (da <= db && da <= kHoldMs)
                        bits[k] = va;
                    else if (db <= kHoldMs)
                        bits[k] = vb;
                }
                any = any || IsSet(bits[k]);
            }
            if (!any)
            {
                bits.clear();
                return false;
            }
            dofs = &block->dofs;
            return true;
        }

        // The ring: points before keepFromMs go, except the last one of each
        // owner before it (the value in force at keepFromMs). An owner whose
        // last point is more than kHoldMs before keepFromMs goes whole.
        void DropBefore(std::uint32_t keepFromMs)
        {
            for (Series& s : series)
            {
                if (s.blocks.empty() ||
                    static_cast<std::uint64_t>(s.blocks.back().Back()) + kHoldMs < static_cast<std::uint64_t>(keepFromMs))
                {
                    s.blocks.clear();
                    continue;
                }
                // The last point before keepFromMs: (block, index).
                std::size_t keepBlock = 0, keepPoint = 0;
                bool found = false;
                for (std::size_t bi = 0; bi < s.blocks.size(); ++bi)
                {
                    const std::vector<std::uint32_t>& times = s.blocks[bi].times;
                    const auto first = std::lower_bound(times.begin(), times.end(), keepFromMs);
                    if (first == times.begin())
                        break;
                    keepBlock = bi;
                    keepPoint = static_cast<std::size_t>(std::prev(first) - times.begin());
                    found = true;
                    if (first != times.end())
                        break;
                }
                if (!found)
                    continue;
                s.blocks.erase(s.blocks.begin(), s.blocks.begin() + static_cast<std::ptrdiff_t>(keepBlock));
                Block& b = s.blocks.front();
                b.times.erase(b.times.begin(), b.times.begin() + static_cast<std::ptrdiff_t>(keepPoint));
                b.values.erase(b.values.begin(), b.values.begin() + static_cast<std::ptrdiff_t>(keepPoint * b.lanes));
            }
            series.erase(std::remove_if(series.begin(), series.end(), [](const Series& s) { return s.blocks.empty(); }),
                series.end());
        }

        // A saved window [baseMs, endMs]: the kept owners' points inside it,
        // shifted to the window, and the value in force at baseMs at 0.
        template <typename Kept>
        void CutInto(Track& out, std::uint32_t baseMs, std::uint32_t endMs, Kept kept) const
        {
            out.Clear();
            std::vector<std::uint32_t> bits;
            for (const Series& s : series)
            {
                if (!kept(s.owner))
                    continue;
                Series o;
                o.owner = s.owner;
                auto push = [&](const std::vector<Dof>& dofs, std::uint32_t lanes, std::uint32_t timeMs, const std::uint16_t* halves) {
                    if (o.blocks.empty() || o.blocks.back().dofs != dofs)
                    {
                        Block b;
                        b.dofs = dofs;
                        b.lanes = lanes;
                        o.blocks.push_back(std::move(b));
                    }
                    Block& b = o.blocks.back();
                    b.times.push_back(timeMs);
                    b.values.insert(b.values.end(), halves, halves + lanes);
                };
                const std::vector<Dof>* dofs = nullptr;
                const bool atBase = At(s.owner, baseMs, dofs, bits) && dofs;
                if (atBase)
                {
                    std::vector<std::uint16_t> halves(bits.size());
                    for (std::size_t k = 0; k < bits.size(); ++k)
                        halves[k] = ToHalf(bits[k]);
                    push(*dofs, static_cast<std::uint32_t>(bits.size()), 0, halves.data());
                }
                for (const Block& b : s.blocks)
                {
                    for (std::size_t n = 0; n < b.times.size(); ++n)
                    {
                        const std::uint32_t t = b.times[n];
                        if (t > endMs)
                            break;
                        if (t < baseMs || (atBase && t == baseMs))
                            continue;
                        push(b.dofs, b.lanes, t - baseMs, b.values.data() + n * b.lanes);
                    }
                }
                if (!o.blocks.empty())
                    out.series.push_back(std::move(o));
            }
        }

        // A loaded section: owners ascending and unique, blocks with a kept
        // layout, times ascending through the owner's blocks.
        bool Valid() const
        {
            if (series.size() > kMaxOwners)
                return false;
            for (std::size_t n = 0; n < series.size(); ++n)
            {
                const Series& s = series[n];
                if (s.blocks.empty() || s.blocks.size() > kMaxBlocks || (n && series[n - 1].owner >= s.owner))
                    return false;
                bool any = false;
                std::uint32_t last = 0;
                for (const Block& b : s.blocks)
                {
                    if (b.dofs.empty() || b.dofs.size() > kMaxDofs || b.lanes != LanesOf(b.dofs) || b.lanes == 0 ||
                        b.lanes > kMaxLanes || b.times.empty() || b.times.size() > kMaxPoints ||
                        b.values.size() != b.times.size() * b.lanes)
                        return false;
                    for (const Dof& dof : b.dofs)
                        if (!LanesOf(dof.type))
                            return false;
                    for (const std::uint32_t t : b.times)
                    {
                        if (any && t <= last)
                            return false;
                        any = true;
                        last = t;
                    }
                }
            }
            return true;
        }
    };

    // The recording's carry: a lane unset in this sample takes the last set
    // value of that lane (kept in `last`, one entry per lane); a set lane
    // updates it. A new layout (another lane count) starts over. Returns the
    // lanes filled.
    inline std::uint32_t CarryForward(std::vector<std::uint32_t>& last, std::vector<std::uint32_t>& bits)
    {
        if (last.size() != bits.size())
            last.assign(bits.size(), kNotSet);
        std::uint32_t filled = 0;
        for (std::size_t k = 0; k < bits.size(); ++k)
        {
            if (IsSet(bits[k]))
                last[k] = bits[k];
            else if (IsSet(last[k]))
            {
                bits[k] = last[k];
                ++filled;
            }
        }
        return filled;
    }

    // ---- the puppet side ---------------------------------------------------

    // One 4-byte write into the puppet's frame buffer: its offset and the
    // recorded lane it takes.
    struct Write
    {
        std::uint16_t offset = 0;
        std::uint16_t lane = 0;
    };

    // The recorded layout onto the puppet's: every recorded DOF the puppet
    // has (same track, id and type), lane by lane. missing: recorded DOFs
    // the puppet does not have (another model, another drawable).
    inline void MapLayout(const std::vector<Dof>& recorded, const Layout& puppet, std::vector<Write>& out, std::uint32_t& missing)
    {
        out.clear();
        missing = 0;
        std::uint32_t lane = 0;
        for (const Dof& dof : recorded)
        {
            const std::uint32_t lanes = LanesOf(dof.type);
            std::size_t found = puppet.dofs.size();
            for (std::size_t n = 0; n < puppet.dofs.size(); ++n)
                if (puppet.dofs[n] == dof)
                {
                    found = n;
                    break;
                }
            if (found == puppet.dofs.size())
                missing += dof.track == kTrackSweat ? 0u : 1u; // the sweat lane is never in a layout
            else
                for (std::uint32_t k = 0; k < lanes; ++k)
                    out.push_back(Write{static_cast<std::uint16_t>(puppet.offsets[found] + k * 4u), static_cast<std::uint16_t>(lane + k)});
            lane += lanes;
        }
    }

    // ---- evidence ----------------------------------------------------------

    // The lanes of one track (the muscles: kTrackAnimatedNormalMaps), with
    // their lane index as the id (a vector DOF gives four).
    inline void TrackLanes(const std::vector<Dof>& dofs, const std::vector<std::uint32_t>& bits, std::uint8_t track,
        std::vector<std::uint16_t>& ids, std::vector<std::uint32_t>& out)
    {
        ids.clear();
        out.clear();
        std::uint32_t lane = 0;
        for (const Dof& dof : dofs)
        {
            const std::uint32_t lanes = LanesOf(dof.type);
            if (dof.track == track)
                for (std::uint32_t k = 0; k < lanes && lane + k < bits.size(); ++k)
                {
                    ids.push_back(static_cast<std::uint16_t>(lane + k));
                    out.push_back(bits[lane + k]);
                }
            lane += lanes;
        }
    }

    // Two samples of the same layout, lane by lane.
    struct Compare
    {
        std::uint32_t lanes = 0;
        std::uint32_t bothSet = 0;
        std::uint32_t setMismatch = 0; // set on one side only
        float maxAbs = 0.0f;
        std::uint32_t worstLane = 0;
    };

    inline Compare CompareLanes(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b)
    {
        Compare c;
        c.lanes = static_cast<std::uint32_t>((std::min)(a.size(), b.size()));
        for (std::uint32_t k = 0; k < c.lanes; ++k)
        {
            const bool sa = IsSet(a[k]), sb = IsSet(b[k]);
            if (sa != sb)
            {
                ++c.setMismatch;
                continue;
            }
            if (!sa)
                continue;
            ++c.bothSet;
            const float d = std::fabs(FloatOf(a[k]) - FloatOf(b[k]));
            if (d > c.maxAbs)
            {
                c.maxAbs = d;
                c.worstLane = k;
            }
        }
        return c;
    }

    // How much a sample moves: the largest step of a set lane against the
    // previous sample (a still horse 0, a galloping one well above 0.1).
    inline float MaxStep(const std::vector<std::uint32_t>& prev, const std::vector<std::uint32_t>& now)
    {
        return CompareLanes(prev, now).maxAbs;
    }
}
