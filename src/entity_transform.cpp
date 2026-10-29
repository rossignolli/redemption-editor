#include "entity_transform.h"

#include <algorithm>
#include <cmath>

namespace replay::entity
{
    pose::WorldTransform RelativeTransform(
        const pose::WorldTransform& parent,
        const pose::WorldTransform& child)
    {
        const pose::Quat inverse = pose::Conjugate(pose::Normalize(parent.rotation));
        const pose::Vec3 delta{
            child.position.x - parent.position.x,
            child.position.y - parent.position.y,
            child.position.z - parent.position.z};
        pose::WorldTransform relative;
        relative.position = pose::Rotate(inverse, delta);
        relative.rotation = pose::Normalize(pose::Multiply(inverse, child.rotation));
        return relative;
    }

    pose::WorldTransform ComposeTransform(
        const pose::WorldTransform& parent,
        const pose::WorldTransform& relative)
    {
        const pose::Quat rotation = pose::Normalize(parent.rotation);
        const pose::Vec3 offset = pose::Rotate(rotation, relative.position);
        pose::WorldTransform world;
        world.position = {
            parent.position.x + offset.x,
            parent.position.y + offset.y,
            parent.position.z + offset.z};
        world.rotation = pose::Normalize(pose::Multiply(rotation, relative.rotation));
        return world;
    }

    float PlacementErrorM(
        const pose::WorldTransform& parent,
        const pose::WorldTransform& seatRelative,
        const pose::WorldTransform& child)
    {
        const pose::WorldTransform seat = ComposeTransform(parent, seatRelative);
        const float dx = seat.position.x - child.position.x;
        const float dy = seat.position.y - child.position.y;
        const float dz = seat.position.z - child.position.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    RelativeTrack::RelativeTrack(std::uint32_t sampleHz)
        : m_intervalMs(sampleHz > 0 ? 1000u / sampleHz : 0u)
    {
    }

    bool RelativeTrack::Append(const TransformSample& sample)
    {
        if (!m_samples.empty())
        {
            const TransformSample& last = m_samples.back();
            if (sample.timeMs < last.timeMs)
                return false;
            // A visibility change is kept even inside the interval.
            if (sample.timeMs - last.timeMs < m_intervalMs && sample.visible == last.visible)
                return false;
        }
        if (m_samples.size() >= kMaxSamples)
        {
            ++m_dropped;
            return false;
        }
        m_samples.push_back(sample);
        return true;
    }

    bool RelativeTrack::Sample(std::uint32_t timeMs, TransformSample& out, const std::vector<std::uint32_t>* cuts) const
    {
        if (m_samples.empty())
            return false;
        if (timeMs <= m_samples.front().timeMs)
        {
            out = m_samples.front();
            return true;
        }
        if (timeMs >= m_samples.back().timeMs)
        {
            out = m_samples.back();
            return true;
        }
        const auto next = std::upper_bound(m_samples.begin(), m_samples.end(), timeMs,
            [](std::uint32_t time, const TransformSample& sample) { return time < sample.timeMs; });
        const TransformSample& b = *next;
        const TransformSample& a = *(next - 1);
        float t = b.timeMs > a.timeMs
            ? static_cast<float>(timeMs - a.timeMs) / static_cast<float>(b.timeMs - a.timeMs)
            : 0.0f;
        // Across a recording cut, the side the playhead is on.
        t = gate::CutAwareFactor(cuts, a.timeMs, b.timeMs, timeMs, t);
        out.timeMs = timeMs;
        out.visible = t >= 1.0f ? b.visible : a.visible;
        out.relative = pose::InterpolateWorld(a.relative, b.relative, t);
        return true;
    }

    bool RelativeTrack::Span(std::uint32_t& firstMs, std::uint32_t& lastMs) const
    {
        if (m_samples.empty())
            return false;
        firstMs = m_samples.front().timeMs;
        lastMs = m_samples.back().timeMs;
        return true;
    }
}
