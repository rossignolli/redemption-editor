// The per-group skeleton map that poses a puppet whose skeleton has another
// bone count than its record.
#include "skeleton_map.h"
#include "skeleton_map_horses_029.h"

#include <cmath>
#include <cstdio>
#include <string_view>
#include <vector>

namespace
{
    int g_mapFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_mapFailures;
    }

    using replay::pose::BuildSkeletonMap;
    using replay::pose::SkeletonMap;
    using replay::pose::SkeletonMapMode;

    std::vector<std::int32_t> Table(const std::int32_t* parents, std::uint32_t count)
    {
        return std::vector<std::int32_t>(parents, parents + count);
    }

    // The engine's table as the puppet gives it: u16 parents, bone 0 not -2.
    std::vector<std::int32_t> EngineTable(const std::int32_t* parents, std::uint32_t count)
    {
        std::vector<std::int32_t> table(parents, parents + count);
        for (std::int32_t& parent : table)
            if (parent < 0)
                parent = 0xFFFF;
        return table;
    }

    SkeletonMap Map(const std::vector<std::int32_t>& recorded, const std::vector<std::int32_t>& puppet)
    {
        return BuildSkeletonMap(recorded.data(), static_cast<std::uint32_t>(recorded.size()),
            puppet.data(), static_cast<std::uint32_t>(puppet.size()));
    }

    // A small rig in depth-first order: a body of 6 bones (0 root, spine
    // 1-2, neck 3-4, a leg 5 off 1), then pieces appended as the engine
    // appends groups.
    std::vector<std::int32_t> Body() { return {-2, 0, 1, 2, 3, 1}; }

    void TestSplit()
    {
        std::vector<std::int32_t> parents;
        std::vector<replay::pose::SkeletonGroup> groups;
        const std::vector<std::int32_t> horse = Table(skeleton_map_data::kHorse38_710, skeleton_map_data::kHorse38_710Count);
        replay::pose::NormaliseParents(horse.data(), static_cast<std::uint32_t>(horse.size()), parents);
        replay::pose::SplitSkeletonGroups(parents, groups);
        Check(groups.size() == 5, "the 710-bone horse splits into the engine's 5 groups");
        if (groups.size() == 5)
        {
            Check(groups[0].first == 0 && groups[0].count == 495 && groups[0].rootParent == -1, "group 0: the body, 495 bones");
            Check(groups[1].first == 495 && groups[1].count == 176 && groups[1].rootParent == 20, "group 1: 176 bones off bone 20");
            Check(groups[2].first == 671 && groups[2].count == 21 && groups[2].rootParent == 247, "group 2: tack, 21 bones off 247");
            Check(groups[3].first == 692 && groups[3].count == 13 && groups[3].rootParent == 192, "group 3: tack, 13 bones off 192");
            Check(groups[4].first == 705 && groups[4].count == 5 && groups[4].rootParent == 316, "group 4: the extra chain, 5 bones off 316");
        }
        const std::vector<std::int32_t> arthur = Table(skeleton_map_data::kHorse0_701, skeleton_map_data::kHorse0_701Count);
        replay::pose::NormaliseParents(arthur.data(), static_cast<std::uint32_t>(arthur.size()), parents);
        replay::pose::SplitSkeletonGroups(parents, groups);
        Check(groups.size() == 4, "Arthur's horse (701) splits into 4 groups");
        if (groups.size() == 4)
        {
            Check(groups[2].count == 17 && groups[2].rootParent == 192, "its first tack: 17 bones off 192");
            Check(groups[3].count == 13 && groups[3].rootParent == 247, "its second tack: 13 bones off 247");
        }
    }

    void TestIdentical()
    {
        const std::vector<std::int32_t> horse = Table(skeleton_map_data::kHorse26_705, skeleton_map_data::kHorse26_705Count);
        const SkeletonMap map = Map(horse, EngineTable(horse.data(), static_cast<std::uint32_t>(horse.size())));
        Check(map.mode == SkeletonMapMode::Exact, "the same table maps exact");
        Check(map.matched == 705 && map.rest == 0 && map.dropped == 0, "exact: every bone matched");

        std::vector<std::int32_t> small = Body();
        const SkeletonMap own = Map(small, small);
        Check(own.mode == SkeletonMapMode::Exact && own.matched == 6, "a synthetic rig on itself is exact");
    }

    void TestTrailingExtraGroup()
    {
        // Recorded 705 bones, the puppet of the same model built 710.

        const std::vector<std::int32_t> recorded = Table(skeleton_map_data::kHorse26_705, skeleton_map_data::kHorse26_705Count);
        const std::vector<std::int32_t> puppet = EngineTable(skeleton_map_data::kHorse38_710, skeleton_map_data::kHorse38_710Count);
        const SkeletonMap map = Map(recorded, puppet);
        Check(map.mode == SkeletonMapMode::Mapped, "705 recorded on a 710 puppet: mapped");
        Check(map.matched == 705 && map.rest == 5 && map.dropped == 0, "705 bones posed, the 5-bone chain at rest");
        bool oneToOne = true;
        for (std::uint32_t bone = 0; bone < 705; ++bone)
            oneToOne = oneToOne && map.source[bone] == static_cast<std::int32_t>(bone);
        Check(oneToOne, "the first 705 bones map one to one");
        Check(map.source[705] == -1 && map.source[709] == -1, "bones 705..709 have no recorded bone");
        Check(map.targetParents[705] == 316, "the chain's root composes off bone 316");
        Check(map.sourceGroups == 4 && map.targetGroups == 5 && map.matchedGroups == 4, "4 of the puppet's 5 groups matched");

        // The other way round (a 710 record on a 705 puppet): the chain is dropped.
        const SkeletonMap back = Map(Table(skeleton_map_data::kHorse38_710, skeleton_map_data::kHorse38_710Count),
            EngineTable(skeleton_map_data::kHorse26_705, skeleton_map_data::kHorse26_705Count));
        Check(back.mode == SkeletonMapMode::Mapped && back.matched == 705 && back.rest == 0 && back.dropped == 5,
            "710 recorded on a 705 puppet: the extra chain dropped");

        // Synthetic: a 2-bone piece appended off bone 3.
        std::vector<std::int32_t> withPiece = Body();
        withPiece.push_back(3);
        withPiece.push_back(6);
        const SkeletonMap small = Map(Body(), withPiece);
        Check(small.mode == SkeletonMapMode::Mapped && small.matched == 6 && small.rest == 2 && small.dropped == 0,
            "synthetic: body posed, the appended piece at rest");
    }

    void TestDifferentTrailingGroups()
    {
        // Arthur's horse: recorded 701 (tack 17 off 192, 13 off 247), the
        // puppet 705 (tack 21 off 247, 13 off 192). The body (671) matches.
        const std::vector<std::int32_t> recorded = Table(skeleton_map_data::kHorse0_701, skeleton_map_data::kHorse0_701Count);
        const std::vector<std::int32_t> puppet = EngineTable(skeleton_map_data::kHorse2_705, skeleton_map_data::kHorse2_705Count);
        const SkeletonMap map = Map(recorded, puppet);
        Check(map.mode == SkeletonMapMode::Mapped, "701 recorded on a 705 puppet: mapped");
        Check(map.matched == 671 && map.rest == 34 && map.dropped == 30, "the body posed, both tack groups at rest, the recorded tack dropped");
        Check(map.matchedGroups == 2, "the two body groups matched");
        Check(map.source[670] == 670 && map.source[671] == -1 && map.source[704] == -1, "body one to one, tack at rest");

        // Synthetic: the same piece on both sides, but listed in another
        // order after a different one: the same piece still matches. On the
        // puppet it hangs off bone 5, the body's last, so it reads as part
        // of the body group (8 bones against the record's 6): the shared
        // front is matched and the piece split off and matched on its own.
        std::vector<std::int32_t> recordedPieces = Body();
        recordedPieces.insert(recordedPieces.end(), {3, 6, 6});  // 3 bones off 3 (6..8)
        recordedPieces.insert(recordedPieces.end(), {5, 9});     // 2 bones off 5 (9..10)
        std::vector<std::int32_t> puppetPieces = Body();
        puppetPieces.insert(puppetPieces.end(), {5, 6});         // 2 bones off 5 (6..7)
        puppetPieces.insert(puppetPieces.end(), {2, 8, 9, 10});  // 4 bones off 2 (8..11)
        const SkeletonMap pieces = Map(recordedPieces, puppetPieces);
        Check(pieces.mode == SkeletonMapMode::Mapped, "synthetic tack: mapped");
        Check(pieces.matched == 8 && pieces.rest == 4 && pieces.dropped == 3, "the shared piece matched, the others rest or dropped");
        Check(pieces.source[6] == 9 && pieces.source[7] == 10, "the piece off bone 5 found where the record has it");
        Check(pieces.source[8] == -1, "the puppet-only piece rests");
    }

    void TestRefusals()
    {
        // An unrelated rig: another body.
        const std::vector<std::int32_t> other = {-2, 0, 0, 0, 1, 2, 3};
        const SkeletonMap map = Map(Body(), other);
        Check(map.mode == SkeletonMapMode::Refused, "an unrelated skeleton is refused");
        Check(std::string_view(map.refuseReason) == "root_group", "refused for the body group");

        // A horse record on a human-sized rig.
        const SkeletonMap horseOnRig = Map(Table(skeleton_map_data::kHorse26_705, skeleton_map_data::kHorse26_705Count), Body());
        Check(horseOnRig.mode == SkeletonMapMode::Refused, "a horse record on another rig is refused");

        // The body matches but the puppet is mostly pieces the record lacks.
        std::vector<std::int32_t> mostlyPieces = Body();
        for (int bone = 6; bone < 20; ++bone)
            mostlyPieces.push_back(bone == 6 ? 3 : bone - 1);
        const SkeletonMap few = Map(Body(), mostlyPieces);
        Check(few.mode == SkeletonMapMode::Refused && std::string_view(few.refuseReason) == "too_few_bones",
            "less than half of the puppet posed: refused");

        const SkeletonMap empty = BuildSkeletonMap(nullptr, 0, nullptr, 0);
        Check(empty.mode == SkeletonMapMode::Refused, "nothing to map: refused");
    }

    bool Near(const float* a, const float* b)
    {
        for (int axis = 0; axis < 3; ++axis)
            if (std::fabs(a[axis] - b[axis]) > 1e-5f)
                return false;
        return true;
    }

    void TestApply()
    {
        // Body() recorded, a puppet with a 2-bone piece off bone 3 at rest.
        std::vector<std::int32_t> puppet = Body();
        puppet.push_back(3);
        puppet.push_back(6);
        const SkeletonMap map = Map(Body(), puppet);
        std::vector<replay::pose::BoneQts> recLocals(6);
        std::vector<replay::pose::BoneQts> recObjects(6);
        for (std::uint32_t bone = 0; bone < 6; ++bone)
        {
            recLocals[bone].translation[0] = static_cast<float>(bone);
            recObjects[bone].translation[0] = 10.0f * static_cast<float>(bone);
            recObjects[bone].translation[1] = 1.0f;
        }
        std::vector<replay::pose::BoneQts> rest(8);
        rest[6].translation[2] = 0.5f;
        rest[7].translation[2] = 0.25f;
        std::vector<replay::pose::BoneQts> locals(8);
        std::vector<replay::pose::BoneQts> objects(8);
        Check(replay::pose::ApplySkeletonMap(map, recLocals.data(), recObjects.data(), 6, rest.data(),
                  locals.data(), objects.data()),
            "a mapped pose is written");
        Check(objects[3].translation[0] == 30.0f && locals[3].translation[0] == 3.0f, "matched bones take the recorded pose");
        const float expected6[3] = {30.0f, 1.0f, 0.5f};
        const float expected7[3] = {30.0f, 1.0f, 0.75f};
        Check(Near(objects[6].translation, expected6), "a rest bone hangs off its parent's recorded object");
        Check(Near(objects[7].translation, expected7), "and its child off it");
        Check(locals[6].translation[2] == 0.5f, "a rest bone keeps its own local");
        Check(!replay::pose::ApplySkeletonMap(map, recLocals.data(), recObjects.data(), 5, rest.data(),
                  locals.data(), objects.data()),
            "a sample of another size is not applied");
    }
}

int RunSkeletonMapTests()
{
    TestSplit();
    TestIdentical();
    TestTrailingExtraGroup();
    TestDifferentTrailingGroups();
    TestRefusals();
    TestApply();
    std::printf("skeleton map tests: %d failure(s)\n", g_mapFailures);
    return g_mapFailures;
}
