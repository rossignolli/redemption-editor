#include "metaped_reader.h"

#include "engine_sigs.h"
#include "entity_appearance.h" // ReadShopItems
#include "entity_cache.h"
#include "game_sig.h"
#include "guarded_memory.h"

#include "natives.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <sstream>

namespace replay::metaped::engine
{
    namespace
    {
        namespace sig = replay::sig;

        // _GET_CHAR_EXPRESSION's core. The getter finds the ped's creature (the
        // reads below repeat engine_sigs.h kCreatureLookup), then calls the frame
        // container and the expression frame (frame + 0x58, track 0x40); the frame
        // jumps to the frame value, which calls the degree-of-freedom find. The
        // offsets below are in these bytes: a build that moved one does not match.
        const sig::Signature kGetter{"char_expression_getter",
            "40 53 48 83 EC 20 83 64 24 30 00 8B DA 48 85 C9 75 05 0F 57 C0 EB 2A 33 D2 E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? "
            "?? ?? 48 85 C0 74 10 4C 8D 44 24 30 0F B7 D3 48 8B C8 E8 ?? ?? ?? ?? F3 0F 10 44 24 30 48 83 C4 20 5B C3",
            0xB44250};
        const sig::Signature kContainer{"frame_container", kGetter, sig::Rel32{0x22, 0x26},
            "44 0F B6 0D ?? ?? ?? ?? 41 83 C8 FF 49 0F BE C0 4A 8D 14 8D 01 00 00 00 48 03 D0 8B C2 8A 94 08 04 01 00 00 "
            "80 FA 14 75 0C 41 FF C0 41 83 F8 03 7C DA 33 C0 C3 0F B6 C2 48 8B 44 C1 18 C3",
            0xB32AD8};
        const sig::Signature kBufferIndex{"buffer_index", kContainer, sig::Rel32{0x04, 0x08}, "", 0x3CE2BF0}; // byte
        const sig::Signature kFrame{"expression_frame", kGetter, sig::Rel32{0x37, 0x3B},
            "4D 8B C8 48 83 C1 58 44 0F B7 C2 B2 40 E9 ?? ?? ?? ??", 0x2BC96C4};
        const sig::Signature kFrameValue{"frame_value", kFrame, sig::Rel32{0x0E, 0x12},
            "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 59 08 48 8B F9 48 8B CB 49 8B F1 E8 ?? ?? ?? ?? 41 83 C9 "
            "FF 4C 8B C0 48 85 C0 74 1F 48 8B D0 48 B8 AB AA AA AA AA AA AA 2A 48 2B 13 48 F7 EA 48 8B CA 48 C1 E9 3F 48 "
            "03 D1 EB 03 41 8B D1 4D 85 C0 74 2B 4C 8B 07 8B C2 48 8D 14 40 48 8B 47 08 48 8B 08 0F B7 04 51 46 39 0C 00 "
            "74 11 48 8B 03 0F B7 0C 50 42 8B 04 01 89 06 B0 01 EB 02 32",
            0x265FDBC};
        const sig::Signature kDofFind{"dof_find", kFrameValue, sig::Rel32{0x1D, 0x21},
            "48 89 5C 24 08 41 0F B7 C0 4C 8B 01 44 0F B6 DA 41 C1 E3 10 44 0B D8 8B 41 2C 48 8D 0C 40 48 B8 AB AA AA AA "
            "AA AA AA 2A 48 03 C9 48 F7 E9 4A 8D 1C 01 48 8B C2 48 C1 E8 3F 48 03 D0 EB 32 4C 8B CA 49 D1 F9 4B 8D 04 49 "
            "4D 8D 14 40 41 0F B6 4A 04 41 0F B7 42 02 C1 E1 10 0B C8 41 3B CB",
            0x26580BC};
        // _SET_PED_SCALE's core: two places by the metaped's state, after the
        // "ready" test it calls (the dressing object [[ped + 0x28] + 0x750] with
        // +0xB72 and +0xB75 both 0).
        const sig::Signature kScaleSet{"scale_setter",
            "48 85 C9 0F 84 ?? ?? ?? ?? 53 48 83 EC 30 0F 29 74 24 20 48 8B D9 0F 28 F1 0F 2F 35 ?? ?? ?? ?? 76 75 E8 ?? "
            "?? ?? ?? 8B 8B 9C 00 00 00 81 E1 FF FF 01 00 2B 0D ?? ?? ?? ?? 84 C0 8B C1 74 30 48 69 C8 48 01 00 00 0F 28 "
            "CE 48 8B 05 ?? ?? ?? ?? 48 8B 94 01 E0 00 00 00 48 8B C2 48 83 E0 FE 48 F7 DA 48 1B C9 48 23 C8 E8 ?? ?? ?? "
            "?? EB 28 48 69 C8 48 01 00 00 48 8B 05 ?? ?? ?? ?? 48 8B 54 01 50 48 8B CA 48 83 E1 FE 48 F7 DA 48 1B C0 48 "
            "23 C1 F3 0F 11 70 48 0F 28 74 24 20 48 83 C4 30 5B C3",
            0xFEF2FC};
        const sig::Signature kScaleReady{"scale_ready", kScaleSet, sig::Rel32{0x23, 0x27},
            "40 53 48 83 EC 20 8B 15 ?? ?? ?? ?? 48 8B D9 48 83 C1 10 E8 ?? ?? ?? ?? 33 D2 48 85 C0 75 27 48 8B 43 28 48 "
            "85 C0 74 1E 48 8B 80 50 07 00 00 48 85 C0 74 12 38 90 72 0B 00 00 75 0A 38 90 75 0B 00 00 75 02 B2 01 8A C2 "
            "48 83 C4 20 5B C3",
            0x8B70E8};
        const sig::Signature kScaleBaseIndex{"scale_base_index", kScaleSet, sig::Rel32{0x35, 0x39}, "", 0x39ECE40}; // dword
        const sig::Signature kScaleTable{"scale_table", kScaleSet, sig::Rel32{0x4C, 0x50}, "", 0x39ECE48};          // qword
        constexpr std::uint32_t kScaleEntryStride = 0x148;
        constexpr std::uint32_t kScaleReadyComponent = 0xE0, kScaleReadyField = 0x620;
        constexpr std::uint32_t kScalePendingComponent = 0x50, kScalePendingField = 0x48;

        constexpr std::uint32_t kPedIndexOffset = 0x9C;
        constexpr std::uint32_t kIndexMask = 0x1FFFF;
        constexpr std::uint32_t kCreatureEntryStride = 16;
        constexpr std::uint32_t kCreatureOffset = 0x28;
        constexpr std::uint32_t kSelectBytes = 0x104;
        constexpr std::uint32_t kContainers = 0x18;
        constexpr std::uint8_t kNoContainer = 0x14;
        constexpr std::uint32_t kFrameOffset = 0x58;
        constexpr std::uint32_t kDofCountOffset = 0x2C;

        constexpr std::uint32_t kDressingOffset = 0x750; // [ped + 0x28] + 0x750
        constexpr std::uint32_t kDressingBusyA = 0xB72, kDressingBusyB = 0xB75;

        struct Globals
        {
            bool checked = false;
            bool expressions = false;
            bool scale = false;
            std::string failed = "none";
            std::uintptr_t creatureTable = 0; // address of the qword
            std::uintptr_t creatureBase = 0;  // address of the dword
            std::uintptr_t bufferIndex = 0;   // address of the byte
            std::uintptr_t scaleTable = 0;
            std::uintptr_t scaleBase = 0;
            bool crossOff = false; // a cross-check with the native failed
        };
        Globals g;
        Stats g_stats;

        // The first signature of `list` the game does not have (its name), or
        // null when all resolved.
        const char* FirstMissing(std::initializer_list<const sig::Signature*> list)
        {
            for (const sig::Signature* s : list)
                if (!sig::Find(*s))
                    return s->name;
            return nullptr;
        }

        void RunChecks()
        {
            if (g.checked)
                return;
            g.checked = true;
            if (const char* missing = FirstMissing({&kGetter, &enginesig::kCreatureLookup, &enginesig::kCreatureTable,
                    &enginesig::kCreatureBaseIndex, &kContainer, &kBufferIndex, &kFrame, &kFrameValue, &kDofFind}))
                g.failed = missing;
            else
            {
                g.expressions = true;
                g.creatureTable = sig::Find(enginesig::kCreatureTable);
                g.creatureBase = sig::Find(enginesig::kCreatureBaseIndex);
                g.bufferIndex = sig::Find(kBufferIndex);
            }
            if (const char* missing = FirstMissing({&kScaleSet, &kScaleReady, &kScaleBaseIndex, &kScaleTable}))
            {
                if (g.failed == "none")
                    g.failed = missing;
            }
            else
            {
                g.scale = true;
                g.scaleBase = sig::Find(kScaleBaseIndex);
                g.scaleTable = sig::Find(kScaleTable);
            }
        }

        template <typename T>
        bool Get(std::uintptr_t address, T& out)
        {
            return replay::mem::Read(address, out);
        }

        // The ped's extension index, as the creature lookup and the scale setter
        // compute it.
        bool PedIndex(std::uintptr_t ped, std::uintptr_t baseAddress, std::uint32_t& index)
        {
            std::uint32_t raw = 0, base = 0;
            if (!Get(ped + kPedIndexOffset, raw) || !Get(baseAddress, base))
                return false;
            index = (raw & kIndexMask) - base;
            return (raw & kIndexMask) >= base;
        }

        // The expression frame: value buffer and frame data (layout in metaped_state.h).
        ReadResult Frame(std::uintptr_t ped, std::uintptr_t& buffer, std::uintptr_t& data)
        {
            std::uint32_t index = 0;
            std::uintptr_t table = 0;
            if (!PedIndex(ped, g.creatureBase, index) || !Get(g.creatureTable, table) || !table)
                return ReadResult::Fault;
            std::uint64_t entry = 0;
            if (!Get(table + static_cast<std::uintptr_t>(index) * kCreatureEntryStride, entry))
                return ReadResult::Fault;
            if (!entry)
                return ReadResult::NoCreature;
            std::uintptr_t creature = 0;
            if (!Get(static_cast<std::uintptr_t>(entry & ~1ull) + kCreatureOffset, creature))
                return ReadResult::Fault;
            if (!creature)
                return ReadResult::NoCreature;
            std::uint8_t bufferIndex = 0;
            if (!Get(g.bufferIndex, bufferIndex))
                return ReadResult::Fault;
            std::uint8_t select[16]{};
            if (!replay::mem::Copy(select, creature + kSelectBytes, sizeof(select)))
                return ReadResult::Fault;
            std::uintptr_t container = 0;
            for (int step = -1; step < 3; ++step)
            {
                // eax = (bufferIndex * 4 + 1 + step) as 32 bits, the byte at creature + 0x104 + eax.
                const std::uint32_t at = static_cast<std::uint32_t>(bufferIndex) * 4u + 1u + static_cast<std::uint32_t>(step);
                std::uint8_t sel = 0;
                if (at < sizeof(select))
                    sel = select[at];
                else if (!Get(creature + kSelectBytes + at, sel))
                    return ReadResult::Fault;
                if (sel == kNoContainer)
                    continue;
                if (!Get(creature + kContainers + static_cast<std::uintptr_t>(sel) * 8u, container))
                    return ReadResult::Fault;
                break;
            }
            if (!container)
                return ReadResult::NoCreature;
            const std::uintptr_t frame = container + kFrameOffset;
            if (!Get(frame, buffer) || !Get(frame + 8, data))
                return ReadResult::Fault;
            return buffer && data ? ReadResult::Ok : ReadResult::NoCreature;
        }

        bool CrossCheck(int ped, const std::vector<Expression>& read)
        {
            // Three ids spread over the list: the native's value must agree.
            if (read.empty())
                return true;
            const std::size_t picks[3] = {0, read.size() / 2, read.size() - 1};
            for (const std::size_t n : picks)
            {
                ++g_stats.crossChecks;
                const float native = PED::_GET_CHAR_EXPRESSION(ped, read[n].id);
                if (std::fabs(native - read[n].value) > 1e-4f)
                {
                    ++g_stats.crossMismatches;
                    return false;
                }
            }
            return true;
        }

        constexpr std::uint64_t kCrossCheckReads = 24; // the first reads of a session
    }

    bool ReaderOn()
    {
        RunChecks();
        return g.expressions && !g.crossOff;
    }

    std::string ReaderStatus()
    {
        RunChecks();
        std::ostringstream line;
        line << "METAPED_READER,phase=config,expressions=" << (g.expressions && !g.crossOff ? "on" : "off")
             << ",scale=" << (g.scale ? "on" : "off")
             << ",failed=" << (g.crossOff ? "cross_check" : g.failed.c_str())
             << ",cross_checks=" << g_stats.crossChecks << ",cross_mismatches=" << g_stats.crossMismatches;
        return line.str();
    }

    const char* ReadResultText(ReadResult result)
    {
        switch (result)
        {
        case ReadResult::Ok: return "ok";
        case ReadResult::Off: return "off";
        case ReadResult::NoEntity: return "no_entity";
        case ReadResult::NoCreature: return "no_creature";
        case ReadResult::Layout: return "layout";
        case ReadResult::Fault: return "fault";
        default: return "?";
        }
    }

    ReadResult ReadExpressions(int ped, std::vector<Expression>& out, std::uint32_t& notSet)
    {
        out.clear();
        notSet = 0;
        if (!ReaderOn())
            return ReadResult::Off;
        const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(replay::entities::AddressOf(ped));
        if (!address)
            return ReadResult::NoEntity;
        std::uintptr_t buffer = 0, data = 0;
        ReadResult result = Frame(address, buffer, data);
        if (result != ReadResult::Ok)
        {
            ++g_stats.readFailures;
            return result;
        }
        std::uintptr_t table = 0;
        std::uint32_t count = 0;
        if (!Get(data, table) || !Get(data + kDofCountOffset, count) || !table)
        {
            ++g_stats.readFailures;
            return ReadResult::Fault;
        }
        if (count == 0)
            return ReadResult::NoCreature;
        if (count > kMaxDofs)
        {
            ++g_stats.readFailures;
            return ReadResult::Layout;
        }
        static std::vector<std::uint8_t> dofs;
        static std::vector<std::uint8_t> values;
        dofs.resize(static_cast<std::size_t>(count) * kDofBytes);
        if (!replay::mem::Copy(dofs.data(), table, dofs.size()))
        {
            ++g_stats.readFailures;
            return ReadResult::Fault;
        }
        const std::size_t bytes = ExpressionBufferBytes(dofs.data(), count);
        values.assign(bytes, std::uint8_t{0});
        if (bytes && !replay::mem::Copy(values.data(), buffer, bytes))
        {
            ++g_stats.readFailures;
            return ReadResult::Fault;
        }
        if (!ParseExpressions(dofs.data(), count, values.data(), values.size(), out, notSet))
        {
            ++g_stats.readFailures;
            out.clear();
            return ReadResult::Layout;
        }
        ++g_stats.reads;
        if (g_stats.reads <= kCrossCheckReads && !CrossCheck(ped, out))
        {
            g.crossOff = true; // the structure is not what the native reads: off for the session
            out.clear();
            return ReadResult::Off;
        }
        return ReadResult::Ok;
    }

    ReadResult ReadScale(int ped, float& out)
    {
        out = 0.0f;
        RunChecks();
        if (!g.scale)
            return ReadResult::Off;
        const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(replay::entities::AddressOf(ped));
        if (!address)
            return ReadResult::NoEntity;
        // The scale setter's ready test (kScaleReady): ready when the dressing
        // object has +0xB72 and +0xB75 at 0 (its extension test on ped + 0x10 is
        // not repeated).
        std::uintptr_t drawData = 0, dressing = 0;
        std::uint8_t busyA = 1, busyB = 1;
        bool ready = false;
        if (Get(address + 0x28, drawData) && drawData && Get(drawData + kDressingOffset, dressing) && dressing &&
            Get(dressing + kDressingBusyA, busyA) && Get(dressing + kDressingBusyB, busyB))
            ready = busyA == 0 && busyB == 0;
        std::uint32_t index = 0;
        std::uintptr_t table = 0;
        if (!PedIndex(address, g.scaleBase, index) || !Get(g.scaleTable, table) || !table)
            return ReadResult::Fault;
        const std::uintptr_t entry = table + static_cast<std::uintptr_t>(index) * kScaleEntryStride;
        std::uint64_t component = 0;
        if (!Get(entry + (ready ? kScaleReadyComponent : kScalePendingComponent), component))
            return ReadResult::Fault;
        if (!component)
            return ReadResult::NoCreature;
        float scale = 0.0f;
        if (!Get(static_cast<std::uintptr_t>(component & ~1ull) + (ready ? kScaleReadyField : kScalePendingField), scale))
            return ReadResult::Fault;
        if (!ScaleValid(scale))
            return ReadResult::Layout;
        out = scale;
        return ReadResult::Ok;
    }

    void Capture(int ped, State& state)
    {
        state.flags = 0;
        state.outfit = PED::_GET_PED_META_OUTFIT_HASH(ped);
        state.metapedType = PED::_GET_META_PED_TYPE(ped);
        state.cleanliness = PED::_GET_PED_DAMAGE_CLEANLINESS(ped);
        if (state.cleanliness >= 0)
            state.flags |= kHasCleanliness;
        // The shop items and their wearable states (natives).
        entity::appearance::ReadShopItems(ped, state.shopItems);
        state.flags |= kHasShopItems;
        std::uint32_t notSet = 0;
        if (ReadExpressions(ped, state.expressions, notSet) == ReadResult::Ok)
            state.flags |= kHasExpressions;
        float scale = 0.0f;
        if (ReadScale(ped, scale) == ReadResult::Ok)
        {
            state.scale = scale;
            state.flags |= kHasScale;
        }
    }

    ApplyReport Apply(int puppet, const State& state)
    {
        ApplyReport report;
        ++g_stats.applies;
        bool changed = false;
        if (state.flags & kHasExpressions)
        {
            static std::vector<Expression> have;
            static std::vector<Expression> set;
            std::uint32_t notSet = 0;
            report.puppetRead = ReadExpressions(puppet, have, notSet) == ReadResult::Ok;
            if (!report.puppetRead)
                have.clear();
            ToApply(state.expressions, have, 1e-4f, set);
            for (const Expression& e : set)
                PED::_SET_CHAR_EXPRESSION(puppet, e.id, e.value);
            report.expressionsSet = static_cast<std::uint32_t>(set.size());
            g_stats.expressionsSet += set.size();
            changed = changed || !set.empty();
        }
        if ((state.flags & kHasScale) && ScaleValid(state.scale))
        {
            float have = 1.0f;
            const bool read = ReadScale(puppet, have) == ReadResult::Ok;
            if (!read ? ScaleNotDefault(state.scale) : std::fabs(have - state.scale) > 0.005f)
            {
                PED::_SET_PED_SCALE(puppet, state.scale);
                report.scaleSet = true;
            }
        }
        if ((state.flags & kHasCleanliness) && state.cleanliness >= 0 &&
            PED::_GET_PED_DAMAGE_CLEANLINESS(puppet) != state.cleanliness)
        {
            PED::_SET_PED_DAMAGE_CLEANLINESS(puppet, state.cleanliness);
            report.cleanlinessSet = true;
        }
        if (changed)
            PED::_UPDATE_PED_VARIATION(puppet, FALSE, TRUE, TRUE, TRUE, FALSE);
        return report;
    }

    Check Verify(int puppet, const State& state)
    {
        Check check;
        if (state.flags & kHasExpressions)
        {
            static std::vector<Expression> have;
            std::uint32_t notSet = 0;
            check.read = ReadExpressions(puppet, have, notSet) == ReadResult::Ok;
            if (check.read)
                check.expressions = Compare(state.expressions, have, 1e-3f);
        }
        check.scaleRead = ReadScale(puppet, check.scale) == ReadResult::Ok;
        check.cleanliness = PED::_GET_PED_DAMAGE_CLEANLINESS(puppet);
        return check;
    }

    const Stats& Counters() { return g_stats; }
    void ResetCounters() { g_stats = Stats{}; }
}
