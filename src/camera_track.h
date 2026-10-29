#pragma once

#include "camera_effects.h"
#include "editor_protocol.h"
#include "time_scale.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace replay::camera
{
    using editor::AttachType;
    using editor::Blend;
    using editor::CameraMode;
    using editor::CopyContext;
    using editor::DofMode;
    using editor::FocusMode;
    using editor::MarkerType;
    using editor::ShakeMotion;
    using editor::ShakeType;

    struct Quaternion
    {
        float w = 1.0f;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct Pose
    {
        editor::Vec3 position{};
        Quaternion orientation{};
        float fov = editor::kMarkerFovDefault;
    };

    // A camera marker (the code keeps the name keyframe). Two views live side
    // by side:
    //
    //  - the legacy view (easing, path, mode, target, offset): what the old
    //    Track API and Track::Evaluate use. Easing and path are on the
    //    ARRIVING keyframe there.
    //  - the marker model (everything after offset): what EvaluateMarkers,
    //    the rules below and CAMR v3 use. The blend is on the SOURCE marker.
    //
    // The legacy API keeps the model of the keyframes it touches in sync
    // (Track::AddOrReplace and friends), a v2 clip is converted as a whole
    // (ConvertV2ToV3) and a v3 clip rebuilds the legacy view (DeriveLegacy).
    struct Keyframe
    {
        std::uint32_t id = 0;
        std::uint64_t timeMs = 0;
        Pose pose;                 // world pose; FOV in degrees
        // Legacy view (API 7, CAMR v2).
        editor::Easing easing = editor::Easing::SmoothStep;
        editor::Path path = editor::Path::Smooth;
        editor::CameraMode mode = editor::CameraMode::Free;
        std::uint32_t target = 0;  // packed replay id (Target, Follow)
        editor::Vec3 offset{};     // Follow: camera minus target, target heading frame
        // Marker model (API 8, CAMR v3).
        CameraMode cameraMode = CameraMode::Game; // default: Game Camera
        MarkerType markerType = MarkerType::Edit;
        // Spline is the default blend of a new marker (it only blends
        // between two Free cameras).
        Blend blend = Blend::Spline;              // of the segment that STARTS here
        bool ease = true;
        std::uint32_t lookAtTarget = 0;           // packed replay id, 0 = None
        editor::Vec2 lookAtOffset{};              // -1..1 on screen
        float lookAtRoll = 0.0f;                  // degrees; presets use it as their roll
        std::uint32_t attachTarget = 0;
        AttachType attachType = AttachType::Default;
        editor::Vec3 relPosition{};               // in the attach frame (see AttachType)
        editor::Vec3 relRotation{};               // same layout as CameraState::rotation: x pitch, y roll, z heading
        float presetDistanceScale = 0.5f;         // 0..1 maps to 0.5x..1.5x of the preset distance
        // The marker's shake, the whole state (camera_marker_shake.h). Every
        // marker but a plain Anchor holds one; an Anchor added by the shake
        // migration (origin 1) holds one too, so the camera path never changes.
        editor::MarkerShake shake = editor::DefaultMarkerShake();
        std::uint8_t origin = 0;                  // 0 placed in the editor, 1 added by the shake migration (kMarkerOriginShakeMigration)
        // The legacy marker shake (CAMR v3..v8: type, intensity, speed, plus
        // movement and Custom numbers): read from an older clip and folded
        // into `shake` on load (mshake::Migrate); None after.
        struct LegacyShake
        {
            ShakeType type = ShakeType::None;
            std::uint8_t intensity = 5;           // 1..10
            std::uint8_t speedIndex = editor::kMarkerSpeedDefaultIndex;
            ShakeMotion motion = ShakeMotion::Off; // CAMR v5
            editor::ShakeCustom custom = editor::DefaultShakeCustom(); // CAMR v6
        } legacyShake;
        editor::SplineTuning spline = editor::DefaultSplineTuning();     // CAMR v7: Spline tuning, overrides over the defaults
        DofMode dof = DofMode::Default;
        FocusMode focus = FocusMode::Auto;
        float dofIntensity = 50.0f;               // 0..100
        float focusDistance = 10.0f;              // metres, 0.5..1000
        std::uint32_t focusTarget = 0;
        std::uint8_t speedIndex = editor::kMarkerSpeedDefaultIndex;
        bool legacyCurve = false;                 // converted v2 clips only: the Hermite path
        // The clip's first marker fades in, its last fades out.
        std::uint8_t transition = 0;              // 0 none, 1 fade
        std::uint16_t transitionMs = 1000;
        // Effects from this marker on (filters and colour).
        std::uint8_t filter = 0;                  // index in editor::kMarkerFilters, 0 none
        std::uint8_t filterIntensity = 100;       // 0..100
        std::int8_t saturation = 0;               // -100..100
        std::int8_t contrast = 0;
        std::int8_t vignette = 0;
        std::int8_t brightness = 0;
        std::uint64_t editedMask = 0;             // CameraOptionField bits edited since inheriting
        // CAMR v8: the bone of the attach and
        // Look At targets (camera_bones.h reference, 0 the entity's root),
        // the near clip (metres, 0 default) and the DOF's own numbers (0:
        // from the intensity).
        std::uint32_t attachBone = 0;
        std::uint32_t lookAtBone = 0;
        float nearClip = 0.0f;
        float dofFStop = 0.0f;
        float dofLensMm = 0.0f;
    };

    // Resolves a keyframe that is not Free into its pose at a time (the
    // recorded game camera, a target's position); false keeps the stored
    // pose. Supplied by the caller: the track itself has no engine access.
    // Legacy view only (Track::Evaluate).
    using Resolver = std::function<bool(const Keyframe& keyframe, std::uint64_t timeMs, Pose& pose)>;

    // Degrees (rotation order 2) of a camera at `from` looking at `to`.
    editor::Vec3 LookAtDegrees(const editor::Vec3& from, const editor::Vec3& to);
    // A world offset into a heading frame (degrees, 0 faces +Y) and back.
    editor::Vec3 ToHeadingFrame(const editor::Vec3& world, float headingDegrees);
    editor::Vec3 FromHeadingFrame(const editor::Vec3& local, float headingDegrees);

    // The camera the game rendered while recording: one sample per captured
    // frame, stamped with the capture clock.
    struct GameSample
    {
        std::uint32_t timeMs;
        editor::Vec3 position;
        editor::Vec3 rotation; // degrees, rotation order 2
        float fov;
    };

    // The game camera at timeMs, interpolated between the two samples around
    // it (each rotation axis the short way round) and held at the ends.
    // False when there are no samples. `samples` is sorted by time.
    // With `cuts` (the recording cuts, sorted), two samples on either side of
    // a cut are not blended: the one on the playhead's side.
    // Two samples at most kGameCutMaxGapMs apart are a camera cut when the
    // camera jumps kGameCutMeters, turns kGameCutDegrees on an axis or
    // changes the FOV by kGameCutFov between them: no real camera moves like
    // that in one frame. EvaluateGame holds the earlier sample until the
    // later one instead of blending (a kill cam cut 36 m and 170 degrees in
    // 17 ms; a blend renders a frame halfway between the shots).
    constexpr std::uint32_t kGameCutMaxGapMs = 50;
    constexpr float kGameCutMeters = 5.0f;
    constexpr float kGameCutDegrees = 60.0f;
    constexpr float kGameCutFov = 10.0f;
    bool IsGameCameraCut(const GameSample& a, const GameSample& b);

    bool EvaluateGame(const std::vector<GameSample>& samples, std::uint64_t timeMs, editor::CameraState& camera,
        const std::vector<std::uint32_t>* cuts = nullptr);

    // The game's cinematic slow motion (the kill cam): a stretch
    // that opens with a camera cut while the recorded time scale falls under
    // kCinematicScale, and closes at the first later cut with the scale back
    // over it (or when the scale is back to normal). A cut inside the
    // stretch (a second angle) keeps it. The replay keeps the slow motion
    // there while the game camera is shown (the time compensation would play
    // 1.9 s of kill cam in 0.23 s). Sorted, not overlapping.
    struct TimeSpan
    {
        std::uint32_t startMs = 0;
        std::uint32_t endMs = 0;
    };
    constexpr float kCinematicScale = 0.5f;
    constexpr std::uint32_t kCinematicScaleWindowMs = 250; // the scale drops this soon after the cut
    std::vector<TimeSpan> CinematicSlowSpans(const std::vector<GameSample>& samples, const timescale::Track& scale);
    const TimeSpan* SpanAt(const std::vector<TimeSpan>& spans, std::uint64_t timeMs);

    // No time limit for the marker spacing cascade.
    constexpr std::uint64_t kNoTimeLimit = ~0ull;

    class Track;
    constexpr std::uint8_t kMarkerOriginShakeMigration = 1;
    // What the shake migration did (SHAKE_MIGRATION).
    struct ShakeMigrationReport
    {
        bool ran = false;
        std::uint32_t legacyMarkers = 0;   // markers that had an old shake
        std::uint32_t events = 0;          // shake events of the old track
        std::uint32_t eventsOnMarkers = 0; // events that sat on a marker and went into it
        std::uint32_t breakpoints = 0;     // the times the events' shake changed while it played
        std::uint32_t markersAdded = 0;    // Anchors added where a change fell between markers
        std::uint32_t pruned = 0;          // added ones dropped again (nothing changed there)
        std::uint32_t inexact = 0;         // segments the model could not hold exactly
        std::shared_ptr<const Track> before; // the track before (the proof); reset once logged
    };

    class Track final
    {
    public:
        // ---- Legacy API (legacy view). Each call also re-derives the marker
        // model of the keyframes it touches and marks the track
        // legacyBlendAnyMode (built with the legacy rules).
        std::uint32_t AddOrReplace(
            std::uint64_t timeMs,
            const editor::CameraState& camera,
            editor::Easing easing = editor::Easing::SmoothStep,
            editor::Path path = editor::Path::Smooth);
        bool Remove(std::uint32_t id);
        bool Move(std::uint32_t id, std::uint64_t timeMs);
        bool Update(std::uint32_t id, const editor::CameraState& camera,
                    editor::Easing easing, editor::Path path);
        bool SetMode(std::uint32_t id, editor::CameraMode mode, std::uint32_t target, const editor::Vec3& offset);
        const Keyframe* Find(std::uint32_t id) const;
        // Each end of the segment around timeMs is resolved at timeMs (so a
        // blend between two live cameras moves with both), then blended.
        bool Evaluate(std::uint64_t timeMs, editor::CameraState& camera, const Resolver& resolver = {}) const;
        const std::vector<Keyframe>& Keyframes() const noexcept;
        bool Empty() const noexcept;
        void Clear();

        // ---- Marker API (model view).
        // Adds a marker (its id is assigned; the rest is taken as given, see
        // InheritFrom). 0 when CanPlace refuses the time.
        std::uint32_t AddMarker(const Keyframe& marker);
        // Removes a marker and re-applies the last marker rule.
        // Refuses the clip's start and end (IsClipBound).
        bool RemoveMarker(std::uint32_t id);
        // Moves a marker when CanPlace allows the new time, never past a
        // neighbour (ClampMoveTime must keep the time): the list order never
        // changes.
        bool MoveMarker(std::uint32_t id, std::uint64_t timeMs);
        // Mutable access for the rules below. Never change timeMs through it
        // (use MoveMarker): the list stays sorted by time.
        Keyframe* FindMutable(std::uint32_t id);
        // Applies an options command (ApplyOptions) and the last marker rule.
        // These three push the following markers out
        // to keep their spacing; with a limit (the clip's duration) the
        // whole change must fit in it: one that would push the last marker
        // past the limit is refused with nothing changed (the push is the
        // smallest the spacing allows, so nothing else would fit).
        bool SetOptions(std::uint32_t id, const editor::CameraKeyframeOptions& options,
            std::uint64_t limitMs = kNoTimeLimit);
        // ResetToDefault on the marker (the last one keeps its camera type).
        bool ResetMarker(std::uint32_t id, std::uint64_t limitMs = kNoTimeLimit);
        // Paste(from) into the marker, then the last marker rule.
        bool PasteMarker(std::uint32_t id, const Keyframe& from, CopyContext context,
            std::uint64_t limitMs = kNoTimeLimit);

        // New times for some markers at once. All or
        // nothing: false (nothing changed) when the list order would change,
        // two markers would sit closer than their spacing (MarkerSpacingMs
        // of the earlier one) or one would pass limitMs. With `pushAfter`,
        // the markers after the last one retimed are pushed only as far as
        // the spacing asks (KeepSpacingAfter).
        bool Retime(const std::vector<std::pair<std::uint32_t, std::uint64_t>>& times, std::uint64_t limitMs = kNoTimeLimit,
            bool pushAfter = false);
        // Adds a copy of `marker` (any id; the caller sets
        // its time) without the borrowed view bit. 0 when CanPlace refuses.
        std::uint32_t AddCopy(const Keyframe& marker);

        // A converted v2 clip: blends between any camera types, and the last
        // marker always counts.
        bool LegacyBlendAnyMode() const noexcept;
        void SetLegacyBlendAnyMode(bool value) noexcept;
        // Whole track: legacy view to model (v2 load, the 15 column text).
        void ConvertLegacy();
        // Whole track: model to legacy view (v3 load).
        void DeriveLegacy();

        static Quaternion FromEulerDegrees(const editor::Vec3& rotation);
        static editor::Vec3 ToEulerDegrees(const Quaternion& orientation);

    private:
        void Sort();
        std::size_t IndexOf(std::uint32_t id) const;
        void SyncFromLegacy(std::size_t index, bool camera);
        // A track built with the marker API (not empty, not a converted v2
        // clip): the legacy API only touches the legacy view and the pose
        // there, never the blends nor the legacy flag.
        bool ModelTrack() const noexcept;
        // After a speed change the markers that follow are pushed to keep
        // their spacing, and kept at or before limitMs; false when they
        // cannot be (the caller restores the track).
        bool KeepSpacingAfter(std::size_t index, std::uint64_t limitMs);

    public:
        // The shake events track (camera_effects.h): only an input, an older
        // clip's CFXE section (or the director's and the bench's event
        // lines), folded into the markers by mshake::Migrate and empty after.
        fx::EffectTrack& LegacyEffects() noexcept { return effects_; }
        const fx::EffectTrack& LegacyEffects() const noexcept { return effects_; }
        // Something legacy to fold: an event, or a marker with a legacy shake.
        bool HasLegacyShake() const noexcept;
        // An id for a marker the migration adds (the
        // spacing rules do not apply: it is an Anchor the camera skips).
        // 0 when the track is full.
        std::uint32_t InsertMigrated(const Keyframe& marker);
        // The last migration's report and the track as
        // it was before it (for the proof, mshake::Verify; null once the
        // caller has checked and logged it).
        ShakeMigrationReport& Migration() noexcept { return migration_; }
        const ShakeMigrationReport& Migration() const noexcept { return migration_; }

    private:
        fx::EffectTrack effects_;
        ShakeMigrationReport migration_;
        std::vector<Keyframe> keyframes_;
        std::uint32_t nextId_ = 1;
        bool legacyBlendAnyMode_ = false;
    };

    // ------------------------------------------------------------------
    // Marker rules (pure).

    constexpr std::size_t kMaxMarkers = 512;
    constexpr std::uint32_t kMarkerFrameMs = 33;   // one frame at 30 Hz, times the speed
    constexpr std::uint32_t kMarkerMinSpacingMs = 2;

    float SpeedFactor(std::uint8_t speedIndex); // 0.05 .. 2.0
    // Minimum distance after a marker with this speed: 33 ms x speed,
    // rounded up, at least 2 ms.
    std::uint32_t MarkerSpacingMs(std::uint8_t speedIndex);
    // True when a marker fits at timeMs: under 512 markers (not counting
    // ignoreId, the one being moved) and at least MarkerSpacingMs from both
    // neighbours (the previous marker's speed; a moved marker's own speed
    // after it).
    bool CanPlace(const Track& track, std::uint64_t timeMs, std::uint32_t ignoreId = 0);
    // Where a moved marker may go: held between its neighbours with their
    // spacing, never past one (the clip's start or end included).
    // The marker's own time when it does not fit anywhere.
    std::uint64_t ClampMoveTime(const Track& track, std::uint32_t id, std::uint64_t timeMs);
    // The marker a scrub step from fromMs to toMs stops on (fine scrub
    // snap): the first one it crosses in its
    // own direction, i.e. the nearest to fromMs strictly between the two;
    // null when it crosses none.
    const Keyframe* FirstMarkerCrossed(const Track& track, std::uint64_t fromMs, std::uint64_t toMs);

    // A new marker inherits from the previous one (not the next). Keeps
    // fresh.id and fresh.timeMs. Returns true when the caller must take the
    // pose from the frame rendered at the new time (the previous marker is a
    // Free camera): the copied world pose is only a placeholder then.
    bool InheritFrom(Keyframe& fresh, const Keyframe& previous);
    // The defaults, keeping id and time (the last marker keeps its camera type).
    void ResetToDefault(Keyframe& marker, bool isLast);
    // The last (non Anchor) marker: when the one before it is a Free camera
    // with a blend, the last becomes Free with the same Look At and Attach
    // targets; otherwise it is left alone (the evaluator ignores it).
    // A last marker that becomes Free from another type (the clip's end, a
    // Game Camera) takes the world pose of the one before it (and its
    // relative pose when it takes its targets): its own pose was never set.
    // The view taken that way is "borrowed" (editedMask bit 63, with a hash
    // of the view in bits 32..62): while nobody edits it, it follows the
    // marker before it (a new, moved or deleted penultimate, or its new
    // framing), and when that marker stops blending the last one goes back
    // to Game Camera. Once the view is edited (pose, FOV, targets, framing,
    // paste) it is kept. Does nothing on a legacyBlendAnyMode track (a v2
    // clip blends into any type; the rule would rewrite its last marker).
    void ApplyLastMarkerRule(Track& track);
    constexpr std::uint64_t kEditedBorrowedView = 1ull << 63;
    // True while the marker holds an untouched view borrowed by the rule.
    bool HasBorrowedView(const Keyframe& marker);
    // The clip's start or end marker (first or last in the list)
    // of a marker track with two markers or more; never on a legacy track.
    // They cannot be deleted; moving them trims the clip.
    bool IsClipBound(const Track& track, std::uint32_t id);
    // A new marker of a track with clip bounds (IsClipBound) goes strictly
    // between them: nothing is placed outside the trimmed clip. True on a
    // track without bounds (fewer than two markers, a legacy track).
    // without bounds (fewer than two markers, a legacy track).
    bool WithinClipBounds(const Track& track, std::uint64_t timeMs);
    // The marker the last marker rule applies to (the last one that is not
    // an Anchor, with another such marker before it); never on a legacy
    // track. The editor cannot change its camera type: it takes the camera
    // of the marker before it.
    bool IsLastMarker(const Track& track, std::uint32_t id);
    // The same markers, every field: the editor's undo step and "unsaved
    // changes" only when a command changed something (a move to the time it
    // had, a speed already at 200 %, an add refused change nothing).
    bool SameMarkers(const Track& a, const Track& b);
    // Look At and Attach exclusion rules: a Look At on another target than
    // the attach one forces Flexible; Rigid with a Look At on another target
    // clears the Look At.
    void SetLookAt(Keyframe& marker, std::uint32_t target);
    void SetAttach(Keyframe& marker, std::uint32_t target);
    void SetAttachType(Keyframe& marker, AttachType type);
    // Copies `from` into `to` (not id, time or the edited mask). Only All
    // copies anything; EffectsOnly and AudioOnly return false.
    bool Paste(Keyframe& to, const Keyframe& from, CopyContext context);
    // The masked fields of an options command, validated (enums in range,
    // FOV 10..100, offsets -1..1, intensity 1..10, speeds 0..8, distance
    // scale 0..1, DOF 0..100, focus 0.5..1000 m) and through the exclusion
    // rules. False (nothing changed) when a field is out of range or a
    // legacy mode (Target, Follow) is asked for. Marks editedMask.
    bool ApplyOptions(Keyframe& marker, const editor::CameraKeyframeOptions& options);
    // The snapshot entry of a marker (both the legacy and the API 8 fields).
    void FillSnapshotKeyframe(const Keyframe& marker, editor::CameraKeyframe& out);

    // CAMR v2 to v3 (pure): the arriving easing and path move to the source
    // keyframe, Target and Follow become Free with Look At and Attach.
    void ConvertV2ToV3(std::vector<Keyframe>& keyframes);

    // ------------------------------------------------------------------
    // Marker tools, sequence tools adapted to the replay's markers. A "move"
    // is the run of
    // markers (Anchors skipped) around a marker in which each one blends
    // into the next, Free to Free, with no recording cut between: what plays
    // as one continuous camera move.

    struct Scene;

    struct MoveRange
    {
        int first = -1; // indices into Track::Keyframes(); -1 when the marker is not there
        int last = -1;
        bool Valid() const { return first >= 0 && last >= first; }
    };
    MoveRange MoveOf(const Track& track, std::uint32_t id, const std::vector<std::uint32_t>* cuts = nullptr);

    // A copy of the marker halfway to the next marker (any type), or
    // kDuplicateAfterLastMs after it when it is the last; inside the clip's
    // bounds and limitMs. The new id, 0 when it does not fit.
    constexpr std::uint64_t kDuplicateAfterLastMs = 2000;
    std::uint32_t DuplicateMarker(Track& track, std::uint32_t id, std::uint64_t limitMs = kNoTimeLimit);

    // The move's inner markers retimed between its first and last (both
    // kept): even times, or by distance (the path length between markers,
    // their views resolved at their own times): a constant speed. Anchors
    // inside the move keep their share of the time. False when the move has
    // fewer than three markers or the spacing does not fit.
    bool DistributeMove(Track& track, std::uint32_t id, bool byDistance, const Scene& scene);

    // The move's markers after its first, retimed to `percent` of their
    // distance in time from the first (25..400): the move plays slower or
    // faster on the same action. Markers after the move keep their times,
    // pushed only to keep the spacing, within limitMs. False when the move
    // ends on the clip's end marker (it cannot move) or nothing fits.
    bool StretchMove(Track& track, std::uint32_t id, std::uint32_t percent, std::uint64_t limitMs, const Scene& scene);

    // Every Free marker of the move (or of the track) attached to `target`
    // with `type`, each keeping the view it shows now (resolved at its own
    // time, then stored relative to the target). Target 0: the attachment
    // dropped, the view kept as a world pose ("bake"). The number of markers
    // changed.
    int LockMarkers(Track& track, std::uint32_t id, bool everyMarker, std::uint32_t target, AttachType type, const Scene& scene);

    // ------------------------------------------------------------------
    // Evaluator (pure).

    enum class EaseCurve : std::uint8_t
    {
        Linear = 0,
        In = 1,
        Out = 2,
        InOut = 3
    };

    // The ease of the segment source -> next, from the chain of blends (a
    // six row table): a chain accelerates only at its start and
    // brakes only at its end. `anyMode` counts ease on any camera type.
    EaseCurve ChainEase(const Keyframe* previous, const Keyframe& source, const Keyframe* next, bool anyMode = false);
    float ApplyEase(EaseCurve curve, float phase);
    // Shake gain from the 1..10 intensity (camera_shake.h): ((i + 1) / 6)^2, 1.0 at 5.
    float ShakeAmplitude(std::uint8_t intensity);

    struct ShakePlan
    {
        ShakeType type = ShakeType::None;
        float amplitude = 0.0f;
        double timeMs = 0.0; // the warped shake time (deterministic)
        // A Custom shake's numbers at t (lerped
        // inside a blend into another Custom marker, by the raw phase), its
        // phase in cycles since the run of Custom markers started (the frequency
        // integrated, the speed coupling left to EvaluateMarkers) and that start.
        editor::ShakeCustom custom = editor::DefaultShakeCustom();
        double cycles = 0.0;
        std::uint64_t runStartMs = 0;
    };

    struct SegmentPlan
    {
        bool valid = false;
        int previous = -1;  // indices into Track::Keyframes(), -1 = none
        int current = -1;
        int next = -1;
        int nextNext = -1;  // legacyCurve's outer neighbour
        bool blending = false;
        float rawPhase = 0.0f;
        float phase = 0.0f; // after the chain ease
        EaseCurve curve = EaseCurve::Linear;
        bool smooth = false;
        float smoothWeight = 0.0f; // spring stiffness weight: k = lerp(100, 10, weight)
        float smoothMix = 0.0f;    // spring output mix (the undo ramp from 80 % of the last segment)
        int dofIndex = -1;
        // The DOF marker the blend
        // arrives at (the segment that starts at `next` takes its DOF from
        // it; the last marker's from the one before it). -1 without a blend.
        int dofNextIndex = -1;
        ShakePlan shake;
        // Set by EvaluateMarkers when the Smooth springs ran: the
        // attach target they ran relative to (0: world space) and how far
        // the sprung camera sits from the raw one (metres).
        std::uint32_t smoothAnchor = 0;
        float smoothLagMeters = 0.0f;
    };

    // The plan of the instant: current marker (the last one at or before t
    // that is not an Anchor; the last marker only when the one before it
    // blends into it, or always on a legacyBlendAnyMode track), destination,
    // blend (Free to Free only, unless legacyBlendAnyMode), eased phase,
    // Smooth weights, DOF marker and shake.
    SegmentPlan Plan(const Track& track, std::uint64_t timeMs);

    // The clip's fades (the markers' transition): how black the picture is
    // at t, 0 clear to 1 black. The first (non Anchor) marker with a fade
    // goes from black at its time to clear transitionMs later; the last one
    // with a fade goes from clear transitionMs before it to black at its
    // time (and stays black after). A track with one marker only fades in.
    float FadeAmount(const Track& track, std::uint64_t timeMs);

    // An entity at a time, from the caller (engine free). Rotation in
    // degrees as GET_ENTITY_ROTATION order 2 gives it.
    struct EntityState
    {
        editor::Vec3 position{};
        float heading = 0.0f;
        float pitch = 0.0f;
        float roll = 0.0f;
        bool hasHead = false; // peds and horses: the head bone
        editor::Vec3 head{};
    };
    // False when the entity does not exist at that time.
    using EntityLookup = std::function<bool(std::uint32_t id, std::uint64_t timeMs, EntityState& state)>;
    // A bone of an entity at a time (camera_bones.h reference):
    // position the bone's world position, heading/pitch/roll its world
    // rotation (same convention as EntityState), head the bone position.
    // False when the entity or the bone is not there.
    using BoneLookup = std::function<bool(std::uint32_t id, std::uint32_t bone, std::uint64_t timeMs, EntityState& state)>;

    // The memo of a Custom shake's frequency coupling (speed -> frequency),
    // the integral of
    // hz x speedHz x the camera's normalised speed over the curve on a fixed
    // grid from the run's start. Only a memo: EvaluateMarkers gives the same
    // value with or without it; it is rebuilt when the markers, the events
    // or the run change. The caller owns one (script.cpp) and points the
    // Scene at it.
    constexpr std::uint32_t kShakePhaseGridMs = 100;
    struct ShakePhaseCache
    {
        std::uint64_t signature = 0;
        std::uint64_t runStartMs = ~0ull;
        int source = -1; // 0 markers, 1 events
        std::vector<editor::Vec3> points; // the camera at runStart + n x grid
        std::vector<std::uint8_t> known;  // points[n] evaluated
        std::vector<double> sums;         // the coupling's cycles up to point n
        std::uint32_t rebuilds = 0;       // evidence
        void Clear()
        {
            signature = 0;
            runStartMs = ~0ull;
            source = -1;
            points.clear();
            known.clear();
            sums.clear();
        }
    };

    struct Scene
    {
        EntityLookup lookup;
        const std::vector<GameSample>* game = nullptr; // the recorded game camera
        float aspect = 16.0f / 9.0f;                   // screen width / height
        std::uint32_t mainCharacter = 0;               // packed id of the player's puppet (presets)
        // The recording cuts (Recording::cuts): the game camera is
        // not interpolated across one and a marker blend that would cross
        // one does not blend (the camera holds the source marker, like No
        // Blend, until the next marker).
        const std::vector<std::uint32_t>* cuts = nullptr;
        // LockCutsceneCamera: true at the times the markers give
        // way to the recorded game camera (a recorded cutscene).
        std::function<bool(std::uint64_t timeMs)> gameOnly;
        // The Custom shake's phase memo (may be null).
        ShakePhaseCache* shakePhase = nullptr;
        // The Spline defaults (a marker's tuning
        // overrides some fields) and whether the camera weight applies: only
        // while the clip plays or exports (paused, a saved view would carry
        // the weight and get it again on play), and for the overlay's path
        // (what plays).
        editor::SplineTuning splineDefaults = editor::DefaultSplineTuning();
        bool splineWeight = false;
        // The bones of the targets (may be
        // empty: a bone reference then falls back to the entity's root).
        BoneLookup boneLookup;
    };

    // The attach or Look At target of a marker at t, through its
    // bone when it has one. A bone that is not there falls back to the
    // entity's root (boneMissing set when given); false only when the
    // entity itself is not there.
    bool LookupTarget(const Scene& scene, std::uint32_t id, std::uint32_t bone, std::uint64_t timeMs, EntityState& state,
        bool* boneMissing = nullptr);
    inline bool LookupAttach(const Keyframe& marker, std::uint64_t timeMs, const Scene& scene, EntityState& state,
        bool* boneMissing = nullptr)
    {
        return marker.attachTarget != 0 && LookupTarget(scene, marker.attachTarget, marker.attachBone, timeMs, state, boneMissing);
    }
    inline bool LookupLookAt(const Keyframe& marker, std::uint64_t timeMs, const Scene& scene, EntityState& state,
        bool* boneMissing = nullptr)
    {
        return marker.lookAtTarget != 0 && LookupTarget(scene, marker.lookAtTarget, marker.lookAtBone, timeMs, state, boneMissing);
    }

    // A bone's world state from its entity's recorded root
    // (position, quaternion x, y, z, w) and the bone's object space
    // transform (translation, quaternion x, y, z, w), as the recorded pose
    // stores it: world = root x bone. The EntityState carries the bone's
    // position (also as the head) and its world rotation.
    EntityState EntityFromBone(const editor::Vec3& rootPosition, const float rootQuaternion[4], const float boneTranslation[3],
        const float boneQuaternion[4]);

    // The marker fields of a CameraMarkerExtra command (only
    // its `fields`), validated: bone references in range (camera_bones.h),
    // near clip 0 or 0.01..1 m, f-stop 0 or 0.5..32, lens 0 or 10..600 mm,
    // attach pose finite (position within 1000 m, rotation wrapped). False
    // (nothing changed) on a value out of range. Marks the edited mask
    // (CameraOptionExtra) and marks the view as edited.
    bool SetMarkerExtra(Keyframe& marker, const editor::CameraMarkerExtra& extra);
    // The snapshot's extra entry of a marker.
    void FillSnapshotExtra(const Keyframe& marker, editor::CameraMarkerExtra& out);
    // The near clip a marker asks for (its own, else kNearClipDefault).
    float MarkerNearClip(const Keyframe& marker);
    // The near clip of the instant: the segment's marker, inside a blend
    // lerped to the next one by the eased phase. kNearClipDefault without
    // a usable plan.
    float NearClipAt(const Track& track, const SegmentPlan& plan);

    struct LivePose
    {
        Pose pose;
        bool hasLookAt = false;
        editor::Vec3 lookAtPoint{};
        editor::Vec2 lookAtOffset{};
        float lookAtRoll = 0.0f;
        bool targetMissing = false; // an attach or look at target was not found
    };

    constexpr float kLookAtAimHeight = 0.3f; // above the origin when there is no head

    // The camera rotation (degrees, order 2) at cameraPosition that shows
    // `point` at `screenOffset` (-1..1, +x right, +y up) with `roll` around
    // the view axis. `fov` is vertical, `aspect` width / height.
    editor::Vec3 LookAtWithScreenOffset(
        const editor::Vec3& point,
        const editor::Vec3& cameraPosition,
        const editor::Vec2& screenOffset,
        float fov,
        float roll,
        float aspect);
    // Where `point` lands on screen (-1..1) for a camera; false behind it.
    bool ProjectToScreen(const editor::CameraState& camera, const editor::Vec3& point, float aspect, editor::Vec2& screen);

    // The live pose of one marker at t: Game from the recorded camera, Free
    // from its world pose, Attach from its target's matrix (Rigid, Default,
    // Flexible), Look At with framing and roll, presets around the main
    // character. A missing target keeps the stored world pose (attach) or
    // the stored orientation (look at).
    LivePose Resolve(const Keyframe& marker, std::uint64_t timeMs, const Scene& scene);
    // Stores the relative attach pose (relPosition, relRotation) that puts
    // the camera at `world` for the target state, per the marker's attach type.
    void CaptureAttachPose(Keyframe& marker, const editor::CameraState& world, const EntityState& target);
    // Stores a world view in a marker so that Resolve at the
    // marker's time gives that view back: the pose (FOV held 10..100), the
    // attach pose relative to the target at that time, and the Look At
    // framing (where the target sits on screen) and roll. What Edit
    // Camera, "Use this view" and a new marker (the frame shown) store.
    // The view counts as edited (clears a borrowed view).
    void SetMarkerView(Keyframe& marker, const editor::CameraState& view, const Scene& scene);

    // Preset camera layout: offset from the main character in its frame
    // (metres at scale 1) and the point it looks at. [hypothesis values]
    bool PresetLayout(CameraMode mode, editor::Vec3& offset, editor::Vec3& lookAt);

    // The Smooth blend's springs (critically damped): position per axis,
    // pitch, roll, heading, FOV. Owned by the caller, outside the track.
    // Reset whenever playback is not going forward (pause, seek, step,
    // back, marker edit): the next Step returns its target.
    // When the segment's two markers are attached to the same
    // target, EvaluateMarkers runs the springs in that target's attach frame
    // (anchor), so the camera follows the target exactly and the springs
    // only round the blend; a change of anchor resets them.
    struct SpringState
    {
        bool primed = false;
        float value[7] = {};
        float velocity[7] = {};
        std::uint32_t anchor = 0;
        std::uint32_t anchorBone = 0; // the anchor's bone (camera_bones.h)
        AttachType anchorType = AttachType::Flexible;
        // A change of anchor re-expresses the springs' value and velocity in
        // the new frame (the camera stays where it was) instead of Reset() (a
        // jump of smooth_lag_m in one frame). False: reset
        // (ReplayCamSmoothReframe=0). Kept by Reset().
        bool reframe = true;
        std::uint32_t reframes = 0; // anchor changes re-expressed (evidence)
        std::uint32_t resets = 0;   // anchor changes that reset (reframe off, or the old anchor not found)

        void Reset();
        editor::CameraState Step(const editor::CameraState& target, float stiffness, float dtSeconds);
    };

    struct ShakeDelta
    {
        editor::Vec3 rotation{}; // degrees: x pitch, y roll, z heading
        editor::Vec3 position{}; // world metres
    };
    // Procedural shake (camera_shake.h: fractal noise, gait and impulse
    // profiles per type; Explosion one shot from the chain start).
    // Deterministic and continuous in shakeTimeMs.
    ShakeDelta ShakeOffset(ShakeType type, float amplitude, double shakeTimeMs);

    // The dt EvaluateMarkers wants is clip time (the real frame
    // time x the playhead rate), so the Smooth rounding has the same shape
    // at every marker speed (5 to 200 %). The engine's frame time follows
    // SET_TIME_SCALE, which the replay only lowers below 100 % and which
    // the time compensation does not touch.
    constexpr float ClipStepSeconds(float realSeconds, double playheadRate)
    {
        return realSeconds > 0.0f && playheadRate > 0.0 ? static_cast<float>(realSeconds * playheadRate) : 0.0f;
    }

    // The marker camera at t: the plan, the live pose(s), the blend
    // (lerp/slerp, or the Look At recomputed when both look at the same
    // target; legacyCurve keeps the Hermite path), the Smooth springs
    // (when `spring` is given and dtSeconds > 0; reset otherwise) and the
    // shake on top. False when the track has no usable marker.
    bool EvaluateMarkers(
        const Track& track,
        std::uint64_t timeMs,
        const Scene& scene,
        editor::CameraState& camera,
        SpringState* spring = nullptr,
        float dtSeconds = 0.0f,
        SegmentPlan* planOut = nullptr);

    // The shake's gain from the
    // camera's speed on the marker curve at t (speed from the curve 40 ms
    // around t, no springs, no shake; scrub equals play). Speed: 1 standing
    // to 2 at kShakeMotionFullSpeed. SpeedStill: also 0 while it stands still,
    // released over about 250 ms after it stops. Off: 1.
    // The marker curve alone at t (no springs, no shake):
    // the path the overlay draws and the speed the shake reads.
    bool EvaluateMarkersRaw(const Track& track, std::uint64_t timeMs, const Scene& scene, editor::CameraState& camera,
        SegmentPlan* planOut = nullptr);

    constexpr std::int64_t kShakeMotionProbeMs = 40;
    constexpr float kShakeMotionFullSpeed = 15.0f;  // m/s
    constexpr float kShakeMotionStillSpeed = 0.1f;  // m/s
    constexpr float kShakeMotionCutSpeed = 200.0f;  // m/s: faster is a cut, not a move
    float ShakeMotionGain(const Track& track, std::uint64_t timeMs, const Scene& scene, ShakeMotion motion);

    // A Custom shake's numbers into a marker's legacy shake (tests of the
    // legacy model and the migration only).
    bool SetShakeCustom(Keyframe& marker, const editor::ShakeCustom& custom);
    // A marker's shake (validated, held to
    // its ranges; marks it edited). False when a number is not finite or a
    // field is out of its set.
    bool SetShake(Keyframe& marker, const editor::MarkerShake& shake);
    // The camera's speed from the marker curve around t, and the Stop When
    // Still gate (what the shakes read; scrub equals play).
    float ShakeCurveSpeed(const Track& track, const Scene& scene, std::int64_t at);
    float ShakeStillGate(const Track& track, const Scene& scene, std::int64_t now, float speedNow);
    // A marker's Spline tuning (held to its
    // ranges; marks it edited). False when a number is not finite or the
    // pacing is unknown.
    bool SetSplineTuning(Keyframe& marker, const editor::SplineTuning& tuning);
    // The cycles the speed coupling adds to a Custom shake's phase at t: the
    // integral from the run's start of hz x speedHz x min(1, speed /
    // speedRef), the camera's speed from the marker curve on the
    // kShakePhaseGridMs grid (a jump faster than kShakeMotionCutSpeed counts
    // as standing). source 0: the legacy markers' Custom shake, 1: the legacy
    // events'; 2: the markers' shake (MarkerShake).
    double CustomCouplingCycles(const Track& track, const Scene& scene, int source, std::uint64_t runStartMs,
        std::uint64_t timeMs);

    // ------------------------------------------------------------------
    // Depth of field of a marker (pure; ApplyMarkerDof sends it).
    // Custom: f-stop from the intensity (f/22 at 0 to f/0.5 at 100) and a
    // focus plane: Manual at its distance; Target on the chosen character's
    // head (none chosen: the main character, "Player"); Auto on the Look At
    // target, else the attach target, else the main character when on
    // screen, else the engine's autofocus. The distance is the depth along
    // the view axis (the focus plane is perpendicular to it), not the
    // straight line. A target that is not there falls back to the engine's
    // autofocus, never to a fixed 10 m that would blur the character.
    enum class FocusSource : std::uint8_t
    {
        Default = 0,  // DOF Default: the camera's own (no block)
        Sharp = 1,    // DOF None: f/22
        Manual = 2,
        LookAt = 3,
        Attach = 4,
        Target = 5,
        MainCharacter = 6,
        Engine = 7,   // the engine's autofocus
        // The last focus distance of the shot, held when the entity left the
        // frame or is not there (the engine's autofocus would switch the
        // camera's focus mode).
        Held = 8
    };
    const char* FocusSourceName(FocusSource source);

    constexpr float kFocusMinMeters = 0.2f;  // the photo mode's focus range starts here
    constexpr float kFocusMaxMeters = 1000.0f;
    constexpr float kDofSharpFStop = 22.0f;

    struct FocusPlan
    {
        FocusSource source = FocusSource::Default;
        std::uint32_t entity = 0;       // the entity in focus (packed id), 0 none
        float fstop = kDofSharpFStop;
        float distance = 10.0f;         // focus plane depth (metres), what _SET_CAM_FOCUS_DISTANCE takes
        float straightMeters = 0.0f;    // camera to the focus point, straight line
        editor::Vec3 point{};           // the focus point (head, or origin + aim height)
        bool targetMissing = false;     // the asked entity was not found
        // The lens for the DOF in mm, 0 when
        // the caller derives it from the FOV (DofLensMm).
        float lensMm = 0.0f;
        float intensity = 50.0f;        // the marker's intensity (the lens multiplier)
    };

    // The DOF lens (mm, 35 mm film: the engine turns a lens into a FOV with
    // 12 mm over tan(fov / 2), 0x428370) for a FOV: the lens of that FOV
    // times a focal length multiplier, 2.0 at intensity 0 to 2.5 at 100, or
    // the marker's own lens when set. Held to kDofLensMin..kDofLensMax.
    float DofLensMm(float fovDegrees, float intensity, float lensOverride);
    constexpr float kDofLensMultiplierMin = 2.0f;
    constexpr float kDofLensMultiplierMax = 2.5f;
    // The depth in focus of a thin lens (information for the
    // editor): near and far limits around the focus distance for an
    // f-stop and a lens, with a 0.03 mm circle of confusion. far 0 when it
    // reaches infinity.
    void InFocusRange(float fstop, float lensMm, float focusMeters, float& nearMeters, float& farMeters);

    float DofFStop(float intensity);
    // Depth of a point along the camera's view axis; the straight distance
    // when it is behind the camera. Held to kFocusMinMeters..kFocusMaxMeters.
    float FocusDepth(const editor::CameraState& camera, const editor::Vec3& point);
    // `marker` is the segment's DOF marker (SegmentPlan::dofIndex), null for
    // none (Default). The entity states are read at timeMs.
    FocusPlan PlanFocus(const Keyframe* marker, const editor::CameraState& camera, const Scene& scene, std::uint64_t timeMs);

    // The DOF across a blend: the camera is kept and the DOF parameters are
    // interpolated inside a blend; nothing here asks the caller to recreate
    // the camera.
    //
    // Auto focus on the main character with hysteresis: it takes the
    // character when its head is inside kFocusEnterFrame of the screen
    // (-1..1), keeps it out to kFocusLeaveFrame, and lets it go only after
    // graceMs outside that. Let go (or an entity not there), the shot keeps
    // its last focus distance (FocusSource::Held) instead of switching to
    // the engine's autofocus; the autofocus only when the shot never had a
    // distance. The memory lives for one shot: the caller resets it on a
    // camera cut.
    constexpr float kFocusEnterFrame = 0.9f;
    constexpr float kFocusLeaveFrame = 1.1f;
    constexpr std::uint32_t kFocusGraceMs = 500;

    struct FocusConfig
    {
        bool hysteresis = true;        // ReplayCamFocusHysteresis
        bool holdLast = true;          // with hysteresis: Held instead of Engine
        bool blend = true;             // ReplayCamDofBlend: interpolate inside a blend
        float smoothOmega = 12.0f;     // ReplayCamFocusSmooth: spring on the focus distance (rad/s), 0 off
        float enter = kFocusEnterFrame;
        float leave = kFocusLeaveFrame;
        std::uint32_t graceMs = kFocusGraceMs;
    };

    struct FocusHold
    {
        std::uint32_t marker = 0;      // the marker id this state belongs to (0: free slot)
        bool locked = false;           // the main character holds the focus
        bool outside = false;
        std::uint64_t outsideSinceMs = 0;
    };

    struct FocusMemory
    {
        FocusHold hold[2];             // the blend's two markers
        bool hasDistance = false;      // a distance was planned in this shot
        float lastDistance = 10.0f;
        // The distance spring (log metres), reset with the shot.
        bool springPrimed = false;
        float springValue = 0.0f;
        float springVelocity = 0.0f;
        // Evidence: source changes inside a shot between an entity and the
        // engine's autofocus (flicker), and all source changes.
        FocusSource lastSource = FocusSource::Default;
        std::uint32_t engineFlips = 0;
        std::uint32_t sourceChanges = 0;

        void ResetShot();              // a camera cut: everything but the evidence counters
        FocusHold& HoldFor(std::uint32_t markerId);
    };

    // PlanFocus with the memory (null: PlanFocus above, no hysteresis).
    FocusPlan PlanFocus(const Keyframe* marker, const editor::CameraState& camera, const Scene& scene, std::uint64_t timeMs,
        FocusMemory* memory, const FocusConfig& config);

    struct BlendedFocus
    {
        FocusPlan plan;            // what to send (source: the side that dominates, entity: its entity)
        bool blended = false;      // interpolated between two DOF markers
        float phase = 0.0f;
        FocusSource from = FocusSource::Default;
        FocusSource to = FocusSource::Default;
    };

    // The DOF of the instant: PlanFocus of the segment's DOF marker and,
    // inside a blend (config.blend), of the one it arrives at, interpolated
    // with the blend's eased phase: f-stop in log space (the intensity's
    // own scale), focus distance linearly. Default and None count as f/22
    // at the other side's distance (Default only when both are Default);
    // an autofocus side takes the other side's distance.
    BlendedFocus BlendFocus(const Track& track, const SegmentPlan& plan, const editor::CameraState& camera, const Scene& scene,
        std::uint64_t timeMs, FocusMemory* memory, const FocusConfig& config);

    // The focus distance through the spring (critically damped, in log
    // metres so 1 m to 2 m moves like 10 m to 20 m). dt 0 or no spring:
    // the distance itself, the spring primed on it. Only for a distance
    // source (not Default, Sharp or Engine).
    float SmoothFocusDistance(FocusMemory& memory, float distance, float dtSeconds, float omega);

    // ------------------------------------------------------------------
    // A camera that follows someone follows the RECORDED root of the target
    // at the playhead, not the live puppet read by natives (the physics push
    // and the skeleton of the frame before would show as a tweak in the
    // shot). The entity's recorded engine matrix (rotation as a quaternion x,
    // y, z, w; right, forward, up are its columns) into the EntityState the
    // evaluator takes: heading, pitch and roll with the same convention as
    // AttachFrame (rotation order 2), and the head when given (world metres).
    EntityState EntityFromRecorded(const editor::Vec3& position, const float quaternion[4], const editor::Vec3* head);

    // ------------------------------------------------------------------
    // Streaming ahead of a cut far away. The player is hidden and the world
    // streams around the replay camera (SET_FOCUS_POS_AND_VEL every tick), so
    // a cut to a marker far away would show the new place before it
    // streamed: leadMs before such a cut, the caller loads the scene at the
    // next marker's view. A cut here is a boundary the camera does not blend
    // into (the current segment does not blend).
    constexpr float kPrefetchMinMeters = 30.0f;  // a free camera's usual leash around the player
    constexpr std::uint32_t kPrefetchLeadMs = 1500;

    struct PrefetchTarget
    {
        bool want = false;
        int marker = -1;               // index into Track::Keyframes()
        std::uint64_t cutMs = 0;       // when the cut comes
        float meters = 0.0f;           // camera now to the next view
        editor::CameraState view{};    // the next marker's view at its time
    };
    PrefetchTarget PlanPrefetch(const Track& track, const SegmentPlan& plan, std::uint64_t timeMs,
        const editor::CameraState& camera, const Scene& scene, std::uint32_t leadMs = kPrefetchLeadMs,
        float minMeters = kPrefetchMinMeters);

    // ------------------------------------------------------------------
    // The recorded game camera while the player looks through
    // a first person scope from third person play (binoculars, a rifle
    // scope, the camera kit). The rendered camera is then inside the
    // character's head; the clip keeps a third person camera instead: the
    // last third person camera's offset from the player (in its heading
    // frame), pitch, roll and FOV, turned by how much the scope view turned
    // since it started, so it looks where the character looks.
    struct ScopeAnchor
    {
        bool valid = false;         // a third person camera was seen
        editor::Vec3 offset{};      // camera minus player, in the camera's heading frame
        float pitch = 0.0f;
        float roll = 0.0f;
        float heading = 0.0f;
        float fov = 50.0f;
        bool inScope = false;
        float scopeHeading = 0.0f;  // the scope view when it started
        float scopePitch = 0.0f;
    };
    // Used without an anchor (a recording that starts in a scope): over
    // the right shoulder, metres in the view's heading frame.
    constexpr editor::Vec3 kScopeDefaultOffset{0.45f, -2.4f, 0.55f};
    // A rendered camera nearer the head than this is not a third person
    // one (the scope's blend in and out).
    constexpr float kScopeHeadClearance = 0.8f;

    // A third person frame: the anchor follows it (leaves the scope).
    void TrackThirdPerson(ScopeAnchor& anchor, const editor::CameraState& rendered, const editor::Vec3& player);
    // A scope frame: the third person camera to record instead.
    editor::CameraState ThirdPersonDuringScope(ScopeAnchor& anchor, const editor::CameraState& scope, const editor::Vec3& player);
}
