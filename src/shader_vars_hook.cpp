#include "shader_vars_hook.h"

#include "guarded_memory.h"
#include "hook_batch.h"
#include "module_lifetime.h"

#include <windows.h>

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <initializer_list>
#include <cstdio>
#include <cstring>

namespace replay::shadervars::engine
{
    namespace
    {
        // ---- game build 1491.50 ---------------------------------------------
        // 0xB2F0D0(effect rcx, ped rdx): the prologue up to mov rax,[rdx+0A8h].
        constexpr std::uintptr_t kReaderRva = 0xB2F0D0;
        constexpr std::uint8_t kReader[] = {
            0x48, 0x89, 0x5C, 0x24, 0x20, 0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41,
            0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x00, 0xFD, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x00, 0x04, 0x00, 0x00,
            0x48, 0x8B, 0x82, 0xA8, 0x00, 0x00, 0x00};
        static_assert(sizeof(kReader) >= 5, "the signature covers the detour's jump");
        // 0xB2F115: mov rdi,[rax+48h] (the creature).
        constexpr std::uintptr_t kReaderCreatureRva = 0xB2F115;
        constexpr std::uint8_t kReaderCreature[] = {0x48, 0x8B, 0x78, 0x48};
        // 0xB2F17A: mov rcx,rdi; call 0x87CBD0; mov rdi,rax (the component).
        constexpr std::uintptr_t kReaderFindRva = 0xB2F17A;
        constexpr std::uint8_t kReaderFind[] = {0x48, 0x8B, 0xCF, 0xE8, 0x4E, 0xDA, 0xD4, 0xFF, 0x48, 0x8B, 0xF8};
        // 0xB2F265: movzx edx,[rcx+rsi+8]; mov rcx,[rdi+68h] (offset, buffer).
        constexpr std::uintptr_t kReaderBufferRva = 0xB2F265;
        constexpr std::uint8_t kReaderBuffer[] = {0x0F, 0xB7, 0x54, 0x31, 0x08, 0x48, 0x8B, 0x4F, 0x68};
        // 0x87CBD0: the component of the type byte at 0x3C7F020.
        constexpr std::uintptr_t kFindRva = 0x87CBD0;
        constexpr std::uint8_t kFind[] = {
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8D, 0x0D, 0x40, 0x24, 0x40, 0x03, 0xE8, 0x57,
            0xC7, 0xDD, 0x01, 0x0F, 0xB6, 0xD0, 0x8A, 0x44, 0x1A, 0x74, 0x3C, 0xFF, 0x74, 0x0C, 0x0F, 0xB6, 0xD0, 0x48,
            0x8B, 0x03, 0x48, 0x8B, 0x04, 0xD0, 0xEB, 0x02, 0x33, 0xC0, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3};
        constexpr std::uintptr_t kTypeByteRva = 0x3C7F020;
        // 0x26507EC: the component's ctor (vtable 0x3625E88, type 9, frame data +0x28).
        constexpr std::uintptr_t kCtorRva = 0x26507EC;
        constexpr std::uint8_t kCtor[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x89, 0x51, 0x08, 0x48, 0x8D, 0x05, 0x87,
            0x56, 0xFD, 0x00, 0x48, 0x89, 0x01, 0x48, 0x8D, 0x51, 0x28, 0xC6, 0x41, 0x10, 0x09};
        constexpr std::uintptr_t kVtableRva = 0x3625E88;
        // 0x2659AA0 (vtable slot 5): the tracks 0x11, 0x12 and 0x17 only.
        constexpr std::uintptr_t kHasDofRva = 0x2659AA0;
        constexpr std::uint8_t kHasDof[] = {
            0x48, 0x83, 0xEC, 0x28, 0x8D, 0x42, 0xEF, 0x3C, 0x01, 0x76, 0x09, 0x80, 0xFA, 0x17, 0x74, 0x04};
        // 0x295AFC4(node): the frame write node. Prologue up to the reads of
        // the destination (+0x28), source (+0x30), map (+0x38), full-copy flag (+0x40).
        constexpr std::uintptr_t kWriteNodeRva = 0x295AFC4;
        constexpr std::uint8_t kWriteNode[] = {
            0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x5B, 0x10, 0x49, 0x89, 0x6B, 0x18, 0x49, 0x89, 0x73, 0x20, 0x57, 0x41, 0x54,
            0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x71, 0x28, 0x48, 0x8B, 0x69, 0x30,
            0x48, 0x8B, 0x79, 0x38, 0x44, 0x8A, 0x79, 0x40};
        static_assert(sizeof(kWriteNode) >= 5, "the signature covers the detour's jump");
        // 0x294F159: the component found, its frame (lea rdx,[rax+68h]) queued
        // through 0x29520A8 (the node's destination is the component's buffer).
        constexpr std::uintptr_t kQueueSiteRva = 0x294F159;
        constexpr std::uint8_t kQueueSite[] = {
            0x48, 0x8B, 0x4B, 0x20, 0xE8, 0x6E, 0xDA, 0xF2, 0xFD, 0x48, 0x85, 0xC0, 0x74, 0x1F, 0x4C, 0x8B, 0x43, 0x18,
            0x48, 0x8D, 0x50, 0x68, 0x49, 0x81, 0xC0, 0xF8, 0x01, 0x00, 0x00, 0x40, 0x88, 0x74, 0x24, 0x20, 0x45, 0x33,
            0xC9, 0x48, 0x8B, 0xCB, 0xE8, 0x22, 0x2F, 0x00, 0x00};
        // 0x2952139 (in 0x29520A8): node +0x28 = [frame] (the buffer); 0x2952164:
        // node +0x50 = the frame's u16 +0x34 (its bytes).
        constexpr std::uintptr_t kQueueDestRva = 0x2952139;
        constexpr std::uint8_t kQueueDest[] = {0x48, 0x8B, 0x02, 0x49, 0x89, 0x41, 0x28};
        constexpr std::uintptr_t kQueueBytesRva = 0x2952164;
        constexpr std::uint8_t kQueueBytes[] = {0x0F, 0xB7, 0x42, 0x34, 0x41, 0x89, 0x41, 0x50};
        constexpr std::uintptr_t kNodeDestination = 0x28;
        constexpr std::uintptr_t kNodeBytes = 0x50;

        // 0xB2D32A (the ped shader effect's update): the graphics component,
        // slot +0x98 of the row ((ped+0x9C) & 0x1FFFF) - [0x39ECE40] of the
        // table [0x39ECE48] (stride 0x148), and 0xB2D4DB: its sweat +0x488
        // copied into the effect instance (+0xB4) every update.
        constexpr std::uintptr_t kSweatLookupRva = 0xB2D32A;
        constexpr std::uint8_t kSweatLookup[] = {
            0x44, 0x8B, 0x86, 0x9C, 0x00, 0x00, 0x00, 0x4C, 0x8B, 0x0D, 0x10, 0xFB, 0xEB, 0x02, 0x8B, 0x3D, 0x02, 0xFB,
            0xEB, 0x02, 0x45, 0x23, 0xC5, 0x44, 0x2B, 0xC7, 0x41, 0x8B, 0xC0, 0x48, 0x69, 0xC8, 0x48, 0x01, 0x00, 0x00,
            0x4A, 0x8B, 0x84, 0x09, 0x98, 0x00, 0x00, 0x00};
        constexpr std::uintptr_t kSweatCopyRva = 0xB2D4DB;
        constexpr std::uint8_t kSweatCopy[] = {0x8B, 0x87, 0x88, 0x04, 0x00, 0x00, 0x89, 0x81, 0xB4, 0x00, 0x00, 0x00};
        constexpr std::uintptr_t kGraphicsTableRva = 0x39ECE48, kGraphicsBaseRva = 0x39ECE40;
        constexpr std::uintptr_t kGraphicsStride = 0x148, kGraphicsSlot = 0x98;
        constexpr std::uintptr_t kSweatOn = 0x484, kSweatValue = 0x488;
        bool g_sweatReady = false;

        constexpr std::uintptr_t kPedCreatureHolder = 0xA8;
        constexpr std::uintptr_t kCreatureOffset = 0x48;
        constexpr std::uintptr_t kTypeIndexTable = 0x74;
        constexpr std::uint8_t kNoIndex = 0xFF;
        constexpr std::uintptr_t kComponentType = 0x10;
        constexpr std::uint8_t kShaderVarsType = 9;
        constexpr std::uintptr_t kFrameData = 0x28;
        constexpr std::uintptr_t kDofTable = 0x28;
        constexpr std::uintptr_t kDofCount = 0x54;
        constexpr std::uintptr_t kBuffer = 0x68;
        constexpr std::uintptr_t kBufferFrameData = 0x70;
        constexpr std::uintptr_t kBufferBytes = 0x9C;

        std::uintptr_t g_base = 0;
        bool g_checked = false;
        bool g_ready = false;
        std::string g_initError;
        std::uint8_t g_type = 0;

        template <typename T>
        bool Get(std::uintptr_t address, T& out)
        {
            return replay::mem::Read(address, out);
        }

        // The component of a ped (the chain of 0xB2F0D0 and 0x87CBD0) and
        // its buffer: guarded reads only.
        Read Chain(std::uintptr_t ped, std::uintptr_t& component, std::uintptr_t& buffer, std::uint16_t& bytes)
        {
            component = 0;
            buffer = 0;
            bytes = 0;
            if (!ped)
                return Read::NoEntity;
            std::uintptr_t holder = 0, creature = 0, array = 0, vtable = 0, frameData = 0;
            std::uint8_t index = kNoIndex, type = 0;
            if (!Get(ped + kPedCreatureHolder, holder))
                return Read::Fault;
            if (!holder)
                return Read::NoComponent;
            if (!Get(holder + kCreatureOffset, creature))
                return Read::Fault;
            if (!creature)
                return Read::NoComponent;
            if (!Get(creature + kTypeIndexTable + g_type, index))
                return Read::Fault;
            if (index == kNoIndex)
                return Read::NoComponent;
            if (!Get(creature, array) || !array || !Get(array + static_cast<std::uintptr_t>(index) * 8u, component))
                return Read::Fault;
            if (!component)
                return Read::NoComponent;
            if (!Get(component, vtable) || !Get(component + kComponentType, type) ||
                !Get(component + kBufferFrameData, frameData) || !Get(component + kBuffer, buffer) ||
                !Get(component + kBufferBytes, bytes))
                return Read::Fault;
            if (vtable != g_base + kVtableRva || type != kShaderVarsType || frameData != component + kFrameData ||
                bytes > kMaxBufferBytes)
                return Read::Layout;
            return Read::Ok;
        }

        // ---- the published table ------------------------------------------------
        // Two tables: the script thread fills the one not in use and flips;
        // a detour reads the active one without a lock (it never waits and
        // never skips a frame). A reader counts itself in, then checks the
        // table is still the active one (else it counts out and takes the
        // new one); the writer fills a table only once its readers left.
        struct Entry
        {
            std::uintptr_t ped;
            std::uintptr_t buffer;
            std::uint32_t first; // into values
            std::uint32_t count;
            std::uint16_t bytes;
            std::int32_t snapshot; // >= 0: a recorded horse, the snapshot slot (no values)
        };
        struct Table
        {
            Entry entries[kMaxTargets];
            std::uint32_t count = 0;
            std::pair<std::uint16_t, std::uint32_t> values[kMaxValues];
        };
        Table g_tables[2];
        // The script thread's halves of the published table (merged at each publish).
        std::vector<PlayTarget> g_playHalf;
        std::vector<RecordTarget> g_recordHalf;

        // A recorded horse's buffer as the node left it (seqlock: odd while
        // a writer copies; the reader takes it only when the count is even and
        // did not move).
        struct NodeCopy
        {
            std::atomic<std::uint32_t> seq{0};
            std::atomic<std::uint8_t> busy{0};
            std::uint16_t bytes = 0;
            std::uint8_t data[kMaxBufferBytes];
        };
        NodeCopy g_snapshots[kMaxTargets];
        std::atomic<std::uint32_t> g_active{0};
        std::atomic<std::int32_t> g_readers[2] = {};
        std::atomic<std::uint32_t> g_activeCount{0}; // entries of the active table (a fast "none")

        struct TableRead
        {
            std::uint32_t index = 0;
            TableRead()
            {
                for (;;)
                {
                    index = g_active.load(std::memory_order_acquire);
                    g_readers[index].fetch_add(1, std::memory_order_seq_cst);
                    if (g_active.load(std::memory_order_seq_cst) == index)
                        return;
                    g_readers[index].fetch_sub(1, std::memory_order_seq_cst);
                }
            }
            ~TableRead() { g_readers[index].fetch_sub(1, std::memory_order_release); }
            const Table& table() const { return g_tables[index]; }
        };

        // ---- the detours ------------------------------------------------------------
        using NodeFn = std::uintptr_t(__fastcall*)(void*);
        std::atomic<NodeFn> g_nodeOriginal{nullptr};
        void* g_nodeTarget = nullptr;
        using ReaderFn = std::uintptr_t(__fastcall*)(void*, void*, void*, void*);
        std::atomic<ReaderFn> g_readerOriginal{nullptr};
        void* g_readerTarget = nullptr;
        std::atomic<std::int32_t> g_inFlight{0};
        bool g_installed = false;

        std::atomic<std::uint64_t> g_nodeCalls{0}, g_hits{0}, g_writes{0}, g_values{0}, g_failed{0};
        std::atomic<std::uint64_t> g_engineSet{0}, g_engineLanes{0}, g_retries{0}, g_mismatch{0};
        std::atomic<std::uint64_t> g_shaderCalls{0}, g_shaderReads{0}, g_shaderLanes{0}, g_shaderMatched{0};
        std::atomic<std::uint64_t> g_watchReads{0};
        std::atomic<std::uint64_t> g_replaced{0};
        std::atomic<std::uint64_t> g_snapshotsTaken{0};
        std::atomic<std::uint64_t> g_snapshotsEmpty{0};
        std::atomic<bool> g_replace{true};
        std::atomic<std::uint32_t> g_slotsMin{0xFFFF}, g_slotsMax{0};
        std::atomic<std::uintptr_t> g_watch[kMaxTargets] = {};
        std::atomic<std::uint32_t> g_watchEnd{0};
        constexpr std::uintptr_t kEffectSlotCount = 0x88; // u16, 0xB2F0D0: cmp cx,[rbx+88h]

        void NoteEffect(void* effect)
        {
            std::uint16_t slots = 0;
            if (!effect || !Get(reinterpret_cast<std::uintptr_t>(effect) + kEffectSlotCount, slots))
                return;
            std::uint32_t v = slots;
            std::uint32_t lo = g_slotsMin.load(std::memory_order_relaxed);
            while (v < lo && !g_slotsMin.compare_exchange_weak(lo, v, std::memory_order_relaxed))
            {
            }
            std::uint32_t hi = g_slotsMax.load(std::memory_order_relaxed);
            while (v > hi && !g_slotsMax.compare_exchange_weak(hi, v, std::memory_order_relaxed))
            {
            }
        }

        struct InFlight
        {
            InFlight() { g_inFlight.fetch_add(1, std::memory_order_seq_cst); }
            ~InFlight() { g_inFlight.fetch_sub(1, std::memory_order_seq_cst); }
        };

        void WriteEntry(const Table& table, const Entry& entry, std::uint32_t nodeBytes)
        {
            std::uint32_t written = 0, failed = 0, set = 0, looked = 0;
            for (std::uint32_t n = 0; n < entry.count; ++n)
            {
                const auto& [offset, bits] = table.values[entry.first + n];
                if (static_cast<std::uint32_t>(offset) + 4u > nodeBytes)
                {
                    ++failed;
                    continue;
                }
                std::uint32_t engine = 0;
                if (replay::mem::Read(entry.buffer + offset, engine))
                {
                    ++looked;
                    set += IsSet(engine) ? 1u : 0u;
                }
                if (replay::mem::Put(entry.buffer + offset, &bits, 4))
                    ++written;
                else
                    ++failed;
            }
            g_engineSet.fetch_add(set, std::memory_order_relaxed);
            g_engineLanes.fetch_add(looked, std::memory_order_relaxed);
            if (written)
            {
                g_writes.fetch_add(1, std::memory_order_relaxed);
                g_values.fetch_add(written, std::memory_order_relaxed);
            }
            if (failed)
                g_failed.fetch_add(1, std::memory_order_relaxed);
        }

        std::uintptr_t __fastcall WriteNodeDetour(void* node)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            InFlight inFlight;
            const NodeFn original = g_nodeOriginal.load(std::memory_order_acquire);
            // The node's fields, read before the original runs (it does not
            // change them; the job may reuse the node after).
            std::uintptr_t destination = 0;
            std::uint32_t bytes = 0;
            const bool wanted = node && g_activeCount.load(std::memory_order_acquire) != 0 &&
                Get(reinterpret_cast<std::uintptr_t>(node) + kNodeDestination, destination) &&
                Get(reinterpret_cast<std::uintptr_t>(node) + kNodeBytes, bytes);
            g_nodeCalls.fetch_add(1, std::memory_order_relaxed);
            if (!wanted || !destination)
                return original ? original(node) : 0;
            const TableRead read;
            const Table& table = read.table();
            for (std::uint32_t index = 0; index < table.count; ++index)
            {
                const Entry& entry = table.entries[index];
                if (entry.buffer != destination)
                    continue;
                if (entry.snapshot >= 0)
                {
                    // A recorded horse: the engine's copy, then the buffer as
                    // the shader will read it (the script thread's own read can
                    // land in the job's 0xFF fill).
                    const std::uintptr_t result = original ? original(node) : 0;
                    NodeCopy& snap = g_snapshots[entry.snapshot];
                    // Several write nodes reach this buffer in one update, and
                    // some copy nothing (the 0xFF fill only: clip 1276 had half
                    // its copies empty). The shader reads the buffer after all
                    // of them, so an empty pass never replaces the values in
                    // the slot: only a copy with a value in it is kept.
                    thread_local std::uint8_t copy[kMaxBufferBytes];
                    if (bytes == entry.bytes && bytes <= kMaxBufferBytes && replay::mem::Copy(copy, destination, bytes))
                    {
                        bool any = false;
                        for (std::uint32_t at = 0; at + 4 <= bytes && !any; at += 4)
                        {
                            std::uint32_t v = 0;
                            std::memcpy(&v, copy + at, 4);
                            any = v != kNotSet;
                        }
                        if (!any)
                            g_snapshotsEmpty.fetch_add(1, std::memory_order_relaxed);
                        else if (!snap.busy.exchange(1, std::memory_order_acquire))
                        {
                            snap.seq.fetch_add(1, std::memory_order_acq_rel); // odd
                            std::memcpy(snap.data, copy, bytes);
                            snap.bytes = static_cast<std::uint16_t>(bytes);
                            snap.seq.fetch_add(1, std::memory_order_release); // even
                            snap.busy.store(0, std::memory_order_release);
                            g_snapshotsTaken.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    return result;
                }
                // The same buffer and size as published: a freed component's
                // address reused by another frame is left alone.
                if (bytes != entry.bytes)
                {
                    g_mismatch.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                g_hits.fetch_add(1, std::memory_order_relaxed);
                if (g_replace.load(std::memory_order_relaxed))
                {
                    // Replace: the engine's copy (0xFF, then the puppet's rest
                    // values) never reaches the buffer, so a reader running
                    // beside the job (the render of the previous frame) only
                    // ever sees recorded values. The original only copies
                    // into this buffer and returns 1.
                    g_replaced.fetch_add(1, std::memory_order_relaxed);
                    WriteEntry(table, entry, bytes);
                    return 1;
                }
                const std::uintptr_t result = original ? original(node) : 0;
                WriteEntry(table, entry, bytes);
                return result;
            }
            return original ? original(node) : 0;
        }

        // Observer: what the ped shader effect reads for a published puppet.
        std::uintptr_t __fastcall ReaderDetour(void* effect, void* ped, void* r8, void* r9)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            InFlight inFlight;
            const ReaderFn original = g_readerOriginal.load(std::memory_order_acquire);
            g_shaderCalls.fetch_add(1, std::memory_order_relaxed);
            const std::uintptr_t key = reinterpret_cast<std::uintptr_t>(ped);
            const std::uint32_t watchEnd = g_watchEnd.load(std::memory_order_acquire);
            for (std::uint32_t index = 0; key && index < watchEnd && index < kMaxTargets; ++index)
                if (g_watch[index].load(std::memory_order_relaxed) == key)
                {
                    g_watchReads.fetch_add(1, std::memory_order_relaxed);
                    NoteEffect(effect);
                    break;
                }
            if (key && g_activeCount.load(std::memory_order_acquire) != 0)
            {
                const TableRead read;
                const Table& table = read.table();
                for (std::uint32_t index = 0; index < table.count; ++index)
                {
                    const Entry& entry = table.entries[index];
                    if (entry.ped != key || entry.snapshot >= 0)
                        continue;
                    std::uintptr_t component = 0, buffer = 0;
                    std::uint16_t bytes = 0;
                    if (Chain(key, component, buffer, bytes) != Read::Ok || buffer != entry.buffer)
                        break;
                    std::uint32_t lanes = 0, matched = 0;
                    for (std::uint32_t n = 0; n < entry.count; ++n)
                    {
                        const auto& [offset, bits] = table.values[entry.first + n];
                        std::uint32_t now = 0;
                        if (static_cast<std::uint32_t>(offset) + 4u > bytes || !replay::mem::Read(buffer + offset, now))
                            continue;
                        ++lanes;
                        matched += now == bits ? 1u : 0u;
                    }
                    g_shaderReads.fetch_add(1, std::memory_order_relaxed);
                    NoteEffect(effect);
                    g_shaderLanes.fetch_add(lanes, std::memory_order_relaxed);
                    g_shaderMatched.fetch_add(matched, std::memory_order_relaxed);
                    break;
                }
            }
            return original ? original(effect, ped, r8, r9) : 0;
        }

        bool Hook(void* target, void* detour, void** original, std::string& error)
        {
            void* trampoline = nullptr;
            MH_STATUS status = MH_CreateHook(target, detour, &trampoline);
            if (status != MH_OK || !trampoline)
            {
                error = std::string("mh_create_") + MH_StatusToString(status);
                return false;
            }
            *original = trampoline;
            status = replay::hooks::Enable(target);
            if (status != MH_OK)
            {
                MH_RemoveHook(target);
                *original = nullptr;
                error = std::string("mh_enable_") + MH_StatusToString(status);
                return false;
            }
            return true;
        }
    }

    const char* ReadText(Read r)
    {
        switch (r)
        {
        case Read::Ok: return "ok";
        case Read::Off: return "off";
        case Read::NoEntity: return "no_entity";
        case Read::NoComponent: return "no_component";
        case Read::Layout: return "layout";
        case Read::Fault: return "fault";
        default: return "?";
        }
    }

    bool Init(std::string& error)
    {
        if (g_checked)
        {
            error = g_initError;
            return g_ready;
        }
        g_checked = true;
        g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        struct Check
        {
            std::uintptr_t rva;
            const std::uint8_t* bytes;
            std::size_t size;
            const char* name;
        };
        const Check checks[] = {
            {kReaderCreatureRva, kReaderCreature, sizeof(kReaderCreature), "reader_creature_signature_mismatch"},
            {kReaderFindRva, kReaderFind, sizeof(kReaderFind), "reader_find_signature_mismatch"},
            {kReaderBufferRva, kReaderBuffer, sizeof(kReaderBuffer), "reader_buffer_signature_mismatch"},
            {kFindRva, kFind, sizeof(kFind), "find_signature_mismatch"},
            {kCtorRva, kCtor, sizeof(kCtor), "ctor_signature_mismatch"},
            {kHasDofRva, kHasDof, sizeof(kHasDof), "has_dof_signature_mismatch"},
            {kQueueSiteRva, kQueueSite, sizeof(kQueueSite), "queue_site_signature_mismatch"},
            {kQueueDestRva, kQueueDest, sizeof(kQueueDest), "queue_dest_signature_mismatch"},
            {kQueueBytesRva, kQueueBytes, sizeof(kQueueBytes), "queue_bytes_signature_mismatch"},
        };
        for (const Check& check : checks)
        {
            if (!replay::mem::Matches(reinterpret_cast<const void*>(g_base + check.rva), check.bytes, check.size))
            {
                g_initError = check.name;
                error = g_initError;
                return false;
            }
        }
        g_sweatReady = replay::mem::Matches(reinterpret_cast<const void*>(g_base + kSweatLookupRva), kSweatLookup, sizeof(kSweatLookup)) &&
            replay::mem::Matches(reinterpret_cast<const void*>(g_base + kSweatCopyRva), kSweatCopy, sizeof(kSweatCopy));
        // The type byte is set at the engine's start (0x265A448); 0 is "not yet".
        if (!Get(g_base + kTypeByteRva, g_type) || g_type == 0 || g_type >= 0x80)
        {
            g_checked = false; // try again later
            g_initError = "type_byte_not_ready";
            error = g_initError;
            return false;
        }
        g_ready = true;
        g_initError.clear();
        error.clear();
        return true;
    }

    bool Ready() { return g_ready; }

    Read ComponentOf(std::uintptr_t ped, std::uintptr_t& component)
    {
        component = 0;
        if (!g_ready)
            return Read::Off;
        std::uintptr_t buffer = 0;
        std::uint16_t bytes = 0;
        return Chain(ped, component, buffer, bytes);
    }

    Read SweatOf(std::uintptr_t ped, float& value, std::uint8_t& on)
    {
        value = 0.0f;
        on = 0;
        if (!g_ready || !g_sweatReady)
            return Read::Off;
        if (!ped)
            return Read::NoEntity;
        std::uint32_t handle = 0, base = 0;
        std::uintptr_t table = 0, entry = 0;
        if (!Get(ped + 0x9C, handle) || !Get(g_base + kGraphicsBaseRva, base) || !Get(g_base + kGraphicsTableRva, table) || !table)
            return Read::Fault;
        const std::uint32_t row = (handle & 0x1FFFFu) - base;
        if ((handle & 0x1FFFFu) < base || row >= 0x10000u)
            return Read::Layout;
        if (!Get(table + static_cast<std::uintptr_t>(row) * kGraphicsStride + kGraphicsSlot, entry))
            return Read::Fault;
        entry &= ~static_cast<std::uintptr_t>(1);
        if (!entry)
            return Read::NoComponent;
        if (!Get(entry + kSweatValue, value) || !Get(entry + kSweatOn, on))
            return Read::Fault;
        return Read::Ok;
    }

    bool SweatReady() { return g_sweatReady; }

    Read BufferOf(std::uintptr_t component, std::uintptr_t& buffer, std::uint16_t& bytes)
    {
        buffer = 0;
        bytes = 0;
        if (!g_ready)
            return Read::Off;
        if (!Get(component + kBuffer, buffer) || !Get(component + kBufferBytes, bytes))
            return Read::Fault;
        return buffer && bytes && bytes <= kMaxBufferBytes ? Read::Ok : Read::Layout;
    }

    Read LayoutOf(std::uintptr_t component, Layout& out, ParseResult& parse)
    {
        out = Layout{};
        parse = ParseResult::Empty;
        if (!g_ready)
            return Read::Off;
        std::uintptr_t table = 0, frameData = 0;
        std::uint32_t count = 0;
        std::uint16_t bytes = 0;
        if (!Get(component + kDofTable, table) || !Get(component + kDofCount, count) ||
            !Get(component + kBufferFrameData, frameData) || !Get(component + kBufferBytes, bytes))
            return Read::Fault;
        if (frameData != component + kFrameData || !table || count == 0 || count > kMaxDofs || bytes > kMaxBufferBytes)
            return Read::Layout;
        static std::vector<std::uint8_t> rows;
        rows.resize(static_cast<std::size_t>(count) * kDofBytes);
        if (!replay::mem::Copy(rows.data(), table, rows.size()))
            return Read::Fault;
        parse = ParseLayout(rows.data(), count, bytes, out);
        return parse == ParseResult::Ok ? Read::Ok : Read::Layout;
    }

    Read ValuesOf(std::uintptr_t component, const Layout& layout, std::vector<std::uint32_t>& bits)
    {
        bits.clear();
        std::uintptr_t buffer = 0;
        std::uint16_t bytes = 0;
        const Read r = BufferOf(component, buffer, bytes);
        if (r != Read::Ok)
            return r;
        static std::vector<std::uint8_t> copy;
        copy.resize(bytes);
        if (!replay::mem::Copy(copy.data(), buffer, bytes))
            return Read::Fault;
        return Gather(layout, copy.data(), copy.size(), bits) ? Read::Ok : Read::Layout;
    }

    bool Install(std::string& error, std::string& observerError)
    {
        observerError.clear();
        if (g_installed)
            return true;
        if (!Init(error))
            return false;
        void* nodeTarget = reinterpret_cast<void*>(g_base + kWriteNodeRva);
        if (!replay::mem::Matches(nodeTarget, kWriteNode, sizeof(kWriteNode)))
        {
            error = "write_node_signature_mismatch";
            return false;
        }
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        {
            error = std::string("mh_initialize_") + MH_StatusToString(init);
            return false;
        }
        void* original = nullptr;
        if (!Hook(nodeTarget, reinterpret_cast<void*>(&WriteNodeDetour), &original, error))
            return false;
        g_nodeOriginal.store(reinterpret_cast<NodeFn>(original), std::memory_order_release);
        g_nodeTarget = nodeTarget;
        g_installed = true;
        // The observer of the shader effect's reader (evidence only).
        void* readerTarget = reinterpret_cast<void*>(g_base + kReaderRva);
        if (!replay::mem::Matches(readerTarget, kReader, sizeof(kReader)))
            observerError = "reader_signature_mismatch";
        else if (Hook(readerTarget, reinterpret_cast<void*>(&ReaderDetour), &original, observerError))
        {
            g_readerOriginal.store(reinterpret_cast<ReaderFn>(original), std::memory_order_release);
            g_readerTarget = readerTarget;
        }
        error.clear();
        return true;
    }

    void Shutdown()
    {
        g_activeCount.store(0, std::memory_order_release);
        for (void** target : {&g_nodeTarget, &g_readerTarget})
        {
            if (*target)
            {
                MH_DisableHook(*target);
                MH_RemoveHook(*target);
                *target = nullptr;
            }
        }
        g_installed = false;
        for (int wait = 0; wait < 200 && g_inFlight.load(std::memory_order_seq_cst) > 0; ++wait)
            Sleep(1);
        g_nodeOriginal.store(nullptr, std::memory_order_release);
        g_readerOriginal.store(nullptr, std::memory_order_release);
    }

    bool Installed() { return g_installed; }
    bool ObserverInstalled() { return g_readerTarget != nullptr; }

    namespace
    {
        void Publish()
        {
            const std::uint32_t next = g_active.load(std::memory_order_relaxed) ^ 1u;
            // The readers of the table to fill are gone in a few microseconds.
            for (int spin = 0; g_readers[next].load(std::memory_order_seq_cst) != 0; ++spin)
            {
                g_retries.fetch_add(1, std::memory_order_relaxed);
                if (spin > 1000)
                    Sleep(0);
            }
            Table& table = g_tables[next];
            std::uint32_t count = 0, used = 0;
            for (const PlayTarget& target : g_playHalf)
            {
                if (count >= kMaxTargets)
                    break;
                if (!target.ped || !target.buffer || target.values.empty() || used + target.values.size() > kMaxValues)
                    continue;
                Entry& entry = table.entries[count++];
                entry.ped = target.ped;
                entry.buffer = target.buffer;
                entry.bytes = target.bytes;
                entry.first = used;
                entry.count = static_cast<std::uint32_t>(target.values.size());
                entry.snapshot = -1;
                for (const auto& value : target.values)
                    table.values[used++] = value;
            }
            for (const RecordTarget& target : g_recordHalf)
            {
                if (count >= kMaxTargets)
                    break;
                if (!target.buffer || target.slot >= kMaxTargets)
                    continue;
                Entry& entry = table.entries[count++];
                entry.ped = target.ped;
                entry.buffer = target.buffer;
                entry.bytes = target.bytes;
                entry.first = 0;
                entry.count = 0;
                entry.snapshot = static_cast<std::int32_t>(target.slot);
            }
            table.count = count;
            g_active.store(next, std::memory_order_seq_cst);
            g_activeCount.store(count, std::memory_order_release);
        }
    }

    void SetPlayTargets(const std::vector<PlayTarget>& targets)
    {
        g_playHalf = targets;
        Publish();
    }

    void SetRecordTargets(const std::vector<RecordTarget>& targets)
    {
        g_recordHalf = targets;
        Publish();
    }

    bool TakeSnapshot(std::uint32_t slot, std::uint32_t& seq, std::vector<std::uint8_t>& out)
    {
        if (slot >= kMaxTargets)
            return false;
        NodeCopy& snap = g_snapshots[slot];
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            const std::uint32_t before = snap.seq.load(std::memory_order_acquire);
            if (before & 1u)
                continue;
            if (before == seq)
                return false; // nothing new
            const std::uint16_t bytes = snap.bytes;
            out.assign(snap.data, snap.data + (bytes <= kMaxBufferBytes ? bytes : 0));
            std::atomic_thread_fence(std::memory_order_acquire);
            if (snap.seq.load(std::memory_order_acquire) == before)
            {
                seq = before;
                return !out.empty();
            }
        }
        return false;
    }

    void SetReplace(bool replace) { g_replace.store(replace, std::memory_order_relaxed); }

    void SetWatch(const std::vector<std::uintptr_t>& peds)
    {
        const std::uint32_t oldEnd = g_watchEnd.load(std::memory_order_relaxed);
        std::uint32_t count = 0;
        for (const std::uintptr_t ped : peds)
            if (ped && count < kMaxTargets)
                g_watch[count++].store(ped, std::memory_order_relaxed);
        for (std::uint32_t index = count; index < oldEnd && index < kMaxTargets; ++index)
            g_watch[index].store(0, std::memory_order_relaxed);
        g_watchEnd.store(count, std::memory_order_release);
    }

    Read BindingsText(std::uintptr_t component, std::string& text, std::uint32_t& count)
    {
        text.clear();
        count = 0;
        if (!g_ready)
            return Read::Off;
        std::uintptr_t rows = 0;
        std::uint16_t n = 0;
        if (!Get(component + 0x18, rows) || !Get(component + 0x20, n))
            return Read::Fault;
        count = n;
        if (!rows || !n)
            return Read::Ok;
        const std::uint32_t shown = (std::min)(static_cast<std::uint32_t>(n), 32u);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(shown) * 16u);
        if (!replay::mem::Copy(bytes.data(), rows, bytes.size()))
            return Read::Fault;
        char item[96];
        for (std::uint32_t k = 0; k < shown; ++k)
        {
            const std::uint8_t* r = bytes.data() + k * 16u;
            std::uint32_t hash = 0;
            std::uint16_t id = 0;
            std::memcpy(&hash, r, 4);
            std::memcpy(&id, r + 10, 2);
            std::snprintf(item, sizeof(item), "%s%08x:%u:%d:%u:%x:%u:%u", text.empty() ? "" : "|", hash, r[4],
                static_cast<int>(static_cast<std::int8_t>(r[5])), r[7], r[12], id, r[13]);
            text += item;
        }
        return Read::Ok;
    }

    Stats Snapshot()
    {
        Stats s;
        s.nodeCalls = g_nodeCalls.load(std::memory_order_relaxed);
        s.hits = g_hits.load(std::memory_order_relaxed);
        s.writes = g_writes.load(std::memory_order_relaxed);
        s.values = g_values.load(std::memory_order_relaxed);
        s.failed = g_failed.load(std::memory_order_relaxed);
        s.engineSet = g_engineSet.load(std::memory_order_relaxed);
        s.engineLanes = g_engineLanes.load(std::memory_order_relaxed);
        s.retries = g_retries.load(std::memory_order_relaxed);
        s.mismatch = g_mismatch.load(std::memory_order_relaxed);
        s.shaderCalls = g_shaderCalls.load(std::memory_order_relaxed);
        s.shaderReads = g_shaderReads.load(std::memory_order_relaxed);
        s.shaderLanes = g_shaderLanes.load(std::memory_order_relaxed);
        s.shaderMatched = g_shaderMatched.load(std::memory_order_relaxed);
        s.watchReads = g_watchReads.load(std::memory_order_relaxed);
        s.replaced = g_replaced.load(std::memory_order_relaxed);
        s.snapshots = g_snapshotsTaken.load(std::memory_order_relaxed);
        s.snapshotsEmpty = g_snapshotsEmpty.load(std::memory_order_relaxed);
        s.effectSlotsMin = g_slotsMin.load(std::memory_order_relaxed);
        s.effectSlotsMax = g_slotsMax.load(std::memory_order_relaxed);
        return s;
    }
}
