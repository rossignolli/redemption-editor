#pragma once

// The replay's cameras without a camera cut on every frame.
//
// SET_CAM_COORD and SET_CAM_ROT always set the cut bits 0x40 (position) and
// 0x80 (orientation) of the camera frame's flags [cam+0xEC] (build 1491.50:
// handlers 0xF7515C and 0xF75520). The frame is copied to the final rendered
// frame (flags at 0x3EA0C4C), whose "cut" test 0x174AE8 feeds the post
// process: the TAA drops its history on a cut frame. Placed with those two
// natives every tick, every frame would render as a cut (a jagged and
// shimmering picture).
//
// SET_CAM_PARAMS with duration 0 (0x31A4A0) sets the same position, rotation
// and FOV and raises the cut bits only when its p12 is true. Its rotation is
// applied when [cam+0x538] == 3 and [cam+0x520] == 0: the base camera
// constructor 0x2EC6BC writes 3 and 0 (0x2EC748, 0x2EC71D), only the
// POINT_CAM_AT_* paths write 1 or 2 (0x313DA3, 0x313C3F) and STOP_CAM_POINTING
// writes 3 back (0x2F8F53); the replay's cameras never point. Its position is
// applied when [cam+0x498] == 0 (not written by that constructor; its role and
// default are not proven, hypothesis: an attach target). The script still
// checks the first placements of each camera with GET_CAM_ROT / GET_CAM_COORD
// and falls back to the plain natives when they did not take
// (CAMERA_CUT_MODE,phase=fallback).
//
// As a replay editor usually does it (a cut only where the recording had one),
// the cut flag is passed only on a real cut: the camera's first placement, a
// jump of the playhead (seek, scrub, frame step, the settle, the load wait), a
// marker boundary without blend, a recorded cut crossed, or a camera that moved
// farther than any real speed in one tick.
//
// The cut rate: the word at the final frame's flags, read once a tick (rung 2,
// guarded) at the address the signature of 0x174AE8 gives (mov eax,0xC0; test
// word [rip+disp],ax; seta al; ret), counted per second (CAMERA_CUT_RATE, kept
// with Debug=0).
//
// Pure: unit tested in tests/lod_doors_camera_tests.cpp.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace replay::camcut
{
    enum class Mode : std::uint8_t
    {
        Params = 0, // SET_CAM_PARAMS, cut only on a real cut
        Legacy = 1, // SET_CAM_COORD + SET_CAM_ROT + SET_CAM_FOV: every frame a cut
        // The default: the plain natives, then the cut bits 0xC0 of the replay's own
        // camera [cam+0xEC] cleared when the placement is not a real cut.
        // SET_CAM_PARAMS does not move a camera made in an earlier tick
        // (CAMERA_CUT_MODE,phase=fallback,why=params_not_applied).
        Bits = 2,
    };

    inline Mode ModeFromIni(const char* text)
    {
        if (!text)
            return Mode::Bits;
        char lower[16]{};
        for (std::size_t n = 0; n + 1 < sizeof(lower) && text[n]; ++n)
            lower[n] = static_cast<char>(text[n] >= 'A' && text[n] <= 'Z' ? text[n] + 32 : text[n]);
        if (std::strcmp(lower, "legacy") == 0)
            return Mode::Legacy;
        if (std::strcmp(lower, "params") == 0)
            return Mode::Params;
        return Mode::Bits; // "bits", "clearbits", anything else
    }
    inline const char* ModeName(Mode mode)
    {
        return mode == Mode::Legacy ? "legacy" : mode == Mode::Params ? "params" : "bits";
    }

    // ---- the camera object (build 1491.50) -----------------------------------
    // The camera behind a script handle: 0x1739DC(handle), the lookup every CAM
    // native starts with (SET_CAM_COORD's core 0xF7515C, SET_CAM_ROT's 0xF75520,
    // SET_CAM_PARAMS' 0xF753FC and 0x31A4A0, GET_CAM_COORD's 0xF6DA60): it
    // decrypts the camera pool pointer (0x3EC3248..0x3EC3258) and returns the pool
    // slot (0x2B09388); no write, no lock. Called at rung 4: its whole body is
    // compared first. On that object SET_CAM_COORD writes the position at +0x50
    // and ORs 0x40 into the word at +0xEC (0xF751BE, 0xF751C2); SET_CAM_ROT writes
    // the matrix at +0x20 and ORs 0x80. The frame copy 0x170028 carries that word
    // to the final frame (0x3EA0C4C), whose test 0x174AE8 feeds the TAA.
    inline constexpr std::uint32_t kCamLookupRva = 0x1739DC;
    inline constexpr std::uint8_t kCamLookup[] = {
        0x48, 0x83, 0xEC, 0x28, 0x45, 0x33, 0xC0, 0x44, 0x8B, 0xC9, 0x85, 0xC9, 0x78, 0x51, 0x8A, 0x05, 0x58, 0xF8,
        0xD4, 0x03, 0x84, 0xC0, 0x74, 0x39, 0x48, 0x8B, 0x0D, 0x5D, 0xF8, 0xD4, 0x03, 0x48, 0x8B, 0x15, 0x4E, 0xF8,
        0xD4, 0x03, 0x48, 0xC1, 0xC1, 0x20, 0x48, 0xC1, 0xC9, 0x03, 0x48, 0x33, 0xD1, 0x8B, 0xC1, 0x83, 0xE0, 0x1F,
        0xFF, 0xC0, 0x8A, 0xC8, 0x8A, 0x05, 0x2C, 0xF8, 0xD4, 0x03, 0x48, 0xD3, 0xC2, 0x48, 0xC1, 0xC2, 0x20, 0x84,
        0xC0, 0x74, 0x06, 0x4C, 0x8B, 0xC2, 0x49, 0xF7, 0xD0, 0x41, 0x8B, 0xD1, 0x49, 0x8B, 0xC8, 0xE8, 0x50, 0x59,
        0x99, 0x02, 0x4C, 0x8B, 0xC0, 0x49, 0x8B, 0xC0, 0x48, 0x83, 0xC4, 0x28, 0xC3};
    inline constexpr std::uint32_t kCamPositionOffset = 0x50; // float x, y, z (SET_CAM_COORD)
    inline constexpr std::uint32_t kCamFlagsOffset = 0xEC;    // u16, cut bits 0x40 | 0x80
    // Read for the fallback line only (SET_CAM_PARAMS' conditions, 0x31A51B
    // .. 0x31A548; GET_CAM_COORD's frame choice, 0xF6DA78).
    inline constexpr std::uint32_t kCamAttachOffset = 0x498;  // qword, position applied only when 0
    inline constexpr std::uint32_t kCamPointOffset = 0x520;   // qword, rotation applied only when 0
    inline constexpr std::uint32_t kCamModeOffset = 0x538;    // dword, rotation applied only when 3
    inline constexpr std::uint32_t kCamFrameOffset = 0x4B4;   // byte, bit 0: GET_CAM_COORD reads +0x120

    // The object is the camera just placed: SET_CAM_COORD wrote `want` at
    // +0x50 and the position bit is set. Before any write.
    inline bool CamObjectIsOurs(const float position[3], const float want[3], std::uint16_t flags)
    {
        for (int k = 0; k < 3; ++k)
        {
            const float d = position[k] - want[k];
            if (d > 1e-3f || d < -1e-3f)
                return false;
        }
        return (flags & 0x40u) != 0;
    }

    // The word written back: the cut bits off, everything else as it was.
    inline std::uint16_t ClearedCutBits(std::uint16_t flags)
    {
        return static_cast<std::uint16_t>(flags & ~0xC0u); // kCutBits
    }

    // Offline dump 1491.50.
    inline constexpr std::uint32_t kFinalFrameCutTestRva = 0x174AE8;
    inline constexpr std::uint32_t kFinalFrameFlagsRva = 0x3EA0C4C;
    inline constexpr std::uint16_t kCutBits = 0xC0;
    // mov eax,0xC0 / test word [rip+disp32],ax / seta al / ret: bytes 8..11 are the disp.
    inline constexpr std::uint8_t kCutTestHead[] = {0xB8, 0xC0, 0x00, 0x00, 0x00, 0x66, 0x85, 0x05};
    inline constexpr std::uint8_t kCutTestTail[] = {0x0F, 0x97, 0xC0, 0xC3};
    inline constexpr std::size_t kCutTestBytes = sizeof(kCutTestHead) + 4 + sizeof(kCutTestTail);

    // The flags' RVA the code at kFinalFrameCutTestRva reads (0 when the
    // bytes are not that function's).
    inline std::uint32_t FlagsRvaFromCode(const std::uint8_t* code, std::uint32_t codeRva)
    {
        if (!code || std::memcmp(code, kCutTestHead, sizeof(kCutTestHead)) != 0 ||
            std::memcmp(code + sizeof(kCutTestHead) + 4, kCutTestTail, sizeof(kCutTestTail)) != 0)
            return 0;
        std::int32_t disp = 0;
        std::memcpy(&disp, code + sizeof(kCutTestHead), 4);
        const std::int64_t next = static_cast<std::int64_t>(codeRva) + static_cast<std::int64_t>(sizeof(kCutTestHead)) + 4;
        const std::int64_t target = next + disp;
        return target > 0 && target < 0x7FFFFFFF ? static_cast<std::uint32_t>(target) : 0u;
    }

    // A real speed never moves the camera this far in one tick: a cut.
    inline constexpr float kCutSpeedMps = 50.0f;   // the motion blur's own "a cut is not speed"
    inline constexpr float kCutSlackM = 2.0f;
    inline constexpr float kCutTurnDeg = 30.0f;    // 1800 degrees a second at 60 fps
    inline constexpr float kCutFovDeg = 10.0f;     // a zoom cut (the recorded camera's kGameCutFov)

    inline float AngleGap(float a, float b)
    {
        float d = std::fmod(a - b + 540.0f, 360.0f) - 180.0f;
        return d < 0.0f ? -d : d;
    }

    // One camera's last placement.
    struct Tracker
    {
        int camera = 0;       // the handle placed last (0: none)
        bool has = false;
        float position[3]{};
        float rotation[3]{};
        float fov = 0.0f;
        std::uint32_t placements = 0; // on this handle
    };

    struct Placement
    {
        int camera = 0;
        float position[3]{};
        float rotation[3]{};
        float fov = 0.0f;     // 0: not compared
        float realDt = 0.0f;  // real seconds since the last tick (0: unknown)
        bool forceCut = false; // the caller knows of a cut (jump, marker without blend, recorded cut)
    };

    enum CutWhy : std::uint32_t
    {
        kWhyNone = 0,
        kWhyFirst = 1u << 0,  // a new camera handle
        kWhyForced = 1u << 1, // the caller's cut
        kWhyMoved = 1u << 2,  // farther than kCutSpeedMps
        kWhyTurned = 1u << 3, // turned more than kCutTurnDeg
        kWhyZoomed = 1u << 4, // FOV changed more than kCutFovDeg
    };

    // The cut flag for this placement, and the tracker updated.
    inline std::uint32_t Decide(Tracker& tracker, const Placement& p)
    {
        std::uint32_t why = kWhyNone;
        if (!tracker.has || tracker.camera != p.camera)
        {
            why |= kWhyFirst;
            tracker.placements = 0;
        }
        else
        {
            const float dx = p.position[0] - tracker.position[0], dy = p.position[1] - tracker.position[1],
                        dz = p.position[2] - tracker.position[2];
            const float moved = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float dt = p.realDt > 0.0f ? (p.realDt < 0.2f ? p.realDt : 0.2f) : 0.05f;
            if (moved > kCutSpeedMps * dt + kCutSlackM)
                why |= kWhyMoved;
            for (int k = 0; k < 3; ++k)
                if (AngleGap(p.rotation[k], tracker.rotation[k]) > kCutTurnDeg)
                    why |= kWhyTurned;
            const float zoom = p.fov - tracker.fov;
            if (p.fov > 0.0f && tracker.fov > 0.0f && (zoom > kCutFovDeg || zoom < -kCutFovDeg))
                why |= kWhyZoomed;
        }
        if (p.forceCut)
            why |= kWhyForced;
        tracker.camera = p.camera;
        tracker.has = true;
        tracker.fov = p.fov;
        for (int k = 0; k < 3; ++k)
        {
            tracker.position[k] = p.position[k];
            tracker.rotation[k] = p.rotation[k];
        }
        ++tracker.placements;
        return why;
    }

    // A marker boundary is a cut when the frame before was not blending INTO the
    // marker now current. The blend of the segment that starts at the new marker
    // says nothing about the jump into it:
    //   A blending to B  -> B without blend : continuous, no cut
    //   A blending to B  -> B blending to C : continuous, no cut
    //   A without blend  -> B without blend : jump, cut
    //   A without blend  -> B blending to C : jump, cut
    //   A blending to B  -> C (B skipped)   : jump, cut
    // The plans are taken after the blend cancelled by a recorded cut
    // (EvaluateMarkers), so that case counts as "without blend".
    struct MarkerFrame
    {
        bool valid = false; // a marker plan (false: the game camera or nothing)
        int current = -1;
        int next = -1;
        bool blending = false; // blending from current to next
    };

    inline bool MarkerBoundaryCut(const MarkerFrame& before, const MarkerFrame& now)
    {
        if (!before.valid || !now.valid || now.current == before.current)
            return false; // a switch to or from the game camera is the caller's cut
        return !(before.blending && before.next == now.current);
    }

    // The engine took the placement (GET_CAM_ROT / GET_CAM_COORD read back).
    inline bool Took(const float wantPos[3], const float wantRot[3], const float gotPos[3], const float gotRot[3])
    {
        for (int k = 0; k < 3; ++k)
        {
            if (std::fabs(wantPos[k] - gotPos[k]) > 0.05f)
                return false;
            if (AngleGap(wantRot[k], gotRot[k]) > 1.0f)
                return false;
        }
        return true;
    }

    // Frames with the final frame's cut bits, per second of wall time.
    class Rate
    {
    public:
        // One tick. True when a second closed: frames/cuts/placedCuts hold
        // that second's counts (until the next Add).
        bool Add(std::uint64_t nowMs, bool cutFrame, bool placedCut)
        {
            if (!m_started)
            {
                m_started = true;
                m_since = nowMs;
            }
            ++m_frames;
            m_cuts += cutFrame ? 1u : 0u;
            m_placed += placedCut ? 1u : 0u;
            if (nowMs - m_since < 1000)
                return false;
            frames = m_frames;
            cuts = m_cuts;
            placedCuts = m_placed;
            m_frames = m_cuts = m_placed = 0;
            m_since = nowMs;
            return true;
        }
        void Reset() { *this = Rate{}; }

        std::uint32_t frames = 0;
        std::uint32_t cuts = 0;
        std::uint32_t placedCuts = 0;

    private:
        bool m_started = false;
        std::uint64_t m_since = 0;
        std::uint32_t m_frames = 0;
        std::uint32_t m_cuts = 0;
        std::uint32_t m_placed = 0;
    };

    // ---- the view lent to another mod ---------------------------------------
    //
    // A camera mod can film over a paused replay. The replay camera is placed and
    // kept as the streaming focus every tick; when another script camera renders
    // (another ASI made and activated it), the replay lends it the view: it does
    // not place, activate or render its own camera, and clears its streaming
    // focus, until that camera stops rendering or the mode does not lend any more.
    // ReplayCameraRelease: "paused" (default: lent while the replay is paused),
    // "always" (also while it plays), "off" (never: the replay takes its view back
    // every tick).
    enum class Release : std::uint8_t
    {
        Off = 0,
        Paused = 1,
        Always = 2,
    };

    inline Release ReleaseFromIni(const char* text)
    {
        if (!text)
            return Release::Paused;
        char lower[16]{};
        for (std::size_t n = 0; n + 1 < sizeof(lower) && text[n]; ++n)
            lower[n] = static_cast<char>(text[n] >= 'A' && text[n] <= 'Z' ? text[n] + 32 : text[n]);
        if (std::strcmp(lower, "off") == 0 || std::strcmp(lower, "0") == 0)
            return Release::Off;
        if (std::strcmp(lower, "always") == 0 || std::strcmp(lower, "2") == 0)
            return Release::Always;
        return Release::Paused;
    }
    inline const char* ReleaseName(Release mode)
    {
        return mode == Release::Off ? "off" : mode == Release::Always ? "always" : "paused";
    }

    // The replay lends the view this tick: another script camera renders
    // (not the replay's nor its free camera) and the mode lends now.
    inline bool LendView(Release mode, bool replayPaused, bool otherCameraRendering)
    {
        if (!otherCameraRendering)
            return false;
        return mode == Release::Always || (mode == Release::Paused && replayPaused);
    }
}
