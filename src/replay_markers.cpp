#include "replay_markers.h"

#include "natives.h"

#include <cmath>

namespace replay::markers
{
    namespace
    {
        // Flat cylinder marker.
        constexpr Hash kCylinderMarker = 0x94FDAE17;
        constexpr unsigned long long kGroundRefreshMs = 500;

        void DrawCylinder(float x, float y, float z, float diameter, float height, const Color& color)
        {
            GRAPHICS::_DRAW_MARKER(
                kCylinderMarker,
                x, y, z,
                0.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 0.0f,
                diameter, diameter, height,
                color.r, color.g, color.b, color.a,
                FALSE, FALSE, 2, FALSE, nullptr, nullptr, FALSE);
        }
    }

    void DrawEntityDiscAt(Entity entity, const Vector3& position, float radius, const Color& color)
    {
        const float height = ENTITY::GET_ENTITY_HEIGHT_ABOVE_GROUND(entity);
        const float groundZ = height > 0.0f && height < 5.0f ? position.z - height : position.z - 1.0f;
        DrawCylinder(position.x, position.y, groundZ + 0.02f, radius * 2.0f, 0.12f, color);
    }

    void GroundCircle::Draw(float x, float y, float z, float radius, const Color& color, unsigned long long nowMs)
    {
        const float dx = x - m_x;
        const float dy = y - m_y;
        const bool moved = dx * dx + dy * dy > 4.0f || radius != m_radius;
        if (!m_valid || moved || nowMs - m_refreshedAt > kGroundRefreshMs)
        {
            for (int index = 0; index < kSegments; ++index)
            {
                const float angle = 6.2831853f * static_cast<float>(index) / kSegments;
                const float px = x + radius * std::cos(angle);
                const float py = y + radius * std::sin(angle);
                float groundZ = 0.0f;
                m_groundZ[index] = MISC::GET_GROUND_Z_FOR_3D_COORD(px, py, z + 30.0f, &groundZ, FALSE)
                    ? groundZ
                    : z - 1.0f;
            }
            m_x = x;
            m_y = y;
            m_radius = radius;
            m_refreshedAt = nowMs;
            m_valid = true;
        }
        for (int index = 0; index < kSegments; ++index)
        {
            const float angle = 6.2831853f * static_cast<float>(index) / kSegments;
            DrawCylinder(
                m_x + m_radius * std::cos(angle),
                m_y + m_radius * std::sin(angle),
                m_groundZ[index] + 0.02f,
                0.6f, 0.35f, color);
        }
    }
}
