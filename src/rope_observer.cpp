#include "rope_observer.h"

#include "audio_observer.h"
#include "engine_calls.h"
#include "game_sig.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace replay::rope
{
    namespace
    {
        namespace sig = replay::sig;

        // The rope manager (a pointer), its active list and the rope instance
        // fields. GET_ROPE_VERTEX_COUNT's core loads the manager, then takes
        // the vertex count (+0x58) less the drawn window's unused tail (+0xCC)
        // and first index (+0xC8).
        const sig::Signature kRopeVertexCountCore{"rope_vertex_count",
            "48 83 EC 28 8B D1 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 1F E8 ?? ?? ?? ?? 48 8B C8 48 85 C0 74 12 0F B7 40 58 2B "
            "81 CC 00 00 00 2B 81 C8 00 00 00 EB 02 33 C0 48 83 C4 28 C3",
            0xFD4930};
        const sig::Signature kRopeManager{"rope_manager", kRopeVertexCountCore, sig::Rel32{9, 13}, "", 0x453A4A8};
        // What WriteWindow writes: GET_ROPE_LAST_VERTEX_COORD's core reads the
        // positions (+0x50, 16 bytes a vertex) up to the unused tail; a pass of
        // the engine over a rope's vertices takes the drawn window (+0xC8,
        // +0xCC, +0x58), the previous step (+0x40) and the positions (+0x50).
        const sig::Signature kRopeLastVertex{"rope_last_vertex",
            "40 53 48 83 EC 20 48 8B D9 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 27 E8 ?? ?? ?? ?? 48 85 C0 74 1D 0F B7 48 58 2B "
            "88 CC 00 00 00 48 8B 40 50 FF C9 48 03 C9 0F 10 04 C8 F3 0F 7F 03 EB 06 0F 57 C0 0F 11 03 48 8B C3 48 83 C4 "
            "20 5B C3",
            0xFD4894};
        const sig::Signature kRopeVertexPass{"rope_vertex_pass",
            "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 81 EC 30 01 00 00 BE 10 00 00 00",
            0x2D1CBD0};
        const sig::Signature kRopeVertexArrays{"rope_vertex_arrays", kRopeVertexPass, sig::Offset{0xC8},
            "41 8B BE C8 00 00 00 41 8B 9E CC 00 00 00 E8 ?? ?? ?? ?? 45 0F B7 4E 58 49 8D 46 70 89 5C 24 58 49 8D 8E 80 "
            "00 00 00 89 7C 24 50 4D 8D 46 60 48 89 44 24 48 33 D2 49 8B 46 40 48 89 4C 24 40 48 8D 8C 24 A0 00 00 00 48 "
            "83 64 24 38 00 48 89 44 24 30 49 8B 46 50",
            0x2D1CC98};
        constexpr std::uint64_t kManagerCount = 0x30;
        constexpr std::uint64_t kManagerListHead = 0x58;
        constexpr std::uint64_t kRopePrevious = 0x40;
        constexpr std::uint64_t kRopePositions = 0x50;
        constexpr std::uint64_t kRopeVertexCount = 0x58;
        constexpr std::uint64_t kRopeDrawStart = 0xC8;
        constexpr std::uint64_t kRopeDrawUnused = 0xCC;
        constexpr std::uint64_t kRopeAttachments = 0x2C0;
        constexpr std::uint64_t kRopeAttachmentCount = 0x2C8;
        constexpr std::uint64_t kAttachmentStride = 0x80;
        constexpr std::uint64_t kAttachmentOffset = 0x10;
        constexpr std::uint64_t kAttachmentBone = 0x24;
        constexpr std::uint64_t kAttachmentEntity = 0x38;
        constexpr std::uint64_t kRopeId = 0x6CC;
        constexpr std::uint64_t kRopeType = 0x6D4;
        constexpr std::uint64_t kRopeMode = 0x6E4;
        constexpr std::uint64_t kRopeLength = 0x708;
        constexpr std::uint64_t kRopeFlags = 0x758;
        constexpr std::uint32_t kMaxRopes = 256;
        constexpr std::uint32_t kMaxVertices = 1024;

        bool Plausible(std::uint64_t pointer)
        {
            return pointer >= 0x10000 && pointer < 0x00007FFFFFFFFFFFull;
        }

        template <typename T>
        bool Read(std::uint64_t address, T& value)
        {
            return replay::audio::ReadBytes(address, &value, sizeof(T));
        }

        // Attachment entity setter used by the engine rope attach
        // (release the old entity reference, add one to the new, store at
        // entry+0x38): two arguments, entry and entity.
        const sig::Signature kRopeSetAttachmentEntity{"rope_set_attachment_entity",
            "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 48 8B DA 48 8B 49 38 48 85 C9 74 06", 0x2D24DE0};

        // The reins slot setter: the entry of a slot (rope +0x2C0, 0x80 each)
        // through the entity setter, then its active byte (+0x70).
        const sig::Signature kRopeSlotSetter{"rope_slot_setter",
            "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 48 8B 41 10 48 8B F1 44 8B F2 41 8A D9 49 C1 "
            "E6 07 48 8B 90 A0 07 00 00 48 8B BA C0 02 00 00 49 8B D0 49 03 FE 48 8B CF E8 ?? ?? ?? ?? 80 7C 24 50 00 88 "
            "5F 70 C6 47 74 00",
            0x6FA1E4};
        constexpr std::uint64_t kAttachmentActive = 0x70;

        // The reins controller update sets and clears the held flag of its rope
        // ([[ctrl+0x10]+0x7A0] +0x758, 0x20000000).
        const sig::Signature kRopeReinsUpdate{"rope_reins_update",
            "48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 55 41 54 41 55 41 56 41 57 48 8D A8 A8 FD FF FF 48 81 EC 30 03 "
            "00 00 83 41 6C FF",
            0x70457C};
        const sig::Signature kRopeHeldFlag{"rope_held_flag", kRopeReinsUpdate, sig::Offset{0xA4},
            "48 8B 41 10 48 8B 88 A0 07 00 00 81 E2 04 40 00 00 74 0D F0 81 89 58 07 00 00 00 00 00 20 EB 08 F0 44 21 81 "
            "58 07 00 00",
            0x704620};

        bool GuardedWrite(std::uint64_t address, const void* data, std::size_t size)
        {
            __try
            {
                std::memcpy(reinterpret_cast<void*>(address), data, size);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool GuardedSetEntity(std::uintptr_t setter, std::uint64_t entry, std::uint64_t entity)
        {
            return engine::Call(engine::kRopeSetAttachmentEntity, setter, entry, entity);
        }

        bool ReadVertex(std::uint64_t positions, std::uint32_t index, float out[3])
        {
            return replay::audio::ReadBytes(positions + index * 16ull, out, 12);
        }
    }

    bool Snapshot(std::vector<RopeInfo>& out, std::uint32_t& managerCount)
    {
        out.clear();
        managerCount = 0;
        const std::uint64_t global = sig::Find(kRopeManager);
        std::uint64_t manager = 0;
        if (!global || !Read(global, manager) || !Plausible(manager))
            return false;
        if (!Read(manager + kManagerCount, managerCount) || managerCount > kMaxRopes)
            return false;
        std::uint64_t node = 0;
        if (!Read(manager + kManagerListHead, node))
            return false;
        for (std::uint32_t walked = 0; node && walked < kMaxRopes; ++walked)
        {
            if (!Plausible(node))
                return false;
            std::uint64_t rope = 0;
            std::uint64_t next = 0;
            if (!Read(node, rope) || !Read(node + 8, next))
                return false;
            node = next;
            if (!Plausible(rope))
                continue;
            RopeInfo info;
            info.address = rope;
            std::uint16_t vertices = 0;
            std::uint16_t attachments = 0;
            std::uint64_t positions = 0;
            std::uint64_t previous = 0;
            std::uint64_t attachmentArray = 0;
            if (!Read(rope + kRopeId, info.id) || !Read(rope + kRopeType, info.type) ||
                !Read(rope + kRopeFlags, info.flags) || !Read(rope + kRopeMode, info.mode) ||
                !Read(rope + kRopeLength, info.length) || !Read(rope + kRopeVertexCount, vertices) ||
                !Read(rope + kRopePositions, positions) ||
                !Read(rope + kRopePrevious, previous) ||
                !Read(rope + kRopeAttachments, attachmentArray) ||
                !Read(rope + kRopeAttachmentCount, attachments))
            {
                continue;
            }
            info.vertices = vertices;
            if (!Read(rope + kRopeDrawStart, info.drawStart) || !Read(rope + kRopeDrawUnused, info.drawUnused))
            {
                info.drawStart = 0;
                info.drawUnused = 0;
            }
            info.positions = Plausible(positions) ? positions : 0;
            info.previous = Plausible(previous) ? previous : 0;
            if (vertices && vertices <= kMaxVertices && Plausible(positions))
            {
                ReadVertex(positions, 0, info.first);
                ReadVertex(positions, vertices - 1u, info.last);
            }
            info.attachmentCount = attachments;
            if (Plausible(attachmentArray))
            {
                const std::uint32_t count = attachments < kMaxAttachments ? attachments : kMaxAttachments;
                for (std::uint32_t index = 0; index < count; ++index)
                {
                    const std::uint64_t entry = attachmentArray + index * kAttachmentStride;
                    RopeAttachment& attachment = info.attachments[index];
                    attachment.entry = entry;
                    Read(entry + kAttachmentEntity, attachment.entity);
                    Read(entry + kAttachmentBone, attachment.bone);
                    replay::audio::ReadBytes(entry + kAttachmentOffset, attachment.offset, 12);
                }
            }
            out.push_back(info);
        }
        return true;
    }

    bool ReadWindow(const RopeInfo& rope, std::vector<float>& out)
    {
        out.clear();
        const std::uint32_t count = WindowCount(rope);
        if (!rope.positions || !count || rope.vertices > kMaxVertices)
            return false;
        static std::vector<float> raw;
        raw.resize(static_cast<std::size_t>(count) * 4u);
        if (!replay::audio::ReadBytes(rope.positions + rope.drawStart * 16ull, raw.data(), raw.size() * sizeof(float)))
            return false;
        out.reserve(static_cast<std::size_t>(count) * 3u);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            out.push_back(raw[index * 4u]);
            out.push_back(raw[index * 4u + 1u]);
            out.push_back(raw[index * 4u + 2u]);
        }
        return true;
    }

    bool MaxStep(const RopeInfo& rope, float& out)
    {
        out = 0.0f;
        if (!rope.positions || !rope.previous || !rope.vertices || rope.vertices > kMaxVertices)
            return false;
        for (std::uint32_t index = 0; index < rope.vertices; ++index)
        {
            float now[3] = {};
            float before[3] = {};
            if (!ReadVertex(rope.positions, index, now) || !ReadVertex(rope.previous, index, before))
                return false;
            const float step = std::sqrt((now[0] - before[0]) * (now[0] - before[0]) +
                (now[1] - before[1]) * (now[1] - before[1]) + (now[2] - before[2]) * (now[2] - before[2]));
            if (step > out)
                out = step;
        }
        return true;
    }

    bool SetAttachmentEntity(std::uint64_t entry, std::uint64_t entity)
    {
        static int checked = 0; // 0 unknown, 1 matches, -1 differs
        static std::uintptr_t setter = 0;
        if (checked == 0)
        {
            setter = sig::Find(kRopeSetAttachmentEntity);
            checked = setter ? 1 : -1;
        }
        if (checked != 1 || !Plausible(entry) || (entity && !Plausible(entity)))
            return false;
        return GuardedSetEntity(setter, entry, entity);
    }

    std::string DescribeHeader(const RopeInfo& rope)
    {
        // 0x6C0..0x780: id, type, mode, lengths (0x708..0x71C), flags (0x758).
        std::uint32_t words[0x30] = {};
        if (!replay::audio::ReadBytes(rope.address + 0x6C0, words, sizeof(words)))
            return "unreadable";
        std::string text;
        char item[32];
        for (std::uint32_t index = 0; index < 0x30; ++index)
        {
            std::snprintf(item, sizeof(item), "%s%08x", index ? " " : "", words[index]);
            text += item;
        }
        return text;
    }

    std::string DescribeEntry(const RopeInfo& rope, std::uint32_t slot)
    {
        if (slot >= kMaxAttachments || !rope.attachments[slot].entry)
            return "none";
        std::uint8_t bytes[0x80] = {};
        if (!replay::audio::ReadBytes(rope.attachments[slot].entry, bytes, sizeof(bytes)))
            return "unreadable";
        std::string text;
        char item[4];
        for (std::uint32_t index = 0; index < sizeof(bytes); ++index)
        {
            std::snprintf(item, sizeof(item), "%02x", bytes[index]);
            text += item;
            if ((index & 7) == 7 && index + 1 < sizeof(bytes))
                text += ' ';
        }
        return text;
    }

    namespace
    {
        // One vertex (x/y/z, the Vec3V w left alone) into both arrays.
        // No objects with destructors here (SEH).
        bool GuardedWriteVertex(std::uint64_t positions, std::uint64_t previous, std::uint32_t index, const float* xyz)
        {
            __try
            {
                std::memcpy(reinterpret_cast<void*>(positions + index * 16ull), xyz, 12);
                std::memcpy(reinterpret_cast<void*>(previous + index * 16ull), xyz, 12);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    bool WriteWindow(const RopeInfo& rope, const float* xyz, std::uint32_t count)
    {
        // Only where the engine's own code shows both arrays and the window.
        static const bool proven = sig::Find(kRopeLastVertex) && sig::Find(kRopeVertexArrays);
        if (!proven || !xyz || !count || !rope.positions || !rope.previous || rope.vertices > kMaxVertices ||
            count != WindowCount(rope))
        {
            return false;
        }
        for (std::uint32_t index = 0; index < count; ++index)
        {
            if (!GuardedWriteVertex(rope.positions, rope.previous, rope.drawStart + index, xyz + index * 3u))
                return false;
        }
        return true;
    }

    bool SetAttachmentActive(std::uint64_t entry, bool active)
    {
        static const bool proven = sig::Find(kRopeSlotSetter) != 0;
        if (!proven || !Plausible(entry))
            return false;
        const std::uint16_t value = active ? 1 : 0;
        return GuardedWrite(entry + kAttachmentActive, &value, sizeof(value));
    }

    bool SetRopeFlag(const RopeInfo& rope, std::uint32_t flag, bool on)
    {
        static const bool proven = sig::Find(kRopeHeldFlag) != 0;
        if (!proven || !Plausible(rope.address))
            return false;
        std::uint32_t flags = 0;
        if (!Read(rope.address + kRopeFlags, flags))
            return false;
        const std::uint32_t wanted = on ? (flags | flag) : (flags & ~flag);
        return wanted == flags || GuardedWrite(rope.address + kRopeFlags, &wanted, sizeof(wanted));
    }
}
