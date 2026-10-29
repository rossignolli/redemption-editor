#include <intrin.h> // _ReturnAddress for the read probe
#include "fx_observer.h"
#include "entity_events.h" // ComposeFxWorld
#include "guarded_memory.h"
#include "mod_clock.h"
#include "profiler.h"
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
        // Build 1491.50: (const u32* effectHash, const u32* assetHash) ->
        // effect instance or null. Reached by the script trigger path and,
        // through a one-argument wrapper, by 71 call sites of the vfx code.
        constexpr std::uintptr_t kTriggerRva = 0x2C3A57C;
        constexpr std::uint8_t kTriggerSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, // mov [rsp+8], rbx
            0x57,                         // push rdi
            0x48, 0x83, 0xEC, 0x20,       // sub rsp, 20h
            0x48, 0x8B, 0xD9,             // mov rbx, rcx
            0x48, 0x8B, 0xFA,             // mov rdi, rdx
        };

        using TriggerFn = void*(__fastcall*)(const std::uint32_t*, const std::uint32_t*);

        // Build 1491.50 fx start, called only by the particle native path
        // (0x2C50E0C, after the name is hashed and the rotation turned into
        // a matrix): (effect*, asset*, entity, bone, matrix*, zoom, flags).
        constexpr std::uintptr_t kStartRva = 0x2C50C88;
        constexpr std::uint8_t kStartSignature[] = {
            0x4C, 0x8B, 0xDC,             // mov r11, rsp
            0x49, 0x89, 0x5B, 0x18,       // mov [r11+18h], rbx
            0x49, 0x89, 0x73, 0x20,       // mov [r11+20h], rsi
            0x57,                         // push rdi
            0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, // sub rsp, 80h
        };
        using StartFn = bool(__fastcall*)(
            const std::uint32_t*, const std::uint32_t*, void*, int, const void*, float, std::uint8_t);
        StartFn g_start = nullptr;

        // The weapon muzzle path (build 1491.50, from 0x14523E0).
        constexpr std::uintptr_t kTriggeredInstRva = 0x2C3A624; // (effect*) -> inst
        constexpr std::uint8_t kTriggeredInstSignature[] = {
            0x48, 0x83, 0xEC, 0x28, 0x8B, 0x01, 0x48, 0x8D, 0x54, 0x24, 0x30};
        constexpr std::uintptr_t kAttachRva = 0x2C2FD40; // (inst, entity, bone, 0, u8, u8)
        // The attach core every attach path ends in (the wrapper above calls
        // it with r9d = -1): observed for the entity and bone of an instance.
        constexpr std::uintptr_t kAttachCoreRva = 0x2C2FB60;
        constexpr std::uint8_t kAttachCoreSignature[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8B, 0xEC};
        using AttachCoreFn = void(__fastcall*)(void*, void*, int, int, std::uint64_t, int, std::uint8_t, std::uint8_t);
        constexpr std::uint8_t kAttachSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,
            0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x40};
        constexpr std::uintptr_t kSetOffsetPosRva = 0x29E42A0; // inst+280h = *vec
        constexpr std::uint8_t kSetOffsetPosSignature[] = {
            0xF3, 0x0F, 0x6F, 0x02, 0x0F, 0x11, 0x81, 0x80, 0x02, 0x00, 0x00, 0xC3};
        constexpr std::uintptr_t kSetMatrixRva = 0x29E4274; // (inst, matrix*): inst+250h..28Fh
        constexpr std::uint8_t kSetMatrixSignature[] = {
            0x0F, 0x10, 0x02, 0x0F, 0x11, 0x81, 0x50, 0x02, 0x00, 0x00, 0x0F, 0x10, 0x4A, 0x10, 0x0F, 0x11};
        constexpr std::uintptr_t kSetParentRva = 0x29E2754; // (inst, out vec*, matrix*): inst+210h..24Fh
        constexpr std::uint8_t kSetParentSignature[] = {
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x41};
        using SetMatrixFn = void(__fastcall*)(void*, const void*);
        using SetParentFn = void*(__fastcall*)(void*, void*, const void*);
        // Registered effect get-or-create core (owner, id, arg3, flag, hash*,
        // asset*, created out*, u8, u8) -> inst; key = owner + id.
        constexpr std::uintptr_t kRegisteredRva = 0x2C49B08;
        constexpr std::uint8_t kRegisteredSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
            0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x40};
        using RegisteredFn = void*(__fastcall*)(void*, int, void*, std::uint8_t, std::uint32_t*, std::uint32_t*,
            std::uint8_t*, std::uint8_t, std::uint8_t);
        constexpr std::uintptr_t kAddPlanesRva = 0x29E41E0; // (inst, list*: count at +0x300, planes at +0x20)
        constexpr std::uint8_t kAddPlanesSignature[] = {
            0x48, 0x8B, 0x05, 0x09, 0x51, 0x5A, 0x02, 0x44, 0x0F, 0xB6, 0x48, 0x01, 0x49, 0x8D, 0x41, 0x03};
        constexpr std::uintptr_t kSetAux350Rva = 0x2C4F508; // (inst, vec*)
        constexpr std::uint8_t kSetAux350Signature[] = {0x80, 0x3D, 0x4B, 0x7D, 0x09, 0x01, 0x00, 0x75, 0x0B, 0xF3, 0x0F, 0x6F};
        constexpr std::uintptr_t kQualityGlobalRva = 0x4F892F0; // pointer; byte +1 = quality level
        using AddPlanesFn = void(__fastcall*)(void*, const void*);
        using SetAux350Fn = void(__fastcall*)(void*, const void*);
        constexpr std::uintptr_t kInstStartRva = 0x29E7644; // (inst)
        constexpr std::uint8_t kInstStartSignature[] = {
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18,
            0x48, 0x89, 0x78, 0x20, 0x55, 0x41, 0x56, 0x41, 0x57};
        constexpr std::size_t kInstFlags = 0x580;
        constexpr std::size_t kInstZoom = 0x394;

        using TriggeredInstFn = std::uint8_t*(__fastcall*)(const std::uint32_t*);
        using AttachFn = void(__fastcall*)(void*, void*, int, int, std::uint8_t, std::uint8_t);
        using SetOffsetPosFn = void(__fastcall*)(void*, const void*);
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

        TriggeredInstFn g_triggeredInst = nullptr;
        AttachFn g_attach = nullptr;
        SetOffsetPosFn g_setOffsetPos = nullptr;
        InstStartFn g_instStart = nullptr;
        SetMatrixFn g_setMatrix = nullptr;
        SetParentFn g_setParent = nullptr;
        AddPlanesFn g_addPlanes = nullptr;
        SetAux350Fn g_setAux350 = nullptr;

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
                // frames[0] returns into the one-argument wrapper 0x2C3A624,
                // frames[1] into its caller (the site).
                captured = RtlCaptureStackBackTrace(1, kCallerDepth + 1, frames, nullptr);
                refused = captured >= 2 && reinterpret_cast<std::uintptr_t>(frames[1]) >= g_base &&
                    reinterpret_cast<std::uintptr_t>(frames[1]) - g_base == kFootVfxSite;
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
            if (result && (watchHash ? effect == watchHash : call.callers[1] == kMuzzleFlashSite))
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
                // frames[0] returns into the wrapper 0x2C3A2C0; its caller is the site.
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
        using KillFn = void(__fastcall*)(void*, std::uint8_t);
        std::atomic<KillFn> g_killFn{nullptr};
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
            if ((start.callerSite == kMuzzleFlashSite || start.callerSite == kBulletImpactSite) &&
                g_weaponSitesExempt.load(std::memory_order_relaxed))
                return false;
            if (ExemptHash(start.effectHash))
                return false;
            if (start.callerSite && WorldSite(start.callerSite))
                return true;
            return start.entity && IsSuppressedOwner(reinterpret_cast<const void*>(start.entity));
        }

        bool GuardedKill(KillFn kill, void* inst)
        {
            __try
            {
                kill(inst, 1);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
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
                const KillFn killFn = g_killFn.load(std::memory_order_acquire);
                if (killFn && GuardedKill(killFn, inst))
                    g_oneShotKilled.fetch_add(1, std::memory_order_relaxed);
            }
        }

        bool GuardedCompare(const void* target, const std::uint8_t* signature, std::size_t size)
        {
            return replay::mem::Matches(target, signature, size); // guarded_memory.h
        }
    }

    bool Install(std::string& error)
    {
        if (g_installed)
            return true;
        QueryPerformanceFrequency(&g_frequency);
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        g_base = reinterpret_cast<std::uintptr_t>(base);
        g_imageEnd = g_base + nt->OptionalHeader.SizeOfImage;
        void* target = base + kTriggerRva;
        if (!GuardedCompare(target, kTriggerSignature, sizeof(kTriggerSignature)))
        {
            error = "fx_trigger_signature_mismatch";
            return false;
        }
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
        void* startTarget = base + kInstStartRva;
        if (GuardedCompare(startTarget, kInstStartSignature, sizeof(kInstStartSignature)))
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
        void* attachTarget = base + kAttachCoreRva;
        if (GuardedCompare(attachTarget, kAttachCoreSignature, sizeof(kAttachCoreSignature)))
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
        void* registeredTarget = base + kRegisteredRva;
        if (GuardedCompare(registeredTarget, kRegisteredSignature, sizeof(kRegisteredSignature)))
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
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        void* target = base + kStartRva;
        if (!GuardedCompare(target, kStartSignature, sizeof(kStartSignature)))
        {
            error = "fx_start_signature_mismatch";
            return false;
        }
        struct Check
        {
            std::uintptr_t rva;
            const std::uint8_t* signature;
            std::size_t size;
            const char* name;
        };
        const Check checks[] = {
            {kTriggeredInstRva, kTriggeredInstSignature, sizeof(kTriggeredInstSignature), "triggered_inst"},
            {kAttachRva, kAttachSignature, sizeof(kAttachSignature), "attach"},
            {kSetOffsetPosRva, kSetOffsetPosSignature, sizeof(kSetOffsetPosSignature), "set_offset_pos"},
            {kInstStartRva, kInstStartSignature, sizeof(kInstStartSignature), "inst_start"},
            {kSetMatrixRva, kSetMatrixSignature, sizeof(kSetMatrixSignature), "set_matrix"},
            {kSetParentRva, kSetParentSignature, sizeof(kSetParentSignature), "set_parent"},
        };
        for (const Check& check : checks)
        {
            // Already verified before the start observer hooked it.
            if (check.rva == kInstStartRva && g_startTarget)
                continue;
            if (!GuardedCompare(base + check.rva, check.signature, check.size))
            {
                error = std::string("fx_") + check.name + "_signature_mismatch";
                return false;
            }
        }
        g_triggeredInst = reinterpret_cast<TriggeredInstFn>(base + kTriggeredInstRva);
        g_attach = reinterpret_cast<AttachFn>(base + kAttachRva);
        g_setOffsetPos = reinterpret_cast<SetOffsetPosFn>(base + kSetOffsetPosRva);
        g_instStart = reinterpret_cast<InstStartFn>(base + kInstStartRva);
        g_setMatrix = reinterpret_cast<SetMatrixFn>(base + kSetMatrixRva);
        g_setParent = reinterpret_cast<SetParentFn>(base + kSetParentRva);
        // Optional (fire): checked here, missing ones logged by ApplyInstancePlanes.
        if (GuardedCompare(base + kAddPlanesRva, kAddPlanesSignature, sizeof(kAddPlanesSignature)))
            g_addPlanes = reinterpret_cast<AddPlanesFn>(base + kAddPlanesRva);
        if (GuardedCompare(base + kSetAux350Rva, kSetAux350Signature, sizeof(kSetAux350Signature)))
            g_setAux350 = reinterpret_cast<SetAux350Fn>(base + kSetAux350Rva);
        g_start = reinterpret_cast<StartFn>(target);
        return true;
    }

    namespace
    {
        bool GuardedStart(
            std::uint32_t effect, void* entity, int bone, const void* matrix, float scale,
            bool& started, std::uint32_t& code)
        {
            const std::uint32_t asset = 0; // any loaded asset, as the vfx code does
            __try
            {
                started = g_start(&effect, &asset, entity, bone, matrix, scale, 0);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
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
                std::uint8_t* inst = g_triggeredInst(&effect);
                if (!inst)
                    return true;
                g_attach(inst, entity, bone, 0, 0, 0);
                std::uint32_t& flags = *reinterpret_cast<std::uint32_t*>(inst + kInstFlags);
                flags = (flags & ~kMuzzleFlagMask) | (recorded & kMuzzleFlagMask);
                alignas(16) const float offset[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                g_setOffsetPos(inst, offset);
                *reinterpret_cast<float*>(inst + kInstZoom) = scale;
                if (!g_watchHash.load(std::memory_order_relaxed) && !g_replaySnapshotReady.load(std::memory_order_relaxed))
                {
                    std::memcpy(g_replaySnapshot, inst, kSnapshotBytes);
                    g_replaySnapshotReady.store(true, std::memory_order_release);
                }
                g_instStart(inst);
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
                std::uint8_t* inst = g_triggeredInst(&effect);
                if (!inst)
                    return true;
                if (entity)
                    g_attach(inst, entity, bone, 0, 0, 0);
                g_setMatrix(inst, offset);
                if (!entity)
                {
                    alignas(16) float delta[4] = {};
                    g_setParent(inst, delta, parent);
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
                            g_setAux350(inst, aux);
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
                g_instStart(inst);
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
        bool GuardedRegisteredGet(RegisteredFn fn, const void* owner, int id, std::uint8_t flag, std::uint32_t hash,
            std::uint8_t arg8, std::uint8_t arg9, std::uint64_t& inst, std::uint8_t& created, std::uint32_t& code)
        {
            __try
            {
                std::uint32_t hashCopy = hash;
                std::uint32_t asset = 0;
                std::uint8_t createdOut = 0;
                t_replayRegistered = true;
                void* result = fn(const_cast<void*>(owner), id, nullptr, flag, &hashCopy, &asset, &createdOut, arg8, arg9);
                t_replayRegistered = false;
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
                g_setMatrix(inst, matrix);
                alignas(16) float delta[4] = {};
                if (writeParent)
                    g_setParent(inst, delta, parent);
                std::memcpy(inst + kInstEvolutions, evolutions, kEvolutionBytes);
                *reinterpret_cast<float*>(inst + kInstZoom) = zoom;
                if (start)
                    g_instStart(inst);
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
        RegisteredFn fn = g_originalRegistered.load(std::memory_order_acquire);
        if (!fn)
        {
            auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
            if (!GuardedCompare(base + kRegisteredRva, kRegisteredSignature, sizeof(kRegisteredSignature)))
            {
                error = "fx_registered_signature_mismatch";
                return 0;
            }
            fn = reinterpret_cast<RegisteredFn>(base + kRegisteredRva);
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
                if (aux350)
                    g_setAux350(inst, aux350);
                if (planeCount)
                {
                    // The list the engine reads (0x29E41E0: r8 = list + 0x20, rows at
                    // r8 - 0x20, r8 - 0x10, r8): planes from +0, 3 float4 each, count at +0x300.
                    alignas(16) std::uint8_t list[0x320] = {};
                    std::memcpy(list, planes, planeCount * 12 * sizeof(float));
                    std::memcpy(list + 0x300, &planeCount, 4);
                    g_addPlanes(inst, list);
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
            // Evidence for the mismatch: the live bytes at both entries.
            auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
            std::uint8_t live[16] = {};
            char text[160]{};
            int n = std::snprintf(text, sizeof(text), "planes_functions_unavailable,add_planes=");
            if (GuardedRead(live, base + kAddPlanesRva, 16))
                for (std::uint8_t byte : live)
                    n += std::snprintf(text + n, sizeof(text) - n, "%02x", byte);
            n += std::snprintf(text + n, sizeof(text) - n, ",set_aux=");
            if (GuardedRead(live, base + kSetAux350Rva, 12))
                for (int i = 0; i < 12; ++i)
                    n += std::snprintf(text + n, sizeof(text) - n, "%02x", live[i]);
            error = text;
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
        if (!GuardedWriteExtras(reinterpret_cast<std::uint8_t*>(inst), start310, farClip))
        {
            error = "extras_write_fault";
            return false;
        }
        return true;
    }

    namespace
    {
        // The instance stop (offline 0x29E7C3C, build 1491.50).
        constexpr std::uintptr_t kInstStopRva = 0x29E7C3C;
        constexpr std::uint8_t kInstStopSignature[] = {
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48,
            0x89, 0x78, 0x20, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0xF9, 0x33, 0xDB, 0x48, 0x8B,
            0x0D, 0x37, 0x5A, 0x4D, 0x01, 0x44, 0x8A, 0xF2};
        using InstStopFn = void(__fastcall*)(void*, std::uint8_t);

        bool GuardedStop(InstStopFn stop, std::uint64_t inst, std::uint8_t immediate, std::uint32_t& code)
        {
            __try
            {
                stop(reinterpret_cast<void*>(inst), immediate);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
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
        static int verified = -1;
        static InstStopFn stop = nullptr;
        if (verified < 0)
        {
            auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
            verified = GuardedCompare(base + kInstStopRva, kInstStopSignature, sizeof(kInstStopSignature)) ? 1 : 0;
            if (verified)
                stop = reinterpret_cast<InstStopFn>(base + kInstStopRva);
        }
        if (!verified || !stop)
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
            auto* module = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
            if (GuardedRead(pointer, module + kQualityGlobalRva, 8))
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
                g_attach(inst, entity, bone, 0, 0, 0);
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
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        if (!GuardedCompare(base + kInstStopRva, kInstStopSignature, sizeof(kInstStopSignature)))
        {
            error = "fx_stop_signature_mismatch";
            g_oneShotOwnerRule.store(false, std::memory_order_release);
            return false;
        }
        g_killFn.store(reinterpret_cast<KillFn>(base + kInstStopRva), std::memory_order_release);
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
            // One byte. +0x3BC is a byte (read as one at 0x29C8844, the site
            // 0x145441C writes one); +0x3BD is the instance update's own
            // (zeroed at 0x29ED5D9 in 0x29ED368, tested with +0x340 at
            // 0x29F3C22) [binary]: a recorded value of a live instance would
            // put that runtime byte back on ours.
            if (s.mask & entity::FxSettings::kCull3BC)
                inst[0x3BC] = static_cast<std::uint8_t>(s.cull3BC & 0xFFu);
        }

        bool GuardedWriteSettings(std::uint8_t* inst, const entity::FxSettings& s, const float* aux, std::uint32_t& code)
        {
            __try
            {
                if (aux)
                    g_setAux350(inst, aux);
                WriteSettingsRaw(inst, s);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // The particle manager's lock (see ClearRelocateMarks).
        constexpr std::uintptr_t kPtfxManagerRva = 0x3EBD698;
        constexpr std::size_t kPtfxLockOffset = 0x3E280;
        constexpr std::uintptr_t kParentLockLoadRva = 0x29E2773; // mov rcx, [rip+0x14DAF1E] -> 0x3EBD698
        constexpr std::uint8_t kParentLockLoadSignature[] = {0x48, 0x8B, 0x0D, 0x1E, 0xAF, 0x4D, 0x01};
        constexpr std::uintptr_t kLockOwnedRva = 0x29DB42C;
        constexpr std::uint8_t kLockOwnedSignature[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10,
            0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x41, 0x56, 0x48, 0x83, 0xEC};
        constexpr std::uintptr_t kLockRva = 0x25A1658;
        constexpr std::uintptr_t kUnlockRva = 0x25A5694;
        // sub rsp,28h; cmp qword [rcx],0; je +6; call [rip+rel32] (the import, free); add rsp,28h; ret
        constexpr std::uint8_t kLockHead[] = {0x48, 0x83, 0xEC, 0x28, 0x48, 0x83, 0x39, 0x00, 0x74, 0x06, 0xFF, 0x15};
        constexpr std::uint8_t kLockTail[] = {0x48, 0x83, 0xC4, 0x28, 0xC3};
        using LockOwnedFn = bool(__fastcall*)(void*);
        using LockFn = void(__fastcall*)(void*);
        int g_relocateLockChecked = -1;
        LockOwnedFn g_lockOwned = nullptr;
        LockFn g_lock = nullptr;
        LockFn g_unlock = nullptr;
        std::string g_relocateLockError;

        bool CheckRelocateLock()
        {
            if (g_relocateLockChecked >= 0)
                return g_relocateLockChecked == 1;
            auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
            const auto lockShape = [&](std::uintptr_t rva) {
                return GuardedCompare(base + rva, kLockHead, sizeof(kLockHead)) &&
                    GuardedCompare(base + rva + sizeof(kLockHead) + 4, kLockTail, sizeof(kLockTail));
            };
            char text[64]{};
            if (!GuardedCompare(base + kParentLockLoadRva, kParentLockLoadSignature, sizeof(kParentLockLoadSignature)))
                std::snprintf(text, sizeof(text), "sig_mismatch,rva=0x%llx", static_cast<unsigned long long>(kParentLockLoadRva));
            else if (!GuardedCompare(base + kLockOwnedRva, kLockOwnedSignature, sizeof(kLockOwnedSignature)))
                std::snprintf(text, sizeof(text), "sig_mismatch,rva=0x%llx", static_cast<unsigned long long>(kLockOwnedRva));
            else if (!lockShape(kLockRva))
                std::snprintf(text, sizeof(text), "sig_mismatch,rva=0x%llx", static_cast<unsigned long long>(kLockRva));
            else if (!lockShape(kUnlockRva))
                std::snprintf(text, sizeof(text), "sig_mismatch,rva=0x%llx", static_cast<unsigned long long>(kUnlockRva));
            if (text[0])
            {
                g_relocateLockError = text;
                g_relocateLockChecked = 0;
                return false;
            }
            g_lockOwned = reinterpret_cast<LockOwnedFn>(base + kLockOwnedRva);
            g_lock = reinterpret_cast<LockFn>(base + kLockRva);
            g_unlock = reinterpret_cast<LockFn>(base + kUnlockRva);
            g_relocateLockChecked = 1;
            return true;
        }

        // The owner test takes the MANAGER, not the lock object: 0x29E2773
        // loads [0x3EBD698] into rcx, 0x29E277F only copies manager+0x3E280
        // into rbx for the lock calls, and 0x29E2786 calls 0x29DB42C with rcx
        // still the manager; 0x29DB42C compares [rcx+0x3E378] with the
        // thread's TLS slot +0x650 (and scans the worker list 0x3CA7B10)
        // [binary].
        bool GuardedLockEnter(void* manager, void* lock, bool& taken, std::uint32_t& code)
        {
            taken = false;
            __try
            {
                if (!g_lockOwned(manager))
                {
                    g_lock(lock);
                    taken = true;
                }
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool GuardedLockLeave(void* lock, std::uint32_t& code)
        {
            __try
            {
                g_unlock(lock);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
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
        constexpr std::uintptr_t kVfxUpdateRva = 0x145C17C;
        constexpr std::uint8_t kVfxUpdateSignature[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10,
            0x48, 0x89, 0x70, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x0F, 0x29, 0x70, 0xE8};
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

        bool InstallHookAt(std::uintptr_t rva, const std::uint8_t* signature, std::size_t size, void* detour, void** trampoline,
            void** target, const char* name, std::string& error)
        {
            if (*target)
                return true;
            auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
            void* at = base + rva;
            if (!GuardedCompare(at, signature, size))
            {
                char text[96]{};
                std::snprintf(text, sizeof(text), "%s_sig_mismatch,rva=0x%llx", name, static_cast<unsigned long long>(rva));
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
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        std::uint64_t manager = 0;
        if (!GuardedRead(reinterpret_cast<std::uint8_t*>(&manager), base + kPtfxManagerRva, 8) || !manager)
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
        if (!InstallHookAt(kVfxUpdateRva, kVfxUpdateSignature, sizeof(kVfxUpdateSignature),
                reinterpret_cast<void*>(&VfxUpdateDetour), &trampoline, &g_vfxUpdateTarget, "vfx_update", error))
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
        // Build 1491.50 [binary]: the muzzle request (one caller, 0x11479FC,
        // after the chance roll 0x1147648) and the shell request (one caller,
        // 0x110142C, from a queued record {shooter, weapon, object, a5, a6}).
        constexpr std::uintptr_t kMuzzleRequestRva = 0x14309A8;
        constexpr std::uint8_t kMuzzleRequestSignature[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10,
            0x48, 0x89, 0x70, 0x18, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x60};
        constexpr std::uintptr_t kShellRequestRva = 0x14308F0;
        constexpr std::uint8_t kShellRequestSignature[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10,
            0x48, 0x89, 0x70, 0x18, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x40};
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
        // Both signatures first: neither is hooked when one differs.
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        if (!GuardedCompare(base + kMuzzleRequestRva, kMuzzleRequestSignature, sizeof(kMuzzleRequestSignature)))
        {
            error = "muzzle_request_sig_mismatch,rva=0x14309a8";
            return false;
        }
        if (!GuardedCompare(base + kShellRequestRva, kShellRequestSignature, sizeof(kShellRequestSignature)))
        {
            error = "shell_request_sig_mismatch,rva=0x14308f0";
            return false;
        }
        void* trampoline = nullptr;
        if (!g_muzzleRequestTarget)
        {
            if (!InstallHookAt(kMuzzleRequestRva, kMuzzleRequestSignature, sizeof(kMuzzleRequestSignature),
                    reinterpret_cast<void*>(&MuzzleRequestDetour), &trampoline, &g_muzzleRequestTarget, "muzzle_request", error))
                return false;
            g_originalMuzzleRequest.store(reinterpret_cast<MuzzleRequestFn>(trampoline), std::memory_order_release);
        }
        trampoline = nullptr;
        if (!g_shellRequestTarget)
        {
            if (!InstallHookAt(kShellRequestRva, kShellRequestSignature, sizeof(kShellRequestSignature),
                    reinterpret_cast<void*>(&ShellRequestDetour), &trampoline, &g_shellRequestTarget, "shell_request", error))
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
        int CaptureWeaponFxFault(EXCEPTION_POINTERS* info, std::uint32_t& code, std::uint64_t& rip)
        {
            code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
            rip = info && info->ContextRecord ? info->ContextRecord->Rip : 0;
            return EXCEPTION_EXECUTE_HANDLER;
        }

        bool GuardedMuzzleCall(MuzzleRequestFn fn, void* manager, void* shooter, void* weapon, void* object, std::int32_t bone,
            const void* info, const void* vector, std::int32_t argA, std::uint32_t& code, std::uint64_t& rip)
        {
            __try
            {
                t_replayRegistered = true;
                fn(manager, shooter, weapon, object, bone, info, vector, argA);
                t_replayRegistered = false;
                return true;
            }
            __except (CaptureWeaponFxFault(GetExceptionInformation(), code, rip))
            {
                t_replayRegistered = false;
                return false;
            }
        }

        bool GuardedShellCall(ShellRequestFn fn, void* manager, void* shooter, void* weapon, void* object, std::int32_t argA,
            std::int32_t bone, std::uint32_t& code, std::uint64_t& rip)
        {
            __try
            {
                t_replayRegistered = true;
                fn(manager, shooter, weapon, object, argA, bone);
                t_replayRegistered = false;
                return true;
            }
            __except (CaptureWeaponFxFault(GetExceptionInformation(), code, rip))
            {
                t_replayRegistered = false;
                return false;
            }
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
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        void* manager = base + replay::weaponfx::kManagerRva;
        // movaps reads the vector (0x14309EE): 16-byte aligned.
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
                out.startsMuzzle += start.callerSite == kMuzzleFlashSite ? 1 : 0;
            }
        static std::vector<RegisteredCall> gets;
        CollectRegisteredSince(registeredCursor, gets);
        for (const RegisteredCall& get : gets)
            if (get.threadId == thread && get.caller == replay::weaponfx::kShellRegisteredSite)
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
