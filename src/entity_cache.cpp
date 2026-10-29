#include "entity_cache.h"

#include "main.h"
#include "natives.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <sstream>

// See entity_cache.h.
namespace replay::entities
{
    namespace
    {
        // _HAS_PED_TAKEN_GORE_DAMAGE's getter 0xB4498C (the rip
        // displacements pin the count 0x497CB88 and the table 0x497CB80)
        // and its core 0xFD66F4 (`bt [entry+0x18], limb`). Build 1491.50.
        constexpr std::uintptr_t kGoreTableRva = 0x497CB80;
        constexpr std::uintptr_t kGoreCountRva = 0x497CB88;
        constexpr std::uintptr_t kGoreGetterRva = 0xB4498C;
        constexpr std::uint8_t kGoreGetterSignature[] = {
            0x33, 0xC0, 0x48, 0x85, 0xC9, 0x74, 0x2C, 0x0F, 0xB7, 0x15, 0xEE, 0x81, 0xE3, 0x03, 0x48, 0x85, 0xD2, 0x74,
            0x20, 0x4C, 0x8B, 0x15, 0xDA, 0x81, 0xE3, 0x03, 0x44, 0x8B, 0xC8, 0x4D, 0x8B, 0xC2, 0x49, 0x39, 0x08, 0x74,
            0x11, 0xFF, 0xC0, 0x49, 0xFF, 0xC1, 0x49, 0x83, 0xC0, 0x20, 0x4C, 0x3B, 0xCA, 0x7C, 0xED, 0x33, 0xC0, 0xC3,
            0x8B, 0xC0, 0x48, 0xC1, 0xE0, 0x05, 0x49, 0x03, 0xC2, 0xC3};
        constexpr std::uintptr_t kGoreCoreRva = 0xFD66F4;
        constexpr std::uint8_t kGoreCoreSignature[] = {
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0xDA, 0x48, 0x85, 0xC9, 0x74, 0x15, 0xE8, 0x86, 0xE2, 0xB6, 0xFF,
            0x48, 0x85, 0xC0, 0x74, 0x0B, 0x8B, 0x40, 0x18, 0x0F, 0xA3, 0xD8, 0x0F, 0x92, 0xC0, 0xEB, 0x02, 0x32, 0xC0,
            0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3};

        DWORD g_scriptThread = 0;
        std::uint64_t g_frame = 0;
        pools::PoolHeader g_guids;
        bool g_guidsOk = false;
        bool g_checked = false;
        bool g_signaturesOk = false;
        bool g_goreSignatureOk = false;
        std::string g_error;
        std::uintptr_t g_base = 0;
        CrossCheck g_resolveCheck;
        CrossCheck g_positionCheck;
        CrossCheck g_goreCheck;
        std::uint32_t g_mismatchLogs = 0;
        Stats g_stats;
        FrameTable<bool> g_frozen;
        std::vector<GoreEntry> g_gore;
        std::uint64_t g_goreFrame = ~0ull;
        bool g_goreOk = false;
        std::vector<std::uint8_t> g_goreBytes;

        bool GuardedByte(std::uint64_t address, std::uint8_t& out)
        {
            __try
            {
                out = *reinterpret_cast<const volatile std::uint8_t*>(address);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool GuardedQword(std::uint64_t address, std::uint64_t& out)
        {
            __try
            {
                out = *reinterpret_cast<const volatile std::uint64_t*>(address);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool GuardedCopy(void* out, std::uint64_t from, std::size_t size)
        {
            __try
            {
                std::memcpy(out, reinterpret_cast<const void*>(from), size);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool GuardedCompare(std::uintptr_t target, const std::uint8_t* signature, std::size_t size)
        {
            __try
            {
                return std::memcmp(reinterpret_cast<const void*>(target), signature, size) == 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        void Log(const std::string& line)
        {
            // The mod's log is owned by script.cpp; the cache reports through
            // its summary lines. Mismatches are rare and kept for them.
            if (g_error.size() < 600)
                g_error += line;
        }

        void CheckOnce()
        {
            if (g_checked)
                return;
            g_checked = true;
            g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            std::string poolError;
            g_signaturesOk = pools::Check(poolError);
            if (!g_signaturesOk)
                g_error = "pools:" + poolError;
            g_goreSignatureOk = g_base &&
                GuardedCompare(g_base + kGoreGetterRva, kGoreGetterSignature, sizeof(kGoreGetterSignature)) &&
                GuardedCompare(g_base + kGoreCoreRva, kGoreCoreSignature, sizeof(kGoreCoreSignature));
        }

        bool OnScriptThread() { return g_scriptThread && GetCurrentThreadId() == g_scriptThread; }

        bool MemoryPath() { return g_guidsOk && !g_resolveCheck.disabled && OnScriptThread(); }

        // The entity of a handle through memory, and its type byte.
        std::uint64_t ResolveMemory(std::int32_t handle, std::uint8_t& type)
        {
            type = 0;
            const std::uint64_t entity = ResolveGuid(g_guids, handle, GuardedByte, GuardedQword);
            if (!entity || !GuardedByte(entity + pools::kEntityType, type))
                return 0;
            return entity;
        }

        bool KnownType(std::uint8_t type)
        {
            return type == pools::kTypePed || type == pools::kTypeVehicle || type == pools::kTypeObject;
        }

        void NoteMismatch(const char* what, std::int32_t handle, std::uint64_t memory, std::uint64_t native)
        {
            ++g_stats.mismatches;
            if (g_mismatchLogs < 8)
            {
                ++g_mismatchLogs;
                std::ostringstream line;
                line << ';' << what << ":handle=" << handle << "|memory=0x" << std::hex << memory << "|native=0x" << native
                     << std::dec;
                Log(line.str());
            }
        }

        // Resolves through memory, cross-checking a sample with the natives.
        // `nativePath` true: the answer must come from the natives.
        std::uint64_t Resolve(std::int32_t handle, std::uint8_t& type, bool& nativePath)
        {
            nativePath = !MemoryPath();
            if (nativePath)
            {
                ++g_stats.nativeResolves;
                return 0;
            }
            // The mod uses 0 for "no entity" everywhere; never a lookup.
            if (handle <= 0)
                return 0;
            const std::uint64_t entity = ResolveMemory(handle, type);
            if (entity && !KnownType(type))
            {
                // A guid of something that is not a ped, vehicle or object:
                // the native decides.
                nativePath = true;
                ++g_stats.nativeResolves;
                return 0;
            }
            ++g_stats.memoryResolves;
            if (g_resolveCheck.Due())
            {
                // The ScriptHook's own lookup (an export, not a native: safe
                // wherever the old call was); null when the entity is gone.
                const auto native = reinterpret_cast<std::uint64_t>(getScriptHandleBaseAddress(handle));
                bool match = native == entity;
                // The ScriptHook export can answer an address for a handle
                // whose slot the memory path finds free or of another
                // generation. The engine's own DOES_ENTITY_EXIST decides (one
                // native, only on a difference): gone means the memory path
                // was right (the export does not check the generation), and it
                // is not a mismatch.
                if (!match && entity == 0 && native != 0 && ENTITY::DOES_ENTITY_EXIST(handle) == FALSE)
                {
                    match = true;
                    ++g_stats.exportStale;
                    if (g_mismatchLogs < 8)
                    {
                        ++g_mismatchLogs;
                        std::ostringstream line;
                        line << ";export_stale:handle=" << handle << "|native=0x" << std::hex << native << std::dec;
                        Log(line.str());
                    }
                }
                if (!match)
                    NoteMismatch("resolve", handle, entity, native);
                g_resolveCheck.Note(match);
                if (!match)
                {
                    nativePath = true;
                    return 0;
                }
            }
            return entity;
        }
    }

    void BeginFrame()
    {
        CheckOnce();
        g_scriptThread = GetCurrentThreadId();
        ++g_frame;
        ++g_stats.frames;
        g_guidsOk = g_signaturesOk && pools::GuidHeader(g_guids);
        g_frozen.BeginFrame(g_frame);
    }

    std::string Describe()
    {
        CheckOnce();
        std::ostringstream out;
        out << "ENTITY_CACHE,status=" << (g_signaturesOk && pools::Available() ? "memory" : "natives")
            << ",gore=" << (g_goreSignatureOk ? "memory" : "natives");
        if (!g_signaturesOk)
            out << ",error=" << g_error;
        return out.str();
    }

    bool Exists(std::int32_t handle)
    {
        std::uint8_t type = 0;
        bool nativePath = false;
        const std::uint64_t entity = Resolve(handle, type, nativePath);
        if (nativePath)
            return ENTITY::DOES_ENTITY_EXIST(handle) != FALSE;
        return entity != 0;
    }

    std::uint8_t* AddressOf(std::int32_t handle)
    {
        std::uint8_t type = 0;
        bool nativePath = false;
        const std::uint64_t entity = Resolve(handle, type, nativePath);
        if (nativePath)
            return getScriptHandleBaseAddress(handle);
        return reinterpret_cast<std::uint8_t*>(entity);
    }

    namespace
    {
        bool IsType(std::int32_t handle, std::uint8_t wanted, bool (*native)(Entity))
        {
            std::uint8_t type = 0;
            bool nativePath = false;
            const std::uint64_t entity = Resolve(handle, type, nativePath);
            if (nativePath)
                return native(handle);
            return entity && type == wanted;
        }
    }

    bool IsPed(std::int32_t handle)
    {
        return IsType(handle, pools::kTypePed, [](Entity e) { return ENTITY::IS_ENTITY_A_PED(e) != FALSE; });
    }

    bool IsVehicle(std::int32_t handle)
    {
        return IsType(handle, pools::kTypeVehicle, [](Entity e) { return ENTITY::IS_ENTITY_A_VEHICLE(e) != FALSE; });
    }

    bool Position(std::int32_t handle, float out[3])
    {
        out[0] = out[1] = out[2] = 0.0f;
        ++g_stats.positions;
        std::uint8_t type = 0;
        bool nativePath = false;
        const std::uint64_t entity = Resolve(handle, type, nativePath);
        std::uint8_t bytes[16]{};
        if (!nativePath && !g_positionCheck.disabled)
        {
            if (!entity)
                return false;
            if (GuardedCopy(bytes, entity + pools::kEntityPosition, sizeof(bytes)))
            {
                const pools::Position at = pools::DecodePosition(bytes);
                out[0] = at.x;
                out[1] = at.y;
                out[2] = at.z;
                if (!g_positionCheck.Due())
                    return true;
                const Vector3 native = ENTITY::GET_ENTITY_COORDS(handle, TRUE, FALSE);
                const float dx = native.x - at.x, dy = native.y - at.y, dz = native.z - at.z;
                bool match = dx * dx + dy * dy + dz * dz <= 0.01f * 0.01f;
                // With realCoords false the native answers a mounted or seated
                // ped with its mount's or vehicle's position; +0x70 is the
                // ped's own. A memory value equal to the real coordinates is
                // right: the ped's own position is what the cache returns (one
                // more native, only on a difference).
                if (!match)
                {
                    const Vector3 real = ENTITY::GET_ENTITY_COORDS(handle, TRUE, TRUE);
                    const float rx = real.x - at.x, ry = real.y - at.y, rz = real.z - at.z;
                    if (rx * rx + ry * ry + rz * rz <= 0.01f * 0.01f)
                    {
                        match = true;
                        ++g_stats.positionReal;
                    }
                }
                if (!match)
                    NoteMismatch("position", handle, entity, static_cast<std::uint64_t>(std::fabs(dx) + std::fabs(dy) + std::fabs(dz)));
                g_positionCheck.Note(match);
                if (match)
                    return true;
            }
        }
        if (!ENTITY::DOES_ENTITY_EXIST(handle))
            return false;
        const Vector3 at = ENTITY::GET_ENTITY_COORDS(handle, TRUE, FALSE);
        out[0] = at.x;
        out[1] = at.y;
        out[2] = at.z;
        return true;
    }

    bool Frozen(std::int32_t handle)
    {
        if (OnScriptThread())
        {
            if (const bool* cached = g_frozen.Find(handle))
            {
                ++g_stats.frozenHits;
                return *cached;
            }
        }
        ++g_stats.frozenNatives;
        const bool frozen = ENTITY::_IS_ENTITY_FROZEN(handle) != FALSE;
        if (OnScriptThread())
            g_frozen.Put(handle, frozen);
        return frozen;
    }

    void NoteFrozen(std::int32_t handle, bool frozen)
    {
        if (OnScriptThread())
            g_frozen.Put(handle, frozen);
    }

    std::int32_t HandleAtAddress(std::uint64_t address, bool& available)
    {
        available = pools::Available();
        if (!available || !address)
            return 0;
        pools::Kind kind = pools::Kind::Count;
        pools::Entry entry;
        if (!pools::EntryAt(address, kind, entry))
            return 0;
        return pools::HandleOf(kind, entry, true);
    }

    int ListNear(pools::Kind kind, const float center[3], float radius, int* handles, int capacity)
    {
        const pools::Position at{center[0], center[1], center[2]};
        return pools::ListHandles(kind, &at, 1, radius, radius, capacity, handles, capacity);
    }

    int ReadPool(pools::Kind kind, std::vector<pools::Entry>& out)
    {
        out.clear();
        if (!pools::Available() || kind == pools::Kind::Count)
            return -1;
        const pools::PoolState state = pools::State(kind);
        if (!state.ok)
            return -1;
        out.resize(static_cast<std::size_t>(state.size));
        const int count = pools::Read(kind, out.data(), static_cast<int>(out.size()));
        out.resize(static_cast<std::size_t>(count));
        return count;
    }

    bool GoreMask(const void* ped, std::uint32_t& mask, bool& available)
    {
        mask = 0;
        CheckOnce();
        // EntityPoolReader=0 (the ini's kill switch) sends the gore poll back
        // to the natives too.
        available = g_goreSignatureOk && !g_goreCheck.disabled && pools::Enabled() && OnScriptThread();
        if (!available || !ped)
            return false;
        if (g_goreFrame != g_frame)
        {
            g_goreFrame = g_frame;
            g_gore.clear();
            std::uint64_t table = 0;
            std::uint16_t count = 0;
            g_goreOk = GuardedQword(g_base + kGoreTableRva, table) &&
                GuardedCopy(&count, g_base + kGoreCountRva, sizeof(count)) && count <= kGoreMaxEntries;
            if (g_goreOk && table && count)
            {
                g_goreBytes.resize(static_cast<std::size_t>(count) * kGoreEntryBytes);
                g_goreOk = GuardedCopy(g_goreBytes.data(), table, g_goreBytes.size());
                if (g_goreOk)
                    ParseGore(g_goreBytes.data(), count, g_gore);
            }
            ++g_stats.goreReads;
        }
        if (!g_goreOk)
        {
            available = false;
            return false;
        }
        return GoreMaskOf(g_gore, reinterpret_cast<std::uint64_t>(ped), mask);
    }

    bool GoreCheckDue() { return g_goreCheck.Due(); }

    void GoreNote(bool match, std::int32_t handle, int limb)
    {
        if (!match)
            NoteMismatch("gore", handle, static_cast<std::uint64_t>(limb), 0);
        g_goreCheck.Note(match);
    }

    Stats Snapshot()
    {
        Stats stats = g_stats;
        stats.checked = g_resolveCheck.checked + g_positionCheck.checked + g_goreCheck.checked;
        stats.memory = g_signaturesOk && !g_resolveCheck.disabled && g_guidsOk;
        stats.gore = g_goreSignatureOk && !g_goreCheck.disabled;
        return stats;
    }

    std::string SummaryLine(const char* phase)
    {
        const Stats stats = Snapshot();
        std::ostringstream out;
        out << "ENTITY_CACHE_TOTAL,phase=" << phase << ",memory=" << (stats.memory ? 1 : 0)
            << ",gore_memory=" << (stats.gore ? 1 : 0) << ",frames=" << stats.frames
            << ",memory_resolves=" << stats.memoryResolves << ",native_resolves=" << stats.nativeResolves
            << ",positions=" << stats.positions << ",frozen_natives=" << stats.frozenNatives
            << ",frozen_hits=" << stats.frozenHits << ",gore_reads=" << stats.goreReads
            << ",checked=" << stats.checked << ",mismatches=" << stats.mismatches
            << ",export_stale=" << stats.exportStale << ",position_real=" << stats.positionReal
            << ",resolve_off=" << (g_resolveCheck.disabled ? 1 : 0)
            << ",position_off=" << (g_positionCheck.disabled ? 1 : 0)
            << ",gore_off=" << (g_goreCheck.disabled ? 1 : 0);
        if (!g_error.empty())
            out << ",notes=" << g_error;
        return out.str();
    }
}
