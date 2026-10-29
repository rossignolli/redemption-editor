#pragma once

// The CAMR payload as versions v4..v8 lay it out (with the old marker shake
// fields), for the tests that read old clips and projects (the current
// writer only writes v9). Test only.

#include "camera_track.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace testpayload
{
    using namespace replay;
    using camera::Blend;
    using camera::Keyframe;
    using camera::Track;

    template <typename T>
    void Put(std::vector<std::uint8_t>& out, T value)
    {
        const auto* p = reinterpret_cast<const std::uint8_t*>(&value);
        out.insert(out.end(), p, p + sizeof(T));
    }

    // The CAMR payload of versions v4..v8.
    inline std::vector<std::uint8_t> OldCamera(const Track& track, std::uint16_t version)
    {
        std::vector<std::uint8_t> out;
        std::uint32_t nextId = 1;
        for (const Keyframe& k : track.Keyframes())
            nextId = std::max(nextId, k.id + 1);
        Put(out, nextId);
        Put<std::uint8_t>(out, track.LegacyBlendAnyMode() ? 1 : 0);
        Put<std::uint64_t>(out, track.Keyframes().size());
        for (const Keyframe& k : track.Keyframes())
        {
            Put(out, k.id);
            Put(out, k.timeMs);
            Put(out, k.pose.position.x);
            Put(out, k.pose.position.y);
            Put(out, k.pose.position.z);
            Put(out, k.pose.orientation.w);
            Put(out, k.pose.orientation.x);
            Put(out, k.pose.orientation.y);
            Put(out, k.pose.orientation.z);
            Put(out, k.pose.fov);
            Put(out, static_cast<std::uint32_t>(k.cameraMode));
            Put(out, static_cast<std::uint8_t>(k.markerType));
            Put(out, static_cast<std::uint8_t>(version >= 5 ? k.blend : (k.blend == Blend::Spline ? Blend::Smooth : k.blend)));
            Put<std::uint8_t>(out, k.ease ? 1 : 0);
            Put(out, k.lookAtTarget);
            Put(out, k.lookAtOffset.x);
            Put(out, k.lookAtOffset.y);
            Put(out, k.lookAtRoll);
            Put(out, k.attachTarget);
            Put(out, static_cast<std::uint8_t>(k.attachType));
            Put(out, k.relPosition.x);
            Put(out, k.relPosition.y);
            Put(out, k.relPosition.z);
            Put(out, k.relRotation.x);
            Put(out, k.relRotation.y);
            Put(out, k.relRotation.z);
            Put(out, k.presetDistanceScale);
            Put(out, static_cast<std::uint8_t>(k.legacyShake.type));
            Put(out, k.legacyShake.intensity);
            Put(out, k.legacyShake.speedIndex);
            Put(out, static_cast<std::uint8_t>(k.dof));
            Put(out, static_cast<std::uint8_t>(k.focus));
            Put(out, k.dofIntensity);
            Put(out, k.focusDistance);
            Put(out, k.focusTarget);
            Put(out, k.speedIndex);
            Put<std::uint8_t>(out, k.legacyCurve ? 1 : 0);
            Put(out, k.editedMask);
            if (version >= 4)
            {
                Put(out, k.transition);
                Put(out, k.transitionMs);
                Put(out, k.filter);
                Put(out, k.filterIntensity);
                Put(out, k.saturation);
                Put(out, k.contrast);
                Put(out, k.vignette);
                Put(out, k.brightness);
            }
            if (version >= 5)
                Put(out, static_cast<std::uint8_t>(k.legacyShake.motion));
            if (version >= 6)
            {
                const editor::ShakeCustom& c = k.legacyShake.custom;
                Put(out, c.amp);
                Put(out, c.hz);
                Put(out, c.speedAmp);
                Put(out, c.speedHz);
                Put(out, c.rotWeight);
                Put(out, c.posWeight);
                Put(out, c.speedRef);
                Put(out, c.seed);
                Put(out, c.still);
                Put<std::uint8_t>(out, 0);
            }
            if (version >= 7)
            {
                Put(out, k.spline.alpha);
                Put(out, k.spline.easeIn);
                Put(out, k.spline.easeOut);
                Put(out, k.spline.weight);
                Put(out, k.spline.pacing);
                Put(out, k.spline.channels);
                Put(out, k.spline.overrides);
                Put<std::uint8_t>(out, 0);
            }
            if (version >= 8)
            {
                Put(out, k.attachBone);
                Put(out, k.lookAtBone);
                Put(out, k.nearClip);
                Put(out, k.dofFStop);
                Put(out, k.dofLensMm);
            }
        }
        return out;
    }

}
