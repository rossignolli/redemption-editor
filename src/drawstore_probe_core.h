#pragma once

// DrawableStore probe, evidence only, for a crash at RDR2.exe+0x363068.
// Every observed instance destroys the same DrawableStore slot (local index
// 0xF7E5, the edi that 0x2B45BE4 passes to the store's Remove, saved at crash
// rsp+0xF0 by 0x621A24). The crash is in that drawable's own destructor: its
// shader group's page is decommitted while the shader group releases its
// texture dictionary. The store took the "delete the whole object" branch
// (entry flag bit 0), a bit the game's own load callback never sets in this
// build. DRAWSTORE_SLOT logs that slot, the load callback and a census of the
// entries with bit 0, so a crash names the asset and shows who set the bit.
// Pure reads, no behaviour change.
//
// Layout (build 1491.50):
//   global      0x5AFA6B0, the store: static ctor 0xE8280 does
//               lea rcx, [0x5AFA6B0]; call 0x2B85BF8, which passes the name
//               "DrawableStore" (0x3670128) to the store base ctor and
//               writes the vtable 0x366EBA0 (its only writer).
//   vtable      slot 7 (+0x38) Remove 0x2BB1E10; slot 10 (+0x50) Load
//               0x2BAB440; slot 39 (+0x138) GetSize 0x44130C (mov eax,[rcx+0x50]).
//   store       +0x40 entries, +0x48 one flag byte per entry (0x80 = free),
//               +0x50 entry count, +0x54 entry size (Remove 0x2BB1E10);
//               +0xA0 load callback object, +0xA8 load callback function
//               (Load 0x2BAB440 reads both; 0xAA9004 installs {0, 0xAB1510}).
//   entry       +0x00 object, +0x10 name hash (RemoveSlot 0xEF0F78 passes it
//               to the name map), +0x20 parent texture dictionary slot (Load
//               0x2BAB440 pushes it as the current dictionary; -1 none),
//               +0x24 u16 flags: bit 0 = Remove deletes the whole object
//               (set only by Load when the callback says so; 0xAB1510 always
//               says no), bit 1 = vtable slots 15/16.
//   drawable    vtable 0x33C48D0 (gtaDrawable, dtor 0x61F564), +0x10 shader
//               group (rmcDrawable dtor 0x26FC828 deletes it).
//   shader grp  vtable 0x362EC90, +0x08 texture dictionary, released with
//               lock xadd [+0x18] (dtor 0x2700618).
//   dictionary  vtable 0x33BAD48 (pgDictionary<grcTexture>), +0x08 page map
//               node (pgBase dtor 0x2601570), +0x10 parent dictionary and
//               +0x18 refcount (dtor 0x559B88 releases the parent).

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace replay::drawstore
{
    // ------------------------------------------------------------ layout (build 1491.50)
    constexpr std::uint32_t kStoreRva = 0x5AFA6B0;
    constexpr std::uint32_t kStoreVtableRva = 0x366EBA0;
    constexpr std::uint32_t kRemoveRva = 0x2BB1E10;
    constexpr std::uint32_t kLoadRva = 0x2BAB440;
    constexpr std::uint32_t kGetSizeRva = 0x44130C;
    constexpr std::uint32_t kGameLoadCallbackRva = 0xAB1510;
    constexpr std::uint32_t kNameStringRva = 0x3670128; // "DrawableStore"
    constexpr std::uint32_t kDrawableVtableRva = 0x33C48D0;
    constexpr std::uint32_t kShaderGroupVtableRva = 0x362EC90;
    constexpr std::uint32_t kDictionaryVtableRva = 0x33BAD48;

    constexpr std::size_t kVtableRemoveSlot = 7;
    constexpr std::size_t kVtableLoadSlot = 10;
    constexpr std::size_t kVtableGetSizeSlot = 39;

    constexpr std::size_t kStoreEntries = 0x40;
    constexpr std::size_t kStoreFlags = 0x48;
    constexpr std::size_t kStoreCount = 0x50;
    constexpr std::size_t kStoreEntrySize = 0x54;
    constexpr std::size_t kStoreLoadObject = 0xA0;
    constexpr std::size_t kStoreLoadFunction = 0xA8;

    constexpr std::size_t kEntryObject = 0x00;
    constexpr std::size_t kEntryRaw08 = 0x08; // not proven (logged raw)
    constexpr std::size_t kEntryName = 0x10;
    constexpr std::size_t kEntryParentTxd = 0x20;
    constexpr std::size_t kEntryFlags = 0x24;
    constexpr std::size_t kEntryMinBytes = 0x26;
    constexpr std::size_t kEntryMaxBytes = 0x100;

    constexpr std::uint8_t kSlotFree = 0x80;
    constexpr std::uint16_t kFlagWholeDelete = 0x1;

    constexpr std::size_t kDrawableShaderGroup = 0x10;
    constexpr std::size_t kShaderGroupDictionary = 0x08;
    constexpr std::size_t kDictionaryPageNode = 0x08;
    constexpr std::size_t kDictionaryParent = 0x10;
    constexpr std::size_t kDictionaryRefs = 0x18;

    // The slot of the observed crashes (a local index of one install).
    constexpr std::uint32_t kWatchedSlot = 0xF7E5;
    // Sanity bound of the census (the store holds tens of thousands).
    constexpr std::uint32_t kMaxEntries = 0x80000;
    constexpr std::size_t kFirstListed = 6;

    // ------------------------------------------------------------ signatures
    // Bytes at an RVA, "??" a wildcard.
    struct Signature
    {
        const char* name;
        std::uint32_t rva;
        const char* bytes;
    };

    // A rel32 at `rva + dispOffset` of an instruction of `length` bytes that
    // must reach `target`.
    struct Rel32Check
    {
        const char* name;
        std::uint32_t rva;
        std::uint32_t dispOffset;
        std::uint32_t length;
        std::uint32_t target;
    };

    inline constexpr Signature kSignatures[] = {
        // static ctor: lea rcx, [store]; call ctor
        {"store_init", 0xE8280, "48 83 EC 28 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ??"},
        // ctor: lea rdx, [name] ... lea rax, [vtable]; mov [rbx], rax
        {"store_ctor", 0x2B85BF8, "40 53 48 83 EC 40 8B 05 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ??"},
        {"store_vtable_write", 0x2B85C36, "48 8D 05 ?? ?? ?? ?? 48 89 03"},
        // Remove: +0x54 size, +0x40 entries, +0x48 flags, 0x80 free, entry+0 object, entry+0x24 bit 0
        {"remove", kRemoveRva,
            "40 53 48 83 EC 20 8B 41 54 0F AF C2 4C 63 C2 BA 01 00 00 00 48 63 D8 48 03 59 40 48 8B 41 48 42 0F B6 0C 00 "
            "81 E1 80 00 00 00 48 F7 D9 48 C1 F9 3F 48 F7 D1 48 23 D9 48 8B 0B 84 53 24"},
        // Load: entry+0x20 the parent dictionary slot; the callback pair; flag bit 0
        {"load_parent_txd", 0x2BAB489, "8B 56 20"},
        {"load_callback_object", 0x2BAB498, "48 8B 0D ?? ?? ?? ??"},
        {"load_callback_call", 0x2BAB4C1, "FF 15 ?? ?? ?? ??"},
        {"load_flag", 0x2BAB4C7, "66 BF 01 00 48 8B D0 40 38 7C 24 38 74 19 66 09 7E 24"},
        // RemoveSlot: entry+0x10 the name
        {"remove_slot_name", 0xEF0FCB, "8B 57 10 48 8D 4E 78"},
        // GetSize: +0x50
        {"get_size", kGetSizeRva, "8B 41 50 C3"},
        // the game's callback installed at the store's +0xA0 (0xAA9004):
        // {0, fn} built at [rbp-0x10], loaded into xmm0, stored 3 lines later
        {"callback_install", 0xAA902E,
            "48 83 65 F0 00 48 8D 05 ?? ?? ?? ?? 48 89 45 F8 48 8D 0D ?? ?? ?? ?? 0F 10 45 F0 48 83 65 F0 00 "
            "48 8D 05 ?? ?? ?? ?? 48 89 45 F8 48 8D 05 ?? ?? ?? ?? F3 0F 7F 05 ?? ?? ?? ??"},
        // object fields read for the watched slot
        {"drawable_vtable", 0x61F56A, "48 8D 05 ?? ?? ?? ??"},
        {"drawable_shader_group", 0x26FC90B, "48 8B 4F 10 48 89 AF A0 00 00 00 48 85 C9 74 0A 48 8B 01 BA 01 00 00 00 FF 10"},
        {"shader_group_vtable", 0x270062E, "48 8D 05 ?? ?? ?? ??"},
        {"shader_group_dictionary", 0x2700671, "48 8B 4B 08 48 85 C9 74 1C 83 C8 FF F0 0F C1 41 18"},
        {"dictionary_vtable", 0x26FC26E, "48 8D 05 ?? ?? ?? ??"},
        {"dictionary_parent", 0x559BA6, "48 8B 49 10 48 85 C9 74 1E 83 C8 FF F0 0F C1 41 18"},
        {"dictionary_page_node", 0x2601577, "48 8B 41 08 33 D2 48 85 C0"},
    };

    inline constexpr Rel32Check kRel32Checks[] = {
        {"store_init_lea", 0xE8284, 3, 7, kStoreRva},
        {"store_init_call", 0xE828B, 1, 5, 0x2B85BF8},
        {"store_ctor_name", 0x2B85C04, 3, 7, kNameStringRva},
        {"store_vtable_write", 0x2B85C36, 3, 7, kStoreVtableRva},
        {"load_callback_object", 0x2BAB498, 3, 7, kStoreRva + static_cast<std::uint32_t>(kStoreLoadObject)},
        {"load_callback_call", 0x2BAB4C1, 2, 6, kStoreRva + static_cast<std::uint32_t>(kStoreLoadFunction)},
        {"callback_install", 0xAA9033, 3, 7, kGameLoadCallbackRva},
        {"callback_store", 0xAA9060, 4, 8, kStoreRva + static_cast<std::uint32_t>(kStoreLoadObject)},
        {"drawable_vtable", 0x61F56A, 3, 7, kDrawableVtableRva},
        {"shader_group_vtable", 0x270062E, 3, 7, kShaderGroupVtableRva},
        {"dictionary_vtable", 0x26FC26E, 3, 7, kDictionaryVtableRva},
    };

    inline constexpr char kStoreName[] = "DrawableStore";

    // Parses "48 8D ?? 05" into bytes and a wildcard mask; false on a
    // malformed pattern or more than `capacity` bytes.
    inline bool ParsePattern(const char* text, std::uint8_t* bytes, bool* wild, std::size_t capacity, std::size_t& count)
    {
        count = 0;
        if (!text)
            return false;
        const char* p = text;
        while (*p)
        {
            while (*p == ' ')
                ++p;
            if (!*p)
                break;
            if (!p[1] || count >= capacity)
                return false;
            if (p[0] == '?' && p[1] == '?')
            {
                wild[count] = true;
                bytes[count] = 0;
            }
            else
            {
                auto nibble = [](char c) -> int {
                    if (c >= '0' && c <= '9')
                        return c - '0';
                    if (c >= 'A' && c <= 'F')
                        return c - 'A' + 10;
                    if (c >= 'a' && c <= 'f')
                        return c - 'a' + 10;
                    return -1;
                };
                const int hi = nibble(p[0]);
                const int lo = nibble(p[1]);
                if (hi < 0 || lo < 0)
                    return false;
                wild[count] = false;
                bytes[count] = static_cast<std::uint8_t>(hi * 16 + lo);
            }
            ++count;
            p += 2;
            if (*p && *p != ' ')
                return false;
        }
        return count != 0;
    }

    inline bool PatternMatches(const char* text, const std::uint8_t* code, std::size_t codeBytes)
    {
        std::uint8_t want[96] = {};
        bool wild[96] = {};
        std::size_t count = 0;
        if (!ParsePattern(text, want, wild, sizeof(want), count) || count > codeBytes)
            return false;
        for (std::size_t n = 0; n < count; ++n)
            if (!wild[n] && code[n] != want[n])
                return false;
        return true;
    }

    // The RVA a rel32 reaches (instruction RVA + length + displacement).
    inline std::int64_t Rel32Target(std::uint32_t rva, std::uint32_t length, std::int32_t displacement)
    {
        return static_cast<std::int64_t>(rva) + length + displacement;
    }

    // ------------------------------------------------------------ store header
    struct StoreHeader
    {
        std::uint64_t entries = 0;
        std::uint64_t flags = 0;
        std::uint32_t count = 0;
        std::uint32_t entrySize = 0;
    };

    // Whether the header can be walked: both arrays, a count within the
    // bound and an entry that holds the fields read.
    inline bool HeaderSane(const StoreHeader& h)
    {
        return h.entries >= 0x10000 && h.flags >= 0x10000 && h.count > 0 && h.count <= kMaxEntries &&
            h.entrySize >= kEntryMinBytes && h.entrySize <= kEntryMaxBytes;
    }

    // ------------------------------------------------------------ entries
    struct Entry
    {
        bool used = false;
        std::uint64_t object = 0;
        std::uint32_t raw08 = 0;
        std::uint32_t name = 0;
        std::int32_t parentTxd = -1;
        std::uint16_t flags = 0;
    };

    template <typename T>
    inline T Field(const std::uint8_t* entry, std::size_t offset)
    {
        T value{};
        std::memcpy(&value, entry + offset, sizeof(T));
        return value;
    }

    // `entry` holds at least kEntryMinBytes bytes.
    inline Entry DecodeEntry(std::uint8_t slotFlags, const std::uint8_t* entry)
    {
        Entry e;
        e.used = (slotFlags & kSlotFree) == 0;
        if (!e.used)
            return e;
        e.object = Field<std::uint64_t>(entry, kEntryObject);
        e.raw08 = Field<std::uint32_t>(entry, kEntryRaw08);
        e.name = Field<std::uint32_t>(entry, kEntryName);
        e.parentTxd = Field<std::int32_t>(entry, kEntryParentTxd);
        e.flags = Field<std::uint16_t>(entry, kEntryFlags);
        return e;
    }

    struct Listed
    {
        std::uint32_t slot = 0;
        std::uint32_t name = 0;
        bool loaded = false;
    };

    // The entries with flag bit 0 (whole delete), counted over a walk.
    struct Census
    {
        std::uint32_t scanned = 0;
        std::uint32_t used = 0;
        std::uint32_t loaded = 0;
        std::uint32_t wholeDelete = 0;
        std::uint32_t wholeDeleteLoaded = 0;
        std::size_t listed = 0;
        Listed first[kFirstListed] = {};
        bool complete = false; // every entry was read
    };

    // Adds `count` entries starting at `firstSlot`; `flags` one byte per
    // entry, `entries` `count * entrySize` bytes.
    inline void AddToCensus(Census& census, std::uint32_t firstSlot, const std::uint8_t* flags, const std::uint8_t* entries,
        std::uint32_t count, std::uint32_t entrySize)
    {
        for (std::uint32_t n = 0; n < count; ++n)
        {
            ++census.scanned;
            const Entry e = DecodeEntry(flags[n], entries + static_cast<std::size_t>(n) * entrySize);
            if (!e.used)
                continue;
            ++census.used;
            const bool loaded = e.object != 0;
            if (loaded)
                ++census.loaded;
            if (!(e.flags & kFlagWholeDelete))
                continue;
            ++census.wholeDelete;
            if (loaded)
                ++census.wholeDeleteLoaded;
            if (census.listed < kFirstListed)
                census.first[census.listed++] = Listed{firstSlot + n, e.name, loaded};
        }
    }

    // ------------------------------------------------------------ the line
    enum class Callback : std::uint8_t
    {
        Unread = 0,
        Game = 1,  // {0, RDR2+0xAB1510} as installed by 0xAA9004
        Other = 2, // anything else: another function, or an object set
    };

    inline const char* CallbackText(Callback c)
    {
        return c == Callback::Game ? "game" : c == Callback::Other ? "other" : "unread";
    }

    inline Callback ClassifyCallback(bool read, std::uint64_t object, std::uint64_t function, std::uint64_t base)
    {
        if (!read)
            return Callback::Unread;
        return object == 0 && function == base + kGameLoadCallbackRva ? Callback::Game : Callback::Other;
    }

    // An absolute address as "RDR2+0x..." inside the image, raw otherwise.
    inline std::string AddressText(std::uint64_t value, std::uint64_t base, std::uint64_t imageSize)
    {
        char text[40];
        if (base && value >= base && value < base + imageSize)
            std::snprintf(text, sizeof(text), "RDR2+0x%llX", static_cast<unsigned long long>(value - base));
        else
            std::snprintf(text, sizeof(text), "0x%llX", static_cast<unsigned long long>(value));
        return text;
    }

    // The watched slot's object chain (each step read only when the one
    // before it was).
    struct SlotChain
    {
        bool inRange = false;
        bool entryRead = false;
        Entry entry;
        int drawableRead = -1; // -1 not asked, 0 unreadable, 1 read
        bool drawableVtableOk = false;
        std::uint64_t shaderGroup = 0;
        int shaderGroupRead = -1;
        bool shaderGroupVtableOk = false;
        std::uint64_t dictionary = 0;
        int dictionaryRead = -1;
        bool dictionaryVtableOk = false;
        std::uint64_t pageNode = 0;
        std::uint64_t parent = 0;
        std::int32_t refs = 0;
        int parentRead = -1;
        bool parentVtableOk = false;
        std::int32_t parentRefs = 0;
    };

    inline const char* ReadText(int state)
    {
        return state < 0 ? "-" : state ? "read" : "unreadable";
    }

    inline void AppendChain(std::string& out, const SlotChain& s)
    {
        char text[512];
        if (!s.inRange)
        {
            out += ",slot_in_range=0";
            return;
        }
        if (!s.entryRead)
        {
            out += ",entry=unreadable";
            return;
        }
        const Entry& e = s.entry;
        std::snprintf(text, sizeof(text), ",used=%d,name=0x%08X,parent_txd=%d,flags=0x%04X,whole_delete=%d,raw08=0x%X,object=0x%llX",
            e.used ? 1 : 0, e.name, e.parentTxd, e.flags, (e.flags & kFlagWholeDelete) ? 1 : 0, e.raw08,
            static_cast<unsigned long long>(e.object));
        out += text;
        if (s.drawableRead < 0)
            return;
        std::snprintf(text, sizeof(text), ",object_read=%s,object_vt=%s", ReadText(s.drawableRead),
            s.drawableRead > 0 ? (s.drawableVtableOk ? "drawable" : "other") : "-");
        out += text;
        if (s.drawableRead <= 0)
            return;
        std::snprintf(text, sizeof(text), ",shader_group=0x%llX,shader_group_read=%s,shader_group_vt=%s",
            static_cast<unsigned long long>(s.shaderGroup), ReadText(s.shaderGroupRead),
            s.shaderGroupRead > 0 ? (s.shaderGroupVtableOk ? "ok" : "other") : "-");
        out += text;
        if (s.shaderGroupRead <= 0)
            return;
        std::snprintf(text, sizeof(text), ",txd=0x%llX,txd_read=%s,txd_vt=%s", static_cast<unsigned long long>(s.dictionary),
            ReadText(s.dictionaryRead), s.dictionaryRead > 0 ? (s.dictionaryVtableOk ? "dict" : "other") : "-");
        out += text;
        if (s.dictionaryRead <= 0)
            return;
        std::snprintf(text, sizeof(text), ",txd_refs=%d,txd_page_node=0x%llX,txd_parent=0x%llX", s.refs,
            static_cast<unsigned long long>(s.pageNode), static_cast<unsigned long long>(s.parent));
        out += text;
        if (s.parentRead < 0)
            return;
        std::snprintf(text, sizeof(text), ",parent_read=%s,parent_vt=%s,parent_refs=%d", ReadText(s.parentRead),
            s.parentRead > 0 ? (s.parentVtableOk ? "dict" : "other") : "-", s.parentRead > 0 ? s.parentRefs : 0);
        out += text;
    }

    inline void AppendCensus(std::string& out, const Census& c)
    {
        char text[160];
        std::snprintf(text, sizeof(text), ",census=%s,scanned=%u,used=%u,loaded=%u,whole_delete=%u,whole_delete_loaded=%u,first=",
            c.complete ? "complete" : "partial", c.scanned, c.used, c.loaded, c.wholeDelete, c.wholeDeleteLoaded);
        out += text;
        if (!c.listed)
            out += '-';
        for (std::size_t n = 0; n < c.listed; ++n)
        {
            std::snprintf(text, sizeof(text), "%s0x%X:0x%08X:%d", n ? ";" : "", c.first[n].slot, c.first[n].name,
                c.first[n].loaded ? 1 : 0);
            out += text;
        }
    }
}
