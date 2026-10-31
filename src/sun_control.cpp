// The sky object's moon for the recorded moon track (sun_control.h).

#include "sun_control.h"

#include "engine_sigs.h"
#include "game_sig.h"
#include "guarded_memory.h"

#include <windows.h>

#include <cmath>

namespace replay::sunctl
{
    using enginesig::kSkyUpdate;

    namespace
    {
        namespace sig = replay::sig;

        // Every site below is checked before the object is read or written
        // (a mismatch: no moon track). The object: the sky group's setup
        // passes it to the sky init (lea rcx, [object]; call). The update's
        // only caller is a 12-byte thunk (lea rcx, [object]; jmp) too short
        // to be found alone, so the update has its own anchor.
        const sig::Signature kGroup{"sun_sky_group",
            "48 83 EC 28 80 3D ?? ?? ?? ?? 00 74 3F E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 "
            "8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ??",
            0x1471E00};
        const sig::Signature kObject{"sun_sky_object", kGroup, sig::Rel32{0x26, 0x2A}, "", 0x4F3E4A0};
        const sig::Signature kSkyInit{"sun_sky_init", kGroup, sig::Rel32{0x2B, 0x2F},
            "48 8B C4 48 89 58 08 57 48 83 EC 70 F3 0F 10 15 ?? ?? ?? ?? 48 8D 3D ?? ?? ?? ??", 0x1471E54};
        const sig::Signature kUpdate{"sun_update",
            "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 41 56 41 57 48 8D A8 18 FF FF FF 48 81 EC D0 01 00 00 0F "
            "29 70 D8",
            0x145781C};
        // The fields, each proven by an instruction of the update (rdi is
        // the object) or of the readers:
        //   movdqu xmm2, [rdi+50h]   (sun, horizon test)
        //   mulps xmm4, [rdi+260h]   (moon, horizon test)
        //   movsd xmm0, [rdi+0A80h]  (moon turn angle), and its store
        //   addsd xmm0, [rdi+8E8h]   (the moon's orbit angle, added to every
        //                             frame and written back)
        //   movups [rdi+0A90h], xmm2 (sun, last write)
        //   movups [rdi+0AA0h], xmm2 (moon, last write)
        const sig::Signature kSunRead{"sun_read", kUpdate, sig::Offset{0x250}, "F3 0F 6F 57 50", 0x1457A6C};
        const sig::Signature kMoonRead{"sun_moon_read", kUpdate, sig::Offset{0x261}, "0F 59 A7 60 02 00 00", 0x1457A7D};
        const sig::Signature kAngleRead{"sun_angle_read", kUpdate, sig::Offset{0x2C7},
            "F2 0F 10 87 80 0A 00 00", 0x1457AE3};
        const sig::Signature kAngleWrite{"sun_angle_write", kUpdate, sig::Offset{0x2E1},
            "F2 0F 11 87 80 0A 00 00", 0x1457AFD};
        const sig::Signature kOrbitRead{"sun_orbit_read", kUpdate, sig::Offset{0x57A},
            "F2 0F 58 87 E8 08 00 00", 0x1457D96};
        const sig::Signature kOrbitWrite{"sun_orbit_write", kUpdate, sig::Offset{0x587},
            "F2 0F 11 87 E8 08 00 00", 0x1457DA3};
        const sig::Signature kSunWrite{"sun_write", kUpdate, sig::Offset{0x70A}, "0F 11 97 90 0A 00 00", 0x1457F26};
        const sig::Signature kMoonWrite{"sun_moon_write", kUpdate, sig::Offset{0x82B},
            "0F 11 97 A0 0A 00 00", 0x1458047};
        // The shadows read the object's sun (+50h) and moon (+260h) by
        // address: movdqa xmm0, [rip] twice.
        const sig::Signature kShadows{"sun_shadows", "66 0F 6F 05 ?? ?? ?? ?? EB 08 66 0F 6F 05 ?? ?? ?? ??", 0x5F817D};
        const sig::Signature kShadowSun{"sun_shadow_sun", kShadows, sig::Rel32{0x04, 0x08}, "", 0x4F3E4F0};
        const sig::Signature kShadowMoon{"sun_shadow_moon", kShadows, sig::Rel32{0x0E, 0x12}, "", 0x4F3E700};
        // Kept as a fingerprint of the build (they were the probe's): the
        // constructor's stores of latitude (+8DCh) and the day (+8F0h, 80),
        // and the sky update's reads of the timecycle's sun and moon (two
        // movaps xmm0, [rip], 0x10 apart).
        const sig::Signature kInit{"sun_latitude_day_init",
            "C7 81 DC 08 00 00 29 5C 08 42 C7 81 E0 08 00 00 7B 14 BD C2 4C 89 99 E8 08 00 00 C7 81 F0 08 00 00 50 00 "
            "00 00",
            0x14086F8};
        const sig::Signature kTimecycleSunRead{"sun_timecycle_sun_read", kSkyUpdate, sig::Offset{0xA65},
            "0F 28 05 ?? ?? ?? ??", 0x5F90A5};
        const sig::Signature kTimecycleSun{"sun_timecycle_sun", kTimecycleSunRead, sig::Rel32{0x03, 0x07}, "",
            0x4A98B10};
        const sig::Signature kTimecycleMoonRead{"sun_timecycle_moon_read", kSkyUpdate, sig::Offset{0xADB},
            "0F 28 05 ?? ?? ?? ??", 0x5F911B};
        const sig::Signature kTimecycleMoon{"sun_timecycle_moon", kTimecycleMoonRead, sig::Rel32{0x03, 0x07}, "",
            0x4A98B20};

        constexpr std::uintptr_t kSun = 0xA90;
        constexpr std::uintptr_t kMoon = 0xAA0;
        constexpr std::uintptr_t kMoonAngle = 0xA80;
        constexpr std::uintptr_t kMoonOrbit = 0x8E8;
        constexpr std::uintptr_t kShadowSunField = 0x50;
        constexpr std::uintptr_t kShadowMoonField = 0x260;

        std::uintptr_t g_object = 0;
        int g_resolved = 0; // 0 not tried, 1 ok, -1 mismatch
        const char* g_resolveError = "";

        bool Resolve()
        {
            if (g_resolved)
                return g_resolved > 0;
            g_resolved = -1;
            if (!sig::Find(kSkyInit))
            {
                g_resolveError = "sky_group_signature_mismatch";
                return false;
            }
            if (!sig::Find(kUpdate))
            {
                g_resolveError = "update_signature_mismatch";
                return false;
            }
            const std::uintptr_t object = sig::Find(kObject);
            const std::uintptr_t timecycleSun = sig::Find(kTimecycleSun);
            // A site found, and for a read by address the object (or the
            // timecycle's sun) it must reach.
            struct Site
            {
                bool ok;
                const char* name; // the log's
            };
            const Site sites[] = {
                {sig::Find(kSunRead) != 0, "sun_read"},
                {sig::Find(kMoonRead) != 0, "moon_read"},
                {sig::Find(kAngleRead) != 0, "angle_read"},
                {sig::Find(kAngleWrite) != 0, "angle_write"},
                {sig::Find(kOrbitRead) != 0, "orbit_read"},
                {sig::Find(kOrbitWrite) != 0, "orbit_write"},
                {sig::Find(kSunWrite) != 0, "sun_write"},
                {sig::Find(kMoonWrite) != 0, "moon_write"},
                {object && sig::Find(kShadowSun) == object + kShadowSunField, "shadow_sun"},
                {object && sig::Find(kShadowMoon) == object + kShadowMoonField, "shadow_moon"},
                {sig::Find(kInit) != 0, "latitude_init"},
                {sig::Find(kInit) != 0, "day_init"},
                {timecycleSun != 0, "timecycle_sun"},
                {timecycleSun && sig::Find(kTimecycleMoon) == timecycleSun + 0x10, "timecycle_moon"},
            };
            for (const Site& site : sites)
            {
                if (!site.ok)
                {
                    g_resolveError = site.name;
                    return false;
                }
            }
            g_object = object;
            g_resolved = 1;
            return true;
        }

        bool ReadVec(std::uintptr_t address, Vec3f& v)
        {
            float f[4]{};
            if (!replay::mem::Copy(f, address, sizeof(f)))
                return false;
            v = {f[0], f[1], f[2]};
            return std::isfinite(f[0]) && std::isfinite(f[1]) && std::isfinite(f[2]);
        }
    }

    // Script fiber, guarded.
    const char* ResolveError()
    {
        return Resolve() ? "none" : g_resolveError;
    }

    bool ReadMoon(MoonState& out)
    {
        if (!Resolve())
            return false;
        MoonState s;
        if (!replay::mem::Read(g_object + kMoonAngle, s.angle) || !replay::mem::Read(g_object + kMoonOrbit, s.orbit))
            return false;
        if (!std::isfinite(s.angle) || !std::isfinite(s.orbit))
            return false;
        s.directions = ReadVec(g_object + kSun, s.sun) && ReadVec(g_object + kMoon, s.moon);
        out = s;
        return true;
    }

    bool WriteMoon(double angle, double orbit)
    {
        if (!Resolve() || !std::isfinite(angle) || !std::isfinite(orbit))
            return false;
        // Both fields first read: a page that stopped being readable is
        // never written.
        double old = 0.0;
        if (!replay::mem::Read(g_object + kMoonAngle, old) || !replay::mem::Read(g_object + kMoonOrbit, old))
            return false;
        const bool a = replay::mem::Put(g_object + kMoonAngle, &angle, sizeof(angle));
        const bool o = replay::mem::Put(g_object + kMoonOrbit, &orbit, sizeof(orbit));
        return a && o;
    }
}
