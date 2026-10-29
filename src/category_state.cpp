#include <intrin.h>

#include "category_state.h"

#include "guarded_memory.h"
#include "hook_batch.h"
#include "module_lifetime.h"

#include <windows.h>

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

// Implementation of category_state.h. Every read goes through replay::mem
// (SEH, range filtered) after the sites of category_state_core.h matched
// once; the detour is installed only after the resolver's prologue and the
// same sites matched, and its reads and writes run under SEH (a fault stops
// imposing for the session and is counted).

namespace replay::catstate
{
    namespace
    {
        using U = std::uint64_t;

        Config g_cfg;
        std::uintptr_t g_base = 0;

        enum class Proof : std::uint8_t { Unknown, Proven, Failed };
        Proof g_proof = Proof::Unknown;
        const char* g_failedSite = nullptr;
        std::uint32_t g_failedRva = 0;
        bool g_proofLogged = false;

        // ---------------------------------------------------------- detour

        using ResolveFn = U(__fastcall*)(U category, U a2, U useMeta, U a4);
        std::atomic<ResolveFn> g_original{nullptr};
        void* g_target = nullptr;
        std::atomic<int> g_inFlight{0};
        std::atomic<bool> g_imposing{false};
        std::atomic<bool> g_faulted{false}; // a fault in the detour: no more imposing this playback
        // The last publish (GetTickCount64): a playback that stopped without
        // its end (a script restart) stops imposing after kFreshMs.
        constexpr std::uint64_t kFreshMs = 2000;
        std::atomic<std::uint64_t> g_publishedAt{0};
        std::atomic<std::uint32_t> g_staticCount{0};
        std::atomic<bool> g_fieldsAll{false};

        // The values at the playhead, by category index: two buffers, the
        // published one read by the detour, each guarded by its sequence
        // (odd while the script thread writes it).
        struct Snapshot
        {
            std::atomic<std::uint32_t> seq{0};
            std::atomic<std::uint8_t> on[kMaxCategories];
            std::atomic<float> volume[kMaxCategories];
            std::atomic<float> rolloff[kMaxCategories];
            std::atomic<float> x54[kMaxCategories];
            std::atomic<std::uint32_t> pitchLpf[kMaxCategories]; // pitch (low 16 bits) | lpf (high 16)
            std::atomic<std::uint32_t> hpf[kMaxCategories];
        };
        Snapshot g_snap[2];
        std::atomic<int> g_published{0};

        // The controllers' sum the detour found in a category before writing
        // the recorded one (only when it differed: the engine's own value).
        std::atomic<float> g_liveCtl[kMaxCategories];
        std::atomic<std::uint8_t> g_liveSeen[kMaxCategories];

        std::atomic<std::uint64_t> g_calls{0};
        std::atomic<std::uint64_t> g_imposed{0};
        std::atomic<std::uint64_t> g_raced{0};
        std::atomic<std::uint64_t> g_faults{0};
        std::atomic<float> g_maxDelta{0.0f};
        std::atomic<std::uint32_t> g_maxDeltaIndex{0};

        struct Imposed
        {
            std::int32_t index = -1;
            float volume = 0.0f;
        };

        void NoteDelta(std::uint32_t index, float delta)
        {
            float seen = g_maxDelta.load(std::memory_order_relaxed);
            while (delta > seen)
            {
                if (g_maxDelta.compare_exchange_weak(seen, delta, std::memory_order_relaxed))
                {
                    g_maxDeltaIndex.store(index, std::memory_order_relaxed);
                    break;
                }
            }
        }

        // SEH: no object with a destructor in here.
        bool ImposeAt(U category, Imposed& out)
        {
            __try
            {
                const std::int16_t index = *reinterpret_cast<const volatile std::int16_t*>(category + kCatIndex);
                if (index < 0 || static_cast<std::uint32_t>(index) >= g_staticCount.load(std::memory_order_relaxed) ||
                    static_cast<std::size_t>(index) >= kMaxCategories)
                    return false;
                Snapshot& s = g_snap[g_published.load(std::memory_order_acquire) & 1];
                const std::uint32_t s1 = s.seq.load(std::memory_order_acquire);
                if (s1 & 1u)
                    return false;
                if (!s.on[index].load(std::memory_order_relaxed))
                    return false;
                const float volume = s.volume[index].load(std::memory_order_relaxed);
                const bool all = g_fieldsAll.load(std::memory_order_relaxed);
                float rolloff = 1.0f, x54 = 1.0f;
                std::uint32_t pitchLpf = 0, hpf = 0;
                if (all)
                {
                    rolloff = s.rolloff[index].load(std::memory_order_relaxed);
                    x54 = s.x54[index].load(std::memory_order_relaxed);
                    pitchLpf = s.pitchLpf[index].load(std::memory_order_relaxed);
                    hpf = s.hpf[index].load(std::memory_order_relaxed);
                }
                std::atomic_thread_fence(std::memory_order_acquire);
                if (s.seq.load(std::memory_order_relaxed) != s1)
                    return false;
                volatile float* target = reinterpret_cast<volatile float*>(category + kCatVolume);
                const float live = *target;
                if (live != volume)
                {
                    g_liveCtl[index].store(live, std::memory_order_relaxed);
                    g_liveSeen[index].store(1, std::memory_order_relaxed);
                    NoteDelta(static_cast<std::uint32_t>(index), std::fabs(live - volume));
                }
                *target = volume;
                if (all)
                {
                    *reinterpret_cast<volatile float*>(category + kCatRolloff) = rolloff;
                    *reinterpret_cast<volatile float*>(category + kCatX54) = x54;
                    *reinterpret_cast<volatile std::int16_t*>(category + kCatPitch) =
                        static_cast<std::int16_t>(pitchLpf & 0xFFFFu);
                    *reinterpret_cast<volatile std::uint16_t*>(category + kCatLpf) = static_cast<std::uint16_t>(pitchLpf >> 16);
                    *reinterpret_cast<volatile std::uint16_t*>(category + kCatHpf) = static_cast<std::uint16_t>(hpf);
                }
                out.index = index;
                out.volume = volume;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                g_faults.fetch_add(1, std::memory_order_relaxed);
                g_faulted.store(true, std::memory_order_release);
                g_imposing.store(false, std::memory_order_release);
                return false;
            }
        }

        // The resolved value the original wrote for an imposed category must
        // be meta + imposed + parent (0x251C33F..0x251C380, same order of
        // the float additions); otherwise the game side wrote +0x48 between
        // the detour's write and the resolver's read.
        void CheckResolved(U category, U useMeta, const Imposed& imposed)
        {
            __try
            {
                const std::uintptr_t table = *reinterpret_cast<const volatile std::uintptr_t*>(g_base + kResolvedTableRva);
                if (!table)
                    return;
                float expected = (useMeta & 0xFF) ? *reinterpret_cast<const volatile float*>(category + kCatMeta) : 0.0f;
                expected += imposed.volume;
                const std::int16_t parent = *reinterpret_cast<const volatile std::int16_t*>(category + kCatParent);
                const std::uint8_t flags = *reinterpret_cast<const volatile std::uint8_t*>(category + kCatFlags);
                if (parent != -1 && !(flags & 1u))
                    expected += *reinterpret_cast<const volatile float*>(
                        table + static_cast<std::uintptr_t>(static_cast<std::int64_t>(parent)) * kResolvedStride);
                const float actual = *reinterpret_cast<const volatile float*>(
                    table + static_cast<std::uintptr_t>(imposed.index) * kResolvedStride);
                if (std::fabs(actual - expected) > 0.01f)
                    g_raced.fetch_add(1, std::memory_order_relaxed);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                g_faults.fetch_add(1, std::memory_order_relaxed);
                g_faulted.store(true, std::memory_order_release);
                g_imposing.store(false, std::memory_order_release);
            }
        }

        U __fastcall ResolveDetour(U category, U a2, U useMeta, U a4)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            g_inFlight.fetch_add(1, std::memory_order_seq_cst);
            Imposed imposed;
            bool did = false;
            if (g_imposing.load(std::memory_order_acquire) &&
                GetTickCount64() - g_publishedAt.load(std::memory_order_relaxed) <= kFreshMs)
            {
                g_calls.fetch_add(1, std::memory_order_relaxed);
                did = ImposeAt(category, imposed);
                if (did)
                    g_imposed.fetch_add(1, std::memory_order_relaxed);
            }
            U result = 0;
            if (const ResolveFn original = g_original.load(std::memory_order_acquire))
                result = original(category, a2, useMeta, a4);
            if (did)
                CheckResolved(category, useMeta, imposed);
            g_inFlight.fetch_sub(1, std::memory_order_seq_cst);
            return result;
        }

        // ---------------------------------------------------------- proof

        const char* CheckSites(std::uint32_t& failedRva)
        {
            if (!g_base)
                g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            if (!g_base)
            {
                failedRva = 0;
                return "module";
            }
            for (const Site& site : kSites)
            {
                std::uint8_t code[sizeof(site.bytes)]{};
                if (!mem::Copy(code, g_base + site.rva, site.size) || !SiteMatches(site, code) ||
                    (site.ripAt >= 0 && SiteTarget(site) != site.target))
                {
                    failedRva = site.rva;
                    return site.name;
                }
            }
            return nullptr;
        }

        bool Prove(std::ostream* log)
        {
            if (g_proof == Proof::Unknown)
            {
                g_failedSite = CheckSites(g_failedRva);
                g_proof = g_failedSite ? Proof::Failed : Proof::Proven;
            }
            if (log && !g_proofLogged)
            {
                g_proofLogged = true;
                if (g_proof == Proof::Proven)
                    *log << "CATEGORY_STATE,phase=proven,sites=" << kSiteCount << ",resolver=0x" << std::hex << kResolverRva
                         << ",map=0x" << kCategoryMapRva << ",pool=0x" << kCategoryPoolRva << std::dec
                         << ",record=" << (g_cfg.record ? 1 : 0) << ",replay=" << (g_cfg.replay ? 1 : 0)
                         << ",detour=" << (g_target ? 1 : 0) << '\n';
                else
                    *log << "CATEGORY_STATE,phase=unproven,site=" << (g_failedSite ? g_failedSite : "?") << ",rva=0x" << std::hex
                         << g_failedRva << std::dec << ",note=disabled\n";
            }
            return g_proof == Proof::Proven;
        }

        // ---------------------------------------------------------- reads

        struct CategoryRead
        {
            std::uint32_t hash = 0;
            std::uint32_t index = 0;
            std::uintptr_t pointer = 0;
            std::int16_t parent = -1;
            Values v;
        };

        struct PoolView
        {
            std::uintptr_t base = 0;
            std::uint64_t stride = 0;
            std::int32_t count = 0;
        };

        bool ReadPool(PoolView& pool)
        {
            std::uintptr_t object = 0;
            if (!mem::Read(g_base + kCategoryPoolRva, object) || !object)
                return false;
            return mem::Read(object + kPoolBase, pool.base) && mem::Read(object + kPoolStride, pool.stride) &&
                mem::Read(object + kPoolCount, pool.count) && pool.base && pool.stride >= kCatFlags + 4 &&
                pool.stride < 0x1000 && pool.count > 0;
        }

        // The category block from +0x48 to +0xB4 (the controllers' values,
        // the parent and the index).
        constexpr std::uint32_t kReadFrom = kCatVolume;
        constexpr std::uint32_t kReadSize = kCatIndex + 2 - kCatVolume;

        bool DecodeBlock(const std::uint8_t* block, Values& v, std::int16_t& parent, std::int16_t& index)
        {
            std::memcpy(&v.volume, block + (kCatVolume - kReadFrom), 4);
            std::memcpy(&v.rolloff, block + (kCatRolloff - kReadFrom), 4);
            std::memcpy(&v.x54, block + (kCatX54 - kReadFrom), 4);
            std::memcpy(&v.pitch, block + (kCatPitch - kReadFrom), 2);
            std::memcpy(&v.lpf, block + (kCatLpf - kReadFrom), 2);
            std::memcpy(&v.hpf, block + (kCatHpf - kReadFrom), 2);
            std::memcpy(&parent, block + (kCatParent - kReadFrom), 2);
            std::memcpy(&index, block + (kCatIndex - kReadFrom), 2);
            return PlausibleValues(v);
        }

        // Every static category of the map (hash, index, values).
        bool ReadAll(std::vector<CategoryRead>& out, std::uint32_t& staticCount, std::uint32_t& mapCount,
            std::uint32_t& mismatches)
        {
            out.clear();
            mismatches = 0;
            staticCount = 0;
            mapCount = 0;
            std::uintptr_t entries = 0;
            if (!mem::Read(g_base + kStaticCountRva, staticCount) || !staticCount || staticCount > kMaxCategories ||
                !mem::Read(g_base + kCategoryMapRva + kMapCount, mapCount) || !mapCount || mapCount > kMaxMapEntries ||
                !mem::Read(g_base + kCategoryMapRva + kMapEntries, entries) || !entries)
                return false;
            PoolView pool;
            if (!ReadPool(pool))
                return false;
            static std::vector<std::uint8_t> raw;
            raw.resize(static_cast<std::size_t>(mapCount) * kMapEntryStride);
            if (!mem::Copy(raw.data(), entries, raw.size()))
                return false;
            out.reserve(mapCount);
            for (std::uint32_t n = 0; n < mapCount; ++n)
            {
                std::uintptr_t pointer = 0;
                std::uint32_t hash = 0;
                std::memcpy(&pointer, raw.data() + n * kMapEntryStride, 8);
                std::memcpy(&hash, raw.data() + n * kMapEntryStride + 8, 4);
                if (!pointer || !hash || pointer < pool.base || (pointer - pool.base) % pool.stride)
                    continue;
                const std::uint64_t index = (pointer - pool.base) / pool.stride;
                if (index >= static_cast<std::uint64_t>(pool.count) || index >= staticCount)
                    continue; // a dynamic hierarchy's category
                std::uint8_t block[kReadSize]{};
                if (!mem::Copy(block, pointer + kReadFrom, sizeof(block)))
                    continue;
                CategoryRead r;
                std::int16_t own = -1;
                if (!DecodeBlock(block, r.v, r.parent, own))
                    continue;
                if (own != static_cast<std::int16_t>(index))
                {
                    ++mismatches;
                    continue;
                }
                r.hash = hash;
                r.index = static_cast<std::uint32_t>(index);
                r.pointer = pointer;
                out.push_back(r);
            }
            return !out.empty();
        }

        // ---------------------------------------------------------- state

        Recorder g_recorder;
        std::uint64_t g_nextScanWall = 0;
        std::uint32_t g_recFailures = 0;
        std::uint64_t g_nextRecLineWall = 0;
        std::uint64_t g_nextRecWatchWall = 0;

        struct Entry
        {
            std::size_t series = 0;
            std::uint32_t index = 0;
            std::uintptr_t pointer = 0;
        };
        bool g_playing = false;
        PlayIndex g_index;
        std::vector<Entry> g_entries;     // imposed (recorded, found in this session, not kept live)
        std::vector<Entry> g_keptLive;    // recorded but kept live
        std::map<std::uint32_t, std::uint32_t> g_hashOfIndex; // this session's index -> hash
        std::map<std::uint32_t, std::uintptr_t> g_pointerOfIndex;
        std::size_t g_unmapped = 0;
        std::uint64_t g_nextWindowWall = 0;
        std::uint64_t g_nextWatchWall = 0;
        std::uint64_t g_windows = 0;
        std::uint64_t g_lastCalls = 0, g_lastImposed = 0, g_lastRaced = 0, g_lastFaults = 0;
        float g_maxDeltaTotal = 0.0f;
        const char* g_pauseWhy = nullptr;
        bool g_snapOn = false;

        void ResetCounters()
        {
            g_calls.store(0, std::memory_order_relaxed);
            g_imposed.store(0, std::memory_order_relaxed);
            g_raced.store(0, std::memory_order_relaxed);
            g_faults.store(0, std::memory_order_relaxed);
            g_maxDelta.store(0.0f, std::memory_order_relaxed);
            g_maxDeltaIndex.store(0, std::memory_order_relaxed);
            g_lastCalls = g_lastImposed = g_lastRaced = g_lastFaults = 0;
            for (std::size_t n = 0; n < kMaxCategories; ++n)
                g_liveSeen[n].store(0, std::memory_order_relaxed);
        }

        // Writes the values at timeMs into the unpublished buffer, then
        // publishes it.
        void Publish(std::uint32_t timeMs, bool on)
        {
            const int next = 1 - (g_published.load(std::memory_order_relaxed) & 1);
            Snapshot& s = g_snap[next];
            s.seq.fetch_add(1, std::memory_order_relaxed); // odd
            std::atomic_thread_fence(std::memory_order_release);
            for (const Entry& e : g_entries)
            {
                if (e.index >= kMaxCategories)
                    continue;
                const Values* v = on ? PlayIndex::At(g_index.series[e.series], timeMs) : nullptr;
                if (!v)
                {
                    s.on[e.index].store(0, std::memory_order_relaxed);
                    continue;
                }
                s.volume[e.index].store(v->volume, std::memory_order_relaxed);
                s.rolloff[e.index].store(v->rolloff, std::memory_order_relaxed);
                s.x54[e.index].store(v->x54, std::memory_order_relaxed);
                s.pitchLpf[e.index].store(static_cast<std::uint32_t>(static_cast<std::uint16_t>(v->pitch)) |
                        (static_cast<std::uint32_t>(v->lpf) << 16),
                    std::memory_order_relaxed);
                s.hpf[e.index].store(v->hpf, std::memory_order_relaxed);
                s.on[e.index].store(1, std::memory_order_relaxed);
            }
            s.seq.fetch_add(1, std::memory_order_release); // even
            g_published.store(next, std::memory_order_release);
            g_publishedAt.store(GetTickCount64(), std::memory_order_release);
            g_snapOn = on;
        }

        // Both buffers off (the playback ended).
        void ClearSnapshots()
        {
            for (Snapshot& s : g_snap)
            {
                s.seq.fetch_add(1, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_release);
                for (std::size_t n = 0; n < kMaxCategories; ++n)
                    s.on[n].store(0, std::memory_order_relaxed);
                s.seq.fetch_add(1, std::memory_order_release);
            }
            g_snapOn = false;
        }

        // The engine's own value back into the imposed categories that still
        // hold the recorded one (the controller manager rewrites +0x48 every
        // update; this only covers an update that would not come).
        std::uint32_t GiveBack()
        {
            std::uint32_t written = 0;
            for (const Entry& e : g_entries)
            {
                if (e.index >= kMaxCategories || !g_liveSeen[e.index].load(std::memory_order_relaxed))
                    continue;
                const Snapshot& s = g_snap[g_published.load(std::memory_order_relaxed) & 1];
                const float imposed = s.volume[e.index].load(std::memory_order_relaxed);
                float now = 0.0f;
                if (!mem::Read(e.pointer + kCatVolume, now) || now != imposed)
                    continue;
                const float live = g_liveCtl[e.index].load(std::memory_order_relaxed);
                if (mem::Put(e.pointer + kCatVolume, &live, sizeof(live)))
                    ++written;
            }
            return written;
        }

        std::string HashText(std::uint32_t hash)
        {
            char b[16];
            std::snprintf(b, sizeof(b), "0x%08x", hash);
            return b;
        }

        // CATEGORY_CTL lines of the watched categories (and their parents'
        // values on the same line).
        void WatchLines(const char* phase, std::uint32_t clipMs, std::ostream& log)
        {
            if (g_cfg.watch.empty())
                return;
            PoolView pool;
            std::uintptr_t table = 0;
            if (!ReadPool(pool) || !mem::Read(g_base + kResolvedTableRva, table) || !table)
                return;
            const Snapshot& s = g_snap[g_published.load(std::memory_order_acquire) & 1];
            for (std::uint32_t index : g_cfg.watch)
            {
                if (index >= static_cast<std::uint32_t>(pool.count) || index >= kMaxCategories)
                    continue;
                const std::uintptr_t pointer = pool.base + static_cast<std::uintptr_t>(index) * pool.stride;
                std::uint8_t block[kReadSize]{};
                float meta = 0.0f, resolved = 0.0f, parentResolved = 0.0f;
                Values v;
                std::int16_t parent = -1, own = -1;
                if (!mem::Copy(block, pointer + kReadFrom, sizeof(block)) || !mem::Read(pointer + kCatMeta, meta))
                    continue;
                DecodeBlock(block, v, parent, own);
                if (own != static_cast<std::int16_t>(index))
                    continue;
                mem::Read(table + static_cast<std::uintptr_t>(index) * kResolvedStride, resolved);
                if (parent >= 0)
                    mem::Read(table + static_cast<std::uintptr_t>(parent) * kResolvedStride, parentResolved);
                const auto hash = g_hashOfIndex.find(index);
                char b[360];
                const bool imposedNow = g_imposing.load(std::memory_order_relaxed) && s.on[index].load(std::memory_order_relaxed);
                char imposedText[24] = "none";
                if (imposedNow)
                    std::snprintf(imposedText, sizeof(imposedText), "%.2f", static_cast<double>(s.volume[index].load()));
                char liveText[24] = "none";
                if (g_liveSeen[index].load(std::memory_order_relaxed))
                    std::snprintf(liveText, sizeof(liveText), "%.2f", static_cast<double>(g_liveCtl[index].load()));
                std::snprintf(b, sizeof(b),
                    "CATEGORY_CTL,phase=%s,clip_ms=%u,cat=%u,hash=%s,parent=%d,ctl_db=%.2f,live_ctl_db=%s,imposed_db=%s,"
                    "meta_db=%.2f,resolved_db=%.2f,parent_db=%.2f,lpf=%u,hpf=%u",
                    phase, clipMs, index, hash != g_hashOfIndex.end() ? HashText(hash->second).c_str() : "?",
                    static_cast<int>(parent), static_cast<double>(v.volume), liveText, imposedText, static_cast<double>(meta),
                    static_cast<double>(resolved), static_cast<double>(parentResolved), static_cast<unsigned>(v.lpf),
                    static_cast<unsigned>(v.hpf));
                log << b << '\n';
            }
        }

        void Pause(const char* why, std::ostream& log)
        {
            if (g_imposing.exchange(false, std::memory_order_acq_rel))
            {
                const std::uint32_t given = GiveBack();
                log << "CATEGORY_STATE,phase=play,event=pause,why=" << why << ",given_back=" << given << '\n';
            }
            else if (g_pauseWhy != why)
                log << "CATEGORY_STATE,phase=play,event=idle,why=" << why << '\n';
            g_pauseWhy = why;
        }
    }

    void Configure(const Config& config)
    {
        g_cfg = config;
        g_fieldsAll.store(config.fieldsAll, std::memory_order_relaxed);
    }

    const Config& GetConfig() { return g_cfg; }

    bool Install(std::string& error)
    {
        if (g_target)
            return true;
        if (!g_cfg.replay)
        {
            error = "off";
            return false;
        }
        g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        void* target = reinterpret_cast<std::uint8_t*>(g_base) + kResolverRva;
        if (!mem::Matches(target, kResolverSignature, sizeof(kResolverSignature)))
        {
            error = "resolver_signature_mismatch";
            return false;
        }
        if (!Prove(nullptr))
        {
            error = std::string("site_") + (g_failedSite ? g_failedSite : "?");
            return false;
        }
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        {
            error = std::string("mh_initialize_") + MH_StatusToString(init);
            return false;
        }
        void* trampoline = nullptr;
        MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&ResolveDetour), &trampoline);
        if (status != MH_OK || !trampoline)
        {
            error = std::string("mh_create_") + MH_StatusToString(status);
            return false;
        }
        g_original.store(reinterpret_cast<ResolveFn>(trampoline), std::memory_order_release);
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

    bool Installed() { return g_target != nullptr; }

    void Shutdown()
    {
        g_imposing.store(false, std::memory_order_release);
        // Disable, wait for the calls in flight, then remove (the trampoline
        // goes with MH_RemoveHook).
        if (g_target)
            MH_DisableHook(g_target);
        for (int wait = 0; wait < 200 && g_inFlight.load(std::memory_order_seq_cst) > 0; ++wait)
            Sleep(1);
        if (g_target)
        {
            MH_RemoveHook(g_target);
            g_target = nullptr;
        }
        g_original.store(nullptr, std::memory_order_release);
    }

    void Record(Recording& clip, std::uint32_t timeMs, bool clockRunning, std::ostream& log)
    {
        if (!g_cfg.record || !clockRunning)
            return;
        if (!Prove(&log))
            return;
        const std::uint64_t wall = GetTickCount64();
        if (wall < g_nextScanWall)
            return;
        g_nextScanWall = wall + kScanIntervalMs;
        Track& track = clip.categoryState;
        const bool fresh = track.samples.empty();
        if (fresh)
        {
            g_recorder.Reset();
            g_nextRecLineWall = wall + 10000;
        }
        static std::vector<CategoryRead> reads;
        std::uint32_t staticCount = 0, mapCount = 0, mismatches = 0;
        if (!ReadAll(reads, staticCount, mapCount, mismatches))
        {
            if (g_recFailures++ < 8)
                log << "CATEGORY_STATE,phase=record,event=skip,clip_ms=" << timeMs << ",static=" << staticCount
                    << ",map=" << mapCount << '\n';
            return;
        }
        track.staticCount = staticCount;
        track.mapCount = mapCount;
        std::size_t stored = 0;
        for (const CategoryRead& r : reads)
        {
            if (fresh)
                g_hashOfIndex[r.index] = r.hash;
            if (g_recorder.Offer(track, r.hash, r.v, timeMs))
                ++stored;
        }
        if (fresh)
            log << "CATEGORY_STATE,phase=record,event=begin,section=CATS,static=" << staticCount << ",map=" << mapCount
                << ",categories=" << reads.size() << ",stored=" << stored << ",index_mismatch=" << mismatches
                << ",clip_ms=" << timeMs << '\n';
        else if (wall >= g_nextRecLineWall)
        {
            g_nextRecLineWall = wall + 10000;
            log << "CATEGORY_STATE,phase=record,event=window,clip_ms=" << timeMs << ",samples=" << track.samples.size()
                << ",categories=" << reads.size() << ",index_mismatch=" << mismatches << '\n';
        }
        if (g_cfg.log && wall >= g_nextRecWatchWall)
        {
            g_nextRecWatchWall = wall + 500;
            WatchLines("rec", timeMs, log);
        }
    }

    void BeginPlayback(const Recording& clip, std::ostream& log)
    {
        g_playing = true;
        g_imposing.store(false, std::memory_order_release);
        g_faulted.store(false, std::memory_order_release);
        g_pauseWhy = nullptr;
        g_entries.clear();
        g_keptLive.clear();
        g_hashOfIndex.clear();
        g_unmapped = 0;
        g_windows = 0;
        g_maxDeltaTotal = 0.0f;
        g_nextWindowWall = 0;
        g_nextWatchWall = 0;
        ResetCounters();
        ClearSnapshots();
        const Track& track = clip.categoryState;
        g_index.Build(track);
        const bool proven = Prove(&log);
        std::uint32_t staticCount = 0, mapCount = 0, mismatches = 0;
        std::vector<CategoryRead> reads;
        const bool read = proven && ReadAll(reads, staticCount, mapCount, mismatches);
        std::map<std::uint32_t, const CategoryRead*> byHash;
        for (const CategoryRead& r : reads)
        {
            byHash[r.hash] = &r;
            g_hashOfIndex[r.index] = r.hash;
        }
        std::string keptText;
        for (std::size_t n = 0; n < g_index.series.size(); ++n)
        {
            const Series& s = g_index.series[n];
            const auto found = byHash.find(s.hash);
            if (found == byHash.end())
            {
                ++g_unmapped;
                continue;
            }
            const Entry e{n, found->second->index, found->second->pointer};
            if (KeepLive(g_cfg.keepLive, s.hash))
            {
                g_keptLive.push_back(e);
                // The live controllers against the recorded first value of
                // a category kept live (a candidate to keep or to impose).
                if (keptText.size() < 400)
                {
                    char b[96];
                    std::snprintf(b, sizeof(b), "%s0x%08x:live=%.2f:rec=%.2f", keptText.empty() ? "" : "|", s.hash,
                        static_cast<double>(found->second->v.volume), static_cast<double>(s.points.front().second.volume));
                    keptText += b;
                }
                continue;
            }
            g_entries.push_back(e);
        }
        // The live categories whose controllers differ most from the
        // recording at the start (the scenes this session has and the
        // recording did not, or the other way round).
        std::vector<std::pair<float, std::string>> diffs;
        for (const Entry& e : g_entries)
        {
            const auto found = std::find_if(reads.begin(), reads.end(), [&e](const CategoryRead& r) { return r.index == e.index; });
            const Values* rec = PlayIndex::At(g_index.series[e.series], 0);
            if (found == reads.end() || !rec)
                continue;
            const float d = found->v.volume - rec->volume;
            if (std::fabs(d) > 0.5f)
            {
                char b[64];
                std::snprintf(b, sizeof(b), "%u:%.1f", e.index, static_cast<double>(d));
                diffs.emplace_back(std::fabs(d), b);
            }
        }
        std::sort(diffs.begin(), diffs.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::string diffText;
        for (std::size_t n = 0; n < diffs.size() && n < 16; ++n)
            diffText += (n ? "|" : "") + diffs[n].second;
        g_staticCount.store(staticCount, std::memory_order_relaxed);
        log << "CATEGORY_STATE,phase=play,event=begin,section=" << (track.Empty() ? "absent" : "CATS")
            << ",replay=" << (g_cfg.replay ? 1 : 0) << ",installed=" << (g_target ? 1 : 0) << ",proven=" << (proven ? 1 : 0)
            << ",read=" << (read ? 1 : 0) << ",fields=" << (g_cfg.fieldsAll ? "all" : "volume")
            << ",recorded=" << g_index.series.size() << ",samples=" << track.samples.size() << ",imposed=" << g_entries.size()
            << ",keep_live=" << g_keptLive.size() << ",unmapped=" << g_unmapped << ",static=" << staticCount
            << ",rec_static=" << track.staticCount << ",index_mismatch=" << mismatches
            << ",keep_live_values=" << (keptText.empty() ? "none" : keptText)
            << ",live_minus_rec_at_0=" << (diffText.empty() ? "none" : diffText) << '\n';
    }

    void Play(const Recording& clip, std::uint32_t timeMs, bool owning, std::ostream& log)
    {
        if (!g_playing)
            return;
        (void)clip;
        if (!g_cfg.replay)
            return Pause("off", log);
        if (!g_target)
            return Pause("not_installed", log);
        if (g_entries.empty())
            return Pause("no_section", log);
        if (!owning)
            return Pause("not_owning", log);
        if (g_faulted.load(std::memory_order_acquire))
            return Pause("fault", log);
        Publish(timeMs, true);
        if (!g_imposing.exchange(true, std::memory_order_acq_rel))
            log << "CATEGORY_STATE,phase=play,event=impose,clip_ms=" << timeMs << ",categories=" << g_entries.size() << '\n';
        g_pauseWhy = nullptr;
        const std::uint64_t wall = GetTickCount64();
        if (wall >= g_nextWindowWall)
        {
            g_nextWindowWall = wall + 500;
            Window w;
            const std::uint64_t calls = g_calls.load(std::memory_order_relaxed);
            const std::uint64_t imposed = g_imposed.load(std::memory_order_relaxed);
            const std::uint64_t raced = g_raced.load(std::memory_order_relaxed);
            const std::uint64_t faults = g_faults.load(std::memory_order_relaxed);
            w.calls = calls - g_lastCalls;
            w.imposed = imposed - g_lastImposed;
            w.raced = raced - g_lastRaced;
            w.faults = faults - g_lastFaults;
            g_lastCalls = calls;
            g_lastImposed = imposed;
            g_lastRaced = raced;
            g_lastFaults = faults;
            w.maxDeltaDb = g_maxDelta.exchange(0.0f, std::memory_order_relaxed);
            w.maxDeltaIndex = g_maxDeltaIndex.load(std::memory_order_relaxed);
            g_maxDeltaTotal = (std::max)(g_maxDeltaTotal, w.maxDeltaDb);
            ++g_windows;
            if (g_windows <= 400)
                log << WindowLine(timeMs, w, g_entries.size(), g_keptLive.size()) << '\n';
        }
        if (g_cfg.log && wall >= g_nextWatchWall)
        {
            g_nextWatchWall = wall + 500;
            WatchLines("play", timeMs, log);
        }
    }

    void EndPlayback(const Recording& clip, std::ostream& log)
    {
        if (!g_playing)
            return;
        const bool was = g_imposing.exchange(false, std::memory_order_acq_rel);
        const std::uint32_t given = was ? GiveBack() : 0;
        ClearSnapshots();
        g_playing = false;
        log << "CATEGORY_STATE,phase=play,event=end,section=" << (clip.categoryState.Empty() ? "absent" : "CATS")
            << ",calls=" << g_calls.load() << ",imposed=" << g_imposed.load() << ",raced=" << g_raced.load()
            << ",faults=" << g_faults.load() << ",windows=" << g_windows << ",max_delta_db=" << Fmt2(g_maxDeltaTotal)
            << ",given_back=" << given << '\n';
    }

    void ForgetPlayback()
    {
        g_imposing.store(false, std::memory_order_release);
        ClearSnapshots();
        g_entries.clear();
        g_keptLive.clear();
        g_playing = false;
    }
}
