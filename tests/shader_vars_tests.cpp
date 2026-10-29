// The horses' muscles (shader_vars_core.h): the component frame's layout,
// the half float codec, the recorded track (note, lookup with interpolation,
// layouts, ring, cut, validation), the mapping onto a puppet's layout and
// the evidence helpers; the config default.
#include "shader_vars_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
    int g_shaderVarsFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_shaderVarsFailures;
    }

    namespace sv = replay::shadervars;

    bool Near(float a, float b, float tol = 1e-3f) { return std::fabs(a - b) <= tol; }

    void PutDof(std::vector<std::uint8_t>& table, std::uint16_t offset, std::uint16_t id, std::uint8_t track, std::uint8_t type)
    {
        std::uint8_t dof[sv::kDofBytes]{};
        std::memcpy(dof, &offset, 2);
        std::memcpy(dof + 2, &id, 2);
        dof[4] = track;
        dof[5] = type;
        table.insert(table.end(), dof, dof + sv::kDofBytes);
    }

    // A horse's component: two slide floats, a muscle float and a muscle vec4.
    std::vector<std::uint8_t> HorseTable()
    {
        std::vector<std::uint8_t> table;
        PutDof(table, 0x00, 5, sv::kTrackShaderSlideU, 2);
        PutDof(table, 0x04, 5, sv::kTrackShaderSlideV, 2);
        PutDof(table, 0x08, 1, sv::kTrackAnimatedNormalMaps, 2);
        PutDof(table, 0x10, 2, sv::kTrackAnimatedNormalMaps, 0xF4); // type 4 (the high bits are flags)
        return table;
    }

    void TestParseLayout()
    {
        const std::vector<std::uint8_t> table = HorseTable();
        sv::Layout layout;
        Check(sv::ParseLayout(table.data(), 4, 0x20, layout) == sv::ParseResult::Ok, "a horse's table parses");
        Check(layout.dofs.size() == 4 && layout.lanes == 1 + 1 + 1 + 4, "four DOFs, seven lanes");
        Check(layout.CountTrack(sv::kTrackAnimatedNormalMaps) == 2, "two muscle DOFs (track 0x17)");
        Check(layout.dofs[3].type == 4 && layout.offsets[3] == 0x10, "the type's low nibble, the offset");
        Check(sv::ParseLayout(table.data(), 4, 0x1C, layout) == sv::ParseResult::Range, "a vec4 past the buffer is refused");
        Check(sv::ParseLayout(nullptr, 0, 0x20, layout) == sv::ParseResult::Empty, "no table");

        std::vector<std::uint8_t> unsorted;
        PutDof(unsorted, 0x00, 2, sv::kTrackAnimatedNormalMaps, 2);
        PutDof(unsorted, 0x04, 1, sv::kTrackAnimatedNormalMaps, 2);
        Check(sv::ParseLayout(unsorted.data(), 2, 0x10, layout) == sv::ParseResult::Order, "keys must ascend");

        std::vector<std::uint8_t> overlap;
        PutDof(overlap, 0x00, 1, sv::kTrackAnimatedNormalMaps, 4);
        PutDof(overlap, 0x08, 2, sv::kTrackAnimatedNormalMaps, 2);
        Check(sv::ParseLayout(overlap.data(), 2, 0x20, layout) == sv::ParseResult::Overlap, "values sharing bytes are refused");

        std::vector<std::uint8_t> unknown;
        PutDof(unknown, 0x00, 1, sv::kTrackAnimatedNormalMaps, 2);
        PutDof(unknown, 0x10, 2, sv::kTrackAnimatedNormalMaps, 7);
        Check(sv::ParseLayout(unknown.data(), 2, 0x20, layout) == sv::ParseResult::Ok && layout.dofs.size() == 1 &&
                  layout.skipped == 1,
            "an unknown type is left out, counted");

        // Gather: the lanes in table order.
        sv::ParseLayout(table.data(), 4, 0x20, layout);
        float buffer[8] = {0.1f, 0.2f, 0.75f, 0.0f, 1.0f, 0.5f, -0.25f, 0.0f};
        std::uint32_t notSet = sv::kNotSet;
        std::memcpy(&buffer[3], &notSet, 4); // the gap is not a DOF
        std::vector<std::uint32_t> bits;
        Check(sv::Gather(layout, reinterpret_cast<const std::uint8_t*>(buffer), sizeof(buffer), bits) && bits.size() == 7,
            "gather reads seven lanes");
        Check(Near(sv::FloatOf(bits[2]), 0.75f) && Near(sv::FloatOf(bits[5]), -0.25f), "the muscle lanes");
        Check(!sv::Gather(layout, reinterpret_cast<const std::uint8_t*>(buffer), 0x10, bits), "a short buffer is refused");
    }

    void TestHalf()
    {
        const float values[] = {0.0f, 1.0f, -1.0f, 0.5f, 0.333f, 2.5f, 1000.0f, -0.0001f, 6.1e-5f};
        for (const float v : values)
        {
            const float back = sv::FloatOf(sv::FromHalf(sv::ToHalf(sv::BitsOf(v))));
            Check(std::fabs(back - v) <= std::fabs(v) * 1e-3f + 1e-7f, "a half keeps 11 bits");
        }
        Check(sv::ToHalf(sv::kNotSet) == sv::kHalfNotSet && sv::FromHalf(sv::kHalfNotSet) == sv::kNotSet, "not set stays not set");
        Check(sv::ToHalf(sv::BitsOf(std::nanf(""))) == sv::kHalfNotSet, "a NaN is not set");
        Check(sv::FloatOf(sv::FromHalf(sv::ToHalf(sv::BitsOf(1e9f)))) == 65504.0f, "too large: the largest half");
        Check(sv::FromHalf(sv::ToHalf(sv::BitsOf(1e-9f))) == 0u, "too small: 0");
        // The shader's precision: a byte over [-1, 1] (0xB2F0D0) never moves.
        for (int b = 0; b <= 255; ++b)
        {
            const float v = static_cast<float>(b) / 127.5f - 1.0f;
            const float back = sv::FloatOf(sv::FromHalf(sv::ToHalf(sv::BitsOf(v))));
            if (static_cast<int>((back + 1.0f) * 127.5f + 0.5f) != b)
            {
                Check(false, "a byte of the shader survives the half");
                break;
            }
        }
    }

    std::vector<sv::Dof> Dofs(std::uint16_t secondId = 2)
    {
        return {sv::Dof{sv::kTrackAnimatedNormalMaps, 2, 1}, sv::Dof{sv::kTrackAnimatedNormalMaps, 4, secondId}};
    }

    std::vector<std::uint32_t> Lanes(float a, float b)
    {
        return {sv::BitsOf(a), sv::BitsOf(b), sv::BitsOf(b), sv::BitsOf(b), sv::BitsOf(b)};
    }

    void TestTrack()
    {
        constexpr std::uint32_t horse = 0x00050001u;
        sv::Track track;
        Check(track.Due(horse, 0), "a first point is due");
        Check(track.Note(horse, 0, Dofs(), Lanes(0.0f, 1.0f)), "note");
        Check(!track.Due(horse, 20) && track.Due(horse, 33), "the rate");
        Check(track.Note(horse, 100, Dofs(), Lanes(1.0f, 0.0f)), "a second point");
        Check(!track.Note(horse, 120, Dofs(), {sv::BitsOf(1.0f)}), "lanes must match the layout");
        Check(!track.Note(horse, 120, {sv::Dof{0x17, 9, 1}}, {sv::BitsOf(1.0f)}), "an unknown type is refused");
        Check(track.Points() == 2 && track.series.size() == 1 && track.series[0].blocks.size() == 1, "one block of two");

        const std::vector<sv::Dof>* dofs = nullptr;
        std::vector<std::uint32_t> bits;
        Check(track.At(horse, 50, dofs, bits) && dofs && dofs->size() == 2 && bits.size() == 5, "a value between points");
        Check(Near(sv::FloatOf(bits[0]), 0.5f, 2e-3f) && Near(sv::FloatOf(bits[4]), 0.5f, 2e-3f), "linear between them");
        Check(track.At(horse, 100 + sv::kHoldMs, dofs, bits) && Near(sv::FloatOf(bits[0]), 1.0f), "held after the last");
        Check(!track.At(horse, 101 + sv::kHoldMs, dofs, bits), "not past the hold");
        Check(!track.At(0x00090001u, 50, dofs, bits), "another owner has none");

        // A lane not set on one side takes the other side's value.
        std::vector<std::uint32_t> withHole = Lanes(1.0f, 1.0f);
        withHole[1] = sv::kNotSet;
        track.Note(horse, 200, Dofs(), withHole);
        Check(track.At(horse, 140, dofs, bits) && sv::IsSet(bits[1]), "the set side (near it)");
        Check(track.At(horse, 180, dofs, bits) && sv::IsSet(bits[1]) && Near(sv::FloatOf(bits[1]), 0.0f),
            "the set side (near the unset one)");

        // Another layout starts a block; the lookup takes the block in force.
        Check(track.Note(horse, 300, Dofs(7), Lanes(0.25f, 0.25f)), "a new layout");
        Check(track.series[0].blocks.size() == 2, "two blocks");
        Check(track.At(horse, 250, dofs, bits) && (*dofs)[1].id == 2, "before the second block: the first layout, held");
        Check(track.At(horse, 310, dofs, bits) && (*dofs)[1].id == 7 && Near(sv::FloatOf(bits[0]), 0.25f), "the second layout");

        // A clock that went back drops the newer tail, through blocks.
        Check(track.Note(horse, 150, Dofs(), Lanes(0.5f, 0.5f)), "rewound");
        Check(track.series[0].blocks.size() == 1 && track.series[0].blocks[0].times.back() == 150, "the tail went");
        Check(track.Valid(), "valid");

        // A hole up to kBridgeMs (a far horse the engine animated a few times
        // a second) is interpolated, never the rest values.
        sv::Track gap;
        gap.Note(horse, 0, Dofs(), Lanes(0.0f, 0.0f));
        gap.Note(horse, 2000, Dofs(), Lanes(1.0f, 1.0f));
        Check(gap.At(horse, 1000, dofs, bits) && Near(sv::FloatOf(bits[0]), 0.5f, 2e-3f), "a 2 s hole: interpolated");
        // A longer hole: held from either side within kHoldMs, none in the middle.
        sv::Track hole;
        hole.Note(horse, 0, Dofs(), Lanes(0.0f, 0.0f));
        hole.Note(horse, 5000, Dofs(), Lanes(1.0f, 1.0f));
        Check(hole.At(horse, 1400, dofs, bits) && Near(sv::FloatOf(bits[0]), 0.0f), "a 5 s hole: the first held");
        Check(!hole.At(horse, 2500, dofs, bits), "the middle of a 5 s hole: none");
        Check(hole.At(horse, 4000, dofs, bits) && Near(sv::FloatOf(bits[0]), 1.0f), "a 5 s hole: the second ahead");

        // A lane unset in some samples (the engine's frame left it unwritten):
        // interpolated between its set samples, held after the last one; the
        // other lanes keep their own samples.
        sv::Track lanes;
        std::vector<std::uint32_t> unset = Lanes(0.5f, 0.5f);
        unset[0] = sv::kNotSet;
        lanes.Note(horse, 0, Dofs(), Lanes(0.2f, 0.0f));
        lanes.Note(horse, 33, Dofs(), unset);
        lanes.Note(horse, 66, Dofs(), unset);
        lanes.Note(horse, 99, Dofs(), Lanes(0.8f, 1.0f));
        lanes.Note(horse, 132, Dofs(), unset);
        Check(lanes.At(horse, 50, dofs, bits) && Near(sv::FloatOf(bits[0]), 0.2f + 0.6f * 50.0f / 99.0f, 3e-3f) &&
                  Near(sv::FloatOf(bits[1]), 0.5f, 2e-3f),
            "an unset lane between set samples: interpolated; the others their own");
        Check(lanes.At(horse, 140, dofs, bits) && Near(sv::FloatOf(bits[0]), 0.8f, 2e-3f), "an unset lane after: held");
        sv::Track never;
        never.Note(horse, 0, Dofs(), {sv::kNotSet, sv::kNotSet, sv::kNotSet, sv::kNotSet, sv::kNotSet});
        Check(!never.At(horse, 0, dofs, bits), "no lane ever set: no value");
    }

    void TestRingAndCut()
    {
        constexpr std::uint32_t a = 0x00050001u, b = 0x00060001u;
        sv::Track track;
        for (std::uint32_t t = 0; t <= 1000; t += 100)
            track.Note(a, t, Dofs(), Lanes(static_cast<float>(t) / 1000.0f, 0.0f));
        track.Note(b, 0, Dofs(), Lanes(0.0f, 0.0f));
        track.Note(a, 1100, Dofs(9), Lanes(0.0f, 0.0f));

        sv::Track cut;
        track.CutInto(cut, 250, 650, [](std::uint32_t owner) { return owner == 0x00050001u; });
        Check(cut.series.size() == 1 && cut.series[0].owner == a, "only the kept owner");
        const sv::Block& block = cut.series[0].blocks[0];
        Check(block.times.front() == 0 && block.times[1] == 50 && block.times.back() == 350, "shifted, the value at base at 0");
        Check(Near(sv::FloatOf(sv::FromHalf(block.values[0])), 0.25f, 2e-3f), "the value in force at base");
        Check(cut.Valid(), "the cut is valid");

        sv::Track ring = track;
        ring.DropBefore(450);
        Check(ring.series.size() == 2 && ring.series[0].owner == a, "an owner within the hold stays");
        Check(ring.series[0].blocks[0].times.front() == 400, "the point in force stays");
        Check(ring.series[0].blocks.size() == 2 && ring.Valid(), "the later block stays");
        const std::vector<sv::Dof>* dofs = nullptr;
        std::vector<std::uint32_t> bits;
        Check(ring.At(a, 450, dofs, bits) && Near(sv::FloatOf(bits[0]), 0.45f, 2e-3f), "still interpolated at the cut");
        ring.DropBefore(1105);
        Check(ring.series[0].blocks.size() == 1 && ring.series[0].blocks[0].times.front() == 1100, "a whole block before goes");
        ring.DropBefore(1600);
        Check(ring.series.size() == 1 && ring.series[0].owner == a, "an owner long gone goes whole");

        // Validation.
        sv::Track bad = track;
        bad.series[0].blocks[0].values.pop_back();
        Check(!bad.Valid(), "values must match the lanes");
        bad = track;
        bad.series[0].blocks[1].times[0] = 50;
        Check(!bad.Valid(), "times ascend through blocks");
        bad = track;
        std::swap(bad.series[0], bad.series[1]);
        Check(!bad.Valid(), "owners ascend");
    }

    void TestMapAndEvidence()
    {
        const std::vector<std::uint8_t> table = HorseTable();
        sv::Layout puppet;
        sv::ParseLayout(table.data(), 4, 0x20, puppet);
        std::vector<sv::Write> writes;
        std::uint32_t missing = 0;
        // Recorded: the vec4 muscle, a DOF the puppet lacks, the float muscle.
        const std::vector<sv::Dof> recorded = {sv::Dof{sv::kTrackAnimatedNormalMaps, 4, 2},
            sv::Dof{sv::kTrackAnimatedNormalMaps, 2, 99}, sv::Dof{sv::kTrackAnimatedNormalMaps, 2, 1}};
        sv::MapLayout(recorded, puppet, writes, missing);
        Check(missing == 1 && writes.size() == 5, "four lanes of the vec4 and one float");
        Check(writes[0].offset == 0x10 && writes[0].lane == 0 && writes[3].offset == 0x1C && writes[3].lane == 3,
            "the vec4 at the puppet's offset, lane by lane");
        Check(writes[4].offset == 0x08 && writes[4].lane == 5, "the float after the missing DOF's lane");
        sv::MapLayout({sv::Dof{sv::kTrackAnimatedNormalMaps, 4, 1}}, puppet, writes, missing);
        Check(writes.empty() && missing == 1, "another type is another DOF");

        std::vector<std::uint16_t> ids;
        std::vector<std::uint32_t> lanes;
        const std::vector<std::uint32_t> bits = {sv::BitsOf(0.1f), sv::BitsOf(0.2f), sv::BitsOf(0.3f), sv::BitsOf(0.4f),
            sv::BitsOf(0.5f), sv::BitsOf(0.6f), sv::BitsOf(0.7f)};
        sv::TrackLanes(puppet.dofs, bits, sv::kTrackAnimatedNormalMaps, ids, lanes);
        Check(ids.size() == 5 && ids[0] == 2 && ids[4] == 6 && Near(sv::FloatOf(lanes[0]), 0.3f), "the muscle lanes only");

        std::vector<std::uint32_t> other = bits;
        other[2] = sv::BitsOf(0.8f);
        other[5] = sv::kNotSet;
        const sv::Compare cmp = sv::CompareLanes(bits, other);
        Check(cmp.lanes == 7 && cmp.bothSet == 6 && cmp.setMismatch == 1 && Near(cmp.maxAbs, 0.5f) && cmp.worstLane == 2,
            "compare: lanes, set on both, difference");
        Check(sv::MaxStep(bits, bits) == 0.0f, "a still sample does not move");
    }

    // The sweat lane rides in the sample: the track keeps and interpolates it,
    // the mapping never writes it into the component nor counts it missing.
    void TestSweatLane()
    {
        constexpr std::uint32_t horse = 0x00050001u;
        std::vector<sv::Dof> dofs = Dofs();
        dofs.push_back(sv::Dof{sv::kTrackSweat, 2, sv::kSweatId});
        sv::Track track;
        std::vector<std::uint32_t> a = Lanes(0.0f, 0.0f), b = Lanes(1.0f, 1.0f);
        a.push_back(sv::BitsOf(0.2f));
        b.push_back(sv::BitsOf(0.6f));
        Check(track.Note(horse, 0, dofs, a) && track.Note(horse, 100, dofs, b) && track.Valid(), "a sample with the sweat lane");
        const std::vector<sv::Dof>* got = nullptr;
        std::vector<std::uint32_t> bits;
        Check(track.At(horse, 50, got, bits) && bits.size() == 6 && Near(sv::FloatOf(bits[5]), 0.4f, 2e-3f),
            "the sweat interpolated like any lane");
        const std::vector<std::uint8_t> table = HorseTable();
        sv::Layout puppet;
        sv::ParseLayout(table.data(), 4, 0x20, puppet);
        std::vector<sv::Write> writes;
        std::uint32_t missing = 0;
        sv::MapLayout(dofs, puppet, writes, missing);
        bool sweatWritten = false;
        for (const sv::Write& w : writes)
            sweatWritten = sweatWritten || w.lane == 5;
        Check(!sweatWritten && missing == 0 && writes.size() == 5, "the sweat lane is not mapped and not missing");
    }

    void TestCarryForward()
    {
        std::vector<std::uint32_t> last;
        std::vector<std::uint32_t> a = {sv::BitsOf(0.1f), sv::kNotSet, sv::BitsOf(0.3f)};
        Check(sv::CarryForward(last, a) == 0 && !sv::IsSet(a[1]), "first sample: nothing to carry");
        std::vector<std::uint32_t> b = {sv::kNotSet, sv::BitsOf(0.5f), sv::kNotSet};
        Check(sv::CarryForward(last, b) == 2 && Near(sv::FloatOf(b[0]), 0.1f) && Near(sv::FloatOf(b[2]), 0.3f) &&
                  Near(sv::FloatOf(b[1]), 0.5f),
            "unset lanes keep their last value");
        std::vector<std::uint32_t> c = {sv::kNotSet, sv::kNotSet};
        Check(sv::CarryForward(last, c) == 0 && last.size() == 2, "another lane count starts over");
    }

    void TestConfigDefault()
    {
        const sv::ReplayConfig config;
        Check(config.record && config.replay, "MuscleRecord and MuscleReplay are on by default");
    }
}

int RunShaderVarsTests()
{
    g_shaderVarsFailures = 0;
    TestParseLayout();
    TestHalf();
    TestTrack();
    TestRingAndCut();
    TestMapAndEvidence();
    TestSweatLane();
    TestCarryForward();
    TestConfigDefault();
    if (g_shaderVarsFailures == 0)
        std::printf("Shader vars tests passed.\n");
    return g_shaderVarsFailures;
}
