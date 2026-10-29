#pragma once

// Read-only view of the engine rope manager: which ropes exist, their type,
// flags, vertex count, length, first and last vertex, and the entities they
// are attached to. Every read is guarded; nothing is written and no engine
// function is called. Script thread.

#include <cstdint>
#include <string>
#include <vector>

namespace replay::rope
{
    // Every entry of the ropes seen live (reins and lead ropes have 24, wagon
    // reins 12, the rest 2 or none); the rider's reins entries are 8..23.
    constexpr std::uint32_t kMaxAttachments = 32;

    struct RopeAttachment
    {
        std::uint64_t entry = 0;  // the attachment entry (stride 0x80)
        std::uint64_t entity = 0; // CEntity*
        std::int32_t bone = -1;
        float offset[3] = {};
    };

    struct RopeInfo
    {
        std::uint64_t address = 0;
        std::uint64_t positions = 0; // Vec3V array, `vertices` entries
        std::uint64_t previous = 0;  // Vec3V array of the previous step
        std::uint32_t id = 0;
        std::uint32_t type = 0;
        std::uint32_t flags = 0;
        std::uint32_t mode = 0;
        std::uint32_t vertices = 0;
        // The drawn window of the vertex array (lasso ropes: +0xC8 first
        // drawn index, +0xCC vertices left unused after it;
        // GET_ROPE_VERTEX_COUNT == vertices - drawStart - drawUnused in every
        // lasso phase; 0 and 0 on script ropes).
        std::uint32_t drawStart = 0;
        std::uint32_t drawUnused = 0;
        float length = 0.0f;
        float first[3] = {};
        float last[3] = {};
        std::uint32_t attachmentCount = 0;
        RopeAttachment attachments[kMaxAttachments];
    };

    // False when the manager or its list cannot be read. managerCount is
    // the manager's own count, for comparison with the walked list.
    bool Snapshot(std::vector<RopeInfo>& out, std::uint32_t& managerCount);

    // The drawn window of the rope, x/y/z packed, in drawing order: the
    // vertices from drawStart up to the unused tail. False when the array
    // cannot be read or the window is not inside it.
    bool ReadWindow(const RopeInfo& rope, std::vector<float>& out);
    inline std::uint32_t WindowCount(const RopeInfo& rope)
    {
        return rope.drawStart + rope.drawUnused <= rope.vertices ? rope.vertices - rope.drawStart - rope.drawUnused : 0;
    }

    // Writes a drawn window (x/y/z packed, `count` vertices from drawStart,
    // count == WindowCount(rope)) into the rope's position and previous-step
    // arrays, so the verlet step starts from that shape at rest. Guarded plain
    // writes on a rope the replay owns (a puppet's reins) or created. False
    // when the window does not match or a write faults.
    bool WriteWindow(const RopeInfo& rope, const float* xyz, std::uint32_t count);

    // Evidence: the rope header fields around lengths/flags/mode and the
    // raw bytes of one attachment entry, as text.
    std::string DescribeHeader(const RopeInfo& rope);
    std::string DescribeEntry(const RopeInfo& rope, std::uint32_t slot);

    // Largest distance between a vertex and its previous-step position
    // (evidence that the previous array is what the offline layout says).
    bool MaxStep(const RopeInfo& rope, float& out);

    // Sets the entity of one attachment entry the way the engine does
    // (its reference-counted entity setter; bone and offset stay), so a
    // rope end moves from one entity to another, or is released with a
    // null entity. Signature-checked and guarded; false when the setter
    // does not match or faults.
    bool SetAttachmentEntity(std::uint64_t entry, std::uint64_t entity);

    // The engine keeps an entry's active word (+0x70) in step with its
    // entity: 1 with an entity, 0 without. Guarded write.
    bool SetAttachmentActive(std::uint64_t entry, bool active);

    // Rope flag set while a ped holds the rope (live evidence: set one
    // frame after the reins go to the hand, cleared after release).
    constexpr std::uint32_t kHeldFlag = 0x20000000u;
    bool SetRopeFlag(const RopeInfo& rope, std::uint32_t flag, bool on);
}
