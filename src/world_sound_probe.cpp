#include "world_sound_probe.h"

#include "game_listener_core.h"
#include "guarded_memory.h"
#include "sound_census.h"
#include "sound_world_core.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ostream>
#include <string>
#include <vector>

// See world_sound_probe.h. Read only; every read refuses an address below
// 4 GB without touching it (guarded_memory.h kGameHeapLowest: the image and
// the game's heaps live above it), as the volume probe does.

namespace replay::worldsoundprobe
{
    namespace sw = replay::soundworld;
    namespace gl = replay::gamelistener;

    namespace
    {
        namespace mem
        {
            inline bool Copy(void* out, std::uintptr_t source, std::size_t size)
            {
                return replay::mem::Copy(out, source, size, replay::mem::kGameHeapLowest);
            }
            template <typename T>
            inline bool Read(std::uintptr_t address, T& out)
            {
                return Copy(&out, address, sizeof(T));
            }
        }

        // Lines per minute of wall time (a ring recording runs for as long
        // as the game is played: a budget per pass would be spent before the
        // part a clip keeps).
        constexpr std::uint32_t kPtfxLines = 300;
        constexpr std::uint32_t kGroupLines = 600;
        constexpr std::uint64_t kBudgetPeriodMs = 60000;
        constexpr std::uint64_t kPassGapMs = 1500;  // wall time without a tick: the pass ended

        Config g_cfg;

        enum class Proof : std::uint8_t { Unknown, Proven, Failed };
        Proof g_proof = Proof::Unknown;
        std::uintptr_t g_base = 0;
        std::uintptr_t g_imageEnd = 0;
        bool g_ptfxOn = false;
        bool g_groupOn = false;
        bool g_listenerOn = false;

        struct Counters
        {
            std::uint64_t ticks = 0;
            std::uint64_t ptfxReadFailed = 0;
            std::uint64_t ptfxStart = 0;
            std::uint64_t ptfxCreated = 0;
            std::uint64_t ptfxFar = 0;          // done without a sound, no sound before
            std::uint64_t ptfxFarStopped = 0;   // done without a sound, a sound before (far stop)
            std::uint64_t ptfxFarBeyondMax = 0; // of both, listener farther than the max
            std::uint64_t ptfxEnded = 0;
            std::uint64_t groupReadFailed = 0;
            std::uint64_t groupNoManager = 0;
            std::uint64_t groupInsane = 0;
            std::uint64_t groupSamples = 0;     // active groups read
            std::uint64_t groupLoopOn = 0;      // the loop appeared
            std::uint64_t groupLoopOff = 0;
            std::uint32_t groupMaxMembers = 0;
            std::uint32_t groupMaxReplay = 0;
            std::uint32_t groupMaxMovers = 0;
            std::uint64_t groupSamplesMoving = 0; // samples with movers
            std::uint64_t groupSamplesLoop = 0;   // samples with the loop
        };

        // The pass.
        bool g_inPass = false;
        std::string g_phase = "idle";
        std::uint32_t g_pass = 0;
        double g_lastClipMs = 0.0;
        std::uint64_t g_lastWall = 0;
        Counters g_counters;
        sw::Budget g_ptfxBudget{kPtfxLines};
        sw::Budget g_groupBudget{kGroupLines};
        sw::PtfxWatch g_ptfx;
        std::uint64_t g_budgetWall = 0;
        std::vector<std::uint8_t> g_ptfxBuffer;
        sw::PtfxSlot g_ptfxSlots[sw::kPtfxSlotCount];
        std::vector<sw::PtfxChange> g_ptfxChanges;
        bool g_groupLoop[sw::kGroupCount] = {};
        bool g_groupActive[sw::kGroupCount] = {};
        std::uint32_t g_nextGroupMs = 0;

        std::uint64_t Wall() { return GetTickCount64(); }

        std::string Hex(std::uint64_t value)
        {
            char text[24]{};
            std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
            return text;
        }

        // An image address as its RVA ("rva:0x..."), else the raw value.
        std::string Where(std::uint64_t address)
        {
            if (address >= g_base && address < g_imageEnd)
                return "rva:" + Hex(address - g_base);
            return Hex(address);
        }

        bool Prove(std::ostream& log)
        {
            if (g_proof != Proof::Unknown)
                return g_proof == Proof::Proven;
            g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            g_ptfxOn = g_groupOn = g_listenerOn = g_base != 0;
            std::string failed;
            for (const sw::ProbeSite& site : sw::kProbeSites)
            {
                std::uint8_t code[8]{};
                if (g_base && mem::Copy(code, g_base + site.rva, site.size) && sw::ProbeSiteMatches(site, code))
                    continue;
                bool& on = site.part == sw::ProbePart::Ptfx ? g_ptfxOn : site.part == sw::ProbePart::Group ? g_groupOn : g_listenerOn;
                if (on)
                    log << "WORLD_SOUND_PROBE_CHECK,ok=0,part=" << sw::ProbePartName(site.part) << ",site=" << site.name
                        << ",rva=" << Hex(site.rva) << ",note=part_disabled\n";
                on = false;
            }
            if (g_listenerOn)
            {
                for (const gl::Site& site : gl::kSites)
                {
                    std::uint8_t code[16]{};
                    if (!mem::Copy(code, g_base + site.rva, site.size) || !gl::SiteMatches(site, code))
                    {
                        log << "WORLD_SOUND_PROBE_CHECK,ok=0,part=listener,site=" << site.name << ",rva=" << Hex(site.rva)
                            << ",note=part_disabled\n";
                        g_listenerOn = false;
                        break;
                    }
                }
            }
            std::int32_t lfanew = 0;
            std::uint32_t sizeOfImage = 0;
            if (mem::Read(g_base + 0x3C, lfanew) && lfanew > 0 && lfanew < 0x1000 &&
                mem::Read(g_base + static_cast<std::uint32_t>(lfanew) + 0x50, sizeOfImage) && sizeOfImage > 0x6000000 &&
                sizeOfImage < 0x20000000)
                g_imageEnd = g_base + sizeOfImage;
            else
                g_imageEnd = g_base + 0x8000000;
            g_proof = g_ptfxOn || g_groupOn ? Proof::Proven : Proof::Failed;
            log << "WORLD_SOUND_PROBE_CHECK,ok=" << (g_proof == Proof::Proven ? 1 : 0) << ",sites=" << sw::kProbeSiteCount
                << ",ptfx=" << (g_ptfxOn ? 1 : 0) << ",group=" << (g_groupOn ? 1 : 0) << ",listener=" << (g_listenerOn ? 1 : 0)
                << ",ptfx_manager=" << Hex(sw::kPtfxManagerRva) << ",ptfx_slots=" << sw::kPtfxSlotCount
                << ",group_manager_ptr=" << Hex(sw::kGroupManagerPtrRva) << ",groups=" << sw::kGroupCount
                << ",group_ms=" << g_cfg.groupMs << ",census=" << (census::Enabled() ? 1 : 0) << ",writes=0\n";
            return g_proof == Proof::Proven;
        }

        // The listener points the particle slot's test reads (0x31DBE60):
        // both descriptions whose weights pass 1e-5, every volume point.
        struct Listener
        {
            std::size_t count = 0;
            float points[gl::kListenerCount * gl::kMaxVolumePoints][3] = {};
        };

        Listener ReadListener()
        {
            Listener l;
            if (!g_listenerOn)
                return l;
            const std::uintptr_t env = g_base + gl::kEnvironmentRva;
            for (std::uint32_t d = 0; d < gl::kListenerCount; ++d)
            {
                const std::uintptr_t desc = env + gl::ListenerOffset(d);
                float weight[4]{};
                if (!mem::Copy(weight, desc + sw::kListenerWeight, sizeof(weight)))
                    continue;
                bool live = false;
                for (float w : weight)
                    live = live || w > sw::kListenerWeightMin;
                if (!live)
                    continue;
                std::uint32_t points = 0;
                if (!mem::Read(desc + gl::kVolumePointCount, points) || points == 0 || points > gl::kMaxVolumePoints)
                    continue;
                for (std::uint32_t j = 0; j < points; ++j)
                {
                    float p[4]{};
                    if (!mem::Copy(p, desc + gl::kVolumePointPos + j * gl::kVolumePointStride, sizeof(p)))
                        break;
                    if (!(std::fabs(p[0]) < 100000.0f && std::fabs(p[1]) < 100000.0f && std::fabs(p[2]) < 100000.0f))
                        break;
                    std::memcpy(l.points[l.count++], p, sizeof(float) * 3);
                }
            }
            return l;
        }

        float ListenerDistance(const Listener& l, const float pos[3])
        {
            return sw::NearestListener(l.points, l.count, pos);
        }

        void ClosePass(std::ostream& log, const char* why)
        {
            if (!g_inPass)
                return;
            const Counters& c = g_counters;
            log << "WORLD_SOUND_PROBE_TOTAL,phase=" << g_phase << ",pass=" << g_pass << ",why=" << why << ",ticks=" << c.ticks
                << ",ptfx=start:" << c.ptfxStart << ";created:" << c.ptfxCreated << ";done_no_sound:" << c.ptfxFar
                << ";far_stopped:" << c.ptfxFarStopped << ";beyond_max:" << c.ptfxFarBeyondMax << ";ended:" << c.ptfxEnded
                << ";read_failed:" << c.ptfxReadFailed << ";lines_dropped:" << g_ptfxBudget.Dropped()
                << ",group=samples:" << c.groupSamples << ";moving:" << c.groupSamplesMoving << ";with_loop:" << c.groupSamplesLoop
                << ";loop_on:" << c.groupLoopOn << ";loop_off:" << c.groupLoopOff << ";max_members:" << c.groupMaxMembers
                << ";max_replay:" << c.groupMaxReplay << ";max_movers:" << c.groupMaxMovers << ";no_manager:" << c.groupNoManager
                << ";insane:" << c.groupInsane << ";read_failed:" << c.groupReadFailed << ";lines_dropped:" << g_groupBudget.Dropped()
                << ",writes=0\n";
            g_inPass = false;
        }

        void OpenPass(const char* phase, double clipMs)
        {
            g_inPass = true;
            g_phase = phase;
            ++g_pass;
            g_counters = Counters{};
            g_ptfxBudget.Reset(kPtfxLines);
            g_groupBudget.Reset(kGroupLines);
            g_budgetWall = Wall();
            g_ptfx.Reset();
            for (std::uint32_t k = 0; k < sw::kGroupCount; ++k)
            {
                g_groupLoop[k] = false;
                g_groupActive[k] = false;
            }
            g_nextGroupMs = clipMs > 0.0 ? static_cast<std::uint32_t>(clipMs) : 0;
            g_lastClipMs = clipMs;
        }

        // ------------------------------------------------ particle audio slots

        void TickPtfx(std::ostream& log, std::uint32_t clipMs, const Listener& listener)
        {
            const std::uintptr_t mgr = g_base + sw::kPtfxManagerRva;
            if (g_ptfxBuffer.size() != sw::kPtfxSlotCount * sw::kPtfxSlotStride)
                g_ptfxBuffer.assign(sw::kPtfxSlotCount * sw::kPtfxSlotStride, 0);
            std::uint8_t bufferByte = 0;
            if (!mem::Copy(g_ptfxBuffer.data(), mgr + sw::kPtfxSlotsOffset, g_ptfxBuffer.size()) ||
                !mem::Read(mgr + sw::kPtfxBufferIndex, bufferByte))
            {
                ++g_counters.ptfxReadFailed;
                g_ptfx.Reset();
                return;
            }
            const std::uint32_t posIndex = static_cast<std::uint8_t>(bufferByte - 1) & 1u; // 0x32148E0..0x32148F8
            for (std::uint32_t n = 0; n < sw::kPtfxSlotCount; ++n)
            {
                const std::uint8_t* slot = g_ptfxBuffer.data() + n * sw::kPtfxSlotStride;
                sw::PtfxSlot& s = g_ptfxSlots[n];
                s.active = slot[sw::kPtfxActive] != 0;
                s.skip = slot[sw::kPtfxSkip] != 0;
                s.done = slot[sw::kPtfxDone] != 0;
                std::memcpy(&s.sound, slot + sw::kPtfxSound, sizeof(s.sound));
                std::memcpy(&s.settings, slot + sw::kPtfxSettings, sizeof(s.settings));
            }
            g_ptfxChanges.clear();
            g_ptfx.Diff(g_ptfxSlots, sw::kPtfxSlotCount, g_ptfxChanges);
            for (const sw::PtfxChange& change : g_ptfxChanges)
            {
                const std::uint8_t* slot = g_ptfxBuffer.data() + change.slot * sw::kPtfxSlotStride;
                const sw::PtfxSlot& s = g_ptfxSlots[change.slot];
                float pos[4]{};
                std::memcpy(pos, slot + posIndex * 0x10, sizeof(pos));
                float maxM = -1.0f;
                std::uint32_t mode = 0xFFFFFFFFu;
                if (s.settings)
                {
                    if (!mem::Read(s.settings + sw::kPtfxSettingsMaxM, maxM))
                        maxM = -1.0f;
                    if (mem::Read(s.settings + sw::kPtfxSettingsMode, mode))
                        mode &= 3u;
                    else
                        mode = 0xFFFFFFFFu;
                }
                const float dist = ListenerDistance(listener, pos);
                switch (change.event)
                {
                case sw::PtfxEvent::Start: ++g_counters.ptfxStart; break;
                case sw::PtfxEvent::Created: ++g_counters.ptfxCreated; break;
                case sw::PtfxEvent::Far:
                    ++(change.hadSound ? g_counters.ptfxFarStopped : g_counters.ptfxFar);
                    if (maxM > 0.0f && dist > maxM)
                        ++g_counters.ptfxFarBeyondMax;
                    break;
                case sw::PtfxEvent::Ended: ++g_counters.ptfxEnded; break;
                }
                if (!g_ptfxBudget.Take())
                    continue;
                char line[400]{};
                std::snprintf(line, sizeof(line),
                    "PTFX_AUDIO_SLOT,phase=%s,clip_ms=%u,event=%s,slot=%u,settings=%s,max_m=%.1f,dist_m=%.1f,beyond_max=%d,"
                    "mode=%d,active=%d,skip=%d,done=%d,sound=%d,had_sound=%d,pos=%.1f;%.1f;%.1f",
                    g_phase.c_str(), clipMs, sw::PtfxEventName(change.event), change.slot, Where(s.settings).c_str(), maxM,
                    dist, maxM > 0.0f && dist > maxM ? 1 : 0, mode == 0xFFFFFFFFu ? -1 : static_cast<int>(mode),
                    s.active ? 1 : 0, s.skip ? 1 : 0, s.done ? 1 : 0, s.sound ? 1 : 0, change.hadSound ? 1 : 0, pos[0], pos[1],
                    pos[2]);
                log << line << '\n';
            }
        }

        // ------------------------------------------------ horse group audio

        std::string MemberId(std::uint64_t entity, std::uint32_t& replayOut, std::uint32_t& liveOut)
        {
            census::Owner owner;
            if (!entity || !census::OwnerOf(entity, entity, owner))
                return std::string();
            if (owner.kind == census::OwnerKind::LivePlayer)
            {
                ++liveOut;
                return "live";
            }
            if (owner.kind != census::OwnerKind::Recorded && owner.kind != census::OwnerKind::Puppet)
                return std::string();
            ++replayOut;
            return sw::OwnerText(owner.id);
        }

        // The part of a group the probe reads: its metadata to its loop.
        constexpr std::uint32_t kGroupBlockFrom = sw::kGroupMeta;
        constexpr std::uint32_t kGroupBlockSize = sw::kGroupSound + 8 - kGroupBlockFrom;

        void TickGroups(std::ostream& log, std::uint32_t clipMs, const Listener& listener)
        {
            std::uint64_t mgr = 0;
            if (!mem::Read(g_base + sw::kGroupManagerPtrRva, mgr))
            {
                ++g_counters.groupReadFailed;
                return;
            }
            if (!mgr)
            {
                ++g_counters.groupNoManager;
                return;
            }
            const bool sample = clipMs >= g_nextGroupMs;
            if (sample)
                g_nextGroupMs = clipMs + (g_cfg.groupMs ? g_cfg.groupMs : 1000);
            std::uint8_t block[kGroupBlockSize];
            for (std::uint32_t k = 0; k < sw::kGroupCount; ++k)
            {
                const std::uintptr_t group = static_cast<std::uintptr_t>(mgr) + sw::kGroupFirst + k * sw::kGroupStride;
                if (!mem::Copy(block, group + kGroupBlockFrom, kGroupBlockSize))
                {
                    ++g_counters.groupReadFailed;
                    continue;
                }
                auto q = [&block](std::uint32_t offset) {
                    std::uint64_t v = 0;
                    std::memcpy(&v, block + (offset - kGroupBlockFrom), sizeof(v));
                    return v;
                };
                auto d = [&block](std::uint32_t offset) {
                    std::uint32_t v = 0;
                    std::memcpy(&v, block + (offset - kGroupBlockFrom), sizeof(v));
                    return v;
                };
                auto f = [&block](std::uint32_t offset) {
                    float v = 0.0f;
                    std::memcpy(&v, block + (offset - kGroupBlockFrom), sizeof(v));
                    return v;
                };
                const bool active = q(sw::kGroupMeta) != 0;
                const bool loop = active && q(sw::kGroupSound) != 0;
                const bool toggled = active != g_groupActive[k] || loop != g_groupLoop[k];
                if (loop && !g_groupLoop[k])
                    ++g_counters.groupLoopOn;
                if (!loop && g_groupLoop[k])
                    ++g_counters.groupLoopOff;
                const bool wasActive = g_groupActive[k];
                g_groupActive[k] = active;
                g_groupLoop[k] = loop;
                if (!active && !wasActive)
                    continue;
                if (!sample && !toggled)
                    continue;
                sw::GroupView g;
                g.group = k;
                g.sound = loop;
                if (active)
                {
                    g.members = d(sw::kGroupMemberCount);
                    for (std::uint32_t i = 0; i < 4; ++i)
                        g.gaits[i] = d(sw::kGroupGaitCounts + i * 4);
                    g.speed = f(sw::kGroupMeanSpeed);
                    for (std::uint32_t i = 0; i < 3; ++i)
                        g.pos[i] = f(sw::kGroupMeanPos + i * 4);
                    if (!sw::GroupSane(g))
                    {
                        ++g_counters.groupInsane;
                        continue;
                    }
                    std::uint32_t shown = 0;
                    for (std::uint32_t i = 0; i < g.members; ++i)
                    {
                        const std::uint64_t member = q(sw::kGroupMembers + i * 8);
                        if (!member)
                            continue;
                        ++g.nonNull;
                        const std::string id = MemberId(member, g.replay, g.livePlayer);
                        if (!id.empty() && shown < 8)
                        {
                            g.memberIds += (shown ? ";" : "") + id;
                            ++shown;
                        }
                    }
                    const std::uint64_t leader = q(sw::kGroupLeader);
                    census::Owner owner;
                    if (leader && census::OwnerOf(leader, leader, owner) &&
                        (owner.kind == census::OwnerKind::Recorded || owner.kind == census::OwnerKind::Puppet))
                        g.leaderId = owner.id;
                    ++g_counters.groupSamples;
                    if (sw::Movers(g))
                        ++g_counters.groupSamplesMoving;
                    if (loop)
                        ++g_counters.groupSamplesLoop;
                    if (g.members > g_counters.groupMaxMembers)
                        g_counters.groupMaxMembers = g.members;
                    if (g.replay > g_counters.groupMaxReplay)
                        g_counters.groupMaxReplay = g.replay;
                    if (sw::Movers(g) > g_counters.groupMaxMovers)
                        g_counters.groupMaxMovers = sw::Movers(g);
                }
                const bool placed = sw::Movers(g) && (g.pos[0] != 0.0f || g.pos[1] != 0.0f || g.pos[2] != 0.0f);
                const float dist = placed ? ListenerDistance(listener, g.pos) : -1.0f;
                if (!g_groupBudget.Take())
                    continue;
                const char* why = !active ? "inactive" : toggled ? (loop ? "loop_on" : wasActive ? "loop_off" : "active") : "sample";
                log << sw::FormatGroup(g_phase.c_str(), clipMs, why, g, dist) << '\n';
            }
        }
    }

    void Configure(const Config& config) { g_cfg = config; }
    bool Enabled() { return g_cfg.enabled; }

    void Tick(Context& context, const char* phase, double clipMs, bool normal)
    {
        if (!g_cfg.enabled || !phase)
            return;
        if (!Prove(context.log))
            return;
        const std::uint64_t wall = Wall();
        const bool newPass = !g_inPass || g_phase != phase || wall > g_lastWall + kPassGapMs;
        if (newPass)
        {
            ClosePass(context.log, g_inPass && g_phase != phase ? "phase" : "gap");
            OpenPass(phase, clipMs);
        }
        else if (clipMs + 1000.0 < g_lastClipMs)
        {
            // A jump back (a loop or a seek): the slots' baseline starts over.
            g_ptfx.Reset();
            g_nextGroupMs = clipMs > 0.0 ? static_cast<std::uint32_t>(clipMs) : 0;
        }
        g_lastWall = wall;
        g_lastClipMs = clipMs;
        if (wall >= g_budgetWall + kBudgetPeriodMs)
        {
            g_budgetWall = wall;
            g_ptfxBudget.Refill(kPtfxLines);
            g_groupBudget.Refill(kGroupLines);
        }
        if (!normal)
        {
            g_ptfx.Reset(); // a paused or scrubbed clock: the next normal tick is a new baseline
            return;
        }
        ++g_counters.ticks;
        const std::uint32_t ms = clipMs > 0.0 ? static_cast<std::uint32_t>(clipMs) : 0;
        const Listener listener = ReadListener();
        if (g_ptfxOn)
            TickPtfx(context.log, ms, listener);
        if (g_groupOn)
            TickGroups(context.log, ms, listener);
    }

    void Idle(Context& context)
    {
        if (g_inPass && Wall() > g_lastWall + kPassGapMs)
            ClosePass(context.log, "gap");
    }

    void EndPlayback(Context& context)
    {
        if (g_inPass && g_phase == "play")
            ClosePass(context.log, "playback_end");
    }
}
