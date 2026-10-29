#include "metaped_reader.h"

#include "entity_appearance.h" // ReadShopItems
#include "entity_cache.h"
#include "guarded_memory.h"

#include "natives.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <sstream>

namespace replay::metaped::engine
{
    namespace
    {
        // ---- game build 1491.50: the code the layout comes from ----------
        // _GET_CHAR_EXPRESSION's core: ped -> 0x2BDDE1C (creature) ->
        // 0xB32AD8 (container) -> 0x2BC96C4 (frame + 0x58, track 0x40).
        constexpr std::uint32_t kGetterRva = 0xB44250;
        constexpr std::uint8_t kGetter[] = {
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x83, 0x64, 0x24, 0x30, 0x00, 0x8B, 0xDA, 0x48, 0x85, 0xC9, 0x75, 0x05,
            0x0F, 0x57, 0xC0, 0xEB, 0x2A, 0x33, 0xD2, 0xE8, 0xAE, 0x9B, 0x09, 0x02, 0x48, 0x8B, 0xC8, 0xE8, 0x62, 0xE8,
            0xFE, 0xFF, 0x48, 0x85, 0xC0, 0x74, 0x10, 0x4C, 0x8D, 0x44, 0x24, 0x30, 0x0F, 0xB7, 0xD3, 0x48, 0x8B, 0xC8,
            0xE8, 0x39, 0x54, 0x08, 0x02, 0xF3, 0x0F, 0x10, 0x44, 0x24, 0x30, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3};
        constexpr std::uint32_t kCreatureRva = 0x2BDDE1C;
        constexpr std::uint8_t kCreature[] = {
            0x8B, 0x89, 0x9C, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x05, 0x5F, 0x65, 0x0F, 0x01, 0x81, 0xE1, 0xFF, 0xFF, 0x01,
            0x00, 0x2B, 0x0D, 0x4B, 0x65, 0x0F, 0x01, 0x48, 0x03, 0xC9, 0x48, 0x8B, 0x14, 0xC8, 0x48, 0x8B, 0xCA, 0x48,
            0x83, 0xE1, 0xFE, 0x48, 0xF7, 0xDA, 0x48, 0x1B, 0xC0, 0x48, 0x23, 0xC1, 0x74, 0x05, 0x48, 0x8B, 0x40, 0x28,
            0xC3, 0x33, 0xC0, 0xC3};
        constexpr std::uint32_t kCreatureTableDisp = 0x09, kCreatureTableNext = 0x0D; // qword 0x3CD4388
        constexpr std::uint32_t kCreatureBaseDisp = 0x15, kCreatureBaseNext = 0x19;   // dword 0x3CD4380
        constexpr std::uint32_t kContainerRva = 0xB32AD8;
        constexpr std::uint8_t kContainer[] = {
            0x44, 0x0F, 0xB6, 0x0D, 0x10, 0x01, 0x1B, 0x03, 0x41, 0x83, 0xC8, 0xFF, 0x49, 0x0F, 0xBE, 0xC0, 0x4A, 0x8D,
            0x14, 0x8D, 0x01, 0x00, 0x00, 0x00, 0x48, 0x03, 0xD0, 0x8B, 0xC2, 0x8A, 0x94, 0x08, 0x04, 0x01, 0x00, 0x00,
            0x80, 0xFA, 0x14, 0x75, 0x0C, 0x41, 0xFF, 0xC0, 0x41, 0x83, 0xF8, 0x03, 0x7C, 0xDA, 0x33, 0xC0, 0xC3, 0x0F,
            0xB6, 0xC2, 0x48, 0x8B, 0x44, 0xC1, 0x18, 0xC3};
        constexpr std::uint32_t kContainerIndexDisp = 0x04, kContainerIndexNext = 0x08; // byte 0x3CE2BF0
        constexpr std::uint32_t kFrameRva = 0x2BC96C4;
        constexpr std::uint8_t kFrame[] = {
            0x4D, 0x8B, 0xC8, 0x48, 0x83, 0xC1, 0x58, 0x44, 0x0F, 0xB7, 0xC2, 0xB2, 0x40, 0xE9, 0xE6, 0x66, 0xA9, 0xFF};
        constexpr std::uint32_t kFrameValueRva = 0x265FDBC;
        constexpr std::uint8_t kFrameValue[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x59,
            0x08, 0x48, 0x8B, 0xF9, 0x48, 0x8B, 0xCB, 0x49, 0x8B, 0xF1, 0xE8, 0xDF, 0x82, 0xFF, 0xFF, 0x41, 0x83, 0xC9,
            0xFF, 0x4C, 0x8B, 0xC0, 0x48, 0x85, 0xC0, 0x74, 0x1F, 0x48, 0x8B, 0xD0, 0x48, 0xB8, 0xAB, 0xAA, 0xAA, 0xAA,
            0xAA, 0xAA, 0xAA, 0x2A, 0x48, 0x2B, 0x13, 0x48, 0xF7, 0xEA, 0x48, 0x8B, 0xCA, 0x48, 0xC1, 0xE9, 0x3F, 0x48,
            0x03, 0xD1, 0xEB, 0x03, 0x41, 0x8B, 0xD1, 0x4D, 0x85, 0xC0, 0x74, 0x2B, 0x4C, 0x8B, 0x07, 0x8B, 0xC2, 0x48,
            0x8D, 0x14, 0x40, 0x48, 0x8B, 0x47, 0x08, 0x48, 0x8B, 0x08, 0x0F, 0xB7, 0x04, 0x51, 0x46, 0x39, 0x0C, 0x00,
            0x74, 0x11, 0x48, 0x8B, 0x03, 0x0F, 0xB7, 0x0C, 0x50, 0x42, 0x8B, 0x04, 0x01, 0x89, 0x06, 0xB0, 0x01, 0xEB,
            0x02, 0x32};
        constexpr std::uint32_t kDofFindRva = 0x26580BC;
        constexpr std::uint8_t kDofFind[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x41, 0x0F, 0xB7, 0xC0, 0x4C, 0x8B, 0x01, 0x44, 0x0F, 0xB6, 0xDA, 0x41, 0xC1,
            0xE3, 0x10, 0x44, 0x0B, 0xD8, 0x8B, 0x41, 0x2C, 0x48, 0x8D, 0x0C, 0x40, 0x48, 0xB8, 0xAB, 0xAA, 0xAA, 0xAA,
            0xAA, 0xAA, 0xAA, 0x2A, 0x48, 0x03, 0xC9, 0x48, 0xF7, 0xE9, 0x4A, 0x8D, 0x1C, 0x01, 0x48, 0x8B, 0xC2, 0x48,
            0xC1, 0xE8, 0x3F, 0x48, 0x03, 0xD0, 0xEB, 0x32, 0x4C, 0x8B, 0xCA, 0x49, 0xD1, 0xF9, 0x4B, 0x8D, 0x04, 0x49,
            0x4D, 0x8D, 0x14, 0x40, 0x41, 0x0F, 0xB6, 0x4A, 0x04, 0x41, 0x0F, 0xB7, 0x42, 0x02, 0xC1, 0xE1, 0x10, 0x0B,
            0xC8, 0x41, 0x3B, 0xCB};
        // _SET_PED_SCALE's core: two places by the metaped's state.
        constexpr std::uint32_t kScaleSetRva = 0xFEF2FC;
        constexpr std::uint8_t kScaleSet[] = {
            0x48, 0x85, 0xC9, 0x0F, 0x84, 0x98, 0x00, 0x00, 0x00, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x0F, 0x29, 0x74, 0x24,
            0x20, 0x48, 0x8B, 0xD9, 0x0F, 0x28, 0xF1, 0x0F, 0x2F, 0x35, 0x64, 0x00, 0x32, 0x02, 0x76, 0x75, 0xE8, 0xC5,
            0x7D, 0x8C, 0xFF, 0x8B, 0x8B, 0x9C, 0x00, 0x00, 0x00, 0x81, 0xE1, 0xFF, 0xFF, 0x01, 0x00, 0x2B, 0x0D, 0x0B,
            0xDB, 0x9F, 0x02, 0x84, 0xC0, 0x8B, 0xC1, 0x74, 0x30, 0x48, 0x69, 0xC8, 0x48, 0x01, 0x00, 0x00, 0x0F, 0x28,
            0xCE, 0x48, 0x8B, 0x05, 0xFC, 0xDA, 0x9F, 0x02, 0x48, 0x8B, 0x94, 0x01, 0xE0, 0x00, 0x00, 0x00, 0x48, 0x8B,
            0xC2, 0x48, 0x83, 0xE0, 0xFE, 0x48, 0xF7, 0xDA, 0x48, 0x1B, 0xC9, 0x48, 0x23, 0xC8, 0xE8, 0x33, 0xEB, 0x9F,
            0xFF, 0xEB, 0x28, 0x48, 0x69, 0xC8, 0x48, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x05, 0xCF, 0xDA, 0x9F, 0x02, 0x48,
            0x8B, 0x54, 0x01, 0x50, 0x48, 0x8B, 0xCA, 0x48, 0x83, 0xE1, 0xFE, 0x48, 0xF7, 0xDA, 0x48, 0x1B, 0xC0, 0x48,
            0x23, 0xC1, 0xF3, 0x0F, 0x11, 0x70, 0x48, 0x0F, 0x28, 0x74, 0x24, 0x20, 0x48, 0x83, 0xC4, 0x30, 0x5B, 0xC3};
        constexpr std::uint32_t kScaleBaseDisp = 0x35, kScaleBaseNext = 0x39;   // dword 0x39ECE40
        constexpr std::uint32_t kScaleTableDisp = 0x4C, kScaleTableNext = 0x50; // qword 0x39ECE48
        // The "ready" test the setter branches on (0x8B70E8): the dressing
        // object [[ped + 0x28] + 0x750] with +0xB72 and +0xB75 both 0.
        constexpr std::uint32_t kScaleReadyRva = 0x8B70E8;
        constexpr std::uint8_t kScaleReady[] = {
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x15, 0x20, 0x48, 0x06, 0x04, 0x48, 0x8B, 0xD9, 0x48, 0x83, 0xC1,
            0x10, 0xE8, 0x70, 0x6C, 0x32, 0x02, 0x33, 0xD2, 0x48, 0x85, 0xC0, 0x75, 0x27, 0x48, 0x8B, 0x43, 0x28, 0x48,
            0x85, 0xC0, 0x74, 0x1E, 0x48, 0x8B, 0x80, 0x50, 0x07, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x12, 0x38, 0x90,
            0x72, 0x0B, 0x00, 0x00, 0x75, 0x0A, 0x38, 0x90, 0x75, 0x0B, 0x00, 0x00, 0x75, 0x02, 0xB2, 0x01, 0x8A, 0xC2,
            0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3};
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
        bool g_record = true;
        bool g_apply = true;

        std::uintptr_t Base() { return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)); }

        bool Same(std::uint32_t rva, const std::uint8_t* bytes, std::size_t size)
        {
            return replay::mem::Matches(reinterpret_cast<const void*>(Base() + rva), bytes, size);
        }

        std::uintptr_t RipTarget(std::uint32_t rva, std::uint32_t dispAt, std::uint32_t next)
        {
            std::int32_t disp = 0;
            if (!replay::mem::Read(Base() + rva + dispAt, disp))
                return 0;
            return Base() + rva + next + static_cast<std::intptr_t>(disp);
        }

        void RunChecks()
        {
            if (g.checked)
                return;
            g.checked = true;
            struct Item
            {
                const char* name;
                std::uint32_t rva;
                const std::uint8_t* bytes;
                std::size_t size;
            };
            const Item expressionItems[] = {
                {"char_expression_getter", kGetterRva, kGetter, sizeof(kGetter)},
                {"creature_lookup", kCreatureRva, kCreature, sizeof(kCreature)},
                {"frame_container", kContainerRva, kContainer, sizeof(kContainer)},
                {"expression_frame", kFrameRva, kFrame, sizeof(kFrame)},
                {"frame_value", kFrameValueRva, kFrameValue, sizeof(kFrameValue)},
                {"dof_find", kDofFindRva, kDofFind, sizeof(kDofFind)},
            };
            g.expressions = true;
            for (const Item& item : expressionItems)
                if (!Same(item.rva, item.bytes, item.size))
                {
                    g.expressions = false;
                    g.failed = item.name;
                    break;
                }
            if (g.expressions)
            {
                g.creatureTable = RipTarget(kCreatureRva, kCreatureTableDisp, kCreatureTableNext);
                g.creatureBase = RipTarget(kCreatureRva, kCreatureBaseDisp, kCreatureBaseNext);
                g.bufferIndex = RipTarget(kContainerRva, kContainerIndexDisp, kContainerIndexNext);
                // The expected addresses (a rebased module moves all three together).
                g.expressions = g.creatureTable == Base() + 0x3CD4388 && g.creatureBase == Base() + 0x3CD4380 &&
                    g.bufferIndex == Base() + 0x3CE2BF0;
                if (!g.expressions)
                    g.failed = "expression_globals";
            }
            g.scale = Same(kScaleSetRva, kScaleSet, sizeof(kScaleSet)) && Same(kScaleReadyRva, kScaleReady, sizeof(kScaleReady));
            if (g.scale)
            {
                g.scaleBase = RipTarget(kScaleSetRva, kScaleBaseDisp, kScaleBaseNext);
                g.scaleTable = RipTarget(kScaleSetRva, kScaleTableDisp, kScaleTableNext);
                g.scale = g.scaleBase == Base() + 0x39ECE40 && g.scaleTable == Base() + 0x39ECE48;
            }
            if (!g.scale && g.failed == "none")
                g.failed = "scale_setter";
        }

        template <typename T>
        bool Get(std::uintptr_t address, T& out)
        {
            return replay::mem::Read(address, out);
        }

        // The ped's extension index, as 0x2BDDE1C and 0xFEF2FC compute it.
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

    void SetEnabled(bool record, bool apply)
    {
        g_record = record;
        g_apply = apply;
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
             << ",scale=" << (g.scale ? "on" : "off") << ",record=" << (g_record ? 1 : 0)
             << ",apply=" << (g_apply ? 1 : 0) << ",failed=" << (g.crossOff ? "cross_check" : g.failed.c_str())
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
        // 0x8B70E8: ready when the dressing object has +0xB72 and +0xB75 at 0
        // (its extension test, 0x2BDDD70 on ped + 0x10, is not repeated).
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
        // The shop items and their wearable states (natives, independent of
        // the reader's ini MetapedRecord).
        entity::appearance::ReadShopItems(ped, state.shopItems);
        state.flags |= kHasShopItems;
        if (!g_record)
            return;
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
        if (!g_apply)
            return report;
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
