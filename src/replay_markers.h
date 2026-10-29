#pragma once

// Editor markers drawn with the engine marker renderer: a disc under an
// entity and a dotted circle on the ground. Script thread only; drawing has
// no effect on the recording or on the puppets.

#include "types.h"

namespace replay::markers
{
    struct Color
    {
        int r = 255;
        int g = 255;
        int b = 255;
        int a = 160;
    };

    // The same disc for an entity the caller knows exists, at `position`, its
    // GET_ENTITY_COORDS(entity, TRUE, FALSE) read this tick.
    void DrawEntityDiscAt(Entity entity, const Vector3& position, float radius, const Color& color);

    // Dotted circle on the terrain. Ground heights are cached per circle and
    // refreshed when the centre moves or the cache is older than refreshMs.
    class GroundCircle
    {
    public:
        void Draw(float x, float y, float z, float radius, const Color& color, unsigned long long nowMs);

    private:
        static constexpr int kSegments = 40;
        float m_groundZ[kSegments] = {};
        float m_x = 0.0f;
        float m_y = 0.0f;
        float m_radius = 0.0f;
        unsigned long long m_refreshedAt = 0;
        bool m_valid = false;
    };
}
