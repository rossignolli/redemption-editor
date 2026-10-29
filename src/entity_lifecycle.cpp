#include "entity_lifecycle.h"

#include <algorithm>

namespace replay::entity
{
    IdAllocator::IdAllocator(std::uint16_t maxSlots)
        : m_slotInstance(maxSlots, 0)
    {
    }

    void IdAllocator::Reset()
    {
        std::fill(m_slotInstance.begin(), m_slotInstance.end(), std::uint16_t{0});
        m_inUse = 0;
        // The instance counter keeps running: ids from an earlier recording
        // are never reused by a later one.
    }

    ReplayId IdAllocator::Allocate()
    {
        for (std::size_t slot = 0; slot < m_slotInstance.size(); ++slot)
        {
            if (m_slotInstance[slot] != 0)
                continue;
            const std::uint16_t instance = m_nextInstance;
            m_nextInstance = static_cast<std::uint16_t>(m_nextInstance == 0xFFFF ? 1 : m_nextInstance + 1);
            m_slotInstance[slot] = instance;
            ++m_inUse;
            return {static_cast<std::uint16_t>(slot), instance};
        }
        return {};
    }

    void IdAllocator::Release(ReplayId id)
    {
        if (!id.Valid() || id.slot >= m_slotInstance.size() ||
            m_slotInstance[id.slot] != id.instance)
        {
            return;
        }
        m_slotInstance[id.slot] = 0;
        --m_inUse;
    }

    void LifecycleTrack::Begin(std::uint32_t blockMs)
    {
        m_blockMs = blockMs ? blockMs : kBlockMs;
        m_live.clear();
        m_liveAttachments.clear();
        m_blocks.clear();
        m_stats = {};
    }

    void LifecycleTrack::Advance(std::uint32_t timeMs)
    {
        const std::uint32_t start = timeMs - timeMs % m_blockMs;
        if (!m_blocks.empty() && m_blocks.back().startMs >= start)
            return;
        std::uint32_t next = m_blocks.empty() ? start : m_blocks.back().startMs + m_blockMs;
        // A gap of empty blocks carries nothing new; open only the covering one.
        if (next < start)
            next = start;
        Block block;
        block.startMs = next;
        block.fullCreates = m_live;
        block.fullAttachments = m_liveAttachments;
        m_stats.fullCreates += block.fullCreates.size();
        m_stats.fullAttachments += block.fullAttachments.size();
        ++m_stats.blocks;
        m_blocks.push_back(std::move(block));
    }

    bool LifecycleTrack::Create(const CreatePacket& packet)
    {
        if (!packet.id.Valid())
            return false;
        for (const CreatePacket& live : m_live)
        {
            if (live.id == packet.id)
                return false;
        }
        Advance(packet.timeMs);
        m_live.push_back(packet);
        m_blocks.back().events.push_back({true, packet.timeMs, packet});
        ++m_stats.creates;
        return true;
    }

    bool LifecycleTrack::Delete(ReplayId id, std::uint32_t timeMs)
    {
        const auto it = std::find_if(m_live.begin(), m_live.end(),
            [&](const CreatePacket& live) { return live.id == id; });
        if (it == m_live.end())
            return false;
        Advance(timeMs);
        for (std::size_t index = m_liveAttachments.size(); index-- > 0;)
        {
            const Attachment attachment = m_liveAttachments[index];
            if (attachment.child == id || attachment.parent == id)
                Detach(attachment.child, timeMs);
        }
        m_blocks.back().events.push_back({false, timeMs, *it});
        m_live.erase(it);
        ++m_stats.deletes;
        return true;
    }

    bool LifecycleTrack::Attach(const Attachment& attachment, std::uint32_t timeMs)
    {
        const auto isLive = [&](ReplayId id) {
            return std::any_of(m_live.begin(), m_live.end(),
                [&](const CreatePacket& live) { return live.id == id; });
        };
        if (!isLive(attachment.child) || !isLive(attachment.parent) ||
            attachment.child == attachment.parent)
        {
            return false;
        }
        const auto current = std::find_if(m_liveAttachments.begin(), m_liveAttachments.end(),
            [&](const Attachment& live) { return live.child == attachment.child; });
        if (current != m_liveAttachments.end())
        {
            if (*current == attachment)
                return false;
            Detach(attachment.child, timeMs);
        }
        Advance(timeMs);
        m_blocks.back().attachEvents.push_back({true, timeMs, attachment});
        m_liveAttachments.push_back(attachment);
        ++m_stats.attaches;
        return true;
    }

    bool LifecycleTrack::Detach(ReplayId child, std::uint32_t timeMs)
    {
        const auto it = std::find_if(m_liveAttachments.begin(), m_liveAttachments.end(),
            [&](const Attachment& live) { return live.child == child; });
        if (it == m_liveAttachments.end())
            return false;
        Advance(timeMs);
        m_blocks.back().attachEvents.push_back({false, timeMs, *it});
        m_liveAttachments.erase(it);
        ++m_stats.detaches;
        return true;
    }

    std::size_t LifecycleTrack::DropBefore(std::uint32_t timeMs)
    {
        std::size_t dropped = 0;
        while (m_blocks.size() > 1 && m_blocks[1].startMs <= timeMs)
        {
            m_blocks.pop_front();
            ++dropped;
        }
        return dropped;
    }

    std::size_t LifecycleTrack::ApproxBytes() const
    {
        std::size_t bytes = 0;
        for (const Block& block : m_blocks)
        {
            bytes += sizeof(Block) + block.fullAttachments.size() * sizeof(Attachment) +
                block.attachEvents.size() * sizeof(AttachEvent) + block.events.size() * sizeof(Event);
            for (const CreatePacket& packet : block.fullCreates)
                bytes += sizeof(CreatePacket) + packet.appearance.assets.size() * sizeof(MetaPedAsset) +
                    packet.weaponComponents.size() * sizeof(std::uint32_t);
        }
        return bytes;
    }

    const LifecycleTrack::Block* LifecycleTrack::BlockAt(std::uint32_t timeMs) const
    {
        const auto after = std::upper_bound(m_blocks.begin(), m_blocks.end(), timeMs,
            [](std::uint32_t time, const Block& block) { return time < block.startMs; });
        return after == m_blocks.begin() ? nullptr : &*(after - 1);
    }

    void LifecycleTrack::AttachmentsAt(std::uint32_t timeMs, std::vector<Attachment>& out) const
    {
        out.clear();
        const Block* block = BlockAt(timeMs);
        if (!block)
            return;
        out = block->fullAttachments;
        for (const AttachEvent& event : block->attachEvents)
        {
            if (event.timeMs > timeMs)
                break;
            out.erase(std::remove_if(out.begin(), out.end(),
                          [&](const Attachment& live) { return live.child == event.attachment.child; }),
                out.end());
            if (event.attach)
                out.push_back(event.attachment);
        }
    }

    void LifecycleTrack::KeptIds(std::vector<std::uint32_t>& out) const
    {
        out.clear();
        for (const CreatePacket& packet : m_live)
            out.push_back(packet.id.Packed());
        for (const Block& block : m_blocks)
        {
            for (const CreatePacket& packet : block.fullCreates)
                out.push_back(packet.id.Packed());
            for (const Event& event : block.events)
                out.push_back(event.packet.id.Packed());
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }

    void LifecycleTrack::ModelsBetween(std::uint32_t fromMs, std::uint32_t toMs, std::vector<std::uint32_t>& out) const
    {
        out.clear();
        if (m_blocks.empty() || toMs < fromMs)
            return;
        const auto add = [&](const CreatePacket& packet) {
            if (packet.weaponHash || !packet.modelHash)
                return;
            if (std::find(out.begin(), out.end(), packet.modelHash) == out.end())
                out.push_back(packet.modelHash);
        };
        // The block holding fromMs (its state there), then every create up
        // to toMs in it and in the blocks after it.
        auto block = std::upper_bound(m_blocks.begin(), m_blocks.end(), fromMs,
            [](std::uint32_t time, const Block& candidate) { return time < candidate.startMs; });
        if (block != m_blocks.begin())
        {
            --block;
            std::vector<CreatePacket> alive;
            AliveAt(fromMs, alive);
            for (const CreatePacket& packet : alive)
                add(packet);
        }
        for (; block != m_blocks.end() && block->startMs <= toMs; ++block)
        {
            if (block->startMs > fromMs)
                for (const CreatePacket& packet : block->fullCreates)
                    add(packet);
            for (const Event& event : block->events)
            {
                if (event.timeMs > toMs)
                    break;
                if (event.create && event.timeMs >= fromMs)
                    add(event.packet);
            }
        }
    }

    std::uint64_t LifecycleTrack::AliveStamp(std::uint32_t timeMs) const
    {
        const auto after = std::upper_bound(m_blocks.begin(), m_blocks.end(), timeMs,
            [](std::uint32_t time, const Block& block) { return time < block.startMs; });
        if (after == m_blocks.begin())
            return kNoAliveStamp;
        const Block& block = *(after - 1);
        // The same walk as AliveAt: events in order, up to the first after timeMs.
        std::uint32_t applied = 0;
        for (const Event& event : block.events)
        {
            if (event.timeMs > timeMs)
                break;
            ++applied;
        }
        return (static_cast<std::uint64_t>(block.startMs) << 32) | applied;
    }

    void LifecycleTrack::AliveAt(std::uint32_t timeMs, std::vector<CreatePacket>& out) const
    {
        out.clear();
        const auto after = std::upper_bound(m_blocks.begin(), m_blocks.end(), timeMs,
            [](std::uint32_t time, const Block& block) { return time < block.startMs; });
        if (after == m_blocks.begin())
            return;
        const Block& block = *(after - 1);
        out = block.fullCreates;
        for (const Event& event : block.events)
        {
            if (event.timeMs > timeMs)
                break;
            if (event.create)
            {
                out.push_back(event.packet);
                continue;
            }
            out.erase(std::remove_if(out.begin(), out.end(),
                          [&](const CreatePacket& live) { return live.id == event.packet.id; }),
                out.end());
        }
    }
}
