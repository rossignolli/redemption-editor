#include "weapon_observer.h"
#include "guarded_memory.h"
#include "profiler.h"

#include "audio_observer.h"

#include <algorithm>
#include <atomic>
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <windows.h>

#include <MinHook.h>
#include "hook_batch.h"
#include "module_lifetime.h"
#include "pose_capture.h" // ApplyOverrideAfterWeaponPass
#include "weapon_bones_core.h" // the pass's change per bone
#include "strap_state_core.h" // the flags written on a puppet weapon
#include "weapon_look_core.h" // the look's layout

namespace replay::weapon
{
    namespace
    {
        // The component query behind the weapon component natives reads these
        // fields.
        constexpr std::uint64_t kObjectWeapon = 0x448;
        constexpr std::uint64_t kWeaponTable = 0x10;
        constexpr std::uint64_t kWeaponTableBuckets = 0x18;
        constexpr std::uint64_t kWeaponArray = 0x390;
        constexpr std::uint64_t kWeaponArrayCount = 0x3E0;
        constexpr std::uint32_t kArrayMax = 10;
        constexpr std::uint32_t kBucketsMax = 1024;
        constexpr std::uint64_t kComponentInfo = 0x10;
        constexpr std::uint64_t kInfoHash = 0x20;

        bool Plausible(std::uint64_t pointer)
        {
            return pointer >= 0x10000 && pointer < 0x00007FFFFFFFFFFFull;
        }

        template <typename T>
        bool Read(std::uint64_t address, T& value)
        {
            return replay::audio::ReadBytes(address, &value, sizeof(T));
        }

        constexpr std::uint64_t kWeaponMaskHolder = 0x80;
        constexpr std::uint64_t kMaskWords = 0x10;
        constexpr std::uint64_t kWeaponFlags = 0x62C;
        constexpr std::uint64_t kWeaponCloth = 0x440;
        constexpr std::uint64_t kWeaponShoulder = 0x450;
        constexpr std::uint64_t kWeaponWeightA = 0x5D4;
        constexpr std::uint64_t kWeaponWeightB = 0x5D8;

        bool GuardedWrite(std::uint64_t address, const void* data, std::size_t size)
        {
            __try
            {
                std::memcpy(reinterpret_cast<void*>(address), data, size);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool View(std::uint64_t component, bool fromTable, ComponentView& out)
        {
            out = {};
            out.component = component;
            out.fromTable = fromTable;
            return Plausible(component) && Read(component + kComponentInfo, out.info) &&
                Plausible(out.info) && Read(out.info + kInfoHash, out.hash);
        }
    }

    namespace
    {
        bool WeaponOf(std::uint64_t object, std::uint64_t& weapon)
        {
            weapon = 0;
            return Plausible(object) && Read(object + kObjectWeapon, weapon) && Plausible(weapon);
        }

        bool MaskWords(std::uint64_t weapon, std::uint64_t& words)
        {
            std::uint64_t holder = 0;
            return Read(weapon + kWeaponMaskHolder, holder) && Plausible(holder) &&
                Read(holder + kMaskWords, words) && Plausible(words);
        }
    }

    bool ReadStrapState(std::uint64_t object, StrapState& out)
    {
        out = {};
        std::uint64_t weapon = 0;
        if (!WeaponOf(object, weapon))
            return false;
        std::uint64_t cloth = 0;
        std::uint64_t shoulder = 0;
        Read(weapon + kWeaponCloth, cloth);
        Read(weapon + kWeaponShoulder, shoulder);
        out.hasStraps = cloth || shoulder;
        Read(weapon + kWeaponFlags, out.flags);
        Read(weapon + kWeaponWeightA, out.weightA);
        Read(weapon + kWeaponWeightB, out.weightB);
        std::uint64_t words = 0;
        std::int32_t count = 0;
        if (!MaskWords(weapon, words) || !Read(weapon + kWeaponArrayCount, count) ||
            count <= 0 || count > static_cast<std::int32_t>(kArrayMax))
        {
            return true;
        }
        for (std::int32_t index = 0; index < count; ++index)
        {
            std::uint64_t component = 0;
            ComponentView view;
            if (!Read(weapon + kWeaponArray + index * 8ull, component) || !View(component, false, view))
                continue;
            const std::uint32_t bit = static_cast<std::uint32_t>(index) + 1u;
            std::uint32_t word = 0;
            if (Read(words + (bit >> 5) * 4ull, word))
                out.maskBits.push_back({view.hash, ((word >> (bit & 31u)) & 1u) != 0});
        }
        return true;
    }

    namespace
    {
        std::atomic<std::uint32_t> g_testClearBits{0};
        std::atomic<float> g_testWeightA{-1.0f};
        std::atomic<float> g_testWeightB{-1.0f};
        SRWLOCK g_strapTestLock = SRWLOCK_INIT;
        std::vector<std::pair<std::uint32_t, bool>> g_strapTestBits;
    }

    void SetStrapTestBit(std::uint32_t hash, int bit)
    {
        AcquireSRWLockExclusive(&g_strapTestLock);
        g_strapTestBits.erase(std::remove_if(g_strapTestBits.begin(), g_strapTestBits.end(),
                                  [&](const auto& entry) { return entry.first == hash; }),
            g_strapTestBits.end());
        if (bit >= 0)
            g_strapTestBits.push_back({hash, bit != 0});
        ReleaseSRWLockExclusive(&g_strapTestLock);
    }

    void ClearStrapTestBits()
    {
        AcquireSRWLockExclusive(&g_strapTestLock);
        g_strapTestBits.clear();
        ReleaseSRWLockExclusive(&g_strapTestLock);
    }

    namespace
    {
        std::atomic<std::uint32_t> g_regiveHash{0};
        std::atomic<std::uint32_t> g_regiveGeneration{0};
    }

    void RequestStrapTestRegive(std::uint32_t hash)
    {
        g_regiveHash.store(hash);
        g_regiveGeneration.fetch_add(1);
    }

    std::uint32_t StrapTestRegive(std::uint32_t& generation)
    {
        generation = g_regiveGeneration.load();
        return g_regiveHash.load();
    }

    void SetStrapTestFlags(std::uint32_t clearBits, float weightA, float weightB)
    {
        g_testClearBits.store(clearBits);
        g_testWeightA.store(weightA);
        g_testWeightB.store(weightB);
    }


    bool ApplyStrapState(std::uint64_t object, const StrapState& state, StrapApplyResult* result)
    {
        std::uint64_t weapon = 0;
        if (!WeaponOf(object, weapon))
            return false;
        std::uint64_t cloth = 0;
        std::uint64_t shoulder = 0;
        Read(weapon + kWeaponCloth, cloth);
        Read(weapon + kWeaponShoulder, shoulder);
        if (!cloth && !shoulder)
            return false;
        std::vector<std::pair<std::uint32_t, bool>> maskBits = state.maskBits;
#if defined(REDEMPTION_DEV)
        AcquireSRWLockShared(&g_strapTestLock);
        for (const auto& forced : g_strapTestBits)
        {
            for (auto& bit : maskBits)
            {
                if (bit.first == forced.first)
                    bit.second = forced.second;
            }
        }
        ReleaseSRWLockShared(&g_strapTestLock);
#endif
        std::uint64_t words = 0;
        std::int32_t count = 0;
        if (MaskWords(weapon, words) && Read(weapon + kWeaponArrayCount, count) &&
            count > 0 && count <= static_cast<std::int32_t>(kArrayMax))
        {
            for (std::int32_t index = 0; index < count; ++index)
            {
                std::uint64_t component = 0;
                ComponentView view;
                if (!Read(weapon + kWeaponArray + index * 8ull, component) || !View(component, false, view))
                    continue;
                for (const auto& recorded : maskBits)
                {
                    if (recorded.first != view.hash)
                        continue;
                    const std::uint32_t bit = static_cast<std::uint32_t>(index) + 1u;
                    const std::uint64_t address = words + (bit >> 5) * 4ull;
                    std::uint32_t word = 0;
                    if (!Read(address, word))
                        break;
                    const std::uint32_t wanted = recorded.second ? (word | (1u << (bit & 31u)))
                                                                 : (word & ~(1u << (bit & 31u)));
                    if (wanted != word)
                        GuardedWrite(address, &wanted, sizeof(wanted));
                    break;
                }
            }
        }
        std::uint32_t flags = 0;
        if (!Read(weapon + kWeaponFlags, flags))
            return false;
        // strap_state_core.h: the hold bits and the cloth state as recorded,
        // and the rebuild request only where the engine made it.
        std::uint32_t recordedFlags = state.flags;
        float weightA = state.weightA;
        float weightB = state.weightB;
#if defined(REDEMPTION_DEV)
        recordedFlags &= ~g_testClearBits.load();
        if (g_testWeightA.load() >= 0.0f)
            weightA = g_testWeightA.load();
        if (g_testWeightB.load() >= 0.0f)
            weightB = g_testWeightB.load();
#endif
        const std::uint32_t wanted = replay::strap::PlaybackFlags(flags, recordedFlags);
        const bool written = wanted == flags || GuardedWrite(weapon + kWeaponFlags, &wanted, sizeof(wanted));
        GuardedWrite(weapon + kWeaponWeightA, &weightA, sizeof(float));
        GuardedWrite(weapon + kWeaponWeightB, &weightB, sizeof(float));
        if (result)
        {
            result->before = flags;
            result->wanted = wanted;
            result->after = 0;
            result->readBack = Read(weapon + kWeaponFlags, result->after);
        }
        return written;
    }

    bool ReadComponentList(std::uint64_t object, std::vector<std::uint32_t>& out)
    {
        namespace look = replay::weaponlook;
        out.clear();
        std::uint64_t weapon = 0;
        if (!WeaponOf(object, weapon))
            return false;
        const auto hashOf = [](std::uint64_t component, std::uint32_t& hash) {
            std::uint64_t info = 0;
            return Plausible(component) && Read(component + look::kComponentInfo, info) && Plausible(info) &&
                Read(info + look::kInfoHash, hash);
        };
        std::int32_t count = 0;
        if (!Read(weapon + look::kComponentCount, count) || count < 0 || count > look::kArrayMax)
            return false;
        for (std::int32_t index = 0; index < count; ++index)
        {
            std::uint64_t component = 0;
            std::uint32_t hash = 0;
            if (Read(weapon + look::kComponentArray + index * 8ull, component) && hashOf(component, hash))
                out.push_back(hash);
        }
        std::uint64_t buckets = 0;
        std::uint32_t bucketCount = 0;
        if (!Read(weapon + look::kComponentTable, buckets) || !Read(weapon + look::kComponentBuckets, bucketCount))
            return false;
        if (bucketCount && (!Plausible(buckets) || bucketCount > look::kBucketsMax))
            return false;
        for (std::uint32_t bucket = 0; bucket < bucketCount; ++bucket)
        {
            std::uint64_t node = 0;
            if (!Read(buckets + bucket * 8ull, node))
                return false;
            for (std::uint32_t chain = 0; node && chain < look::kChainMax; ++chain)
            {
                if (!Plausible(node))
                    return false;
                std::uint64_t component = 0;
                std::uint32_t hash = 0;
                if (Read(node + look::kNodeValue, component) && hashOf(component, hash))
                    out.push_back(hash);
                if (!Read(node + look::kNodeNext, node))
                    return false;
            }
        }
        look::Normalize(out);
        return true;
    }

    bool ReadWear(std::uint64_t object, WearFields& out)
    {
        namespace look = replay::weaponlook;
        out = {};
        std::uint64_t weapon = 0;
        if (!WeaponOf(object, weapon))
            return false;
        std::uint64_t info = 0;
        out.hasInfo = Read(weapon + look::kWeaponInfo, info) && Plausible(info);
        return Read(weapon + look::kDegradation, out.degradation) && Read(weapon + look::kPermanent, out.permanent) &&
            Read(weapon + look::kSoot, out.soot) && Read(weapon + look::kDamage, out.damage) &&
            Read(weapon + look::kDirt, out.dirt) && Read(weapon + look::kWearE4, out.e4);
    }

    bool ReadWearShaderDirt(std::uint64_t object, float& value)
    {
        namespace look = replay::weaponlook;
        value = 0.0f;
        std::uint64_t weapon = 0, holder = 0, drawable = 0, effect = 0, data = 0;
        std::uint8_t type = 0;
        return WeaponOf(object, weapon) && Read(weapon + look::kWeaponDrawHolder, holder) && Plausible(holder) &&
            Read(holder + look::kDrawHolderDrawable, drawable) && Plausible(drawable) &&
            Read(drawable + look::kDrawableEffect, effect) && Plausible(effect) && Read(effect + look::kEffectType, type) &&
            (type & 0x1F) == look::kEffectTypeWeapon && Read(effect + look::kEffectData, data) && Plausible(data) &&
            Read(data + look::kEffectDirt, value);
    }

    bool WriteWearRaw(std::uint64_t object, float permanent, float e4)
    {
        namespace look = replay::weaponlook;
        std::uint64_t weapon = 0;
        if (!WeaponOf(object, weapon))
            return false;
        const float p = look::Clean(permanent);
        const float e = look::Clean(e4);
        return GuardedWrite(weapon + look::kPermanent, &p, sizeof(p)) && GuardedWrite(weapon + look::kWearE4, &e, sizeof(e));
    }

    bool ReadAttachFrame(std::uint64_t object, AttachFrame& out)
    {
        constexpr std::uint64_t kObjectExtensions = 0xA8;
        constexpr std::uint64_t kAttachExtension = 0x50;
        constexpr std::uint64_t kAttachPosition = 0x30;
        constexpr std::uint64_t kAttachRotation = 0x40;
        constexpr std::uint64_t kAttachBone = 0x58;
        std::uint64_t extensions = 0;
        std::uint64_t attach = 0;
        if (!Plausible(object) || !Read(object + kObjectExtensions, extensions) || !Plausible(extensions) ||
            !Read(extensions + kAttachExtension, attach) || !Plausible(attach))
        {
            return false;
        }
        AttachFrame frame;
        if (!Read(attach + kAttachBone, frame.bone) ||
            !replay::audio::ReadBytes(attach + kAttachPosition, frame.position, sizeof(frame.position)) ||
            !replay::audio::ReadBytes(attach + kAttachRotation, frame.rotation, sizeof(frame.rotation)))
        {
            return false;
        }
        out = frame;
        return true;
    }

    namespace
    {
        // Build 1491.50: 0x1172E44(weapon). Prologue: push rbx; sub rsp,20h;
        // cmp qword ptr [rcx+440h],0; mov rbx,rcx; jne.
        constexpr std::uintptr_t kStrapHideRva = 0x1172E44;
        constexpr std::uint8_t kStrapHideSignature[] = {
            0x40, 0x53,                                     // push rbx
            0x48, 0x83, 0xEC, 0x20,                         // sub rsp, 20h
            0x48, 0x83, 0xB9, 0x40, 0x04, 0x00, 0x00, 0x00, // cmp qword ptr [rcx+440h], 0
            0x48, 0x8B, 0xD9,                               // mov rbx, rcx
            0x75, 0x14,                                     // jne
        };
        constexpr std::uint64_t kWeaponObject = 0xE8;
        using StrapHideFn = void(__fastcall*)(void*);

        std::atomic<StrapHideFn> g_strapOriginal{nullptr};
        std::atomic<std::int32_t> g_strapInFlight{0};
        std::atomic<std::uint64_t> g_strapCalls{0};
        std::atomic<std::uint64_t> g_strapSkipped{0};
        std::atomic<std::uint64_t> g_strapFaults{0};
        void* g_strapTarget = nullptr;
        bool g_strapInstalled = false;
        SRWLOCK g_strapLock = SRWLOCK_INIT;
        std::vector<std::uint64_t> g_strapOwned;

        bool StrapOwned(std::uint64_t object)
        {
            AcquireSRWLockShared(&g_strapLock);
            bool owned = false;
            for (const std::uint64_t candidate : g_strapOwned)
            {
                if (candidate == object)
                {
                    owned = true;
                    break;
                }
            }
            ReleaseSRWLockShared(&g_strapLock);
            return owned;
        }

        bool GuardedReadObject(void* weapon, std::uint64_t& object)
        {
            __try
            {
                object = *reinterpret_cast<const std::uint64_t*>(
                    reinterpret_cast<std::uint8_t*>(weapon) + kWeaponObject);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        void __fastcall StrapHideDetour(void* weapon)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.strap_hide");
            struct InFlight
            {
                InFlight() { g_strapInFlight.fetch_add(1, std::memory_order_seq_cst); }
                ~InFlight() { g_strapInFlight.fetch_sub(1, std::memory_order_seq_cst); }
            } inFlight;
            const StrapHideFn original = g_strapOriginal.load(std::memory_order_acquire);
            g_strapCalls.fetch_add(1, std::memory_order_relaxed);
            std::uint64_t object = 0;
            if (weapon && GuardedReadObject(weapon, object))
            {
                if (object && StrapOwned(object))
                {
                    g_strapSkipped.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }
            else
            {
                g_strapFaults.fetch_add(1, std::memory_order_relaxed);
            }
            if (original)
                original(weapon);
        }

        bool GuardedCompare(const void* target, const std::uint8_t* signature, std::size_t size)
        {
            return replay::mem::Matches(target, signature, size);
        }
    }

    bool InstallStrapVisibility(std::string& error)
    {
        if (g_strapInstalled)
            return true;
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        void* target = base + kStrapHideRva;
        if (!GuardedCompare(target, kStrapHideSignature, sizeof(kStrapHideSignature)))
        {
            error = "strap_hide_signature_mismatch";
            return false;
        }
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        {
            error = std::string("mh_initialize_") + MH_StatusToString(init);
            return false;
        }
        void* trampoline = nullptr;
        MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&StrapHideDetour), &trampoline);
        if (status != MH_OK || !trampoline)
        {
            error = std::string("mh_create_") + MH_StatusToString(status);
            return false;
        }
        g_strapOriginal.store(reinterpret_cast<StrapHideFn>(trampoline), std::memory_order_release);
        status = replay::hooks::Enable(target);
        if (status != MH_OK)
        {
            MH_RemoveHook(target);
            g_strapOriginal.store(nullptr, std::memory_order_release);
            error = std::string("mh_enable_") + MH_StatusToString(status);
            return false;
        }
        g_strapTarget = target;
        g_strapInstalled = true;
        return true;
    }

    void ShutdownStrapVisibility()
    {
        if (g_strapInstalled && g_strapTarget)
        {
            MH_DisableHook(g_strapTarget);
            MH_RemoveHook(g_strapTarget);
            g_strapInstalled = false;
            g_strapTarget = nullptr;
        }
        for (int wait = 0; wait < 200 && g_strapInFlight.load(std::memory_order_seq_cst) > 0; ++wait)
            Sleep(1);
        g_strapOriginal.store(nullptr, std::memory_order_release);
        AcquireSRWLockExclusive(&g_strapLock);
        g_strapOwned.clear();
        ReleaseSRWLockExclusive(&g_strapLock);
    }

    void SetStrapOwnedObjects(const std::vector<std::uint64_t>& objects)
    {
        AcquireSRWLockExclusive(&g_strapLock);
        g_strapOwned = objects;
        ReleaseSRWLockExclusive(&g_strapLock);
    }

    StrapVisibilityStats StrapVisibilitySnapshot()
    {
        StrapVisibilityStats stats;
        stats.calls = g_strapCalls.load(std::memory_order_relaxed);
        stats.skipped = g_strapSkipped.load(std::memory_order_relaxed);
        stats.faults = g_strapFaults.load(std::memory_order_relaxed);
        return stats;
    }

    // See InstallWeaponPassPose in weapon_observer.h.
    namespace
    {
        // Build 1491.50: 0x117D49C(weapon). Prologue: mov rax,rsp;
        // mov [rax+20h],rbx; push rbp,rsi,rdi,r12..r15; lea rbp,[rax-128h];
        // sub rsp,1F0h. Only rcx is read before it is set (mov rbx,rcx;
        // mov rdx,[rcx+0E8h]); the caller 0xC23738 ignores rax.
        constexpr std::uintptr_t kWeaponPassRva = 0x117D49C;
        constexpr std::uint8_t kWeaponPassSignature[] = {
            0x48, 0x8B, 0xC4,                         // mov rax, rsp
            0x48, 0x89, 0x58, 0x20,                   // mov [rax+20h], rbx
            0x55, 0x56, 0x57,                         // push rbp, rsi, rdi
            0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, // push r12..r15
            0x48, 0x8D, 0xA8, 0xD8, 0xFE, 0xFF, 0xFF, // lea rbp, [rax-128h]
            0x48, 0x81, 0xEC, 0xF0, 0x01, 0x00, 0x00, // sub rsp, 1F0h
        };
        using WeaponPassFn = void(__fastcall*)(void*);
        std::atomic<WeaponPassFn> g_passOriginal{nullptr};
        std::atomic<std::int32_t> g_passInFlight{0};
        std::atomic<std::uint64_t> g_passCalls{0};
        std::atomic<std::uint64_t> g_passHits{0};
        std::atomic<std::uint64_t> g_passWrites{0};
        std::atomic<std::uint64_t> g_passFaults{0};
        std::atomic<std::uint32_t> g_passSlotCount{0}; // fast exit with nothing published
        void* g_passTarget = nullptr;
        bool g_passInstalled = false;
        SRWLOCK g_passLock = SRWLOCK_INIT;
        std::vector<WeaponPoseSlot> g_passSlots;

        bool PassSlotOf(std::uint64_t object, std::uint32_t& slot)
        {
            AcquireSRWLockShared(&g_passLock);
            bool found = false;
            for (const WeaponPoseSlot& candidate : g_passSlots)
            {
                if (candidate.object == object)
                {
                    slot = candidate.slot;
                    found = true;
                    break;
                }
            }
            ReleaseSRWLockShared(&g_passLock);
            return found;
        }

        // The recording table (object -> record) and the per-record stats, in
        // fixed entries the script thread assigns.
        constexpr std::uint32_t kRecEntries = 256;
        struct RecStats
        {
            std::atomic<std::uint32_t> recordId{0};
            std::atomic<std::uint32_t> calls{0};
            std::atomic<std::uint32_t> compared{0};
            std::atomic<std::uint32_t> changed{0};
            std::atomic<std::uint32_t> maxMilliDeg{0};
            std::atomic<int> worstBone{-1};
            std::atomic<std::uint32_t> boneChanged[kPassBones];
        };
        RecStats g_recStats[kRecEntries];
        // Compare buffers (locals and objects, up to 512 bones), no allocation in the detour.
        constexpr std::size_t kPassBufferQts = 1024;
        replay::pose::BoneQts g_passBefore[kPassBufferQts];
        replay::pose::BoneQts g_passAfter[kPassBufferQts];
        std::atomic<bool> g_passBuffersBusy{false};
        std::atomic<std::uint32_t> g_recCount{0};
        SRWLOCK g_recLock = SRWLOCK_INIT;
        std::vector<WeaponRecordEntry> g_recEntries;

        bool RecordOf(std::uint64_t object, std::uint32_t& recordId)
        {
            AcquireSRWLockShared(&g_recLock);
            bool found = false;
            for (const WeaponRecordEntry& entry : g_recEntries)
            {
                if (entry.object == object)
                {
                    recordId = entry.recordId;
                    found = true;
                    break;
                }
            }
            ReleaseSRWLockShared(&g_recLock);
            return found;
        }

        RecStats* StatsOf(std::uint32_t recordId)
        {
            for (RecStats& stats : g_recStats)
                if (stats.recordId.load(std::memory_order_acquire) == recordId)
                    return &stats;
            return nullptr;
        }

        void NotePassChange(RecStats& stats, const replay::pose::BoneQts* before, const replay::pose::BoneQts* after,
            std::size_t count)
        {
            const std::size_t bones = count / 2u;
            if (bones < 2)
                return;
            stats.compared.fetch_add(1, std::memory_order_relaxed);
            bool any = false;
            for (std::size_t n = 1; n < bones; ++n)
            {
                // Local and object of one bone, the weapon_bones_core.h limits.
                const float localRad = replay::weaponbones::QuatAngle(before[n].rotation, after[n].rotation);
                const float objectRad = replay::weaponbones::QuatAngle(before[bones + n].rotation, after[bones + n].rotation);
                const float dx = after[n].translation[0] - before[n].translation[0];
                const float dy = after[n].translation[1] - before[n].translation[1];
                const float dz = after[n].translation[2] - before[n].translation[2];
                const float m = std::sqrt(dx * dx + dy * dy + dz * dz);
                const float rad = localRad > objectRad ? localRad : objectRad;
                if (!(rad == rad) || !(m == m))
                    continue;
                if (rad <= replay::weaponbones::kMovedRad && m <= replay::weaponbones::kMovedM)
                    continue;
                any = true;
                if (n < kPassBones)
                    stats.boneChanged[n].fetch_add(1, std::memory_order_relaxed);
                const std::uint32_t milli = static_cast<std::uint32_t>(rad * 57295.78f);
                std::uint32_t seen = stats.maxMilliDeg.load(std::memory_order_relaxed);
                while (milli > seen && !stats.maxMilliDeg.compare_exchange_weak(seen, milli, std::memory_order_relaxed))
                {
                }
                if (milli >= seen)
                    stats.worstBone.store(static_cast<int>(n), std::memory_order_relaxed);
            }
            if (any)
                stats.changed.fetch_add(1, std::memory_order_relaxed);
        }

        void __fastcall WeaponPassDetour(void* weapon)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            struct InFlight
            {
                InFlight() { g_passInFlight.fetch_add(1, std::memory_order_seq_cst); }
                ~InFlight() { g_passInFlight.fetch_sub(1, std::memory_order_seq_cst); }
            } inFlight;
            const WeaponPassFn original = g_passOriginal.load(std::memory_order_acquire);
            // A recorded weapon: its pose before the pass. The pass runs from one
            // loop (0xC23738); the compare buffers are static, taken with a flag (a
            // second thread only skips the compare).
            std::uint32_t recordId = 0;
            bool recorded = false;
            std::size_t beforeCount = 0;
            bool buffers = false;
            if (weapon && g_recCount.load(std::memory_order_acquire) != 0)
            {
                std::uint64_t recObject = 0;
                if (GuardedReadObject(weapon, recObject) && recObject && RecordOf(recObject, recordId))
                {
                    recorded = true;
                    buffers = !g_passBuffersBusy.exchange(true, std::memory_order_acquire);
                    if (buffers && !replay::pose::capture::WeaponPassPoseBefore(recordId, g_passBefore, kPassBufferQts, beforeCount))
                        beforeCount = 0;
                }
            }
            if (original)
                original(weapon);
            g_passCalls.fetch_add(1, std::memory_order_relaxed);
            if (recorded)
            {
                const bool compared = replay::pose::capture::WeaponPassPoseAfter(
                    recordId, buffers ? beforeCount : 0u, buffers ? g_passAfter : nullptr, kPassBufferQts, true);
                if (RecStats* stats = StatsOf(recordId))
                {
                    stats->calls.fetch_add(1, std::memory_order_relaxed);
                    if (compared)
                        NotePassChange(*stats, g_passBefore, g_passAfter, beforeCount);
                }
                if (buffers)
                    g_passBuffersBusy.store(false, std::memory_order_release);
            }
            if (!weapon || g_passSlotCount.load(std::memory_order_acquire) == 0)
                return;
            std::uint64_t object = 0;
            if (!GuardedReadObject(weapon, object))
            {
                g_passFaults.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            std::uint32_t slot = 0;
            if (!object || !PassSlotOf(object, slot))
                return;
            g_passHits.fetch_add(1, std::memory_order_relaxed);
            REPLAY_PROFILE_ZONE("detour.weapon_pass_pose");
            if (replay::pose::capture::ApplyOverrideAfterWeaponPass(slot))
                g_passWrites.fetch_add(1, std::memory_order_relaxed);
        }
    }

    bool InstallWeaponPassPose(std::string& error)
    {
        if (g_passInstalled)
            return true;
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        void* target = base + kWeaponPassRva;
        if (!GuardedCompare(target, kWeaponPassSignature, sizeof(kWeaponPassSignature)))
        {
            error = "weapon_pass_signature_mismatch";
            return false;
        }
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        {
            error = std::string("mh_initialize_") + MH_StatusToString(init);
            return false;
        }
        void* trampoline = nullptr;
        MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&WeaponPassDetour), &trampoline);
        if (status != MH_OK || !trampoline)
        {
            error = std::string("mh_create_") + MH_StatusToString(status);
            return false;
        }
        g_passOriginal.store(reinterpret_cast<WeaponPassFn>(trampoline), std::memory_order_release);
        status = replay::hooks::Enable(target);
        if (status != MH_OK)
        {
            MH_RemoveHook(target);
            g_passOriginal.store(nullptr, std::memory_order_release);
            error = std::string("mh_enable_") + MH_StatusToString(status);
            return false;
        }
        g_passTarget = target;
        g_passInstalled = true;
        return true;
    }

    void ShutdownWeaponPassPose()
    {
        g_passSlotCount.store(0, std::memory_order_seq_cst);
        if (g_passInstalled && g_passTarget)
        {
            MH_DisableHook(g_passTarget);
            MH_RemoveHook(g_passTarget);
            g_passInstalled = false;
            g_passTarget = nullptr;
        }
        for (int wait = 0; wait < 200 && g_passInFlight.load(std::memory_order_seq_cst) > 0; ++wait)
            Sleep(1);
        g_passOriginal.store(nullptr, std::memory_order_release);
        AcquireSRWLockExclusive(&g_passLock);
        g_passSlots.clear();
        ReleaseSRWLockExclusive(&g_passLock);
        ResetWeaponPassRecords();
    }

    void SetWeaponPoseSlots(const std::vector<WeaponPoseSlot>& slots)
    {
        AcquireSRWLockExclusive(&g_passLock);
        g_passSlots = slots;
        g_passSlotCount.store(static_cast<std::uint32_t>(g_passSlots.size()), std::memory_order_release);
        ReleaseSRWLockExclusive(&g_passLock);
    }

    void SetWeaponRecordIds(const std::vector<WeaponRecordEntry>& entries)
    {
        // Stats entries for new records (script thread is the only writer of
        // recordId; a full table leaves the newest without stats).
        for (const WeaponRecordEntry& entry : entries)
        {
            if (StatsOf(entry.recordId))
                continue;
            for (RecStats& stats : g_recStats)
            {
                if (stats.recordId.load(std::memory_order_acquire) != 0)
                    continue;
                stats.calls.store(0);
                stats.compared.store(0);
                stats.changed.store(0);
                stats.maxMilliDeg.store(0);
                stats.worstBone.store(-1);
                for (auto& bone : stats.boneChanged)
                    bone.store(0);
                stats.recordId.store(entry.recordId, std::memory_order_release);
                break;
            }
        }
        AcquireSRWLockExclusive(&g_recLock);
        g_recEntries = entries;
        g_recCount.store(static_cast<std::uint32_t>(g_recEntries.size()), std::memory_order_release);
        ReleaseSRWLockExclusive(&g_recLock);
    }

    bool WeaponPassRecord(std::uint32_t recordId, WeaponPassRecordStats& out)
    {
        const RecStats* stats = StatsOf(recordId);
        if (!stats)
            return false;
        out.calls = stats->calls.load();
        out.compared = stats->compared.load();
        out.changed = stats->changed.load();
        out.maxMilliDeg = stats->maxMilliDeg.load();
        out.worstBone = stats->worstBone.load();
        for (std::uint32_t n = 0; n < kPassBones; ++n)
            out.boneChanged[n] = stats->boneChanged[n].load();
        return true;
    }

    void ResetWeaponPassRecords()
    {
        AcquireSRWLockExclusive(&g_recLock);
        g_recEntries.clear();
        g_recCount.store(0, std::memory_order_release);
        ReleaseSRWLockExclusive(&g_recLock);
        for (RecStats& stats : g_recStats)
            stats.recordId.store(0, std::memory_order_release);
    }

    WeaponPassStats WeaponPassSnapshot()
    {
        WeaponPassStats stats;
        stats.calls = g_passCalls.load(std::memory_order_relaxed);
        stats.hits = g_passHits.load(std::memory_order_relaxed);
        stats.writes = g_passWrites.load(std::memory_order_relaxed);
        stats.faults = g_passFaults.load(std::memory_order_relaxed);
        return stats;
    }
}
