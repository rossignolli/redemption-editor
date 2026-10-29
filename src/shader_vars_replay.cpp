#include "shader_vars_replay.h"

#include "entity_cache.h"
#include "entity_lifecycle.h"
#include "log_format.h"
#include "muscle_watch.h"
#include "shader_vars_core.h"
#include "shader_vars_hook.h"

#include "natives.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace replay::muscles
{
    namespace
    {
        namespace sv = replay::shadervars;
        namespace sve = replay::shadervars::engine;

        Context* g_context = nullptr;
        Context& Ctx() { return *g_context; }
        Config g_config;

        // A layout read from a component (re-read when the component changes).
        struct ComponentCache
        {
            std::uintptr_t component = 0;
            sv::Layout layout;
            bool ok = false;
        };

        bool ReadComponent(std::uintptr_t ped, ComponentCache& cache, sve::Read& result, sv::ParseResult& parse)
        {
            std::uintptr_t component = 0;
            result = sve::ComponentOf(ped, component);
            parse = sv::ParseResult::Ok;
            if (result != sve::Read::Ok)
                return false;
            if (component != cache.component)
            {
                cache.component = component;
                result = sve::LayoutOf(component, cache.layout, parse);
                cache.ok = result == sve::Read::Ok;
            }
            return cache.ok;
        }

        // MuscleDebugForce: every track 0x17 lane of the layout at 1.0 (1) or 0.0 (2).
        void ForcedValues(const sv::Layout& layout, std::uint8_t force, std::vector<std::pair<std::uint16_t, std::uint32_t>>& out)
        {
            out.clear();
            const std::uint32_t bits = sv::BitsOf(force == 1 ? 1.0f : 0.0f);
            for (std::size_t n = 0; n < layout.dofs.size(); ++n)
            {
                if (layout.dofs[n].track != sv::kTrackAnimatedNormalMaps)
                    continue;
                const std::uint32_t lanes = sv::LanesOf(layout.dofs[n].type);
                for (std::uint32_t k = 0; k < lanes; ++k)
                    out.emplace_back(static_cast<std::uint16_t>(layout.offsets[n] + k * 4u), bits);
            }
        }

        std::string BindingsOf(std::uintptr_t component)
        {
            std::string text;
            std::uint32_t count = 0;
            if (sve::BindingsText(component, text, count) != sve::Read::Ok)
                return "read_failed";
            return std::to_string(count) + "[" + text + "]";
        }

        std::string LanesText(const std::vector<std::uint32_t>& bits, std::size_t first = 8)
        {
            std::string text;
            char item[24];
            for (std::size_t k = 0; k < bits.size() && k < first; ++k)
            {
                if (sv::IsSet(bits[k]))
                    std::snprintf(item, sizeof(item), "%s%.3f", text.empty() ? "" : ";", sv::FloatOf(bits[k]));
                else
                    std::snprintf(item, sizeof(item), "%sx", text.empty() ? "" : ";");
                text += item;
            }
            return text.empty() ? "-" : text;
        }

        std::string IdOf(std::uint32_t owner) { return replay::entity::IdText(replay::entity::ReplayId::FromPacked(owner)); }

#if defined(REDEMPTION_DEV)
        std::string Histogram(const sv::Layout& layout)
        {
            std::uint32_t perTrack[256] = {};
            for (const sv::Dof& dof : layout.dofs)
                ++perTrack[dof.track];
            return replay::musclewatch::HistogramText(perTrack);
        }

        constexpr std::uint32_t kTickLineCap = 300;
        constexpr std::uint32_t kCmpLineCap = 240;
        constexpr std::uint32_t kCmpBlockMs = 2000;

        struct Evidence
        {
            replay::musclewatch::Watch watch;
            bool described = false;
            std::uint32_t model = 0;
        };

        void Observe(const char* phase, std::uint32_t owner, Evidence& evidence, const sv::Layout& layout,
            const std::vector<std::uint32_t>& bits, std::uint32_t& tickLines, std::uint32_t timeMs, std::uintptr_t component)
        {
            if (!evidence.described)
            {
                evidence.described = true;
                Ctx().log << "MUSCLE_NM_DOFS,phase=" << phase << ",id=" << IdOf(owner) << ",model=0x" << std::hex
                          << evidence.model << std::dec << ",source=shader_vars,component=0x" << std::hex << component
                          << std::dec << ",dofs=" << layout.dofs.size() << ",nm=" << layout.CountTrack(sv::kTrackAnimatedNormalMaps)
                          << ",lanes=" << layout.lanes << ",skipped=" << layout.skipped << ",tracks=" << Histogram(layout)
                          << ",values=" << LanesText(bits, 16) << ",bindings=" << BindingsOf(component) << ",ms=" << timeMs << '\n';
            }
            std::vector<std::uint16_t> ids;
            std::vector<std::uint32_t> lanes;
            sv::TrackLanes(layout.dofs, bits, sv::kTrackAnimatedNormalMaps, ids, lanes);
            const replay::musclewatch::Tick tick = replay::musclewatch::Observe(evidence.watch, ids, lanes);
            if ((tick.alternation || tick.setFlips) && tickLines < kTickLineCap)
            {
                ++tickLines;
                const replay::logfmt::FormatScope logFormat(Ctx().log);
                Ctx().log << std::fixed << std::setprecision(3) << "MUSCLE_NM_TICK,phase=" << phase << ",id=" << IdOf(owner)
                          << ",tick=" << evidence.watch.ticks << ",set=" << tick.set << ",unset=" << tick.unset
                          << ",set_flips=" << tick.setFlips << ",alternating=" << tick.alternating
                          << ",max_delta=" << tick.maxDelta << ",worst_lane=" << tick.worstId << ",sum=" << tick.sum
                          << ",ms=" << timeMs << '\n';
            }
        }

        void Summary(const char* phase, const std::unordered_map<std::uint32_t, Evidence>& all)
        {
            const replay::logfmt::FormatScope logFormat(Ctx().log);
            std::uint32_t lines = 0;
            std::uint64_t ticks = 0, changed = 0, alternations = 0, flips = 0;
            std::size_t moving = 0;
            for (const auto& [owner, evidence] : all)
            {
                const replay::musclewatch::Watch& w = evidence.watch;
                ticks += w.ticks;
                changed += w.changedTicks;
                alternations += w.alternationTicks;
                flips += w.setFlips;
                moving += w.changedTicks ? 1u : 0u;
                if (lines++ >= 64)
                    continue;
                Ctx().log << std::fixed << std::setprecision(3) << "MUSCLE_NM_SUMMARY,phase=" << phase << ",id=" << IdOf(owner)
                          << ",model=0x" << std::hex << evidence.model << std::dec << ",nm_lanes=" << w.ids.size()
                          << ",ticks=" << w.ticks << ",changed_ticks=" << w.changedTicks
                          << ",alternation_ticks=" << w.alternationTicks << ",set_flips=" << w.setFlips
                          << ",unset_values=" << w.unsetValues << ",max_delta=" << w.maxDelta
                          << ",max_delta_lane=" << w.maxDeltaId << '\n';
            }
            if (!all.empty())
                Ctx().log << "MUSCLE_NM_TOTAL,phase=" << phase << ",horses=" << all.size() << ",moving=" << moving
                          << ",ticks=" << ticks << ",changed_ticks=" << changed << ",alternation_ticks=" << alternations
                          << ",set_flips=" << flips << '\n';
        }
#endif

        // ---- recording ----------------------------------------------------------
        struct RecState
        {
            ComponentCache cache;
            std::uint32_t lastSet = 0;
            std::uint32_t tornRun = 0;
            std::vector<std::uint32_t> carry; // the last set value of each lane
            std::vector<sv::Dof> withSweat; // the layout plus the sweat lane
            float lastSweat = -1.0f;
            std::uint8_t lastSweatOn = 0;
            std::int32_t slot = -1;      // the write node's snapshot slot
            std::uint32_t seq = 0;       // the last snapshot taken
            std::uintptr_t ped = 0;
#if defined(REDEMPTION_DEV)
            Evidence evidence;
#endif
        };
        bool g_recActive = false;
        std::uint32_t g_recStartMs = 0, g_recDurationMs = 0;
        std::unordered_map<std::uint32_t, RecState> g_rec;
        struct RecTotals
        {
            std::uint64_t noted = 0;
            std::uint64_t refused = 0;
            std::uint64_t noComponent = 0;
            std::uint64_t layoutFailed = 0;
            std::uint64_t readFailed = 0;
            std::uint64_t torn = 0;
            std::uint64_t empty = 0;      // node copies with no lane set (not points)
            std::uint64_t carried = 0;    // unset lanes given their last value
            std::uint64_t sweatReads = 0, sweatPositive = 0, sweatOn = 0;
            float sweatMax = 0.0f;
            std::uint64_t fromNode = 0;   // points from the write node's snapshot
            std::uint64_t fromScript = 0; // points from a script-thread read (no hook)
            std::string initError = "none";
            std::string lastLayoutError = "none";
        } g_recTotals;
#if defined(REDEMPTION_DEV)
        std::uint32_t g_recTickLines = 0;
#endif

        // ---- playback -----------------------------------------------------------
        struct PlayState
        {
            ComponentCache cache;
            std::vector<sv::Dof> mappedFrom; // the recorded layout the writes are for
            std::vector<sv::Write> writes;
            std::uint32_t missing = 0;
            std::uint64_t ticks = 0;     // ticks with a recorded value published
            std::uint64_t noValue = 0;   // ticks without a recorded value at the playhead
            float sweat = -1.0f;         // the recorded sweat given last
#if defined(REDEMPTION_DEV)
            Evidence evidence;
            std::vector<std::uint32_t> lastPublished; // the lanes published last tick (recorded layout)
            std::vector<std::uint32_t> lastRecorded;
            std::uint32_t loggedBlock = 0xFFFFFFFFu;
            std::uint64_t compared = 0;
            float maxAbs = 0.0f;
            float recordedMaxStep = 0.0f;
            float puppetMaxStep = 0.0f;
            std::vector<std::uint32_t> lastPuppet;
#endif
        };
        struct PhaseCounts
        {
            std::uint64_t ticks = 0, hits = 0, shaderReads = 0, shaderMatched = 0, shaderLanes = 0;
        };
        PhaseCounts g_phase[3]; // play, pause, scrub
        int g_lastPhase = 0;
        sve::Stats g_lastStats;
        bool g_liveForced = false;
        std::uint32_t g_liveLines = 0;
        ULONGLONG g_liveLogAt = 0;
        bool g_playActive = false;
        bool g_installFailed = false;
        std::string g_installError = "none";
        std::string g_observerError;
        std::unordered_map<std::uint32_t, PlayState> g_play;
        std::vector<sve::PlayTarget> g_targets;
        sve::Stats g_playBase;
        std::uint64_t g_playTicks = 0;
        std::uint64_t g_playPublished = 0;
        std::uint32_t g_playMaxTargets = 0;
        std::uint64_t g_playNoComponent = 0;
        std::uint64_t g_playSweatTicks = 0;
#if defined(REDEMPTION_DEV)
        std::uint32_t g_playTickLines = 0;
        std::uint32_t g_cmpLines = 0;
#endif

        void BeginPlay()
        {
            g_playActive = true;
            g_play.clear();
            g_targets.clear();
            g_playTicks = 0;
            g_playPublished = 0;
            g_playMaxTargets = 0;
            g_playNoComponent = 0;
            g_playSweatTicks = 0;
#if defined(REDEMPTION_DEV)
            g_playTickLines = 0;
            g_cmpLines = 0;
#endif
            if (g_config.replay && !g_installFailed && !sve::Installed())
            {
                std::string error;
                if (!sve::Install(error, g_observerError))
                {
                    g_installError = error.empty() ? "unknown" : error;
                    // A signature mismatch does not get better; a type byte not
                    // set yet does (the next playback tries again).
                    g_installFailed = error != "type_byte_not_ready";
                }
                else
                    g_installError = "none";
            }
            sve::SetReplace(!g_config.debugOverwrite);
            g_playBase = sve::Snapshot();
            g_lastStats = g_playBase;
            g_lastPhase = 0;
            for (PhaseCounts& c : g_phase)
                c = PhaseCounts{};
            const sv::Track& track = Ctx().clip.shaderVars;
            Ctx().log << "MUSCLE_REPLAY_BEGIN,enabled=" << (g_config.replay ? 1 : 0)
                      << ",installed=" << (sve::Installed() ? 1 : 0) << ",error=" << g_installError
                      << ",writer=write_node,observer=" << (sve::ObserverInstalled() ? 1 : 0)
                      << ",observer_error=" << (g_observerError.empty() ? "none" : g_observerError.c_str())
                      << ",section=" << (track.Empty() ? "none" : "SHVR") << ",rec_horses=" << track.series.size()
                      << ",rec_points=" << track.Points() << ",mode=" << (g_config.debugOverwrite ? "overwrite" : "replace")
                      << ",debug_force=" << static_cast<int>(g_config.debugForce) << ",debug_sweat=" << g_config.debugSweat
                      << '\n';
        }
    }

    void SetConfig(const Config& config) { g_config = config; }

    void Attach(Context& context) { g_context = &context; }

    namespace
    {
        // The sample's layout plus the sweat lane (kTrackSweat) when the
        // sweat reader is in; its value goes at the end of bits.
        const std::vector<sv::Dof>& WithSweat(RecState& state, std::uintptr_t ped, std::vector<std::uint32_t>& bits)
        {
            if (!sve::SweatReady())
                return state.cache.layout.dofs;
            float sweat = 0.0f;
            std::uint8_t on = 0;
            const bool read = sve::SweatOf(ped, sweat, on) == sve::Read::Ok && std::isfinite(sweat);
            state.lastSweat = read ? sweat : -1.0f;
            state.lastSweatOn = on;
            if (read)
            {
                ++g_recTotals.sweatReads;
                g_recTotals.sweatPositive += sweat > 0.0f ? 1u : 0u;
                g_recTotals.sweatOn += on ? 1u : 0u;
                g_recTotals.sweatMax = (std::max)(g_recTotals.sweatMax, sweat);
            }
            if (state.withSweat.size() != state.cache.layout.dofs.size() + 1 ||
                !std::equal(state.cache.layout.dofs.begin(), state.cache.layout.dofs.end(), state.withSweat.begin()))
            {
                state.withSweat = state.cache.layout.dofs;
                state.withSweat.push_back(sv::Dof{sv::kTrackSweat, 2, sv::kSweatId});
            }
            bits.push_back(read ? sv::BitsOf(sweat) : sv::kNotSet);
            return state.withSweat;
        }
    }

    void TickRecord(std::uint32_t nowMs)
    {
        if (!g_context || !g_config.record)
            return;
        if (g_recActive)
            g_recDurationMs = nowMs > g_recStartMs ? nowMs - g_recStartMs : 0;
        else
            g_recStartMs = nowMs;
        if (!g_recActive)
        {
            g_recActive = true;
            g_rec.clear();
            g_recTotals = RecTotals{};
#if defined(REDEMPTION_DEV)
            g_recTickLines = 0;
#endif
            std::string error;
            if (!sve::Init(error))
                g_recTotals.initError = error.empty() ? "unknown" : error;
            // The write node's snapshots: the values as the shader reads them.
            if (!sve::Installed() && !g_installFailed && sve::Ready())
            {
                if (!sve::Install(error, g_observerError))
                {
                    g_installError = error.empty() ? "unknown" : error;
                    g_installFailed = error != "type_byte_not_ready";
                }
            }
        }
        if (!sve::Ready())
        {
            std::string error;
            if (!sve::Init(error))
                return;
            g_recTotals.initError = "none";
        }
        std::vector<std::uint32_t> bits;
        sv::Track& track = Ctx().clip.shaderVars;
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (entity.vehicle)
                continue;
            const replay::entity::CreatePacket* create = Ctx().clip.FindCreate(entity.recordId);
            if (!create || create->kind != replay::entity::EntityKind::Horse)
                continue;
            const std::uint32_t owner = entity.id.Packed();
            if (!track.Due(owner, nowMs))
                continue;
            const std::uintptr_t ped = reinterpret_cast<std::uintptr_t>(replay::entities::AddressOf(entity.handle));
            RecState& state = g_rec[owner];
            sve::Read result = sve::Read::Ok;
            sv::ParseResult parse = sv::ParseResult::Ok;
            if (!ReadComponent(ped, state.cache, result, parse))
            {
                if (result == sve::Read::NoComponent)
                    ++g_recTotals.noComponent;
                else
                {
                    ++g_recTotals.layoutFailed;
                    g_recTotals.lastLayoutError =
                        std::string(sve::ReadText(result)) + (parse != sv::ParseResult::Ok ? std::string("_") + sv::ParseResultText(parse) : "");
                }
                continue;
            }
            // The write node's copy when the hook is in: the buffer right
            // after the engine's write (the script thread runs beside the
            // job: a read of its own can land in the 0xFF fill, which the
            // clips of an earlier build kept as unset lanes, flat muscles).
            if (sve::Installed())
            {
                state.ped = ped;
                if (state.slot < 0)
                {
                    for (std::uint32_t s = 0; s < sve::kMaxTargets && state.slot < 0; ++s)
                    {
                        bool used = false;
                        for (const auto& [o, other] : g_rec)
                            used = used || other.slot == static_cast<std::int32_t>(s);
                        if (!used)
                            state.slot = static_cast<std::int32_t>(s);
                    }
                }
                static std::vector<std::uint8_t> snapshot;
                if (state.slot < 0 || !sve::TakeSnapshot(static_cast<std::uint32_t>(state.slot), state.seq, snapshot))
                    continue; // nothing new since the last point (the node did not run)
                if (!sv::Gather(state.cache.layout, snapshot.data(), snapshot.size(), bits))
                {
                    ++g_recTotals.readFailed;
                    continue;
                }
                // A frame the engine left without values (clip 1275: every
                // other sample of a horse has all 64 lanes unset, the write
                // node's 0xFF fill with nothing mapped) is not a point: the
                // playback interpolates over it (At, lane by lane).
                bool anySet = false;
                for (const std::uint32_t v : bits)
                    anySet = anySet || sv::IsSet(v);
                if (!anySet)
                {
                    ++g_recTotals.empty;
                    continue;
                }
                // A lane the engine left unset in this pass keeps its last
                // value (the shader goes on drawing it): the track stores full
                // samples (shader_vars_core.h CarryForward).
                g_recTotals.carried += sv::CarryForward(state.carry, bits);
                if (track.Note(owner, nowMs, WithSweat(state, ped, bits), bits))
                {
                    ++g_recTotals.noted;
                    ++g_recTotals.fromNode;
                }
                else
                    ++g_recTotals.refused;
#if defined(REDEMPTION_DEV)
                state.evidence.model = create->modelHash;
                Observe("rec", owner, state.evidence, state.cache.layout, bits, g_recTickLines, nowMs, state.cache.component);
#endif
                continue;
            }
            if (sve::ValuesOf(state.cache.component, state.cache.layout, bits) != sve::Read::Ok)
            {
                ++g_recTotals.readFailed;
                continue;
            }
            ++g_recTotals.fromScript;
            // The animation job fills the buffer with 0xFF before it copies
            // the values: a read that loses set lanes may have caught it.
            // Skipped, unless it lasts (then the lanes are really unset).
            std::uint32_t set = 0;
            for (const std::uint32_t v : bits)
                set += sv::IsSet(v) ? 1u : 0u;
            if (set < state.lastSet && state.tornRun < 2)
            {
                ++state.tornRun;
                ++g_recTotals.torn;
                continue;
            }
            state.tornRun = 0;
            state.lastSet = set;
            if (track.Note(owner, nowMs, WithSweat(state, ped, bits), bits))
                ++g_recTotals.noted;
            else
                ++g_recTotals.refused;
#if defined(REDEMPTION_DEV)
            state.evidence.model = create->modelHash;
            Observe("rec", owner, state.evidence, state.cache.layout, bits, g_recTickLines, nowMs, state.cache.component);
#endif
        }
        if (sve::Installed())
        {
            // Every recorded horse with a slot, published every tick (a puppet
            // of the playback never shares the table with the recording).
            static std::vector<sve::RecordTarget> targets;
            targets.clear();
            for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
            {
                const auto it = g_rec.find(entity.id.Packed());
                if (it == g_rec.end() || it->second.slot < 0 || !it->second.cache.ok)
                    continue;
                sve::RecordTarget target;
                target.ped = reinterpret_cast<std::uintptr_t>(replay::entities::AddressOf(entity.handle));
                target.slot = static_cast<std::uint32_t>(it->second.slot);
                if (target.ped && sve::BufferOf(it->second.cache.component, target.buffer, target.bytes) == sve::Read::Ok)
                    targets.push_back(target);
            }
            sve::SetRecordTargets(targets);
        }
    }

    void EndRecord()
    {
        if (!g_recActive || !g_context)
            return;
        g_recActive = false;
        if (sve::Installed())
            sve::SetRecordTargets({});
        Ctx().log << "MUSCLE_RECORD_END,enabled=" << (g_config.record ? 1 : 0) << ",init_error=" << g_recTotals.initError
                  << ",horses=" << g_rec.size() << ",track_horses=" << Ctx().clip.shaderVars.series.size()
                  << ",points=" << Ctx().clip.shaderVars.Points() << ",noted=" << g_recTotals.noted
                  << ",refused=" << g_recTotals.refused << ",no_component=" << g_recTotals.noComponent
                  << ",layout_failed=" << g_recTotals.layoutFailed << ",layout_error=" << g_recTotals.lastLayoutError
                  << ",read_failed=" << g_recTotals.readFailed << ",torn_skipped=" << g_recTotals.torn
                  << ",empty_skipped=" << g_recTotals.empty << ",lanes_carried=" << g_recTotals.carried << ",sweat_reader=" << (sve::SweatReady() ? 1 : 0)
                  << ",sweat_reads=" << g_recTotals.sweatReads << ",sweat_positive=" << g_recTotals.sweatPositive
                  << ",sweat_script_on=" << g_recTotals.sweatOn << ",sweat_max=" << g_recTotals.sweatMax << ",from_node=" << g_recTotals.fromNode << ",from_script=" << g_recTotals.fromScript
                  << ",hook=" << (sve::Installed() ? 1 : 0) << ",hook_error=" << g_installError
                  << ",snapshots=" << sve::Snapshot().snapshots << ",node_empty_passes=" << sve::Snapshot().snapshotsEmpty
                  << ",points_per_s=" << (g_recDurationMs ? static_cast<double>(g_recTotals.noted) * 1000.0 /
                                                                 static_cast<double>(g_recDurationMs) / (std::max<std::size_t>)(1, g_rec.size())
                                                           : 0.0) << '\n';
#if defined(REDEMPTION_DEV)
        std::unordered_map<std::uint32_t, Evidence> all;
        for (const auto& [owner, state] : g_rec)
            if (state.evidence.described)
                all[owner] = state.evidence;
        Summary("rec", all);
#endif
        g_rec.clear();
    }

    void TickPlay(std::uint32_t timeMs)
    {
        if (!g_context)
            return;
        if (!g_playActive)
            BeginPlay();
        ++g_playTicks;
        // The detour's counts of the last tick go to the transport state they
        // ran in (play, pause, scrub): the look differs between them.
        {
            const sve::Stats now = sve::Snapshot();
            const int phase = Ctx().transport.Scrubbing() ? 2 : Ctx().transport.Paused() ? 1 : 0;
            PhaseCounts& c = g_phase[g_lastPhase];
            c.ticks += 1;
            c.hits += now.hits - g_lastStats.hits;
            c.shaderReads += now.shaderReads - g_lastStats.shaderReads;
            c.shaderMatched += now.shaderMatched - g_lastStats.shaderMatched;
            c.shaderLanes += now.shaderLanes - g_lastStats.shaderLanes;
            g_lastStats = now;
            g_lastPhase = phase;
        }
        const sv::Track& track = Ctx().clip.shaderVars;
        g_targets.clear();
        std::vector<std::uint32_t> bits;
        for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (puppet.packet.kind != replay::entity::EntityKind::Horse || !puppet.clone)
                continue;
            const std::uint32_t owner = puppet.packet.id.Packed();
            const std::vector<sv::Dof>* dofs = nullptr;
            PlayState& state = g_play[owner];
            if (track.Empty() || !track.At(owner, timeMs, dofs, bits) || !dofs)
            {
                ++state.noValue;
#if !defined(REDEMPTION_DEV)
                continue;
#endif
            }
            const std::uintptr_t ped = reinterpret_cast<std::uintptr_t>(replay::entities::AddressOf(puppet.clone));
            sve::Read result = sve::Read::Ok;
            sv::ParseResult parse = sv::ParseResult::Ok;
            if (!ReadComponent(ped, state.cache, result, parse))
            {
                ++g_playNoComponent;
                continue;
            }
            if (g_config.debugSweat >= 0.0f)
                PED::SET_PED_SWEAT(puppet.clone, g_config.debugSweat);
            else if (dofs && g_config.replay)
            {
                // The recorded sweat (the lane kTrackSweat): the shader effect
                // copies the graphics component's +0x488 every update; the
                // native writes it (+0x488) with the script flag (+0x484).
                std::uint32_t lane = 0;
                for (const sv::Dof& dof : *dofs)
                {
                    if (dof.track == sv::kTrackSweat)
                    {
                        if (lane < bits.size() && sv::IsSet(bits[lane]) && sv::FloatOf(bits[lane]) >= 0.0f)
                        {
                            PED::SET_PED_SWEAT(puppet.clone, sv::FloatOf(bits[lane]));
                            state.sweat = sv::FloatOf(bits[lane]);
                            ++g_playSweatTicks;
                        }
                        break;
                    }
                    lane += sv::LanesOf(dof.type);
                }
            }
            if (g_config.debugForce && sve::Installed() && g_targets.size() < sve::kMaxTargets)
            {
                sve::PlayTarget target;
                target.ped = ped;
                if (sve::BufferOf(state.cache.component, target.buffer, target.bytes) == sve::Read::Ok)
                {
                    ForcedValues(state.cache.layout, g_config.debugForce, target.values);
                    if (!target.values.empty())
                    {
                        g_targets.push_back(std::move(target));
                        ++state.ticks;
                    }
                }
            }
            else if (dofs)
            {
                if (state.mappedFrom != *dofs || state.writes.empty())
                {
                    state.mappedFrom = *dofs;
                    sv::MapLayout(*dofs, state.cache.layout, state.writes, state.missing);
                }
                if (g_config.replay && sve::Installed() && !state.writes.empty() && g_targets.size() < sve::kMaxTargets)
                {
                    sve::PlayTarget target;
                    target.ped = ped;
                    if (sve::BufferOf(state.cache.component, target.buffer, target.bytes) != sve::Read::Ok)
                    {
                        ++g_playNoComponent;
                        continue;
                    }
                    target.values.reserve(state.writes.size());
                    // A lane the recording had unset is left to the engine (a
                    // clip of an earlier build has them where its read met
                    // the job's 0xFF fill: written, they flattened the muscles).
                    for (const sv::Write& w : state.writes)
                        if (w.lane < bits.size() && sv::IsSet(bits[w.lane]))
                            target.values.emplace_back(w.offset, bits[w.lane]);
                    if (target.values.empty())
                        continue;
                    g_targets.push_back(std::move(target));
                    ++state.ticks;
                }
            }
#if defined(REDEMPTION_DEV)
            // The puppet's values now: what the detour wrote last frame (or the
            // puppet's own, without the section or the hook), against the
            // recording at the playhead of that frame.
            std::vector<std::uint32_t> puppetBits;
            if (sve::ValuesOf(state.cache.component, state.cache.layout, puppetBits) != sve::Read::Ok)
                continue;
            state.evidence.model = puppet.packet.modelHash;
            Observe("play", owner, state.evidence, state.cache.layout, puppetBits, g_playTickLines, timeMs, state.cache.component);
            if (!state.lastPuppet.empty())
                state.puppetMaxStep = (std::max)(state.puppetMaxStep, sv::MaxStep(state.lastPuppet, puppetBits));
            state.lastPuppet = puppetBits;
            if (!state.lastRecorded.empty() && dofs && state.lastRecorded.size() == bits.size())
                state.recordedMaxStep = (std::max)(state.recordedMaxStep, sv::MaxStep(state.lastRecorded, bits));
            // The puppet's lanes in the recorded layout (the writes' order).
            std::vector<std::uint32_t> puppetInRecorded;
            if (!state.lastPublished.empty())
            {
                puppetInRecorded.assign(state.lastPublished.size(), sv::kNotSet);
                for (std::size_t n = 0; n < state.writes.size(); ++n)
                {
                    const sv::Write& w = state.writes[n];
                    // The puppet's value at w.offset: its lane in its own layout.
                    std::uint32_t lane = 0;
                    for (std::size_t d = 0; d < state.cache.layout.dofs.size(); ++d)
                    {
                        const std::uint32_t lanes = sv::LanesOf(state.cache.layout.dofs[d].type);
                        const std::uint32_t begin = state.cache.layout.offsets[d];
                        if (w.offset >= begin && w.offset < begin + lanes * 4u)
                        {
                            lane += (w.offset - begin) / 4u;
                            break;
                        }
                        lane += lanes;
                    }
                    if (w.lane < puppetInRecorded.size() && lane < puppetBits.size())
                        puppetInRecorded[w.lane] = puppetBits[lane];
                }
                const sv::Compare cmp = sv::CompareLanes(state.lastPublished, puppetInRecorded);
                ++state.compared;
                state.maxAbs = (std::max)(state.maxAbs, cmp.maxAbs);
                const std::uint32_t block = timeMs / kCmpBlockMs;
                if (block != state.loggedBlock && g_cmpLines < kCmpLineCap)
                {
                    state.loggedBlock = block;
                    ++g_cmpLines;
                    const replay::logfmt::FormatScope logFormat(Ctx().log);
                    Ctx().log << std::fixed << std::setprecision(3) << "MUSCLE_NM_CMP,phase=play,id=" << IdOf(owner)
                              << ",ms=" << timeMs << ",lanes=" << cmp.lanes << ",both_set=" << cmp.bothSet
                              << ",set_mismatch=" << cmp.setMismatch << ",max_abs=" << cmp.maxAbs
                              << ",worst_lane=" << cmp.worstLane << ",missing_dofs=" << state.missing
                              << ",rec_step_max=" << state.recordedMaxStep << ",puppet_step_max=" << state.puppetMaxStep
                              << ",sweat=" << state.sweat << ",paused=" << (Ctx().transport.Paused() ? 1 : 0) << ",scrub=" << (Ctx().transport.Scrubbing() ? 1 : 0)
                              << ",rec=" << LanesText(state.lastPublished) << ",pup=" << LanesText(puppetInRecorded)
                              << ",same_layout=" << (state.cache.layout.dofs == state.mappedFrom ? 1 : 0) << '\n';
                }
            }
            state.lastPublished = dofs ? bits : std::vector<std::uint32_t>{};
            state.lastRecorded = dofs ? bits : std::vector<std::uint32_t>{};
#endif
        }
        g_playMaxTargets = (std::max)(g_playMaxTargets, static_cast<std::uint32_t>(g_targets.size()));
        g_playPublished += g_targets.size();
        if (sve::Installed())
            sve::SetPlayTargets(g_targets);
    }

    void EndPlay()
    {
        if (!g_playActive || !g_context)
            return;
        g_playActive = false;
        g_targets.clear();
        if (sve::Installed())
            sve::SetPlayTargets(g_targets);
        const sve::Stats now = sve::Snapshot();
        const auto since = [](std::uint64_t a, std::uint64_t b) { return a >= b ? a - b : a; };
        std::uint64_t noValue = 0, written = 0;
        std::uint32_t missing = 0;
        for (const auto& [owner, state] : g_play)
        {
            noValue += state.noValue;
            written += state.ticks;
            missing = (std::max)(missing, state.missing);
        }
        Ctx().log << "MUSCLE_REPLAY_END,installed=" << (sve::Installed() ? 1 : 0) << ",error=" << g_installError
                  << ",horses=" << g_play.size() << ",ticks=" << g_playTicks << ",puppet_ticks_published=" << written
                  << ",puppet_ticks_no_value=" << noValue << ",max_targets=" << g_playMaxTargets
                  << ",no_component=" << g_playNoComponent << ",missing_dofs_max=" << missing
                  << ",sweat_ticks=" << g_playSweatTicks << ",sweat_reader=" << (sve::SweatReady() ? 1 : 0)
                  << ",node_calls=" << since(now.nodeCalls, g_playBase.nodeCalls) << ",hits=" << since(now.hits, g_playBase.hits)
                  << ",writes=" << since(now.writes, g_playBase.writes) << ",values=" << since(now.values, g_playBase.values)
                  << ",failed=" << since(now.failed, g_playBase.failed) << ",mismatch=" << since(now.mismatch, g_playBase.mismatch)
                  << ",engine_set=" << since(now.engineSet, g_playBase.engineSet) << "/"
                  << since(now.engineLanes, g_playBase.engineLanes) << ",publish_waits=" << since(now.retries, g_playBase.retries)
                  << ",shader_calls=" << since(now.shaderCalls, g_playBase.shaderCalls)
                  << ",shader_reads=" << since(now.shaderReads, g_playBase.shaderReads)
                  << ",shader_matched=" << since(now.shaderMatched, g_playBase.shaderMatched) << "/"
                  << since(now.shaderLanes, g_playBase.shaderLanes) << ",replaced=" << since(now.replaced, g_playBase.replaced)
                  << ",effect_slots=" << (now.effectSlotsMin == 0xFFFF ? 0u : now.effectSlotsMin) << "-" << now.effectSlotsMax
                  << '\n';
        static const char* const kPhaseNames[3] = {"play", "pause", "scrub"};
        for (int p = 0; p < 3; ++p)
            Ctx().log << "MUSCLE_REPLAY_PHASE,phase=" << kPhaseNames[p] << ",ticks=" << g_phase[p].ticks << ",hits=" << g_phase[p].hits
                      << ",shader_reads=" << g_phase[p].shaderReads << ",shader_matched=" << g_phase[p].shaderMatched << "/"
                      << g_phase[p].shaderLanes << '\n';
#if defined(REDEMPTION_DEV)
        std::unordered_map<std::uint32_t, Evidence> all;
        {
            const replay::logfmt::FormatScope logFormat(Ctx().log);
            std::uint32_t lines = 0;
            for (const auto& [owner, state] : g_play)
            {
                if (state.evidence.described)
                    all[owner] = state.evidence;
                if (lines++ >= 64)
                    continue;
                const sv::Series* series = Ctx().clip.shaderVars.Find(owner);
                Ctx().log << std::fixed << std::setprecision(3) << "MUSCLE_NM_CMP_SUMMARY,id=" << IdOf(owner)
                          << ",no_value_ticks=" << state.noValue << ",published_ticks=" << state.ticks
                          << ",rec_from_ms=" << (series && !series->blocks.empty() ? static_cast<long long>(series->blocks.front().Front()) : -1)
                          << ",rec_to_ms=" << (series && !series->blocks.empty() ? static_cast<long long>(series->blocks.back().Back()) : -1)
                          << ",compared=" << state.compared << ",max_abs=" << state.maxAbs
                          << ",rec_step_max=" << state.recordedMaxStep << ",puppet_step_max=" << state.puppetMaxStep << '\n';
            }
        }
        Summary("play", all);
#endif
        g_play.clear();
    }

    void TickLive(bool playing)
    {
        if (!g_context || playing || g_playActive)
            return;
        if (!g_config.debugForce)
        {
            if (g_liveForced)
            {
                g_liveForced = false;
                g_targets.clear();
                sve::SetPlayTargets(g_targets);
                Ctx().log << "MUSCLE_DEBUG_LIVE,force=0,released=1\n";
            }
            return;
        }
        if (!sve::Installed() && !g_installFailed)
        {
            std::string error;
            if (!sve::Install(error, g_observerError))
            {
                g_installError = error.empty() ? "unknown" : error;
                g_installFailed = error != "type_byte_not_ready";
                return;
            }
        }
        if (!sve::Installed())
            return;
        sve::SetReplace(!g_config.debugOverwrite);
        const Ped player = PLAYER::PLAYER_PED_ID();
        Ped horse = PED::GET_MOUNT(player);
        if (!horse)
            horse = PED::_GET_LAST_MOUNT(player);
        g_targets.clear();
        static ComponentCache cache;
        sve::Read result = sve::Read::Ok;
        sv::ParseResult parse = sv::ParseResult::Ok;
        const std::uintptr_t ped = horse ? reinterpret_cast<std::uintptr_t>(replay::entities::AddressOf(horse)) : 0;
        sve::PlayTarget target;
        target.ped = ped;
        if (ped && ReadComponent(ped, cache, result, parse) &&
            sve::BufferOf(cache.component, target.buffer, target.bytes) == sve::Read::Ok)
        {
            ForcedValues(cache.layout, g_config.debugForce, target.values);
            if (!target.values.empty())
                g_targets.push_back(std::move(target));
        }
        sve::SetPlayTargets(g_targets);
        g_liveForced = true;
        const ULONGLONG now = GetTickCount64();
        if (now >= g_liveLogAt && g_liveLines < 60)
        {
            g_liveLogAt = now + 5000;
            ++g_liveLines;
            std::vector<std::uint32_t> bits;
            const bool read = ped && cache.ok && sve::ValuesOf(cache.component, cache.layout, bits) == sve::Read::Ok;
            const sve::Stats s = sve::Snapshot();
            Ctx().log << "MUSCLE_DEBUG_LIVE,force=" << static_cast<int>(g_config.debugForce)
                      << ",mode=" << (g_config.debugOverwrite ? "overwrite" : "replace") << ",horse=" << horse
                      << ",component=" << sve::ReadText(result) << ",values=" << g_targets.size()
                      << ",nm=" << cache.layout.CountTrack(sv::kTrackAnimatedNormalMaps) << ",now="
                      << (read ? LanesText(bits, 16) : std::string("-")) << ",hits=" << s.hits
                      << ",shader_reads=" << s.shaderReads << ",shader_matched=" << s.shaderMatched << "/" << s.shaderLanes
                      << ",bindings=" << (cache.ok ? BindingsOf(cache.component) : std::string("-")) << '\n';
        }
    }

    void Shutdown()
    {
        if (sve::Installed())
            sve::Shutdown();
    }
}
