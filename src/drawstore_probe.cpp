// DRAWSTORE_SLOT: the DrawableStore slot 0xF7E5 that the crash at
// RDR2.exe+0x363068 destroys, the store's load callback and the census of
// the entries with flag bit 0. Layout and rules in drawstore_probe_core.h.
// Pure guarded reads (replay::mem::Copy: range filter and SEH); nothing is
// written to the engine and nothing is called in it.

#include "drawstore_probe.h"

#include "drawstore_probe_core.h"
#include "guarded_memory.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace replay::drawstore
{
    namespace
    {
        enum class State : std::uint8_t
        {
            Unchecked,
            Ready,
            Disabled,
        };

        State g_state = State::Unchecked;
        std::string g_failure;
        std::uintptr_t g_base = 0;
        std::uint64_t g_imageSize = 0;
        std::uint32_t g_lines = 0;

        // The crash handler's copy: two buffers, the index of the complete one.
        constexpr int kLineBytes = 1024;
        char g_lastLines[2][kLineBytes] = {};
        std::atomic<int> g_lastLength[2] = {};
        std::atomic<int> g_lastIndex{-1};

        void KeepLastLine(const std::string& line)
        {
            const int next = g_lastIndex.load(std::memory_order_acquire) == 0 ? 1 : 0;
            const int length = static_cast<int>((std::min)(line.size(), static_cast<std::size_t>(kLineBytes - 1)));
            g_lastLength[next].store(0, std::memory_order_release);
            std::memcpy(g_lastLines[next], line.data(), static_cast<std::size_t>(length));
            g_lastLines[next][length] = 0;
            g_lastLength[next].store(length, std::memory_order_release);
            g_lastIndex.store(next, std::memory_order_release);
        }

        template <typename T>
        bool ReadAt(std::uintptr_t address, T& out)
        {
            return replay::mem::Read(address, out);
        }

        bool ImageRange()
        {
            const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            IMAGE_DOS_HEADER dos{};
            if (!base || !replay::mem::Copy(&dos, base, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE)
                return false;
            IMAGE_NT_HEADERS64 nt{};
            if (!replay::mem::Copy(&nt, base + static_cast<std::uintptr_t>(dos.e_lfanew), sizeof(nt)))
                return false;
            g_base = base;
            g_imageSize = nt.OptionalHeader.SizeOfImage;
            return true;
        }

        // Every signature, every rel32 target, the name string and the
        // store's vtable slots; the first failure's name, or empty.
        std::string CheckLayout()
        {
            if (!ImageRange())
                return "image";
            std::uint8_t code[128] = {};
            for (const Signature& s : kSignatures)
            {
                std::uint8_t want[96] = {};
                bool wild[96] = {};
                std::size_t count = 0;
                if (!ParsePattern(s.bytes, want, wild, sizeof(want), count) || s.rva + count > g_imageSize ||
                    !replay::mem::Copy(code, g_base + s.rva, count) || !PatternMatches(s.bytes, code, count))
                    return s.name;
            }
            for (const Rel32Check& r : kRel32Checks)
            {
                std::int32_t displacement = 0;
                if (r.rva + r.length > g_imageSize || !ReadAt(g_base + r.rva + r.dispOffset, displacement) ||
                    Rel32Target(r.rva, r.length, displacement) != static_cast<std::int64_t>(r.target))
                    return std::string(r.name) + "_target";
            }
            char name[sizeof(kStoreName)] = {};
            if (!replay::mem::Copy(name, g_base + kNameStringRva, sizeof(name)) || std::memcmp(name, kStoreName, sizeof(name)) != 0)
                return "store_name";
            std::uint64_t vtable = 0;
            if (!ReadAt(g_base + kStoreRva, vtable) || vtable != g_base + kStoreVtableRva)
                return "store_vtable";
            const struct
            {
                std::size_t slot;
                std::uint32_t rva;
                const char* name;
            } slots[] = {
                {kVtableRemoveSlot, kRemoveRva, "vtable_remove"},
                {kVtableLoadSlot, kLoadRva, "vtable_load"},
                {kVtableGetSizeSlot, kGetSizeRva, "vtable_get_size"},
            };
            for (const auto& s : slots)
            {
                std::uint64_t function = 0;
                if (!ReadAt(static_cast<std::uintptr_t>(vtable) + s.slot * 8, function) || function != g_base + s.rva)
                    return s.name;
            }
            return {};
        }

        void ReadChain(const StoreHeader& header, SlotChain& s)
        {
            s.inRange = kWatchedSlot < header.count;
            if (!s.inRange)
                return;
            std::uint8_t slotFlags = 0;
            std::uint8_t entry[kEntryMaxBytes] = {};
            const std::uintptr_t at = static_cast<std::uintptr_t>(header.entries) + static_cast<std::uintptr_t>(kWatchedSlot) * header.entrySize;
            s.entryRead = ReadAt(static_cast<std::uintptr_t>(header.flags) + kWatchedSlot, slotFlags) &&
                replay::mem::Copy(entry, at, kEntryMinBytes);
            if (!s.entryRead)
                return;
            s.entry = DecodeEntry(slotFlags, entry);
            if (!s.entry.used || !s.entry.object)
                return;
            const auto object = static_cast<std::uintptr_t>(s.entry.object);
            std::uint64_t vtable = 0;
            s.drawableRead = ReadAt(object, vtable) && ReadAt(object + kDrawableShaderGroup, s.shaderGroup) ? 1 : 0;
            if (s.drawableRead <= 0)
                return;
            s.drawableVtableOk = vtable == g_base + kDrawableVtableRva;
            if (!s.shaderGroup)
                return;
            const auto group = static_cast<std::uintptr_t>(s.shaderGroup);
            s.shaderGroupRead = ReadAt(group, vtable) && ReadAt(group + kShaderGroupDictionary, s.dictionary) ? 1 : 0;
            if (s.shaderGroupRead <= 0)
                return;
            s.shaderGroupVtableOk = vtable == g_base + kShaderGroupVtableRva;
            if (!s.dictionary)
                return;
            const auto dictionary = static_cast<std::uintptr_t>(s.dictionary);
            s.dictionaryRead = ReadAt(dictionary, vtable) && ReadAt(dictionary + kDictionaryPageNode, s.pageNode) &&
                    ReadAt(dictionary + kDictionaryParent, s.parent) && ReadAt(dictionary + kDictionaryRefs, s.refs)
                ? 1
                : 0;
            if (s.dictionaryRead <= 0)
                return;
            s.dictionaryVtableOk = vtable == g_base + kDictionaryVtableRva;
            if (!s.parent)
                return;
            const auto parent = static_cast<std::uintptr_t>(s.parent);
            s.parentRead = ReadAt(parent, vtable) && ReadAt(parent + kDictionaryRefs, s.parentRefs) ? 1 : 0;
            if (s.parentRead > 0)
                s.parentVtableOk = vtable == g_base + kDictionaryVtableRva;
        }

        // The walk: the flag bytes at once, the entries in chunks.
        void ReadCensus(const StoreHeader& header, Census& census)
        {
            static std::vector<std::uint8_t> flags;
            static std::vector<std::uint8_t> entries;
            constexpr std::uint32_t kChunk = 4096;
            flags.resize(header.count);
            entries.resize(static_cast<std::size_t>(kChunk) * header.entrySize);
            if (!replay::mem::Copy(flags.data(), static_cast<std::uintptr_t>(header.flags), header.count))
                return;
            for (std::uint32_t first = 0; first < header.count; first += kChunk)
            {
                const std::uint32_t count = (std::min)(kChunk, header.count - first);
                const std::uintptr_t at = static_cast<std::uintptr_t>(header.entries) + static_cast<std::uintptr_t>(first) * header.entrySize;
                if (!replay::mem::Copy(entries.data(), at, static_cast<std::size_t>(count) * header.entrySize))
                    return;
                AddToCensus(census, first, flags.data() + first, entries.data(), count, header.entrySize);
            }
            census.complete = census.scanned == header.count;
        }
    }

    void LogSlot(const char* why, std::ostream& log)
    {
        if (g_state == State::Disabled)
            return;
        LARGE_INTEGER frequency{}, started{}, ended{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&started);
        std::string line = "DRAWSTORE_SLOT,why=";
        line += why ? why : "?";
        if (g_state == State::Unchecked)
        {
            g_failure = CheckLayout();
            g_state = g_failure.empty() ? State::Ready : State::Disabled;
            if (g_state == State::Disabled)
            {
                line += ",signature_mismatch=" + g_failure + ",disabled=1";
                log << line << '\n';
                KeepLastLine(line);
                return;
            }
        }
        StoreHeader header;
        std::uint32_t entrySize = 0;
        const std::uintptr_t store = g_base + kStoreRva;
        const bool headerRead = ReadAt(store + kStoreEntries, header.entries) && ReadAt(store + kStoreFlags, header.flags) &&
            ReadAt(store + kStoreCount, header.count) && ReadAt(store + kStoreEntrySize, entrySize);
        header.entrySize = entrySize;
        std::uint64_t callbackObject = 0;
        std::uint64_t callbackFunction = 0;
        const bool callbackRead = ReadAt(store + kStoreLoadObject, callbackObject) && ReadAt(store + kStoreLoadFunction, callbackFunction);
        const Callback callback = ClassifyCallback(callbackRead, callbackObject, callbackFunction, g_base);
        char text[256];
        std::snprintf(text, sizeof(text), ",n=%u,load_cb=%s,load_cb_fn=%s,load_cb_obj=0x%llX,count=%u,entry_size=0x%X,slot=0x%X", ++g_lines,
            CallbackText(callback), callbackRead ? AddressText(callbackFunction, g_base, g_imageSize).c_str() : "-",
            static_cast<unsigned long long>(callbackObject), header.count, header.entrySize, kWatchedSlot);
        line += text;
        if (!headerRead || !HeaderSane(header))
            line += ",header=unreadable";
        else
        {
            SlotChain chain;
            ReadChain(header, chain);
            AppendChain(line, chain);
            Census census;
            ReadCensus(header, census);
            AppendCensus(line, census);
        }
        QueryPerformanceCounter(&ended);
        const long long us = frequency.QuadPart ? (ended.QuadPart - started.QuadPart) * 1000000 / frequency.QuadPart : 0;
        std::snprintf(text, sizeof(text), ",us=%lld", us);
        line += text;
        log << line << '\n';
        KeepLastLine(line);
    }

    int CopyLastLine(char* out, int capacity)
    {
        if (!out || capacity <= 0)
            return 0;
        out[0] = 0;
        const int index = g_lastIndex.load(std::memory_order_acquire);
        if (index < 0)
            return 0;
        const int length = (std::min)(g_lastLength[index].load(std::memory_order_acquire), capacity - 1);
        if (length <= 0)
            return 0;
        std::memcpy(out, g_lastLines[index], static_cast<std::size_t>(length));
        out[length] = 0;
        return length;
    }
}
