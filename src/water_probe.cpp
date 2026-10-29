#include "water_probe.h"
#include "mod_identity.h" // RedemptionEditor.ini (old ReplayProbe.ini read too)
#include "guarded_memory.h"
#include "bench_paths.h"

#include "main.h"
#include "natives.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace replay::water::probe
{
    namespace
    {
        // Build 1491.50.
        constexpr std::uintptr_t kManagerPointerRva = 0x3F0D968; // water manager, 0x7C940 bytes (ctor 0x558FE0)
        constexpr std::uintptr_t kTuningPointerRva = 0x3F0D960;  // 0x13E0 bytes (0x5B2820)
        constexpr std::uintptr_t kSimRectRva = 0x453A520;        // minX, maxX, minY, maxY (0x6FDD84); +0x10..0x13 flags
        // The 0x13E0 block holds the Ocean (+0) and River (+0x9F0) render
        // resources (0x5B2820 loads "Ocean"/"River"). Seen in game: River +0x08
        // min x/y of the simulation area, River +0x238 a 128x128 grid of two u16
        // (0x8000 = rest) with ripple rings.
        constexpr std::uintptr_t kRiverRect = 0x9F0 + 0x8;
        constexpr std::uintptr_t kRiverGrid = 0x9F0 + 0x238;
        constexpr std::size_t kGridBytes = 0x10000;
        // The fixed RVAs above hold only for the build whose code matches these
        // sites; anything else leaves every read and write of this file off.
        //  - 0x5B2826: the water setup loads and stores the tuning global
        //    (rip + 0x395B133 = 0x3F0D960), allocates its 0x13E0 bytes and
        //    builds the River at +0x9F0 (ctor 0x558AE8);
        //  - 0x558B44: that ctor stores the River's vtable (rip + 0x2E65CB5 =
        //    0x33BE800), checked on the live object before each read of it;
        //  - 0x6E530A: a reader of the simulation rectangle (0x453A520).
        constexpr std::uintptr_t kSetupSiteRva = 0x5B2826;
        constexpr std::uint8_t kSetupSiteSignature[] = {0x48, 0x8B, 0x1D, 0x33, 0xB1, 0x95, 0x03, 0x48, 0x85, 0xDB, 0x75,
            0x69, 0xB9, 0xE0, 0x13, 0x00, 0x00, 0xE8, 0xE4, 0x3E, 0xB8, 0xFF, 0x48, 0x8B, 0xD8, 0x48, 0x85, 0xC0, 0x74, 0x16,
            0x48, 0x8B, 0xC8, 0xE8, 0x1C, 0x5D, 0xFA, 0xFF, 0x48, 0x8D, 0x8B, 0xF0, 0x09, 0x00, 0x00, 0xE8, 0x90, 0x62, 0xFA,
            0xFF, 0xEB, 0x02, 0x33, 0xDB, 0xB9, 0x40, 0xC9, 0x07, 0x00, 0x48, 0x89, 0x1D, 0xF8, 0xB0, 0x95, 0x03};
        constexpr std::uintptr_t kRiverVtableSiteRva = 0x558B44;
        constexpr std::uint8_t kRiverVtableSiteSignature[] = {0x48, 0x8D, 0x05, 0xB5, 0x5C, 0xE6, 0x02};
        constexpr std::uintptr_t kRiverVtableRva = 0x33BE800;
        constexpr std::uintptr_t kRiverObject = 0x9F0;
        constexpr std::uintptr_t kRectSiteRva = 0x6E530A;
        constexpr std::uint8_t kRectSiteSignature[] = {0xF3, 0x41, 0x0F, 0x58, 0xC0, 0x0F, 0x2F, 0x05, 0x0A, 0x52, 0xE5, 0x03};
        constexpr int kGridFiles = 120;
        constexpr std::size_t kManagerSize = 0x7C940;
        constexpr std::size_t kFollowBytes = 0x10000;
        constexpr std::size_t kFollowMax = 64;
        constexpr int kSnapshotFiles = 2;
        constexpr int kGrid = 5;
        constexpr float kSpacing = 2.0f;

        bool g_enabled = false;
        bool g_layoutOk = false; // the sites above matched
        const char* g_layoutError = "not_checked";
        unsigned long long g_nextGrid = 0;
        int g_gridFiles = 0;
        std::uint8_t g_grid[kGridBytes];
        float g_cx = -729.0f, g_cy = -500.0f, g_cz = 40.4f;
        std::uintptr_t g_base = 0;
        std::uintptr_t g_imageEnd = 0;

        unsigned long long g_nextProbe = 0, g_nextRect = 0, g_nextMem = 0, g_windowStart = 0;
        bool g_wasActive = false;
        float g_baseline[kGrid * kGrid] = {};
        bool g_haveBaseline = false;
        float g_min[kGrid * kGrid] = {}, g_max[kGrid * kGrid] = {};
        int g_hits[kGrid * kGrid] = {};

        struct Block
        {
            std::uintptr_t address = 0;
            std::uint32_t foundAt = 0xFFFFFFFF; // offset of the pointer in the manager
            std::vector<std::uint8_t> bytes;
        };
        std::vector<Block> g_previous;
        int g_snapshot = 0;

        bool GuardedCopy(void* out, const void* source, std::size_t size)
        {
            return replay::mem::Copy(out, source, size); // guarded_memory.h
        }

        std::uintptr_t ReadPointer(std::uintptr_t rva)
        {
            std::uintptr_t value = 0;
            return GuardedCopy(&value, reinterpret_cast<const void*>(g_base + rva), sizeof(value)) ? value : 0;
        }

        bool SiteMatches(std::uintptr_t rva, const std::uint8_t* signature, std::size_t size)
        {
            std::uint8_t bytes[96] = {};
            return size <= sizeof(bytes) && g_base + rva + size <= g_imageEnd &&
                GuardedCopy(bytes, reinterpret_cast<const void*>(g_base + rva), size) && std::memcmp(bytes, signature, size) == 0;
        }

        // The tuning block of this build, with the River object of this build
        // at +0x9F0 (its vtable), or 0.
        std::uintptr_t Tuning()
        {
            if (!g_layoutOk)
                return 0;
            const std::uintptr_t tuning = ReadPointer(kTuningPointerRva);
            std::uintptr_t vtable = 0;
            if (!tuning || !GuardedCopy(&vtable, reinterpret_cast<const void*>(tuning + kRiverObject), sizeof(vtable)) ||
                vtable != g_base + kRiverVtableRva)
                return 0;
            return tuning;
        }

        // Committed private read-write memory outside the module: how many
        // bytes from `address` (capped) can be read.
        std::size_t Readable(std::uintptr_t address)
        {
            if (address < 0x10000 || (address >= g_base && address < g_imageEnd) || (address & 7))
                return 0;
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)))
                return 0;
            if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
                !(info.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)))
                return 0;
            const std::uintptr_t end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
            return (std::min)(kFollowBytes, static_cast<std::size_t>(end - address));
        }

        std::vector<Block> TakeSnapshot()
        {
            std::vector<Block> blocks;
            if (!g_layoutOk)
                return blocks;
            const std::uintptr_t manager = ReadPointer(kManagerPointerRva);
            if (!manager)
                return blocks;
            Block root;
            root.address = manager;
            root.bytes.resize(kManagerSize);
            if (!GuardedCopy(root.bytes.data(), reinterpret_cast<const void*>(manager), kManagerSize))
                return blocks;
            std::map<std::uintptr_t, std::uint32_t> pointers;
            if (const std::uintptr_t tuning = Tuning())
                pointers.emplace(tuning, 0xFFFFFFFE);
            for (std::size_t at = 0; at + 8 <= kManagerSize && pointers.size() < kFollowMax; at += 8)
            {
                std::uintptr_t value = 0;
                std::memcpy(&value, root.bytes.data() + at, 8);
                if ((value >= manager && value < manager + kManagerSize) || pointers.count(value))
                    continue;
                if (Readable(value))
                    pointers.emplace(value, static_cast<std::uint32_t>(at));
            }
            blocks.push_back(std::move(root));
            for (const auto& [address, foundAt] : pointers)
            {
                const std::size_t size = Readable(address);
                if (!size)
                    continue;
                Block block;
                block.address = address;
                block.foundAt = foundAt;
                block.bytes.resize(size);
                if (GuardedCopy(block.bytes.data(), reinterpret_cast<const void*>(address), size))
                    blocks.push_back(std::move(block));
            }
            return blocks;
        }

        void SaveSnapshot(const std::vector<Block>& blocks, const char* phase, unsigned long long nowMs)
        {
            CreateDirectoryA(REPLAY_WATER_PROBE_DIR, nullptr);
            CreateDirectoryA(REPLAY_WATER_PROBE_DIR "\\snap", nullptr);
            char path[MAX_PATH]{};
            std::snprintf(path, sizeof(path), REPLAY_WATER_PROBE_DIR "\\snap\\%s-%02d-%llu.bin", phase, g_snapshot, nowMs);
            FILE* file = nullptr;
            if (fopen_s(&file, path, "wb") || !file)
                return;
            const std::uint32_t count = static_cast<std::uint32_t>(blocks.size());
            std::fwrite("WSNP", 1, 4, file);
            std::fwrite(&count, 4, 1, file);
            for (const Block& block : blocks)
            {
                const std::uint64_t address = block.address;
                const std::uint32_t size = static_cast<std::uint32_t>(block.bytes.size());
                std::fwrite(&address, 8, 1, file);
                std::fwrite(&size, 4, 1, file);
                std::fwrite(&block.foundAt, 4, 1, file);
                std::fwrite(block.bytes.data(), 1, size, file);
            }
            std::fclose(file);
        }

        std::uint32_t ChangedDwords(const std::uint8_t* a, const std::uint8_t* b, std::size_t size)
        {
            std::uint32_t changed = 0;
            for (std::size_t at = 0; at + 4 <= size; at += 4)
                changed += std::memcmp(a + at, b + at, 4) != 0;
            return changed;
        }

        void MemoryTick(std::ostream& log, const char* phase, unsigned long long nowMs)
        {
            std::vector<Block> blocks = TakeSnapshot();
            if (blocks.empty())
            {
                log << "WATER_MEM,phase=" << phase << ",manager=0\n";
                return;
            }
            if (g_snapshot < kSnapshotFiles)
                SaveSnapshot(blocks, phase, nowMs);
            ++g_snapshot;
            log << "WATER_MEM,phase=" << phase << ",snap=" << g_snapshot << ",blocks=" << blocks.size();
            if (!g_previous.empty() && g_previous.front().address == blocks.front().address)
            {
                // Manager pages (0x1000) with changes.
                const Block& now = blocks.front();
                const Block& before = g_previous.front();
                log << ",mgr_pages=";
                bool first = true;
                for (std::size_t page = 0; page < kManagerSize; page += 0x1000)
                {
                    const std::size_t size = (std::min)(static_cast<std::size_t>(0x1000), kManagerSize - page);
                    const std::uint32_t changed = ChangedDwords(now.bytes.data() + page, before.bytes.data() + page, size);
                    if (!changed)
                        continue;
                    log << (first ? "" : ";") << std::hex << "0x" << page << std::dec << ':' << changed;
                    first = false;
                }
                // Followed blocks by address.
                std::map<std::uintptr_t, const Block*> old;
                for (const Block& block : g_previous)
                    old.emplace(block.address, &block);
                log << ",heap=";
                first = true;
                for (std::size_t n = 1; n < blocks.size(); ++n)
                {
                    const Block& block = blocks[n];
                    const auto found = old.find(block.address);
                    if (found == old.end() || found->second->bytes.size() != block.bytes.size())
                        continue;
                    const std::uint32_t changed = ChangedDwords(block.bytes.data(), found->second->bytes.data(), block.bytes.size());
                    if (!changed)
                        continue;
                    log << (first ? "" : ";") << std::hex << "@0x" << block.foundAt << std::dec << ':' << changed << '/'
                        << block.bytes.size() / 4;
                    first = false;
                }
            }
            log << '\n';
            g_previous = std::move(blocks);
        }

        void RectTick(std::ostream& log, const char* phase)
        {
            float rect[4] = {};
            std::uint8_t flags[4] = {};
            const bool ok = g_layoutOk && GuardedCopy(rect, reinterpret_cast<const void*>(g_base + kSimRectRva), sizeof(rect)) &&
                GuardedCopy(flags, reinterpret_cast<const void*>(g_base + kSimRectRva + 0x10), sizeof(flags));
            const Vector3 cam = CAM::GET_FINAL_RENDERED_CAM_COORD();
            const Vector3 player = ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), TRUE, FALSE);
            log << "WATER_RECT,phase=" << phase << ",ok=" << (ok ? 1 : 0) << ",x=" << rect[0] << ';' << rect[1]
                << ",y=" << rect[2] << ';' << rect[3] << ",flags=" << int(flags[0]) << ';' << int(flags[1]) << ';'
                << int(flags[2]) << ';' << int(flags[3]) << ",cam=" << cam.x << ';' << cam.y << ';' << cam.z
                << ",player=" << player.x << ';' << player.y << ";" << player.z << '\n';
        }

        // The River grid: pointer, area, memory kind, a copy saved to disk.
        void GridTick(std::ostream& log, const char* phase, unsigned long long nowMs)
        {
            const std::uintptr_t tuning = Tuning();
            std::uintptr_t grid = 0;
            float area[4] = {};
            if (!tuning || !GuardedCopy(&grid, reinterpret_cast<const void*>(tuning + kRiverGrid), 8) ||
                !GuardedCopy(area, reinterpret_cast<const void*>(tuning + kRiverRect), sizeof(area)) || !grid)
            {
                log << "WATER_GRID,phase=" << phase << ",grid=0\n";
                return;
            }
            MEMORY_BASIC_INFORMATION info{};
            VirtualQuery(reinterpret_cast<const void*>(grid), &info, sizeof(info));
            const bool copied = GuardedCopy(g_grid, reinterpret_cast<const void*>(grid), kGridBytes);
            std::uint32_t active = 0, land = 0;
            std::int32_t lo = 0, hi = 0;
            for (std::size_t n = 0; copied && n < kGridBytes / 2; ++n)
            {
                std::uint16_t v = 0;
                std::memcpy(&v, g_grid + n * 2, 2);
                const std::int32_t d = static_cast<std::int32_t>(v) - 0x8000;
                if (!v)
                    ++land;
                else
                {
                    active += d > 16 || d < -16;
                    lo = (std::min)(lo, d);
                    hi = (std::max)(hi, d);
                }
            }
            log << "WATER_GRID,phase=" << phase << ",t=" << nowMs << ",copied=" << (copied ? 1 : 0) << ",area=" << area[0]
                << ';' << area[1] << ';' << area[2] << ';' << area[3] << ",type=0x" << std::hex << info.Type << ",protect=0x"
                << info.Protect << std::dec << ",region_kb=" << info.RegionSize / 1024 << ",offset_in_alloc=0x" << std::hex
                << (grid - reinterpret_cast<std::uintptr_t>(info.AllocationBase)) << std::dec << ",active=" << active
                << ",land=" << land << ",range=" << lo << ';' << hi << '\n';
            if (copied && g_gridFiles < kGridFiles)
            {
                CreateDirectoryA(REPLAY_WATER_PROBE_DIR, nullptr);
                CreateDirectoryA(REPLAY_WATER_PROBE_DIR "\\grid", nullptr);
                char path[MAX_PATH]{};
                std::snprintf(path, sizeof(path), REPLAY_WATER_PROBE_DIR "\\grid\\%s-%03d-%llu.bin", phase, g_gridFiles, nowMs);
                FILE* file = nullptr;
                if (!fopen_s(&file, path, "wb") && file)
                {
                    std::fwrite(area, sizeof(area), 1, file);
                    std::fwrite(g_grid, 1, kGridBytes, file);
                    std::fclose(file);
                }
                ++g_gridFiles;
            }
        }

        void ProbeTick(std::ostream& log, const char* phase, unsigned long long nowMs)
        {
            float heights[kGrid * kGrid] = {};
            int hits = 0;
            for (int j = 0; j < kGrid; ++j)
                for (int i = 0; i < kGrid; ++i)
                {
                    const float x = g_cx + (i - kGrid / 2) * kSpacing;
                    const float y = g_cy + (j - kGrid / 2) * kSpacing;
                    float h = 0.0f;
                    const int hit = WATER::TEST_VERTICAL_PROBE_AGAINST_ALL_WATER(x, y, g_cz + 20.0f, 0, &h);
                    heights[j * kGrid + i] = hit ? h : -1000.0f;
                    hits += hit ? 1 : 0;
                }
            float centre = 0.0f, centreNoWaves = 0.0f;
            const BOOL waves = WATER::GET_WATER_HEIGHT(g_cx, g_cy, g_cz + 20.0f, &centre);
            const BOOL flat = WATER::GET_WATER_HEIGHT_NO_WAVES(g_cx, g_cy, g_cz + 20.0f, &centreNoWaves);
            if (!g_haveBaseline)
            {
                std::memcpy(g_baseline, heights, sizeof(heights));
                g_haveBaseline = true;
            }
            log << "WATER_PROBE,phase=" << phase << ",t=" << nowMs << ",hits=" << hits << ",c=" << (waves ? centre : -1000.0f)
                << ",c_flat=" << (flat ? centreNoWaves : -1000.0f) << ",base=" << g_baseline[kGrid * kGrid / 2] << ",d_mm=";
            for (int n = 0; n < kGrid * kGrid; ++n)
            {
                const float h = heights[n];
                if (h > -999.0f)
                {
                    if (!g_hits[n]++)
                        g_min[n] = g_max[n] = h;
                    g_min[n] = (std::min)(g_min[n], h);
                    g_max[n] = (std::max)(g_max[n], h);
                }
                log << (n ? ";" : "") << (h > -999.0f ? static_cast<int>((h - g_baseline[n]) * 1000.0f) : -99999);
            }
            log << '\n';
        }

        void FlushWindow(std::ostream& log, const char* phase)
        {
            log << "WATER_PROBE_RANGE,phase=" << phase << ",range_mm=";
            for (int n = 0; n < kGrid * kGrid; ++n)
                log << (n ? ";" : "") << (g_hits[n] ? static_cast<int>((g_max[n] - g_min[n]) * 1000.0f) : -1);
            log << '\n';
            std::memset(g_hits, 0, sizeof(g_hits));
        }
    }

    void Configure(const std::string& iniPath)
    {
        g_enabled = replay::identity::IniInt("WaterGridProbe", 0, iniPath.c_str()) != 0;
        char at[96]{};
        replay::identity::IniString("WaterProbeAt", "", at, sizeof(at), iniPath.c_str());
        float x = 0, y = 0, z = 0;
        if (sscanf_s(at, "%f,%f,%f", &x, &y, &z) == 3)
        {
            g_cx = x;
            g_cy = y;
            g_cz = z;
        }
        const HMODULE module = GetModuleHandleW(nullptr);
        g_base = reinterpret_cast<std::uintptr_t>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + static_cast<std::uintptr_t>(dos->e_lfanew));
        g_imageEnd = g_base + nt->OptionalHeader.SizeOfImage;
        // The layout of this build, or nothing at all.
        g_layoutOk = false;
        if (!SiteMatches(kSetupSiteRva, kSetupSiteSignature, sizeof(kSetupSiteSignature)))
            g_layoutError = "water_setup_signature_mismatch";
        else if (!SiteMatches(kRiverVtableSiteRva, kRiverVtableSiteSignature, sizeof(kRiverVtableSiteSignature)))
            g_layoutError = "water_river_vtable_signature_mismatch";
        else if (!SiteMatches(kRectSiteRva, kRectSiteSignature, sizeof(kRectSiteSignature)))
            g_layoutError = "water_rect_signature_mismatch";
        else
        {
            g_layoutOk = true;
            g_layoutError = "none";
        }
    }

    bool Enabled() { return g_enabled; }

    void Tick(std::ostream& log, const char* phase, bool active, unsigned long long nowMs)
    {
        if (!g_enabled)
            return;
        if (active != g_wasActive)
        {
            g_wasActive = active;
            g_haveBaseline = false;
            g_previous.clear();
            g_snapshot = 0;
            g_gridFiles = 0;
            g_windowStart = nowMs;
            std::memset(g_hits, 0, sizeof(g_hits));
            log << "WATER_PROBE_STATE,phase=" << phase << ",active=" << (active ? 1 : 0) << ",at=" << g_cx << ';' << g_cy
                << ';' << g_cz << '\n';
        }
        if (!active)
            return;
        if (nowMs >= g_nextRect)
        {
            g_nextRect = nowMs + 500;
            RectTick(log, phase);
        }
        if (nowMs >= g_nextGrid)
        {
            g_nextGrid = nowMs + 250;
            GridTick(log, phase, nowMs);
        }
        if (nowMs >= g_nextProbe)
        {
            g_nextProbe = nowMs + 100;
            ProbeTick(log, phase, nowMs);
        }
        if (nowMs - g_windowStart >= 1000)
        {
            g_windowStart = nowMs;
            FlushWindow(log, phase);
        }
        if (nowMs >= g_nextMem && std::strcmp(phase, "record") == 0)
        {
            g_nextMem = nowMs + 1500;
            MemoryTick(log, phase, nowMs);
        }
    }
}
