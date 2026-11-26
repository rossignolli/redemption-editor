#include "lightning_state.h"
#include "game_sig.h"
#include "guarded_memory.h"

#include <cmath>
#include <cstddef>

namespace replay::lightningstate
{
    namespace
    {
        namespace sig = replay::sig;

        // Build 1491.50 (the capture probe's probe 16): the manager is the
        // static object the coordinate port's wrapper loads into rcx; the type,
        // request, forced type and position are the fields the manager update
        // names by their offsets.
        constexpr std::uintptr_t kTypeOffset = 0x6E00;
        constexpr std::uintptr_t kRequestOffset = 0x6DE0;
        constexpr std::uintptr_t kPositionOffset = 0x6E50;

        // lea rcx, [manager] at the _FORCE_LIGHTNING_FLASH_AT_COORDS wrapper.
        const sig::Signature kManagerLea{"lightning_manager_lea", "48 8D 0D 49 A2 7F 04", 0x733E90};
        const sig::Signature kManager{"lightning_manager", kManagerLea, sig::Rel32{0x03, 0x07}, "", 0x4F2E0E0};
        // The manager update's own instruction per field (mov [rbx+6E00], ecx),
        // and the coordinate port's store of the position ([rcx+6E50]).
        const sig::Signature kTypeField{"lightning_type_field", "89 8B 00 6E 00 00", 0x1459C9B};
        const sig::Signature kPositionField{"lightning_position_field", "0F 11 81 50 6E 00 00", 0x14192A8};

        bool g_checked = false;
        bool g_ok = false;
        std::string g_error;
        std::uintptr_t g_manager = 0;

        bool Check(std::string& error)
        {
            const std::uintptr_t manager = sig::Find(kManager);
            if (!sig::Find(kManagerLea))
                error = "manager_lea_mismatch";
            else if (!manager)
                error = "manager_mismatch";
            else if (!sig::Find(kTypeField))
                error = "type_field_mismatch";
            else if (!sig::Find(kPositionField))
                error = "position_field_mismatch";
            else
            {
                g_manager = manager;
                return true;
            }
            return false;
        }
    }

    bool Resolve(std::string& error)
    {
        if (!g_checked)
        {
            g_checked = true;
            g_ok = Check(g_error);
        }
        error = g_error;
        return g_ok;
    }

    bool Ready() { return g_ok; }

    bool Read(Snapshot& out)
    {
        if (!g_ok)
            return false;
        std::int32_t type = 0;
        std::uint8_t request = 0;
        if (!mem::Read(g_manager + kTypeOffset, type) || !mem::Read(g_manager + kRequestOffset, request))
            return false;
        out.type = type >= 0 && type <= 3 ? type : 0;
        out.request = request != 0;
        out.positionKnown = false;
        float position[4] = {};
        if (out.type != 0 && mem::Copy(position, g_manager + kPositionOffset, sizeof(position)) &&
            std::isfinite(position[0]) && std::isfinite(position[1]) && std::isfinite(position[2]) &&
            std::fabs(position[0]) + std::fabs(position[1]) + std::fabs(position[2]) > 0.01f)
        {
            out.position[0] = position[0];
            out.position[1] = position[1];
            out.position[2] = position[2];
            out.positionKnown = true;
        }
        return true;
    }
}
