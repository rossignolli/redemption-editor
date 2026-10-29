#pragma once
// The requests the weapon system makes for a shot's effects, recorded before
// the engine's own range and camera tests (per shot, not the effect
// instances), and the per-shot audit of what the clip has for each shot.
//
// Why [binary]: the muzzle flash 0x14523E0 tests the distance to the camera
// position (0x141B954 against the info's range +8, squared) and the frustum
// of the viewport [0x3EC2610]+0x530..0x5A0 (cmpleps/movmskps, cmp eax,0xF)
// and leaves at 0x1452C24 before it creates the effect at 0x2C3A624; the
// shell 0x145111C does the same (0x14512C8, 0x14512DA). Both are called,
// twice each (a byte 0 then 1 as the last argument), by the wrappers
// 0x14309A8 (muzzle: rcx manager 0x4F3EF90, rdx shooter, r8 CWeapon, r9
// weapon object, then the bone, the info, a vector by pointer and a dword)
// and 0x14308F0 (shell: rcx manager, rdx shooter, r8 CWeapon, r9 weapon
// object, two dwords: the bone is the second). Copying the instances already
// made misses the flash of an NPC's shot outside the gameplay camera's view.
//
// The wrappers are detoured to record (they always forward). The replay calls
// the same wrapper per recorded request (PlanCall below builds the arguments
// from the puppets) and lets the in-game test decide about the sound: no
// direct call path from either wrapper reaches the audio creates within 12
// levels [binary], but 629 indirect calls stay unresolved, so every call is
// measured (sounds of the shooter and its weapon right after it). The
// viewport [0x3EC2610] being the replay's free camera stays [hypothesis]; the
// playback logs the engine's own plane test against the rendered camera.
//
// Pure (no game); unit tested in tests/fx_residue_tests.cpp.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace replay::weaponfx
{
    enum class Kind : std::uint8_t
    {
        Muzzle = 1, // 0x14309A8 -> 0x14523E0 (flash, then the smoke pass)
        Shell = 2,  // 0x14308F0 -> 0x145111C (the cartridge)
    };

    constexpr std::size_t kMaxIntents = 65536;
    constexpr std::uint32_t kNoId = 0xFFFF0000u; // an invalid replay id, packed (entity::ReplayId{})

    struct Intent
    {
        std::uint32_t timeMs = 0;
        std::uint32_t shooter = kNoId; // packed replay id of the shooter, kNoId not recorded
        std::uint32_t weapon = kNoId;  // packed replay id of the weapon object, kNoId not recorded
        std::uint8_t kind = 0;       // Kind
        std::int32_t bone = -1;      // muzzle: arg 5; shell: arg 6
        std::int32_t argA = 0;       // muzzle: arg 8; shell: arg 5
        std::uint32_t fxHash = 0;    // muzzle: [info+0]; shell: 0
        std::uint32_t fxHashAlt = 0; // muzzle: [info+4]
        float range = 0.0f;          // muzzle: [info+8] (the distance test)
        float vec[4] = {};           // muzzle: arg 7
    };
    constexpr std::size_t kIntentBytes = 4 + 4 + 4 + 1 + 4 + 4 + 4 + 4 + 4 + 16;

    inline bool ValidIntent(const Intent& i)
    {
        if (i.kind != static_cast<std::uint8_t>(Kind::Muzzle) && i.kind != static_cast<std::uint8_t>(Kind::Shell))
            return false;
        if (!std::isfinite(i.range))
            return false;
        for (const float v : i.vec)
            if (!std::isfinite(v))
                return false;
        return true;
    }

    struct Track
    {
        std::vector<Intent> intents; // time ascending

        void Clear() { intents.clear(); }
        bool Empty() const { return intents.empty(); }
        std::size_t Count(Kind kind) const
        {
            return static_cast<std::size_t>(std::count_if(intents.begin(), intents.end(),
                [&](const Intent& i) { return i.kind == static_cast<std::uint8_t>(kind); }));
        }
        // A late time is clamped to the last one. False: invalid or the cap.
        bool Add(const Intent& i)
        {
            if (!ValidIntent(i) || intents.size() >= kMaxIntents)
                return false;
            Intent copy = i;
            if (!intents.empty() && copy.timeMs < intents.back().timeMs)
                copy.timeMs = intents.back().timeMs;
            intents.push_back(copy);
            return true;
        }
        bool Valid() const
        {
            if (intents.size() > kMaxIntents)
                return false;
            for (std::size_t n = 0; n < intents.size(); ++n)
            {
                if (!ValidIntent(intents[n]))
                    return false;
                if (n && intents[n].timeMs < intents[n - 1].timeMs)
                    return false;
            }
            return true;
        }
    };

    // Per shot of the clip, what it has: a muzzle effect event of the
    // shooter's (WeaponFxEvents) or the shot's own muzzle hash (older clips,
    // where a hash shared with another shot counts as none: the claim by time
    // of ShotFxLegacyDedupe gave it to the other), a shell event, and the
    // weapon system's requests (this section).
    constexpr std::uint32_t kShotWindowMs = 60; // a request or an effect this close to the shot is the shot's
    constexpr std::uint32_t kShellWindowMs = 1000; // a shell up to this long after the shot (a lever or a pump cycles later)
    constexpr std::uint32_t kShellRegisteredSite = 0x14513CE; // the shell's registered get (0x2C3A2C0 at 0x14513C9) [binary+log]

    struct ShotRow
    {
        std::uint32_t timeMs = 0;
        std::uint32_t shooter = 0;
        bool player = false;
        bool muzzleEvent = false;  // a muzzle one-shot in the clip for this shot
        bool shellEvent = false;   // a shell one-shot in the clip for this shot
        bool muzzleShared = false; // older clip: the shot's muzzle hash was claimed by two shots
        bool hasLegacyMuzzle = false;
        int inView = -1;           // the muzzle in the recorded gameplay camera's view: 1, 0, -1 unknown
    };

    // Whether a point is in a camera's view, as evidence: the angle between
    // the camera's forward (rotation order 2: pitch x, yaw z, degrees) and the
    // point under half the horizontal field of view of a 16:9 frame (fov is
    // the vertical one). A cone, not the engine's frustum planes: evidence
    // only.
    inline bool InView(const float camera[3], float pitchDeg, float yawDeg, float fovDeg, const float point[3])
    {
        constexpr float kDeg = 3.14159265358979f / 180.0f;
        const float pitch = pitchDeg * kDeg;
        const float yaw = yawDeg * kDeg;
        const float forward[3] = {-std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), std::sin(pitch)};
        const float v[3] = {point[0] - camera[0], point[1] - camera[1], point[2] - camera[2]};
        const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (!(length > 0.0f) || !std::isfinite(length) || !(fovDeg > 0.0f))
            return false;
        float cosine = (v[0] * forward[0] + v[1] * forward[1] + v[2] * forward[2]) / length;
        cosine = cosine > 1.0f ? 1.0f : cosine < -1.0f ? -1.0f : cosine;
        const float angle = std::acos(cosine);
        const float halfH = std::atan(std::tan(fovDeg * kDeg * 0.5f) * 16.0f / 9.0f);
        return angle < halfH;
    }

    struct ShotAudit
    {
        std::uint32_t shots = 0;
        std::uint32_t withMuzzle = 0;
        std::uint32_t noMuzzle = 0;      // shot_no_fx: no muzzle effect of its own in the clip
        std::uint32_t noMuzzleNpc = 0;
        std::uint32_t culled = 0;        // the weapon system asked and the engine made none (range or view)
        std::uint32_t noRequest = 0;     // the clip has requests but none for this shot
        std::uint32_t withShell = 0;
        std::uint32_t requestsMuzzle = 0;
        std::uint32_t requestsShell = 0;
        std::uint32_t noMuzzleOutOfView = 0; // of noMuzzle: the gameplay camera did not see the muzzle
        bool hasRequests = false;

        std::string Line(const std::string& clip) const
        {
            std::ostringstream line;
            line << "FX_SHOT_AUDIT_SHOTS,clip=" << clip << ",shots=" << shots << ",with_muzzle=" << withMuzzle
                 << ",no_muzzle=" << noMuzzle << ",no_muzzle_npc=" << noMuzzleNpc << ",culled=" << culled
                 << ",no_request=" << noRequest << ",with_shell=" << withShell << ",requests_muzzle=" << requestsMuzzle
                 << ",requests_shell=" << requestsShell << ",no_muzzle_out_of_view=" << noMuzzleOutOfView
                 << ",requests=" << (hasRequests ? 1 : 0);
            return line.str();
        }
    };

    inline std::uint32_t AbsDiff(std::uint32_t a, std::uint32_t b) { return a > b ? a - b : b - a; }

    // shots: one row per recorded shot (time ascending or not); intents: the
    // clip's track. A shotgun's pellets (same shooter, same ms) are one shot.
    inline ShotAudit AuditShots(const std::vector<ShotRow>& shots, const Track& intents)
    {
        ShotAudit audit;
        audit.hasRequests = !intents.Empty();
        audit.requestsMuzzle = static_cast<std::uint32_t>(intents.Count(Kind::Muzzle));
        audit.requestsShell = static_cast<std::uint32_t>(intents.Count(Kind::Shell));
        for (std::size_t n = 0; n < shots.size(); ++n)
        {
            const ShotRow& shot = shots[n];
            bool pellet = false;
            for (std::size_t m = 0; m < n && !pellet; ++m)
                pellet = shots[m].shooter == shot.shooter && shots[m].timeMs == shot.timeMs;
            if (pellet)
                continue;
            ++audit.shots;
            const bool muzzle = shot.muzzleEvent || (shot.hasLegacyMuzzle && !shot.muzzleShared);
            audit.withMuzzle += muzzle ? 1 : 0;
            audit.withShell += shot.shellEvent ? 1 : 0;
            if (muzzle)
                continue;
            ++audit.noMuzzle;
            audit.noMuzzleNpc += shot.player ? 0 : 1;
            audit.noMuzzleOutOfView += shot.inView == 0 ? 1 : 0;
            if (!audit.hasRequests)
                continue;
            bool requested = false;
            for (const Intent& intent : intents.intents)
                if (intent.kind == static_cast<std::uint8_t>(Kind::Muzzle) && intent.shooter == shot.shooter &&
                    AbsDiff(intent.timeMs, shot.timeMs) <= kShotWindowMs)
                {
                    requested = true;
                    break;
                }
            if (requested)
                ++audit.culled;
            else
                ++audit.noRequest;
        }
        return audit;
    }

    // ---- the requests played back ----------------------------------------

    // The recorded sites the request makes (fx_observer.h kMuzzleFlashSite;
    // the shell's registered get above). In a clip with requests the replay
    // calls the wrapper and drops the recorded effect of the same shot.
    constexpr std::uint32_t kMuzzleEffectSite = 0x1452671;
    // [binary] the managers both callers pass in rcx (0x11479D8, 0x1101421).
    constexpr std::uintptr_t kManagerRva = 0x4F3EF90;
    // [binary] the muzzle info map the caller 0x1147648 asks (0x1280278 on
    // the object at 0x4E37550: a binary map at +0x78, entries at +8, u16 count
    // at +0x10, 16-byte entries {u32 key, pad, info*}; 0x450210 is a lower
    // bound search on the key), the key [[CWeapon+0x60]+0x240] or, when not
    // zero, the component's [[[CWeapon+0x70]+0x80]+0x94].
    constexpr std::uintptr_t kMuzzleInfoTableRva = 0x4E37550;
    constexpr std::uint32_t kInfoMapOffset = 0x78;
    constexpr std::uint32_t kInfoMapEntries = 0x8;
    constexpr std::uint32_t kInfoMapCount = 0x10;
    constexpr std::uint32_t kInfoEntryBytes = 16;
    constexpr std::uint32_t kInfoEntryValue = 8;
    constexpr std::uint32_t kMaxInfoEntries = 4096;
    constexpr std::uint32_t kWeaponInfoOffset = 0x60;      // CWeapon -> CWeaponInfo
    constexpr std::uint32_t kWeaponInfoMuzzleKey = 0x240;  // CWeaponInfo -> muzzle info key
    constexpr std::uint32_t kWeaponComponentOffset = 0x70; // CWeapon -> component holder
    constexpr std::uint32_t kComponentInfoOffset = 0x80;
    constexpr std::uint32_t kComponentMuzzleKey = 0x94;
    constexpr std::uint32_t kObjectWeaponOffset = 0x448;   // CObject -> CWeapon (object_check.h)
    // [binary] the viewport both cores test (0x14525A1, 0x14512DA): [rva]
    // holds it; 8 float4 at +0x530..+0x5A0 are two sets of four planes (x, y,
    // z coefficients and d per lane); a point is in when every plane gives at
    // least -0.5 (the constant at 0x330F190, lane 3).
    constexpr std::uintptr_t kViewportRva = 0x3EC2610;
    constexpr std::uint32_t kViewportPlanes = 0x530;
    constexpr float kViewRadius = 0.5f;
    // At most this many calls per playback tick (a gunfight's rest waits for
    // nothing: the excess is counted, not delayed).
    constexpr std::uint32_t kMaxCallsPerTick = 16;
    // The sound window: the census drains after the call (the tick of the
    // call and the two after it).
    constexpr std::uint32_t kSoundWindowDrains = 3;
    constexpr std::size_t kMaxSoundWindows = 64;

    inline bool Plausible(std::uint64_t pointer) { return pointer >= 0x10000 && pointer < 0x00007FFFFFFFFFFFull; }

    // Whether a request can be played: a weapon object recorded (the CWeapon
    // and the object come from its puppet), a bone (the cores leave at once
    // on -1: 0x145244E, 0x1451144) and, for the muzzle, the last dword 0 or 1
    // (the core indexes CWeapon+0x528 + 32 * it, the caller sets it with
    // setne, 0x11477F6).
    inline bool Playable(const Intent& i)
    {
        if (!ValidIntent(i) || i.weapon == kNoId || i.bone < 0)
            return false;
        if (i.kind == static_cast<std::uint8_t>(Kind::Muzzle) && i.argA != 0 && i.argA != 1)
            return false;
        return true;
    }

    // The recorded effects a request replaces (no effect twice).
    struct EventRef
    {
        std::uint32_t site = 0;
        std::uint32_t timeMs = 0;
        std::uint32_t entity = kNoId; // packed id the effect rides (the weapon object or the shooter)
    };
    struct TrackRef
    {
        std::uint32_t site = 0;
        std::uint32_t startMs = 0;
        std::uint32_t endMs = 0;
        std::uint32_t attach = kNoId;
    };

    inline bool SameOwner(const Intent& i, std::uint32_t entity)
    {
        return entity != kNoId && (entity == i.weapon || (i.shooter != kNoId && entity == i.shooter));
    }

    // A muzzle effect of the muzzle site within kShotWindowMs of a playable
    // muzzle request on its weapon or shooter.
    inline bool CoversMuzzleEvent(const Track& track, const EventRef& event)
    {
        if (event.site != kMuzzleEffectSite)
            return false;
        for (const Intent& i : track.intents)
            if (i.kind == static_cast<std::uint8_t>(Kind::Muzzle) && Playable(i) && SameOwner(i, event.entity) &&
                AbsDiff(i.timeMs, event.timeMs) <= kShotWindowMs)
                return true;
        return false;
    }

    // A shell track (the registered get of the shell site: one entry per
    // CWeapon and slot, alive over several shots) with a playable shell
    // request on its weapon or shooter within it.
    inline bool CoversShellTrack(const Track& track, const TrackRef& ref)
    {
        if (ref.site != kShellRegisteredSite)
            return false;
        for (const Intent& i : track.intents)
            if (i.kind == static_cast<std::uint8_t>(Kind::Shell) && Playable(i) && SameOwner(i, ref.attach) &&
                i.timeMs + kShotWindowMs >= ref.startMs && i.timeMs <= ref.endMs + kShotWindowMs)
                return true;
        return false;
    }

    // The arguments of one call, from the request and what the playback
    // read off the puppets (addresses; 0 when absent).
    struct CallInput
    {
        std::uint64_t shooter = 0;    // the shooter's puppet (0: none, both cores test it)
        std::uint64_t object = 0;     // the weapon object's puppet
        std::uint64_t weapon = 0;     // [object + 0x448]
        std::uint64_t weaponInfo = 0; // [weapon + 0x60]
        std::uint64_t info = 0;       // muzzle: the info entry (InfoMatches checked)
    };
    struct CallPlan
    {
        bool ok = false;
        const char* why = "none";
        std::uint8_t kind = 0;
        std::uint64_t shooter = 0;
        std::uint64_t weapon = 0;
        std::uint64_t object = 0;
        std::int32_t bone = -1;
        std::uint64_t info = 0;
        std::int32_t argA = 0;
        float vec[4] = {};
    };
    inline CallPlan PlanCall(const Intent& i, const CallInput& in)
    {
        CallPlan plan;
        plan.kind = i.kind;
        if (!ValidIntent(i))
            return plan.why = "invalid_intent", plan;
        if (i.weapon == kNoId)
            return plan.why = "no_weapon_id", plan;
        if (i.bone < 0)
            return plan.why = "no_bone", plan;
        if (i.kind == static_cast<std::uint8_t>(Kind::Muzzle) && i.argA != 0 && i.argA != 1)
            return plan.why = "bad_arg", plan;
        if (!Plausible(in.object))
            return plan.why = "no_weapon_puppet", plan;
        if (!Plausible(in.weapon))
            return plan.why = "no_cweapon", plan;
        if (!Plausible(in.weaponInfo))
            return plan.why = "no_weapon_info", plan;
        if (in.shooter && !Plausible(in.shooter))
            return plan.why = "bad_shooter", plan;
        if (i.kind == static_cast<std::uint8_t>(Kind::Muzzle) && !Plausible(in.info))
            return plan.why = "no_info", plan;
        plan.shooter = in.shooter;
        plan.weapon = in.weapon;
        plan.object = in.object;
        plan.bone = i.bone;
        plan.argA = i.argA;
        plan.info = i.kind == static_cast<std::uint8_t>(Kind::Muzzle) ? in.info : 0;
        for (int n = 0; n < 4; ++n)
            plan.vec[n] = i.vec[n];
        plan.ok = true;
        plan.why = "ok";
        return plan;
    }

    // The first 12 bytes of a muzzle info entry against the request ([info+0]
    // and [info+4] the effect hashes, [info+8] the range, bit for bit).
    inline bool InfoMatches(const std::uint8_t head[12], const Intent& i)
    {
        std::uint32_t a = 0, b = 0, r = 0, ir = 0;
        std::memcpy(&a, head, 4);
        std::memcpy(&b, head + 4, 4);
        std::memcpy(&r, head + 8, 4);
        std::memcpy(&ir, &i.range, 4);
        return a == i.fxHash && b == i.fxHashAlt && r == ir;
    }

    // The engine's plane test (0x14525A1..0x1452659): planes[0..3] are the
    // x, y, z and d rows of the first four planes (+0x530..+0x560), [4..7]
    // the second four (+0x570..+0x5A0).
    inline bool EngineInView(const float planes[8][4], const float p[3], float radius = kViewRadius)
    {
        for (int lane = 0; lane < 4; ++lane)
        {
            const float a = p[0] * planes[0][lane] + p[1] * planes[1][lane] + p[2] * planes[2][lane] + planes[3][lane];
            const float b = p[0] * planes[4][lane] + p[1] * planes[5][lane] + p[2] * planes[6][lane] + planes[7][lane];
            if (!(a >= -radius) || !(b >= -radius))
                return false;
        }
        return true;
    }

    // The recorded shot a request belongs to: the shot of the same shooter
    // within kShotWindowMs (the nearest), -1 when none.
    inline int ShotIndexFor(const std::vector<ShotRow>& shots, const Intent& i)
    {
        int best = -1;
        std::uint32_t bestDiff = kShotWindowMs + 1;
        if (i.shooter == kNoId)
            return -1;
        for (std::size_t n = 0; n < shots.size(); ++n)
        {
            const std::uint32_t diff = AbsDiff(shots[n].timeMs, i.timeMs);
            if (shots[n].shooter == i.shooter && diff < bestDiff)
            {
                best = static_cast<int>(n);
                bestDiff = diff;
            }
        }
        return best;
    }

    // The sound of one call: census rows of the shooter and its
    // weapon made during the call (the call is synchronous on the script
    // thread: a row inside it is the call's); per shot, the rows of the
    // weapon in the shot's two census windows (kWindowMs, sound_check_core.h)
    // in the playback (after) against the recording's own census of the same
    // windows (expected, -1 when the clip has none); and the refusals the
    // puppet sound rules made during the call (the engine asked, nothing
    // played).
    struct SoundWindow
    {
        std::int32_t inCall = 0;
        std::int32_t after = 0;
        std::int32_t expected = 0;
        std::int32_t refusedInCall = 0;
        bool measured = true; // false: the census is off
    };
    // An extra (heard) sound: one made during the call, or more weapon rows
    // in the shot's windows than the recording had.
    inline bool ExtraSound(const SoundWindow& w)
    {
        return w.measured && (w.inCall > 0 || (w.expected >= 0 && w.after > w.expected));
    }

    struct PlayTotals
    {
        std::uint32_t calls = 0, created = 0, failed = 0, skipped = 0, capped = 0;
        std::uint32_t muzzle = 0, shell = 0;
        std::uint32_t shotsWithExtraSound = 0, shotsWithRefusedSound = 0, shotsCalled = 0;
        std::uint32_t shotsRecorded = 0, droppedEvents = 0, droppedTracks = 0;
        std::uint32_t soundsInCall = 0, soundsAfter = 0, soundsExpected = 0, refusedInCall = 0;
        std::uint32_t viewIn = 0, viewOut = 0, viewUnknown = 0, viewAgree = 0, viewDisagree = 0;
        std::uint32_t windowsUnmeasured = 0;

        std::string Line() const
        {
            std::ostringstream line;
            line << "WEAPON_FX_INTENT_PLAY_TOTAL,calls=" << calls << ",created=" << created << ",failed=" << failed
                 << ",skipped=" << skipped << ",capped=" << capped << ",muzzle=" << muzzle << ",shell=" << shell
                 << ",shots_with_extra_sound=" << shotsWithExtraSound << ",shots_with_refused_sound=" << shotsWithRefusedSound
                 << ",shots_called=" << shotsCalled << ",shots_recorded=" << shotsRecorded
                 << ",sounds_in_call=" << soundsInCall << ",sounds_after=" << soundsAfter << ",sounds_expected=" << soundsExpected
                 << ",refused_in_call=" << refusedInCall << ",dropped_events=" << droppedEvents << ",dropped_tracks=" << droppedTracks
                 << ",in_view=" << viewIn << ",out_of_view=" << viewOut << ",view_unknown=" << viewUnknown
                 << ",view_agree_cam=" << viewAgree << ",view_disagree_cam=" << viewDisagree
                 << ",windows_unmeasured=" << windowsUnmeasured;
            return line.str();
        }
    };

    // Distinct shots of a clip (a shotgun's pellets, same shooter and ms, are one).
    inline std::uint32_t DistinctShots(const std::vector<ShotRow>& shots)
    {
        std::uint32_t count = 0;
        for (std::size_t n = 0; n < shots.size(); ++n)
        {
            bool pellet = false;
            for (std::size_t m = 0; m < n && !pellet; ++m)
                pellet = shots[m].shooter == shots[n].shooter && shots[m].timeMs == shots[n].timeMs;
            count += pellet ? 0 : 1;
        }
        return count;
    }
}
