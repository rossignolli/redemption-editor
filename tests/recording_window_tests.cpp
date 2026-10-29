#include "clip_file.h"
#include "decal_state.h"
#include "fire_observer.h"
#include "object_check.h"
#include "recording_window.h"
#include "replay_buffer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// The rope records have no setter outside the recorder; the test fills
// them the way clip_file.cpp reads them.
namespace window_tests_access
{
    template <typename Tag, auto Member>
    struct Steal
    {
        template <typename Object, typename Visit>
        friend void With(Tag, Object& object, Visit&& visit)
        {
            visit(object.*Member);
        }
    };
    struct RopeRecords
    {
        template <typename Object, typename Visit>
        friend void With(RopeRecords, Object& object, Visit&& visit);
    };
    template struct Steal<RopeRecords, &replay::ropes::Ropes::m_records>;
}

namespace
{
    using namespace replay;
    using entity::ReplayId;

    int g_windowFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_windowFailures;
    }

    bool Near(float a, float b, float tolerance = 1e-4f) { return std::fabs(a - b) <= tolerance; }

    // Decal settings life, fade in, growth, growth time, growth end.
    void SetPool(std::uint8_t* settings, float life, float fadeIn, float growth, float growthTime, float growthEnd)
    {
        decal::SetSettingsFloat(settings, decal::kSettingsLife, life);
        decal::SetSettingsFloat(settings, decal::kSettingsFadeIn, fadeIn);
        decal::SetSettingsFloat(settings, decal::kSettingsGrowth, growth);
        decal::SetSettingsFloat(settings, decal::kSettingsGrowthTime, growthTime);
        decal::SetSettingsFloat(settings, decal::kSettingsGrowthEnd, growthEnd);
    }

    // S1, decal_state.h AgeSettings.
    void TestDecalAging()
    {
        std::uint8_t settings[0x150] = {};
        SetPool(settings, 10.0f, 0.5f, 0.2f, 4.0f, 1.0f);
        std::uint8_t same[0x150];
        std::memcpy(same, settings, sizeof(same));
        Check(decal::AgeSettings(same, 0.0f) && std::memcmp(same, settings, sizeof(same)) == 0,
            "decal age 0: settings unchanged (forward play)");
        Check(decal::AgeSettings(settings, 2.0f), "decal aged 2 s: alive");
        Check(Near(decal::SettingsFloat(settings, decal::kSettingsFadeIn), 0.0f) &&
                Near(decal::SettingsFloat(settings, decal::kSettingsLife), 8.0f) &&
                Near(decal::SettingsFloat(settings, decal::kSettingsGrowth), 0.6f) &&
                Near(decal::SettingsFloat(settings, decal::kSettingsGrowthTime), 2.0f) &&
                Near(decal::SettingsFloat(settings, decal::kSettingsGrowthEnd), 1.0f),
            "decal aged 2 s: fade in 0, life 8, growth half way, growth time halved");
        Check(decal::SettingsGrowing(settings, 1.0f) && !decal::SettingsGrowing(settings, 2.0f), "decal growing test");
        std::uint8_t full[0x150] = {};
        SetPool(full, 10.0f, 0.5f, 0.2f, 4.0f, 1.0f);
        Check(decal::AgeSettings(full, 6.0f) && Near(decal::SettingsFloat(full, decal::kSettingsGrowth), 1.0f) &&
                Near(decal::SettingsFloat(full, decal::kSettingsGrowthTime), 0.0f),
            "decal aged past its growth: full size, growth time 0");
        std::uint8_t dead[0x150] = {};
        SetPool(dead, 10.0f, 0.5f, 0.2f, 4.0f, 1.0f);
        Check(!decal::AgeSettings(dead, 10.0f), "decal aged to its life: expired");
        std::uint8_t untimed[0x150] = {};
        SetPool(untimed, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f);
        Check(decal::AgeSettings(untimed, 1000.0f) && Near(decal::SettingsFloat(untimed, decal::kSettingsLife), 0.0f),
            "decal with life 0 never expires by age");
    }

    // decal_state.h AgedGrowth, RegrowNeeded and the bounded retry of a
    // refused regrow.
    void TestDecalRegrow()
    {
        std::uint8_t pool[0x150] = {};
        SetPool(pool, -1.0f, 0.0f, 0.2f, 4.0f, 1.0f);
        Check(Near(decal::AgedGrowth(pool, 0.0f), 0.2f) && Near(decal::AgedGrowth(pool, 2.0f), 0.6f) &&
                Near(decal::AgedGrowth(pool, 9.0f), 1.0f),
            "regrow: aged growth follows AgeSettings");
        Check(decal::RegrowNeeded(pool, 2.0f, 1.0f), "regrow: a growing pool whose size changed regrows");
        Check(!decal::RegrowNeeded(pool, 1.05f, 1.0f), "regrow: a change under the step does not regrow");
        Check(!decal::RegrowNeeded(pool, 6.0f, 5.0f), "regrow: a full pool at both playheads does not regrow");
        Check(decal::RegrowNeeded(pool, 6.0f, 1.0f), "regrow: from growing to full regrows once");
        // A splash: +0x54 > 0 but +0x50 == +0x60 (growth=1->1): its size cannot
        // change, so it never regrows.
        std::uint8_t splash[0x150] = {};
        SetPool(splash, 180.0f, 0.0f, 1.0f, 15.0f, 1.0f);
        Check(!decal::RegrowNeeded(splash, 3.0f, 2.0f), "regrow: a decal with no growth range never regrows");
        // A pool (growth54=0.0096): regrows once from its first add (age 0) to
        // full, then never again.
        std::uint8_t bench[0x150] = {};
        SetPool(bench, -1.0f, 0.0f, 0.1917f, 0.009605f, 1.0f);
        Check(decal::RegrowNeeded(bench, 0.5f, 0.0f) && !decal::RegrowNeeded(bench, 0.9f, 0.5f),
            "regrow: the bench pool regrows once, not on every scrub tick");
        std::uint8_t nan[0x150] = {};
        SetPool(nan, -1.0f, 0.0f, std::nanf(""), 4.0f, 1.0f);
        Check(!decal::RegrowNeeded(nan, 2.0f, 1.0f), "regrow: a non finite growth never regrows");

        decal::RegrowRetry retry;
        Check(decal::RegrowAllowed(retry, 0), "regrow retry: allowed at first");
        Check(!decal::NoteRegrowRefused(retry, 1000) && !decal::RegrowAllowed(retry, 1100) && decal::RegrowAllowed(retry, 1250),
            "regrow retry: a refusal waits kRegrowRetryMs");
        decal::NoteRegrowRefused(retry, 2000);
        Check(decal::NoteRegrowRefused(retry, 3000) && !decal::RegrowAllowed(retry, 100000),
            "regrow retry: gives up after kRegrowMaxRefusals");
        decal::ResetRegrow(retry);
        Check(decal::RegrowAllowed(retry, 0), "regrow retry: a new event resets it");
    }

    // fire_observer.h, the replayed start descriptor and the bounded retry
    // of a start.
    void TestFireStartRetry()
    {
        std::uint8_t desc[fire::kDescBytes] = {};
        const std::uint64_t parent = 0x7FF7C0000000ull;
        const std::uint64_t exclude = 0x274734DC7B0ull;
        std::memcpy(desc + fire::kDescParentFire, &parent, sizeof(parent));
        std::memcpy(desc + fire::kDescTraceExclude, &exclude, sizeof(exclude));
        desc[0x100] = 0x5A;
        Check(fire::ClearDescParent(desc), "fire desc: a parent pointer is reported");
        std::uint64_t after[2] = {1, 1};
        std::memcpy(after, desc + fire::kDescParentFire, sizeof(after));
        Check(after[0] == 0 && after[1] == 0 && desc[0x100] == 0x5A, "fire desc: +0xF0 and +0xF8 cleared, +0x100 kept");
        Check(!fire::ClearDescParent(desc), "fire desc: no parent, nothing reported");

        fire::StartRetry retry;
        Check(fire::StartAllowed(retry, 0), "fire retry: allowed at first");
        Check(!fire::NoteStartFailed(retry, 1000) && !fire::StartAllowed(retry, 1400) && fire::StartAllowed(retry, 1500),
            "fire retry: a failure waits kStartRetryMs");
        fire::NoteStartOk(retry);
        Check(retry.failures == 0 && fire::StartAllowed(retry, 0), "fire retry: a start resets the failures");
        bool gaveUp = false;
        for (std::uint32_t n = 0; n < fire::kStartMaxAttempts; ++n)
            gaveUp = fire::NoteStartFailed(retry, 2000 + n * 1000);
        Check(gaveUp && retry.gaveUp && !fire::StartAllowed(retry, 1000000), "fire retry: gives up after kStartMaxAttempts");
        Check(!fire::NoteStartFailed(retry, 9000) && retry.failures == fire::kStartMaxAttempts,
            "fire retry: no count after giving up");

        std::vector<std::uint32_t> missing;
        using W = fire::FireWant;
        Check(fire::OnlyMissing({{0, true}, {2, false}}, {{0, true}, {1, true}, {2, false}, {3, true}}, missing) &&
                missing == std::vector<std::uint32_t>{1, 3},
            "fire reconcile: only the refused starts are missing");
        Check(fire::OnlyMissing({}, {W{4, true}}, missing) && missing == std::vector<std::uint32_t>{4},
            "fire reconcile: nothing started yet, all missing");
        Check(!fire::OnlyMissing({{0, true}}, {{0, false}}, missing), "fire reconcile: a fire wanted ended needs a rebuild");
        Check(!fire::OnlyMissing({{0, true}, {5, true}}, {{0, true}}, missing), "fire reconcile: a fire not wanted needs a rebuild");
        Check(!fire::OnlyMissing({{0, true}, {0, true}}, {{0, true}}, missing), "fire reconcile: a duplicate needs a rebuild");
        Check(fire::OnlyMissing({{3, true}, {1, false}}, {{1, false}, {3, true}}, missing) && missing.empty(),
            "fire reconcile: order does not matter");
    }

    // Timeline of the synthetic clip: 8 s; the window [4300, 7000] starts
    // in the block at 4000 (base), so every output time is input - 4000.
    constexpr std::uint32_t kDurationMs = 8000;
    constexpr std::uint32_t kFromMs = 4300;
    constexpr std::uint32_t kToMs = 7000;
    constexpr std::uint32_t kBaseMs = 4000;

    const ReplayId kA{0, 1};  // ped alive throughout
    const ReplayId kW{1, 2};  // A's weapon, sampled only before base
    const ReplayId kB{2, 3};  // ped created at 2500
    const ReplayId kC{3, 4};  // ped created at 3000, deleted at 3500
    const ReplayId kD{4, 5};  // ped created at 4200 (inside the window)
    const ReplayId kE{5, 6};  // ped created at 7500 (after the window)

    entity::CreatePacket Create(ReplayId id, std::uint32_t timeMs, entity::EntityKind kind = entity::EntityKind::Ped)
    {
        entity::CreatePacket packet;
        packet.id = id;
        packet.timeMs = timeMs;
        packet.kind = kind;
        packet.modelHash = 0x1000u + id.slot;
        packet.boneCount = 3;
        return packet;
    }

    pose::SkeletonLayout Layout()
    {
        pose::SkeletonLayout layout;
        layout.boneCount = 3;
        layout.parent = {pose::kRootParent, 0, 1};
        layout.ambiguous = {0, 0, 0};
        layout.highPrecision = {0, 0, 0};
        layout.evaluationOrder = {0, 1, 2};
        return layout;
    }

    // A pose track sampled every 33 ms over [firstMs, lastMs] with 2 s
    // chunks; world[12] carries the frame time in seconds.
    pose::PoseTrack MakeTrack(std::uint32_t firstMs, std::uint32_t lastMs)
    {
        const pose::SkeletonLayout layout = Layout();
        pose::StreamConfig config;
        config.keyframeIntervalMs = 2000;
        pose::PoseTrack track;
        track.Begin(layout, config);
        for (std::uint32_t timeMs = firstMs; timeMs <= lastMs; timeMs += 33)
        {
            const float t = static_cast<float>(timeMs) / 1000.0f;
            std::vector<pose::BoneQts> locals(3);
            for (std::uint32_t bone = 0; bone < 3; ++bone)
            {
                const float angle = t * (1.0f + static_cast<float>(bone));
                locals[bone].rotation = pose::Normalize({0.0f, 0.0f, std::sin(angle * 0.5f), std::cos(angle * 0.5f)});
                locals[bone].translation[0] = 0.1f * static_cast<float>(bone) + t * 0.01f;
            }
            std::vector<pose::BoneQts> objects(3);
            pose::ComposeObjects(layout, locals.data(), nullptr, objects.data());
            float world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, t, 2.0f, 3.0f, 1};
            pose::FrameInput input;
            input.timeMs = timeMs;
            input.world = world;
            input.locals = locals.data();
            input.objects = objects.data();
            track.Append(input);
        }
        return track;
    }

    struct Source
    {
        Recording recording;
        std::vector<pose::PoseRecordExport> poses;
        pose::PoseTrack poseA;
    };

    void Build(Source& s)
    {
        Recording& r = s.recording;
        // Lifecycle: the recorder advances every tick.
        for (std::uint32_t t = 0; t <= kDurationMs; t += 100)
        {
            r.lifecycle.Advance(t);
            if (t == 0)
            {
                r.lifecycle.Create(Create(kA, 50));
                r.lifecycle.Create(Create(kW, 60, entity::EntityKind::Weapon));
                r.lifecycle.Attach({kW, kA, entity::AttachmentKind::WeaponPoint, 1}, 60);
            }
            if (t == 2500)
                r.lifecycle.Create(Create(kB, 2500));
            if (t == 3000)
                r.lifecycle.Create(Create(kC, 3000));
            if (t == 3500)
                r.lifecycle.Delete(kC, 3500);
            if (t == 4200)
                r.lifecycle.Create(Create(kD, 4200));
            if (t == 7500)
                r.lifecycle.Create(Create(kE, 7500));
        }
        r.created = {Create(kA, 50), Create(kW, 60, entity::EntityKind::Weapon), Create(kB, 2500), Create(kC, 3000),
            Create(kD, 4200), Create(kE, 7500)};
        // The loose pieces' groups, kept with their prop.
        r.fragPieces.push_back({kB, 0, {14, 15}});
        r.fragPieces.push_back({kC, 1, {3}});

        // Pose segments: A changes skeleton at 1000 and 5000; B at 2000 and 3000.
        const std::uint32_t segA1 = r.segments.NextRecordId();
        const std::uint32_t segA2 = r.segments.NextRecordId();
        const std::uint32_t segB1 = r.segments.NextRecordId();
        const std::uint32_t segB2 = r.segments.NextRecordId();
        r.segments.Add({kA, 1000, segA1, 3, {}});
        r.segments.Add({kB, 2000, segB1, 3, {}});
        r.segments.Add({kB, 3000, segB2, 3, {}});
        r.segments.Add({kA, 5000, segA2, 3, {}});

        // Pose records: A over the whole clip, C (dropped entity), and a
        // record of A that ended before base.
        s.poseA = MakeTrack(0, kDurationMs);
        pose::PoseRecordExport recordA;
        recordA.recordId = kA.Packed();
        recordA.boneCount = 3;
        s.poseA.Export(recordA.track);
        s.poses.push_back(recordA);
        pose::PoseRecordExport recordC;
        recordC.recordId = kC.Packed();
        recordC.boneCount = 3;
        MakeTrack(3000, 3500).Export(recordC.track);
        s.poses.push_back(recordC);
        pose::PoseRecordExport recordSegB1;
        recordSegB1.recordId = segB1;
        recordSegB1.boneCount = 3;
        MakeTrack(2000, 3000).Export(recordSegB1.track);
        s.poses.push_back(recordSegB1);
        pose::PoseRecordExport recordSegA2;
        recordSegA2.recordId = segA2;
        recordSegA2.boneCount = 3;
        MakeTrack(5000, 7600).Export(recordSegA2.track);
        s.poses.push_back(recordSegA2);

        // Weapon W: sampled only up to 3000 (then holstered, not sampled).
        r.weaponTracks.push_back({kW.Packed(), entity::RelativeTrack(30)});
        for (std::uint32_t t = 60; t <= 3000; t += 34)
        {
            entity::TransformSample sample;
            sample.timeMs = t;
            sample.relative.position = {static_cast<float>(t), 0.0f, 0.0f};
            r.weaponTracks.back().track.Append(sample);
        }
        StrapTrack straps;
        straps.recordId = kW.Packed();
        weapon::StrapState strap;
        strap.flags = 1;
        straps.states.push_back({100, strap});
        strap.flags = 2;
        straps.states.push_back({3000, strap});
        strap.flags = 3;
        straps.states.push_back({6000, strap});
        r.strapTracks.push_back(straps);

        // One-shots.
        for (const std::uint32_t t : {1000u, 4000u, 4500u, 7200u})
        {
            entity::ShotEvent shot;
            shot.timeMs = t;
            shot.shooter = kA;
            r.shots.Add(shot);
        }
        // Animation audio events (one-shot).
        for (const std::uint32_t t : {1000u, 4000u, 4600u, 7200u})
        {
            entity::AnimAudioEvent anim;
            anim.timeMs = t;
            anim.entity = kA;
            anim.hash = 0x0F198072u + t;
            r.animAudio.Add(anim);
        }
        entity::SpeechEvent line;
        line.timeMs = 1500;
        line.speaker = kA;
        r.speechLines.Add(line);
        // A line still audible at the base (known natural end past it) is
        // adopted at 0 with the time played.
        line.timeMs = 3000;
        line.endMs = 4600;
        r.speechLines.Add(line);
        line.endMs = 0;
        line.timeMs = 4500;
        line.coreMs = 4400;
        line.stopMs = 4800;
        r.speechLines.Add(line);

        // State: decals (a pair that cancels out, one alive, one later),
        // blood, surfaces, gore of the deleted ped, fires, time scale.
        entity::DecalEvent decal;
        decal.timeMs = 1000;
        decal.engineId = 11;
        r.decals.Add(decal);
        decal.timeMs = 2000;
        decal.engineId = 12;
        // A growing pool, aged by its fold gap (2001 ms).
        SetPool(decal.settings, 10.0f, 0.5f, 0.2f, 4.0f, 1.0f);
        r.decals.Add(decal);
        decal.timeMs = 2500;
        decal.engineId = 14;
        // A 1 s decal folded by 1501 ms: expired, dropped.
        SetPool(decal.settings, 1.0f, 0.5f, 0.0f, 0.0f, 0.0f);
        r.decals.Add(decal);
        decal = {};
        decal.timeMs = 5000;
        decal.engineId = 13;
        // Unattached, on C (dropped by the cut): kept without it.
        decal.entity = kC;
        decal.hasEntity = true;
        r.decals.Add(decal);
        r.decalRemoves.Add({1500, 11});
        r.decalRemoves.Add({6000, 12});
        entity::BloodEvent blood;
        blood.timeMs = 3000;
        blood.entity = kA;
        blood.age = 0.5f;
        r.blood.Add(blood);
        blood.timeMs = 3100;
        blood.entity = kC;
        r.blood.Add(blood);
        entity::SurfaceEvent surface;
        surface.entity = kA;
        surface.timeMs = 1000;
        surface.wet[0] = 0.1f;
        r.surfaces.Add(surface);
        surface.timeMs = 3000;
        surface.wet[0] = 0.3f;
        r.surfaces.Add(surface);
        surface.timeMs = 6000;
        surface.wet[0] = 0.6f;
        r.surfaces.Add(surface);
        r.gore.Add({3200, kC, 2});
        r.gore.Add({5000, kA, 3});

        entity::FireStartEvent fire;
        fire.timeMs = 1000;
        r.fireStarts.Add(fire); // 0: burnt out before base
        fire.timeMs = 3500;
        fire.initialAge = 0.25f;
        r.fireStarts.Add(fire); // 1: alive at base
        fire.timeMs = 7500;
        fire.initialAge = 0.0f;
        r.fireStarts.Add(fire); // 2: after end
        entity::FireStopEvent stop;
        stop.timeMs = 2000;
        stop.start = 0;
        r.fireStops.Add(stop);
        stop.timeMs = 7600;
        stop.start = 2;
        r.fireStops.Add(stop);
        for (const auto& [t, start] : std::vector<std::pair<std::uint32_t, std::uint32_t>>{
                 {1000, 0}, {1250, 0}, {2000, 0}, {3500, 1}, {3750, 1}, {4250, 1}, {7500, 2}})
        {
            entity::FireItemSample item;
            item.timeMs = t;
            item.start = start;
            item.burnt = static_cast<float>(t);
            r.fireItems.Add(item);
        }
        r.timeScale.Note(1000, 0.5f);
        r.timeScale.Note(3000, 1.0f);
        r.timeScale.Note(5000, 0.3f);

        // Continuous: game camera every 100 ms, two registered fx tracks.
        for (std::uint32_t t = 0; t <= kDurationMs; t += 100)
        {
            camera::GameSample sample{};
            sample.timeMs = t;
            sample.position.x = static_cast<float>(t);
            r.gameCamera.push_back(sample);
        }
        entity::RegisteredFxTrack ended;
        ended.startMs = 1000;
        ended.endMs = 2000;
        ended.frames.resize(1);
        ended.frames[0].timeMs = 1000;
        r.registeredFx.push_back(ended);
        entity::RegisteredFxTrack live;
        live.startMs = 3000;
        live.endMs = 7100;
        for (const std::uint32_t t : {3000u, 3900u, 4100u, 6900u, 7100u})
        {
            entity::RegisteredFxFrame frame;
            frame.timeMs = t;
            frame.zoom = static_cast<float>(t);
            live.frames.push_back(frame);
        }
        // Attach changes; C is gone before base, E after end.
        live.attachTimeline = {{3500, kB, 2}, {3900, kC, 5}, {4500, kD, 7}, {7200, kE, 1}};
        r.registeredFx.push_back(live);

        CollisionSound early;
        early.entity = kA;
        early.startMs = 3000;
        early.endMs = 4500;
        r.collisionSounds.push_back(early);
        CollisionSound late;
        late.entity = kA;
        late.startMs = 5000;
        late.endMs = 8000;
        late.intensity = {{5000, 0.1f}, {6000, 0.2f}, {7500, 0.3f}};
        r.collisionSounds.push_back(late);

        // A rope sampled from 3000 to 5000, held until 4500.
        ropes::RopeRecord rope;
        rope.engineId = 7;
        rope.vertices = 2;
        rope.heldEvents = {{3000, true}, {4500, false}};
        for (std::uint32_t t = 3000; t <= 5000; t += 500)
        {
            rope.times.push_back(t);
            rope.windowCounts.push_back(2);
            for (int n = 0; n < 6; ++n)
                rope.positions.push_back(static_cast<float>(t));
        }
        With(window_tests_access::RopeRecords{}, r.ropes,
            [&](std::vector<ropes::RopeRecord>& records) { records.push_back(rope); });

        r.environment.recorded = true;
        r.environment.clockStartS = 3600;
        r.environment.clockEndS = 3608; // 8 s of game clock over 8 s
    }

    // Decodes every chunk of an exported track with PoseDecoder and checks
    // that each record carries its chunk frame time.
    bool DecodeAll(const pose::TrackExport& track, std::uint32_t& first, std::uint32_t& last)
    {
        first = ~0u;
        last = 0;
        pose::PoseDecoder decoder;
        for (const pose::TrackChunkExport& chunk : track.chunks)
        {
            decoder.Begin(track.layouts[chunk.layout], track.config);
            if (chunk.anchor >= 0)
            {
                const std::vector<std::uint8_t>& anchor = track.anchors[static_cast<std::size_t>(chunk.anchor)];
                std::size_t consumed = 0;
                if (!decoder.Apply(anchor.data(), anchor.size(), consumed))
                    return false;
            }
            for (std::size_t frame = 0; frame < chunk.frameTimes.size(); ++frame)
            {
                const std::size_t offset = chunk.frameOffsets[frame];
                std::size_t consumed = 0;
                if (!decoder.Apply(chunk.bytes.data() + offset, chunk.bytes.size() - offset, consumed) ||
                    decoder.TimeMs() != chunk.frameTimes[frame])
                {
                    return false;
                }
                first = decoder.TimeMs() < first ? decoder.TimeMs() : first;
                last = decoder.TimeMs() > last ? decoder.TimeMs() : last;
            }
        }
        return true;
    }

    const pose::PoseRecordExport* FindPose(const std::vector<pose::PoseRecordExport>& poses, std::uint32_t id)
    {
        for (const pose::PoseRecordExport& record : poses)
        {
            if (record.recordId == id)
                return &record;
        }
        return nullptr;
    }

    bool HasCreate(const Recording& r, ReplayId id)
    {
        for (const entity::CreatePacket& packet : r.created)
        {
            if (packet.id == id)
                return true;
        }
        return false;
    }

    void TestBlockChoice(const Source& s)
    {
        Check(window::BaseFor(s.recording, 4300) == 4000, "window: base is the block holding fromMs");
        Check(window::BaseFor(s.recording, 4000) == 4000, "window: a block start is its own base");
        Check(window::BaseFor(s.recording, 3999) == 2000, "window: base of 3999 is 2000");
        Check(window::BaseFor(s.recording, 0) == 0, "window: base of 0 is 0");
    }

    void TestCut(const Source& s, const Recording& out, const std::vector<pose::PoseRecordExport>& poses,
        const window::Result& result)
    {
        Check(result.baseMs == kBaseMs && result.endMs == kToMs && result.durationMs == kToMs - kBaseMs,
            "window: base, end and duration");

        // Lifecycle and entities.
        Check(out.lifecycle.BlockCount() == 2, "window: blocks from base to end");
        std::vector<entity::CreatePacket> alive;
        out.lifecycle.AliveAt(0, alive);
        Check(alive.size() == 3, "window: the first kept block holds A, W and B");
        out.lifecycle.AliveAt(300, alive);
        Check(alive.size() == 4, "window: D is created inside the window");
        std::vector<entity::Attachment> attachments;
        out.lifecycle.AttachmentsAt(0, attachments);
        Check(attachments.size() == 1 && attachments[0].child == kW, "window: attach state at the first block");
        Check(out.created.size() == 4 && HasCreate(out, kA) && HasCreate(out, kW) && HasCreate(out, kB) &&
                HasCreate(out, kD) && !HasCreate(out, kC) && !HasCreate(out, kE),
            "window: created keeps the entities of the kept blocks");
        for (const entity::CreatePacket& packet : out.created)
        {
            if (packet.id == kD)
                Check(packet.timeMs == 200, "window: a create inside the window is shifted");
            else
                Check(packet.timeMs == 0, "window: a create before base is clamped to 0");
        }

        // Segments: A1 kept at 0, A2 at 1000; B1 superseded before base.
        const auto& segments = out.segments.Segments();
        Check(segments.size() == 3, "window: superseded segment dropped");
        Check(out.segments.At(kA, 0) && out.segments.At(kA, 0)->startMs == 0, "window: A's segment at base is at 0");
        Check(out.segments.At(kA, 1000) && out.segments.At(kA, 1000)->startMs == 1000, "window: A's later segment shifted");

        // One-shots.
        const auto& shots = out.shots.Shots();
        Check(shots.size() == 2 && shots[0].timeMs == 0 && shots[1].timeMs == 500, "window: one-shot shots cut and shifted");
        const auto& anims = out.animAudio.Events();
        Check(anims.size() == 2 && anims[0].timeMs == 0 && anims[0].hash == 0x0F198072u + 4000u && anims[1].timeMs == 600,
            "window: animation audio events cut and shifted");
        const auto& lines = out.speechLines.Events();
        Check(lines.size() == 2 && lines[1].timeMs == 500 && lines[1].coreMs == 400 && lines[1].stopMs == 800,
            "window: speech line and its request and stop times shifted");
        Check(lines.size() == 2 && lines[0].timeMs == 0 && lines[0].startedBeforeMs == 1000 && lines[0].endMs == 600 &&
                lines[0].coreMs == 0,
            "window: a line playing at the base adopted with the time it had played");

        // Decals: pair dropped, 12 folded to 1 ms, 13 at 1000.
        const auto& decals = out.decals.Events();
        Check(result.decalPairsDropped == 1, "window: one decal pair dropped");
        Check(decals.size() == 2 && decals[0].engineId == 12 && decals[0].timeMs == 1 && decals[1].engineId == 13 &&
                decals[1].timeMs == 1000,
            "window: decal state folded to 1 ms");
        Check(result.decalsExpiredDropped == 1, "window: a decal past its life by base dropped");
        if (decals.size() == 2)
        {
            Check(Near(decal::SettingsFloat(decals[0].settings, decal::kSettingsLife), 10.0f - 2.001f) &&
                    Near(decal::SettingsFloat(decals[0].settings, decal::kSettingsFadeIn), 0.0f) &&
                    Near(decal::SettingsFloat(decals[0].settings, decal::kSettingsGrowth), 0.2f + 0.8f * (2.001f / 4.0f)),
                "window: a folded pool is aged by its gap");
            Check(!decals[1].entity.Valid() && !decals[1].hasEntity,
                "window: an unattached decal loses an entity that is not kept");
        }
        const auto& removes = out.decalRemoves.Events();
        Check(removes.size() == 1 && removes[0].engineId == 12 && removes[0].timeMs == 2000,
            "window: decal remove of the pair dropped, later one shifted");

        // Blood: A's record folded and aged, C's dropped with C.
        const auto& blood = out.blood.Events();
        Check(blood.size() == 1 && blood[0].entity == kA && blood[0].timeMs == 1 && Near(blood[0].age, 0.5f + 1.001f),
            "window: blood folded to 1 ms and aged by the gap");
        const auto& surfaces = out.surfaces.Events();
        Check(surfaces.size() == 2 && surfaces[0].timeMs == 1 && Near(surfaces[0].wet[0], 0.3f) &&
                surfaces[1].timeMs == 2000,
            "window: surfaces keep the last state before base");
        const auto& gore = out.gore.Events();
        Check(gore.size() == 1 && gore[0].entity == kA && gore[0].timeMs == 1000, "window: gore of a dropped ped goes");

        // Fires: the tail start after end goes, the others fold to 0.
        const auto& starts = out.fireStarts.Events();
        Check(starts.size() == 2 && starts[0].timeMs == 0 && starts[1].timeMs == 0 && starts[1].initial &&
                Near(starts[1].initialAge, 0.25f + 0.5f),
            "window: fire starts folded to 0 with their age");
        const auto& stops = out.fireStops.Events();
        Check(stops.size() == 1 && stops[0].start == 0 && stops[0].timeMs == 0, "window: fire stop folded");
        const auto& items = out.fireItems.Events();
        Check(items.size() == 3 && items[0].start == 0 && Near(items[0].burnt, 2000.0f) && items[0].timeMs == 0 &&
                items[1].start == 1 && Near(items[1].burnt, 3750.0f) && items[1].timeMs == 0 &&
                items[2].timeMs == 250,
            "window: fire items keep the last sample per fire");
        const auto& scale = out.timeScale.Samples();
        Check(scale.size() == 2 && scale[0].timeMs == 0 && Near(scale[0].scale, 1.0f) && scale[1].timeMs == 1000 &&
                Near(scale[1].scale, 0.3f),
            "window: time scale in effect at base");

        // Samples.
        Check(out.gameCamera.size() == 31 && out.gameCamera.front().timeMs == 0 &&
                Near(out.gameCamera.front().position.x, 4000.0f) && out.gameCamera.back().timeMs == 3000,
            "window: game camera samples from base to end");
        Check(out.registeredFx.size() == 1, "window: registered fx ended before base dropped");
        if (out.registeredFx.size() == 1)
        {
            const entity::RegisteredFxTrack& track = out.registeredFx[0];
            Check(track.frames.size() == 3 && track.frames[0].timeMs == 0 && Near(track.frames[0].zoom, 3900.0f) &&
                    track.frames[1].timeMs == 100 && track.frames[2].timeMs == 2900 && track.startMs == 0 &&
                    track.endMs == 3000,
                "window: registered fx keeps the last frame before base");
            Check(track.attachTimeline.size() == 2 && track.attachTimeline[0].timeMs == 0 &&
                    track.attachTimeline[0].id == kB && track.attachTimeline[0].bone == 2 &&
                    track.attachTimeline[1].timeMs == 500 && track.attachTimeline[1].id == kD,
                "window: registered fx attach changes onto kept entities, rebased");
        }
        Check(out.weaponTracks.size() == 1 && out.weaponTracks[0].track.Size() == 1,
            "window: weapon track of a kept weapon keeps its last sample");
        entity::TransformSample weaponSample;
        Check(out.weaponTracks.size() == 1 && out.weaponTracks[0].track.Sample(0, weaponSample) &&
                weaponSample.relative.position.x > 2900.0f,
            "window: weapon sample at 0 is the last one before base");
        Check(out.strapTracks.size() == 1 && out.strapTracks[0].states.size() == 2 &&
                out.strapTracks[0].states[0].first == 0 && out.strapTracks[0].states[0].second.flags == 2 &&
                out.strapTracks[0].states[1].first == 2000,
            "window: strap state in effect at base");
        Check(out.collisionSounds.size() == 1 && out.collisionSounds[0].startMs == 1000 &&
                out.collisionSounds[0].endMs == 3000 && out.collisionSounds[0].intensity.size() == 2,
            "window: collision sound started before base dropped, the later one cut at end");
        const auto& ropeRecords = out.ropes.Records();
        Check(ropeRecords.size() == 1 && ropeRecords[0].times.size() == 3 && ropeRecords[0].times[0] == 0 &&
                ropeRecords[0].times[2] == 1000 && ropeRecords[0].positions.size() == 3 * 6 &&
                Near(ropeRecords[0].positions[0], 4000.0f) && ropeRecords[0].heldEvents.size() == 2 &&
                ropeRecords[0].heldEvents[0].first == 0 && ropeRecords[0].heldEvents[0].second &&
                ropeRecords[0].heldEvents[1].first == 500,
            "window: rope samples and held state");
        Check(out.environment.clockStartS == 3604 && out.environment.clockEndS == 3607,
            "window: game clock at base and at end");

        Check(out.fragPieces.size() == 1 && out.fragPieces[0].id == kB && out.fragPieces[0].source == 0 &&
                out.fragPieces[0].groups.size() == 2 && out.fragPieces[0].groups[1] == 15,
            "window: a kept piece keeps its groups, a dropped one goes (task 48)");

        // Pose records: A kept, C (dropped entity) and B1 (superseded) gone,
        // A2 (starts inside) kept.
        Check(FindPose(poses, kA.Packed()) != nullptr, "window: pose record of A kept");
        Check(FindPose(poses, kC.Packed()) == nullptr, "window: pose record of a dropped entity goes");
        Check(poses.size() == 2, "window: superseded segment record goes");
        const pose::PoseRecordExport* a = FindPose(poses, kA.Packed());
        if (a)
        {
            std::uint32_t first = 0;
            std::uint32_t last = 0;
            Check(DecodeAll(a->track, first, last), "window: cut pose chunks decode with their frame times");
            Check(first == 0 && last <= kToMs - kBaseMs && last > kToMs - kBaseMs - 40,
                "window: pose frames from 0 to end");
            Check(a->track.stats.firstTimeMs == first && a->track.stats.lastTimeMs == last, "window: pose stats span");
            pose::PoseTrack cut;
            std::string error;
            Check(cut.Import(a->track, error), "window: cut pose record imports");
            pose::PoseTrack original = s.poseA;
            pose::PoseSample atCut;
            pose::PoseSample atOriginal;
            for (const std::uint32_t t : {500u, 1500u, 2900u})
            {
                cut.Sample(t, atCut);
                original.Sample(kBaseMs + t, atOriginal);
                Check(Near(atCut.worldA[12], atOriginal.worldA[12]) && Near(atCut.worldB[12], atOriginal.worldB[12]) &&
                        Near(atCut.t, atOriginal.t, 1e-3f),
                    "window: cut pose samples as the original at base + t");
            }
            // At 0: the last frame at or before base (3993 ms).
            cut.Sample(0, atCut);
            Check(Near(atCut.worldA[12], 3.993f), "window: pose at 0 is the last frame before base");
        }
        Check(result.poseFramesClamped > 0 && result.poseFramesTruncated > 0, "window: pose clamp and tail counted");
    }

    void TestRoundTrip(const Recording& out, const std::vector<pose::PoseRecordExport>& poses, const window::Result& result)
    {
        constexpr std::uintptr_t kBase = 0x140000000ull;
        constexpr std::size_t kSize = 0x6000000;
        camera::Track cameraIn;
        editor::CameraState state{};
        cameraIn.AddOrReplace(1000, state);
        cameraIn.AddOrReplace(4500, state);
        cameraIn.AddOrReplace(7500, state);
        camera::Track cameraCut;
        window::CutCamera(cameraIn, result.baseMs, result.endMs, cameraCut);
        Check(cameraCut.Keyframes().size() == 1 && cameraCut.Keyframes()[0].timeMs == 500,
            "window: editor camera keyframes cut and shifted");

        std::vector<std::uint8_t> bytes;
        std::string error;
        Check(clipfile::Encode(out, poses, cameraCut, result.durationMs, kBase, kSize, bytes, error),
            "window: cut recording encodes");
        Recording loaded;
        std::vector<pose::PoseRecordExport> loadedPoses;
        camera::Track loadedCamera;
        std::uint64_t duration = 0;
        const bool decoded =
            clipfile::Decode(bytes.data(), bytes.size(), kBase, kSize, loaded, loadedPoses, loadedCamera, duration, error);
        Check(decoded, "window: cut recording decodes");
        if (!decoded)
        {
            std::fprintf(stderr, "  decode error: %s\n", error.c_str());
            return;
        }
        Check(duration == result.durationMs, "window: duration round trip");
        const std::vector<window::KindCount> a = window::Tally(out, poses);
        const std::vector<window::KindCount> b = window::Tally(loaded, loadedPoses);
        bool same = a.size() == b.size();
        for (std::size_t n = 0; same && n < a.size(); ++n)
            same = a[n].before == b[n].before;
        Check(same, "window: every kind survives the round trip");
        Check(loadedCamera.Keyframes().size() == 1, "window: camera round trip");
        const pose::PoseRecordExport* record = FindPose(loadedPoses, kA.Packed());
        std::uint32_t first = 0;
        std::uint32_t last = 0;
        Check(record && DecodeAll(record->track, first, last) && first == 0, "window: loaded pose decodes from 0");

        // A second cut of the cut (the last 1 s) still works.
        Recording again;
        std::vector<pose::PoseRecordExport> againPoses;
        window::Result second;
        Check(window::Cut(loaded, loadedPoses, duration, 2000, 3000, again, againPoses, second) && second.baseMs == 2000 &&
                second.durationMs == 1000,
            "window: a cut clip cuts again");
    }
}

namespace
{
    // The replay buffer trims a registered fx track's attach changes with
    // its frames, keeping the one in force at the head.
    // The recording gate's data (cuts, spans) and the charred tracks
    // through the window cut and the buffer trim.
    void AddGateData(Source& s)
    {
        Recording& r = s.recording;
        r.scorched.push_back({kA.Packed(), {{1000, true}, {4500, false}, {6000, true}}, 0, false});
        r.scorched.push_back({kC.Packed(), {{3200, true}}, 0, false});
        r.spans.push_back({1000, 3000, gate::SpanKind::ScriptCamera, false});
        r.spans.push_back({3800, 4600, gate::SpanKind::Cutscene, false});
        r.spans.push_back({6500, 7800, gate::SpanKind::ControlsOff, false});
        r.cuts = {2000, 4000, 5000, 7200};
    }

    // The scenery and its children through the cut (base 4000, end 7000):
    // an entry gone before the base drops its children, the kept ones are
    // renumbered, the children's lives and visibility move to the window
    // like their parents'.
    void AddSceneryChildData(Source& s)
    {
        Recording& r = s.recording;
        SceneryObject gone;
        gone.model = 0x1u;
        gone.appearMs = 0;
        gone.goneMs = 3000; // dropped
        SceneryObject chest;
        chest.model = 0x4334F471u; // S_RUSTEDCHEST01X
        chest.appearMs = 1000;     // kept as entry 0
        SceneryObject late;
        late.model = 0x3u;
        late.appearMs = 5000; // kept as entry 1
        r.scenery = {gone, chest, late};
        const auto child = [](std::uint32_t model, std::uint32_t parent, std::uint32_t appear, std::uint32_t goneMs) {
            SceneryChild c;
            c.model = model;
            c.parent = parent;
            c.appearMs = appear;
            c.goneMs = goneMs;
            return c;
        };
        SceneryChild hidden = child(0x13u, 2, 6000, objcheck::kNeverGone);
        hidden.visible = 0;
        hidden.visibleFlipsMs = {6500};
        r.sceneryChildren = {child(0x10u, 0, 0, objcheck::kNeverGone), child(0x11u, 1, 2000, 4500), hidden,
            child(0x14u, 1, 7500, objcheck::kNeverGone), child(0x15u, 1, 0, 3500)};
        // The scenery burn levels, keyed by the entry's index: the dropped
        // entry's goes, the kept ones are renumbered with the level in force at
        // the base folded to 0.
        r.sceneryBurnLevels = {{0, {{1000u, 0.3f}}, 0, false},
            {1, {{1000u, 0.0f}, {3000u, 0.4f}, {5000u, 0.9f}, {7500u, 1.0f}}, 0, false},
            {2, {{5500u, 1.0f}}, 0, false}};
    }

    void TestSceneryChildrenCut(const Recording& out)
    {
        Check(out.scenery.size() == 2 && out.scenery[0].model == 0x4334F471u && out.scenery[1].model == 0x3u,
            "window: the scenery gone before the base is dropped");
        Check(out.sceneryChildren.size() == 2, "window: only the children alive in the window with a kept parent stay");
        if (out.sceneryChildren.size() != 2)
            return;
        const SceneryChild& a = out.sceneryChildren[0];
        const SceneryChild& b = out.sceneryChildren[1];
        Check(a.model == 0x11u && a.parent == 0 && a.appearMs == 0 && a.goneMs == 500,
            "window: a child renumbered to its kept parent, its life moved to the window");
        Check(b.model == 0x13u && b.parent == 1 && b.appearMs == 2000 && b.visible == 0 && b.visibleFlipsMs.size() == 1 &&
                b.visibleFlipsMs[0] == 2500,
            "window: a later child keeps its visibility, moved to the window");
        // Scenery burn levels.
        Check(out.sceneryBurnLevels.size() == 2, "window: the burn level of a dropped scenery entry goes");
        if (out.sceneryBurnLevels.size() != 2)
            return;
        const BurnLevelTrack& chest = out.sceneryBurnLevels[0];
        const BurnLevelTrack& late = out.sceneryBurnLevels[1];
        Check(chest.id == 0 && chest.states.size() == 2 && chest.states[0].first == 0 && chest.states[0].second == 0.4f &&
                chest.states[1].first == 1000 && chest.states[1].second == 0.9f,
            "window: a scenery burn level renumbered, the level at the base folded to 0, the one after the end dropped");
        Check(late.id == 1 && late.states.size() == 1 && late.states[0].first == 1500 && late.states[0].second == 1.0f,
            "window: a later entry's burn level renumbered and moved to the window");
    }

    void TestGateCut(const Recording& out)
    {
        const auto scorchOf = [&](ReplayId id) -> const ScorchTrack* {
            for (const ScorchTrack& track : out.scorched)
                if (track.id == id.Packed())
                    return &track;
            return nullptr;
        };
        const ScorchTrack* a = scorchOf(kA);
        Check(a && a->states.size() == 3 && a->states[0].first == 0 && a->states[0].second &&
                a->states[1].first == 500 && !a->states[1].second && a->states[2].first == 2000 && a->states[2].second,
            "window: charred state in force at base folded to 0, the rest shifted");
        Check(!scorchOf(kC), "window: charred track of a dropped entity goes");
        Check(a && ScorchedAt(*a, 0) && !ScorchedAt(*a, 700) && ScorchedAt(*a, 2500), "window: charred state at a playhead");
        Check(out.spans.size() == 2 && out.spans[0].kind == gate::SpanKind::Cutscene && out.spans[0].startMs == 0 &&
                out.spans[0].endMs == 600 && out.spans[1].kind == gate::SpanKind::ControlsOff && out.spans[1].startMs == 2500 &&
                out.spans[1].endMs == 3000,
            "window: spans cut to the window (one before base dropped)");
        Check(out.cuts.size() == 1 && out.cuts[0] == 1000, "window: cuts after base only, shifted");
    }

    void TestTrimGateData()
    {
        Recording clip;
        clip.scorched.push_back({kA.Packed(), {{100, true}, {200, false}, {400, true}}, 0, false});
        clip.spans.push_back({50, 150, gate::SpanKind::Cutscene, false});
        clip.spans.push_back({150, 350, gate::SpanKind::ScriptCamera, false});
        clip.spans.push_back({300, 300, gate::SpanKind::ControlsOff, true});
        clip.cuts = {120, 250, 500};
        // The tracks keep from the lifecycle block holding the head (here 50 ms
        // blocks: it starts at the head).
        clip.lifecycle.Begin(50);
        for (std::uint32_t t = 0; t <= 500; t += 50)
            clip.lifecycle.Advance(t);
        buffer::TrimBefore(clip, 250);
        Check(clip.scorched[0].states.size() == 2 && clip.scorched[0].states[0].first == 200 &&
                !ScorchedAt(clip.scorched[0], 250) && ScorchedAt(clip.scorched[0], 450),
            "buffer: charred track keeps the state in force at the head");
        Check(clip.spans.size() == 2 && clip.spans[0].kind == gate::SpanKind::ScriptCamera && clip.spans[1].open,
            "buffer: spans ended before the head dropped, open ones kept");
        Check(clip.cuts.size() == 2 && clip.cuts[0] == 250 && clip.cuts[1] == 500, "buffer: cuts before the head dropped");
    }

    // Finding 17 of the release review: the visibility rows of entities the
    // lifecycle no longer keeps go with the ring (the 4096 rows cap is not
    // used up by entities long gone), and the scan finds a row by replay id.
    void TestTrimVisibilityRows()
    {
        Recording clip;
        clip.lifecycle.Begin(50);
        for (std::uint32_t t = 0; t <= 500; t += 50)
        {
            clip.lifecycle.Advance(t);
            if (t == 0)
            {
                clip.lifecycle.Create(Create(kA, 0));
                clip.lifecycle.Create(Create(kB, 0));
            }
            if (t == 100)
                clip.lifecycle.Delete(kA, 100); // gone long before the head
            if (t == 300)
                clip.lifecycle.Create(Create(kC, 300)); // after the head
        }
        for (const ReplayId id : {kA, kB, kC})
        {
            PedVisibility row;
            row.id = id;
            row.visible = 0;
            row.flipsMs = {20, 400};
            clip.pedVisibility.push_back(row);
        }
        const buffer::TrimStats stats = buffer::TrimBefore(clip, 250);
        Check(stats.visibilityRows == 1 && clip.pedVisibility.size() == 2 && clip.pedVisibility[0].id == kB &&
                clip.pedVisibility[1].id == kC,
            "buffer: the visibility row of an entity gone before the head goes, the live ones stay");
        Check(clip.pedVisibility[0].visible == 1 && clip.pedVisibility[0].flipsMs == std::vector<std::uint32_t>{400},
            "buffer: a kept row still folds its flips before the base");
        // Without lifecycle blocks nothing is pruned.
        Recording bare;
        PedVisibility lone;
        lone.id = kD;
        bare.pedVisibility.push_back(lone);
        buffer::TrimBefore(bare, 250);
        Check(bare.pedVisibility.size() == 1, "buffer: no lifecycle, no row pruned");

        // The index: a row by id, the cap, a trim under it.
        std::vector<PedVisibility> rows;
        buffer::VisibilityIndex index;
        bool added = true;
        for (std::uint16_t n = 0; n < buffer::kMaxVisibilityRows; ++n)
        {
            PedVisibility row;
            row.id = ReplayId{static_cast<std::uint16_t>(n + 1), 1};
            added = index.Add(rows, row) && added;
        }
        Check(added, "index: rows up to the cap");
        PedVisibility over;
        over.id = ReplayId{9000, 2};
        Check(!index.Add(rows, over) && rows.size() == buffer::kMaxVisibilityRows && !index.Find(rows, over.id),
            "index: none past the cap");
        const PedVisibility* found = index.Find(rows, ReplayId{4000, 1});
        Check(found && found->id == (ReplayId{4000, 1}), "index: a row by its replay id");
        rows.erase(rows.begin(), rows.begin() + 10); // the ring pruned some
        found = index.Find(rows, ReplayId{4000, 1});
        Check(found && found->id == (ReplayId{4000, 1}) && !index.Find(rows, ReplayId{5, 1}),
            "index: rebuilt after rows went");
        Check(index.Add(rows, over) && index.Find(rows, over.id) == &rows.back(), "index: room again after a prune");
        // Rows replaced in place (same buffer and size, a new recording's):
        // the index follows them, no stale row.
        const ReplayId firstId = rows.front().id;
        rows.front().id = ReplayId{9001, 3};
        Check(!index.Find(rows, firstId) && index.Find(rows, ReplayId{9001, 3}) == &rows.front(),
            "index: rows changed in place are indexed again");
    }

    // The ring drops animation audio events older than one lifecycle block
    // before the head (a saved window can start that early).
    void TestTrimAnimAudio()
    {
        Recording clip;
        for (const std::uint32_t t : {500u, 2900u, 3100u, 6000u})
        {
            entity::AnimAudioEvent anim;
            anim.timeMs = t;
            anim.entity = kA;
            clip.animAudio.Add(anim);
        }
        const buffer::TrimStats stats = buffer::TrimBefore(clip, 5000);
        Check(stats.animAudioEvents == 2 && clip.animAudio.Events().size() == 2 &&
                clip.animAudio.Events()[0].timeMs == 3100,
            "buffer: animation audio events before head - block dropped");
        buffer::TrimBefore(clip, 1000);
        Check(clip.animAudio.Events().size() == 2, "buffer: an earlier head drops nothing");
    }

    void TestTrimAttachTimeline()
    {
        Recording clip;
        entity::RegisteredFxTrack track;
        track.startMs = 100;
        track.endMs = 300;
        for (const std::uint32_t t : {100u, 200u, 300u})
        {
            entity::RegisteredFxFrame frame;
            frame.timeMs = t;
            track.frames.push_back(frame);
        }
        track.attachTimeline = {{50, kA, 1}, {150, kB, 2}, {250, kD, 3}};
        clip.registeredFx.push_back(track);
        entity::RegisteredFxTrack ended = track;
        ended.endMs = 120;
        clip.registeredFx.push_back(ended);
        clip.lifecycle.Begin(50); // the block holding the head starts at it
        for (std::uint32_t t = 0; t <= 300; t += 50)
        {
            clip.lifecycle.Advance(t);
            if (t == 0)
                for (const ReplayId id : {kA, kB, kD})
                    clip.lifecycle.Create(Create(id, 0));
        }
        buffer::TrimBefore(clip, 200);
        const entity::RegisteredFxTrack& trimmed = clip.registeredFx[0];
        Check(trimmed.frames.size() == 2 && trimmed.frames[0].timeMs == 200, "buffer: fx frames before the head dropped");
        Check(trimmed.attachTimeline.size() == 2 && trimmed.attachTimeline[0].id == kB &&
                trimmed.attachTimeline[1].id == kD,
            "buffer: fx attach changes keep the one in force at the head");
        Check(clip.registeredFx[1].frames.empty() && clip.registeredFx[1].attachTimeline.empty(),
            "buffer: an ended fx track loses its attach changes with its frames");
    }

    // The trim empties the fx tracks that ended before its base; the
    // recorder then removes them (the track count no longer grows the whole
    // session), keeps one a live key still holds, and remaps the rest.
    void TestTrimDropsEmptiedFxTracks()
    {
        Recording clip;
        const auto addTrack = [&](std::uint32_t startMs, std::uint32_t endMs, std::uint32_t hash) {
            entity::RegisteredFxTrack track;
            track.effectHash = hash;
            track.startMs = startMs;
            track.endMs = endMs;
            for (std::uint32_t t = startMs; t <= endMs; t += 50)
            {
                entity::RegisteredFxFrame frame;
                frame.timeMs = t;
                track.frames.push_back(frame);
            }
            clip.registeredFx.push_back(track);
        };
        addTrack(0, 100, 1);    // ended long before the head
        addTrack(0, 4000, 2);   // runs across the head
        addTrack(0, 150, 3);    // ended, its key still live (held)
        addTrack(50, 120, 4);   // ended
        addTrack(3000, 5000, 5); // after the head
        clip.lifecycle.Begin(0);
        for (std::uint32_t t = 0; t <= 5000; t += 50)
            clip.lifecycle.Advance(t);
        const std::uint32_t head = 3000;
        const buffer::TrimStats stats = buffer::TrimBefore(clip, head);
        const std::uint32_t base = buffer::ExportableBaseMs(clip, head);
        Check(stats.fxTracksEmptied == 3 && clip.registeredFx.size() == 5, "buffer: the trim empties the ended tracks, removes none");
        std::vector<std::uint8_t> held(clip.registeredFx.size(), 0);
        held[2] = 1;
        std::vector<std::size_t> remap;
        const std::size_t dropped = entity::DropEmptiedRegisteredFx(clip.registeredFx, base, held, remap);
        Check(dropped == 2 && clip.registeredFx.size() == 3, "buffer: the emptied tracks no key holds are removed");
        Check(clip.registeredFx[0].effectHash == 2 && clip.registeredFx[1].effectHash == 3 && clip.registeredFx[2].effectHash == 5,
            "buffer: the kept tracks keep their order");
        Check(remap.size() == 5 && remap[0] == entity::kDroppedRegisteredFxTrack && remap[1] == 0 && remap[2] == 1 &&
                remap[3] == entity::kDroppedRegisteredFxTrack && remap[4] == 2,
            "buffer: the remap follows every kept track, the removed ones are marked");
        Check(!clip.registeredFx[0].frames.empty() && !clip.registeredFx[2].frames.empty(),
            "buffer: no recorded frame after the base is lost");
        // Nothing to remove: no change, an identity remap.
        Check(entity::DropEmptiedRegisteredFx(clip.registeredFx, base, {}, remap) == 1 && clip.registeredFx.size() == 2,
            "buffer: the held track goes once its key is no longer live");
        Check(entity::DropEmptiedRegisteredFx(clip.registeredFx, base, {}, remap) == 0 && remap[0] == 0 && remap[1] == 1,
            "buffer: tracks with frames are never removed");
        // A track not ended before the base but still empty (its key has not
        // sampled yet) stays.
        entity::RegisteredFxTrack fresh;
        fresh.startMs = fresh.endMs = 5000;
        clip.registeredFx.push_back(fresh);
        Check(entity::DropEmptiedRegisteredFx(clip.registeredFx, base, {}, remap) == 0 && clip.registeredFx.size() == 3,
            "buffer: a new empty track is not removed");
    }

    // The ropes count in the recording's bytes: they have no record cap any
    // more, the ring's budget and the manual part's size bound them.
    void TestApproxBytesCountsRopes()
    {
        Recording clip;
        const std::size_t before = buffer::ApproxBytes(clip);
        ropes::RopeRecord record;
        record.vertices = 16;
        record.times.assign(300, 0);
        record.positions.assign(300 * 16 * 3, 0.0f);
        record.windowCounts.assign(300, 16);
        With(window_tests_access::RopeRecords{}, clip.ropes,
            [&](std::vector<ropes::RopeRecord>& records) { records.push_back(record); });
        Check(buffer::ApproxBytes(clip) >= before + 300 * 16 * 3 * sizeof(float), "buffer: the rope samples count in the budget");
    }
}

namespace
{
    // Appearance changes (appearance_track.h): the appearance at a playhead
    // against the pose segments, the fingerprint, the window cut and the
    // buffer trim.
    entity::Appearance Look(std::uint32_t drawable, std::size_t layers = 1)
    {
        entity::Appearance appearance;
        for (std::size_t n = 0; n < layers; ++n)
        {
            entity::MetaPedAsset asset;
            asset.drawable = drawable + static_cast<std::uint32_t>(n);
            asset.albedo = 0xA0u + static_cast<std::uint32_t>(n);
            appearance.assets.push_back(asset);
        }
        return appearance;
    }

    void TestAppearanceAt()
    {
        const entity::Appearance create = Look(0x10);
        AppearanceTrack track;
        track.id = kA.Packed();
        track.changes.push_back({100, Look(0x20)});
        track.changes.push_back({300, Look(0x30)});
        entity::PoseSegment segment{kA, 200, 0x80000001u, 3, Look(0x40)};
        WantedAppearance at = AppearanceAt(nullptr, nullptr, create, 500);
        Check(at.appearance == &create && at.key == 0, "appearance: no track, the create packet");
        at = AppearanceAt(&track, nullptr, create, 50);
        Check(at.appearance == &create && at.key == 0, "appearance: before the first change, the create packet");
        at = AppearanceAt(&track, nullptr, create, 100);
        Check(at.appearance == &track.changes[0].appearance && at.key == 1, "appearance: a change at its own time");
        at = AppearanceAt(&track, &segment, create, 250);
        Check(at.appearance == &segment.appearance && at.key == 0, "appearance: a later segment overrides an older change");
        at = AppearanceAt(&track, &segment, create, 300);
        Check(at.appearance == &track.changes[1].appearance && at.key == 2, "appearance: a change after the segment wins");
        entity::PoseSegment same{kA, 300, 0x80000002u, 3, Look(0x50)};
        at = AppearanceAt(&track, &same, create, 300);
        Check(at.key == 2, "appearance: a change at the segment's start wins");

        entity::Appearance swapped = Look(0x60, 3);
        std::swap(swapped.assets[0], swapped.assets[2]);
        entity::Appearance tinted = Look(0x60, 3);
        tinted.assets[1].tint0 = 7;
        Check(AppearancePrint(Look(0x60, 3)) == AppearancePrint(swapped), "appearance: print ignores the layer order");
        Check(AppearancePrint(Look(0x60, 3)) == AppearancePrint(tinted), "appearance: print ignores tints");
        Check(AppearancePrint(Look(0x60, 3)) != AppearancePrint(Look(0x61, 3)), "appearance: print sees another drawable");
        Check(AppearancePrint(Look(0x60, 3)) != AppearancePrint(Look(0x60, 2)), "appearance: print sees a layer less");
        Check(AppearancePrint(entity::Appearance{}) != AppearancePrint(Look(0x60, 1)), "appearance: empty print differs");
    }

    // A: segments at 1000 and 5000; B: at 2000 and 3000; C is dropped. Base
    // 4000, end 7000.
    void AddAppearanceData(Source& s)
    {
        Recording& r = s.recording;
        r.appearances.push_back({kA.Packed(), {{500, Look(0x1)}, {3500, Look(0x2)}, {6000, Look(0x3)}, {7500, Look(0x4)}}});
        r.appearances.push_back({kB.Packed(), {{2500, Look(0x5)}, {4500, Look(0x6)}}});
        r.appearances.push_back({kC.Packed(), {{3200, Look(0x7)}}});
    }

    void TestAppearanceCut(const Recording& out)
    {
        const AppearanceTrack* a = FindAppearanceTrack(out.appearances, kA.Packed());
        const AppearanceTrack* b = FindAppearanceTrack(out.appearances, kB.Packed());
        Check(a && a->changes.size() == 2 && a->changes[0].timeMs == 0 && a->changes[0].appearance.assets[0].drawable == 0x2 &&
                a->changes[1].timeMs == 2000 && a->changes[1].appearance.assets[0].drawable == 0x3,
            "window: appearance change in force at base folded to 0, the rest shifted, after end dropped");
        Check(b && b->changes.size() == 1 && b->changes[0].timeMs == 500 && b->changes[0].appearance.assets[0].drawable == 0x6,
            "window: an appearance change older than the segment at base goes");
        Check(!FindAppearanceTrack(out.appearances, kC.Packed()), "window: appearance track of a dropped entity goes");
    }

    void TestTrimAppearances()
    {
        Recording clip;
        clip.appearances.push_back({kA.Packed(), {{100, Look(0x1)}, {1000, Look(0x2)}, {3000, Look(0x3)}, {5000, Look(0x4)}}});
        const buffer::TrimStats stats = buffer::TrimBefore(clip, 4500);
        const std::vector<AppearanceChange>& changes = clip.appearances[0].changes;
        Check(stats.appearanceChanges == 1 && changes.size() == 3 && changes[0].timeMs == 1000 && changes[2].timeMs == 5000,
            "buffer: appearance changes keep the one in force a block before the head");
    }
}

namespace
{
    // The ring's trim at a head, then a cut from the head, gives the same
    // clip as the cut of the untrimmed recording, with every kind of track
    // changing between the block start (the cut's base, 4000) and the head
    // (4500). Compared as the encoded clip: every section at once.
    void TestTrimThenCut(const Source& source)
    {
        Recording full = source.recording;
        const auto sample = [](std::uint32_t t, float x) {
            camera::GameSample s{};
            s.timeMs = t;
            s.position = {x, 0.0f, 0.0f};
            s.rotation = {0.0f, 0.0f, x};
            s.fov = 50.0f;
            return s;
        };
        full.gameCamera = {sample(1000, 1.0f), sample(4200, 2.0f), sample(5000, 3.0f)};
        DoorTrack door;
        door.hash = 0xD00Du;
        door.samples.resize(2);
        door.samples[0].timeMs = 1000;
        door.samples[1].timeMs = 4300;
        door.samples[1].rotation[2] = 90.0f;
        full.doors.push_back(door);
        entity::RegisteredFxTrack fx;
        fx.effectHash = 0xF0F0u;
        fx.startMs = 3500;
        fx.endMs = 4200; // ended between the base and the head
        for (const std::uint32_t t : {3500u, 3900u, 4100u, 4200u})
        {
            entity::RegisteredFxFrame frame;
            frame.timeMs = t;
            fx.frames.push_back(frame);
        }
        full.registeredFx.push_back(fx);
        full.world.recorded = true;
        full.world.ipls = {{1000, 0x1111u, true}, {4100, 0x1111u, false}, {4300, 0x2222u, true}};
        full.abilities.push_back({3000, 4200, AbilityKind::DeadEye, false});
        full.cuts = {1000, 4200, 5000};
        full.spans.push_back({3900, 4200, gate::SpanKind::ScriptCamera, false});
        full.vehicleWindows.push_back({kA.Packed(), {{1000u, std::uint8_t{0}}, {4300u, std::uint8_t{1}}}, 0, false});
        full.vehicleParts.push_back({kA.Packed(), {{1000u, std::uint16_t{0}}, {4300u, std::uint16_t{2}}}, 0, false});
        full.vehicleBurnt.push_back({kA.Packed(), {{1000, false}, {4300, true}}, 0, false});
        full.burnLevels.push_back({kA.Packed(), {{1000, 0.1f}, {4300, 0.5f}}, 0, false});
        full.scorched.push_back({kB.Packed(), {{2600, false}, {4300, true}}, 0, false});
        const std::size_t bytesBefore = buffer::ApproxBytes(full);

        Recording trimmed = full;
        buffer::TrimBefore(trimmed, 4500);
        Check(buffer::ExportableBaseMs(trimmed, 4500) == 4000, "trim then cut: the base is the block holding the head");
        Check(buffer::ApproxBytes(trimmed) < bytesBefore, "trim then cut: the trimmed recording counts fewer bytes");

        Recording cutFull, cutTrimmed;
        std::vector<pose::PoseRecordExport> posesFull, posesTrimmed;
        window::Result resultFull, resultTrimmed;
        const bool ok = window::Cut(full, source.poses, kDurationMs, 4500, kToMs, cutFull, posesFull, resultFull) &&
            window::Cut(trimmed, source.poses, kDurationMs, 4500, kToMs, cutTrimmed, posesTrimmed, resultTrimmed);
        Check(ok && resultFull.baseMs == 4000 && resultTrimmed.baseMs == 4000, "trim then cut: both cuts start at 4000");
        if (!ok)
            return;
        std::vector<std::uint8_t> a, b;
        std::string error;
        camera::Track none;
        clipfile::Encode(cutFull, posesFull, none, resultFull.durationMs, 0, 0, a, error);
        clipfile::Encode(cutTrimmed, posesTrimmed, none, resultTrimmed.durationMs, 0, 0, b, error);
        Check(!a.empty() && a == b, "trim then cut: the clip equals the cut of the untrimmed recording (every track)");
        Check(cutTrimmed.gameCamera.size() >= 1 && cutTrimmed.gameCamera[0].timeMs == 0 &&
                cutTrimmed.gameCamera[0].position.x == 1.0f,
            "trim then cut: the game camera in force at the base is frame 0");
        Check(std::any_of(cutTrimmed.registeredFx.begin(), cutTrimmed.registeredFx.end(),
                  [](const entity::RegisteredFxTrack& track) { return track.effectHash == 0xF0F0u; }),
            "trim then cut: an effect that ended between base and head stays");
    }
}

namespace
{
    // A window saved while the recording runs (the buffer's Shift+F4, a manual part
    // inside the ring) has the environment of a stopped recording's window: the
    // recorded flag (the editor's time and weather rows, playback's environment)
    // and the clock at base and end from the track (QA 232).
    void TestLiveWindowEnvironment(const Source& source)
    {
        Recording stopped = source.recording; // recorded, clock 3600 to 3608 over 8 s
        for (std::uint32_t t = 0; t <= kDurationMs; t += 2000)
        {
            EnvironmentSample sample;
            sample.timeMs = t;
            sample.clockS = 3600 + t / 1000;
            sample.weatherFrom = 0x1111u;
            sample.weatherTo = 0x2222u;
            sample.weatherMix = 0.25f;
            stopped.environment.track.push_back(sample);
        }
        Recording live = stopped;
        live.environment.recorded = false; // set at the stop only
        live.environment.clockEndS = 0;
        live.environment.clockStartS = 3000; // the buffer's start, long trimmed

        Recording cutStopped, cutLive;
        std::vector<pose::PoseRecordExport> posesStopped, posesLive;
        window::Result resultStopped, resultLive;
        const bool ok = window::Cut(stopped, source.poses, kDurationMs, kFromMs, kToMs, cutStopped, posesStopped, resultStopped) &&
            window::Cut(live, source.poses, kToMs, kFromMs, kToMs, cutLive, posesLive, resultLive);
        Check(ok, "live window environment: both cuts succeed");
        if (!ok)
            return;
        const Environment& a = cutStopped.environment;
        const Environment& b = cutLive.environment;
        Check(b.recorded, "live window environment: the window says the environment was recorded");
        Check(b.clockStartS == 3604 && b.clockEndS == 3607, "live window environment: the clock at base and end from the track");
        Check(a.recorded == b.recorded && a.clockStartS == b.clockStartS && a.clockEndS == b.clockEndS &&
                a.weatherFrom == b.weatherFrom && a.weatherTo == b.weatherTo && a.weatherMix == b.weatherMix &&
                a.track.size() == b.track.size(),
            "live window environment: the same fields as the stopped recording's window");

        // A running recording without an environment track (none recorded) stays
        // without it.
        Recording bare = source.recording;
        bare.environment = Environment{};
        Recording cutBare;
        std::vector<pose::PoseRecordExport> posesBare;
        window::Result resultBare;
        Check(window::Cut(bare, source.poses, kToMs, kFromMs, kToMs, cutBare, posesBare, resultBare) &&
                !cutBare.environment.recorded && cutBare.environment.clockStartS == 0 && cutBare.environment.clockEndS == 0,
            "live window environment: no track, no environment");

        // The window saved, then loaded: the flag and the clock stay.
        std::vector<std::uint8_t> bytes;
        std::string error;
        camera::Track none;
        Recording loaded;
        std::vector<pose::PoseRecordExport> loadedPoses;
        camera::Track loadedCamera;
        std::uint64_t loadedMs = 0;
        Check(clipfile::Encode(cutLive, posesLive, none, resultLive.durationMs, 0x140000000ull, 0x6000000, bytes, error) &&
                clipfile::Decode(bytes.data(), bytes.size(), 0x140000000ull, 0x6000000, loaded, loadedPoses, loadedCamera, loadedMs, error) &&
                loaded.environment.recorded && !loaded.environment.repairedOnLoad && loaded.environment.clockStartS == 3604 &&
                loaded.environment.clockEndS == 3607,
            "live window environment: saved recorded, loaded without a repair");
    }
}

namespace
{
    // A saved buffer window takes the place in force at its start.
    void TestWindowPlace()
    {
        const auto place = [](std::uint32_t town, const char* name) {
            Place p;
            p.recorded = true;
            p.zoneTown = town;
            std::snprintf(p.town, sizeof(p.town), "%s", name);
            std::snprintf(p.character, sizeof(p.character), "Arthur");
            return p;
        };
        std::vector<window::PlaceSample> track;
        Check(window::PlaceAt(track, 100) == nullptr, "place: empty track gives none");
        Check(window::AddPlaceSample(track, 0, place(1, "Valentine")), "place: first sample added");
        Check(!window::AddPlaceSample(track, 2000, place(1, "Valentine")), "place: same zones not added");
        Check(window::AddPlaceSample(track, 4000, place(2, "Saint Denis")), "place: a new zone is added");
        Check(window::AddPlaceSample(track, 8000, place(3, "Rhodes")), "place: a third zone is added");
        Check(std::strcmp(window::PlaceAt(track, 3999)->town, "Valentine") == 0 &&
                std::strcmp(window::PlaceAt(track, 4000)->town, "Saint Denis") == 0 &&
                std::strcmp(window::PlaceAt(track, 9000)->town, "Rhodes") == 0,
            "place: the sample in force at a time");
        window::DropPlacesBefore(track, 5000);
        Check(track.size() == 2 && track[0].timeMs == 4000, "place: the drop keeps the sample in force at the head");
        Check(std::strcmp(window::PlaceAt(track, 100)->town, "Saint Denis") == 0, "place: before the first, the first");
        Place out = place(1, "Valentine");
        std::snprintf(out.weather, sizeof(out.weather), "Rain");
        window::ApplyWindowPlace(out, track, 6000);
        Check(out.zoneTown == 2 && std::strcmp(out.town, "Saint Denis") == 0 && std::strcmp(out.weather, "Rain") == 0 &&
                std::strcmp(out.character, "Arthur") == 0,
            "place: the window takes the zones, keeps its weather and character");
    }
}

int RunRecordingWindowTests()
{
    TestWindowPlace();
    TestAppearanceAt();
    TestTrimAppearances();
    TestTrimAttachTimeline();
    TestTrimDropsEmptiedFxTracks();
    TestApproxBytesCountsRopes();
    TestTrimGateData();
    TestTrimVisibilityRows();
    TestTrimAnimAudio();
    TestDecalAging();
    TestDecalRegrow();
    TestFireStartRetry();
    Source source;
    Build(source);
    AddGateData(source);
    AddAppearanceData(source);
    AddSceneryChildData(source);
    TestBlockChoice(source);

    Recording out;
    std::vector<pose::PoseRecordExport> poses;
    window::Result result;
    const bool ok = window::Cut(source.recording, source.poses, kDurationMs, kFromMs, kToMs, out, poses, result);
    Check(ok, "window: cut succeeds");
    if (!ok)
        std::fprintf(stderr, "  cut error: %s\n", result.error.c_str());
    else
    {
        TestCut(source, out, poses, result);
        TestGateCut(out);
        TestAppearanceCut(out);
        TestSceneryChildrenCut(out);
        TestRoundTrip(out, poses, result);
    }
    TestTrimThenCut(source);
    TestLiveWindowEnvironment(source);

    window::Result empty;
    Recording unused;
    std::vector<pose::PoseRecordExport> unusedPoses;
    Check(!window::Cut(source.recording, source.poses, kDurationMs, 5000, 5000, unused, unusedPoses, empty) &&
            !empty.error.empty(),
        "window: an empty window is refused");

    if (g_windowFailures == 0)
        std::puts("Recording window tests passed.");
    return g_windowFailures;
}
