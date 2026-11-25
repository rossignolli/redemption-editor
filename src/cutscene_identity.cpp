#include "cutscene_identity.h"

#include "guarded_memory.h"
#include "qa_bundle.h"

#include <cstdint>
#include <cstring>

#include <Windows.h>

namespace replay::cutsceneidentity
{
    namespace
    {
        // RDR2.asl mission/cutscene globals, verified against game build
        // 1.0.1491.50 in capture_probe.cpp and the attached replay reports.
        constexpr std::uintptr_t kMissionNameRva = 0x528DDC0;
        constexpr std::uintptr_t kCutsceneContextRva = 0x49F8648;
        constexpr std::uintptr_t kCutsceneFlagOffset = 0xB208;
        constexpr std::uintptr_t kCutsceneNameOffset = 0xB210;

        bool ReadToken(std::uintptr_t address, char* out)
        {
            bool terminated = false;
            std::size_t written = 0;
            for (std::size_t i = 0; i < 63; ++i)
            {
                char c = 0;
                if (!replay::mem::Copy(&c, address + i, 1))
                    return i != 0;
                if (!c)
                {
                    terminated = true;
                    break;
                }
                const unsigned char u = static_cast<unsigned char>(c);
                if ((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '_' || u == '-')
                    out[written++] = c;
                else
                    out[written++] = '_';
            }
            out[written] = '\0';
            return terminated || written == 63;
        }
    }

    bool Read(Snapshot& snapshot)
    {
        static const bool supportedBuild = replay::qa::bundle::GameExeVersion() == "1.0.1491.50";
        snapshot = {};
        if (!supportedBuild)
            return false;

        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (!base)
            return false;
        std::uintptr_t context = 0;
        std::uint8_t active = 0;
        if (!ReadToken(base + kMissionNameRva, snapshot.mission) ||
            !replay::mem::Read(base + kCutsceneContextRva, context) || !context ||
            !replay::mem::Read(context + kCutsceneFlagOffset, active) ||
            !ReadToken(context + kCutsceneNameOffset, snapshot.cutscene))
        {
            snapshot = {};
            return false;
        }
        snapshot.inCutscene = active != 0;
        return true;
    }
}
