#pragma once

// Rope subsystem: records the engine ropes attached to recorded entities
// and replays them on the puppets' own engine ropes.
//
// Recording: each rope of the engine rope manager with a recorded entity in
// its attachments becomes a RopeRecord; every frame its held flag and
// attachment entries are noted as events and its vertices sampled at the
// capture rate.
//
// Playback: each record finds the puppets' engine rope with the same vertex
// count attached, in the same entry and bone, to the puppet of a recorded
// attachment (a puppet horse has its reins). Every entry then gets the
// entity it had at the playhead through the engine's attachment setter,
// with the entry's active word in step (an active entry without an entity
// pins the rope to the world origin), and the held flag as recorded; the
// recorded window is also written into that rope every frame, so it has the
// recorded shape from the first frame, in pause and after a seek. A record
// with no such rope (a thrown lasso) is played on a rope of its own: a rope
// of the recorded type and vertex count plus the vertex setter the game's
// scripts use on their own ropes, every frame. Only the rope's drawn window
// is recorded (the engine keeps parked and stale vertices in the same array
// and draws a contiguous window of it, see rope_observer.h), resampled to
// the record's vertex count so the whole own rope is that window.
//
// The recorder maps engine entity addresses to replay ids and the player
// maps replay ids to puppet addresses through the two small interfaces;
// the log stream is the caller's.

#include "log_policy.h"
#include "rope_observer.h"
#include "rope_track.h"

#include <algorithm>
#include <cstdint>
#include <ostream>
#include <utility>
#include <vector>

namespace replay::ropes
{
    // Recording: the replay id of an engine entity address (invalid when
    // the entity is not recorded).
    struct EntityIndex
    {
        virtual ~EntityIndex() = default;
        virtual entity::ReplayId IdOf(std::uint64_t address) const = 0;
        // The recorded ped standing within radius of the point (invalid
        // when none): a rope in a recorded ped's hand (a lasso before it
        // catches anything) has no recorded attachment yet.
        virtual entity::ReplayId NearestRecordedPed(const float xyz[3], float radius) const = 0;
        // World position of the recorded ped's right hand (evidence, and the
        // lasso rule, RopeCandidate::inHand).
        virtual bool HandOf(entity::ReplayId id, float out[3]) const = 0;
        // The recording's focus points (the player, the gameplay camera), up to
        // 2; 0 when unknown (a full cap then refuses).
        virtual std::size_t Focus(float out[2][3]) const
        {
            (void)out;
            return 0;
        }
        // An object at an end of a recorded rope that is not recorded (the
        // fishing bobber) becomes a prop with a world track owned by the rope's
        // recorded entity `owner` (a ped, or the weapon whose holder owns it).
        // Returns its id, invalid when it is not an object or cannot be recorded;
        // `status` says why (evidence).
        virtual entity::ReplayId AdoptEndObject(std::uint64_t address, entity::ReplayId owner, const char*& status) const
        {
            (void)address;
            (void)owner;
            status = "unsupported";
            return {};
        }
    };

    // Playback: the engine address of the puppet playing a replay id (0
    // when there is none yet).
    struct PuppetIndex
    {
        virtual ~PuppetIndex() = default;
        virtual std::uint64_t AddressOf(entity::ReplayId id) const = 0;
        // World position of the puppet ped's right hand (evidence).
        virtual bool HandOf(entity::ReplayId id, float out[3]) const = 0;
    };

    class Ropes
    {
    public:
        void BeginRecording();
        // One recording frame at timeMs (the recorder's clock); vertices
        // are sampled every intervalMs.
        void Sample(const EntityIndex& entities, std::uint32_t timeMs, std::uint32_t intervalMs,
            std::ostream& log);

        void BeginPlayback();
        // The reins guard's rope set emptied (a script restart forgets the
        // playback).
        static void ClearFollowedRopes();
        // Deletes the ropes playback created for records without an engine
        // counterpart.
        void EndPlayback();
        // ROPE_PIN_CHECK, before EndPlayback.
        void LogPinCheck(std::ostream& log) const;
        // A script restart with a playback open: the ropes playback created are
        // the old world's handles: forgotten, not deleted.
        void ForgetPlayback()
        {
            ClearFollowedRopes();
            m_own.clear();
            m_matches.clear();
        }
        // One playback frame at the playhead.
        void Play(const PuppetIndex& puppets, std::uint32_t timeMs, std::ostream& log);

        const std::vector<RopeRecord>& Records() const { return m_records; }
        // The replay buffer's trim (replay_buffer.h TrimBefore): the records of
        // ropes gone before keepFrom are freed, the others keep from keepFrom.
        RopeTrimStats DropBefore(std::uint32_t keepFrom)
        {
            const RopeTrimStats stats = DropRopeRecordsBefore(m_records, keepFrom);
            m_freedRecords += static_cast<std::uint32_t>(stats.records);
            if (stats.records)
            {
                // The evidence budgets of the freed ropes go too.
                m_budgets.erase(std::remove_if(m_budgets.begin(), m_budgets.end(),
                                    [this](const LogBudget& budget) {
                                        return std::none_of(m_records.begin(), m_records.end(),
                                            [&](const RopeRecord& record) { return record.engineId == budget.engineId; });
                                    }),
                    m_budgets.end());
            }
            return stats;
        }
        // Ropes refused because the engine's rope pool was live in the records
        // (distinct engine ids) and records freed by the buffer, this recording.
        std::uint32_t RefusedFull() const { return static_cast<std::uint32_t>(m_refusedFull.size()); }
        std::uint32_t FreedRecords() const { return m_freedRecords; }
        // The most records live at once (evidence: against kEngineRopePoolCapacity),
        // and vertex samples not stored (still ropes).
        std::uint32_t MaxLiveRecords() const { return m_maxLive; }
        std::uint64_t StillSamples() const { return m_stillSamples; }
        // The engine rope is one the playback plays (its own rope, or the puppet
        // rope a record followed at the last match).
        bool Plays(std::uint32_t ropeId) const;

    private:
        void LogFollow(const rope::RopeInfo& rope, const char* side, std::uint32_t timeMs, std::ostream& log);
        // A record with no engine rope to follow (a thrown lasso: puppets
        // never throw) gets its own script rope, created where the recorded
        // one starts and shaped every frame by the recorded vertices.
        void PlayOwn(const PuppetIndex& puppets, RopeRecord& record, std::uint32_t timeMs, std::ostream& log);
        void DeleteOwn(std::uint32_t engineId);
        // The recorded window written into the followed engine rope (see
        // DecideRecordedShape in rope_track.h).
        void ApplyRecordedShape(const PuppetIndex& puppets, const RopeRecord& record, const rope::RopeInfo& target,
            std::uint32_t timeMs, std::ostream& log);

        struct OwnRope
        {
            std::uint32_t engineId = 0;
            int rope = 0;
            bool visible = true;
            std::uint32_t nextEvidenceMs = 0;
            bool sameLayout = false; // recorded type and vertex count, set 1:1
        };
        std::vector<OwnRope> m_own;

        std::vector<RopeRecord> m_records;
        // Evidence: after an attachment change, the rope header, its first
        // entries and end vertices for a few frames.
        std::vector<std::pair<std::uint32_t, int>> m_follow;
        std::uint32_t m_evidenceLines = 0; // recording evidence, all ropes (safety cap)
        std::uint32_t m_attachLogs = 0;    // playback evidence, all ropes (safety cap)
        PinTally m_pinTally;
        // Evidence budgets per rope: attachment changes have their own budget per
        // rope, state lines are written when the state changes (plus a slow
        // heartbeat), and playback attachment and shape lines are counted per rope,
        // so a few busy ropes cannot hide every later attachment change.
        struct LogBudget
        {
            std::uint32_t engineId = 0;
            std::uint32_t stateLines = 0;
            std::uint32_t attachLines = 0;
            std::uint32_t shapeLines = 0;
            bool hasState = false;
            std::uint32_t flags = 0;
            std::uint32_t mode = 0;
            std::uint32_t drawStart = 0;
            std::uint32_t drawCount = 0;
            std::int32_t lengthCm = 0;
            std::uint32_t lastStateMs = 0;
            // Recorded shape on a followed rope (evidence).
            int shapeStatus = -1;
            std::uint32_t nextShapeMs = 0;
        };
        std::vector<LogBudget> m_budgets;
        LogBudget& Budget(std::uint32_t engineId);
        // Playback entries the engine keeps writing back (see DecideSlotWrite),
        // per followed rope and entry.
        std::vector<SlotGuard> m_slotGuards;
        SlotGuard& GuardFor(std::uint32_t engineId, std::uint32_t slot);
        // Rope end objects already offered to the recorder (once per engine
        // address and recording), and their evidence cap.
        std::vector<std::uint64_t> m_endObjectsTried;
        std::uint32_t m_endObjectLogs = 0;

        // The cap evidence (POSE_V2_ROPE_CAP_SUMMARY).
        std::vector<std::uint32_t> m_refusedFull;
        std::uint32_t m_freedRecords = 0;
        std::uint32_t m_maxLive = 0;
        std::uint64_t m_stillSamples = 0;
        std::uint32_t m_capLogMs = 0;
        bool m_capLogged = false;
        // Ropes the recorder did not record, not looked at again every frame.
        RejectedRopes m_rejected;
        // POSE_V2_ROPE_FOLLOW (a full dump of the rope's entries, ~3 KB a line)
        // with a cap per session, and only with Debug=1.
        replay::log::LogLimit m_followLines;
        // Playback: the engine rope each record followed and the entry that
        // matched, checked again with two reads and matched from scratch every
        // kRematchMs, instead of testing every record against every rope each
        // frame.
        struct Match
        {
            std::uint32_t engineId = 0; // the record
            std::uint32_t ropeId = 0;   // the puppet's engine rope
            std::uint32_t slot = 0;
            std::uint64_t address = 0;  // the puppet in that entry
            std::int32_t bone = -1;
            unsigned long long rematchAt = 0; // GetTickCount64
        };
        std::vector<Match> m_matches;
        // Own rope creations per playback tick (each is a native rope creation
        // plus a fallback; several on one tick cause a frame spike).
        std::uint32_t m_ownCreatesLeft = 0;
    };
}
