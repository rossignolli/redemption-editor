#include "ped_gore.h"
#include "mod_clock.h"

#include "profiler.h"

#include <windows.h>

#include <MinHook.h>
#include "hook_batch.h"
#include "module_lifetime.h"
#include "event_ring.h"

#include <algorithm>
#include <atomic>

#include <cstdio>
#include <cstring>

namespace replay::gore
{
    namespace
    {
        constexpr std::uintptr_t kApplyRva = 0xB5203C;
        constexpr std::uint8_t kApplySignature[] = {
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x44, 0x88, 0x48, 0x20, 0x89, 0x48, 0x08, 0x55, 0x56,
            0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41};
        constexpr std::uint32_t kContextHash = 0xE4DF46D5;
        using ApplyFn = bool(__fastcall*)(int, void*, std::uint32_t*, std::uint8_t);

        bool GuardedCompare(const void* target, const std::uint8_t* signature, std::size_t size)
        {
            __try
            {
                return std::memcmp(target, signature, size) == 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // The context 0xEAC808 builds on its stack: the ped, zeros, the hash
        // at +0x40 (0x50 bytes with room to spare).
        struct alignas(16) Context
        {
            void* ped = nullptr;
            std::uint64_t zero8 = 0;
            std::uint8_t zeros[0x30] = {};
            std::uint32_t hash = kContextHash;
            std::uint32_t zero44 = 0;
            std::uint16_t zero48 = 0;
            std::uint8_t zero4a = 0;
            std::uint8_t pad[0x35] = {};
        };

        bool GuardedApply(ApplyFn fn, int limb, void* ped, bool& result, std::uint32_t& code)
        {
            __try
            {
                Context context;
                context.ped = ped;
                std::uint32_t hash = kContextHash;
                result = fn(limb, &context, &hash, 1);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    bool ApplyLimb(void* ped, int limb, std::string& error)
    {
        if (!ped)
        {
            error = "no_ped";
            return false;
        }
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        if (!GuardedCompare(base + kApplyRva, kApplySignature, sizeof(kApplySignature)))
        {
            error = "gore_apply_signature_mismatch";
            return false;
        }
        bool result = false;
        std::uint32_t code = 0;
        if (!GuardedApply(reinterpret_cast<ApplyFn>(base + kApplyRva), limb, ped, result, code))
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        if (!result)
            error = "engine_refused";
        return result;
    }
}

namespace replay::gore
{
    namespace
    {
        constexpr std::uintptr_t kHandlerRva = 0xB4EB3C;
        constexpr std::uint8_t kHandlerSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
            0x48, 0x8D, 0x6C, 0x24, 0xD0, 0x48, 0x81, 0xEC, 0x30, 0x01, 0x00, 0x00};
        using HandlerFn = bool(__fastcall*)(void*);

        constexpr std::uint32_t kRingSize = 256;
        using Slot = replay::ring::Slot<EventCall>; // event_ring.h
        Slot g_ring[kRingSize];
        std::atomic<std::uint64_t> g_next{0};
        std::atomic<HandlerFn> g_original{nullptr};
        std::atomic<bool> g_observing{false};
        std::atomic<std::int32_t> g_inFlight{0};
        void* g_target = nullptr;
        std::uintptr_t g_base = 0;
        std::uintptr_t g_imageEnd = 0;
        LARGE_INTEGER g_frequency{};
        thread_local bool t_replaying = false;

        std::uint64_t QpcUs()
        {
            return replay::clock::NowUs(); // mod_clock.h: no 21-day overflow
        }

        bool GuardedRead(std::uint8_t* out, const void* source, std::size_t size)
        {
            __try
            {
                std::memcpy(out, source, size);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool __fastcall HandlerDetour(void* event)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.gore_event");
            struct InFlight
            {
                InFlight() { g_inFlight.fetch_add(1, std::memory_order_seq_cst); }
                ~InFlight() { g_inFlight.fetch_sub(1, std::memory_order_seq_cst); }
            } inFlight;
            // Copy before: the handler may change the event it was given.
            alignas(16) std::uint8_t before[kEventBytes] = {};
            const bool copied = event && GuardedRead(before, event, kEventBytes);
            const HandlerFn original = g_original.load(std::memory_order_acquire);
            const bool result = original ? original(event) : false;
            if (!g_observing.load(std::memory_order_relaxed) || t_replaying || !copied)
                return result;
            void* frames[1] = {};
            const USHORT captured = RtlCaptureStackBackTrace(1, 1, frames, nullptr);
            replay::ring::Writer<EventCall> slot(g_ring, kRingSize, g_next);
            EventCall& call = slot.value;
            call.timeUs = QpcUs();
            std::memcpy(call.context, before, kEventBytes);
            call.result = result ? 1 : 0;
            const std::uintptr_t address = captured ? reinterpret_cast<std::uintptr_t>(frames[0]) : 0;
            call.caller = address >= g_base && address < g_imageEnd ? static_cast<std::uint32_t>(address - g_base) : 0;
            slot.Publish();
            return result;
        }

        bool GuardedHandler(HandlerFn fn, std::uint8_t* event, bool& result, std::uint32_t& code)
        {
            __try
            {
                result = fn(event);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    bool InstallObserver(std::string& error)
    {
        if (g_target)
            return true;
        QueryPerformanceFrequency(&g_frequency);
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        g_base = reinterpret_cast<std::uintptr_t>(base);
        g_imageEnd = g_base + nt->OptionalHeader.SizeOfImage;
        void* target = base + kHandlerRva;
        if (!GuardedCompare(target, kHandlerSignature, sizeof(kHandlerSignature)))
        {
            error = "gore_handler_signature_mismatch";
            return false;
        }
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        {
            error = std::string("mh_initialize_") + MH_StatusToString(init);
            return false;
        }
        void* trampoline = nullptr;
        MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&HandlerDetour), &trampoline);
        if (status != MH_OK || !trampoline)
        {
            error = std::string("mh_create_") + MH_StatusToString(status);
            return false;
        }
        g_original.store(reinterpret_cast<HandlerFn>(trampoline), std::memory_order_release);
        status = replay::hooks::Enable(target);
        if (status != MH_OK)
        {
            MH_RemoveHook(target);
            g_original.store(nullptr, std::memory_order_release);
            error = std::string("mh_enable_") + MH_StatusToString(status);
            return false;
        }
        g_target = target;
        return true;
    }

    void SetObserving(bool on)
    {
        g_observing.store(on, std::memory_order_relaxed);
    }

    std::uint64_t NowUs()
    {
        return replay::clock::NowUs(); // mod_clock.h: no 21-day overflow

    }

    std::uint64_t TotalEvents()
    {
        return g_next.load(std::memory_order_acquire);
    }

    void CollectEventsSince(std::uint64_t& cursor, std::vector<EventCall>& out)
    {
        out.clear();
        replay::ring::ForEachSince(g_ring, kRingSize, g_next, cursor, [&](const EventCall& call) { out.push_back(call); });
    }

    bool ReplayEvent(const std::uint8_t* context, void* ped, void* inflictor, bool& handled, std::string& error)
    {
        handled = false;
        if (!ped || !context)
        {
            error = "no_ped";
            return false;
        }
        // With the observer installed the entry holds the hook jump, so the
        // replay calls the trampoline (the original); otherwise the entry,
        // after its signature check.
        HandlerFn fn = g_original.load(std::memory_order_acquire);
        if (!fn)
        {
            auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
            if (!GuardedCompare(base + kHandlerRva, kHandlerSignature, sizeof(kHandlerSignature)))
            {
                error = "gore_handler_signature_mismatch";
                return false;
            }
            fn = reinterpret_cast<HandlerFn>(base + kHandlerRva);
        }
        alignas(16) std::uint8_t event[0x60] = {};
        std::memcpy(event, context, kEventBytes);
        std::memcpy(event + 0x00, &ped, sizeof(ped));
        std::memcpy(event + 0x08, &inflictor, sizeof(inflictor));
        std::uint32_t code = 0;
        t_replaying = true;
        const bool ok = GuardedHandler(fn, event, handled, code);
        t_replaying = false;
        if (!ok)
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        return true;
    }

    bool FireEvent(void* ped, void* inflictor, std::uint16_t mask, bool& handled, std::string& error)
    {
        handled = false;
        if (!ped)
        {
            error = "no_ped";
            return false;
        }
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        if (!g_target && !GuardedCompare(base + kHandlerRva, kHandlerSignature, sizeof(kHandlerSignature)))
        {
            error = "gore_handler_signature_mismatch";
            return false;
        }
        const HandlerFn fn = reinterpret_cast<HandlerFn>(base + kHandlerRva);
        alignas(16) std::uint8_t event[0x60] = {};
        std::memcpy(event + 0x00, &ped, sizeof(ped));
        std::memcpy(event + 0x08, &inflictor, sizeof(inflictor));
        const std::uint32_t hash = kContextHash;
        std::memcpy(event + 0x40, &hash, sizeof(hash));
        std::memcpy(event + 0x48, &mask, sizeof(mask));
        std::uint32_t code = 0;
        if (!GuardedHandler(fn, event, handled, code))
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        return true;
    }

    void Shutdown()
    {
        g_observing.store(false, std::memory_order_relaxed);
        if (g_target)
        {
            MH_DisableHook(g_target);
            MH_RemoveHook(g_target);
            g_target = nullptr;
        }
        for (int wait = 0; wait < 200 && g_inFlight.load(std::memory_order_seq_cst) > 0; ++wait)
            Sleep(1);
        g_original.store(nullptr, std::memory_order_release);
    }
}

namespace replay::gore
{
    namespace
    {
        bool GuardedReadMask(std::uintptr_t base, const void* ped, std::uint32_t& mask, bool& found)
        {
            __try
            {
                const std::uintptr_t table = *reinterpret_cast<const std::uintptr_t*>(base + 0x497CB80);
                const std::uint16_t count = *reinterpret_cast<const std::uint16_t*>(base + 0x497CB88);
                if (!table || count > 4096)
                    return true;
                for (std::uint32_t n = 0; n < count; ++n)
                {
                    const auto* entry = reinterpret_cast<const std::uint8_t*>(table + n * 0x20);
                    if (*reinterpret_cast<const void* const*>(entry) != ped)
                        continue;
                    mask = *reinterpret_cast<const std::uint32_t*>(entry + 0x18) & 0x7FFF;
                    found = true;
                    break;
                }
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    bool ReadGoreMask(const void* ped, std::uint32_t& mask)
    {
        mask = 0;
        if (!ped)
            return false;
        bool found = false;
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        return GuardedReadMask(base, ped, mask, found) && found;
    }
}
