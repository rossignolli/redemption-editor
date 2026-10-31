// replay_events_speech_record.cpp: speech recorded: the lines created, started, stopped and adopted.
// Split from replay_events.cpp (moved code); shared internals in replay_events_internal.h.

#include "replay_events_internal.h"

namespace replay::events
{

    bool PoseV2SpeechLineList(Ped ped, std::uint64_t& list, std::uint16_t& count)
    {
        const std::uint64_t speech = replay::audio::SpeechEntity(replay::entities::AddressOf(ped));
        count = 0;
        return speech && replay::audio::ReadU64(speech + 0x78, list) &&
            replay::audio::ReadBytes(speech + 0x80, &count, sizeof(count));
    }

    void FollowPoseV2QueuedLines()
    {
        for (std::size_t index = g_poseV2QueuedLines.size(); index-- > 0;)
        {
            PoseV2QueuedLine& queued = g_poseV2QueuedLines[index];
            std::uint64_t list = 0;
            std::uint16_t count = 0;
            bool present = false;
            if (replay::entities::Exists(queued.ped) &&
                PoseV2SpeechLineList(queued.ped, list, count) && list && count <= 64)
            {
                for (std::uint16_t slot = 0; slot < count && !present; ++slot)
                {
                    std::uint64_t line = 0;
                    present = replay::audio::ReadU64(list + slot * 8u, line) && line == queued.line;
                }
            }
            if (present)
            {
                std::int32_t state = -1;
                replay::audio::ReadBytes(queued.line + 0x234, &state, sizeof(state));
                if (state != queued.lastState)
                {
                    queued.states += std::to_string(queued.frames) + ":" + std::to_string(state) + " ";
                    queued.lastState = state;
                }
                ++queued.frames;
                continue;
            }
            std::ostream& log = Ctx().log;
            log << "POSE_V2_SPEECH_LINE_END"
                << ",ped=" << replay::entity::IdText(queued.id)
                << ",phase=" << queued.phase
                << ",context=0x" << std::hex << replay::audio::FinalizeHash(queued.context) << std::dec
                << ",alive_ms=" << (replay::audio::NowUs() - queued.queuedUs) / 1000
                << ",frames=" << queued.frames
                << ",states=" << queued.states;
            replay::audio::SpeechLineStop stop;
            if (replay::audio::FindLineStop(queued.line, stop))
            {
                log << ",stop_reason=" << stop.reason
                    << ",stop_state=" << stop.state
                    << ",stop_callers=0x" << std::hex << stop.callers[0] << "/0x" << stop.callers[1]
                    << "/0x" << stop.callers[2] << "/0x" << stop.callers[3] << std::dec;
            }
            log << ",speaking=" << (replay::entities::Exists(queued.ped) &&
                       AUDIO::IS_ANY_SPEECH_PLAYING(queued.ped) ? 1 : 0)
                << '\n';
            log.flush();
            g_poseV2QueuedLines.erase(g_poseV2QueuedLines.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }
    PoseV2SpeechMatcher g_poseV2SpeechMatcher;
    std::uint64_t g_poseV2SpeechStartCursor = 0;
    std::uint64_t g_poseV2SpeechStopCursor = 0;
    std::uint32_t g_poseV2SpeechDroppedUnheard = 0;
    std::uint32_t g_poseV2SpeechVoiceInRequest = 0; // lines replayed with the recorded voice in the request
    namespace
    {

    std::uint32_t PoseV2RecordMs(std::uint64_t timeUs)
    {
        const std::uint64_t now = replay::audio::NowUs();
        const std::uint64_t ageMs = now > timeUs ? (now - timeUs) / 1000 : 0;
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        return replay::pose::capture::RecordMsAgo(ageMs, nowMs);
    }
    }

    replay::speech::Identity PoseV2SpeechIdentity(const replay::entity::SpeechEvent& line)
    {
        replay::speech::Identity id;
        id.voice = line.voiceHash;
        id.context = line.contextPartialHash;
        id.variation = line.variation;
        // From the recorded type and name, never the session pointer (the
        // type may come from the line's start).
        id.voiceObjectHash = line.voiceObjectPartial ? replay::speech::Finalize(line.voiceObjectPartial) : 0;
        id.voiceObjectType = line.voiceObjectType;
        return id;
    }

    // The identity line both phases log, so a test can pair them.
    void LogPoseV2SpeechId(const char* phase, const replay::entity::SpeechEvent& line, std::uint64_t linePtr,
        const replay::speech::Identity& id, std::uint64_t voiceObject, std::uint32_t dataType, std::uint32_t path,
        std::uint32_t coreMs, std::int64_t startMs, const std::string& extra)
    {
        std::ostream& log = Ctx().log;
        log << "POSE_V2_SPEECH_ID,phase=" << phase
            << ",ped=" << replay::entity::IdText(line.speaker)
            << ",line=0x" << std::hex << linePtr
            << ",path=" << replay::speech::PathName(path, replay::audio::SpeechCorePaths())
            << ",voice=0x" << id.voice
            << ",context=0x" << replay::speech::Finalize(id.context)
            << std::dec << ",variation=" << id.variation
            << std::hex << ",voice_obj=0x" << voiceObject
            << ",voice_obj_type=0x" << static_cast<unsigned>(id.voiceObjectType)
            << ",voice_obj_hash=0x" << id.voiceObjectHash
            << std::dec << ",data_type=" << dataType
            << std::hex << ",label=0x" << replay::speech::Label(id.voice, id.context, id.variation)
            << std::dec << ",t_core_ms=" << coreMs
            << ",t_start_ms=" << startMs
            << ",event_ms=" << line.timeMs;
        if (!extra.empty())
            log << ',' << extra;
        log << '\n';
        log.flush();
    }
    namespace
    {

    replay::speech::rec::LineKey PoseV2SpeechKey(const replay::entity::SpeechEvent& line)
    {
        replay::speech::rec::LineKey key;
        key.voice = line.voiceHash;
        key.context = line.contextPartialHash;
        key.variation = line.variation;
        return key;
    }

    // A pending line dropped without being heard (stop, reuse, start of
    // another line, end of recording): logged, counted, never in the clip.
    void LogPoseV2SpeechUnheard(const PoseV2PendingSpeech& pending, const char* reason, std::uint32_t stopState,
        bool hasState, const char* at, std::uint32_t atMs)
    {
        std::ostream& log = Ctx().log;
        log << "POSE_V2_SPEECH_UNHEARD,ped=" << replay::entity::IdText(pending.event.speaker)
            << ",line=0x" << std::hex << pending.line
            << ",context=0x" << replay::audio::FinalizeHash(pending.event.contextPartialHash) << std::dec
            << ",variation=" << pending.event.variation
            << ",stop_reason=" << reason;
        if (hasState)
            log << ",stop_state=" << stopState;
        log << ",core_ms=" << pending.event.coreMs;
        if (at && *at)
            log << ",at=" << at << ",at_ms=" << atMs;
        log << '\n';
        log.flush();
        ++g_poseV2SpeechDroppedUnheard;
    }
    }

    void LogPoseV2SpeechDrops(const std::vector<PoseV2SpeechMatcher::Drop>& drops, std::uint32_t atMs)
    {
        for (const PoseV2SpeechMatcher::Drop& drop : drops)
            LogPoseV2SpeechUnheard(drop.entry.value, replay::speech::rec::DropReasonName(drop.reason), 0, false,
                "rule", atMs);
    }

    // Rule 1 for a started recorded line whose stop was never seen: the
    // address was reused, its event keeps its natural end (no stop time).
    void LogPoseV2SpeechStartedReused(const std::vector<PoseV2SpeechMatcher::StartedDrop>& drops, std::uint32_t atMs)
    {
        for (const PoseV2SpeechMatcher::StartedDrop& drop : drops)
        {
            if (drop.eventIndex >= Ctx().clip.speechLines.Events().size())
                continue;
            const replay::entity::SpeechEvent& line = Ctx().clip.speechLines.Events()[drop.eventIndex];
            Ctx().log << "POSE_V2_SPEECH_STARTED_REUSED,ped=" << replay::entity::IdText(line.speaker)
                      << ",line=0x" << std::hex << drop.line << std::dec
                      << ",event_ms=" << line.timeMs << ",at_ms=" << atMs << '\n';
        }
    }
    namespace
    {

    void AddPoseV2SpeechEvent(const PoseV2PendingSpeech& pending, const char* timing)
    {
        const std::size_t index = Ctx().clip.speechLines.Add(pending.event);
        const replay::entity::SpeechEvent& line = Ctx().clip.speechLines.Events()[index];
        g_poseV2SpeechMatcher.Started(pending.line, index);
        char extra[64]{};
        std::snprintf(extra, sizeof(extra), "timing=%s,requested=0x%x", timing,
            replay::audio::FinalizeHash(line.requestedContextPartialHash));
        LogPoseV2SpeechId("record", line, pending.line, PoseV2SpeechIdentity(line), line.voiceObject,
            line.dataType, line.path, line.coreMs, line.timedByStart ? static_cast<std::int64_t>(line.timeMs) : -1,
            extra);
    }

    // Lines asked before the recording and audible starts without a seen
    // request (defined with the adoption).
    void AdoptPoseV2StartedSpeech(const replay::audio::SpeechLineStart& start, std::uint32_t atMs);

    void RecordPoseV2SpeechCreate(const replay::audio::SpeechCoreCall& call, bool byStart)
    {
        // A line created at an address the early table holds: that early line
        // is gone (rule 1).
        if (call.queued && call.line)
            DropPoseV2EarlyLine(call.line);
        if (!call.queued || !call.lineContext)
            return;
        // Rule 1 first, for every created line (a ped not recorded too): the
        // address no longer holds what the recorder had there.
        if (call.line && byStart)
        {
            std::vector<PoseV2SpeechMatcher::Drop> drops;
            std::vector<PoseV2SpeechMatcher::StartedDrop> startedDrops;
            g_poseV2SpeechMatcher.Created(call.line, drops, startedDrops);
            const std::uint32_t atMs = PoseV2RecordMs(call.timeUs);
            LogPoseV2SpeechDrops(drops, atMs);
            LogPoseV2SpeechStartedReused(startedDrops, atMs);
        }
        const replay::recorder::RecordedEntity* speaker = nullptr;
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (!entity.vehicle && replay::entities::Exists(entity.handle) &&
                replay::audio::SpeechEntity(replay::entities::AddressOf(entity.handle)) == call.speechEntity)
            {
                speaker = &entity;
                break;
            }
        }
        if (!speaker)
            return;
        PoseV2PendingSpeech pending;
        replay::entity::SpeechEvent& line = pending.event;
        line.coreMs = PoseV2RecordMs(call.timeUs);
        line.timeMs = line.coreMs;
        line.speaker = speaker->id;
        line.voiceHash = call.lineVoice;
        line.contextPartialHash = call.lineContext;
        line.variation = call.lineVariation;
        line.requestedContextPartialHash = call.requestedContext;
        line.voiceObject = call.voiceObject;
        line.voiceObjectType = call.voiceObjectType;
        line.voiceObjectPartial = call.voiceObjectPartial;
        line.dataType = call.dataType;
        line.request = call.request;
        line.path = call.callers[0];
        pending.line = call.line;
        std::ostream& log = Ctx().log;
        log << "POSE_V2_SPEECH_EVENT,ped=" << replay::entity::IdText(speaker->id)
            << ",voice=0x" << std::hex << call.lineVoice
            << ",context=0x" << replay::audio::FinalizeHash(call.lineContext)
            << ",requested=0x" << replay::audio::FinalizeHash(call.requestedContext) << std::dec
            << ",variation=" << call.lineVariation
            << ",voice_obj_type=0x" << std::hex << static_cast<unsigned>(call.voiceObjectType)
            << ",line=0x" << call.line
            << ",caller=0x" << call.callers[0] << std::dec
            << ",core_ms=" << line.coreMs
            << ",timing=" << (byStart && call.line ? "wait_start" : "core")
            << '\n';
        log.flush();
        TrackPoseV2QueuedLine(speaker->handle, speaker->id, call.lineContext, "record");
        if (byStart && call.line)
            g_poseV2SpeechMatcher.AddPending(call.line, PoseV2SpeechKey(line), pending);
        else
            AddPoseV2SpeechEvent(pending, "core"); // no start detour: timed by the request
    }

    void RecordPoseV2SpeechStart(const replay::audio::SpeechLineStart& start)
    {
        replay::speech::rec::LineKey key;
        key.voice = start.voice;
        key.context = start.context;
        key.variation = start.variation;
        PoseV2SpeechMatcher::Entry entry;
        std::vector<PoseV2SpeechMatcher::Drop> drops;
        const replay::speech::rec::StartResult result =
            g_poseV2SpeechMatcher.Start(start.line, start.identityRead, key, entry, drops);
        const std::uint32_t atMs = PoseV2RecordMs(start.timeUs);
        if (result == replay::speech::rec::StartResult::Mismatch)
        {
            for (const PoseV2SpeechMatcher::Drop& drop : drops)
            {
                const replay::entity::SpeechEvent& old = drop.entry.value.event;
                Ctx().log << "POSE_V2_SPEECH_START_MISMATCH,ped=" << replay::entity::IdText(old.speaker)
                          << ",line=0x" << std::hex << start.line
                          << ",pending_voice=0x" << old.voiceHash
                          << ",pending_context=0x" << replay::audio::FinalizeHash(old.contextPartialHash) << std::dec
                          << ",pending_variation=" << old.variation
                          << std::hex << ",start_voice=0x" << start.voice
                          << ",start_context=0x" << replay::audio::FinalizeHash(start.context) << std::dec
                          << ",start_variation=" << start.variation
                          << ",core_ms=" << old.coreMs << ",start_ms=" << atMs << '\n';
            }
            LogPoseV2SpeechDrops(drops, atMs);
            return;
        }
        if (result == replay::speech::rec::StartResult::NoPending)
        {
            // A line asked before the recording (or whose request was not
            // seen) that starts now.
            AdoptPoseV2StartedSpeech(start, atMs);
            return;
        }
        if (result != replay::speech::rec::StartResult::Matched)
            return;
        PoseV2PendingSpeech started = entry.value;
        started.event.timeMs = atMs;
        started.event.timedByStart = true;
        AddPoseV2SpeechEvent(started, !start.identityRead ? "start_identity_unread"
                : start.stateAfter == 5                     ? "start"
                                                            : "start_state_other");
    }

    void RecordPoseV2SpeechStop(const replay::audio::SpeechLineStop& stop)
    {
        DropPoseV2EarlyLine(stop.line); // an early line stopped before its start was never heard
        PoseV2SpeechMatcher::Entry entry;
        if (g_poseV2SpeechMatcher.StopPending(stop.line, entry))
        {
            char reason[16]{};
            std::snprintf(reason, sizeof(reason), "%u", stop.reason);
            LogPoseV2SpeechUnheard(entry.value, reason, static_cast<std::uint32_t>(stop.state), true, "", 0);
            return;
        }
        std::size_t eventIndex = 0;
        if (!g_poseV2SpeechMatcher.StopStarted(stop.line, eventIndex))
            return;
        if (eventIndex >= Ctx().clip.speechLines.Events().size())
            return;
        if (stop.reason == 1)
        {
            // The natural end, so a jump into the line knows how much is left
            // (speech_resume.h, SPCH v2 endMs).
            replay::entity::SpeechEvent& ended = Ctx().clip.speechLines.At(eventIndex);
            ended.endMs = std::max(PoseV2RecordMs(stop.timeUs), ended.timeMs);
            return;
        }
        replay::entity::SpeechEvent& line = Ctx().clip.speechLines.At(eventIndex);
        line.stopMs = std::max(PoseV2RecordMs(stop.timeUs), line.timeMs);
        line.stopReason = stop.reason;
        std::ostream& log = Ctx().log;
        log << "POSE_V2_SPEECH_STOP,phase=record,ped=" << replay::entity::IdText(line.speaker)
            << ",line=0x" << std::hex << stop.line
            << ",context=0x" << replay::audio::FinalizeHash(line.contextPartialHash) << std::dec
            << ",variation=" << line.variation
            << ",reason=" << stop.reason << ",state=" << stop.state
            << ",event_ms=" << line.timeMs << ",stop_ms=" << line.stopMs
            << ",callers=0x" << std::hex << stop.callers[0] << "/0x" << stop.callers[1] << std::dec
            << '\n';
        log.flush();
    }

    // ---- Lines playing when the recording begins. Taken only from its
    // creation, a line already audible would never enter the clip. A replay
    // regrabs a playing line with the time already played as its start
    // offset; here the start detour's ring (it records with or without a
    // recording) gives each line still audible when the recording begins and
    // when it started, and the line is adopted at the recording's first
    // ticks with startedBeforeMs (SPCH v3), played with the offset.
    struct PoseV2AdoptCandidate
    {
        std::uint64_t line = 0;
        std::uint64_t startUs = 0;
        std::uint32_t voice = 0;
        std::uint32_t context = 0;
        std::int32_t variation = 0;
        // A line asked before the recording whose audible start came before
        // its speaker was recorded waits here too (Queued), with what its
        // request asked.
        replay::speech::rec::AdoptWhy why = replay::speech::rec::AdoptWhy::Playing;
        std::uint32_t requestedContext = 0;
        std::uint32_t askedPath = 0;
        std::uint64_t createdUs = 0;
    };
    std::vector<PoseV2AdoptCandidate> g_poseV2AdoptCandidates;
    std::uint64_t g_poseV2AdoptUntilUs = 0;
    std::uint64_t g_poseV2SpeechAdopted = 0;
    std::uint64_t g_poseV2SpeechAdoptDropped = 0;   // found on a recorded speaker but no longer audible or another line
    std::uint64_t g_poseV2SpeechAdoptUnmatched = 0; // no recorded speaker holds it
    constexpr std::uint64_t kPoseV2AdoptLookbackMs = 30000; // older starts are not looked at
    constexpr std::uint64_t kPoseV2AdoptWindowMs = 3000;    // the speaker must be recorded by then
    // ---- Lines asked before the recording (a conversation that began
    // before it) and any other audible start without a seen request, adopted
    // at their start (speech_record_match.h AdoptWhy). The line allocator
    // has one caller (a chunk entered only from the speech core), so every
    // line that starts was
    // created by a core call: a start the recorder holds nothing for is a
    // line created before the recording (the stale ring still lists it:
    // Queued; it fell out of the ring: Unrequested) or while the recording
    // gate discarded the rings.
    replay::speech::rec::EarlyLines g_poseV2EarlyLines;
    std::uint64_t g_poseV2SpeechEarlyAtStart = 0;        // lines waiting for their start when the recording began
    std::uint64_t g_poseV2SpeechAdoptedQueued = 0;       // asked before the recording, adopted at their start
    std::uint64_t g_poseV2SpeechAdoptedUnrequested = 0;  // no request seen, adopted at their start
    std::uint64_t g_poseV2SpeechStartAdoptDropped = 0;   // on a recorded speaker, no longer playing or another identity
    std::uint64_t g_poseV2SpeechQueuedUnmatched = 0;     // asked before the recording, its speaker not recorded
    replay::log::LogLimit g_poseV2SpeechAdoptDropLogs;
    constexpr std::uint64_t kPoseV2EarlyLookbackMs = 120000; // creations older than this are not held
    }

    void CollectPoseV2AdoptCandidates(const std::vector<replay::audio::SpeechLineStart>& starts,
        const std::vector<replay::audio::SpeechLineStop>& stops)
    {
        g_poseV2AdoptCandidates.clear();
        g_poseV2SpeechAdopted = 0;
        g_poseV2SpeechAdoptDropped = 0;
        g_poseV2SpeechAdoptUnmatched = 0;
        g_poseV2SpeechAdoptedQueued = 0;
        g_poseV2SpeechAdoptedUnrequested = 0;
        g_poseV2SpeechStartAdoptDropped = 0;
        g_poseV2SpeechQueuedUnmatched = 0;
        const std::uint64_t nowUs = replay::audio::NowUs();
        g_poseV2AdoptUntilUs = nowUs + kPoseV2AdoptWindowMs * 1000;
        if (!replay::audio::SpeechStartObserved())
            return;
        for (const replay::audio::SpeechLineStart& start : starts)
        {
            if (!start.line || !start.identityRead || start.stateAfter != 5 || start.timeUs > nowUs ||
                (nowUs - start.timeUs) / 1000 > kPoseV2AdoptLookbackMs)
                continue;
            bool stopped = false;
            for (const replay::audio::SpeechLineStop& stop : stops)
                stopped = stopped || (stop.line == start.line && stop.timeUs >= start.timeUs);
            if (stopped)
                continue;
            // The newest start of an address wins (an address is reused).
            bool replaced = false;
            for (PoseV2AdoptCandidate& candidate : g_poseV2AdoptCandidates)
                if (candidate.line == start.line)
                {
                    candidate = {start.line, start.timeUs, start.voice, start.context, start.variation};
                    replaced = true;
                }
            if (!replaced)
                g_poseV2AdoptCandidates.push_back({start.line, start.timeUs, start.voice, start.context, start.variation});
        }
    }
    namespace
    {

    // One adopted line (a line already playing at the recording start, or a
    // line whose audible start the recorder held nothing for): its identity and
    // request read from the line itself, timed at `timeMs` with the time it had
    // already played.
    void AddPoseV2AdoptedLine(const replay::recorder::RecordedEntity& entity, const replay::audio::PlayedLine& got,
        std::uint64_t linePtr, replay::speech::rec::AdoptWhy why, std::uint32_t timeMs, std::uint32_t startedBeforeMs,
        std::uint32_t requestedContext, std::uint32_t askedPath, std::uint64_t createdUs, std::uint64_t startUs)
    {
        using replay::speech::rec::AdoptWhy;
        PoseV2PendingSpeech pending;
        replay::entity::SpeechEvent& line = pending.event;
        line.timeMs = timeMs;
        line.coreMs = timeMs;
        line.timedByStart = true;
        line.startedBeforeMs = startedBeforeMs;
        line.speaker = entity.id;
        line.voiceHash = got.voice;
        line.contextPartialHash = got.context;
        line.variation = got.variation;
        line.requestedContextPartialHash = requestedContext ? requestedContext : got.context;
        line.voiceObject = got.voiceObject;
        line.voiceObjectType = got.voiceObjectType;
        line.voiceObjectPartial = got.voiceObjectPartial;
        line.dataType = got.dataType;
        line.request = got.request; // with the set and duck bits
        pending.line = linePtr;
        AddPoseV2SpeechEvent(pending, why == AdoptWhy::Playing ? "adopted" : replay::speech::rec::AdoptWhyName(why));
        ++g_poseV2SpeechAdopted;
        if (why == AdoptWhy::Queued)
            ++g_poseV2SpeechAdoptedQueued;
        else if (why == AdoptWhy::Unrequested)
            ++g_poseV2SpeechAdoptedUnrequested;
        std::ostream& log = Ctx().log;
        log << "POSE_V2_SPEECH_ADOPT,ped=" << replay::entity::IdText(entity.id) << ",line=0x" << std::hex << linePtr
            << ",context=0x" << replay::audio::FinalizeHash(got.context) << std::dec << ",variation=" << got.variation
            << ",started_before_ms=" << startedBeforeMs << ",event_ms=" << timeMs
            << ",why=" << replay::speech::rec::AdoptWhyName(why) << ",voice=0x" << std::hex << got.voice << std::dec
            << ",type=" << got.dataType;
        if (why == AdoptWhy::Queued)
            log << ",asked_path=" << replay::speech::PathName(askedPath, replay::audio::SpeechCorePaths())
                << ",requested=0x" << std::hex
                << replay::audio::FinalizeHash(line.requestedContextPartialHash) << std::dec
                << ",asked_before_start_ms=" << (startUs > createdUs ? (startUs - createdUs) / 1000 : 0);
        log << ",set=0x" << std::hex << (got.request.setRead ? got.request.setRef : 0u) << std::dec
            << ",duck=" << replay::speech::conversation::RecordedDuck(got.request) << '\n';
    }

    void AdoptPoseV2PlayingSpeech()
    {
        if (g_poseV2AdoptCandidates.empty())
            return;
        const std::uint64_t nowUs = replay::audio::NowUs();
        for (std::size_t n = 0; n < g_poseV2AdoptCandidates.size();)
        {
            const PoseV2AdoptCandidate candidate = g_poseV2AdoptCandidates[n];
            bool settled = false; // adopted, or no longer adoptable
            for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
            {
                if (entity.vehicle || !replay::entities::Exists(entity.handle))
                    continue;
                void* address = replay::entities::AddressOf(entity.handle);
                replay::audio::PlayedLine got;
                if (!replay::audio::ReadSpeechLineIdentity(address, candidate.line, got))
                    continue; // not this speaker's line
                settled = true;
                std::int32_t state = -1;
                if (!replay::audio::SpeechLineState(address, candidate.line, state) || state != 5 ||
                    got.voice != candidate.voice || got.context != candidate.context || got.variation != candidate.variation)
                {
                    ++(candidate.why == replay::speech::rec::AdoptWhy::Playing ? g_poseV2SpeechAdoptDropped
                                                                               : g_poseV2SpeechStartAdoptDropped);
                    break;
                }
                AddPoseV2AdoptedLine(entity, got, candidate.line, candidate.why, PoseV2RecordMs(nowUs),
                    static_cast<std::uint32_t>((nowUs - candidate.startUs) / 1000), candidate.requestedContext,
                    candidate.askedPath, candidate.createdUs, candidate.startUs);
                break;
            }
            if (settled)
            {
                g_poseV2AdoptCandidates.erase(g_poseV2AdoptCandidates.begin() + static_cast<std::ptrdiff_t>(n));
                continue;
            }
            ++n;
        }
        if (nowUs > g_poseV2AdoptUntilUs && !g_poseV2AdoptCandidates.empty())
        {
            // Lines of speakers not recorded (far away, a ped gone): left out.
            for (const PoseV2AdoptCandidate& candidate : g_poseV2AdoptCandidates)
                ++(candidate.why == replay::speech::rec::AdoptWhy::Playing ? g_poseV2SpeechAdoptUnmatched
                                                                           : g_poseV2SpeechQueuedUnmatched);
            g_poseV2AdoptCandidates.clear();
        }
    }
    }

    // At the recording start, the lines the stale core ring shows created and
    // not yet started nor stopped (the start and stop rings record with or without
    // a recording; the core detour too, it has no enabled check). `core`, `starts`
    // and `stops` are what the rings held before the recording.
    void CollectPoseV2EarlyLines(const std::vector<replay::audio::SpeechCoreCall>& core,
        const std::vector<replay::audio::SpeechLineStart>& starts, const std::vector<replay::audio::SpeechLineStop>& stops)
    {
        g_poseV2EarlyLines.Clear();
        g_poseV2SpeechEarlyAtStart = 0;
        if (!replay::audio::SpeechStartObserved())
            return;
        const std::uint64_t nowUs = replay::audio::NowUs();
        std::vector<replay::speech::rec::TimedRef> timeline;
        for (std::size_t n = 0; n < core.size(); ++n)
            timeline.push_back({core[n].timeUs, replay::speech::rec::EventKind::Create, n});
        for (std::size_t n = 0; n < starts.size(); ++n)
            timeline.push_back({starts[n].timeUs, replay::speech::rec::EventKind::Start, n});
        for (std::size_t n = 0; n < stops.size(); ++n)
            timeline.push_back({stops[n].timeUs, replay::speech::rec::EventKind::Stop, n});
        replay::speech::rec::SortTimeline(timeline);
        for (const replay::speech::rec::TimedRef& ref : timeline)
        {
            if (ref.kind == replay::speech::rec::EventKind::Create)
            {
                const replay::audio::SpeechCoreCall& call = core[ref.index];
                if (!call.queued || !call.line || !call.lineContext)
                    continue;
                if (call.timeUs > nowUs || (nowUs - call.timeUs) / 1000 > kPoseV2EarlyLookbackMs)
                {
                    g_poseV2EarlyLines.Drop(call.line); // the address holds a newer line than any held there
                    continue;
                }
                replay::speech::rec::EarlyLine early;
                early.line = call.line;
                early.key.voice = call.lineVoice;
                early.key.context = call.lineContext;
                early.key.variation = call.lineVariation;
                early.createdUs = call.timeUs;
                early.requestedContext = call.requestedContext;
                early.path = call.callers[0];
                g_poseV2EarlyLines.Created(early);
            }
            else if (ref.kind == replay::speech::rec::EventKind::Start)
                g_poseV2EarlyLines.Drop(starts[ref.index].line); // playing: the adoption above takes it
            else
                g_poseV2EarlyLines.Drop(stops[ref.index].line);
        }
        g_poseV2SpeechEarlyAtStart = g_poseV2EarlyLines.Lines().size();
        std::uint64_t oldestMs = 0;
        for (const replay::speech::rec::EarlyLine& early : g_poseV2EarlyLines.Lines())
            oldestMs = std::max<std::uint64_t>(oldestMs, nowUs > early.createdUs ? (nowUs - early.createdUs) / 1000 : 0);
        Ctx().log << "POSE_V2_SPEECH_EARLY,waiting=" << g_poseV2SpeechEarlyAtStart << ",oldest_ms=" << oldestMs
                  << ",core_seen=" << core.size() << ",starts_seen=" << starts.size() << ",stops_seen=" << stops.size()
                  << '\n';
    }

    void DropPoseV2EarlyLine(std::uint64_t line)
    {
        if (line)
            g_poseV2EarlyLines.Drop(line);
    }
    namespace
    {

    // An audible start the recorder held no line for (NoPending) on a recorded
    // speaker is adopted from the line at that start (speech_record_match.h
    // AdoptsStart).
    void AdoptPoseV2StartedSpeech(const replay::audio::SpeechLineStart& start, std::uint32_t atMs)
    {
        using namespace replay::speech::rec;
        if (!AdoptsStart(StartResult::NoPending, g_poseV2SpeechMatcher.IsStarted(start.line), start.identityRead,
                start.stateAfter))
        {
            return;
        }
        LineKey key;
        key.voice = start.voice;
        key.context = start.context;
        key.variation = start.variation;
        EarlyLine early;
        const bool queued = g_poseV2EarlyLines.Take(start.line, key, early);
        const AdoptWhy why = queued ? AdoptWhy::Queued : AdoptWhy::Unrequested;
        for (const replay::recorder::RecordedEntity& entity : Ctx().recorder.entities)
        {
            if (entity.vehicle || !replay::entities::Exists(entity.handle))
                continue;
            void* address = replay::entities::AddressOf(entity.handle);
            replay::audio::PlayedLine got;
            if (!replay::audio::ReadSpeechLineIdentity(address, start.line, got))
                continue; // not this speaker's line
            std::int32_t state = -1;
            if (!replay::audio::SpeechLineState(address, start.line, state) || state != 5 || got.voice != start.voice ||
                got.context != start.context || got.variation != start.variation)
            {
                ++g_poseV2SpeechStartAdoptDropped;
                if (g_poseV2SpeechAdoptDropLogs.Take(16))
                    Ctx().log << "POSE_V2_SPEECH_ADOPT_DROP,ped=" << replay::entity::IdText(entity.id) << ",line=0x"
                              << std::hex << start.line << ",context=0x" << replay::audio::FinalizeHash(start.context)
                              << ",got_context=0x" << replay::audio::FinalizeHash(got.context) << std::dec
                              << ",state=" << state << ",why=" << AdoptWhyName(why) << ",start_ms=" << atMs << '\n';
                return;
            }
            AddPoseV2AdoptedLine(entity, got, start.line, why, atMs, 0, queued ? early.requestedContext : 0,
                queued ? early.path : 0, queued ? early.createdUs : 0, start.timeUs);
            return;
        }
        // No recorded speaker holds it yet: a line asked before the recording
        // waits for its speaker like a playing one (the first ticks record the
        // scene); any other start is a ped not recorded.
        if (!queued)
            return;
        if (replay::audio::NowUs() < g_poseV2AdoptUntilUs)
        {
            PoseV2AdoptCandidate candidate;
            candidate.line = start.line;
            candidate.startUs = start.timeUs;
            candidate.voice = start.voice;
            candidate.context = start.context;
            candidate.variation = start.variation;
            candidate.why = AdoptWhy::Queued;
            candidate.requestedContext = early.requestedContext;
            candidate.askedPath = early.path;
            candidate.createdUs = early.createdUs;
            g_poseV2AdoptCandidates.push_back(candidate);
        }
        else
            ++g_poseV2SpeechQueuedUnmatched;
    }
    }

    void RecordPoseV2SpeechEvents()
    {
        REPLAY_PROFILE_ZONE("rec.speech");
        AdoptPoseV2PlayingSpeech();
        static std::vector<replay::audio::SpeechCoreCall> calls;
        static std::vector<replay::audio::SpeechLineStart> starts;
        static std::vector<replay::audio::SpeechLineStop> stops;
        static std::vector<replay::speech::rec::TimedRef> timeline;
        replay::audio::CollectSpeechCoreSince(g_poseV2CoreCursor, calls);
        replay::audio::CollectSpeechStartsSince(g_poseV2SpeechStartCursor, starts);
        replay::audio::CollectSpeechStopsSince(g_poseV2SpeechStopCursor, stops);
        const bool byStart = replay::audio::SpeechStartObserved();
        // The three rings in time order, so a line created at an address after
        // another line there started or stopped is applied after it.
        timeline.clear();
        for (std::size_t n = 0; n < calls.size(); ++n)
            timeline.push_back({calls[n].timeUs, replay::speech::rec::EventKind::Create, n});
        for (std::size_t n = 0; n < starts.size(); ++n)
            timeline.push_back({starts[n].timeUs, replay::speech::rec::EventKind::Start, n});
        for (std::size_t n = 0; n < stops.size(); ++n)
            timeline.push_back({stops[n].timeUs, replay::speech::rec::EventKind::Stop, n});
        replay::speech::rec::SortTimeline(timeline);
        for (const replay::speech::rec::TimedRef& ref : timeline)
        {
            if (ref.kind == replay::speech::rec::EventKind::Create)
                RecordPoseV2SpeechCreate(calls[ref.index], byStart);
            else if (ref.kind == replay::speech::rec::EventKind::Start)
                RecordPoseV2SpeechStart(starts[ref.index]);
            else
                RecordPoseV2SpeechStop(stops[ref.index]);
        }
    }

    // End of recording: lines created but never heard are dropped (a
    // scripted line that was never played, an interaction option never
    // chosen); logged. There is no preload event, so nothing goes in the clip.
    void FlushPoseV2PendingSpeech()
    {
        std::ostream& log = Ctx().log;
        for (const PoseV2SpeechMatcher::Entry& entry : g_poseV2SpeechMatcher.TakePendings())
            LogPoseV2SpeechUnheard(entry.value, "none", 0, false, "record_end", 0);
        const replay::speech::rec::Counters& count = g_poseV2SpeechMatcher.Count();
        std::size_t playingLeft = 0; // candidates still waiting, by kind
        for (const PoseV2AdoptCandidate& candidate : g_poseV2AdoptCandidates)
            playingLeft += candidate.why == replay::speech::rec::AdoptWhy::Playing ? 1 : 0;
        log << "POSE_V2_SPEECH_RECORD_TOTAL,recorded=" << Ctx().clip.speechLines.Events().size()
            << ",unheard_dropped=" << g_poseV2SpeechDroppedUnheard
            << ",reused_dropped=" << count.reusedPending
            << ",reused_started=" << count.reusedStarted
            << ",start_mismatch=" << count.startMismatch
            << ",start_identity_unread=" << count.startUnread
            << ",adopted=" << g_poseV2SpeechAdopted << ",adopt_dropped=" << g_poseV2SpeechAdoptDropped
            << ",adopt_unmatched=" << g_poseV2SpeechAdoptUnmatched + playingLeft
            << ",timing=" << (replay::audio::SpeechStartObserved() ? "start" : "core")
            // Lines asked before the recording and starts without a seen request
            // (both in adopted).
            << ",early_at_start=" << g_poseV2SpeechEarlyAtStart
            << ",adopted_queued=" << g_poseV2SpeechAdoptedQueued
            << ",adopted_unrequested=" << g_poseV2SpeechAdoptedUnrequested
            << ",start_adopt_dropped=" << g_poseV2SpeechStartAdoptDropped
            << ",queued_unmatched=" << g_poseV2SpeechQueuedUnmatched + (g_poseV2AdoptCandidates.size() - playingLeft)
            << ",early_left=" << g_poseV2EarlyLines.Lines().size()
            << '\n';
        log.flush();
        g_poseV2SpeechMatcher.Clear();
        g_poseV2EarlyLines.Clear();
    }

    // Every foot-down event the engine queued for a recorded ped becomes a
    // footstep event of that entity.
    // Wetness and dirt of every recorded ped, 5 Hz, stored when it changes.
    std::uint32_t g_poseV2SurfaceNextMs = 0;
    std::map<std::uint32_t, replay::blood::Surface> g_poseV2SurfaceLast;
    std::uint64_t g_poseV2SurfacesRecorded = 0;
    std::uint64_t g_poseV2SurfacesUnreadable = 0;
}
