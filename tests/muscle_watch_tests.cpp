// The horse muscle evidence (muscle_watch.h): the DOF table walk of one
// track, the per-track histogram, the A, B, A and set / unset detector of
// the animated normal map weights, and the recorded row detector of
// ReplayClipTool muscle-rows. Also the playback config default
// (PuppetAnimLod0 on, puppet_lod.h).
#include "muscle_watch.h"
#include "puppet_lod.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
    int g_muscleWatchFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_muscleWatchFailures;
    }

    namespace mw = replay::musclewatch;

    void PutDof(std::vector<std::uint8_t>& table, std::uint16_t offset, std::uint16_t id, std::uint8_t track)
    {
        std::uint8_t dof[mw::kDofBytes]{};
        std::memcpy(dof, &offset, 2);
        std::memcpy(dof + 2, &id, 2);
        dof[4] = track;
        table.insert(table.end(), dof, dof + mw::kDofBytes);
    }

    std::uint32_t Bits(float value)
    {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, 4);
        return bits;
    }

    void TestCollectTrack()
    {
        std::vector<std::uint8_t> table;
        PutDof(table, 0x00, 1, 0x00);
        PutDof(table, 0x10, 2, 0x01);
        PutDof(table, 0x40, 3, 0x17);
        PutDof(table, 0x44, 9, 0x17);
        PutDof(table, 0x48, 1, 0x40);
        mw::TrackDofs dofs;
        Check(mw::CollectTrack(table.data(), 5, mw::kAnimatedNormalMapsTrack, dofs), "a sorted table is read");
        Check(dofs.ids.size() == 2 && dofs.ids[0] == 3 && dofs.ids[1] == 9, "the two DOFs of track 0x17");
        Check(dofs.offsets[1] == 0x44 && dofs.bufferBytes == 0x48, "offsets and the bytes they reach");

        std::uint32_t perTrack[256];
        mw::Histogram(table.data(), 5, perTrack);
        Check(perTrack[0x17] == 2 && perTrack[0x40] == 1 && perTrack[0] == 1, "DOFs per track");
        Check(mw::HistogramText(perTrack) == "0:1;1:1;17:2;40:1", "histogram text, hex tracks");

        std::vector<std::uint8_t> unsorted;
        PutDof(unsorted, 0x00, 5, 0x17);
        PutDof(unsorted, 0x04, 1, 0x01);
        Check(!mw::CollectTrack(unsorted.data(), 2, mw::kAnimatedNormalMapsTrack, dofs), "an unsorted table is not the layout");

        std::vector<std::uint8_t> buffer(0x48, 0xFF);
        const std::uint32_t half = Bits(0.5f);
        std::memcpy(buffer.data() + 0x40, &half, 4);
        std::vector<std::uint32_t> bits;
        mw::CollectTrack(table.data(), 5, mw::kAnimatedNormalMapsTrack, dofs);
        Check(mw::ValuesOf(dofs, buffer.data(), buffer.size(), bits), "values read");
        Check(bits.size() == 2 && mw::IsSet(bits[0]) && !mw::IsSet(bits[1]), "0xFFFFFFFF is not set");
        Check(!mw::ValuesOf(dofs, buffer.data(), 0x44, bits), "a value past the buffer is refused");
    }

    void TestWatchSteady()
    {
        mw::Watch watch;
        const std::vector<std::uint16_t> ids = {3, 9};
        // A weight that rises smoothly: changes, no alternation.
        for (int n = 0; n < 10; ++n)
        {
            const mw::Tick tick = mw::Observe(watch, ids, {Bits(0.1f * n), Bits(0.3f)});
            Check(!tick.alternation && tick.setFlips == 0, "a smooth ramp never alternates");
        }
        Check(watch.ticks == 10 && watch.changedTicks == 9 && watch.alternationTicks == 0, "ramp: changes counted");
        Check(std::fabs(watch.maxDelta - 0.1f) < 1e-4f && watch.maxDeltaId == 3, "ramp: largest step and its id");
    }

    void TestWatchAlternation()
    {
        mw::Watch watch;
        const std::vector<std::uint16_t> ids = {3};
        mw::Observe(watch, ids, {Bits(0.8f)});
        mw::Observe(watch, ids, {Bits(0.1f)});
        const mw::Tick back = mw::Observe(watch, ids, {Bits(0.8f)});
        Check(back.alternation && back.alternating == 1, "0.8, 0.1, 0.8 is an alternation");
        // A step and a hold is not.
        mw::Watch hold;
        mw::Observe(hold, ids, {Bits(0.8f)});
        mw::Observe(hold, ids, {Bits(0.1f)});
        Check(!mw::Observe(hold, ids, {Bits(0.1f)}).alternation, "a step then a hold is not");
        // Noise under eps is not.
        mw::Watch noise;
        mw::Observe(noise, ids, {Bits(0.50f)});
        mw::Observe(noise, ids, {Bits(0.51f)});
        Check(!mw::Observe(noise, ids, {Bits(0.50f)}).alternation, "noise under eps is not");
    }

    void TestWatchSetFlips()
    {
        // The expression switched off (not written) and on again.
        mw::Watch watch;
        const std::vector<std::uint16_t> ids = {3, 9};
        mw::Observe(watch, ids, {Bits(0.4f), Bits(0.2f)});
        const mw::Tick off = mw::Observe(watch, ids, {mw::kNotSet, mw::kNotSet});
        Check(off.setFlips == 2 && off.unset == 2 && off.changed, "on to off: two flips");
        const mw::Tick on = mw::Observe(watch, ids, {Bits(0.4f), Bits(0.2f)});
        Check(on.setFlips == 2 && on.alternation && on.alternating == 2, "on, off, on: both alternate");
        Check(watch.setFlips == 4 && watch.unsetValues == 2, "flips and unset values summed");
        // Another layout starts again (no comparison across it).
        const mw::Tick other = mw::Observe(watch, {1}, {Bits(0.9f)});
        Check(!other.changed && !other.alternation, "a new id list is a new start");
        Check(!mw::Observe(watch, {1, 2}, {Bits(0.9f)}).changed, "mismatched sizes are ignored");
    }

    replay::pose::BoneQts Bone(float tx, float angle, float scale)
    {
        replay::pose::BoneQts bone;
        bone.translation[0] = tx;
        bone.rotation.z = std::sin(angle * 0.5f);
        bone.rotation.w = std::cos(angle * 0.5f);
        bone.scale[0] = bone.scale[1] = bone.scale[2] = scale;
        return bone;
    }

    void TestRowWatch()
    {
        Check(std::fabs(mw::RowDistance(Bone(0, 0.0f, 1), Bone(0, 0.2f, 1), 1) - 0.2f) < 1e-4f, "rotation distance is the angle");
        Check(std::fabs(mw::RowDistance(Bone(0, 0, 1.0f), Bone(0, 0, 1.1f), 2) - std::sqrt(0.03f)) < 1e-4f, "scale distance");
        mw::RowWatch watch;
        // Bone 0 moves smoothly; bone 1 scale flickers between bind (1) and 1.2.
        std::uint32_t events = 0;
        for (int n = 0; n < 6; ++n)
        {
            const std::vector<replay::pose::BoneQts> locals = {Bone(0.01f * n, 0.0f, 1.0f), Bone(0, 0, n % 2 ? 1.2f : 1.0f)};
            events += mw::ObserveRows(watch, locals);
        }
        Check(watch.samples == 6, "six samples");
        Check(watch.alternations[0 * 3 + 0] == 0, "a smooth translation never alternates");
        Check(watch.alternations[1 * 3 + 2] == 4 && events == 4, "the scale flicker: every sample from the third");
        Check(watch.changes[1 * 3 + 2] == 5 && watch.changes[0 * 3 + 0] == 5, "changes counted per row");
        // A new bone count starts again.
        Check(mw::ObserveRows(watch, {Bone(0, 0, 1)}) == 0 && watch.alternations.size() == 3, "another skeleton restarts");
    }

    void TestConfigDefault()
    {
        const replay::lod::Config config;
        Check(config.animLod0, "PuppetAnimLod0 is on by default");
    }
}

int RunMuscleWatchTests()
{
    g_muscleWatchFailures = 0;
    TestCollectTrack();
    TestWatchSteady();
    TestWatchAlternation();
    TestWatchSetFlips();
    TestRowWatch();
    TestConfigDefault();
    return g_muscleWatchFailures;
}
