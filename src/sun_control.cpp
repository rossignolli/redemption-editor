// The sky object's moon for the recorded moon track (sun_control.h).

#include "sun_control.h"

#include "guarded_memory.h"

#include <windows.h>

#include <cmath>

namespace replay::sunctl
{
    namespace
    {
        // Build 1491.50 dump, disassembled. Every site below is checked
        // before the object is read or written (a mismatch: no moon track).
        // The update of the sky object:
        //   0145781C  48 8B C4              mov rax, rsp
        //   0145781F  48 89 58 08           mov [rax+8], rbx
        //   01457823  48 89 70 10           mov [rax+10h], rsi
        //   01457827  48 89 78 18           mov [rax+18h], rdi
        //   0145782B  55                    push rbp
        //   0145782C  41 56                 push r14
        //   0145782E  41 57                 push r15
        //   01457830  48 8D A8 18 FF FF FF  lea rbp, [rax-0E8h]
        //   01457837  48 81 EC D0 01 00 00  sub rsp, 1D0h
        constexpr std::uintptr_t kUpdateRva = 0x145781C;
        constexpr std::uint8_t kUpdateSignature[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x48,
            0x89, 0x78, 0x18, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xA8, 0x18, 0xFF, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xD0,
            0x01, 0x00, 0x00};
        // Its only caller, the thunk the update group runs (registered by
        // the protected code at 0x64D84D0): the object is 0x4F3E4A0.
        //   01472430  48 8D 0D 69 C0 AC 03  lea rcx, [0x4F3E4A0]
        //   01472437  E9 E0 53 FE FF        jmp 0x145781C
        constexpr std::uintptr_t kThunkRva = 0x1472430;
        constexpr std::uint8_t kThunkSignature[] = {0x48, 0x8D, 0x0D, 0x69, 0xC0, 0xAC, 0x03, 0xE9, 0xE0, 0x53, 0xFE, 0xFF};
        constexpr std::uintptr_t kObjectRva = 0x4F3E4A0;
        // The fields, each proven by an instruction of the update (rdi is
        // the object) or of the readers:
        //   01457A6C  F3 0F 6F 57 50              movdqu xmm2, [rdi+50h]   (sun, horizon test)
        //   01457A7D  0F 59 A7 60 02 00 00        mulps xmm4, [rdi+260h]   (moon, horizon test)
        //   01457AE3  F2 0F 10 87 80 0A 00 00     movsd xmm0, [rdi+0A80h]  (moon turn angle)
        //   01457AFD  F2 0F 11 87 80 0A 00 00     movsd [rdi+0A80h], xmm0
        //   01457F26  0F 11 97 90 0A 00 00        movups [rdi+0A90h], xmm2 (sun, last write)
        //   01458047  0F 11 97 A0 0A 00 00        movups [rdi+0AA0h], xmm2 (moon, last write)
        //   005F817D  66 0F 6F 05 6B 63 94 04     movdqa xmm0, [0x4F3E4F0] (shadows: object+50h)
        //   005F8187  66 0F 6F 05 71 65 94 04     movdqa xmm0, [0x4F3E700] (shadows: object+260h)
        struct Site
        {
            std::uintptr_t rva;
            std::uint8_t bytes[8];
            std::size_t size;
            const char* name;
        };
        const Site kSites[] = {
            {0x1457A6C, {0xF3, 0x0F, 0x6F, 0x57, 0x50}, 5, "sun_read"},
            {0x1457A7D, {0x0F, 0x59, 0xA7, 0x60, 0x02, 0x00, 0x00}, 7, "moon_read"},
            {0x1457AE3, {0xF2, 0x0F, 0x10, 0x87, 0x80, 0x0A, 0x00, 0x00}, 8, "angle_read"},
            {0x1457AFD, {0xF2, 0x0F, 0x11, 0x87, 0x80, 0x0A, 0x00, 0x00}, 8, "angle_write"},
            // The moon's orbit angle, added to every frame and written back:
            //   01457D96  F2 0F 58 87 E8 08 00 00     addsd xmm0, [rdi+8E8h]
            //   01457DA3  F2 0F 11 87 E8 08 00 00     movsd [rdi+8E8h], xmm0
            {0x1457D96, {0xF2, 0x0F, 0x58, 0x87, 0xE8, 0x08, 0x00, 0x00}, 8, "orbit_read"},
            {0x1457DA3, {0xF2, 0x0F, 0x11, 0x87, 0xE8, 0x08, 0x00, 0x00}, 8, "orbit_write"},
            {0x1457F26, {0x0F, 0x11, 0x97, 0x90, 0x0A, 0x00, 0x00}, 7, "sun_write"},
            {0x1458047, {0x0F, 0x11, 0x97, 0xA0, 0x0A, 0x00, 0x00}, 7, "moon_write"},
            {0x5F817D, {0x66, 0x0F, 0x6F, 0x05, 0x6B, 0x63, 0x94, 0x04}, 8, "shadow_sun"},
            {0x5F8187, {0x66, 0x0F, 0x6F, 0x05, 0x71, 0x65, 0x94, 0x04}, 8, "shadow_moon"},
            // Kept as a fingerprint of the build (they were the probe's): the
            // constructor's stores of latitude and the day, and the sky
            // update's reads of the timecycle's sun and moon.
            //   014086F8  C7 81 DC 08 00 00 ...       mov [rcx+8DCh], 34.09
            //   01408713  C7 81 F0 08 00 00 50 ...    mov [rcx+8F0h], 80
            //   005F90A5  0F 28 05 64 FA 49 04        movaps xmm0, [0x4A98B10]
            //   005F911B  0F 28 05 FE F9 49 04        movaps xmm0, [0x4A98B20]
            {0x14086F8, {0xC7, 0x81, 0xDC, 0x08, 0x00, 0x00}, 6, "latitude_init"},
            {0x1408713, {0xC7, 0x81, 0xF0, 0x08, 0x00, 0x00, 0x50}, 7, "day_init"},
            {0x5F90A5, {0x0F, 0x28, 0x05, 0x64, 0xFA, 0x49, 0x04}, 7, "timecycle_sun"},
            {0x5F911B, {0x0F, 0x28, 0x05, 0xFE, 0xF9, 0x49, 0x04}, 7, "timecycle_moon"},
        };
        constexpr std::uintptr_t kSun = 0xA90;
        constexpr std::uintptr_t kMoon = 0xAA0;
        constexpr std::uintptr_t kMoonAngle = 0xA80;
        constexpr std::uintptr_t kMoonOrbit = 0x8E8;

        std::uintptr_t g_base = 0;
        std::uintptr_t g_object = 0;
        int g_resolved = 0; // 0 not tried, 1 ok, -1 mismatch
        const char* g_resolveError = "";

        bool Matches(std::uintptr_t rva, const std::uint8_t* bytes, std::size_t size)
        {
            return replay::mem::Matches(reinterpret_cast<const void*>(g_base + rva), bytes, size);
        }

        bool Resolve()
        {
            if (g_resolved)
                return g_resolved > 0;
            g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            g_resolved = -1;
            if (!Matches(kUpdateRva, kUpdateSignature, sizeof(kUpdateSignature)))
            {
                g_resolveError = "update_signature_mismatch";
                return false;
            }
            if (!Matches(kThunkRva, kThunkSignature, sizeof(kThunkSignature)))
            {
                g_resolveError = "thunk_signature_mismatch";
                return false;
            }
            for (const Site& site : kSites)
            {
                if (!Matches(site.rva, site.bytes, site.size))
                {
                    g_resolveError = site.name;
                    return false;
                }
            }
            g_object = g_base + kObjectRva;
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
