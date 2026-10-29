// The skeleton signature compared at the arm, the skeleton map it falls back
// to, and the QA kit's skeleton_signature_mismatch anomaly.
#include "pose_skeleton_layout.h"
#include "pose_stream.h"
#include "qa_health.h"
#include "skeleton_map.h"
#include "skeleton_signature.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    int g_signatureFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_signatureFailures;
    }

    using replay::pose::CompareSkeletonSignature;
    using replay::pose::SignatureVerdict;

    // A body of `body` bones (an arm 1..5 off the root and a spine from 6
    // to the last body bone, so the arm is not an ancestor of the body's last
    // bone), then two extra groups (a hat and hair, say) of `a` and `b`
    // bones fanning out of their first bone, off bones 3 and 5 of the arm,
    // in the order given.
    std::vector<std::int32_t> Skeleton(std::uint32_t body, std::uint32_t a, std::uint32_t b, bool aFirst)
    {
        std::vector<std::int32_t> parents;
        parents.push_back(-1);
        for (std::uint32_t n = 1; n < body; ++n)
            parents.push_back(n == 6 ? 0 : static_cast<std::int32_t>(n - 1));
        const auto group = [&](std::uint32_t count, std::int32_t hangOff) {
            const std::int32_t first = static_cast<std::int32_t>(parents.size());
            parents.push_back(hangOff);
            for (std::uint32_t n = 1; n < count; ++n)
                parents.push_back(first);
        };
        if (aFirst)
        {
            group(a, 3);
            group(b, 5);
        }
        else
        {
            group(b, 5);
            group(a, 3);
        }
        return parents;
    }

    std::vector<std::uint16_t> Engine(const std::vector<std::int32_t>& parents)
    {
        std::vector<std::uint16_t> table;
        for (const std::int32_t parent : parents)
            table.push_back(parent < 0 ? 0xFFFFu : static_cast<std::uint16_t>(parent));
        return table;
    }

    void TestSame()
    {
        const std::vector<std::int32_t> record = Skeleton(40, 20, 24, true);
        const std::vector<std::uint16_t> puppet = Engine(record);
        const auto compare = CompareSkeletonSignature(record, {}, puppet.data(), static_cast<std::uint32_t>(puppet.size()));
        Check(compare.verdict == SignatureVerdict::Same && compare.differ == 0, "the same table is the same skeleton");
        Check(compare.compared == record.size() - 1 && compare.skipped == 1, "the root is skipped, the rest compared");

        // The noise real clips show between records of one skeleton: a bone or two
        // discovered with another parent.
        std::vector<std::uint16_t> noisy = puppet;
        noisy[30] = 28;
        const auto noise = CompareSkeletonSignature(record, {}, noisy.data(), static_cast<std::uint32_t>(noisy.size()));
        Check(noise.verdict == SignatureVerdict::Same && noise.differ == 1 && noise.firstDiff == 30,
            "one differing bone is noise, not another skeleton");
    }

    void TestGroupOrder()
    {
        // player_zero's two signatures: the same bones, two groups in the
        // other order (a block of differing parents from the first group).
        const std::vector<std::int32_t> record = Skeleton(40, 20, 24, true);
        const std::vector<std::int32_t> other = Skeleton(40, 20, 24, false);
        Check(record.size() == other.size(), "the same bone count");
        const std::vector<std::uint16_t> puppet = Engine(other);
        const auto compare = CompareSkeletonSignature(record, {}, puppet.data(), static_cast<std::uint32_t>(puppet.size()));
        Check(compare.verdict == SignatureVerdict::Differ, "groups in another order: another skeleton");
        Check(compare.firstDiff == 40 && compare.lastDiff > 40 && compare.differ > replay::pose::kSignatureDifferLimit,
            "the difference starts at the first extra group");

        // The fallback: the skeleton map matches the groups by size and
        // shape, whatever their order.
        std::vector<std::int32_t> target(other.begin(), other.end());
        const replay::pose::SkeletonMap map = replay::pose::BuildSkeletonMap(record.data(),
            static_cast<std::uint32_t>(record.size()), target.data(), static_cast<std::uint32_t>(target.size()));
        Check(map.mode == replay::pose::SkeletonMapMode::Mapped && map.matched == record.size() && map.rest == 0,
            "the map poses every bone of the reordered skeleton");
        Check(map.source[40] == 60 && map.source[64] == 40, "each group takes its own recorded group");

        // Two extra groups of the same size and shape cannot be told apart
        // by the parents alone: the map takes them in order (documented
        // limit; the hanging bone differs here, so they are told apart).
        const std::vector<std::int32_t> twinsA = Skeleton(40, 20, 20, true);
        const std::vector<std::int32_t> twinsB = Skeleton(40, 20, 20, false);
        std::vector<std::int32_t> twinsTarget(twinsB.begin(), twinsB.end());
        const replay::pose::SkeletonMap twins = replay::pose::BuildSkeletonMap(twinsA.data(),
            static_cast<std::uint32_t>(twinsA.size()), twinsTarget.data(), static_cast<std::uint32_t>(twinsTarget.size()));
        Check(twins.mode == replay::pose::SkeletonMapMode::Mapped && twins.source[40] == 60,
            "twin groups are told apart by the bone they hang off");
    }

    void TestUnknown()
    {
        const std::vector<std::int32_t> record = Skeleton(40, 20, 24, true);
        const std::vector<std::int32_t> other = Skeleton(40, 20, 24, false);
        const std::vector<std::uint16_t> puppet = Engine(other);
        // A rider's layout: most bones ambiguous at discovery.
        std::vector<std::uint8_t> ambiguous(record.size(), 0);
        for (std::size_t bone = 0; bone < 60; ++bone)
            ambiguous[bone] = 1;
        const auto rider = CompareSkeletonSignature(record, ambiguous, puppet.data(), static_cast<std::uint32_t>(puppet.size()));
        Check(rider.verdict == SignatureVerdict::Unknown && std::string(rider.reason) == "ambiguous",
            "more than 50 ambiguous bones: the layout cannot vouch");
        Check(std::string(replay::pose::SignatureUnusable(record, ambiguous)) == "ambiguous",
            "and the puppet's table is not even asked for");
        Check(replay::pose::SignatureUnusable(record, {}) == nullptr, "a clean layout can vouch");

        // Ambiguous bones under the limit are left out of the comparison.
        std::vector<std::uint8_t> few(record.size(), 0);
        std::vector<std::uint16_t> noisy = Engine(record);
        for (std::size_t bone = 70; bone < 80; ++bone)
        {
            few[bone] = 1;
            noisy[bone] = 1;
        }
        const auto skipped = CompareSkeletonSignature(record, few, noisy.data(), static_cast<std::uint32_t>(noisy.size()));
        Check(skipped.verdict == SignatureVerdict::Same && skipped.differ == 0 && skipped.ambiguous == 10,
            "ambiguous bones are not compared");

        // Demoted bones (object space in the record) are not compared: a record can
        // have a whole block (e.g. 122 bones of 764..935) demoted.
        std::vector<std::int32_t> demoted = record;
        for (std::size_t bone = 40; bone < 52; ++bone)
            demoted[bone] = replay::pose::kUnresolvedParent;
        const std::vector<std::uint16_t> same = Engine(record);
        const auto demotedSame = CompareSkeletonSignature(demoted, {}, same.data(), static_cast<std::uint32_t>(same.size()));
        Check(demotedSame.verdict == SignatureVerdict::Same && demotedSame.skipped == 13, "demoted bones are skipped");

        // Another bone count: not a signature question.
        const auto count = CompareSkeletonSignature(record, {}, puppet.data(), static_cast<std::uint32_t>(puppet.size() - 1));
        Check(count.verdict == SignatureVerdict::Unknown && std::string(count.reason) == "count", "another count: unknown");

        // Too few bones the record can vouch for.
        std::vector<std::int32_t> mostlyDemoted(record.size(), replay::pose::kUnresolvedParent);
        mostlyDemoted[1] = 0;
        Check(std::string(replay::pose::SignatureUnusable(mostlyDemoted, {})) == "too_few", "a mostly demoted layout cannot vouch");
    }

    // The capture demoted a block mid-recording (the engine rebuilt the original's
    // face group under the same bone count). The track tells which bones of the
    // layout at the playhead were parented before, and the rebuilt map leaves
    // exactly those to the puppet.
    void TestRebuilt()
    {
        constexpr std::uint32_t kBones = 40;
        replay::pose::SkeletonLayout layout;
        layout.boneCount = kBones;
        layout.parent.push_back(replay::pose::kRootParent);
        for (std::uint32_t bone = 1; bone < kBones; ++bone)
            layout.parent.push_back(static_cast<std::int32_t>(bone - 1));
        layout.ambiguous.assign(kBones, 0);
        layout.highPrecision.assign(kBones, 0);
        for (std::uint32_t bone = 0; bone < kBones; ++bone)
            layout.evaluationOrder.push_back(bone);
        replay::pose::StreamConfig config;
        config.keyframeIntervalMs = 200;
        replay::pose::PoseTrack track;
        track.Begin(layout, config);
        std::vector<std::uint32_t> block;
        for (std::uint32_t bone = 8; bone < 20; ++bone)
            block.push_back(bone);
        for (std::uint32_t timeMs = 0; timeMs <= 900; timeMs += 33)
        {
            if (timeMs == 495)
            {
                replay::pose::SkeletonLayout demoted = layout;
                replay::pose::DemoteBones(demoted, block);
                track.UpdateLayout(demoted);
            }
            const float t = static_cast<float>(timeMs) / 1000.0f;
            std::vector<replay::pose::BoneQts> locals(kBones);
            for (std::uint32_t bone = 0; bone < kBones; ++bone)
            {
                const float angle = t * (1.0f + 0.1f * static_cast<float>(bone));
                locals[bone].rotation = replay::pose::Normalize({0.0f, 0.0f, std::sin(angle * 0.5f), std::cos(angle * 0.5f)});
                locals[bone].translation[0] = 0.1f;
            }
            std::vector<replay::pose::BoneQts> objects(kBones);
            replay::pose::ComposeObjects(layout, locals.data(), nullptr, objects.data());
            float world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, t, 2.0f, 3.0f, 1};
            replay::pose::FrameInput input;
            input.timeMs = timeMs;
            input.world = world;
            input.locals = locals.data();
            input.objects = objects.data();
            track.Append(input);
        }
        std::vector<std::uint8_t> rebuilt;
        Check(track.RebuiltBonesAt(100, rebuilt) == 0, "before the rebuild nothing is rebuilt");
        Check(track.LayoutAt(100).parent[10] == 9 && track.LayoutAt(800).parent[10] == replay::pose::kUnresolvedParent,
            "the layout at the playhead is the chunk's, not the last one");
        Check(track.RebuiltBonesAt(800, rebuilt) == 12 && rebuilt[8] && rebuilt[19] && !rebuilt[7] && !rebuilt[20],
            "after it, the demoted block");
        // The demoted bones are skipped by the parent comparison: it says same.
        std::vector<std::uint16_t> puppet;
        for (const std::int32_t parent : layout.parent)
            puppet.push_back(parent < 0 ? 0xFFFFu : static_cast<std::uint16_t>(parent));
        const auto compare = CompareSkeletonSignature(track.LayoutAt(800).parent, {}, puppet.data(), kBones);
        Check(compare.verdict == SignatureVerdict::Same && compare.skipped == 13, "the block hides from the comparison");
        const replay::pose::SkeletonMap map = replay::pose::BuildRebuiltMap(puppet.data(), kBones, rebuilt);
        Check(map.mode == replay::pose::SkeletonMapMode::Mapped && map.matched == 28 && map.rest == 12 && map.source[7] == 7 &&
                map.source[8] == -1 && map.source[20] == 20,
            "the rebuilt map keeps every other bone and rests the block");
        std::vector<std::uint8_t> most(kBones, 1);
        most[0] = 0;
        Check(replay::pose::BuildRebuiltMap(puppet.data(), kBones, most).mode == replay::pose::SkeletonMapMode::Refused,
            "a rebuild of most of the skeleton is refused");
    }

    void TestQaKit()
    {
        using replay::qa::Anomaly;
        using replay::qa::Kind;
        Anomaly anomaly;
        const std::string differ =
            "POSE_V2_SKELETON_SIGNATURE,id=0:341,kind=ped,model=0x0D7114C9,phase=arm,verdict=differ,"
            "anomaly=skeleton_signature_mismatch,why=none,action=mapped,bones=1050,compared=1040,differ=128,"
            "first_bone=764,last_bone=935,ambiguous=0,skipped=10,matched=1050,rest=0,dropped=0,groups=9/9,"
            "matched_groups=9,map=mapped,map_reason=none,record=0x155,skeleton=0x1234,playhead_ms=8100\n";
        Check(replay::qa::ClassifyLine(differ, 0, anomaly) && anomaly.kind == Kind::SkeletonSignatureMismatch &&
                anomaly.entity == "0:341" && anomaly.playheadMs == 8100 &&
                anomaly.detail.find("differ=128") != std::string::npos && anomaly.detail.find("first_bone=764") != std::string::npos,
            "a mismatch line is skeleton_signature_mismatch");
        const std::string same =
            "POSE_V2_SKELETON_SIGNATURE,id=0:341,kind=ped,model=0x0D7114C9,phase=arm,verdict=same,why=none,action=exact,"
            "bones=1050,compared=1049,differ=1,first_bone=763,last_bone=763,ambiguous=0,skipped=1,record=0x155,"
            "skeleton=0x1234,playhead_ms=100\n";
        Check(!replay::qa::ClassifyLine(same, 0, anomaly), "a matching skeleton is no anomaly");
        Check(std::string(replay::qa::KindText(Kind::SkeletonSignatureMismatch)) == "skeleton_signature_mismatch" &&
                replay::qa::CountsText(replay::qa::Counts()).find("skeleton_signature_mismatch=0") != std::string::npos,
            "the kind is named and counted");
    }
}

int RunSkeletonSignatureTests()
{
    TestSame();
    TestGroupOrder();
    TestUnknown();
    TestRebuilt();
    TestQaKit();
    std::printf("skeleton signature tests: %d failure(s)\n", g_signatureFailures);
    return g_signatureFailures;
}
