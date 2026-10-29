#include "rope_observer.h"

#include "audio_observer.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace replay::rope
{
    namespace
    {
        // Offline layout of the retail rope manager: the manager pointer, its
        // active list and the rope instance fields.
        constexpr std::uintptr_t kRopeManagerRva = 0x453A4A8;
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
        constexpr std::uintptr_t kSetAttachmentEntityRva = 0x2D24DE0;
        constexpr std::uint8_t kSetAttachmentEntitySignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
            0xF9, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0x49, 0x38, 0x48, 0x85, 0xC9, 0x74};
        using SetAttachmentEntityFn = void(__fastcall*)(std::uint64_t entry, std::uint64_t entity);

        constexpr std::uint64_t kAttachmentActive = 0x70;

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

        bool GuardedSetEntity(SetAttachmentEntityFn setter, std::uint64_t entry, std::uint64_t entity)
        {
            __try
            {
                setter(entry, entity);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
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
        const std::uint64_t base = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
        std::uint64_t manager = 0;
        if (!Read(base + kRopeManagerRva, manager) || !Plausible(manager))
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
        static SetAttachmentEntityFn setter = nullptr;
        if (checked == 0)
        {
            const std::uint64_t base = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
            std::uint8_t bytes[sizeof(kSetAttachmentEntitySignature)] = {};
            const bool read = replay::audio::ReadBytes(base + kSetAttachmentEntityRva, bytes, sizeof(bytes));
            checked = read && std::memcmp(bytes, kSetAttachmentEntitySignature, sizeof(bytes)) == 0 ? 1 : -1;
            if (checked == 1)
                setter = reinterpret_cast<SetAttachmentEntityFn>(base + kSetAttachmentEntityRva);
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
        if (!xyz || !count || !rope.positions || !rope.previous || rope.vertices > kMaxVertices ||
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
        if (!Plausible(entry))
            return false;
        const std::uint16_t value = active ? 1 : 0;
        return GuardedWrite(entry + kAttachmentActive, &value, sizeof(value));
    }

    bool SetRopeFlag(const RopeInfo& rope, std::uint32_t flag, bool on)
    {
        if (!Plausible(rope.address))
            return false;
        std::uint32_t flags = 0;
        if (!Read(rope.address + kRopeFlags, flags))
            return false;
        const std::uint32_t wanted = on ? (flags | flag) : (flags & ~flag);
        return wanted == flags || GuardedWrite(rope.address + kRopeFlags, &wanted, sizeof(wanted));
    }
}
