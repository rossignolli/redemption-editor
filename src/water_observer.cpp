#include "water_observer.h"
#include "guarded_memory.h"
#include "mod_clock.h"

#include <windows.h>

#include <MinHook.h>
#include "hook_batch.h"
#include "event_ring.h"
#include "module_lifetime.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

namespace replay::water
{
    namespace
    {
        constexpr std::uintptr_t kManagerPointerRva = 0x3F0D968;

        // Signatures (build 1491.50): the prologue, and for each
        // method the call into the first simulation's adder, whose target is
        // checked too (+0x120 and +0x130 have the same bytes and differ only
        // there).
        constexpr std::uintptr_t kDescRva = 0x58ADD8;
        constexpr std::uint8_t kDescSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0xFA,
            0x48, 0x81, 0xC1, 0xF0, 0x00, 0x04, 0x00, 0xE8};
        constexpr std::size_t kDescCallAt = 0x17;         // e8 at 0x58ADEF
        constexpr std::uintptr_t kDescAdderRva = 0x6CA38C;

        constexpr std::uintptr_t kBoxRva = 0x58AE24;
        constexpr std::uint8_t kBoxSignature[] = {
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48,
            0x89, 0x78, 0x20, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x60, 0x48, 0x8B, 0xBC, 0x24, 0x98, 0x00, 0x00,
            0x00, 0x48, 0x8B, 0xF1, 0x48, 0x8B, 0x9C, 0x24, 0x90, 0x00, 0x00, 0x00, 0x48, 0x81, 0xC1, 0xF0,
            0x00, 0x04, 0x00};
        constexpr std::size_t kBoxCallAt = 0x62;          // e8 at 0x58AE86
        constexpr std::uintptr_t kBoxAdderRva = 0x6CA634;

        // 0x58AF3C and 0x58B110: identical up to the adder call.
        constexpr std::uintptr_t kFloats120Rva = 0x58AF3C;
        constexpr std::uintptr_t kFloats130Rva = 0x58B110;
        constexpr std::uint8_t kFloatsSignature[] = {
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x57, 0x48, 0x81, 0xEC, 0xD0,
            0x00, 0x00, 0x00, 0x48, 0x8B, 0x9C, 0x24, 0x18, 0x01, 0x00, 0x00, 0x48, 0x8B, 0xF1, 0x48, 0x8B,
            0xBC, 0x24, 0x20, 0x01, 0x00, 0x00, 0x48, 0x81, 0xC1, 0xF0, 0x00, 0x04, 0x00};
        constexpr std::size_t kFloatsCallAt = 0xA9;       // e8 at 0x58AFE5 / 0x58B1B9
        constexpr std::uintptr_t kFloats120AdderRva = 0x6CA7F0;
        constexpr std::uintptr_t kFloats130AdderRva = 0x6CA98C;

        using DescFn = void(__fastcall*)(void* self, const void* desc);
        using BoxFn = void(__fastcall*)(void* self, const void* a, const void* b, float f, const void* v0, const void* v1);
        using FloatsFn = void(__fastcall*)(void* self, float f1, float f2, float f3, float f4, float f5, float f6,
            const void* v0, const void* v1);

        constexpr std::uint64_t kRing = 512;
        using RingSlot = replay::ring::Slot<ShapeCall>; // event_ring.h
        RingSlot g_ring[kRing];
        std::atomic<std::uint64_t> g_next{0};
        std::atomic<DescFn> g_desc{nullptr};
        std::atomic<BoxFn> g_box{nullptr};
        std::atomic<FloatsFn> g_floats120{nullptr};
        std::atomic<FloatsFn> g_floats130{nullptr};
        std::vector<void*> g_targets;
        std::atomic<int> g_inFlight{0};
        std::uintptr_t g_base = 0;
        std::size_t g_imageSize = 0;
        LARGE_INTEGER g_frequency{};

        // Script thread only (Drain).
        std::uint64_t g_cursor = 0;
        std::uint32_t g_lines = 0;
        std::uint64_t g_windowStartUs = 0;
        std::uint64_t g_lost = 0;
        std::map<std::pair<std::uint8_t, std::uint32_t>, std::uint32_t> g_counts; // (slot, caller) -> calls this second
        std::vector<ShapeCall> g_batch;

        std::uint64_t NowUs()
        {
            return replay::clock::NowUs(); // mod_clock.h (no 21-day overflow)
        }

        bool GuardedCompare(const void* target, const std::uint8_t* signature, std::size_t size)
        {
            return replay::mem::Matches(target, signature, size); // guarded_memory.h
        }

        bool GuardedCopy(void* out, const void* source, std::size_t size)
        {
            return replay::mem::Copy(out, source, size); // guarded_memory.h
        }

        // The prologue matches and the e8 at `callAt` lands on `adderRva`.
        bool Matches(std::uintptr_t rva, const std::uint8_t* signature, std::size_t size, std::size_t callAt,
            std::uintptr_t adderRva)
        {
            const auto* target = reinterpret_cast<const std::uint8_t*>(g_base + rva);
            if (!GuardedCompare(target, signature, size))
                return false;
            std::uint8_t opcode = 0;
            std::int32_t displacement = 0;
            if (!GuardedCopy(&opcode, target + callAt, 1) || opcode != 0xE8 ||
                !GuardedCopy(&displacement, target + callAt + 1, 4))
                return false;
            return rva + callAt + 5 + static_cast<std::intptr_t>(displacement) == adderRva;
        }

        std::uint32_t ToRva(void* address)
        {
            const auto value = reinterpret_cast<std::uintptr_t>(address);
            return value >= g_base && value < g_base + g_imageSize ? static_cast<std::uint32_t>(value - g_base) : 0;
        }

        // Fills the common fields of a claimed ring slot (event_ring.h);
        // the caller copies the arguments and publishes.
        void Begin(replay::ring::Writer<ShapeCall>& ring, Slot slot, void* self)
        {
            ShapeCall& call = ring.value;
            call = ShapeCall{};
            call.timeUs = NowUs();
            call.slot = slot;
            call.threadId = GetCurrentThreadId();
            void* frames[4] = {};
            // Frame 0 is this detour; 1 is the return into the caller (or a
            // static shortcut such as 0x58AEE8, hence two more).
            const USHORT captured = RtlCaptureStackBackTrace(1, 3, frames, nullptr);
            for (USHORT n = 0; n < captured && n < 3; ++n)
                call.callers[n] = ToRva(frames[n]);
            std::uintptr_t manager = 0;
            call.managerMatches = GuardedCopy(&manager, reinterpret_cast<const void*>(g_base + kManagerPointerRva), 8) &&
                manager == reinterpret_cast<std::uintptr_t>(self);
        }

        void __fastcall DescDetour(void* self, const void* desc)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            g_inFlight.fetch_add(1, std::memory_order_seq_cst);
            const DescFn original = g_desc.load(std::memory_order_acquire);
            if (original)
                original(self, desc);
            replay::ring::Writer<ShapeCall> ring(g_ring, kRing, g_next);
            Begin(ring, Slot::Desc118, self);
            ring.value.copied = desc && GuardedCopy(ring.value.data, desc, sizeof(ring.value.data));
            ring.Publish();
            g_inFlight.fetch_sub(1, std::memory_order_seq_cst);
        }

        void __fastcall BoxDetour(void* self, const void* a, const void* b, float f, const void* v0, const void* v1)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            g_inFlight.fetch_add(1, std::memory_order_seq_cst);
            const BoxFn original = g_box.load(std::memory_order_acquire);
            if (original)
                original(self, a, b, f, v0, v1);
            replay::ring::Writer<ShapeCall> ring(g_ring, kRing, g_next);
            Begin(ring, Slot::Box128, self);
            ShapeCall& call = ring.value;
            call.scalars[0] = f;
            call.copied = a && b && v0 && v1 && GuardedCopy(call.data, a, 0x20) && GuardedCopy(call.data + 0x20, b, 0x40) &&
                GuardedCopy(call.vecA, v0, sizeof(call.vecA)) && GuardedCopy(call.vecB, v1, sizeof(call.vecB));
            ring.Publish();
            g_inFlight.fetch_sub(1, std::memory_order_seq_cst);
        }

        void RecordFloats(Slot slot, void* self, const float (&values)[6], const void* v0, const void* v1)
        {
            replay::ring::Writer<ShapeCall> ring(g_ring, kRing, g_next);
            Begin(ring, slot, self);
            ShapeCall& call = ring.value;
            std::memcpy(call.scalars, values, sizeof(call.scalars));
            call.copied = v0 && v1 && GuardedCopy(call.vecA, v0, sizeof(call.vecA)) && GuardedCopy(call.vecB, v1, sizeof(call.vecB));
            ring.Publish();
        }

        void __fastcall Floats120Detour(void* self, float f1, float f2, float f3, float f4, float f5, float f6,
            const void* v0, const void* v1)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            g_inFlight.fetch_add(1, std::memory_order_seq_cst);
            const FloatsFn original = g_floats120.load(std::memory_order_acquire);
            if (original)
                original(self, f1, f2, f3, f4, f5, f6, v0, v1);
            RecordFloats(Slot::Floats120, self, {f1, f2, f3, f4, f5, f6}, v0, v1);
            g_inFlight.fetch_sub(1, std::memory_order_seq_cst);
        }

        void __fastcall Floats130Detour(void* self, float f1, float f2, float f3, float f4, float f5, float f6,
            const void* v0, const void* v1)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            g_inFlight.fetch_add(1, std::memory_order_seq_cst);
            const FloatsFn original = g_floats130.load(std::memory_order_acquire);
            if (original)
                original(self, f1, f2, f3, f4, f5, f6, v0, v1);
            RecordFloats(Slot::Floats130, self, {f1, f2, f3, f4, f5, f6}, v0, v1);
            g_inFlight.fetch_sub(1, std::memory_order_seq_cst);
        }

        template <typename Fn>
        bool Hook(std::uintptr_t rva, void* detour, std::atomic<Fn>& original, std::string& error)
        {
            void* target = reinterpret_cast<void*>(g_base + rva);
            void* trampoline = nullptr;
            MH_STATUS status = MH_CreateHook(target, detour, &trampoline);
            if (status != MH_OK || !trampoline)
            {
                error = std::string("mh_create_") + MH_StatusToString(status);
                return false;
            }
            original.store(reinterpret_cast<Fn>(trampoline), std::memory_order_release);
            status = replay::hooks::Enable(target);
            if (status != MH_OK)
            {
                MH_RemoveHook(target);
                original.store(nullptr, std::memory_order_release);
                error = std::string("mh_enable_") + MH_StatusToString(status);
                return false;
            }
            g_targets.push_back(target);
            return true;
        }

        const char* SlotName(std::uint8_t slot)
        {
            switch (slot)
            {
            case 0: return "0x118";
            case 1: return "0x120";
            case 2: return "0x128";
            default: return "0x130";
            }
        }

        void LogCall(std::ostream& log, const ShapeCall& call, const char* phase)
        {
            log << "WATER_SHAPE,phase=" << phase << ",slot=" << SlotName(static_cast<std::uint8_t>(call.slot))
                << std::hex << ",caller=0x" << call.callers[0] << ",caller2=0x" << call.callers[1]
                << ",caller3=0x" << call.callers[2] << std::dec << ",thread=" << call.threadId
                << ",manager=" << (call.managerMatches ? 1 : 0) << ",copied=" << (call.copied ? 1 : 0);
            float words[8] = {};
            switch (call.slot)
            {
            case Slot::Desc118:
            {
                std::uint32_t type = 0;
                std::memcpy(&type, call.data, 4);
                std::memcpy(words, call.data + 0x10, sizeof(words));
                log << ",type=" << type << ",f10=";
                break;
            }
            case Slot::Box128:
                std::memcpy(words, call.data, sizeof(words));
                log << ",f=" << call.scalars[0] << ",a=";
                break;
            default:
                std::memcpy(words, call.scalars, sizeof(call.scalars));
                log << ",f=";
                break;
            }
            const int count = call.slot == Slot::Floats120 || call.slot == Slot::Floats130 ? 6 : 8;
            for (int n = 0; n < count; ++n)
                log << (n ? ";" : "") << words[n];
            log << ",v0=" << call.vecA[0] << ';' << call.vecA[1] << ';' << call.vecA[2] << ';' << call.vecA[3]
                << ",v1=" << call.vecB[0] << ';' << call.vecB[1] << ';' << call.vecB[2] << ';' << call.vecB[3] << '\n';
        }
    }

    bool Install(std::string& error)
    {
        if (!g_targets.empty())
            return true;
        const HMODULE module = GetModuleHandleW(nullptr);
        g_base = reinterpret_cast<std::uintptr_t>(module);
        {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + static_cast<std::uintptr_t>(dos->e_lfanew));
            g_imageSize = nt->OptionalHeader.SizeOfImage;
        }
        if (!Matches(kDescRva, kDescSignature, sizeof(kDescSignature), kDescCallAt, kDescAdderRva))
        {
            error = "water_desc_signature_mismatch";
            return false;
        }
        if (!Matches(kBoxRva, kBoxSignature, sizeof(kBoxSignature), kBoxCallAt, kBoxAdderRva))
        {
            error = "water_box_signature_mismatch";
            return false;
        }
        if (!Matches(kFloats120Rva, kFloatsSignature, sizeof(kFloatsSignature), kFloatsCallAt, kFloats120AdderRva) ||
            !Matches(kFloats130Rva, kFloatsSignature, sizeof(kFloatsSignature), kFloatsCallAt, kFloats130AdderRva))
        {
            error = "water_floats_signature_mismatch";
            return false;
        }
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        {
            error = std::string("mh_initialize_") + MH_StatusToString(init);
            return false;
        }
        const bool ok = Hook(kDescRva, reinterpret_cast<void*>(&DescDetour), g_desc, error) &&
            Hook(kBoxRva, reinterpret_cast<void*>(&BoxDetour), g_box, error) &&
            Hook(kFloats120Rva, reinterpret_cast<void*>(&Floats120Detour), g_floats120, error) &&
            Hook(kFloats130Rva, reinterpret_cast<void*>(&Floats130Detour), g_floats130, error);
        if (!ok)
            Shutdown();
        return ok;
    }

    bool Installed() { return !g_targets.empty(); }

    std::uint64_t Total() { return g_next.load(std::memory_order_acquire); }

    void Drain(std::ostream& log, const char* phase)
    {
        if (g_targets.empty())
            return;
        constexpr std::uint32_t kMaxLines = 256;
        const std::uint64_t end = g_next.load(std::memory_order_acquire);
        std::uint64_t first = end > kRing ? end - kRing : 0;
        if (g_cursor < first)
            g_lost += first - g_cursor;
        g_batch.clear();
        replay::ring::ForEachSince(g_ring, kRing, g_next, g_cursor, [](const ShapeCall& call) { g_batch.push_back(call); });
        for (const ShapeCall& call : g_batch)
        {
            if (g_lines < kMaxLines)
            {
                ++g_lines;
                LogCall(log, call, phase);
            }
            ++g_counts[{static_cast<std::uint8_t>(call.slot), call.callers[0]}];
        }
        const std::uint64_t now = NowUs();
        if (!g_windowStartUs)
            g_windowStartUs = now;
        if (now - g_windowStartUs < 1000000)
            return;
        g_windowStartUs = now;
        if (g_counts.empty() && !g_lost)
            return;
        log << "WATER_SHAPE_HISTOGRAM,phase=" << phase << ",total=" << end << ",lost=" << g_lost;
        for (const auto& [key, count] : g_counts)
            log << ',' << SlotName(key.first) << '@' << std::hex << "0x" << key.second << std::dec << '=' << count;
        log << '\n';
        g_counts.clear();
        g_lost = 0;
    }

    void Shutdown()
    {
        for (void* target : g_targets)
        {
            MH_DisableHook(target);
            MH_RemoveHook(target);
        }
        g_targets.clear();
        for (int wait = 0; wait < 200 && g_inFlight.load(std::memory_order_seq_cst) > 0; ++wait)
            Sleep(1);
        g_desc.store(nullptr, std::memory_order_release);
        g_box.store(nullptr, std::memory_order_release);
        g_floats120.store(nullptr, std::memory_order_release);
        g_floats130.store(nullptr, std::memory_order_release);
    }
}
