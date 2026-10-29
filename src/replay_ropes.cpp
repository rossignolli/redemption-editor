#include "replay_ropes.h"

#include "log_format.h"
#include "profiler.h"
#include "reins_guard.h"

#include "natives.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <string>

namespace replay::ropes
{
    namespace
    {
        // Safety caps over all ropes; the budgets are per rope (LogBudget).
        constexpr std::uint32_t kEvidenceLineCap = 4000;
        constexpr std::uint32_t kAttachLogCap = 4000;
        constexpr std::uint32_t kStateLinesPerRope = 48;
        constexpr std::uint32_t kAttachLinesPerRope = 96;  // recording: ENTRY + ATTACH pairs
        constexpr std::uint32_t kPlaybackAttachLinesPerRope = 48;
        constexpr std::uint32_t kShapeLinesPerRope = 60;   // 30 s at two a second
        constexpr std::uint32_t kStateHeartbeatMs = 2000;
        constexpr int kFollowFrames = 8;
        // Own rope fallback: type 14 is a thin visible rope (the reins family), used
        // when the recorded layout cannot be created.
        constexpr int kOwnRopeType = 14;
        // Longest step between neighbouring vertices of a recorded window;
        // a longer one is a gap, not rope, when the working length is measured.
        constexpr float kMaxVertexStep = 1.5f;
        constexpr std::uint32_t kFollowLineCap = 200;    // POSE_V2_ROPE_FOLLOW per session
        constexpr unsigned long long kRematchMs = 500;   // playback: full rope match
        constexpr std::uint32_t kOwnCreatesPerTick = 1;  // playback: own rope creations

        std::uint64_t AttachKeyOf(const rope::RopeInfo& rope)
        {
            std::uint64_t entities[rope::kMaxAttachments] = {};
            std::int32_t bones[rope::kMaxAttachments] = {};
            const std::uint32_t count = (std::min)(rope.attachmentCount, rope::kMaxAttachments);
            for (std::uint32_t n = 0; n < count; ++n)
            {
                entities[n] = rope.attachments[n].entity;
                bones[n] = rope.attachments[n].bone;
            }
            return AttachKey(entities, bones, count);
        }

        std::string HolderText(const RopeSlotEvent& state)
        {
            return state.holder == RopeHolder::Recorded ? entity::IdText(state.id)
                : state.holder == RopeHolder::Empty     ? std::string("none")
                                                        : std::string("other");
        }
    }

    Ropes::LogBudget& Ropes::Budget(std::uint32_t engineId)
    {
        for (LogBudget& budget : m_budgets)
        {
            if (budget.engineId == engineId)
                return budget;
        }
        m_budgets.push_back({});
        m_budgets.back().engineId = engineId;
        return m_budgets.back();
    }

    SlotGuard& Ropes::GuardFor(std::uint32_t engineId, std::uint32_t slot)
    {
        for (SlotGuard& guard : m_slotGuards)
        {
            if (guard.engineId == engineId && guard.slot == slot)
                return guard;
        }
        m_slotGuards.push_back({});
        m_slotGuards.back().engineId = engineId;
        m_slotGuards.back().slot = slot;
        return m_slotGuards.back();
    }

    void Ropes::BeginRecording()
    {
        m_records.clear();
        m_follow.clear();
        m_evidenceLines = 0;
        m_budgets.clear();
        m_endObjectsTried.clear();
        m_endObjectLogs = 0;
        m_rejected.Reset();
        m_refusedFull.clear();
        m_freedRecords = 0;
        m_maxLive = 0;
        m_stillSamples = 0;
        m_capLogMs = 0;
        m_capLogged = false;
    }

    void Ropes::EndPlayback()
    {
        replay::reins::SetOwnedRopes({}); // no rope is the replay's any more
        for (OwnRope& own : m_own)
        {
            if (PHYSICS::DOES_ROPE_EXIST(own.rope))
                PHYSICS::DELETE_ROPE(&own.rope);
        }
        m_own.clear();
        m_matches.clear();
    }

    void Ropes::ClearFollowedRopes() { replay::reins::SetOwnedRopes({}); }

    void Ropes::LogPinCheck(std::ostream& log) const
    {
        // Debug=0 too (rope_track.h PinTally).
        const replay::logfmt::FormatScope logFormat(log);
        log << std::fixed << std::setprecision(1) << "POSE_V2_ROPE_PIN_CHECK,slot_ticks=" << m_pinTally.slotTicks
            << ",held=" << m_pinTally.held << ",engine_other=" << m_pinTally.engineOther << ",yields=" << m_pinTally.yields
            << ",held_pct=" << m_pinTally.HeldPct() << ",verdict=" << m_pinTally.Verdict() << '\n';
    }

    bool Ropes::Plays(std::uint32_t ropeId) const
    {
        for (const OwnRope& own : m_own)
            if (static_cast<std::uint32_t>(own.rope) == ropeId)
                return true;
        for (const Match& match : m_matches)
            if (match.ropeId == ropeId)
                return true;
        return false;
    }

    void Ropes::DeleteOwn(std::uint32_t engineId)
    {
        for (auto it = m_own.begin(); it != m_own.end(); ++it)
        {
            if (it->engineId != engineId)
                continue;
            if (PHYSICS::DOES_ROPE_EXIST(it->rope))
                PHYSICS::DELETE_ROPE(&it->rope);
            m_own.erase(it);
            return;
        }
    }

    void Ropes::PlayOwn(const PuppetIndex& puppets, RopeRecord& record, std::uint32_t timeMs, std::ostream& log)
    {
        static std::vector<float> recorded;
        static std::vector<float> shaped;
        if (!VerticesAt(record, timeMs, recorded))
        {
            DeleteOwn(record.engineId);
            return;
        }
        OwnRope* own = nullptr;
        for (OwnRope& candidate : m_own)
        {
            if (candidate.engineId == record.engineId)
                own = &candidate;
        }
        if (!own || !PHYSICS::DOES_ROPE_EXIST(own->rope))
        {
            // kOwnCreatesPerTick creations a tick; the other records get theirs on the
            // next ticks.
            if (!m_ownCreatesLeft)
                return;
            --m_ownCreatesLeft;
            REPLAY_PROFILE_ZONE("play.ropes.own_create");
            const float length = (std::max)(MaxPathLength(record, kMaxVertexStep) * 1.05f, 1.0f);
            // First choice: the recorded type and vertex count, through the _ADD_ROPE_2
            // form the game's scripts use for type 0 ropes (its int argument is the
            // segment count: type 0 with 129 segments -> 130 vertices, the lasso's own
            // layout). Never networked. Fallback: the thin type 14 rope, resampled.
            int rope = PHYSICS::_ADD_ROPE_2(recorded[0], recorded[1], recorded[2], 0.0f, 0.0f, 0.0f, length,
                static_cast<int>(record.type), FALSE, static_cast<int>(record.vertices) - 1, -1.0f);
            const bool sameLayout = PHYSICS::DOES_ROPE_EXIST(rope) &&
                PHYSICS::GET_ROPE_VERTEX_COUNT(rope) == static_cast<int>(record.vertices);
            if (!sameLayout)
            {
                if (PHYSICS::DOES_ROPE_EXIST(rope))
                    PHYSICS::DELETE_ROPE(&rope);
                // Arguments as the game's own scripts create a rope (act_bankrobbery01).
                rope = PHYSICS::ADD_ROPE(recorded[0], recorded[1], recorded[2], 0.0f, 0.0f, 0.0f, length,
                    kOwnRopeType, length, 0.5f, 0.5f, FALSE, TRUE, FALSE, 1.0f, FALSE, nullptr, TRUE);
            }
            const bool created = PHYSICS::DOES_ROPE_EXIST(rope) != FALSE;
            log << "POSE_V2_PLAYBACK_ROPE_OWN,engine_id=" << record.engineId
                << ",status=" << (created ? "created" : "failed")
                << ",rope=" << rope
                << ",type=" << (sameLayout ? static_cast<int>(record.type) : kOwnRopeType)
                << ",layout=" << (sameLayout ? "recorded" : "resampled")
                << ",length=" << length
                << ",vertices=" << (created ? PHYSICS::GET_ROPE_VERTEX_COUNT(rope) : 0)
                << ",recorded_vertices=" << record.vertices
                << ",playhead_ms=" << timeMs
                << '\n';
            if (!created)
                return;
            DeleteOwn(record.engineId);
            m_own.push_back({record.engineId, rope, true, 0, sameLayout});
            own = &m_own.back();
        }
        const int count = PHYSICS::GET_ROPE_VERTEX_COUNT(own->rope);
        if (count <= 0)
            return;
        // The recorded window, already resampled to the record's vertex
        // count; resampled again only onto a fallback rope of another count.
        const std::uint32_t inUse = WindowCountAt(record, timeMs);
        if (static_cast<std::uint32_t>(count) == record.vertices)
        {
            shaped = recorded;
        }
        else
        {
            Resample(recorded, static_cast<std::uint32_t>(count), shaped);
        }
        for (int v = 0; v < count; ++v)
            PHYSICS::_0xE54BF2CE6C7D23A9(own->rope, v, shaped[v * 3], shaped[v * 3 + 1], shaped[v * 3 + 2]);

        // Evidence, twice a second: the recorded shape and where the engine
        // has the rope's ends after the setter.
        LogBudget& budget = Budget(record.engineId);
        if (replay::log::Verbose() && timeMs >= own->nextEvidenceMs && m_attachLogs < kAttachLogCap &&
            budget.shapeLines < kShapeLinesPerRope)
        {
            ++m_attachLogs;
            ++budget.shapeLines;
            own->nextEvidenceMs = timeMs + 500;
            std::size_t atOrigin = 0;
            for (std::size_t n = 0; n + 2 < recorded.size(); n += 3)
            {
                if (std::fabs(recorded[n]) < 1.0f && std::fabs(recorded[n + 1]) < 1.0f)
                    ++atOrigin;
            }
            const std::size_t last = recorded.size() - 3;
            const Vector3 a0 = PHYSICS::GET_ROPE_VERTEX_COORD(own->rope, 0);
            const Vector3 an = PHYSICS::GET_ROPE_VERTEX_COORD(own->rope, count - 1);
            const replay::logfmt::FormatScope logFormat(log); // precision not left behind
            log << std::fixed << std::setprecision(2)
                << "POSE_V2_PLAYBACK_ROPE_OWN_SHAPE,engine_id=" << record.engineId
                << ",rope=" << own->rope
                << ",extent=" << Extent(recorded)
                << ",visible=" << (own->visible ? 1 : 0)
                << ",at_origin=" << atOrigin << ",in_use=" << inUse << "/" << record.vertices
                << ",rec_first=" << recorded[0] << "/" << recorded[1] << "/" << recorded[2]
                << ",rec_last=" << recorded[last] << "/" << recorded[last + 1] << "/" << recorded[last + 2]
                << ",rope_first=" << a0.x << "/" << a0.y << "/" << a0.z
                << ",rope_last=" << an.x << "/" << an.y << "/" << an.z;
            // Distance of the rope's first vertex to the puppet's right hand
            // (the recording logs the same for the original: a lag shows
            // here as a growing distance while the hand moves).
            float hand[3] = {};
            if (record.nearPed.Valid() && puppets.HandOf(record.nearPed, hand))
            {
                const float dx = a0.x - hand[0];
                const float dy = a0.y - hand[1];
                const float dz = a0.z - hand[2];
                log << ",hand_m=" << std::sqrt(dx * dx + dy * dy + dz * dz);
            }
            log << ",playhead_ms=" << timeMs << '\n';
            log.unsetf(std::ios_base::floatfield);
        }
    }

    void Ropes::BeginPlayback()
    {
        for (RopeRecord& record : m_records)
            record.playbackLogged = false;
        m_attachLogs = 0;
        m_pinTally = {};
        m_follow.clear();
        m_budgets.clear();
        m_slotGuards.clear();
        m_matches.clear();
    }

    void Ropes::LogFollow(const rope::RopeInfo& rope, const char* side, std::uint32_t timeMs, std::ostream& log)
    {
        // Debug=1 only, capped per session, no flush per line (the script flushes
        // once a tick).
        if (m_follow.empty() || !replay::log::Verbose())
            return;
        for (auto& follow : m_follow)
        {
            if (follow.first != rope.id || follow.second <= 0)
                continue;
            --follow.second;
            if (!m_followLines.Take(kFollowLineCap))
                return;
            const replay::logfmt::FormatScope logFormat(log); // precision not left behind
            log << std::fixed << std::setprecision(2)
                << "POSE_V2_ROPE_FOLLOW,side=" << side
                << ",engine_id=" << rope.id
                << ",left=" << follow.second
                << ",first=" << rope.first[0] << "/" << rope.first[1] << "/" << rope.first[2]
                << ",last=" << rope.last[0] << "/" << rope.last[1] << "/" << rope.last[2]
                << ",header=" << rope::DescribeHeader(rope);
            // Every entry (reins and traces use 8); entries past 8 (reins have 24) only
            // while they hold an entity.
            const std::uint32_t entries = (std::min)(rope.attachmentCount, rope::kMaxAttachments);
            for (std::uint32_t slot = 0; slot < entries; ++slot)
            {
                if (slot < 8 || rope.attachments[slot].entity)
                    log << ",e" << slot << "=" << rope::DescribeEntry(rope, slot);
            }
            log << ",time_ms=" << timeMs << '\n';
            log.unsetf(std::ios_base::floatfield);
            return;
        }
    }

    void Ropes::Sample(const EntityIndex& entities, std::uint32_t timeMs, std::uint32_t intervalMs,
        std::ostream& log)
    {
        static std::vector<rope::RopeInfo> ropes;
        std::uint32_t managerCount = 0;
        if (!rope::Snapshot(ropes, managerCount))
            return;
        static std::vector<float> vertices;
        m_rejected.BeginFrame(timeMs);
        // The focus (player, gameplay camera) for the distance of each rope, and the
        // hand test of the ropes recorded for a ped's hand.
        float focus[2][3] = {};
        const std::size_t focusCount = (std::min)(entities.Focus(focus), static_cast<std::size_t>(2));
        const auto inHandOf = [&entities](entity::ReplayId ped, const rope::RopeInfo& rope, bool& handRead) {
            float hand[3] = {};
            handRead = ped.Valid() && entities.HandOf(ped, hand);
            if (!handRead)
                return false;
            const float(*handPoint)[3] = &hand;
            return RopeFocusDistance(rope.first, rope.last, handPoint, 1) <= kRopeHandRadiusM;
        };
        for (const rope::RopeInfo& rope : ropes)
        {
            if (!rope.vertices || rope.vertices > kMaxRopeVertices)
                continue;
            const float focusDistance = RopeFocusDistance(rope.first, rope.last, focus, focusCount);
            auto record = std::find_if(m_records.begin(), m_records.end(),
                [&](const RopeRecord& entry) { return entry.engineId == rope.id; });
            if (record == m_records.end())
            {
                // A rope rejected before with the same entries waits for the next recheck
                // (RejectedRopes).
                const std::uint64_t attachKey = AttachKeyOf(rope);
                if (m_rejected.Skip(rope.id, attachKey))
                    continue;
                std::vector<RopeAttach> attachments;
                bool recorded = false;
                const std::uint32_t count = (std::min)(rope.attachmentCount, rope::kMaxAttachments);
                for (std::uint32_t index = 0; index < count; ++index)
                {
                    const entity::ReplayId id = entities.IdOf(rope.attachments[index].entity);
                    recorded = recorded || id.Valid();
                    attachments.push_back({id, rope.attachments[index].bone});
                }
                // A rope in a recorded ped's hand (a lasso before it catches anything) is
                // recorded too: an end in the hand, not anywhere within 3 m of the ped (a
                // clothes line or a fence rope a ped walks by is not the scene).
                RopeCandidate candidate;
                candidate.holdsRecorded = recorded;
                entity::ReplayId nearPed;
                if (!recorded)
                {
                    nearPed = entities.NearestRecordedPed(rope.first, kRopePedRadiusM);
                    if (!nearPed.Valid())
                        nearPed = entities.NearestRecordedPed(rope.last, kRopePedRadiusM);
                    bool handRead = false;
                    candidate.inHand = inHandOf(nearPed, rope, handRead);
                    candidate.nearPedNoHand = nearPed.Valid() && !handRead;
                    if (!candidate.inHand && !candidate.nearPedNoHand)
                        nearPed = {};
                }
                if (!RopeRelevant(candidate))
                {
                    m_rejected.Reject(rope.id, attachKey);
                    continue;
                }
                // A recorded rope's history is never given away for a new one (RopeRecordFits):
                // only the engine's whole rope pool live in the records refuses it.
                if (!RopeRecordFits(m_records, timeMs))
                {
                    // A rope to record and the pool's worth of records live. With Debug=0 too
                    // (_SUMMARY): the first time and every 10 s while it lasts, with the
                    // distinct ropes refused.
                    m_rejected.Reject(rope.id, attachKey);
                    if (std::find(m_refusedFull.begin(), m_refusedFull.end(), rope.id) == m_refusedFull.end() &&
                        m_refusedFull.size() < 4096)
                        m_refusedFull.push_back(rope.id);
                    if (!m_capLogged || timeMs >= m_capLogMs + 10000)
                    {
                        m_capLogged = true;
                        m_capLogMs = timeMs;
                        log << "POSE_V2_ROPE_CAP_SUMMARY,records=" << m_records.size()
                            << ",live=" << LiveRopeRecords(m_records, timeMs) << ",live_cap=" << kEngineRopePoolCapacity
                            << ",refused_full=" << m_refusedFull.size() << ",freed=" << m_freedRecords
                            << ",still_samples=" << m_stillSamples
                            << ",distance_m=" << static_cast<int>(focusDistance)
                            << ",engine_id=" << rope.id << ",type=" << rope.type << ",vertices=" << rope.vertices
                            << ",time_ms=" << timeMs << '\n';
                    }
                    continue;
                }
                RopeRecord entry;
                entry.engineId = rope.id;
                entry.type = rope.type;
                entry.vertices = rope.vertices;
                entry.attachments = attachments;
                entry.nearPed = nearPed;
                entry.lastSeenMs = timeMs; // live from its first frame (RopeRecordFits)
                m_records.push_back(entry);
                record = m_records.end() - 1;
                if (m_records.size() > m_maxLive)
                    m_maxLive = (std::max)(m_maxLive, static_cast<std::uint32_t>(LiveRopeRecords(m_records, timeMs)));
                log << "POSE_V2_ROPE_TRACK,engine_id=" << rope.id
                    << ",type=" << rope.type
                    << ",vertices=" << rope.vertices
                    << ",entries=" << rope.attachmentCount
                    << ",attached=";
                for (const RopeAttach& attachment : attachments)
                {
                    log << (attachment.id.Valid() ? entity::IdText(attachment.id) : std::string("none"))
                        << "@" << attachment.bone << " ";
                }
                float step = -1.0f;
                rope::MaxStep(rope, step);
                log << ",header=" << rope::DescribeHeader(rope)
                    << ",e2=" << rope::DescribeEntry(rope, 2)
                    << ",e3=" << rope::DescribeEntry(rope, 3)
                    << ",max_step_m=" << step
                    << ",script_exists=" << (PHYSICS::DOES_ROPE_EXIST(static_cast<int>(rope.id)) ? 1 : 0)
                    << ",script_vertices=" << PHYSICS::GET_ROPE_VERTEX_COUNT(static_cast<int>(rope.id))
                    << ",time_ms=" << timeMs
                    << '\n';
            }
            NoteHeld(*record, timeMs, (rope.flags & rope::kHeldFlag) != 0);
            record->focusDistance = focusDistance;
            // Seen this frame while an entry holds a recorded entity (set in the entry
            // loop below), or, for a rope recorded for the ped's hand, while an end stays
            // in that hand (a world rope a ped walked by would otherwise keep its record
            // for the whole buffer session).
            const bool nearPedRecord = record->nearPed.Valid() &&
                std::none_of(record->attachments.begin(), record->attachments.end(),
                    [](const RopeAttach& attachment) { return attachment.id.Valid(); });
            bool holdsRecorded = false;
            if (nearPedRecord)
            {
                bool handRead = false;
                const bool inHand = inHandOf(record->nearPed, rope, handRead);
                // No hand to read: the ped's rope while it exists.
                holdsRecorded = RopeStillSeen(false, true, inHand || !handRead);
            }
            // Evidence (lasso states: stowed, spinning, thrown, caught): the engine flags,
            // mode, length, the drawn window and the script vertex count (they agree in
            // every lasso phase), the steps along the window and the distance of its first
            // vertex to the holder's right hand, at most 4 times a second, only when the
            // flags, mode, drawn window or length changed, or every 2 s, within the rope's
            // own budget.
            LogBudget& budget = Budget(rope.id);
            const std::int32_t lengthCm = static_cast<std::int32_t>(std::lround(rope.length * 100.0f));
            const bool stateChanged = !budget.hasState || budget.flags != rope.flags || budget.mode != rope.mode ||
                budget.drawStart != rope.drawStart || budget.drawCount != rope::WindowCount(rope) ||
                budget.lengthCm != lengthCm;
            if (replay::log::Verbose() && timeMs >= record->nextStateLogMs && // evidence only
                (stateChanged || timeMs >= budget.lastStateMs + kStateHeartbeatMs) &&
                budget.stateLines < kStateLinesPerRope && m_evidenceLines < kEvidenceLineCap &&
                rope::ReadWindow(rope, vertices))
            {
                record->nextStateLogMs = timeMs + 250;
                ++m_evidenceLines;
                ++budget.stateLines;
                budget.hasState = true;
                budget.flags = rope.flags;
                budget.mode = rope.mode;
                budget.drawStart = rope.drawStart;
                budget.drawCount = rope::WindowCount(rope);
                budget.lengthCm = lengthCm;
                budget.lastStateMs = timeMs;
                const int scriptId = static_cast<int>(rope.id);
                const int nativeCount = PHYSICS::DOES_ROPE_EXIST(scriptId) ? PHYSICS::GET_ROPE_VERTEX_COUNT(scriptId) : -1;
                const replay::logfmt::FormatScope logFormat(log); // precision not left behind
                log << std::fixed << std::setprecision(2)
                    << "POSE_V2_ROPE_STATE,engine_id=" << rope.id
                    << ",flags=0x" << std::hex << rope.flags << std::dec
                    << ",mode=" << rope.mode
                    << ",length=" << rope.length
                    << ",draw_start=" << rope.drawStart
                    << ",draw_count=" << rope::WindowCount(rope)
                    << ",native_count=" << nativeCount
                    << ",extent=" << Extent(vertices)
                    << ",header=" << rope::DescribeHeader(rope);
                log << ",steps_cm=";
                for (std::size_t v = 0; v + 1 < vertices.size() / 3; ++v)
                {
                    const float dx = vertices[(v + 1) * 3] - vertices[v * 3];
                    const float dy = vertices[(v + 1) * 3 + 1] - vertices[v * 3 + 1];
                    const float dz = vertices[(v + 1) * 3 + 2] - vertices[v * 3 + 2];
                    const float cm = std::sqrt(dx * dx + dy * dy + dz * dz) * 100.0f;
                    log << (cm > 999.0f ? 999 : static_cast<int>(cm)) << ".";
                }
                float hand[3] = {};
                if (record->nearPed.Valid() && entities.HandOf(record->nearPed, hand))
                {
                    const float dx = vertices[0] - hand[0];
                    const float dy = vertices[1] - hand[1];
                    const float dz = vertices[2] - hand[2];
                    log << ",hand_m=" << std::sqrt(dx * dx + dy * dy + dz * dz);
                }
                log << ",time_ms=" << timeMs << '\n';
                log.unsetf(std::ios_base::floatfield);
            }
            // Attachment changes, every frame.
            const std::uint32_t slots = (std::min)(rope.attachmentCount, rope::kMaxAttachments);
            for (std::uint32_t slot = 0; slot < slots; ++slot)
            {
                const rope::RopeAttachment& attachment = rope.attachments[slot];
                RopeSlotEvent state;
                state.timeMs = timeMs;
                state.slot = slot;
                state.bone = attachment.bone;
                state.id = entities.IdOf(attachment.entity);
                // An unknown entity at an end of a recorded rope (the fishing bobber on the
                // rod's line) is offered to the recorder once; when it is an object it becomes
                // a prop and this slot holds a recorded entity from then on.
                if (attachment.entity && !state.id.Valid() &&
                    std::find(m_endObjectsTried.begin(), m_endObjectsTried.end(), attachment.entity) == m_endObjectsTried.end())
                {
                    m_endObjectsTried.push_back(attachment.entity);
                    // The owner: the recorded entity at another end of the
                    // rope (the rod), else the ped the rope started next to.
                    entity::ReplayId owner;
                    for (std::uint32_t other = 0; other < slots && !owner.Valid(); ++other)
                    {
                        if (other != slot)
                            owner = entities.IdOf(rope.attachments[other].entity);
                    }
                    if (!owner.Valid())
                        owner = record->nearPed;
                    const char* status = "no_owner";
                    if (owner.Valid())
                        state.id = entities.AdoptEndObject(attachment.entity, owner, status);
                    if (m_endObjectLogs < 64)
                    {
                        ++m_endObjectLogs;
                        log << "POSE_V2_ROPE_END_OBJECT,engine_id=" << rope.id
                            << ",slot=" << slot
                            << ",entity=0x" << std::hex << attachment.entity << std::dec
                            << ",bone=" << attachment.bone
                            << ",owner=" << (owner.Valid() ? entity::IdText(owner) : std::string("none"))
                            << ",status=" << status
                            << ",id=" << (state.id.Valid() ? entity::IdText(state.id) : std::string("none"))
                            << ",time_ms=" << timeMs << '\n';
                    }
                }
                state.holder = !attachment.entity ? RopeHolder::Empty
                    : state.id.Valid()           ? RopeHolder::Recorded
                                                 : RopeHolder::Other;
                holdsRecorded = holdsRecorded || state.holder == RopeHolder::Recorded;
                if (NoteSlot(*record, state) != SlotNote::Changed || m_evidenceLines >= kEvidenceLineCap)
                    continue;
                LogBudget& changes = Budget(rope.id);
                if (changes.attachLines >= kAttachLinesPerRope)
                    continue;
                ++changes.attachLines;
                ++m_evidenceLines;
                m_follow.push_back({rope.id, kFollowFrames});
                log << "POSE_V2_ROPE_ENTRY,engine_id=" << rope.id
                    << ",slot=" << slot
                    << ",bytes=" << rope::DescribeEntry(rope, slot)
                    << ",time_ms=" << timeMs << '\n';
                log << "POSE_V2_ROPE_ATTACH,engine_id=" << rope.id
                    << ",slot=" << slot
                    << ",holder=" << HolderText(state)
                    << ",bone=" << state.bone
                    << ",time_ms=" << timeMs
                    << '\n';
            }
            if (holdsRecorded)
                record->lastSeenMs = timeMs;
            LogFollow(rope, "record", timeMs, log);
            if (!WantsVertices(*record, rope.vertices, timeMs, intervalMs))
                continue;
            // The drawn window only (the rest of the array holds parked
            // and stale vertices the engine never draws); a window of one
            // vertex or none is not a rope this frame.
            if (rope::WindowCount(rope) < 2 || !rope::ReadWindow(rope, vertices))
                continue;
            // A still rope moves its last sample's time.
            if (!AppendOrExtendVertices(*record, timeMs, vertices))
                ++m_stillSamples;
        }
    }

    void Ropes::Play(const PuppetIndex& puppets, std::uint32_t timeMs, std::ostream& log)
    {
        // The engine ropes this tick follows, published on every way out for the
        // reins guard (reins_guard.h: the controller's put-back on the saddle is
        // skipped for them).
        static std::vector<std::uint64_t> followed;
        followed.clear();
        struct Publish
        {
            ~Publish() { replay::reins::SetOwnedRopes(followed); }
        } publish;
        if (m_records.empty())
            return;
        // The engine ropes are read only when a record is in its span at the playhead
        // (the snapshot is ~15 guarded reads per engine rope, every rope of the town).
        bool anyInSpan = false;
        for (const RopeRecord& record : m_records)
        {
            if (InSpan(record, timeMs))
                anyInSpan = true;
            else if (!m_own.empty())
                DeleteOwn(record.engineId);
        }
        if (!anyInSpan)
            return;
        static std::vector<rope::RopeInfo> ropes;
        std::uint32_t managerCount = 0;
        {
            REPLAY_PROFILE_ZONE("play.ropes.snapshot");
            if (!rope::Snapshot(ropes, managerCount))
                return;
        }
        m_ownCreatesLeft = kOwnCreatesPerTick;
        const unsigned long long wallMs = GetTickCount64();
        static std::vector<std::uint32_t> claimed;
        claimed.clear();
        const auto isOwn = [&](std::uint32_t ropeId) {
            return std::any_of(m_own.begin(), m_own.end(),
                [&](const OwnRope& o) { return static_cast<std::uint32_t>(o.rope) == ropeId; });
        };
        const auto isClaimed = [&](std::uint32_t ropeId) {
            return std::find(claimed.begin(), claimed.end(), ropeId) != claimed.end();
        };
        for (RopeRecord& record : m_records)
        {
            if (!InSpan(record, timeMs))
                continue;
            const rope::RopeInfo* target = nullptr;
            // The rope matched last time, checked again with two reads of the snapshot
            // (same rope, same puppet in the same entry and bone); a full match every
            // kRematchMs or when that fails.
            auto match = std::find_if(m_matches.begin(), m_matches.end(),
                [&](const Match& entry) { return entry.engineId == record.engineId; });
            if (match != m_matches.end() && wallMs < match->rematchAt)
            {
                for (const rope::RopeInfo& rope : ropes)
                {
                    if (rope.id != match->ropeId)
                        continue;
                    if (rope.vertices == record.vertices && match->slot < rope.attachmentCount &&
                        match->slot < rope::kMaxAttachments && rope.attachments[match->slot].entity == match->address &&
                        rope.attachments[match->slot].bone == match->bone && !isClaimed(rope.id) && !isOwn(rope.id))
                    {
                        target = &rope;
                    }
                    break;
                }
            }
            if (!target)
            {
                REPLAY_PROFILE_ZONE("play.ropes.match");
                std::uint32_t matchedSlot = 0;
                std::uint64_t matchedAddress = 0;
                for (const rope::RopeInfo& rope : ropes)
                {
                    if (rope.vertices != record.vertices || isClaimed(rope.id) || isOwn(rope.id))
                        continue;
                    const std::uint32_t count = (std::min)(
                        (std::min)(rope.attachmentCount, rope::kMaxAttachments),
                        static_cast<std::uint32_t>(record.attachments.size()));
                    for (std::uint32_t slot = 0; slot < count && !target; ++slot)
                    {
                        const RopeAttach& recorded = record.attachments[slot];
                        const std::uint64_t address = recorded.id.Valid() ? puppets.AddressOf(recorded.id) : 0;
                        if (address && rope.attachments[slot].entity == address &&
                            rope.attachments[slot].bone == recorded.bone)
                        {
                            target = &rope;
                            matchedSlot = slot;
                            matchedAddress = address;
                        }
                    }
                    if (target)
                        break;
                }
                if (target)
                {
                    if (match == m_matches.end())
                    {
                        m_matches.push_back({});
                        match = m_matches.end() - 1;
                    }
                    *match = {record.engineId, target->id, matchedSlot, matchedAddress, target->attachments[matchedSlot].bone,
                        wallMs + kRematchMs};
                }
                else if (match != m_matches.end())
                {
                    m_matches.erase(match);
                }
            }
            if (!record.playbackLogged)
            {
                record.playbackLogged = true;
                log << "POSE_V2_PLAYBACK_ROPE,engine_id=" << record.engineId
                    << ",type=" << record.type
                    << ",vertices=" << record.vertices
                    << ",status=" << (target ? "matched" : "no_target")
                    << ",target=" << (target ? target->id : 0)
                    << ",target_type=" << (target ? target->type : 0)
                    << ",slot_events=" << record.slotEvents.size()
                    << ",playhead_ms=" << timeMs
                    << '\n';
            }
            if (!target)
            {
                PlayOwn(puppets, record, timeMs, log);
                continue;
            }
            claimed.push_back(target->id);
            followed.push_back(target->address);
            // A record that started on its own rope (the puppet's reins may not exist yet
            // in the first playback tick) drops it once the engine rope is followed, or
            // the own rope would stay frozen where it was created.
            DeleteOwn(record.engineId);
            LogFollow(*target, "playback", timeMs, log);
            rope::SetRopeFlag(*target, rope::kHeldFlag, HeldAt(record, timeMs));
            const std::uint32_t slots = (std::min)(target->attachmentCount, rope::kMaxAttachments);
            for (std::uint32_t slot = 0; slot < slots; ++slot)
            {
                const RopeSlotEvent* state = SlotStateAt(record, slot, timeMs);
                const rope::RopeAttachment& current = target->attachments[slot];
                if (!state || state->holder == RopeHolder::Other || state->bone != current.bone)
                    continue;
                std::uint64_t desired = 0;
                if (state->holder == RopeHolder::Recorded)
                {
                    desired = puppets.AddressOf(state->id);
                    if (!desired)
                        continue;
                }
                // An entry the engine writes back every frame is left to it after three writes
                // that did not hold, until the recorded state changes (no re-attach loop).
                const SlotWrite decision = DecideSlotWrite(GuardFor(record.engineId, slot), state->timeMs,
                    current.entity == desired);
                // The pins held by another entity than the rope's own (hands, a wagon),
                // checked every tick.
                if (desired && slot > 0)
                {
                    const RopeSlotEvent* own = SlotStateAt(record, 0, timeMs);
                    if (!own || own->holder != RopeHolder::Recorded || own->id != state->id)
                        m_pinTally.Note(current.entity == desired, decision);
                }
                if (decision == SlotWrite::Hold || decision == SlotWrite::Yielded)
                    continue;
                LogBudget& budget = Budget(record.engineId);
                if (decision == SlotWrite::Yield)
                {
                    if (m_attachLogs < kAttachLogCap)
                    {
                        ++m_attachLogs;
                        log << "POSE_V2_PLAYBACK_ROPE_ATTACH,engine_id=" << record.engineId
                            << ",target=" << target->id
                            << ",slot=" << slot
                            << ",holder=" << (state->holder == RopeHolder::Recorded
                                   ? entity::IdText(state->id) : std::string("none"))
                            << ",bone=" << state->bone
                            << ",status=engine_owned,writes=" << kSlotRevertLimit
                            << ",engine_entity=0x" << std::hex << current.entity << std::dec
                            << ",playhead_ms=" << timeMs
                            << '\n';
                    }
                    continue;
                }
                const bool set = rope::SetAttachmentEntity(current.entry, desired) &&
                    rope::SetAttachmentActive(current.entry, desired != 0);
                const bool logLine = m_attachLogs < kAttachLogCap && budget.attachLines < kPlaybackAttachLinesPerRope;
                if (set && logLine)
                    m_follow.push_back({target->id, kFollowFrames});
                if (logLine || (!set && m_attachLogs < kAttachLogCap))
                {
                    ++m_attachLogs;
                    ++budget.attachLines;
                    log << "POSE_V2_PLAYBACK_ROPE_ATTACH,engine_id=" << record.engineId
                        << ",target=" << target->id
                        << ",slot=" << slot
                        << ",holder=" << (state->holder == RopeHolder::Recorded
                               ? entity::IdText(state->id) : std::string("none"))
                        << ",bone=" << state->bone
                        << ",status=" << (set ? "set" : "failed")
                        << ",playhead_ms=" << timeMs
                        << '\n';
                }
            }
            ApplyRecordedShape(puppets, record, *target, timeMs, log);
        }
    }

    void Ropes::ApplyRecordedShape(const PuppetIndex& puppets, const RopeRecord& record, const rope::RopeInfo& target,
        std::uint32_t timeMs, std::ostream& log)
    {
        static std::vector<float> recorded;
        const ShapeApply decision = DecideRecordedShape(record, timeMs, rope::WindowCount(target));
        bool written = false;
        if (decision == ShapeApply::Apply && VerticesAt(record, timeMs, recorded))
            written = rope::WriteWindow(target, recorded.data(), record.vertices);
        if (!replay::log::Verbose())
            return; // the rest is evidence (Debug=1)
        // Evidence: on a status change, then twice a second within the
        // rope's shape budget: the status and the distance of the nearest
        // recorded vertex to the holder's right hand (the reins run through
        // the hand: a few centimetres when the shape is in it).
        const int status = decision != ShapeApply::Apply ? static_cast<int>(decision) : written ? 0 : 3;
        LogBudget& budget = Budget(record.engineId);
        const bool changed = status != budget.shapeStatus;
        if ((!changed && timeMs < budget.nextShapeMs) || budget.shapeLines >= kShapeLinesPerRope ||
            m_attachLogs >= kAttachLogCap)
        {
            return;
        }
        budget.shapeStatus = status;
        budget.nextShapeMs = timeMs + 500;
        ++budget.shapeLines;
        ++m_attachLogs;
        static const char* const kNames[] = {"applied", "no_sample", "window_mismatch", "write_failed"};
        const replay::logfmt::FormatScope logFormat(log); // precision not left behind
        log << std::fixed << std::setprecision(3)
            << "POSE_V2_PLAYBACK_ROPE_SHAPE,engine_id=" << record.engineId
            << ",target=" << target.id
            << ",status=" << kNames[status]
            << ",window=" << rope::WindowCount(target) << "/" << record.vertices
            << ",recorded_window=" << WindowCountAt(record, timeMs);
        // Every recorded holder the rope ever had with a right hand (the
        // horse's own entries have none): the rider or leader, even while
        // the recorded entries say nobody holds the rope.
        float hand[3] = {};
        std::vector<entity::ReplayId> holders;
        if (record.nearPed.Valid())
            holders.push_back(record.nearPed);
        for (const RopeSlotEvent& event : record.slotEvents)
        {
            if (event.holder == RopeHolder::Recorded && holders.size() < 4 &&
                std::find(holders.begin(), holders.end(), event.id) == holders.end())
            {
                holders.push_back(event.id);
            }
        }
        for (const entity::ReplayId& id : holders)
        {
            if (written && puppets.HandOf(id, hand))
                log << ",hand_" << entity::IdText(id) << "_nearest_m=" << NearestVertexDistance(recorded, hand);
        }
        log << ",playhead_ms=" << timeMs << '\n';
        log.unsetf(std::ios_base::floatfield);
    }
}
