#include "vegetation_speed.h"
#include "fx_motion_rule.h"
#include "game_sig.h"
#include "guarded_memory.h"

#include <windows.h>

#include <MinHook.h>
#include "hook_batch.h"
#include "module_lifetime.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace replay::vegetation
{
    namespace
    {
        const replay::sig::Signature kSpeed{"vegetation_speed",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 8A 42 30 3C 04 75 66",
            0xC32058};
        using SpeedFn = float*(__fastcall*)(float* out, void* entity);

        // Published by the script thread, read by the detour: the entity
        // pointer is written last, cleared first.
        constexpr int kSlots = 256;
        struct Slot
        {
            std::atomic<std::uint64_t> entity{0};
            std::atomic<float> speedSquared{0.0f};
            std::atomic<float> vx{0.0f};
            std::atomic<float> vy{0.0f};
            std::atomic<float> vz{0.0f};
            // Script thread only.
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
            std::uint64_t atMs = 0;
        };
        Slot g_slots[kSlots];
        std::atomic<SpeedFn> g_original{nullptr};
        void* g_target = nullptr;
        std::atomic<int> g_inFlight{0};
        std::atomic<std::uint64_t> g_overrides{0};

        float* __fastcall SpeedDetour(float* out, void* entity)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            g_inFlight.fetch_add(1, std::memory_order_seq_cst);
            const SpeedFn original = g_original.load(std::memory_order_acquire);
            float* result = original ? original(out, entity) : out;
            const std::uint64_t key = reinterpret_cast<std::uint64_t>(entity);
            if (key && out)
            {
                for (const Slot& slot : g_slots)
                {
                    if (slot.entity.load(std::memory_order_acquire) != key)
                        continue;
                    const float speedSquared = slot.speedSquared.load(std::memory_order_relaxed);
                    out[0] = out[1] = out[2] = out[3] = speedSquared;
                    g_overrides.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
            g_inFlight.fetch_sub(1, std::memory_order_seq_cst);
            return result;
        }
    }

    bool Install(std::string& error)
    {
        if (g_target)
            return true;
        void* target = reinterpret_cast<void*>(replay::sig::Find(kSpeed));
        if (!target)
        {
            error = "vegetation_speed_signature_mismatch";
            return false;
        }
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        {
            error = std::string("mh_initialize_") + MH_StatusToString(init);
            return false;
        }
        void* trampoline = nullptr;
        MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&SpeedDetour), &trampoline);
        if (status != MH_OK || !trampoline)
        {
            error = std::string("mh_create_") + MH_StatusToString(status);
            return false;
        }
        g_original.store(reinterpret_cast<SpeedFn>(trampoline), std::memory_order_release);
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

    void Publish(std::uint64_t entity, float x, float y, float z, std::uint64_t nowMs, bool normalPlay)
    {
        if (!entity)
            return;
        Slot* found = nullptr;
        Slot* empty = nullptr;
        for (Slot& slot : g_slots)
        {
            const std::uint64_t held = slot.entity.load(std::memory_order_relaxed);
            if (held == entity)
            {
                found = &slot;
                break;
            }
            if (!held && !empty)
                empty = &slot;
        }
        if (!found)
        {
            if (!empty)
                return;
            empty->x = x;
            empty->y = y;
            empty->z = z;
            empty->atMs = nowMs;
            empty->speedSquared.store(0.0f, std::memory_order_relaxed);
            empty->vx.store(0.0f, std::memory_order_relaxed);
            empty->vy.store(0.0f, std::memory_order_relaxed);
            empty->vz.store(0.0f, std::memory_order_relaxed);
            empty->entity.store(entity, std::memory_order_release);
            return;
        }
        // In normal play a tick whose GetTickCount64 did not advance (15.6 ms
        // steps: frames above 64 fps) keeps everything published as it was:
        // storing zero there would drop the speed and the particles' inherited
        // velocity (FxPuppetVelocity, +0x360) to nothing on those frames, and
        // moving x/y/z without atMs would halve the next frame's velocity.
        if (normalPlay && nowMs <= found->atMs)
            return;
        float velocity[3] = {};
        float speedSquared = 0.0f;
        if (normalPlay && nowMs > found->atMs)
        {
            const float dt = static_cast<float>(nowMs - found->atMs) / 1000.0f;
            const float dx = (x - found->x) / dt;
            const float dy = (y - found->y) / dt;
            const float dz = (z - found->z) / dt;
            speedSquared = dx * dx + dy * dy + dz * dz;
            // A jump (seek, a new segment) is not motion.
            if (speedSquared > 40.0f * 40.0f)
                speedSquared = 0.0f;
            // Smoothed: capture ticks and game frames are not in step.
            const float previous = found->speedSquared.load(std::memory_order_relaxed);
            speedSquared = previous * 0.5f + speedSquared * 0.5f;
            // The vector, smoothed the same way.
            const float from[3] = {found->x, found->y, found->z};
            const float to[3] = {x, y, z};
            replay::fxmotion::StepVelocity(from, to, dt, velocity);
            if (velocity[0] * velocity[0] + velocity[1] * velocity[1] + velocity[2] * velocity[2] > 40.0f * 40.0f)
                velocity[0] = velocity[1] = velocity[2] = 0.0f;
            velocity[0] = found->vx.load(std::memory_order_relaxed) * 0.5f + velocity[0] * 0.5f;
            velocity[1] = found->vy.load(std::memory_order_relaxed) * 0.5f + velocity[1] * 0.5f;
            velocity[2] = found->vz.load(std::memory_order_relaxed) * 0.5f + velocity[2] * 0.5f;
        }
        found->vx.store(velocity[0], std::memory_order_relaxed);
        found->vy.store(velocity[1], std::memory_order_relaxed);
        found->vz.store(velocity[2], std::memory_order_relaxed);
        found->x = x;
        found->y = y;
        found->z = z;
        found->atMs = nowMs;
        found->speedSquared.store(speedSquared, std::memory_order_relaxed);
    }

    float PublishedSpeedSquared(std::uint64_t entity)
    {
        for (Slot& slot : g_slots)
            if (slot.entity.load(std::memory_order_relaxed) == entity)
                return slot.speedSquared.load(std::memory_order_relaxed);
        return 0.0f;
    }

    void Remove(std::uint64_t entity)
    {
        for (Slot& slot : g_slots)
            if (slot.entity.load(std::memory_order_relaxed) == entity)
            {
                slot.entity.store(0, std::memory_order_release);
                slot.speedSquared.store(0.0f, std::memory_order_relaxed);
            }
    }

    void Clear()
    {
        for (Slot& slot : g_slots)
        {
            slot.entity.store(0, std::memory_order_release);
            slot.speedSquared.store(0.0f, std::memory_order_relaxed);
            slot.vx.store(0.0f, std::memory_order_relaxed);
            slot.vy.store(0.0f, std::memory_order_relaxed);
            slot.vz.store(0.0f, std::memory_order_relaxed);
        }
    }

    std::uint64_t Overrides() { return g_overrides.load(std::memory_order_relaxed); }

    bool PublishedVelocity(std::uint64_t entity, float out[3])
    {
        out[0] = out[1] = out[2] = 0.0f;
        if (!entity)
            return false;
        for (Slot& slot : g_slots)
            if (slot.entity.load(std::memory_order_acquire) == entity)
            {
                out[0] = slot.vx.load(std::memory_order_relaxed);
                out[1] = slot.vy.load(std::memory_order_relaxed);
                out[2] = slot.vz.load(std::memory_order_relaxed);
                return true;
            }
        return false;
    }

    namespace
    {
        // vfunc +0xB0 of the particle entity interface, no direct callers
        // (called through the vtable by the attach update, and by one more site).
        const replay::sig::Signature kFxVelocity{"vegetation_fx_velocity",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 50 0F 29 74 24 40 0F 57 F6 41 0F 11 30",
            0x1405BA4};
        using FxVelocityFn = bool(__fastcall*)(void*, void*, float*);
        std::atomic<FxVelocityFn> g_fxVelocityOriginal{nullptr};
        void* g_fxVelocityTarget = nullptr;
        std::atomic<bool> g_fxVelocityOn{false};
        std::atomic<std::uint64_t> g_fxVelocityOverrides{0};

        bool __fastcall FxVelocityDetour(void* self, void* entity, float* out)
        {
            replay::lifetime::DetourScope lifetime;
            g_inFlight.fetch_add(1, std::memory_order_seq_cst);
            bool result = false;
            bool handled = false;
            const std::uint64_t key = reinterpret_cast<std::uint64_t>(entity);
            if (key && out && g_fxVelocityOn.load(std::memory_order_relaxed))
            {
                for (const Slot& slot : g_slots)
                {
                    if (slot.entity.load(std::memory_order_acquire) != key)
                        continue;
                    out[0] = slot.vx.load(std::memory_order_relaxed);
                    out[1] = slot.vy.load(std::memory_order_relaxed);
                    out[2] = slot.vz.load(std::memory_order_relaxed);
                    out[3] = 0.0f;
                    result = out[0] != 0.0f || out[1] != 0.0f || out[2] != 0.0f;
                    handled = true;
                    g_fxVelocityOverrides.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
            if (!handled)
            {
                const FxVelocityFn original = g_fxVelocityOriginal.load(std::memory_order_acquire);
                result = original ? original(self, entity, out) : false;
            }
            g_inFlight.fetch_sub(1, std::memory_order_seq_cst);
            return result;
        }
    }

    bool InstallFxVelocity(std::string& error)
    {
        if (g_fxVelocityTarget)
            return true;
        void* target = reinterpret_cast<void*>(replay::sig::Find(kFxVelocity));
        if (!target)
        {
            char text[64];
            std::snprintf(text, sizeof(text), "fx_velocity_sig_mismatch,rva=0x%x", kFxVelocity.rva);
            error = text;
            return false;
        }
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        {
            error = std::string("mh_initialize_") + MH_StatusToString(init);
            return false;
        }
        void* trampoline = nullptr;
        MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&FxVelocityDetour), &trampoline);
        if (status != MH_OK || !trampoline)
        {
            error = std::string("mh_create_") + MH_StatusToString(status);
            return false;
        }
        g_fxVelocityOriginal.store(reinterpret_cast<FxVelocityFn>(trampoline), std::memory_order_release);
        status = replay::hooks::Enable(target);
        if (status != MH_OK)
        {
            MH_RemoveHook(target);
            g_fxVelocityOriginal.store(nullptr, std::memory_order_release);
            error = std::string("mh_enable_") + MH_StatusToString(status);
            return false;
        }
        g_fxVelocityTarget = target;
        return true;
    }

    void SetFxVelocity(bool on) { g_fxVelocityOn.store(on, std::memory_order_relaxed); }

    std::uint64_t FxVelocityOverrides() { return g_fxVelocityOverrides.load(std::memory_order_relaxed); }

    void Shutdown()
    {
        Clear();
        g_fxVelocityOn.store(false, std::memory_order_relaxed);
        if (g_fxVelocityTarget)
        {
            MH_DisableHook(g_fxVelocityTarget);
            MH_RemoveHook(g_fxVelocityTarget);
            g_fxVelocityTarget = nullptr;
        }
        if (g_target)
        {
            MH_DisableHook(g_target);
            MH_RemoveHook(g_target);
            g_target = nullptr;
        }
        for (int wait = 0; wait < 200 && g_inFlight.load(std::memory_order_seq_cst) > 0; ++wait)
            Sleep(1);
        g_original.store(nullptr, std::memory_order_release);
        g_fxVelocityOriginal.store(nullptr, std::memory_order_release);
    }
}
