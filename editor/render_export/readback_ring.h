#pragma once

// Render export: bookkeeping of the GPU to CPU readback ring.
// Each resolved output frame is copied into a free slot's readback buffer;
// the slot is mapped only after the GPU finished the copy (a fence, or
// "enough presents later"), so the render thread never waits for the GPU.
// Frames leave in order. When every slot is in flight the caller holds the
// present (backpressure, never a drop): with the fixed export timestep a
// held frame changes nothing in the video.
// Pure (unit tested in ReplayCoreTests).

#include <cstdint>

namespace render_export
{
    constexpr std::uint32_t kNoSlot = 0xFFFFFFFFu;

    template <std::uint32_t Slots>
    class ReadbackRing
    {
    public:
        enum class SlotState : std::uint8_t { Free, InFlight, GpuDone };

        // A free slot for output frame `frame`, or kNoSlot.
        std::uint32_t Acquire(std::int64_t frame) noexcept
        {
            for (std::uint32_t i = 0; i < Slots; ++i)
            {
                const std::uint32_t slot = (m_next + i) % Slots;
                if (m_state[slot] != SlotState::Free)
                    continue;
                m_state[slot] = SlotState::InFlight;
                m_frame[slot] = frame;
                m_order[slot] = m_sequence++;
                m_next = (slot + 1) % Slots;
                return slot;
            }
            return kNoSlot;
        }

        void MarkGpuDone(std::uint32_t slot) noexcept
        {
            if (slot < Slots && m_state[slot] == SlotState::InFlight)
                m_state[slot] = SlotState::GpuDone;
        }

        // The oldest slot in the ring if its copy is done (frames leave in
        // order: a newer finished copy waits for the older one).
        std::uint32_t OldestReady() const noexcept
        {
            std::uint32_t oldest = kNoSlot;
            for (std::uint32_t i = 0; i < Slots; ++i)
            {
                if (m_state[i] == SlotState::Free)
                    continue;
                if (oldest == kNoSlot || m_order[i] < m_order[oldest])
                    oldest = i;
            }
            return oldest != kNoSlot && m_state[oldest] == SlotState::GpuDone ? oldest : kNoSlot;
        }

        void Release(std::uint32_t slot) noexcept
        {
            if (slot < Slots)
                m_state[slot] = SlotState::Free;
        }

        std::int64_t FrameOf(std::uint32_t slot) const noexcept { return slot < Slots ? m_frame[slot] : -1; }

        std::uint32_t InFlight() const noexcept
        {
            std::uint32_t count = 0;
            for (std::uint32_t i = 0; i < Slots; ++i)
                count += m_state[i] != SlotState::Free ? 1u : 0u;
            return count;
        }

    private:
        SlotState m_state[Slots] = {};
        std::int64_t m_frame[Slots] = {};
        std::uint64_t m_order[Slots] = {};
        std::uint64_t m_sequence = 0;
        std::uint32_t m_next = 0;
    };
}
