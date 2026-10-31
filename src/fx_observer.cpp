#include <intrin.h> // _ReturnAddress for the read probe
#include "fx_observer.h"
#include "entity_events.h" // ComposeFxWorld
#include "fx_motion_rule.h" // the hoof and registry restart sites
#include "engine_calls.h"
#include "game_sig.h"
#include "guarded_memory.h"
#include "mod_clock.h"
#include "profiler.h"
#include "script_fx_loop.h" // the script looped sites
#include "shot_fx_audit_core.h" // the impact and body hit sites
#include "soundfx_probe.h"
#include "weapon_fx_intent_core.h" // the wrapper played back

#include <windows.h>

#include <MinHook.h>
#include "hook_batch.h"
#include "module_lifetime.h"
#include "event_ring.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace replay::fx
{
    namespace
    {
        namespace sig = replay::sig;

        // (const u32* effectHash, const u32* assetHash) -> effect instance or
        // null. Reached by the script trigger path and, through a one-argument
        // wrapper, by 71 call sites of the vfx code.
        const sig::Signature kFxTrigger{"fx_trigger",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 8B FA 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 50 01 00 00",
            0x2C3A57C};

        using TriggerFn = void*(__fastcall*)(const std::uint32_t*, const std::uint32_t*);

        // The fx start, called only by the particle native path (after the
        // name is hashed and the rotation turned into a matrix): (effect*,
        // asset*, entity, bone, matrix*, zoom, flags).
        const sig::Signature kFxStart{"fx_start",
            "4C 8B DC 49 89 5B 18 49 89 73 20 57 48 81 EC 80 00 00 00 8B 02 41 8B F1 41 89 43 08 49 8D 53 08 8B 01 49 "
            "8B F8 49 8D 4B 10 41 89 43 10 E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 0F 84 ?? ?? ?? ?? 48 8B 08 83 79 50 FF 75 "
            "1A 83 B8 44 03 00 00 02 75 0A 33 D2 48 8B C8 E8 ?? ?? ?? ?? 32 C0 E9 ?? ?? ?? ?? 48 8B CB 48 85 FF 74 1A "
            "C6 44 24 28 00 45 33 C9 44 8B C6 C6 44 24 20 00 48 8B D7 E8 ?? ?? ?? ?? EB 3D 0F 28 05 ?? ?? ?? ?? 4C 8D "
            "44 24 40 0F 57 C9 0F 29 44 24 40 0F 14 C8 48 8D 54 24 30 0F 57 D2 0F 29 4C 24 50 0F 14 CA 0F 29 4C 24 60 "
            "0F 29 54 24 70 E8 ?? ?? ?? ?? C6 83 D0 05 00 00 02 48 8B 94 24 B0 00 00 00 48 8B CB E8 ?? ?? ?? ?? 8A 84 "
            "24 C0 00 00 00 48 8B CB 88 83 BA 03 00 00 E8 ?? ?? ?? ?? F3 0F 10 84 24 B8 00 00 00",
            0x2C50C88};
        std::uintptr_t g_start = 0;

        // The weapon muzzle path (the muzzle core, weapon_fx_intent_core.h),
        // reached from its own calls: the triggered instance wrapper (effect*)
        // -> inst (its bytes alone are not unique), the attach wrapper (inst,
        // entity, bone, 0, u8, u8) and the offset setter (inst+280h = *vec).
        const sig::Signature kFxTriggeredInst{"fx_triggered_inst", weaponfx::kWeaponMuzzleCall, sig::Rel32{1, 5},
            "48 83 EC 28 8B 01 48 8D 54 24 30 83 64 24 30 00 48 8D 4C 24 38 89 44 24 38 E8 ?? ?? ?? ?? 48 83 C4 28 C3",
            0x2C3A624};
        const sig::Signature kFxAttach{"fx_attach", weaponfx::kWeaponMuzzleCore, sig::Rel32{0x402, 0x406},
            "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 40 33 C0 41 8B F9 41 8B F0 48 8B DA 48 8B E9 48 "
            "85 D2 74 0E 48 8B 4A 20 48 85 C9 74 05 E8 ?? ?? ?? ?? 8A 4C 24 78 41 83 C9 FF 88 4C 24 38 44 8B C6 8A 4C "
            "24 70",
            0x2C2FD40};
        const sig::Signature kFxSetOffsetPos{"fx_set_offset_pos", weaponfx::kWeaponMuzzleCore, sig::Rel32{0x472, 0x476},
            "F3 0F 6F 02 0F 11 81 80 02 00 00 C3", 0x29E42A0};
        // The attach core every attach path ends in (the wrapper above calls
        // it with r9d = -1): observed for the entity and bone of an instance.
        const sig::Signature kFxAttachCore{"fx_attach_core",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 20 55 41 56 41 57 48 8B EC 48 83 EC 70 48 8B F1 45 8B F0",
            0x2C2FB60};
        using AttachCoreFn = void(__fastcall*)(void*, void*, int, int, std::uint64_t, int, std::uint8_t, std::uint8_t);
        // (inst, matrix*): inst+250h..28Fh, reached from the bullet impact
        // core's call (its bytes alone are not unique).
        const sig::Signature kFxSetMatrix{"fx_set_matrix", shotfx::kShotImpactCore, sig::Rel32{0x431, 0x435},
            "0F 10 02 0F 11 81 50 02 00 00 0F 10 4A 10 0F 11 89 60 02 00 00 0F 10 42 20 0F 11 81 70 02 00 00 0F 10 "
            "4A 30 0F 11 89 80 02 00 00 C3",
            0x29E4274};
        // (inst, out vec*, matrix*): inst+210h..24Fh.
        const sig::Signature kFxSetParent{"fx_set_parent",
            "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 20 4C 8B F1 49 8B E8 48 8B 0D ?? "
            "?? ?? ?? 48 8B F2",
            0x29E2754};
        // Registered effect get-or-create core (owner, id, arg3, flag, hash*,
        // asset*, created out*, u8, u8) -> inst; key = owner + id.
        const sig::Signature kFxRegistered{"fx_registered",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 41 57 48 8B EC 48 83 EC 40 48 8B 45 70 4C 8B E9 4C 63 E2 45 8A "
            "F9 48 8D 55 E0 4C 89 45 E8 49 8B F0 C6 00 00 4E 8D 34 21 48 8D 0D ?? ?? ?? ?? 4C 89 75 E0 E8 ?? ?? ?? ?? "
            "48 8D 48 10 48 F7 D8 48 1B DB 48 23 D9 74 05 48 8B 1B EB 02 33 DB 48 8B 7D 60",
            0x2C49B08};
        // Its reads of the stack arguments (engine_calls.h): a build that
        // changed them fails here.
        const sig::Signature kFxRegisteredArg8{"fx_registered_arg8", kFxRegistered, sig::Offset{0x137},
            "40 8A 7D 78", 0x2C49C3F};
        const sig::Signature kFxRegisteredArg6{"fx_registered_arg6", kFxRegistered, sig::Offset{0x13F},
            "48 8B 45 68", 0x2C49C47};
        const sig::Signature kFxRegisteredArg9{"fx_registered_arg9", kFxRegistered, sig::Offset{0x1AB},
            "8A 85 80 00 00 00", 0x2C49CB3};
        const sig::Signature* const kFxRegisteredArgs[] = {&kFxRegisteredArg8, &kFxRegisteredArg6, &kFxRegisteredArg9};
        using RegisteredFn = void*(__fastcall*)(void*, int, void*, std::uint8_t, std::uint32_t*, std::uint32_t*,
            std::uint8_t*, std::uint8_t, std::uint8_t);
        // (inst, list*: count at +0x300, planes at +0x20), and the quality
        // global it reads (a pointer; byte +1 = quality level).
        const sig::Signature kFxAddPlanes{"fx_add_planes",
            "48 8B 05 ?? ?? ?? ?? 44 0F B6 48 01 49 8D 41 03 4C 69 C0 10 03 00 00 41 83 24 08 00 41 B8 10 00 00 00 8B "
            "82 00 03 00 00",
            0x29E41E0};
        const sig::Signature kFxQuality{"fx_quality", kFxAddPlanes, sig::Rel32{3, 7}, "", 0x4F892F0};
        // (inst, vec*): +0x350, when its global switch allows it.
        const sig::Signature kFxSetAux350{"fx_set_aux350",
            "80 3D ?? ?? ?? ?? 00 75 0B F3 0F 6F 02 0F 11 81 50 03 00 00 C3", 0x2C4F508};
        // (inst)
        const sig::Signature kFxInstStart{"fx_inst_start",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 20 55 41 56 41 57 48 8B EC 48 83 EC 50 48 8B D9 0F 29 70 D8 48 "
            "8B 0D ?? ?? ?? ??",
            0x29E7644};
        // The instance initializer: its stores are the layout the player
        // writes into an instance of ours before its start. The first site
        // holds +0x310 (a tracer's start), +0x350, +0x380 (the colour float4,
        // its alpha at +0x38C), +0x394 (zoom), +0x39C, +0x3A0, +0x3A8 and the
        // word +0x3BC; the second +0x3C4 (far clip), +0x3E0 (the evolutions),
        // +0x580 (flags), +0x584 and +0x5CC.
        const sig::Signature kFxInstInit{"fx_inst_init",
            "48 89 5C 24 08 48 89 6C 24 10 56 57 41 54 41 56 41 57 48 83 EC 50 48 89 11 45 33 F6 0F 28 0D ?? ?? ?? ??",
            0x29D6008};
        const sig::Signature kFxInitSettings{"fx_inst_init_settings", kFxInstInit, sig::Offset{0xF8},
            "0F 11 B1 10 03 00 00 4C 89 B1 20 03 00 00 4C 89 B1 28 03 00 00 48 89 A9 30 03 00 00 4C 89 B1 38 03 00 00 "
            "66 44 89 B1 40 03 00 00 44 89 B1 44 03 00 00 0F 11 B1 50 03 00 00 0F 11 B1 60 03 00 00 0F 11 B1 70 03 00 "
            "00 0F 28 05 ?? ?? ?? ?? 0F 11 81 80 03 00 00 89 A9 90 03 00 00 89 A9 94 03 00 00 89 A9 98 03 00 00 89 A9 "
            "9C 03 00 00 89 A9 A0 03 00 00 89 A9 A4 03 00 00 C7 81 A8 03 00 00 00 00 20 41 44 88 A1 B0 03 00 00 44 88 "
            "A1 B1 03 00 00 44 88 A1 B2 03 00 00 44 88 A1 B3 03 00 00 44 88 A1 B4 03 00 00 44 88 A1 B5 03 00 00 44 89 "
            "B1 B6 03 00 00 44 88 B1 BA 03 00 00 66 44 89 B9 BC 03 00 00",
            0x29D6100};
        const sig::Signature kFxInitFlags{"fx_inst_init_flags", kFxInstInit, sig::Offset{0x201},
            "89 87 C0 03 00 00 89 87 C4 03 00 00 89 87 C8 03 00 00 33 C0 0F 11 B7 D0 03 00 00 44 88 B7 BB 03 00 00 48 "
            "89 87 E0 03 00 00 66 89 87 E8 03 00 00 48 89 8F F0 03 00 00 E8 ?? ?? ?? ?? 89 9F 80 05 00 00 48 8D 8F 27 "
            "06 00 00 44 89 B7 84 05 00 00 48 8D 87 80 0C 00 00 4C 89 B7 88 05 00 00 48 8D 97 30 09 00 00 4C 89 B7 90 "
            "05 00 00 45 8D 46 02 4C 89 B7 98 05 00 00 44 89 B7 CC 05 00 00",
            0x29D6209};
        constexpr std::size_t kInstFlags = 0x580;
        constexpr std::size_t kInstZoom = 0x394;

        using InstStartFn = void(__fastcall*)(void*);
        constexpr std::uint32_t kMuzzleStartRing = 256;
        using MuzzleStartSlot = replay::ring::Slot<MuzzleStart>; // event_ring.h
        MuzzleStartSlot g_muzzleStarts[kMuzzleStartRing];
        std::atomic<std::uint64_t> g_nextMuzzleStart{0};

        // Start observer: the muzzle instance to snapshot at its start.
        std::atomic<void*> g_watchInst{nullptr};
        std::atomic<std::uint32_t> g_watchHash{0};
        std::atomic<InstStartFn> g_originalStart{nullptr};
        void* g_startTarget = nullptr;
        std::uint8_t g_gameplaySnapshot[kSnapshotBytes];
        std::atomic<bool> g_gameplaySnapshotReady{false};
        std::uint8_t g_replaySnapshot[kSnapshotBytes];
        std::atomic<bool> g_replaySnapshotReady{false};

        bool GuardedRead(std::uint8_t* out, const void* source, std::size_t size)
        {
            return replay::mem::Copy(out, source, size); // guarded_memory.h
        }

        bool GuardedSnapshot(std::uint8_t* out, const void* inst)
        {
            __try
            {
                std::memcpy(out, inst, kSnapshotBytes);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        std::uintptr_t g_triggeredInst = 0;
        std::uintptr_t g_attach = 0;
        std::uintptr_t g_setOffsetPos = 0;
        std::uintptr_t g_instStart = 0;
        std::uintptr_t g_setMatrix = 0;
        std::uintptr_t g_setParent = 0;
        std::uintptr_t g_addPlanes = 0;
        std::uintptr_t g_setAux350 = 0;

        constexpr std::uint32_t kRingSize = 8192;

        using Slot = replay::ring::Slot<TriggerCall>;

        Slot g_ring[kRingSize];
        std::atomic<std::uint64_t> g_next{0};

        // Attach calls (inst, entity, bone) and instance starts.
        constexpr std::uint32_t kAttachRing = 512;
        struct AttachRecord
        {
            std::uint64_t timeUs = 0;
            std::uint32_t threadId = 0;
            std::uint64_t inst = 0;
            std::uint64_t entity = 0;
            std::int32_t bone = -1;
        };
        using AttachSlot = replay::ring::Slot<AttachRecord>;
        AttachSlot g_attaches[kAttachRing];
        std::atomic<std::uint64_t> g_nextAttach{0};
        std::atomic<AttachCoreFn> g_originalAttach{nullptr};
        void* g_attachTarget = nullptr;
        constexpr std::uint32_t kStartRing = 4096;
        using StartSlot = replay::ring::Slot<FxStart>;
        StartSlot g_starts[kStartRing];
        std::atomic<std::uint64_t> g_nextStart{0};
        constexpr std::uint32_t kRegisteredRing = 8192;
        using RegisteredSlot = replay::ring::Slot<RegisteredCall>;
        RegisteredSlot g_registered[kRegisteredRing];
        std::atomic<std::uint64_t> g_nextRegistered{0};
        std::atomic<RegisteredFn> g_originalRegistered{nullptr};
        void* g_registeredTarget = nullptr;
        bool g_registeredWanted = true; // the bench can switch it off (fx_registered off, research)
        std::atomic<TriggerFn> g_original{nullptr};
        std::atomic<bool> g_enabled{false};
        // Detour calls in progress; unload waits for them after unhooking.
        std::atomic<std::int32_t> g_inFlight{0};
        void* g_target = nullptr;
        bool g_installed = false;
        std::uintptr_t g_base = 0;
        std::uintptr_t g_imageEnd = 0;
        LARGE_INTEGER g_frequency{};
        // The foot vfx site's return address (its call + 5), set at install, 0
        // when the game has not the call: the trigger detour compares with it.
        std::uintptr_t g_footVfxReturn = 0;

        // A recorded site: the return address of `call` (an E8 call, 5 bytes)
        // as an RVA of this exe, kNoSite when the game has not the call.
        std::uint32_t SiteOf(const sig::Signature& call)
        {
            const std::uintptr_t at = sig::Find(call);
            const std::uintptr_t base = sig::ImageBase();
            return at && base && at > base ? static_cast<std::uint32_t>(at + 5 - base) : kNoSite;
        }

        struct RecordedSites
        {
            std::uint32_t muzzle = SiteOf(weaponfx::kWeaponMuzzleCall);
            std::uint32_t impact = SiteOf(shotfx::kShotImpactCall);
            std::uint32_t foot = SiteOf(kFxFootVfxCall);
            std::uint32_t cameraAmbient = SiteOf(kFxCameraAmbientCall);
            std::uint32_t body = SiteOf(shotfx::kShotBodyCall);
            std::uint32_t shell = SiteOf(weaponfx::kWeaponShellCall);
            std::uint32_t scriptStart = SiteOf(scriptfx::kScriptFxStartCall);
            std::uint32_t scriptKeepAlive = SiteOf(scriptfx::kScriptFxKeepAliveCall);
            std::uint32_t hoof = SiteOf(fxmotion::kFxHoofCall);
            std::uint32_t registryRestart = SiteOf(fxmotion::kFxRegistryRestartCall);
        };

        // Resolved once, on the first use (Install makes it, before any detour
        // reads it).
        const RecordedSites& Sites()
        {
            static const RecordedSites sites;
            return sites;
        }

        std::uint64_t QpcUs()
        {
            return replay::clock::NowUs(); // mod_clock.h (no 21-day overflow)
        }

        std::uint32_t ReadHash(const std::uint32_t* hash)
        {
            __try
            {
                return hash ? *hash : 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        // See SetEngineFootVfxSuppressed.
        std::atomic<bool> g_suppressFootVfx{false};
        std::atomic<std::uint64_t> g_suppressedFootVfx{0};

        void* __fastcall TriggerDetour(const std::uint32_t* effectHash, const std::uint32_t* assetHash)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.fx_trigger");
            struct InFlight
            {
                InFlight() { g_inFlight.fetch_add(1, std::memory_order_seq_cst); }
                ~InFlight() { g_inFlight.fetch_sub(1, std::memory_order_seq_cst); }
            } inFlight;
            // Read the arguments before the original: it rewrites them.
            const std::uint32_t effect = ReadHash(effectHash);
            const std::uint32_t asset = ReadHash(assetHash);
            void* frames[kCallerDepth + 1] = {};
            USHORT captured = 0;
            bool refused = false;
            if (g_suppressFootVfx.load(std::memory_order_relaxed))
            {
                // frames[0] returns into the one-argument wrapper, frames[1]
                // into its caller (the site).
                captured = RtlCaptureStackBackTrace(1, kCallerDepth + 1, frames, nullptr);
                refused = captured >= 2 && g_footVfxReturn &&
                    reinterpret_cast<std::uintptr_t>(frames[1]) == g_footVfxReturn;
                if (refused)
                    g_suppressedFootVfx.fetch_add(1, std::memory_order_relaxed);
            }
            const TriggerFn original = g_original.load(std::memory_order_acquire);
            void* result = refused ? nullptr : original ? original(effectHash, assetHash) : nullptr;
            if (!g_enabled.load(std::memory_order_relaxed))
                return result;

            if (!captured)
                captured = RtlCaptureStackBackTrace(1, kCallerDepth + 1, frames, nullptr);
            replay::ring::Writer<TriggerCall> slot(g_ring, kRingSize, g_next);
            TriggerCall& call = slot.value;
            call.timeUs = QpcUs();
            call.effectHash = effect;
            call.assetHash = asset;
            call.instance = result != nullptr;
            call.threadId = GetCurrentThreadId();
            for (std::uint32_t depth = 0; depth < kCallerDepth; ++depth)
            {
                const auto address = depth < captured
                    ? reinterpret_cast<std::uintptr_t>(frames[depth])
                    : 0;
                call.callers[depth] = address >= g_base && address < g_imageEnd
                    ? static_cast<std::uint32_t>(address - g_base)
                    : 0;
            }
            call.inst = reinterpret_cast<std::uint64_t>(result);
            slot.Publish();
            const std::uint32_t watchHash = g_watchHash.load(std::memory_order_relaxed);
            if (result && (watchHash ? effect == watchHash : call.callers[1] == Sites().muzzle))
                g_watchInst.store(result, std::memory_order_release);
            return result;
        }

        void __fastcall AttachDetour(void* inst, void* entity, int bone, int a4, std::uint64_t a5, int a6,
            std::uint8_t a7, std::uint8_t a8)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            struct InFlight
            {
                InFlight() { g_inFlight.fetch_add(1, std::memory_order_seq_cst); }
                ~InFlight() { g_inFlight.fetch_sub(1, std::memory_order_seq_cst); }
            } inFlight;
            if (g_enabled.load(std::memory_order_relaxed) && inst)
            {
                replay::ring::Writer<AttachRecord> slot(g_attaches, kAttachRing, g_nextAttach);
                slot.value.timeUs = QpcUs();
                slot.value.threadId = GetCurrentThreadId();
                slot.value.inst = reinterpret_cast<std::uint64_t>(inst);
                slot.value.entity = reinterpret_cast<std::uint64_t>(entity);
                slot.value.bone = bone;
                slot.Publish();
            }
            const AttachCoreFn original = g_originalAttach.load(std::memory_order_acquire);
            if (original)
                original(inst, entity, bone, a4, a5, a6, a7, a8);
        }

        // The owners whose engine registered effects are
        // refused (puppet clones), written by the script thread, read by
        // the detour on any thread. A stale entry for a moment is harmless.
        constexpr std::size_t kMaxSuppressedOwners = 256;
        std::atomic<std::uint64_t> g_suppressedOwners[kMaxSuppressedOwners] = {};
        std::atomic<std::uint32_t> g_suppressedCount{0};
        std::atomic<std::uint64_t> g_suppressedCalls{0};
        // Set while the replay itself calls the registered core.
        thread_local bool t_replayRegistered = false;

        bool IsSuppressedOwner(const void* owner)
        {
            const std::uint32_t count = g_suppressedCount.load(std::memory_order_acquire);
            if (!count || !owner)
                return false;
            const std::uint64_t value = reinterpret_cast<std::uint64_t>(owner);
            for (std::uint32_t n = 0; n < count && n < kMaxSuppressedOwners; ++n)
                if (g_suppressedOwners[n].load(std::memory_order_relaxed) == value)
                    return true;
            return false;
        }

        void GuardedWriteByte(std::uint8_t* target, std::uint8_t value)
        {
            __try
            {
                if (target)
                    *target = value;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }

        void* __fastcall RegisteredDetour(void* owner, int id, void* arg3, std::uint8_t flag, std::uint32_t* hash,
            std::uint32_t* asset, std::uint8_t* createdOut, std::uint8_t a8, std::uint8_t a9)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            struct InFlight
            {
                InFlight() { g_inFlight.fetch_add(1, std::memory_order_seq_cst); }
                ~InFlight() { g_inFlight.fetch_sub(1, std::memory_order_seq_cst); }
            } inFlight;
            // With
            // SoundFxProbe=1 this detour and the engine's part are timed.
            const std::uint64_t probeStart = replay::soundfx::Active() ? replay::soundfx::Ticks() : 0;
            std::uint64_t probeEngine = 0;
            const bool suppressed = !t_replayRegistered && IsSuppressedOwner(owner);
            void* result = nullptr;
            if (suppressed)
            {
                g_suppressedCalls.fetch_add(1, std::memory_order_relaxed);
                GuardedWriteByte(createdOut, 0);
            }
            else
            {
                const RegisteredFn original = g_originalRegistered.load(std::memory_order_acquire);
                const std::uint64_t engineStart = probeStart ? replay::soundfx::Ticks() : 0;
                result = original ? original(owner, id, arg3, flag, hash, asset, createdOut, a8, a9) : nullptr;
                if (probeStart)
                    probeEngine = replay::soundfx::Ticks() - engineStart;
            }
            if (g_enabled.load(std::memory_order_relaxed))
            {
                void* frames[3] = {};
                const USHORT captured = RtlCaptureStackBackTrace(1, 3, frames, nullptr);
                // frames[0] returns into the registered wrapper; its caller is the site.
                const std::uintptr_t address = captured > 2 ? reinterpret_cast<std::uintptr_t>(frames[2]) : 0;
                replay::ring::Writer<RegisteredCall> slot(g_registered, kRegisteredRing, g_nextRegistered);
                RegisteredCall& call = slot.value;
                call.timeUs = QpcUs();
                call.owner = reinterpret_cast<std::uint64_t>(owner);
                call.id = id;
                call.arg3 = reinterpret_cast<std::uint64_t>(arg3);
                call.effectHash = ReadHash(hash);
                call.assetHash = ReadHash(asset);
                call.inst = reinterpret_cast<std::uint64_t>(result);
                std::uint8_t created = 0;
                GuardedRead(&created, createdOut, 1);
                call.created = created;
                if (created && result && call.effectHash && call.effectHash == g_watchHash.load(std::memory_order_relaxed))
                    g_watchInst.store(result, std::memory_order_release);
                call.flag = flag;
                call.arg8 = a8;
                call.arg9 = a9;
                call.caller = address >= g_base && address < g_imageEnd ? static_cast<std::uint32_t>(address - g_base) : 0;
                call.threadId = GetCurrentThreadId();
                call.byReplay = t_replayRegistered;
                call.suppressed = suppressed;
                slot.Publish();
            }
            if (probeStart)
                replay::soundfx::NoteRegistered(probeStart, probeEngine, ReadHash(hash), result, _ReturnAddress());
            return result;
        }

        // The replay's playback tick (thread id while inside, 0 outside) and
        // the one-shot owner rule.
        std::atomic<DWORD> g_replayScopeThread{0};
        std::atomic<std::int32_t> g_replayScopeDepth{0};
        std::atomic<bool> g_oneShotOwnerRule{false};
        std::atomic<bool> g_weaponSitesExempt{true}; // see SetWeaponSitesExempt
        std::atomic<std::uintptr_t> g_killFn{0}; // the fx stop
        std::atomic<std::uint64_t> g_oneShotKilled{0};
        constexpr std::size_t kMaxExempt = 64;
        std::atomic<std::uint32_t> g_exemptHashes[kMaxExempt] = {};
        std::atomic<std::uint32_t> g_exemptCount{0};

        bool InReplayScope()
        {
            const DWORD thread = g_replayScopeThread.load(std::memory_order_acquire);
            return t_replayRegistered || (thread && thread == GetCurrentThreadId());
        }

        constexpr std::size_t kMaxWorldSites = 64;
        std::atomic<std::uint32_t> g_worldSites[kMaxWorldSites] = {};
        std::atomic<std::uint32_t> g_worldSiteCount{0};

        bool WorldSite(std::uint32_t site)
        {
            const std::uint32_t count = g_worldSiteCount.load(std::memory_order_acquire);
            for (std::uint32_t n = 0; n < count && n < kMaxWorldSites; ++n)
                if (g_worldSites[n].load(std::memory_order_relaxed) == site)
                    return true;
            return false;
        }

        bool ExemptHash(std::uint32_t hash)
        {
            const std::uint32_t count = g_exemptCount.load(std::memory_order_acquire);
            for (std::uint32_t n = 0; n < count && n < kMaxExempt; ++n)
                if (g_exemptHashes[n].load(std::memory_order_relaxed) == hash)
                    return true;
            return false;
        }

        // An engine one-shot on a puppet: killed after its start.
        bool OwnerRuleKills(const FxStart& start)
        {
            if (!g_oneShotOwnerRule.load(std::memory_order_relaxed) || start.byReplay || !start.effectHash)
                return false;
            if ((start.callerSite == Sites().muzzle || start.callerSite == Sites().impact) &&
                g_weaponSitesExempt.load(std::memory_order_relaxed))
                return false;
            if (ExemptHash(start.effectHash))
                return false;
            if (start.callerSite && WorldSite(start.callerSite))
                return true;
            return start.entity && IsSuppressedOwner(reinterpret_cast<const void*>(start.entity));
        }

        bool GuardedKill(std::uintptr_t kill, void* inst)
        {
            return engine::Call(engine::kFxStop, kill, inst, 1);
        }

        // The settings fields of an instance
        // (entity_events.h FxSettings), guarded reads; mask = the fields read.
        void ReadSettingsRaw(const std::uint8_t* base, entity::FxSettings& out)
        {
            out = entity::FxSettings{};
            std::uint8_t block[0x70] = {}; // +0x350 .. +0x3BF
            if (GuardedRead(block, base + 0x350, sizeof(block)))
            {
                std::memcpy(out.vel350, block + 0x00, 16);
                std::memcpy(out.tint380, block + 0x30, 12);
                std::memcpy(&out.alpha38C, block + 0x3C, 4);
                std::memcpy(&out.aux39C, block + 0x4C, 4);
                std::memcpy(&out.lod3A0, block + 0x50, 4);
                std::memcpy(&out.tele3A8, block + 0x58, 4);
                std::memcpy(&out.cull3BC, block + 0x6C, 2);
                out.mask |= entity::FxSettings::kApplied;
                entity::FxSettingsKeepFinite(out);
            }
            std::uint8_t ev[4] = {};
            if (GuardedRead(ev, base + 0x5CC, 4))
            {
                std::memcpy(&out.ev5CC, ev, 4);
                out.mask |= entity::FxSettings::kEv5CC;
            }
        }

        // The instance as the engine finished setting it up, plus the
        // trigger call that returned it and the attach call on it.
        void RecordStart(void* inst, bool& kill)
        {
            kill = false;
            FxStart start;
            start.timeUs = QpcUs();
            start.inst = reinterpret_cast<std::uint64_t>(inst);
            start.threadId = GetCurrentThreadId();
            std::uint8_t* base = static_cast<std::uint8_t*>(inst);
            if (!GuardedRead(reinterpret_cast<std::uint8_t*>(start.matrix), base + kInstMatrix, sizeof(start.matrix)))
                return;
            GuardedRead(reinterpret_cast<std::uint8_t*>(start.parent), base + kInstParentMatrix, sizeof(start.parent));
            std::uint8_t bytes[4] = {};
            if (GuardedRead(bytes, base + kInstZoom, 4))
                std::memcpy(&start.zoom, bytes, 4);
            if (GuardedRead(bytes, base + kInstFlags, 4))
                std::memcpy(&start.flags, bytes, 4);
            if (GuardedRead(bytes, base + kInstMode, 1))
                start.mode = bytes[0];
            GuardedRead(start.evolutions, base + kInstEvolutions, kEvolutionBytes);
            ReadSettingsRaw(base, start.settings);
            {
                void* frames[kCallerDepth + 1] = {};
                const USHORT captured = RtlCaptureStackBackTrace(1, kCallerDepth + 1, frames, nullptr);
                for (std::uint32_t depth = 0; depth < kCallerDepth; ++depth)
                {
                    const std::uintptr_t address = depth < captured
                        ? reinterpret_cast<std::uintptr_t>(frames[depth])
                        : 0;
                    start.callers[depth] = address >= g_base && address < g_imageEnd
                        ? static_cast<std::uint32_t>(address - g_base)
                        : 0;
                }
            }
            // The trigger and the attach of an instance precede its start on
            // the same thread within the same engine call; instance pointers
            // are pooled, so older records for the same pointer are not it.
            constexpr std::uint64_t kMatchWindowUs = 5000;
            const std::uint64_t next = g_next.load(std::memory_order_acquire);
            for (std::uint64_t back = 1; back <= 256 && back <= next; ++back)
            {
                const std::uint64_t index = next - back;
                TriggerCall call;
                if (replay::ring::TryRead(g_ring[index % kRingSize], index, call) != replay::ring::Read::Ready)
                    continue;
                if (call.timeUs + kMatchWindowUs < start.timeUs)
                    break;
                if (call.inst != start.inst || call.threadId != start.threadId)
                    continue;
                start.effectHash = call.effectHash;
                start.assetHash = call.assetHash;
                start.callerSite = call.callers[1];
                break;
            }
            const std::uint64_t nextAttach = g_nextAttach.load(std::memory_order_acquire);
            for (std::uint64_t back = 1; back <= 64 && back <= nextAttach; ++back)
            {
                const std::uint64_t index = nextAttach - back;
                AttachRecord record;
                if (replay::ring::TryRead(g_attaches[index % kAttachRing], index, record) != replay::ring::Read::Ready)
                    continue;
                if (record.timeUs + kMatchWindowUs < start.timeUs)
                    break;
                if (record.inst != start.inst || record.threadId != start.threadId)
                    continue;
                start.entity = record.entity;
                start.bone = record.bone;
                break;
            }
            start.byReplay = InReplayScope();
            kill = OwnerRuleKills(start);
            start.killed = kill;
            replay::ring::Writer<FxStart> slot(g_starts, kStartRing, g_nextStart);
            slot.value = start;
            slot.Publish();
        }

        void __fastcall InstStartDetour(void* inst)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.fx_inst_start");
            struct InFlight
            {
                InFlight() { g_inFlight.fetch_add(1, std::memory_order_seq_cst); }
                ~InFlight() { g_inFlight.fetch_sub(1, std::memory_order_seq_cst); }
            } inFlight;
            void* watched = g_watchInst.load(std::memory_order_acquire);
            if (inst && inst == watched)
            {
                g_watchInst.store(nullptr, std::memory_order_release);
                if (!g_gameplaySnapshotReady.load(std::memory_order_relaxed) &&
                    GuardedSnapshot(g_gameplaySnapshot, inst))
                {
                    g_gameplaySnapshotReady.store(true, std::memory_order_release);
                }
                std::uint8_t flagBytes[4] = {};
                if (GuardedRead(flagBytes, static_cast<std::uint8_t*>(inst) + 0x580, sizeof(flagBytes)))
                {
                    replay::ring::Writer<MuzzleStart> slot(g_muzzleStarts, kMuzzleStartRing, g_nextMuzzleStart);
                    slot.value.timeUs = QpcUs();
                    std::memcpy(&slot.value.flags, flagBytes, sizeof(flagBytes));
                    slot.Publish();
                }
            }
            bool kill = false;
            if (inst && g_enabled.load(std::memory_order_relaxed))
                RecordStart(inst, kill);
            const InstStartFn original = g_originalStart.load(std::memory_order_acquire);
            if (original)
                original(inst);
            if (kill && original)
            {
                const std::uintptr_t killFn = g_killFn.load(std::memory_order_acquire);
                if (killFn && GuardedKill(killFn, inst))
                    g_oneShotKilled.fetch_add(1, std::memory_order_relaxed);
            }
        }

    }

    bool Install(std::string& error)
    {
        if (g_installed)
            return true;
        QueryPerformanceFrequency(&g_frequency);
        void* target = reinterpret_cast<void*>(sig::Find(kFxTrigger));
        if (!target)
        {
            error = "fx_trigger_signature_mismatch";
            return false;
        }
        auto* base = reinterpret_cast<std::uint8_t*>(sig::ImageBase());
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        g_base = reinterpret_cast<std::uintptr_t>(base);
        g_imageEnd = g_base + nt->OptionalHeader.SizeOfImage;
        // The sites the detours compare with, before any of them runs.
        Sites();
        const std::uintptr_t footCall = sig::Find(kFxFootVfxCall);
        g_footVfxReturn = footCall ? footCall + 5 : 0;
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        {
            error = std::string("mh_initialize_") + MH_StatusToString(init);
            return false;
        }
        void* trampoline = nullptr;
        MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&TriggerDetour), &trampoline);
        if (status != MH_OK || !trampoline)
        {
            error = std::string("mh_create_") + MH_StatusToString(status);
            return false;
        }
        g_original.store(reinterpret_cast<TriggerFn>(trampoline), std::memory_order_release);
        status = replay::hooks::Enable(target);
        if (status != MH_OK)
        {
            MH_RemoveHook(target);
            g_original.store(nullptr, std::memory_order_release);
            error = std::string("mh_enable_") + MH_StatusToString(status);
            return false;
        }
        g_target = target;
        g_installed = true;

        // The player functions are checked before the start observer patches
        // the start prologue (the check would then see the hook's jump).
        std::string playerError;
        EnsurePlayer(playerError);

        // Start observer (research): optional, the trigger observer works
        // without it.
        void* startTarget = reinterpret_cast<void*>(sig::Find(kFxInstStart));
        if (startTarget)
        {
            void* startTrampoline = nullptr;
            if (MH_CreateHook(startTarget, reinterpret_cast<void*>(&InstStartDetour), &startTrampoline) == MH_OK &&
                startTrampoline)
            {
                g_originalStart.store(reinterpret_cast<InstStartFn>(startTrampoline), std::memory_order_release);
                if (replay::hooks::Enable(startTarget) == MH_OK)
                    g_startTarget = startTarget;
                else
                {
                    MH_RemoveHook(startTarget);
                    g_originalStart.store(nullptr, std::memory_order_release);
                }
            }
        }
        if (!g_startTarget)
            error = "start_observer_unavailable";
        // Attach observer: the entity and bone of every attached
        // instance, on the attach core every path ends in.
        void* attachTarget = reinterpret_cast<void*>(sig::Find(kFxAttachCore));
        if (attachTarget)
        {
            void* attachTrampoline = nullptr;
            if (MH_CreateHook(attachTarget, reinterpret_cast<void*>(&AttachDetour), &attachTrampoline) == MH_OK &&
                attachTrampoline)
            {
                g_originalAttach.store(reinterpret_cast<AttachCoreFn>(attachTrampoline), std::memory_order_release);
                if (replay::hooks::Enable(attachTarget) == MH_OK)
                    g_attachTarget = attachTarget;
                else
                {
                    MH_RemoveHook(attachTarget);
                    g_originalAttach.store(nullptr, std::memory_order_release);
                }
            }
        }
        if (!g_attachTarget && error.empty())
            error = "attach_observer_unavailable";
        // Registered-effect observer (research).
        void* registeredTarget = reinterpret_cast<void*>(sig::Find(kFxRegistered));
        if (registeredTarget)
        {
            void* trampolineRegistered = nullptr;
            if (MH_CreateHook(registeredTarget, reinterpret_cast<void*>(&RegisteredDetour), &trampolineRegistered) == MH_OK &&
                trampolineRegistered)
            {
                g_originalRegistered.store(reinterpret_cast<RegisteredFn>(trampolineRegistered), std::memory_order_release);
                g_registeredTarget = registeredTarget; // created; enabled only when wanted
                if (g_registeredWanted)
                    replay::hooks::Enable(registeredTarget);
            }
        }
        if (!g_registeredTarget && error.empty())
            error = "registered_observer_unavailable";
        return true;
    }

    bool EnsurePlayer(std::string& error)
    {
        if (g_start)
            return true;
        const std::uintptr_t start = sig::Find(kFxStart);
        if (!start)
        {
            error = "fx_start_signature_mismatch";
            return false;
        }
        // The functions the player calls, and the initializer's stores of
        // every instance field it writes before a start.
        const sig::Signature* const checks[] = {&kFxTriggeredInst, &kFxAttach, &kFxSetOffsetPos, &kFxInstStart,
            &kFxSetMatrix, &kFxSetParent, &kFxInitSettings, &kFxInitFlags};
        for (const sig::Signature* check : checks)
        {
            if (!sig::Find(*check))
            {
                error = std::string(check->name) + "_signature_mismatch";
                return false;
            }
        }
        g_triggeredInst = sig::Find(kFxTriggeredInst);
        g_attach = sig::Find(kFxAttach);
        g_setOffsetPos = sig::Find(kFxSetOffsetPos);
        g_instStart = sig::Find(kFxInstStart);
        g_setMatrix = sig::Find(kFxSetMatrix);
        g_setParent = sig::Find(kFxSetParent);
        // Optional (fire): missing ones logged by ApplyInstancePlanes.
        g_addPlanes = sig::Find(kFxAddPlanes);
        g_setAux350 = sig::Find(kFxSetAux350);
        g_start = start;
        return true;
    }

    namespace
    {
        bool GuardedStart(
            std::uint32_t effect, void* entity, int bone, const void* matrix, float scale,
            bool& started, std::uint32_t& code)
        {
            const std::uint32_t asset = 0; // any loaded asset, as the vfx code does
            return engine::Call(engine::kFxStart, g_start, started, &effect, &asset, entity, bone, matrix, scale, 0) ||
                engine::Failed(code);
        }
    }

    bool PlayEffect(
        std::uint32_t effectHash,
        void* entity,
        int bone,
        const float matrix[16],
        float scale,
        std::string& error)
    {
        if (!g_start && !EnsurePlayer(error))
            return false;
        alignas(16) float aligned[16];
        std::memcpy(aligned, matrix, sizeof(aligned));
        bool started = false;
        std::uint32_t code = 0;
        if (!GuardedStart(effectHash, entity, bone, aligned, scale, started, code))
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        if (!started)
            error = "no_instance";
        return started;
    }

    namespace
    {
        bool GuardedWeaponEffect(
            std::uint32_t effect, void* entity, int bone, float scale, std::uint32_t recorded,
            bool& started, std::uint32_t& code)
        {
            __try
            {
                std::uint8_t* inst = nullptr;
                if (!engine::Call(engine::kFxTriggeredInst, g_triggeredInst, inst, &effect))
                    return engine::Failed(code);
                if (!inst)
                    return true;
                if (!engine::Call(engine::kFxAttach, g_attach, inst, entity, bone, 0, 0, 0))
                    return engine::Failed(code);
                std::uint32_t& flags = *reinterpret_cast<std::uint32_t*>(inst + kInstFlags);
                flags = (flags & ~kMuzzleFlagMask) | (recorded & kMuzzleFlagMask);
                alignas(16) const float offset[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                if (!engine::Call(engine::kFxSetOffsetPos, g_setOffsetPos, inst, offset))
                    return engine::Failed(code);
                *reinterpret_cast<float*>(inst + kInstZoom) = scale;
                if (!g_watchHash.load(std::memory_order_relaxed) && !g_replaySnapshotReady.load(std::memory_order_relaxed))
                {
                    std::memcpy(g_replaySnapshot, inst, kSnapshotBytes);
                    g_replaySnapshotReady.store(true, std::memory_order_release);
                }
                if (!engine::Call(engine::kFxInstStart, g_instStart, inst))
                    return engine::Failed(code);
                started = true;
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    namespace
    {
        void WriteSettingsRaw(std::uint8_t* inst, const entity::FxSettings& s);

        bool GuardedPlayAs(
            std::uint32_t effect, void* entity, int bone, const float* offset, const float* parent,
            const std::uint8_t* evolutions, float zoom, bool& started, std::uint32_t& code,
            const PlayOptions* options = nullptr)
        {
            __try
            {
                std::uint8_t* inst = nullptr;
                if (!engine::Call(engine::kFxTriggeredInst, g_triggeredInst, inst, &effect))
                    return engine::Failed(code);
                if (!inst)
                    return true;
                if (entity && !engine::Call(engine::kFxAttach, g_attach, inst, entity, bone, 0, 0, 0))
                    return engine::Failed(code);
                if (!engine::Call(engine::kFxSetMatrix, g_setMatrix, inst, offset))
                    return engine::Failed(code);
                if (!entity)
                {
                    alignas(16) float delta[4] = {};
                    void* parented = nullptr;
                    if (!engine::Call(engine::kFxSetParent, g_setParent, parented, inst, delta, parent))
                        return engine::Failed(code);
                }
                std::memcpy(inst + kInstEvolutions, evolutions, kEvolutionBytes);
                *reinterpret_cast<float*>(inst + kInstZoom) = zoom;
                if (options)
                {
                    // The attach bits the weapon code set, and the site's
                    // settings, before the start.
                    if (options->applyFlags && entity)
                    {
                        std::uint32_t& flags = *reinterpret_cast<std::uint32_t*>(inst + kInstFlags);
                        flags = (flags & ~kMuzzleFlagMask) | (options->flags & kMuzzleFlagMask);
                    }
                    if (options->settings)
                    {
                        if ((options->settings->mask & entity::FxSettings::kVel350) && g_setAux350)
                        {
                            alignas(16) float aux[4];
                            std::memcpy(aux, options->settings->vel350, sizeof(aux));
                            if (!engine::Call(engine::kFxSetAux350, g_setAux350, inst, aux))
                                return engine::Failed(code);
                        }
                        WriteSettingsRaw(inst, *options->settings);
                    }
                    if (options->instOut)
                        *options->instOut = reinterpret_cast<std::uint64_t>(inst);
                }
                if (effect == g_watchHash.load(std::memory_order_relaxed) &&
                    !g_replaySnapshotReady.load(std::memory_order_relaxed))
                {
                    std::memcpy(g_replaySnapshot, inst, kSnapshotBytes);
                    g_replaySnapshotReady.store(true, std::memory_order_release);
                }
                if (!engine::Call(engine::kFxInstStart, g_instStart, inst))
                    return engine::Failed(code);
                started = true;
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    bool PlayEffectAs(
        std::uint32_t effectHash,
        void* entity,
        int bone,
        const float offset[16],
        const float parent[16],
        const std::uint8_t evolutions[kEvolutionBytes],
        float zoom,
        std::string& error)
    {
        if (!g_start && !EnsurePlayer(error))
            return false;
        alignas(16) float alignedOffset[16];
        alignas(16) float alignedParent[16];
        std::memcpy(alignedOffset, offset, sizeof(alignedOffset));
        std::memcpy(alignedParent, parent, sizeof(alignedParent));
        bool started = false;
        std::uint32_t code = 0;
        if (!GuardedPlayAs(effectHash, entity, bone, alignedOffset, alignedParent, evolutions, zoom, started, code))
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        if (!started)
            error = "no_instance";
        return started;
    }

    bool PlayEffectAsEx(
        std::uint32_t effectHash,
        void* entity,
        int bone,
        const float offset[16],
        const float parent[16],
        const std::uint8_t evolutions[kEvolutionBytes],
        float zoom,
        const PlayOptions& options,
        std::string& error)
    {
        if (!g_start && !EnsurePlayer(error))
            return false;
        alignas(16) float alignedOffset[16];
        alignas(16) float alignedParent[16];
        std::memcpy(alignedOffset, offset, sizeof(alignedOffset));
        std::memcpy(alignedParent, parent, sizeof(alignedParent));
        bool started = false;
        std::uint32_t code = 0;
        if (options.instOut)
            *options.instOut = 0;
        if (!GuardedPlayAs(effectHash, entity, bone, alignedOffset, alignedParent, evolutions, zoom, started, code, &options))
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        if (!started)
            error = "no_instance";
        return started;
    }

    bool PlayAttachedWeaponEffect(
        std::uint32_t effectHash,
        void* entity,
        int bone,
        float scale,
        std::uint32_t flags,
        std::string& error)
    {
        if (!g_start && !EnsurePlayer(error))
            return false;
        if (!entity)
        {
            error = "no_entity";
            return false;
        }
        bool started = false;
        std::uint32_t code = 0;
        if (!GuardedWeaponEffect(effectHash, entity, bone, scale, flags, started, code))
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        if (!started)
            error = "no_instance";
        return started;
    }

    void CollectMuzzleStarts(std::uint64_t fromUs, std::uint64_t toUs, std::vector<MuzzleStart>& out)
    {
        out.clear();
        replay::ring::ForEachPublished(g_muzzleStarts, kMuzzleStartRing, g_nextMuzzleStart, [&](const MuzzleStart& start) {
            if (start.timeUs >= fromUs && start.timeUs <= toUs)
                out.push_back(start);
        });
    }

    void SetWatchEffect(std::uint32_t effectHash)
    {
        g_watchHash.store(effectHash, std::memory_order_relaxed);
        g_watchInst.store(nullptr, std::memory_order_release);
        g_gameplaySnapshotReady.store(false, std::memory_order_release);
        g_replaySnapshotReady.store(false, std::memory_order_release);
    }

    void SnapshotReplayInstance(std::uint64_t inst)
    {
        if (!inst || g_replaySnapshotReady.load(std::memory_order_relaxed))
            return;
        if (GuardedSnapshot(g_replaySnapshot, reinterpret_cast<const void*>(inst)))
            g_replaySnapshotReady.store(true, std::memory_order_release);
    }

    bool SnapshotInstance(std::uint64_t inst, std::vector<std::uint8_t>& out)
    {
        if (!inst)
            return false;
        out.resize(kSnapshotBytes);
        return GuardedSnapshot(out.data(), reinterpret_cast<const void*>(inst));
    }

    bool TakeGameplaySnapshot(std::vector<std::uint8_t>& out)
    {
        if (!g_gameplaySnapshotReady.load(std::memory_order_acquire))
            return false;
        out.assign(g_gameplaySnapshot, g_gameplaySnapshot + kSnapshotBytes);
        return true;
    }

    bool TakeReplaySnapshot(std::vector<std::uint8_t>& out)
    {
        if (!g_replaySnapshotReady.load(std::memory_order_acquire))
            return false;
        out.assign(g_replaySnapshot, g_replaySnapshot + kSnapshotBytes);
        return true;
    }

    void SetEnabled(bool enabled)
    {
        g_enabled.store(enabled, std::memory_order_relaxed);
    }

    std::uint64_t NowUs()
    {
        return replay::clock::NowUs(); // mod_clock.h (no 21-day overflow)
    }

    void Collect(std::uint64_t fromUs, std::uint64_t toUs, std::vector<TriggerCall>& out)
    {
        out.clear();
        replay::ring::ForEachPublished(g_ring, kRingSize, g_next, [&](const TriggerCall& call) {
            if (call.timeUs >= fromUs && call.timeUs <= toUs)
                out.push_back(call);
        });
    }

    std::uint64_t TotalCalls()
    {
        return g_next.load(std::memory_order_relaxed);
    }

    std::uint64_t TotalStarts()
    {
        return g_nextStart.load(std::memory_order_relaxed);
    }

    std::uint64_t TotalAttaches()
    {
        return g_nextAttach.load(std::memory_order_relaxed);
    }

    bool SetRegisteredObserver(bool enabled, std::string& error)
    {
        g_registeredWanted = enabled;
        if (!g_registeredTarget)
        {
            error = g_installed ? "registered_observer_unavailable" : "applied_at_install";
            return !g_installed;
        }
        const MH_STATUS status = enabled ? MH_EnableHook(g_registeredTarget) : MH_DisableHook(g_registeredTarget);
        if (status != MH_OK && status != MH_ERROR_ENABLED && status != MH_ERROR_DISABLED)
        {
            error = std::string("mh_") + MH_StatusToString(status);
            return false;
        }
        return true;
    }

    namespace
    {
        bool GuardedRegisteredGet(std::uintptr_t fn, const void* owner, int id, std::uint8_t flag, std::uint32_t hash,
            std::uint8_t arg8, std::uint8_t arg9, std::uint64_t& inst, std::uint8_t& created, std::uint32_t& code)
        {
            __try
            {
                std::uint32_t hashCopy = hash;
                std::uint32_t asset = 0;
                std::uint8_t createdOut = 0;
                t_replayRegistered = true;
                void* result = nullptr;
                const bool called = engine::Call(engine::kFxRegistered, fn, result,
                    const_cast<void*>(owner), id, nullptr, flag, &hashCopy, &asset, &createdOut, arg8, arg9);
                t_replayRegistered = false;
                if (!called)
                    return engine::Failed(code);
                inst = reinterpret_cast<std::uint64_t>(result);
                created = createdOut;
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                t_replayRegistered = false;
                return false;
            }
        }

        bool GuardedApplyState(std::uint8_t* inst, const float* matrix, const float* parent, const std::uint8_t* evolutions,
            float zoom, bool start, std::uint32_t& code, bool writeParent)
        {
            __try
            {
                if (!engine::Call(engine::kFxSetMatrix, g_setMatrix, inst, matrix))
                    return engine::Failed(code);
                alignas(16) float delta[4] = {};
                void* parented = nullptr;
                if (writeParent && !engine::Call(engine::kFxSetParent, g_setParent, parented, inst, delta, parent))
                    return engine::Failed(code);
                std::memcpy(inst + kInstEvolutions, evolutions, kEvolutionBytes);
                *reinterpret_cast<float*>(inst + kInstZoom) = zoom;
                if (start && !engine::Call(engine::kFxInstStart, g_instStart, inst))
                    return engine::Failed(code);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    std::uint64_t RegisteredGetOrCreate(const void* owner, int id, std::uint8_t flag, std::uint32_t effectHash,
        std::uint8_t arg8, std::uint8_t arg9, bool& created, std::string& error)
    {
        created = false;
        if (!g_start && !EnsurePlayer(error))
            return 0;
        auto fn = reinterpret_cast<std::uintptr_t>(g_originalRegistered.load(std::memory_order_acquire));
        if (!fn)
        {
            fn = sig::Find(kFxRegistered);
            if (!fn)
            {
                error = "fx_registered_signature_mismatch";
                return 0;
            }
        }
        for (const sig::Signature* site : kFxRegisteredArgs)
        {
            if (!sig::Find(*site))
            {
                error = "fx_registered_signature_mismatch";
                return 0;
            }
        }
        std::uint64_t inst = 0;
        std::uint8_t createdOut = 0;
        std::uint32_t code = 0;
        if (!GuardedRegisteredGet(fn, owner, id, flag, effectHash, arg8, arg9, inst, createdOut, code))
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return 0;
        }
        created = createdOut != 0;
        if (!inst)
            error = "no_instance";
        return inst;
    }

    bool ApplyInstanceState(std::uint64_t inst, const float matrix[16], const float parent[16],
        const std::uint8_t evolutions[kEvolutionBytes], float zoom, bool start, std::string& error, bool writeParent)
    {
        if (!inst)
        {
            error = "no_instance";
            return false;
        }
        if (!g_start && !EnsurePlayer(error))
            return false;
        alignas(16) float alignedMatrix[16];
        alignas(16) float alignedParent[16];
        std::memcpy(alignedMatrix, matrix, sizeof(alignedMatrix));
        std::memcpy(alignedParent, parent, sizeof(alignedParent));
        std::uint32_t code = 0;
        if (!GuardedApplyState(reinterpret_cast<std::uint8_t*>(inst), alignedMatrix, alignedParent, evolutions, zoom, start, code,
                writeParent))
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        return true;
    }

    namespace
    {
        bool GuardedApplyPlanes(std::uint8_t* inst, const float* planes, std::uint32_t planeCount, const float* aux350,
            std::uint32_t& code)
        {
            __try
            {
                if (aux350 && !engine::Call(engine::kFxSetAux350, g_setAux350, inst, aux350))
                    return engine::Failed(code);
                if (planeCount)
                {
                    // The list the engine reads (the plane adder: r8 = list + 0x20, rows at
                    // r8 - 0x20, r8 - 0x10, r8): planes from +0, 3 float4 each, count at +0x300.
                    alignas(16) std::uint8_t list[0x320] = {};
                    std::memcpy(list, planes, planeCount * 12 * sizeof(float));
                    std::memcpy(list + 0x300, &planeCount, 4);
                    if (!engine::Call(engine::kFxAddPlanes, g_addPlanes, inst, list))
                        return engine::Failed(code);
                }
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    bool ApplyInstancePlanes(std::uint64_t inst, const float* planes, std::uint32_t planeCount, const float aux350[4],
        std::string& error)
    {
        if (!inst)
        {
            error = "no_instance";
            return false;
        }
        if (!g_start && !EnsurePlayer(error))
            return false;
        if (!g_addPlanes || !g_setAux350)
        {
            // Which signature the game lacks (the resolver's SIG line has its result).
            error = std::string("planes_functions_unavailable,add_planes=") + (g_addPlanes ? "found" : "missing") +
                ",set_aux=" + (g_setAux350 ? "found" : "missing");
            return false;
        }
        alignas(16) float alignedAux[4];
        std::memcpy(alignedAux, aux350, sizeof(alignedAux));
        std::uint32_t code = 0;
        if (!GuardedApplyPlanes(reinterpret_cast<std::uint8_t*>(inst), planes, planeCount > kMaxFxPlanes ? kMaxFxPlanes : planeCount,
                alignedAux, code))
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        return true;
    }

    namespace
    {
        bool GuardedWriteExtras(std::uint8_t* inst, const float* start310, float farClip)
        {
            __try
            {
                if (start310[0] != 0.0f || start310[1] != 0.0f || start310[2] != 0.0f)
                    std::memcpy(inst + 0x310, start310, 4 * sizeof(float));
                if (farClip >= 0.0f)
                    std::memcpy(inst + 0x3C4, &farClip, sizeof(farClip));
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    bool ApplyInstanceExtras(std::uint64_t inst, const float start310[4], float farClip, std::string& error)
    {
        if (!inst)
        {
            error = "no_instance";
            return false;
        }
        // +0x310 and +0x3C4 only where the initializer's stores were found.
        if (!g_start && !EnsurePlayer(error))
            return false;
        if (!GuardedWriteExtras(reinterpret_cast<std::uint8_t*>(inst), start310, farClip))
        {
            error = "extras_write_fault";
            return false;
        }
        return true;
    }

    namespace
    {
        // The instance stop (inst, immediate).
        const sig::Signature kFxStop{"fx_stop",
            "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 40 48 8B F9 33 DB 48 8B 0D ?? "
            "?? ?? ?? 44 8A F2",
            0x29E7C3C};
        bool GuardedStop(std::uintptr_t stop, std::uint64_t inst, std::uint8_t immediate, std::uint32_t& code)
        {
            return engine::Call(engine::kFxStop, stop, reinterpret_cast<void*>(inst), immediate) || engine::Failed(code);
        }
    }

    bool ReadInstanceIdentity(std::uint64_t inst, InstanceIdentity& out)
    {
        out = InstanceIdentity{};
        if (!inst)
            return false;
        const std::uint8_t* base = reinterpret_cast<const std::uint8_t*>(inst);
        std::uint8_t bytes[8] = {};
        if (!GuardedRead(bytes, base, 8))
            return false;
        std::memcpy(&out.rule, bytes, 8);
        if (!out.rule || !GuardedRead(bytes, reinterpret_cast<const std::uint8_t*>(out.rule) + 0x200, 4))
            return false;
        std::memcpy(&out.hash, bytes, 4);
        if (!GuardedRead(bytes, base + 0x344, 4))
            return false;
        std::memcpy(&out.state, bytes, 4);
        float matrix[16] = {};
        float parent[16] = {};
        if (!GuardedRead(reinterpret_cast<std::uint8_t*>(matrix), base + kInstMatrix, sizeof(matrix)) ||
            !GuardedRead(reinterpret_cast<std::uint8_t*>(parent), base + kInstParentMatrix, sizeof(parent)))
            return false;
        float world[16] = {};
        replay::entity::ComposeFxWorld(matrix, parent, world);
        out.position[0] = world[12];
        out.position[1] = world[13];
        out.position[2] = world[14];
        out.offset[0] = matrix[12];
        out.offset[1] = matrix[13];
        out.offset[2] = matrix[14];
        return true;
    }

    bool StopInstance(std::uint64_t inst, bool immediate, std::string& error)
    {
        static const std::uintptr_t stop = sig::Find(kFxStop);
        if (!stop)
        {
            error = "fx_stop_signature_mismatch";
            return false;
        }
        if (!inst)
        {
            error = "no_instance";
            return false;
        }
        std::uint32_t code = 0;
        if (!GuardedStop(stop, inst, immediate ? 1 : 0, code))
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        return true;
    }

    bool ReadInstanceState(std::uint64_t inst, InstanceState& out)
    {
        if (!inst)
            return false;
        const std::uint8_t* base = reinterpret_cast<const std::uint8_t*>(inst);
        if (!GuardedRead(reinterpret_cast<std::uint8_t*>(out.matrix), base + kInstMatrix, sizeof(out.matrix)))
            return false;
        GuardedRead(reinterpret_cast<std::uint8_t*>(out.parent), base + kInstParentMatrix, sizeof(out.parent));
        GuardedRead(out.evolutions, base + kInstEvolutions, kEvolutionBytes);
        std::uint8_t bytes[4] = {};
        if (GuardedRead(bytes, base + kInstZoom, 4))
            std::memcpy(&out.zoom, bytes, 4);
        if (GuardedRead(bytes, base + kInstFlags, 4))
            std::memcpy(&out.flags, bytes, 4);
        if (GuardedRead(bytes, base + kInstMode, 1))
            out.mode = bytes[0];
        if (GuardedRead(bytes, base + 0x344, 4))
            std::memcpy(&out.state, bytes, 4);
        if (GuardedRead(bytes, base + 0x3F0, 4))
            std::memcpy(&out.timer3f0, bytes, 4);
        if (GuardedRead(bytes, base + 0x320, 4))
            std::memcpy(&out.loops, bytes, 4);
        if (GuardedRead(bytes, base + 0x32C, 4))
            std::memcpy(&out.loopTime, bytes, 4);
        if (GuardedRead(bytes, base + 0x584, 4))
            std::memcpy(&out.flags2, bytes, 4);
        if (GuardedRead(bytes, base + 0x341, 1))
            out.byte341 = bytes[0];
        GuardedRead(reinterpret_cast<std::uint8_t*>(out.aux350), base + 0x350, sizeof(out.aux350));
        GuardedRead(reinterpret_cast<std::uint8_t*>(out.start310), base + 0x310, sizeof(out.start310));
        ReadSettingsRaw(base, out.settings);
        GuardedRead(reinterpret_cast<std::uint8_t*>(out.veladd360), base + 0x360, sizeof(out.veladd360));
        if (GuardedRead(bytes, base + 0x3C4, 4))
            std::memcpy(&out.farClip3C4, bytes, 4);
        // Collision planes of the current quality level.
        std::uint8_t quality = 0;
        {
            std::uint8_t pointer[8] = {};
            const std::uintptr_t global = sig::Find(kFxQuality);
            if (global && GuardedRead(pointer, reinterpret_cast<const std::uint8_t*>(global), 8))
            {
                std::uint64_t address = 0;
                std::memcpy(&address, pointer, 8);
                if (!address || !GuardedRead(&quality, reinterpret_cast<const std::uint8_t*>(address) + 1, 1))
                    quality = 0xFF;
            }
            else
                quality = 0xFF;
        }
        out.planeCount = 0;
        out.quality = quality;
        for (int slotIndex = 0; slotIndex < 3; ++slotIndex)
        {
            std::uint32_t count = 0;
            if (GuardedRead(bytes, base + static_cast<std::size_t>(slotIndex) * 0x310 + 0x930, 4))
                std::memcpy(&count, bytes, 4);
            out.slotCounts[slotIndex] = count;
        }
        if (quality < 3)
        {
            const std::size_t slot = static_cast<std::size_t>(quality) * 0x310;
            std::uint32_t count = 0;
            if (GuardedRead(bytes, base + slot + 0x930, 4))
                std::memcpy(&count, bytes, 4);
            if (count > kMaxFxPlanes)
                count = kMaxFxPlanes;
            if (count && GuardedRead(reinterpret_cast<std::uint8_t*>(out.planes), base + slot + 0x630, count * 12 * sizeof(float)))
                out.planeCount = count;
        }
        return true;
    }

    bool LastAttachOf(std::uint64_t inst, std::uint64_t& entity, std::int32_t& bone, std::uint64_t sinceUs,
        std::uint64_t* whenUs)
    {
        const std::uint64_t next = g_nextAttach.load(std::memory_order_acquire);
        for (std::uint64_t back = 1; back <= kAttachRing && back <= next; ++back)
        {
            const std::uint64_t index = next - back;
            AttachRecord record;
            if (replay::ring::TryRead(g_attaches[index % kAttachRing], index, record) != replay::ring::Read::Ready ||
                record.inst != inst)
                continue;
            if (record.timeUs < sinceUs)
                return false; // older than the instance: a previous tenant of the address
            entity = record.entity;
            bone = record.bone;
            if (whenUs)
                *whenUs = record.timeUs;
            return true;
        }
        return false;
    }

    namespace
    {
        bool GuardedAttach(std::uint8_t* inst, void* entity, int bone, std::uint32_t& code)
        {
            __try
            {
                if (!engine::Call(engine::kFxAttach, g_attach, inst, entity, bone, 0, 0, 0))
                    return engine::Failed(code);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    bool AttachInstance(std::uint64_t inst, void* entity, int bone, std::string& error)
    {
        if (!inst || !entity)
        {
            error = "no_instance_or_entity";
            return false;
        }
        if (!g_start && !EnsurePlayer(error))
            return false;
        std::uint32_t code = 0;
        if (!GuardedAttach(reinterpret_cast<std::uint8_t*>(inst), entity, bone, code))
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        return true;
    }

    void SetSuppressedOwners(const std::vector<std::uint64_t>& owners)
    {
        const std::uint32_t count = static_cast<std::uint32_t>((std::min)(owners.size(), kMaxSuppressedOwners));
        // Shrink first, fill, then publish the new count.
        if (count < g_suppressedCount.load(std::memory_order_relaxed))
            g_suppressedCount.store(count, std::memory_order_release);
        for (std::uint32_t n = 0; n < count; ++n)
            g_suppressedOwners[n].store(owners[n], std::memory_order_relaxed);
        g_suppressedCount.store(count, std::memory_order_release);
    }

    bool SetOneShotOwnerRule(bool on, const std::vector<std::uint32_t>& exempt, std::string& error)
    {
        if (!on)
        {
            g_oneShotOwnerRule.store(false, std::memory_order_release);
            return true;
        }
        const std::uintptr_t stop = sig::Find(kFxStop);
        if (!stop)
        {
            error = "fx_stop_signature_mismatch";
            g_oneShotOwnerRule.store(false, std::memory_order_release);
            return false;
        }
        g_killFn.store(stop, std::memory_order_release);
        const std::uint32_t count = static_cast<std::uint32_t>((std::min)(exempt.size(), kMaxExempt));
        g_exemptCount.store(0, std::memory_order_release);
        for (std::uint32_t n = 0; n < count; ++n)
            g_exemptHashes[n].store(exempt[n], std::memory_order_relaxed);
        g_exemptCount.store(count, std::memory_order_release);
        g_oneShotOwnerRule.store(true, std::memory_order_release);
        return true;
    }

    void SetWorldSiteRule(const std::vector<std::uint32_t>& sites)
    {
        const std::uint32_t count = static_cast<std::uint32_t>((std::min)(sites.size(), kMaxWorldSites));
        g_worldSiteCount.store(0, std::memory_order_release);
        for (std::uint32_t n = 0; n < count; ++n)
            g_worldSites[n].store(sites[n], std::memory_order_relaxed);
        g_worldSiteCount.store(count, std::memory_order_release);
    }

    std::uint64_t OneShotOwnerKilled()
    {
        return g_oneShotKilled.load(std::memory_order_relaxed);
    }

    ReplayScope::ReplayScope()
    {
        if (g_replayScopeDepth.fetch_add(1, std::memory_order_acq_rel) == 0)
            g_replayScopeThread.store(GetCurrentThreadId(), std::memory_order_release);
    }

    ReplayScope::~ReplayScope()
    {
        if (g_replayScopeDepth.fetch_sub(1, std::memory_order_acq_rel) == 1)
            g_replayScopeThread.store(0, std::memory_order_release);
    }

    void SetEngineFootVfxSuppressed(bool suppressed)
    {
        g_suppressFootVfx.store(suppressed, std::memory_order_relaxed);
    }

    std::uint64_t SuppressedFootVfx()
    {
        return g_suppressedFootVfx.load(std::memory_order_relaxed);
    }

    std::uint32_t MuzzleFlashSite() { return Sites().muzzle; }
    std::uint32_t BulletImpactSite() { return Sites().impact; }
    std::uint32_t FootVfxSite() { return Sites().foot; }
    std::uint32_t CameraAmbientFxSite() { return Sites().cameraAmbient; }
    std::uint32_t BodyHitSite() { return Sites().body; }
    std::uint32_t ShellRegisteredSite() { return Sites().shell; }
    std::uint32_t ScriptLoopStartSite() { return Sites().scriptStart; }
    std::uint32_t ScriptLoopKeepAliveSite() { return Sites().scriptKeepAlive; }
    std::uint32_t HoofRegisteredSite() { return Sites().hoof; }
    std::uint32_t RegistryRestartSite() { return Sites().registryRestart; }

    std::uintptr_t MuzzleInfoTable()
    {
        static const std::uintptr_t table = sig::Find(weaponfx::kWeaponMuzzleInfoTable);
        return table;
    }

    std::uintptr_t Viewport()
    {
        static const std::uintptr_t viewport = sig::Find(weaponfx::kWeaponViewport);
        return viewport;
    }

    std::uint64_t TotalSuppressed()
    {
        return g_suppressedCalls.load(std::memory_order_relaxed);
    }

    std::uint64_t TotalRegistered()
    {
        return g_nextRegistered.load(std::memory_order_relaxed);
    }

    void CollectRegisteredSince(std::uint64_t& cursor, std::vector<RegisteredCall>& out)
    {
        out.clear();
        replay::ring::ForEachSince(g_registered, kRegisteredRing, g_nextRegistered, cursor,
            [&](const RegisteredCall& call) { out.push_back(call); });
    }

    void CollectStartsSince(std::uint64_t& cursor, std::vector<FxStart>& out)
    {
        out.clear();
        replay::ring::ForEachSince(g_starts, kStartRing, g_nextStart, cursor, [&](const FxStart& start) { out.push_back(start); });
    }

    namespace
    {
        void WriteSettingsRaw(std::uint8_t* inst, const entity::FxSettings& s)
        {
            // Plain fields of an instance of ours, before its start (degree 3).
            // +0x350 goes through the engine's setter (callers), +0x5CC never.
            if (s.mask & entity::FxSettings::kTint380)
                std::memcpy(inst + 0x380, s.tint380, 12);
            if (s.mask & entity::FxSettings::kAlpha38C)
                std::memcpy(inst + 0x38C, &s.alpha38C, 4);
            if (s.mask & entity::FxSettings::kAux39C)
                std::memcpy(inst + 0x39C, &s.aux39C, 4);
            if (s.mask & entity::FxSettings::kLod3A0)
                std::memcpy(inst + 0x3A0, &s.lod3A0, 4);
            if (s.mask & entity::FxSettings::kTele3A8)
                std::memcpy(inst + 0x3A8, &s.tele3A8, 4);
            // One byte. +0x3BC is a byte (the instance update reads it as
            // one, a vfx site writes one); +0x3BD is the instance update's own
            // (it zeroes it and tests it with +0x340) [binary]: a recorded
            // value of a live instance would put that runtime byte back on
            // ours.
            if (s.mask & entity::FxSettings::kCull3BC)
                inst[0x3BC] = static_cast<std::uint8_t>(s.cull3BC & 0xFFu);
        }

        bool GuardedWriteSettings(std::uint8_t* inst, const entity::FxSettings& s, const float* aux, std::uint32_t& code)
        {
            __try
            {
                if (aux && !engine::Call(engine::kFxSetAux350, g_setAux350, inst, aux))
                    return engine::Failed(code);
                WriteSettingsRaw(inst, s);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // The particle manager's lock (see ClearRelocateMarks), the way the
        // parent setter takes it: it loads the manager global into rcx, takes
        // the lock object at manager +0x3E280, calls the owner test, the lock
        // (when not owned), writes the parent matrix and calls the unlock.
        constexpr std::size_t kPtfxLockOffset = 0x3E280;
        const sig::Signature kFxParentLockLoad{"fx_parent_lock_load", kFxSetParent, sig::Offset{0x1F},
            "48 8B 0D ?? ?? ?? ?? 48 8B F2 33 FF 48 8D 99 80 E2 03 00 E8 ?? ?? ?? ?? 84 C0 75 0D 48 8B CB E8 ?? ?? "
            "?? ?? BF 01 00 00 00 0F 10 45 30 41 0F 5C 86 40 02 00 00 0F 11 06 0F 10 45 00 41 0F 11 86 10 02 00 00 "
            "0F 10 4D 10 41 0F 11 8E 20 02 00 00 0F 10 45 20 41 0F 11 86 30 02 00 00 0F 10 4D 30 41 0F 11 8E 40 02 "
            "00 00 83 FF 01 75 08 48 8B CB E8 ?? ?? ?? ??",
            0x29E2773};
        const sig::Signature kFxPtfxManager{"fx_ptfx_manager", kFxParentLockLoad, sig::Rel32{3, 7}, "", 0x3EBD698};
        const sig::Signature kFxLockOwned{"fx_lock_owned", kFxParentLockLoad, sig::Rel32{0x14, 0x18},
            "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 20 65 48 8B 14 25 58 00 00 00 8B "
            "05 ?? ?? ?? ?? BD 50 06 00 00 48 8B 04 C2 48 8B 2C 28 48 39 A9 78 E3 03 00",
            0x29DB42C};
        // The lock and the unlock: sub rsp,28h; cmp qword [rcx],0; je +6; call
        // [rip+rel32] (the import, free); add rsp,28h; ret.
        const sig::Signature kFxLock{"fx_lock", kFxParentLockLoad, sig::Rel32{0x20, 0x24},
            "48 83 EC 28 48 83 39 00 74 06 FF 15 ?? ?? ?? ?? 48 83 C4 28 C3", 0x25A1658};
        const sig::Signature kFxUnlock{"fx_unlock", kFxParentLockLoad, sig::Rel32{0x71, 0x75},
            "48 83 EC 28 48 83 39 00 74 06 FF 15 ?? ?? ?? ?? 48 83 C4 28 C3", 0x25A5694};
        // The relocation mark: every instance attached to an entity whose
        // transform a script native set gets `or dword [inst+0x584], 2`, the
        // bit ClearRelocateMarks clears.
        const sig::Signature kFxRelocateMark{"fx_relocate_mark",
            "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 20 4C 8B 05 ?? ?? ?? ?? 45 33 FF "
            "4C 8B F2 41 0F B7 80 A8 BB 03 00 66 44 3B F8 73 4F 41 8B DF 0F B7 F0 49 8B 80 A0 BB 03 00 48 8B 0C 03 "
            "48 8B 01 48 85 C0 74 2C 48 8B 28 48 8D 78 20 48 8B CF 49 8B D6 E8 ?? ?? ?? ?? 84 C0 74 07 83 8F 84 05 "
            "00 00 02",
            0x143C180};
        int g_relocateLockChecked = -1;
        std::uintptr_t g_lockOwned = 0;
        std::uintptr_t g_lock = 0;
        std::uintptr_t g_unlock = 0;
        std::string g_relocateLockError;

        bool CheckRelocateLock()
        {
            if (g_relocateLockChecked >= 0)
                return g_relocateLockChecked == 1;
            char text[64]{};
            for (const sig::Signature* check :
                {&kFxParentLockLoad, &kFxPtfxManager, &kFxLockOwned, &kFxLock, &kFxUnlock, &kFxRelocateMark})
                if (!sig::Find(*check))
                {
                    std::snprintf(
                        text, sizeof(text), "sig_mismatch,rva=0x%llx", static_cast<unsigned long long>(check->rva));
                    break;
                }
            if (text[0])
            {
                g_relocateLockError = text;
                g_relocateLockChecked = 0;
                return false;
            }
            g_lockOwned = sig::Find(kFxLockOwned);
            g_lock = sig::Find(kFxLock);
            g_unlock = sig::Find(kFxUnlock);
            g_relocateLockChecked = 1;
            return true;
        }

        // The owner test takes the MANAGER, not the lock object: the parent
        // setter loads the manager global into rcx, only copies
        // manager+0x3E280 into rbx for the lock calls, and calls the owner test
        // with rcx still the manager; the owner test compares [rcx+0x3E378]
        // with the thread's TLS slot +0x650 (and scans the worker list)
        // [binary].
        bool GuardedLockEnter(void* manager, void* lock, bool& taken, std::uint32_t& code)
        {
            taken = false;
            bool owned = false;
            if (!engine::Call(engine::kFxLockOwned, g_lockOwned, owned, manager))
                return engine::Failed(code);
            if (!owned)
            {
                if (!engine::Call(engine::kFxLock, g_lock, lock))
                    return engine::Failed(code);
                taken = true;
            }
            return true;
        }

        bool GuardedLockLeave(void* lock, std::uint32_t& code)
        {
            return engine::Call(engine::kFxUnlock, g_unlock, lock) || engine::Failed(code);
        }

        // Under the lock, the instance must
        // still hold the effect we put there (rule +0x200) and be playing or
        // stopping (+0x344 in 2..3); an instance the engine finished and gave
        // to another effect (a one-shot of ours is kept 5 s in the list, a
        // registered entry the attach update freed) is left alone.
        bool GuardedClearBit(std::uint64_t inst, std::uint32_t hash, bool& wasSet, bool& stale)
        {
            wasSet = false;
            stale = false;
            __try
            {
                const std::uint64_t rule = *reinterpret_cast<const volatile std::uint64_t*>(inst);
                const std::uint32_t held = rule ? *reinterpret_cast<const volatile std::uint32_t*>(rule + 0x200) : 0;
                const std::uint32_t state = *reinterpret_cast<const volatile std::uint32_t*>(inst + 0x344);
                if (!rule || held != hash || state < 2 || state > 3) // regfx kStatePlaying..kStateStopping
                {
                    stale = true;
                    return true;
                }
                volatile long* field = reinterpret_cast<volatile long*>(inst + 0x584);
                const long before = _InterlockedAnd(field, ~2L);
                wasSet = (before & 2L) != 0;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // The placement names its entity.
        thread_local const void* t_placeEntity = nullptr;
        std::atomic<std::uint64_t> g_relocateSkipped{0};

        // The vfx update with the step in xmm1.
        const sig::Signature kFxVfxUpdate{"fx_vfx_update",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 18 57 48 83 EC 30 0F 29 70 E8 48 8B F9 0F 57 F6",
            0x145C17C};
        using VfxUpdateFn = void*(__fastcall*)(void*, float);
        std::atomic<VfxUpdateFn> g_originalVfxUpdate{nullptr};
        void* g_vfxUpdateTarget = nullptr;
        std::atomic<bool> g_pauseFrozen{false};
        std::atomic<std::uint64_t> g_pauseFrozenUpdates{0};
        // The freeze holds only while the
        // script thread renews it (every paused playback tick); a playback
        // that ends by a path that does not reach SetPauseFreeze(false) (a
        // script thread that stops ticking, an exception in the tick) cannot
        // leave the game's particles frozen: kPauseFreezeLeaseMs after the
        // last renewal the detour lets the engine's step through again.
        constexpr std::uint64_t kPauseFreezeLeaseMs = 1500;
        std::atomic<std::uint64_t> g_pauseFrozenUntilMs{0};
        std::atomic<std::uint64_t> g_pauseFreezeExpired{0};

        void* __fastcall VfxUpdateDetour(void* self, float step)
        {
            replay::lifetime::DetourScope lifetime;
            struct InFlight
            {
                InFlight() { g_inFlight.fetch_add(1, std::memory_order_seq_cst); }
                ~InFlight() { g_inFlight.fetch_sub(1, std::memory_order_seq_cst); }
            } inFlight;
            if (g_pauseFrozen.load(std::memory_order_relaxed))
            {
                if (GetTickCount64() < g_pauseFrozenUntilMs.load(std::memory_order_relaxed))
                {
                    step = 0.0f;
                    g_pauseFrozenUpdates.fetch_add(1, std::memory_order_relaxed);
                }
                else if (g_pauseFrozen.exchange(false, std::memory_order_relaxed))
                    g_pauseFreezeExpired.fetch_add(1, std::memory_order_relaxed);
            }
            const VfxUpdateFn original = g_originalVfxUpdate.load(std::memory_order_acquire);
            return original ? original(self, step) : nullptr;
        }

        // A signature found is hooked; `name` is the error's (the log's) name.
        bool InstallHookAt(const sig::Signature& signature, void* detour, void** trampoline, void** target,
            const char* name, std::string& error)
        {
            if (*target)
                return true;
            void* at = reinterpret_cast<void*>(sig::Find(signature));
            if (!at)
            {
                char text[96]{};
                std::snprintf(text, sizeof(text), "%s_sig_mismatch,rva=0x%llx", name,
                    static_cast<unsigned long long>(signature.rva));
                error = text;
                return false;
            }
            const MH_STATUS init = MH_Initialize();
            if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
            {
                error = std::string("mh_initialize_") + MH_StatusToString(init);
                return false;
            }
            MH_STATUS status = MH_CreateHook(at, detour, trampoline);
            if (status != MH_OK || !*trampoline)
            {
                error = std::string("mh_create_") + MH_StatusToString(status);
                return false;
            }
            status = replay::hooks::Enable(at);
            if (status != MH_OK)
            {
                MH_RemoveHook(at);
                error = std::string("mh_enable_") + MH_StatusToString(status);
                return false;
            }
            *target = at;
            return true;
        }
    }

    void SetWeaponSitesExempt(bool exempt)
    {
        g_weaponSitesExempt.store(exempt, std::memory_order_relaxed);
    }

    bool ReadInstanceSettings(std::uint64_t inst, entity::FxSettings& out)
    {
        out = entity::FxSettings{};
        if (!inst)
            return false;
        ReadSettingsRaw(reinterpret_cast<const std::uint8_t*>(inst), out);
        return out.mask != 0;
    }

    bool ApplyInstanceSettings(std::uint64_t inst, const entity::FxSettings& settings, std::string& error)
    {
        if (!inst)
        {
            error = "no_instance";
            return false;
        }
        if (!g_start && !EnsurePlayer(error))
            return false;
        alignas(16) float aux[4];
        std::memcpy(aux, settings.vel350, sizeof(aux));
        const bool withAux = (settings.mask & entity::FxSettings::kVel350) && g_setAux350;
        std::uint32_t code = 0;
        if (!GuardedWriteSettings(reinterpret_cast<std::uint8_t*>(inst), settings, withAux ? aux : nullptr, code))
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        return true;
    }

    bool ReadMotionFields(std::uint64_t inst, MotionFields& out)
    {
        out = MotionFields{};
        if (!inst)
            return false;
        const std::uint8_t* base = reinterpret_cast<const std::uint8_t*>(inst);
        std::uint8_t bytes[4] = {};
        if (!GuardedRead(bytes, base + 0x584, 4))
            return false;
        std::memcpy(&out.flags2, bytes, 4);
        // The effect it holds now (rule
        // +0x200, as ReadInstanceIdentity), so a caller tells a recycled
        // instance from ours.
        std::uint64_t rule = 0;
        if (GuardedRead(reinterpret_cast<std::uint8_t*>(&rule), base, 8) && rule &&
            GuardedRead(bytes, reinterpret_cast<const std::uint8_t*>(rule) + 0x200, 4))
            std::memcpy(&out.hash, bytes, 4);
        if (GuardedRead(bytes, base + 0x3A8, 4))
            std::memcpy(&out.tele3A8, bytes, 4);
        if (GuardedRead(bytes, base + 0x344, 4))
            std::memcpy(&out.state, bytes, 4);
        GuardedRead(reinterpret_cast<std::uint8_t*>(out.veladd), base + 0x360, sizeof(out.veladd));
        return true;
    }

    bool ClearRelocateMarks(const std::vector<std::pair<std::uint64_t, std::uint32_t>>& insts, std::uint32_t& cleared,
        std::uint32_t& stale, std::string& error)
    {
        cleared = 0;
        stale = 0;
        if (insts.empty())
            return true;
        if (!CheckRelocateLock())
        {
            error = g_relocateLockError;
            return false;
        }
        std::uint64_t manager = 0;
        const auto* global = reinterpret_cast<const std::uint8_t*>(sig::Find(kFxPtfxManager));
        if (!GuardedRead(reinterpret_cast<std::uint8_t*>(&manager), global, 8) || !manager)
        {
            error = "no_ptfx_manager";
            return false;
        }
        void* lock = reinterpret_cast<void*>(manager + kPtfxLockOffset);
        bool taken = false;
        std::uint32_t code = 0;
        if (!GuardedLockEnter(reinterpret_cast<void*>(manager), lock, taken, code))
        {
            char text[48]{};
            std::snprintf(text, sizeof(text), "lock_exception_%08X", code);
            error = text;
            return false;
        }
        for (const auto& [inst, hash] : insts)
        {
            bool wasSet = false;
            bool notOurs = false;
            if (!inst || !GuardedClearBit(inst, hash, wasSet, notOurs))
                continue;
            if (notOurs)
                ++stale;
            else if (wasSet)
                ++cleared;
        }
        if (taken && !GuardedLockLeave(lock, code))
        {
            char text[48]{};
            std::snprintf(text, sizeof(text), "unlock_exception_%08X", code);
            error = text;
            return false;
        }
        return true;
    }

    bool RelocateMarkReady(std::string& error)
    {
        if (CheckRelocateLock())
            return true;
        error = g_relocateLockError;
        return false;
    }

    bool ReadEntityDrawData(const void* entity, bool& present)
    {
        present = false;
        if (!entity)
            return false;
        std::uint64_t first = 0;
        if (!GuardedRead(reinterpret_cast<std::uint8_t*>(&first), static_cast<const std::uint8_t*>(entity) + 0x28, 8))
            return false;
        if (!first)
            return true;
        std::uint64_t second = 0;
        if (!GuardedRead(reinterpret_cast<std::uint8_t*>(&second), reinterpret_cast<const std::uint8_t*>(first) + 8, 8))
            return false;
        present = second != 0;
        return true;
    }

    PlaceScope::PlaceScope(const void* entity) { t_placeEntity = entity; }
    PlaceScope::~PlaceScope() { t_placeEntity = nullptr; }

    std::uint64_t RelocateSkipped() { return g_relocateSkipped.load(std::memory_order_relaxed); }

    bool InstallPauseFreeze(std::string& error)
    {
        void* trampoline = nullptr;
        if (g_vfxUpdateTarget)
            return true;
        if (!InstallHookAt(kFxVfxUpdate, reinterpret_cast<void*>(&VfxUpdateDetour), &trampoline, &g_vfxUpdateTarget,
                "vfx_update", error))
            return false;
        g_originalVfxUpdate.store(reinterpret_cast<VfxUpdateFn>(trampoline), std::memory_order_release);
        return true;
    }

    void SetPauseFreeze(bool frozen)
    {
        // The lease first, then the switch (the detour reads them the other way).
        g_pauseFrozenUntilMs.store(frozen ? GetTickCount64() + kPauseFreezeLeaseMs : 0, std::memory_order_relaxed);
        g_pauseFrozen.store(frozen, std::memory_order_relaxed);
    }

    std::uint64_t PauseFrozenUpdates() { return g_pauseFrozenUpdates.load(std::memory_order_relaxed); }

    std::uint64_t PauseFreezeExpired() { return g_pauseFreezeExpired.load(std::memory_order_relaxed); }

    // The weapon system's requests for a shot's muzzle flash and shell,
    // recorded and forwarded.
    namespace
    {
        // [binary] The muzzle request (one caller, weapon_fx_intent_core.h
        // kWeaponMuzzleCaller, after the chance roll) and the shell request
        // (one caller, from a queued record {shooter, weapon, object, a5, a6}).
        const sig::Signature kFxMuzzleRequest{"fx_muzzle_request",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 18 57 41 54 41 55 41 56 41 57 48 83 EC 60 48 8B AC 24 C0 00 00 "
            "00 4D 8B F1 8B B4 24 C8 00 00 00 4D 8B F8 48 8B BC 24 B8 00 00 00 4C 8B E2 8B 9C 24 B0 00 00 00 4C 8B E9",
            0x14309A8};
        const sig::Signature kFxShellRequest{"fx_shell_request",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 18 57 41 56 41 57 48 83 EC 40 8B BC 24 88 00 00 00 49 8B F1 8B "
            "9C 24 80 00 00 00 49 8B E8 C6 40 D8 00 4C 8B F2 89 78 D0 4C 8B F9",
            0x14308F0};
        using MuzzleRequestFn = void*(__fastcall*)(void*, void*, void*, void*, std::int32_t, const void*, const void*, std::int32_t);
        using ShellRequestFn = void*(__fastcall*)(void*, void*, void*, void*, std::int32_t, std::int32_t);
        std::atomic<MuzzleRequestFn> g_originalMuzzleRequest{nullptr};
        std::atomic<ShellRequestFn> g_originalShellRequest{nullptr};
        void* g_muzzleRequestTarget = nullptr;
        void* g_shellRequestTarget = nullptr;

        constexpr std::uint32_t kWeaponFxRing = 1024;
        using WeaponFxSlot = replay::ring::Slot<WeaponFxCall>;
        WeaponFxSlot g_weaponFx[kWeaponFxRing];
        std::atomic<std::uint64_t> g_nextWeaponFx{0};

        void PushWeaponFx(const WeaponFxCall& call)
        {
            replay::ring::Writer<WeaponFxCall> slot(g_weaponFx, kWeaponFxRing, g_nextWeaponFx);
            slot.value = call;
            slot.Publish();
        }

        void* __fastcall MuzzleRequestDetour(void* manager, void* shooter, void* weapon, void* object, std::int32_t bone,
            const void* info, const void* vector, std::int32_t argA)
        {
            replay::lifetime::DetourScope lifetime;
            struct InFlight
            {
                InFlight() { g_inFlight.fetch_add(1, std::memory_order_seq_cst); }
                ~InFlight() { g_inFlight.fetch_sub(1, std::memory_order_seq_cst); }
            } inFlight;
            {
                WeaponFxCall call;
                call.kind = 1;
                call.shooter = reinterpret_cast<std::uint64_t>(shooter);
                call.weapon = reinterpret_cast<std::uint64_t>(weapon);
                call.object = reinterpret_cast<std::uint64_t>(object);
                call.bone = bone;
                call.argA = argA;
                std::uint8_t head[12]{};
                if (info && replay::mem::Copy(head, info, sizeof(head)))
                {
                    std::memcpy(&call.fxHash, head, 4);
                    std::memcpy(&call.fxHashAlt, head + 4, 4);
                    std::memcpy(&call.range, head + 8, 4);
                    call.infoRead = true;
                }
                if (vector)
                    replay::mem::Copy(call.vec, vector, sizeof(call.vec));
                call.threadId = GetCurrentThreadId();
                PushWeaponFx(call);
            }
            const MuzzleRequestFn original = g_originalMuzzleRequest.load(std::memory_order_acquire);
            return original ? original(manager, shooter, weapon, object, bone, info, vector, argA) : nullptr;
        }

        void* __fastcall ShellRequestDetour(void* manager, void* shooter, void* weapon, void* object, std::int32_t argA,
            std::int32_t bone)
        {
            replay::lifetime::DetourScope lifetime;
            struct InFlight
            {
                InFlight() { g_inFlight.fetch_add(1, std::memory_order_seq_cst); }
                ~InFlight() { g_inFlight.fetch_sub(1, std::memory_order_seq_cst); }
            } inFlight;
            {
                WeaponFxCall call;
                call.kind = 2;
                call.shooter = reinterpret_cast<std::uint64_t>(shooter);
                call.weapon = reinterpret_cast<std::uint64_t>(weapon);
                call.object = reinterpret_cast<std::uint64_t>(object);
                call.bone = bone;
                call.argA = argA;
                call.threadId = GetCurrentThreadId();
                PushWeaponFx(call);
            }
            const ShellRequestFn original = g_originalShellRequest.load(std::memory_order_acquire);
            return original ? original(manager, shooter, weapon, object, argA, bone) : nullptr;
        }
    }

    bool InstallWeaponFxIntent(std::string& error)
    {
        if (g_muzzleRequestTarget && g_shellRequestTarget)
            return true;
        // Both signatures first: neither is hooked when one is missing.
        const auto missing = [&error](const sig::Signature& signature, const char* name) {
            if (sig::Find(signature))
                return false;
            char text[96]{};
            std::snprintf(text, sizeof(text), "%s_sig_mismatch,rva=0x%llx", name,
                static_cast<unsigned long long>(signature.rva));
            error = text;
            return true;
        };
        if (missing(kFxMuzzleRequest, "muzzle_request") || missing(kFxShellRequest, "shell_request"))
            return false;
        void* trampoline = nullptr;
        if (!g_muzzleRequestTarget)
        {
            if (!InstallHookAt(kFxMuzzleRequest, reinterpret_cast<void*>(&MuzzleRequestDetour), &trampoline,
                    &g_muzzleRequestTarget, "muzzle_request", error))
                return false;
            g_originalMuzzleRequest.store(reinterpret_cast<MuzzleRequestFn>(trampoline), std::memory_order_release);
        }
        trampoline = nullptr;
        if (!g_shellRequestTarget)
        {
            if (!InstallHookAt(kFxShellRequest, reinterpret_cast<void*>(&ShellRequestDetour), &trampoline,
                    &g_shellRequestTarget, "shell_request", error))
                return false;
            g_originalShellRequest.store(reinterpret_cast<ShellRequestFn>(trampoline), std::memory_order_release);
        }
        return true;
    }

    void CollectWeaponFxSince(std::uint64_t& cursor, std::vector<WeaponFxCall>& out)
    {
        out.clear();
        replay::ring::ForEachSince(g_weaponFx, kWeaponFxRing, g_nextWeaponFx, cursor, [&](const WeaponFxCall& call) { out.push_back(call); });
    }

    std::uint64_t TotalWeaponFxCalls() { return g_nextWeaponFx.load(std::memory_order_acquire); }

    // One recorded request played back through the original wrapper.
    namespace
    {
        // The fault's code and the address that faulted.
        bool WeaponFxFault(std::uint32_t& code, std::uint64_t& rip)
        {
            rip = engine::LastFaultAddress();
            return engine::Failed(code);
        }

        bool GuardedMuzzleCall(MuzzleRequestFn fn, void* manager, void* shooter, void* weapon, void* object, std::int32_t bone,
            const void* info, const void* vector, std::int32_t argA, std::uint32_t& code, std::uint64_t& rip)
        {
            t_replayRegistered = true;
            void* request = nullptr;
            const bool called = engine::Call(engine::kFxMuzzleRequest, reinterpret_cast<std::uintptr_t>(fn), request, manager,
                shooter, weapon, object, bone, info, vector, argA);
            t_replayRegistered = false;
            return called || WeaponFxFault(code, rip);
        }

        bool GuardedShellCall(ShellRequestFn fn, void* manager, void* shooter, void* weapon, void* object, std::int32_t argA,
            std::int32_t bone, std::uint32_t& code, std::uint64_t& rip)
        {
            t_replayRegistered = true;
            void* request = nullptr;
            const bool called = engine::Call(engine::kFxShellRequest, reinterpret_cast<std::uintptr_t>(fn), request, manager,
                shooter, weapon, object, argA, bone);
            t_replayRegistered = false;
            return called || WeaponFxFault(code, rip);
        }
    }

    bool CallWeaponFxRequest(std::uint8_t kind, void* shooter, void* weapon, void* object, std::int32_t bone,
        const void* info, const float* vec16, std::int32_t argA, WeaponFxPlayResult& out)
    {
        out = WeaponFxPlayResult{};
        if (!g_muzzleRequestTarget || !g_shellRequestTarget)
        {
            out.why = "not_installed";
            return false;
        }
        if (!weapon || !object || bone < 0 || (kind == 1 && (!info || !vec16)) || (kind != 1 && kind != 2))
        {
            out.why = "bad_args";
            return false;
        }
        const MuzzleRequestFn muzzle = g_originalMuzzleRequest.load(std::memory_order_acquire);
        const ShellRequestFn shell = g_originalShellRequest.load(std::memory_order_acquire);
        if ((kind == 1 && !muzzle) || (kind == 2 && !shell))
        {
            out.why = "no_trampoline";
            return false;
        }
        struct InFlight
        {
            InFlight() { g_inFlight.fetch_add(1, std::memory_order_seq_cst); }
            ~InFlight() { g_inFlight.fetch_sub(1, std::memory_order_seq_cst); }
        } inFlight;
        // The manager both request callers pass (weapon_fx_intent_core.h).
        static const std::uintptr_t managerAddress = sig::Find(replay::weaponfx::kWeaponFxManager);
        if (!managerAddress)
        {
            out.why = "no_manager";
            return false;
        }
        void* manager = reinterpret_cast<void*>(managerAddress);
        // movaps reads the vector (the muzzle request): 16-byte aligned.
        alignas(16) float vector[4] = {};
        if (vec16)
            std::memcpy(vector, vec16, sizeof(vector));
        std::uint64_t startCursor = g_nextStart.load(std::memory_order_acquire);
        std::uint64_t registeredCursor = g_nextRegistered.load(std::memory_order_acquire);
        out.called = true;
        const bool ok = kind == 1
            ? GuardedMuzzleCall(muzzle, manager, shooter, weapon, object, bone, info, vector, argA, out.code, out.rip)
            : GuardedShellCall(shell, manager, shooter, weapon, object, argA, bone, out.code, out.rip);
        if (!ok)
        {
            out.faulted = true;
            if (g_base && out.rip >= g_base && out.rip < g_imageEnd)
                out.rip -= g_base;
            out.why = "exception";
        }
        const DWORD thread = GetCurrentThreadId();
        static std::vector<FxStart> starts;
        CollectStartsSince(startCursor, starts);
        for (const FxStart& start : starts)
            if (start.threadId == thread)
            {
                ++out.starts;
                out.startsMuzzle += start.callerSite == Sites().muzzle ? 1 : 0;
            }
        static std::vector<RegisteredCall> gets;
        CollectRegisteredSince(registeredCursor, gets);
        for (const RegisteredCall& get : gets)
            if (get.threadId == thread && get.caller == Sites().shell)
            {
                ++out.shellGets;
                out.shellCreated += get.created && get.inst ? 1 : 0;
            }
        if (ok)
            out.why = "ok";
        return ok;
    }

    void Shutdown()
    {
        g_enabled.store(false, std::memory_order_relaxed);
        g_pauseFrozen.store(false, std::memory_order_relaxed);
        g_pauseFrozenUntilMs.store(0, std::memory_order_relaxed);
        if (g_vfxUpdateTarget)
        {
            MH_DisableHook(g_vfxUpdateTarget);
            MH_RemoveHook(g_vfxUpdateTarget);
            g_vfxUpdateTarget = nullptr;
        }
        for (void** target : {&g_muzzleRequestTarget, &g_shellRequestTarget})
            if (*target)
            {
                MH_DisableHook(*target);
                MH_RemoveHook(*target);
                *target = nullptr;
            }
        g_suppressedCount.store(0, std::memory_order_release);
        g_suppressFootVfx.store(false, std::memory_order_relaxed);
        if (g_registeredTarget)
        {
            MH_DisableHook(g_registeredTarget);
            MH_RemoveHook(g_registeredTarget);
            g_registeredTarget = nullptr;
        }
        if (g_attachTarget)
        {
            MH_DisableHook(g_attachTarget);
            MH_RemoveHook(g_attachTarget);
            g_attachTarget = nullptr;
        }
        if (g_startTarget)
        {
            MH_DisableHook(g_startTarget);
            MH_RemoveHook(g_startTarget);
            g_startTarget = nullptr;
        }
        if (g_installed && g_target)
        {
            MH_DisableHook(g_target);
            MH_RemoveHook(g_target);
            g_installed = false;
        }
        for (int wait = 0; wait < 200 && g_inFlight.load(std::memory_order_seq_cst) > 0; ++wait)
            Sleep(1);
        g_original.store(nullptr, std::memory_order_release);
        g_originalAttach.store(nullptr, std::memory_order_release);
        g_originalRegistered.store(nullptr, std::memory_order_release);
        g_originalVfxUpdate.store(nullptr, std::memory_order_release);
        g_originalMuzzleRequest.store(nullptr, std::memory_order_release);
        g_originalShellRequest.store(nullptr, std::memory_order_release);
    }
}
