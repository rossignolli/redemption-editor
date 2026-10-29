#include "world_theatre_engine.h"
#include "audio_observer.h"
#include "speech_replay_rules.h"

#include "guarded_memory.h"
#include "mod_clock.h"

#include <windows.h>

#include <MinHook.h>
#include "hook_batch.h"
#include "module_lifetime.h"
#include "event_ring.h"

#include <intrin.h>

#include <atomic>
#include <cstring>
#include <initializer_list>

namespace replay::theatre
{
    namespace
    {
        using U = std::uint64_t;

        std::uintptr_t Base()
        {
            return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        }

        std::uint32_t ToRva(void* address)
        {
            const std::uintptr_t base = Base();
            const std::uintptr_t at = reinterpret_cast<std::uintptr_t>(address);
            return at >= base && at - base < 0x10000000u ? static_cast<std::uint32_t>(at - base) : 0u;
        }

        bool SignatureAt(const Signature& signature)
        {
            std::uint8_t code[sizeof(signature.bytes)]{};
            return replay::mem::Copy(code, Base() + signature.rva, sizeof(code)) &&
                SignatureMatches(signature, code, sizeof(code));
        }

        bool SiteAt(const Site& site)
        {
            std::uint8_t code[8]{};
            return SiteTarget(site) == site.target && replay::mem::Copy(code, Base() + site.rva, site.size) &&
                SiteMatches(site, code, site.size);
        }

        template <typename T>
        struct Ring
        {
            static constexpr std::uint64_t kSize = 512;
            replay::ring::Slot<T> slots[kSize]; // event_ring.h
            std::atomic<std::uint64_t> next{0};

            void Push(const T& value)
            {
                replay::ring::Writer<T> slot(slots, kSize, next);
                slot.value = value;
                slot.Publish();
            }

            void Collect(std::uint64_t& cursor, std::vector<T>& out)
            {
                out.clear();
                replay::ring::ForEachSince(slots, kSize, next, cursor, [&](const T& value) { out.push_back(value); });
            }
        };

        std::atomic<bool> g_owning{false};
        std::atomic<bool> g_deny{false};
        std::atomic<int> g_inFlight{0};

        struct InFlight
        {
            InFlight() { g_inFlight.fetch_add(1, std::memory_order_seq_cst); }
            ~InFlight() { g_inFlight.fetch_sub(1, std::memory_order_seq_cst); }
        };

        // The animal bank picker detour (end of file).
        void DisableAnimalBank();
        void ForgetAnimalBankOriginal();

        // ---- spawn funnel 0xBF23D4 (21 arguments, the RedM's layout:
        // a1 u16, a2 int, a3 source name, a4 model index, a5 float* position,
        // a6 float, then pointers and flags; all integer registers, the float
        // on the stack, so U slots forward them bit for bit).
        using SpawnFn = void*(__fastcall*)(U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U);
        std::atomic<SpawnFn> g_spawnOriginal{nullptr};
        void* g_spawnTarget = nullptr;
        Ring<SpawnCall> g_spawns;

        void RecordSpawn(U a3, U a4, U a5, void* returnAddress, bool denied)
        {
            SpawnCall call;
            call.timeUs = replay::clock::NowUs();
            char text[48]{};
            std::size_t size = sizeof(text) - 1;
            // A string of the .rdata: read until the copy fails or ends.
            while (size > 0 && !replay::mem::Copy(text, static_cast<std::uintptr_t>(a3), size))
                size /= 2;
            CopyLabel(text, size, call.label, sizeof(call.label));
            call.model = static_cast<std::int32_t>(a4 & 0xFFFFFFFFu);
            replay::mem::Copy(call.position, static_cast<std::uintptr_t>(a5), sizeof(call.position));
            call.caller = ToRva(returnAddress);
            call.denied = denied;
            g_spawns.Push(call);
        }

        void* __fastcall SpawnDetour(U a1, U a2, U a3, U a4, U a5, U a6, U a7, U a8, U a9, U a10, U a11, U a12, U a13,
            U a14, U a15, U a16, U a17, U a18, U a19, U a20, U a21)
        {
            replay::lifetime::DetourScope lifetime;
            InFlight inFlight;
            const bool owning = g_owning.load(std::memory_order_acquire);
            if (owning && g_deny.load(std::memory_order_acquire))
            {
                RecordSpawn(a3, a4, a5, _ReturnAddress(), true);
                return nullptr; // the RedM's populationPedCreating refusal
            }
            void* ped = nullptr;
            if (const SpawnFn original = g_spawnOriginal.load(std::memory_order_acquire))
                ped = original(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20, a21);
            if (owning)
                RecordSpawn(a3, a4, a5, _ReturnAddress(), false);
            return ped;
        }

        // ---- mix scene start 0x2F19018 (mixer, &hash, r8, r9, 3 on the stack)
        using SceneFn = U(__fastcall*)(U, U, U, U, U, U, U);
        std::atomic<SceneFn> g_sceneOriginal{nullptr};
        void* g_sceneTarget = nullptr;
        Ring<SceneCall> g_scenes;

        // The scene events (start and stop) for the
        // mix scene track and the speech level probe.
        std::atomic<bool> g_sceneObserve{false};
        Ring<MixSceneEvent> g_sceneEvents;
        // The refused start sites.
        std::atomic<std::uint32_t> g_sceneRefusals[kMaxSceneRefusals];
        std::atomic<std::uint32_t> g_refusalCount{0};
        std::atomic<std::uint64_t> g_refused{0};

        bool RefuseStart(void* returnAddress)
        {
            const std::uint32_t count = g_refusalCount.load(std::memory_order_acquire);
            if (!count || !g_owning.load(std::memory_order_acquire))
                return false;
            const std::uint32_t rva = ToRva(returnAddress);
            for (std::uint32_t n = 0; n < count && n < kMaxSceneRefusals; ++n)
                if (g_sceneRefusals[n].load(std::memory_order_relaxed) == rva)
                    return true;
            return false;
        }

        void RecordSceneStart(U hashPointer, U slot, void* returnAddress)
        {
            MixSceneEvent event;
            event.timeUs = replay::clock::NowUs();
            replay::mem::Copy(&event.hash, static_cast<std::uintptr_t>(hashPointer), sizeof(event.hash));
            event.slot = slot;
            if (slot)
                replay::mem::Copy(&event.scene, static_cast<std::uintptr_t>(slot), sizeof(event.scene));
            event.caller = ToRva(returnAddress);
            if (event.caller == kConversationSceneCaller && slot > kConversationSceneSlot)
            {
                const std::uintptr_t conversation = static_cast<std::uintptr_t>(slot) - kConversationSceneSlot;
                std::uint64_t parts = 0;
                std::uint16_t count = 0;
                if (replay::mem::Copy(&parts, conversation + kConversationParticipants, sizeof(parts)) &&
                    replay::mem::Copy(&count, conversation + kConversationParticipantCount, sizeof(count)) && parts)
                {
                    event.participants = count;
                    const std::uint32_t read = count < kParticipantReadMax ? count : kParticipantReadMax;
                    for (std::uint32_t n = 0; n < read && event.tagCount < 8; ++n)
                    {
                        const std::uintptr_t part = static_cast<std::uintptr_t>(parts) + n * kParticipantStride;
                        std::uint8_t flag = 0;
                        std::uint32_t tag = 0;
                        if (replay::mem::Copy(&flag, part + kParticipantFlag, 1) && flag &&
                            replay::mem::Copy(&tag, part + kParticipantTag, sizeof(tag)))
                            event.tags[event.tagCount++] = tag;
                    }
                }
            }
            g_sceneEvents.Push(event);
        }

        U __fastcall SceneDetour(U mixer, U hashPointer, U a3, U a4, U a5, U a6, U a7)
        {
            replay::lifetime::DetourScope lifetime;
            InFlight inFlight;
            if (RefuseStart(_ReturnAddress()))
            {
                g_refused.fetch_add(1, std::memory_order_relaxed);
                return 0; // as a full pool: no scene, the reference untouched
            }
            U result = 0;
            if (const SceneFn original = g_sceneOriginal.load(std::memory_order_acquire))
                result = original(mixer, hashPointer, a3, a4, a5, a6, a7);
            if (g_sceneObserve.load(std::memory_order_acquire))
                RecordSceneStart(hashPointer, a3, _ReturnAddress());
            if (g_owning.load(std::memory_order_acquire))
            {
                SceneCall call;
                call.timeUs = replay::clock::NowUs();
                replay::mem::Copy(&call.hash, static_cast<std::uintptr_t>(hashPointer), sizeof(call.hash));
                call.caller = ToRva(_ReturnAddress());
                g_scenes.Push(call);
            }
            return result;
        }

        // ---- mix scene stop 0x327A07C (scene, byte):
        // the hash read before the original (the stop may free the scene).
        using SceneStopFn = U(__fastcall*)(U, U);
        std::atomic<SceneStopFn> g_sceneStopOriginal{nullptr};
        void* g_sceneStopTarget = nullptr;

        U __fastcall SceneStopDetour(U scene, U immediate)
        {
            replay::lifetime::DetourScope lifetime;
            InFlight inFlight;
            if (scene && g_sceneObserve.load(std::memory_order_acquire))
            {
                MixSceneEvent event;
                event.timeUs = replay::clock::NowUs();
                event.scene = scene;
                event.stop = 1;
                replay::mem::Copy(&event.hash, static_cast<std::uintptr_t>(scene + kMixSceneHashOffset), sizeof(event.hash));
                event.caller = ToRva(_ReturnAddress());
                g_sceneEvents.Push(event);
            }
            U result = 0;
            if (const SceneStopFn original = g_sceneStopOriginal.load(std::memory_order_acquire))
                result = original(scene, immediate);
            return result;
        }

        // ---- speech state 3 rules 0x2F972C4 (speech entity, line context);
        // returns the refusal code (0 accepted). Forwarded first.
        using RulesFn = U(__fastcall*)(U, U, U, U);
        std::atomic<RulesFn> g_rulesOriginal{nullptr};
        void* g_rulesTarget = nullptr;
        Ring<RuleRefusal> g_refusals;

        // speech_replay_rules.h: the replay's lines and the
        // three world context codes they pass (SpeechReplayRules).
        std::atomic<bool> g_speechObserve{false}; // set while a playback runs
        std::atomic<bool> g_replayRules{true};
        replay::speech::rules::ReplayLines g_replayLines;
        std::atomic<std::uint32_t> g_rulesBypassed[3] = {}; // 28, 41, 48

        bool ReplayLine(U speechEntity, U line)
        {
            if (g_replayLines.Has(line) || replay::audio::ReplayEmittingHere())
                return true;
            return replay::audio::PuppetOwnSpeechBlock() && replay::audio::IsPuppetSpeechEntity(speechEntity);
        }

        U __fastcall RulesDetour(U speechEntity, U context, U a3, U a4)
        {
            replay::lifetime::DetourScope lifetime;
            InFlight inFlight;
            U result = 0;
            if (const RulesFn original = g_rulesOriginal.load(std::memory_order_acquire))
                result = original(speechEntity, context, a3, a4);
            {
                // The rules' second argument is the line (the
                // state 3 handler 0x2FC486C passes it in rdx with the line's
                // speech entity [line +0x1C0] in rcx).
                const std::uint32_t got = static_cast<std::uint32_t>(result & 0xFFFFFFFFu);
                if (replay::speech::rules::WorldContextCode(got) && g_speechObserve.load(std::memory_order_acquire))
                {
                    const std::uint32_t decided = replay::speech::rules::Decide(
                        g_replayRules.load(std::memory_order_relaxed), true, ReplayLine(speechEntity, context), got);
                    if (decided != got)
                    {
                        const int slot = got == replay::speech::rules::kOtherSpeaker ? 0
                            : got == replay::speech::rules::kOutOfRange ? 1 : 2;
                        g_rulesBypassed[slot].fetch_add(1, std::memory_order_relaxed);
                        return static_cast<U>(decided);
                    }
                }
            }
            const std::uint32_t code = static_cast<std::uint32_t>(result & 0xFFFFFFFFu);
            if (code && g_owning.load(std::memory_order_acquire))
            {
                RuleRefusal refusal;
                refusal.timeUs = replay::clock::NowUs();
                refusal.speechEntity = speechEntity;
                refusal.code = code;
                replay::mem::Copy(&refusal.ped, static_cast<std::uintptr_t>(speechEntity + kSpeechEntityPed), 8);
                replay::mem::Copy(&refusal.e3e0, static_cast<std::uintptr_t>(speechEntity + 0x3E0), 1);
                replay::mem::Copy(&refusal.b2, static_cast<std::uintptr_t>(speechEntity + 0xB2), 1);
                replay::mem::Copy(&refusal.b3, static_cast<std::uintptr_t>(speechEntity + 0xB3), 1);
                refusal.caller = ToRva(_ReturnAddress());
                g_refusals.Push(refusal);
            }
            return result;
        }

        // ---- speech state 3 handler 0x2FC486C (line) -> code; forwarded
        // first, a failure code recorded while the replay plays (the
        // "revalidation" deaths of mission lines, where the theater refuses
        // and the rules detour above records nothing).
        using State3Fn = U(__fastcall*)(U);
        std::atomic<State3Fn> g_state3Original{nullptr};
        void* g_state3Target = nullptr;
        Ring<State3Failure> g_state3Failures;

        U __fastcall State3Detour(U line)
        {
            replay::lifetime::DetourScope lifetime;
            InFlight inFlight;
            U result = 0;
            if (const State3Fn original = g_state3Original.load(std::memory_order_acquire))
                result = original(line);
            const std::uint32_t code = static_cast<std::uint32_t>(result & 0xFFFFFFFFu);
            if (SpeechCodeFailed(code) && g_speechObserve.load(std::memory_order_acquire))
            {
                State3Failure failure;
                failure.timeUs = replay::clock::NowUs();
                failure.line = line;
                failure.code = code;
                g_state3Failures.Push(failure);
            }
            return result;
        }

        template <typename Fn>
        bool Hook(const Signature& signature, void* detour, std::atomic<Fn>& original, void*& target, std::string& report)
        {
            report += signature.name;
            if (!SignatureAt(signature))
            {
                report += "=signature;";
                return false;
            }
            const MH_STATUS init = MH_Initialize();
            if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
            {
                report += "=mh_init;";
                return false;
            }
            void* at = reinterpret_cast<void*>(Base() + signature.rva);
            void* trampoline = nullptr;
            MH_STATUS status = MH_CreateHook(at, detour, &trampoline);
            if (status != MH_OK || !trampoline)
            {
                report += std::string("=mh_create_") + MH_StatusToString(status) + ";";
                return false;
            }
            original.store(reinterpret_cast<Fn>(trampoline), std::memory_order_release);
            status = replay::hooks::Enable(at);
            if (status != MH_OK)
            {
                MH_RemoveHook(at);
                original.store(nullptr, std::memory_order_release);
                report += std::string("=mh_enable_") + MH_StatusToString(status) + ";";
                return false;
            }
            target = at;
            report += "=ok;";
            return true;
        }

        Checks g_checks;
        bool g_checked = false;

        // SEH wrappers: no object with a destructor in these.
        using RemoveVehsFn = void(__fastcall*)(bool respectCanBeDeleted);
        using RemovePedsFn = void(__fastcall*)(void* exception, int mode, void* filter, bool asScript, bool cleanup);
        using EmittersFn = void(__fastcall*)(void* manager);

        bool CallRemoveVehs(std::uintptr_t function, std::uint32_t& code)
        {
            __try
            {
                reinterpret_cast<RemoveVehsFn>(function)(false);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool CallRemovePeds(std::uintptr_t function, void* exception, std::uint32_t& code)
        {
            __try
            {
                reinterpret_cast<RemovePedsFn>(function)(exception, kRemovePedsForceAllRandomAndMission, nullptr, true, true);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool CallEmitters(std::uintptr_t function, void* manager, std::uint32_t& code)
        {
            __try
            {
                reinterpret_cast<EmittersFn>(function)(manager);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    void Install(std::uint32_t hooks, std::string& report)
    {
        report.clear();
        if (!g_spawnTarget && (hooks & kHookSpawn))
            Hook(kSpawnFunnelSignature, reinterpret_cast<void*>(&SpawnDetour), g_spawnOriginal, g_spawnTarget, report);
        if (!g_sceneTarget && (hooks & kHookScene))
            Hook(kMixSceneSignature, reinterpret_cast<void*>(&SceneDetour), g_sceneOriginal, g_sceneTarget, report);
        if (!g_rulesTarget && (hooks & kHookRules))
            Hook(kSpeechRulesSignature, reinterpret_cast<void*>(&RulesDetour), g_rulesOriginal, g_rulesTarget, report);
        if (!g_state3Target && (hooks & kHookState3))
            Hook(kSpeechState3Signature, reinterpret_cast<void*>(&State3Detour), g_state3Original, g_state3Target, report);
        if (!g_sceneStopTarget && (hooks & kHookSceneStop))
            Hook(kMixSceneStopSignature, reinterpret_cast<void*>(&SceneStopDetour), g_sceneStopOriginal, g_sceneStopTarget,
                report);
    }

    void Shutdown()
    {
        g_owning.store(false, std::memory_order_release);
        g_deny.store(false, std::memory_order_release);
        g_speechObserve.store(false, std::memory_order_release);
        g_sceneObserve.store(false, std::memory_order_release);
        g_refusalCount.store(0, std::memory_order_release);
        for (void** target : {&g_spawnTarget, &g_sceneTarget, &g_rulesTarget, &g_state3Target, &g_sceneStopTarget})
        {
            if (*target)
            {
                MH_DisableHook(*target);
                MH_RemoveHook(*target);
                *target = nullptr;
            }
        }
        DisableAnimalBank();
        for (int wait = 0; wait < 200 && g_inFlight.load(std::memory_order_seq_cst) > 0; ++wait)
            Sleep(1);
        ForgetAnimalBankOriginal();
        g_spawnOriginal.store(nullptr, std::memory_order_release);
        g_sceneOriginal.store(nullptr, std::memory_order_release);
        g_rulesOriginal.store(nullptr, std::memory_order_release);
        g_state3Original.store(nullptr, std::memory_order_release);
        g_sceneStopOriginal.store(nullptr, std::memory_order_release);
    }

    void SetSpeechObserve(bool on) { g_speechObserve.store(on, std::memory_order_release); }
    void SetReplayLineRules(bool on) { g_replayRules.store(on, std::memory_order_relaxed); }
    bool ReplayLineRulesReady() { return g_replayRules.load(std::memory_order_relaxed) && g_rulesTarget != nullptr; }
    void NoteReplayLine(std::uint64_t line) { g_replayLines.Note(line); }
    void ClearReplayLines()
    {
        g_replayLines.Clear();
        for (auto& count : g_rulesBypassed)
            count.store(0, std::memory_order_relaxed);
    }
    void ReplayRulesBypassed(std::uint32_t& otherSpeaker, std::uint32_t& outOfRange, std::uint32_t& conversation)
    {
        otherSpeaker = g_rulesBypassed[0].load(std::memory_order_relaxed);
        outOfRange = g_rulesBypassed[1].load(std::memory_order_relaxed);
        conversation = g_rulesBypassed[2].load(std::memory_order_relaxed);
    }
    void CollectState3Failures(std::uint64_t& cursor, std::vector<State3Failure>& out) { g_state3Failures.Collect(cursor, out); }
    bool State3HandlerInstalled() { return g_state3Target != nullptr; }

    void SetOwning(bool owning, bool deny)
    {
        g_deny.store(owning && deny, std::memory_order_release);
        g_owning.store(owning, std::memory_order_release);
    }

    bool Owning() { return g_owning.load(std::memory_order_acquire); }
    bool SpawnFunnelInstalled() { return g_spawnTarget != nullptr; }

    void CollectSpawns(std::uint64_t& cursor, std::vector<SpawnCall>& out) { g_spawns.Collect(cursor, out); }
    void CollectScenes(std::uint64_t& cursor, std::vector<SceneCall>& out) { g_scenes.Collect(cursor, out); }
    void SetSceneObserve(bool on) { g_sceneObserve.store(on, std::memory_order_release); }
    void SetSceneRefusals(const std::vector<std::uint32_t>& returnRvas)
    {
        g_refusalCount.store(0, std::memory_order_release);
        std::uint32_t count = 0;
        for (std::uint32_t rva : returnRvas)
        {
            if (count >= kMaxSceneRefusals || !rva)
                break;
            g_sceneRefusals[count++].store(rva, std::memory_order_relaxed);
        }
        g_refusalCount.store(count, std::memory_order_release);
    }
    std::uint64_t SceneRefusedCount() { return g_refused.load(std::memory_order_relaxed); }
    void CollectMixSceneEvents(std::uint64_t& cursor, std::vector<MixSceneEvent>& out) { g_sceneEvents.Collect(cursor, out); }
    bool SceneStartInstalled() { return g_sceneTarget != nullptr; }
    bool SceneStopInstalled() { return g_sceneStopTarget != nullptr; }
    void CollectRefusals(std::uint64_t& cursor, std::vector<RuleRefusal>& out) { g_refusals.Collect(cursor, out); }

    const char* CallResultName(CallResult result)
    {
        switch (result)
        {
        case CallResult::Ok: return "ok";
        case CallResult::Signature: return "signature";
        default: return "exception";
        }
    }

    const Checks& RunChecks()
    {
        if (g_checked)
            return g_checks;
        g_checked = true;
        const auto first = [](bool ok, const char* name) {
            if (!ok && g_checks.failed == "none")
                g_checks.failed = name;
            return ok;
        };
        g_checks.vehicles = first(SignatureAt(kRemoveAllVehsSignature), kRemoveAllVehsSignature.name) &&
            first(SignatureAt(kFlushVehicleReuseSignature), kFlushVehicleReuseSignature.name) &&
            first(SiteAt(kVehicleReuseSite), kVehicleReuseSite.name);
        g_checks.peds = first(SignatureAt(kRemoveAllPedsSignature), kRemoveAllPedsSignature.name) &&
            first(SiteAt(kPedFlushSite), kPedFlushSite.name) && first(SiteAt(kPedDestroySite), kPedDestroySite.name);
        g_checks.emitters = first(SignatureAt(kStaticEmittersSignature), kStaticEmittersSignature.name) &&
            first(SiteAt(kEmitterManagerSite), kEmitterManagerSite.name);
        g_checks.perschar = first(SiteAt(kPerscharManagerSite), kPerscharManagerSite.name);
        g_checks.speech = first(SiteAt(kSpeechManagerSite), kSpeechManagerSite.name);
        return g_checks;
    }

    CallResult RemoveAllVehicles(std::uint32_t& code)
    {
        code = 0;
        if (!RunChecks().vehicles)
            return CallResult::Signature;
        return CallRemoveVehs(Base() + kRemoveAllVehsSignature.rva, code) ? CallResult::Ok : CallResult::Exception;
    }

    CallResult RemoveAllPeds(std::uint64_t playerPed, std::uint32_t& code)
    {
        code = 0;
        if (!RunChecks().peds || !playerPed)
            return CallResult::Signature;
        return CallRemovePeds(Base() + kRemoveAllPedsSignature.rva, reinterpret_cast<void*>(playerPed), code)
            ? CallResult::Ok
            : CallResult::Exception;
    }

    CallResult ResetStaticEmitters(std::uint32_t& code)
    {
        code = 0;
        if (!RunChecks().emitters)
            return CallResult::Signature;
        return CallEmitters(Base() + kStaticEmittersSignature.rva, reinterpret_cast<void*>(Base() + kStaticEmitterManagerRva), code)
            ? CallResult::Ok
            : CallResult::Exception;
    }

    namespace
    {
        bool GuardedWriteThreadByte(std::uintptr_t address, std::uint8_t value)
        {
            __try
            {
                *reinterpret_cast<volatile std::uint8_t*>(address) = value;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    ThreadSpare SpareScriptHookThreads(bool pauseScriptHook, std::vector<ThreadView>* census)
    {
        static int checked = 0; // 0 not yet, 1 passed, -1 failed
        static std::string failed = "none";
        ThreadSpare out;
        if (checked == 0)
        {
            checked = 1;
            if (!SignatureAt(kPauseThreadsSignature))
                failed = kPauseThreadsSignature.name;
            else if (!SignatureAt(kPauseThreadsMarkSignature))
                failed = kPauseThreadsMarkSignature.name;
            else if (!SignatureAt(kThreadTickSignature))
                failed = kThreadTickSignature.name;
            else if (!SiteAt(kThreadArraySite))
                failed = kThreadArraySite.name;
            if (failed != "none")
                checked = -1;
        }
        if (checked < 0)
        {
            out.error = failed + "_signature_mismatch";
            return out;
        }
        std::uint64_t array = 0;
        std::uint16_t count = 0;
        if (!replay::mem::Copy(&array, Base() + kThreadArrayRva, sizeof(array)) ||
            !replay::mem::Copy(&count, Base() + kThreadCountRva, sizeof(count)) || !array)
        {
            out.error = "thread_array_unreadable";
            return out;
        }
        const std::uint32_t n = count < kMaxScriptThreads ? count : kMaxScriptThreads;
        for (std::uint32_t i = 0; i < n; ++i)
        {
            std::uint64_t thread = 0;
            if (!replay::mem::Copy(&thread, static_cast<std::uintptr_t>(array + 8ull * i), sizeof(thread)) || !thread)
                continue;
            ThreadView view;
            if (!replay::mem::Copy(&view.id, static_cast<std::uintptr_t>(thread + kThreadIdOffset), 4) ||
                !replay::mem::Copy(&view.scriptHash, static_cast<std::uintptr_t>(thread + kThreadScriptHashOffset), 4) ||
                !replay::mem::Copy(&view.paused, static_cast<std::uintptr_t>(thread + kThreadPausedOffset), 1) ||
                !replay::mem::Copy(&view.pausable, static_cast<std::uintptr_t>(thread + kThreadPausableOffset), 1))
                continue;
            if (!view.id)
                continue;
            ++out.threads;
            out.pausedByNative += view.paused ? 1u : 0u;
            out.scriptHook += view.scriptHash == kScriptHookThreadHash ? 1u : 0u;
            if (UnpauseForScriptHook(view, pauseScriptHook))
            {
                if (GuardedWriteThreadByte(static_cast<std::uintptr_t>(thread + kThreadPausedOffset), 0))
                {
                    ++out.unpaused;
                    view.paused = 0;
                }
                else
                {
                    ++out.writeFailed;
                }
            }
            if (census)
                census->push_back(view);
        }
        out.ok = true;
        return out;
    }

    int PerscharHeld()
    {
        if (!RunChecks().perschar)
            return -1;
        std::uint64_t manager = 0;
        std::uint8_t held = 0;
        if (!replay::mem::Copy(&manager, Base() + kPerscharManagerRva, sizeof(manager)) || !manager ||
            !replay::mem::Copy(&held, static_cast<std::uintptr_t>(manager + kPerscharHeldOffset), 1))
            return -1;
        return held;
    }

    SlotCensus ReadSpeechSlots(const std::uint64_t* puppetPeds, std::size_t count)
    {
        SlotCensus census;
        if (!RunChecks().speech)
            return census;
        const std::uintptr_t manager = Base() + kSpeechManagerRva;
        census.ok = true;
        for (std::size_t c = 0; c < 4; ++c)
        {
            const SlotClass& slotClass = kSlotClasses[c];
            std::uint64_t array = 0;
            std::uint16_t total = 0;
            if (!replay::mem::Copy(&array, manager + slotClass.array, 8) || !replay::mem::Copy(&total, manager + slotClass.count, 2))
            {
                census.ok = false;
                continue;
            }
            census.total[c] = total;
            for (std::uint16_t n = 0; n < total && n < 256 && array; ++n)
            {
                const std::uintptr_t slot = static_cast<std::uintptr_t>(array) + static_cast<std::uintptr_t>(n) * slotClass.stride;
                if (c < 3)
                {
                    std::uint32_t bank = 0;
                    if (replay::mem::Copy(&bank, slot + kBankSlotBank, 4) && bank && bank != 0xFFFFFFFFu)
                        ++census.used[c];
                    continue;
                }
                std::uint64_t owner = 0;
                if (!replay::mem::Copy(&owner, slot + kAmbientSlotOwner, 8) || !owner)
                    continue;
                ++census.used[c];
                std::uint64_t ped = 0;
                replay::mem::Copy(&ped, static_cast<std::uintptr_t>(owner + kSpeechEntityPed), 8);
                bool puppet = false;
                for (std::size_t p = 0; p < count && !puppet; ++p)
                    puppet = ped && puppetPeds[p] == ped;
                ++(puppet ? census.ambientByPuppet : census.ambientByWorld);
            }
        }
        return census;
    }

    bool BankOfWaveSlot(std::uint64_t waveSlot, int& slotClass, std::uint32_t& bank)
    {
        if (!waveSlot || !RunChecks().speech)
            return false;
        const std::uintptr_t manager = Base() + kSpeechManagerRva;
        for (int c = 0; c < 3; ++c)
        {
            std::uint64_t array = 0;
            std::uint16_t total = 0;
            if (!replay::mem::Copy(&array, manager + kSlotClasses[c].array, 8) ||
                !replay::mem::Copy(&total, manager + kSlotClasses[c].count, 2) || !array)
                continue;
            for (std::uint16_t n = 0; n < total && n < 256; ++n)
            {
                const std::uintptr_t slot = static_cast<std::uintptr_t>(array) + static_cast<std::uintptr_t>(n) * kSlotClasses[c].stride;
                std::uint64_t wave = 0;
                if (replay::mem::Copy(&wave, slot + kSlotWave, 8) && wave == waveSlot &&
                    replay::mem::Copy(&bank, slot + kBankSlotBank, 4))
                {
                    slotClass = c;
                    return true;
                }
            }
        }
        return false;
    }
}

namespace replay::theatre
{
    namespace
    {
        using AmbientReleaseFn = void(__fastcall*)(void* manager, void* speechEntity);

        bool CallAmbientRelease(std::uintptr_t function, void* manager, void* speechEntity, std::uint32_t& code)
        {
            __try
            {
                reinterpret_cast<AmbientReleaseFn>(function)(manager, speechEntity);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    // Guarded reads of the ambient slots (speech_slots.h), the layout
    // 0x2FBD440 and 0x2FB1E94 read (build 1491.50).
    void ReadAmbientSlots(AmbientSlots& out)
    {
        out.ok = false;
        out.slots.clear();
        if (!RunChecks().speech)
            return;
        const std::uintptr_t manager = Base() + kSpeechManagerRva;
        std::uint64_t array = 0;
        std::uint16_t total = 0;
        if (!replay::mem::Copy(&array, manager + kAmbientArray, 8) || !replay::mem::Copy(&total, manager + kAmbientCount, 2) ||
            !array || total == 0 || total > 64)
            return;
        out.slots.reserve(total);
        for (std::uint16_t n = 0; n < total; ++n)
        {
            const std::uintptr_t slot = static_cast<std::uintptr_t>(array) + static_cast<std::uintptr_t>(n) * kAmbientStride;
            AmbientSlot view;
            if (!replay::mem::Copy(&view.ref, slot + 0x10, 8) || !replay::mem::Copy(&view.wave, slot + 0x18, 8) ||
                !replay::mem::Copy(&view.owner, slot + 0x28, 8) || !replay::mem::Copy(&view.priority, slot + 0x30, 4))
                return;
            if (view.owner)
                replay::mem::Copy(&view.ownerPed, static_cast<std::uintptr_t>(view.owner + kSpeechEntityPed), 8);
            out.slots.push_back(view);
        }
        out.ok = true;
    }

    CallResult ReleaseAmbientSlotsOf(std::uint64_t speechEntity, std::uint32_t& code)
    {
        code = 0;
        static int checked = 0; // 0 not yet, 1 passed, -1 failed
        if (checked == 0)
            checked = RunChecks().speech && SignatureAt(kAmbientReleaseSignature) ? 1 : -1;
        if (checked < 0 || !speechEntity)
            return CallResult::Signature;
        return CallAmbientRelease(Base() + kAmbientReleaseSignature.rva, reinterpret_cast<void*>(Base() + kSpeechManagerRva),
                   reinterpret_cast<void*>(speechEntity), code)
            ? CallResult::Ok
            : CallResult::Exception;
    }
}

// ---- animal voice bank (animal_bank.h) ----
// The recorded voice's bank of an animal loaded and held ahead of its lines:
// the manager's type picker returns the held type with its bank number, or
// no type instead of rotating a type that holds its voice. The detour reads
// the records under guard and writes only the held type's next bank number,
// on the manager's own thread, right before the loader reads it. No native.
namespace replay::theatre
{
    namespace
    {
        namespace bank = replay::animalbank;

        using AnimalBankPickFn = U(__fastcall*)(U manager);
        std::atomic<AnimalBankPickFn> g_bankPickOriginal{nullptr};
        void* g_bankPickTarget = nullptr;
        std::atomic<std::uint64_t> g_bankHolds[bank::kMaxHolds];
        std::atomic<std::uint32_t> g_bankHoldCount{0};
        std::atomic<std::uint64_t> g_bankPicks{0};
        std::atomic<std::uint64_t> g_bankForced{0};
        std::atomic<std::uint64_t> g_bankKeeps{0};
        std::atomic<std::uint64_t> g_bankNotKept{0};
        std::atomic<std::uint64_t> g_bankWriteFailed{0};
        std::atomic<std::uint32_t> g_bankLastForced{0}; // type << 8 | bank
        // The manager the records belong to
        // (Base() + kSpeechManagerRva, its one caller passes it from
        // 0x2F1DA13); the detour acts only on that one.
        std::atomic<std::uintptr_t> g_bankManager{0};
        std::atomic<std::uint64_t> g_bankOtherManager{0};

        int g_bankChecked = 0; // 0 not yet, 1 passed, -1 failed
        std::string g_bankFailed = "none";

        // The record of `type` in the manager at `manager`, as DecidePick
        // needs it (guarded reads; an unreadable field leaves the default,
        // which never forces).
        bank::HoldView ViewOf(std::uintptr_t manager, const bank::Hold& hold, AnimalBankState* state)
        {
            bank::HoldView view;
            view.hold = hold;
            AnimalBankState local;
            AnimalBankState& out = state ? *state : local;
            const int type = hold.type;
            if (!manager || type < 0 || type >= bank::kTypeCount)
                return view;
            std::uint64_t object = 0;
            const bool read = replay::mem::Copy(&out.count, manager + bank::RecordOffset(type, bank::kBankCount), 4) &&
                replay::mem::Copy(&out.next, manager + bank::RecordOffset(type, bank::kNextBank), 4) &&
                replay::mem::Copy(&out.demand, manager + bank::RecordOffset(type, bank::kDemand), 4) &&
                replay::mem::Copy(&object, manager + bank::RecordOffset(type, bank::kHolderObject), 8);
            if (!read)
                return view;
            out.ok = true;
            if (object)
            {
                std::uint64_t entry = 0;
                if (replay::mem::Copy(&entry, static_cast<std::uintptr_t>(object + bank::kObjectEntry), 8) && entry &&
                    replay::mem::Copy(&out.entryState, static_cast<std::uintptr_t>(entry + bank::kEntryState), 4))
                {
                    std::uint8_t exhausted = 0;
                    if (replay::mem::Copy(&exhausted, static_cast<std::uintptr_t>(entry + bank::kEntryExhausted), 1))
                        out.exhausted = exhausted != 0;
                    // The holder takes an entry only once its load completed
                    // (0x2FC66B1), so its voice is the type's bank; a line by
                    // context reads it only in state 5 (0x2FA66AD).
                    if (replay::mem::Copy(&out.holderVoice, static_cast<std::uintptr_t>(entry + bank::kEntryVoice), 4) &&
                        out.entryState == bank::kEntryLoaded)
                        out.loadedVoice = out.holderVoice;
                }
            }
            if (type == bank::kHorseType)
            {
                out.kept = true; // its slots (+0x850) are outside the release pass
            }
            else if (out.demand > 0.0f)
            {
                std::uint16_t keepCount = 0;
                if (replay::mem::Copy(&keepCount, manager + bank::kKeepCount, 2))
                {
                    for (std::uint16_t n = 0; n < keepCount && n < bank::kTypeCount && !out.kept; ++n)
                    {
                        std::uint32_t kept = 0;
                        if (replay::mem::Copy(&kept, manager + bank::kKeepList + n * 8u, 4) && kept == static_cast<std::uint32_t>(type))
                            out.kept = true;
                    }
                }
            }
            view.loadedVoice = out.holderVoice; // a state change of the entry is no reason to load the bank again
            view.count = out.count;
            view.kept = out.kept;
            return view;
        }

        U __fastcall AnimalBankPickDetour(U manager)
        {
            replay::lifetime::DetourScope lifetime;
            InFlight inFlight;
            U picked = 0;
            if (const AnimalBankPickFn original = g_bankPickOriginal.load(std::memory_order_acquire))
                picked = original(manager);
            const std::uint32_t count = (std::min)(g_bankHoldCount.load(std::memory_order_acquire),
                static_cast<std::uint32_t>(bank::kMaxHolds));
            if (!count || !manager)
                return picked;
            if (static_cast<std::uintptr_t>(manager) != g_bankManager.load(std::memory_order_acquire))
            {
                g_bankOtherManager.fetch_add(1, std::memory_order_relaxed);
                return picked;
            }
            g_bankPicks.fetch_add(1, std::memory_order_relaxed);
            bank::HoldView views[bank::kMaxHolds];
            std::size_t used = 0;
            bool missingNotKept = false;
            for (std::uint32_t n = 0; n < count; ++n)
            {
                const bank::Hold hold = bank::Unpack(g_bankHolds[n].load(std::memory_order_acquire));
                if (!bank::Loadable(hold.type) || !hold.voice)
                    continue;
                views[used] = ViewOf(static_cast<std::uintptr_t>(manager), hold, nullptr);
                missingNotKept = missingNotKept || (views[used].loadedVoice != hold.voice && !views[used].kept);
                ++used;
            }
            const bank::PickDecision decision =
                bank::DecidePick(static_cast<int>(static_cast<std::int32_t>(picked & 0xFFFFFFFFu)), views, used);
            if (decision.kind == bank::PickKind::Force)
            {
                const std::uint32_t next = decision.bank;
                if (!replay::mem::Put(static_cast<std::uintptr_t>(manager) + bank::RecordOffset(decision.type, bank::kNextBank),
                        &next, sizeof(next)))
                {
                    g_bankWriteFailed.fetch_add(1, std::memory_order_relaxed);
                    return picked;
                }
                g_bankForced.fetch_add(1, std::memory_order_relaxed);
                g_bankLastForced.store((static_cast<std::uint32_t>(decision.type) << 8) | (next & 0xFFu), std::memory_order_relaxed);
                return static_cast<U>(static_cast<std::uint32_t>(decision.type));
            }
            if (missingNotKept)
                g_bankNotKept.fetch_add(1, std::memory_order_relaxed);
            if (decision.kind == bank::PickKind::Keep)
            {
                g_bankKeeps.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
            return picked;
        }

        void DisableAnimalBank()
        {
            g_bankHoldCount.store(0, std::memory_order_release);
            if (g_bankPickTarget)
            {
                MH_DisableHook(g_bankPickTarget);
                MH_RemoveHook(g_bankPickTarget);
                g_bankPickTarget = nullptr;
            }
        }

        void ForgetAnimalBankOriginal() { g_bankPickOriginal.store(nullptr, std::memory_order_release); }
    }

    bool AnimalBankChecks(std::string& failed)
    {
        if (g_bankChecked == 0)
        {
            const auto first = [](bool ok, const char* name) {
                if (!ok && g_bankFailed == "none")
                    g_bankFailed = name;
                return ok;
            };
            char format[16]{};
            const bool formatText = replay::mem::Copy(format, Base() + kAnimalBankFormatRva, 13) &&
                std::memcmp(format, "%s_NEAR_%.2u", 13) == 0;
            const bool ok = first(RunChecks().speech, kSpeechManagerSite.name) &&
                first(SignatureAt(kAnimalBankPickSignature), kAnimalBankPickSignature.name) &&
                first(SiteAt(kAnimalBankPickSite), kAnimalBankPickSite.name) &&
                first(SignatureAt(kAnimalBankCountSignature), kAnimalBankCountSignature.name) &&
                first(SignatureAt(kAnimalBankNextSignature), kAnimalBankNextSignature.name) &&
                first(SignatureAt(kAnimalVoiceHolderSignature), kAnimalVoiceHolderSignature.name) &&
                first(SignatureAt(kAnimalVoiceEntrySignature), kAnimalVoiceEntrySignature.name) &&
                first(SignatureAt(kAnimalBankKeepSignature), kAnimalBankKeepSignature.name) &&
                first(SiteAt(kAnimalNameTableSite), kAnimalNameTableSite.name) &&
                first(SiteAt(kAnimalBankFormatSite), kAnimalBankFormatSite.name) &&
                first(formatText, "animal_bank_format_text");
            g_bankChecked = ok ? 1 : -1;
        }
        failed = g_bankFailed;
        return g_bankChecked > 0;
    }

    void InstallAnimalBank(std::string& report)
    {
        if (g_bankPickTarget)
            return;
        std::string failed;
        if (!AnimalBankChecks(failed))
        {
            report += std::string(kAnimalBankPickSignature.name) + "=check_" + failed + ";";
            return;
        }
        g_bankManager.store(Base() + kSpeechManagerRva, std::memory_order_release);
        Hook(kAnimalBankPickSignature, reinterpret_cast<void*>(&AnimalBankPickDetour), g_bankPickOriginal, g_bankPickTarget,
            report);
    }

    bool AnimalBankInstalled() { return g_bankPickTarget != nullptr; }

    bool ReadAnimalBankTypes(std::vector<replay::animalbank::TypeBanks>& out)
    {
        out.clear();
        std::string failed;
        if (!AnimalBankChecks(failed))
            return false;
        const std::uintptr_t manager = Base() + kSpeechManagerRva;
        for (int type = 0; type < bank::kTypeCount; ++type)
        {
            bank::TypeBanks entry;
            entry.type = type;
            if (!replay::mem::Copy(&entry.count, manager + bank::RecordOffset(type, bank::kBankCount), 4) ||
                entry.count == 0 || entry.count > bank::kMaxBanks)
                continue;
            std::uint64_t name = 0;
            if (!replay::mem::Copy(&name, Base() + kAnimalNameTableRva + static_cast<std::uintptr_t>(type) * 8u, 8) || !name)
                continue;
            // A string of the .rdata: read until the copy fails or ends.
            std::size_t size = sizeof(entry.name) - 1;
            while (size > 0 && !replay::mem::Copy(entry.name, static_cast<std::uintptr_t>(name), size))
                size /= 2;
            entry.name[size] = 0;
            if (!bank::PlausibleName(entry.name, sizeof(entry.name)))
                continue;
            out.push_back(entry);
        }
        return true;
    }

    AnimalBankState ReadAnimalBank(int type)
    {
        AnimalBankState state;
        std::string failed;
        if (!AnimalBankChecks(failed) || !bank::Loadable(type))
            return state;
        bank::Hold hold;
        hold.type = type;
        ViewOf(Base() + kSpeechManagerRva, hold, &state);
        return state;
    }

    void SetAnimalBankHolds(const std::vector<replay::animalbank::Hold>& holds)
    {
        const std::size_t count = (std::min)(holds.size(), bank::kMaxHolds);
        g_bankHoldCount.store(0, std::memory_order_release);
        for (std::size_t n = 0; n < count; ++n)
            g_bankHolds[n].store(bank::Pack(holds[n]), std::memory_order_release);
        g_bankHoldCount.store(static_cast<std::uint32_t>(count), std::memory_order_release);
    }

    AnimalBankCounters ReadAnimalBankCounters()
    {
        AnimalBankCounters counters;
        counters.picks = g_bankPicks.load(std::memory_order_relaxed);
        counters.forced = g_bankForced.load(std::memory_order_relaxed);
        counters.keeps = g_bankKeeps.load(std::memory_order_relaxed);
        counters.notKept = g_bankNotKept.load(std::memory_order_relaxed);
        counters.writeFailed = g_bankWriteFailed.load(std::memory_order_relaxed);
        counters.otherManager = g_bankOtherManager.load(std::memory_order_relaxed);
        const std::uint32_t last = g_bankLastForced.load(std::memory_order_relaxed);
        counters.lastForcedType = last >> 8;
        counters.lastForcedBank = last & 0xFFu;
        return counters;
    }

    void ResetAnimalBankCounters()
    {
        for (std::atomic<std::uint64_t>* counter : {&g_bankPicks, &g_bankForced, &g_bankKeeps, &g_bankNotKept, &g_bankWriteFailed,
                 &g_bankOtherManager})
            counter->store(0, std::memory_order_relaxed);
        g_bankLastForced.store(0, std::memory_order_relaxed);
    }
}
