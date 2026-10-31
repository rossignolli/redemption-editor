// replay_events_speech.cpp: speech played back: requests, slots, animal banks, followed lines, resume after a jump.
// Split from replay_events.cpp (moved code); shared internals in replay_events_internal.h.

#include "replay_events_internal.h"

namespace replay::events
{
    namespace
    {
    // The follow moved the hidden player within the last second (not skipped
    // for a mission or a vehicle, not off).
    bool PlayerFollowMoving()
    {
        return g_playerFollowMoving && GetTickCount64() - g_playerFollowAtMs < 1000;
    }
    constexpr std::uint32_t kSpeechConversationLogCap = 400;
    bool g_speechConversationChecked = false;
    }

    void TrackPoseV2QueuedLine(Ped ped, replay::entity::ReplayId id, std::uint32_t context,
        const char* phase = "playback")
    {
        if (g_poseV2QueuedLineBudget <= 0)
            return;
        std::uint64_t list = 0;
        std::uint16_t count = 0;
        if (!PoseV2SpeechLineList(ped, list, count) || !list || count == 0 || count > 64)
            return;
        PoseV2QueuedLine queued;
        queued.ped = ped;
        queued.id = id;
        queued.context = context;
        queued.phase = phase;
        queued.queuedUs = replay::audio::NowUs();
        if (!replay::audio::ReadU64(list + (count - 1u) * 8u, queued.line) || !queued.line)
            return;
        std::int32_t state = -1;
        replay::audio::ReadBytes(queued.line + 0x234, &state, sizeof(state));
        queued.states = "q:" + std::to_string(state) + " ";
        queued.lastState = state;
        --g_poseV2QueuedLineBudget;
        g_poseV2QueuedLines.push_back(std::move(queued));
    }

    void MeasurePoseV2SpeechLoad()
    {
        std::vector<std::uint32_t> loads;
        // The AI path's core return address (speech_identity.h CorePaths).
        const std::uint32_t aiPath = replay::audio::SpeechCorePaths().ai;
        for (const replay::entity::SpeechEvent& line : Ctx().clip.speechLines.Events())
        {
            if (aiPath && line.timedByStart && line.path == aiPath && line.timeMs >= line.coreMs)
                loads.push_back(line.timeMs - line.coreMs);
        }
        g_poseV2SpeechLoadMs = replay::speech::MedianLoad(loads);
        std::ostream& log = Ctx().log;
        log << "POSE_V2_SPEECH_LOAD,ai_lines=" << loads.size() << ",load_ms=" << g_poseV2SpeechLoadMs << '\n';
        log.flush();
    }
    std::vector<PoseV2PlayedSpeech> g_poseV2PlayedSpeech;
    replay::speech::LineCounts g_poseV2SpeechLines;
    namespace
    {

    // The voice status as a static name (PinPoseV2Voice returns a string).
    const char* PoseV2VoiceStatusName(const std::string& status)
    {
        static constexpr const char* kNames[] = {"no_voice", "animal", "pinned_before", "pinned_preload", "pinned_late",
            "voice_failed"};
        for (const char* name : kNames)
            if (status == name)
                return name;
        return "other";
    }

    void NotePoseV2LineCreated(PoseV2PlayedSpeech& follow, std::uint32_t timeMs, const char* path,
        const std::string& voiceStatus)
    {
        follow.requestPlayMs = timeMs;
        follow.path = path;
        follow.voiceStatus = PoseV2VoiceStatusName(voiceStatus);
        g_poseV2SpeechLines.Created(follow.animal);
        replay::theatre::NoteReplayLine(follow.line); // speech_replay_rules.h
    }
    }

    // ---- ambient speech slots (speech_slots.h) ----
    // The replay manages the engine's ambient speech slots: a human line is
    // asked with source 4 (an ambient slot by priority, its own wave) and only
    // when a slot is free; otherwise it waits, takes the slot of a weaker replay
    // line, or is not asked when nobody would hear it. The slot manager is always
    // on and the ambient source always off.
    std::set<std::uint64_t> g_poseV2AmbientFailed; // voice << 32 | context whose ambient line failed to load
    std::vector<PoseV2SlotWait> g_poseV2SlotQueue;
    namespace
    {
    replay::speech::slots::Decision g_poseV2SlotGateLast = replay::speech::slots::Decision::Ask;
    }
    std::uint64_t g_poseV2SlotGateAsks = 0;      // lines the gate let through with slots short (free < reserve)
    std::uint64_t g_poseV2SlotGateWaits = 0;     // lines that waited for a slot
    std::uint64_t g_poseV2SlotGateWaitMaxMs = 0;
    std::uint64_t g_poseV2SlotGateWaitAsked = 0; // ... asked once a slot was free
    std::uint64_t g_poseV2SlotGateEvicted = 0;   // replay lines stopped for a stronger due line
    std::uint64_t g_poseV2SlotGateDropInaudible = 0;
    std::uint64_t g_poseV2SlotGateDropLate = 0;
    std::uint64_t g_poseV2SlotGateDropPlay = 0;  // waiting lines dropped by a pause or a jump
    std::uint64_t g_poseV2SlotIdleReleased = 0;  // slot releases on a puppet's idle slots
    std::uint64_t g_poseV2SlotIdleFailed = 0;
    std::uint64_t g_poseV2SlotCensusUnread = 0;
    int g_poseV2SlotMinFree = -1;
    std::uint64_t g_poseV2SourceAmbient = 0;     // lines created with source 4
    std::uint64_t g_poseV2SourceBank = 0;        // ... that the engine turned to 3 (bank resident)
    std::uint64_t g_poseV2SourceOther = 0;
    std::uint64_t g_poseV2AmbientFallbacks = 0;  // voice/context sent back to the metadata's source
    std::uint64_t g_poseV2AmbientKilledRetries = 0;       // ambient lines killed loading, asked again
    std::uint64_t g_poseV2AmbientKilledRetryCreated = 0;
    std::uint64_t g_poseV2AmbientRefusedRetries = 0;     // ambient refusals asked again
    std::uint64_t g_poseV2AmbientRefusedRetryCreated = 0; // ... and created with the metadata's source
    namespace
    {
    replay::log::LogLimit g_poseV2SlotLogs;
    constexpr std::uint32_t kPoseV2SlotLogCap = 120;
    }
    std::uint64_t g_poseV2State3Cursor = 0;
    std::map<std::uint64_t, std::uint32_t> g_poseV2State3Codes; // line -> failure code (bounded)
    std::map<std::uint32_t, std::uint64_t> g_poseV2State3ByCode;
    namespace
    {

    std::uint64_t PoseV2AmbientKey(const replay::entity::SpeechEvent& line)
    {
        return (static_cast<std::uint64_t>(line.voiceHash) << 32) | line.contextPartialHash;
    }

    // The source the request carries for this line (0: the metadata's).
    std::int32_t PoseV2RequestSource(const replay::entity::SpeechEvent& line, bool animal)
    {
        return replay::speech::slots::RequestSource(false, animal,
            g_poseV2AmbientFailed.count(PoseV2AmbientKey(line)) != 0);
    }

    void NotePoseV2LineSource(std::int32_t source)
    {
        if (source == replay::speech::slots::kSourceAmbient)
            ++g_poseV2SourceAmbient;
        else if (source == replay::speech::slots::kSourceBank)
            ++g_poseV2SourceBank;
        else
            ++g_poseV2SourceOther;
    }

    // The replay's priority of a speaker: distance to the rendered camera
    // (the listener) and whether it is on screen.
    float PoseV2SpeakerPriority(Ped ped, const Vector3& camera, bool& audible, bool& onScreen, float& distanceM)
    {
        audible = true;
        onScreen = false;
        distanceM = -1.0f;
        if (!ped || !replay::entities::Exists(ped))
            return 0.0f;
        const Vector3 at = ENTITY::GET_ENTITY_COORDS(ped, TRUE, FALSE);
        const float dx = at.x - camera.x;
        const float dy = at.y - camera.y;
        const float dz = at.z - camera.z;
        distanceM = std::sqrt(dx * dx + dy * dy + dz * dz);
        float sx = 0.0f;
        float sy = 0.0f;
        onScreen = GRAPHICS::GET_SCREEN_COORD_FROM_WORLD_COORD(at.x, at.y, at.z + 0.6f, &sx, &sy) != FALSE;
        audible = replay::speech::slots::Audible(distanceM, kPoseV2AudibleM);
        return replay::speech::slots::Priority(distanceM, onScreen, kPoseV2AudibleM);
    }

    // What is left of a recorded line at the playhead (its end or the
    // estimate, speech_resume.h EndOf).
    std::uint32_t PoseV2RemainingMs(std::size_t index, std::uint32_t timeMs);

    // The gate before a human line is asked. Also gives back the slots a
    // puppet still holds with none of the replay's lines on it (the slot release),
    // and stops the replay's weakest line when a clearly stronger one is due.
    replay::speech::slots::Decision PoseV2SlotGate(std::size_t index, Ped speakerPed, std::uint32_t timeMs,
        std::string& detail);
    }

    std::map<std::string, std::uint64_t> g_poseV2SilentByCause; // in the speech total
    namespace
    {

    // A created line that did not become audible is a speech_silent anomaly
    // (qa_health.h, the POSE_V2_SPEECH_SILENT line reaches the QA ring with
    // Debug=0) and is counted in the total.
    void JudgePoseV2Line(PoseV2PlayedSpeech& played, bool listed, std::int32_t state, std::uint32_t timeMs,
        bool normalPlay)
    {
        if (played.judged)
            return;
        const auto& events = Ctx().clip.speechLines.Events();
        if (played.eventIndex >= events.size())
            return;
        const std::uint64_t nowUs = replay::audio::NowUs();
        replay::speech::LineWatch watch;
        watch.startObserved = replay::audio::SpeechStartObserved();
        watch.started = played.startLogged;
        watch.listed = listed;
        watch.state = state;
        watch.normalPlay = normalPlay;
        watch.audioAgeMs = nowUs > played.requestUs ? (nowUs - played.requestUs) / 1000 : 0;
        watch.playAgeMs = timeMs >= played.requestPlayMs ? timeMs - played.requestPlayMs : 0;
        watch.jumped = replay::speech::PlayheadJumped(played.requestPlayMs, timeMs, watch.audioAgeMs,
            Ctx().transport.Rate());
        watch.windowMs = replay::speech::SilentWindowMs(g_poseV2SpeechLoadMs);
        const replay::speech::LineOutcome outcome = replay::speech::JudgeLine(watch);
        if (outcome == replay::speech::LineOutcome::Pending)
            return;
        played.judged = true;
        g_poseV2SpeechLines.Judged(played.animal, outcome);
        if (outcome == replay::speech::LineOutcome::Started && played.remade)
            ++g_poseV2SpeechLines.startedRemade;
        if (!replay::speech::Silent(outcome))
            return;
        if (played.remade)
            ++g_poseV2SpeechLines.remade;
        const replay::entity::SpeechEvent& line = events[played.eventIndex];
        replay::audio::SpeechLineStop stop;
        const bool stopped = replay::audio::FindLineStop(played.line, stop);
        // The replay's other lines alive now (loading 1 to 4, playing 5), for the
        // wave slot refusals.
        int loading = 0;
        int playing = 0;
        for (const PoseV2PlayedSpeech& other : g_poseV2PlayedSpeech)
        {
            std::int32_t otherState = -1;
            if (&other == &played || !replay::entities::Exists(other.ped) ||
                !replay::audio::SpeechLineState(replay::entities::AddressOf(other.ped), other.line, otherState))
                continue;
            loading += otherState >= 1 && otherState <= 4 ? 1 : 0;
            playing += otherState == 5 ? 1 : 0;
        }
        // Closed theater, E1: the bank of the slot that held the line's wave
        // slot (read now: the slot may have moved on since the stop); E2: the
        // manager's slots in use per class and who holds the ambient ones.
        std::string slotBankText = "none";
        if (stopped && stop.slot)
        {
            int slotClass = -1;
            std::uint32_t bank = 0;
            if (replay::theatre::BankOfWaveSlot(stop.slot, slotClass, bank))
            {
                char text[40]{};
                std::snprintf(text, sizeof(text), "%s:0x%X", replay::theatre::kSlotClasses[slotClass].name, bank);
                slotBankText = text;
            }
        }
        std::string slotCensusText = "na";
        {
            std::vector<std::uint64_t> puppetPeds;
            for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
                if (puppet.clone && replay::entities::Exists(puppet.clone))
                    puppetPeds.push_back(reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(puppet.clone)));
            const replay::theatre::SlotCensus census = replay::theatre::ReadSpeechSlots(puppetPeds.data(), puppetPeds.size());
            if (census.ok)
            {
                char text[96]{};
                std::snprintf(text, sizeof(text), "%u/%u|%u/%u|%u/%u|%u/%u|amb_puppet=%u|amb_world=%u", census.used[0],
                    census.total[0], census.used[1], census.total[1], census.used[2], census.total[2], census.used[3],
                    census.total[3], census.ambientByPuppet, census.ambientByWorld);
                slotCensusText = text;
            }
        }
        std::ostream& log = Ctx().log;
        // The cause named (speech_resume.h SilentCause: e1_wave_missing,
        // e2_no_slot, revalidation, timeout...).
        replay::speech::resume::StopFacts facts;
        facts.found = stopped;
        facts.reason = stopped ? stop.reason : 0;
        facts.fieldsRead = stopped && stop.fieldsRead;
        facts.slot = stopped && stop.slot != 0;
        facts.soundFlagsEE = stopped ? static_cast<std::uint8_t>(stop.soundFlagsEE) : 0;
        facts.dataType = stopped ? static_cast<std::uint32_t>(stop.dataType) : 0;
        const char* cause = replay::speech::resume::SilentCause(outcome == replay::speech::LineOutcome::TimedOut, facts);
        ++g_poseV2SilentByCause[cause];
        // A line asked with the ambient source that died loading its own wave
        // sends this voice and context back to the metadata's source for the rest of
        // the playback (speech_slots.h).
        if (!played.animal && played.askedSource == replay::speech::slots::kSourceAmbient &&
            (std::strcmp(cause, "e1_wave_missing") == 0 || std::strcmp(cause, "killed_loading") == 0) &&
            g_poseV2AmbientFailed.insert(PoseV2AmbientKey(line)).second)
            ++g_poseV2AmbientFallbacks;
        std::int64_t state3Code = -1;
        if (const auto it = g_poseV2State3Codes.find(played.line); it != g_poseV2State3Codes.end())
            state3Code = it->second;
        log << "POSE_V2_SPEECH_SILENT,ped=" << replay::entity::IdText(line.speaker)
            << ",line=0x" << std::hex << played.line << std::dec
            << ",kind=" << (played.animal ? "animal" : "human")
            << ",why=" << replay::speech::OutcomeName(outcome)
            << ",cause=" << cause
            << ",resumed=" << (played.resumed ? 1 : 0) << ",offset_ms=" << played.resumeOffsetMs
            << ",path=" << played.path
            << ",voice_status=" << played.voiceStatus
            << ",state=" << state
            << ",stop_reason=" << (stopped ? static_cast<std::int64_t>(stop.reason) : -1)
            << ",stop_state=" << (stopped ? stop.state : -1)
            << ",alive_ms=" << watch.audioAgeMs
            << ",window_ms=" << watch.windowMs
            // The state 2 checks at the stop.
            << ",slot=" << (stopped && stop.fieldsRead ? (stop.slot ? 1 : 0) : -1)
            << ",voice_ok=" << (stopped && stop.fieldsRead ? (stop.flags2BE & 1) : -1)
            << ",voice_obj=" << (stopped && stop.fieldsRead ? (stop.voiceObject ? 1 : 0) : -1)
            << ",data_type=" << (stopped ? stop.dataType : -1)
            << ",parent=" << (stopped && stop.parentRead ? (stop.parent ? 1 : 0) : -1)
            << ",data_sound=" << (stopped ? (stop.dataSound ? 1 : 0) : -1)
            << ",sound_ee=0x" << std::hex << (stopped ? static_cast<unsigned>(stop.soundFlagsEE) : 0u) << std::dec
            << ",replay_loading=" << loading << ",replay_playing=" << playing
            // Closed theater, evidence E1/E2.
            << ",source=" << (stopped ? stop.source : -1)
            << ",explicit_slot=" << (stopped ? (stop.explicitSlot ? 1 : 0) : -1)
            << ",slot_bank=" << slotBankText
            << ",slots=" << slotCensusText
            << ",remade=" << (played.remade ? 1 : 0)
            // The source asked and got, the state 3 handler's failure code (-1 none
            // seen), the ambient slots free.
            << ",asked_source=" << played.askedSource << ",got_source=" << played.source
            << ",state3_code=" << state3Code
            << ",context=0x" << std::hex << replay::audio::FinalizeHash(line.contextPartialHash)
            << ",voice=0x" << line.voiceHash << std::dec
            << ",event_ms=" << line.timeMs
            << ",playhead_ms=" << timeMs
            << '\n';
        // A person's line created and dead before its audible start is silent in
        // the report's speech section too (horse_sound_report.h SpeechTally::died).
        // Animals keep the killed count above.
        if (!played.animal && normalPlay)
        {
            replay::horsesound::SpeechTally& tally = PoseV2SpeechTally(line.speaker, false);
            replay::horsesound::NoteDied(tally, cause, state3Code);
        }
    }
    }
    std::uint64_t g_poseV2AnimalVocalRetries = 0;
    std::uint64_t g_poseV2AnimalVocalCreated = 0;
    std::uint64_t g_poseV2AnimalNativePlayed = 0;    // lines created by the native path
    std::uint64_t g_poseV2AnimalNativeFallbacks = 0; // animal lines that fell to the direct path
    // The retry above: its entry is fixed in speech_identity.h
    // (CheckAnimalVocalEntry).
    std::unordered_map<Ped, AnimalContextFirst> g_poseV2AnimalContextFirst;
    namespace
    {
    constexpr ULONGLONG kContextFirstMs = 2000;
    constexpr ULONGLONG kContextFirstMaxMs = 16000;
    }
    std::uint32_t g_poseV2AnimalContextRetries = 0; // pinned asked again after a context-first wait

    // ---- animal banks (animal_bank.h) ----
    // The recorded voice of an animal line is the hash of one of its type's
    // banks ("HORSE_NEAR_02"); the speech manager holds one bank per type and
    // rotates them. The replay plans, per type, the bank of the next line due
    // within the lead, has the manager load and keep it (the picker detour,
    // world_theatre_engine.cpp) and asks a line with its recorded voice only
    // while that bank is the type's loaded one (otherwise by context: the pinned
    // line would die loading).
    constexpr std::uint32_t kPoseV2AnimalBankLeadMs = kPoseV2VoicePreloadMs; // the bank loaded this far ahead of the line
    namespace
    {
    replay::animalbank::VoiceMap g_poseV2AnimalBankVoices;
    std::vector<replay::animalbank::Due> g_poseV2AnimalBankDue;
    std::vector<replay::animalbank::HoldState> g_poseV2AnimalBankStates;
    std::vector<replay::animalbank::Hold> g_poseV2AnimalBankPublished;
    std::map<int, std::string> g_poseV2AnimalBankNames; // type -> name (log)
    struct PoseV2AnimalBankCounts
    {
        std::uint64_t asked = 0, loaded = 0, gaveUp = 0, released = 0, releasedUnloaded = 0;
        std::uint64_t retried = 0; // given up, asked again
        std::uint64_t loadWaitMaxMs = 0, loadWaitSumMs = 0;
        // Lines that would be asked with the recorded voice: its bank loaded,
        // not loaded (asked by context instead), or no bank the replay knows.
        std::uint64_t pinnedReady = 0, pinnedMissing = 0, pinnedUnknown = 0;
        // Of the lines asked with the recorded voice: audible starts, deaths
        // while loading (reason 4), split by the bank's state at the request.
        std::uint64_t pinnedStarted = 0, pinnedDiedLoading = 0, pinnedDiedReady = 0, pinnedDiedUnknown = 0;
        // Lines asked by context: the engine gave the recorded voice or another.
        std::uint64_t contextSameVoice = 0, contextOtherVoice = 0;
    };
    PoseV2AnimalBankCounts g_poseV2AnimalBank;
    replay::log::LogLimit g_poseV2AnimalBankLogs;
    constexpr std::uint32_t kPoseV2AnimalBankLogCap = 120;

    std::string PoseV2AnimalBankName(int type)
    {
        const auto it = g_poseV2AnimalBankNames.find(type);
        return it != g_poseV2AnimalBankNames.end() ? it->second : std::string("?");
    }

    // Plans and follows the holds at the playhead and publishes the active
    // ones to the picker detour (script thread; reads only).
    void UpdatePoseV2AnimalBanks(std::uint32_t timeMs)
    {
        if (g_poseV2AnimalBankDue.empty())
            return;
        namespace bank = replay::animalbank;
        const std::vector<bank::Hold> planned = bank::PlanHolds(g_poseV2AnimalBankDue, timeMs, kPoseV2AnimalBankLeadMs);
        std::map<int, replay::theatre::AnimalBankState> seen;
        const auto loadedVoiceOf = [&seen](int type) {
            auto it = seen.find(type);
            if (it == seen.end())
                it = seen.emplace(type, replay::theatre::ReadAnimalBank(type)).first;
            return it->second.holderVoice;
        };
        const std::uint64_t nowMs = GetTickCount64();
        const bank::FollowEvents events = bank::Follow(g_poseV2AnimalBankStates, planned, nowMs, loadedVoiceOf);
        std::ostream& log = Ctx().log;
        const auto line = [&](const char* phase, const bank::Hold& hold, std::uint64_t waitedMs) {
            if (!g_poseV2AnimalBankLogs.Take(kPoseV2AnimalBankLogCap))
                return;
            const replay::theatre::AnimalBankState state = replay::theatre::ReadAnimalBank(hold.type);
            log << "ANIMAL_BANK_HOLD,phase=" << phase << ",type=0x" << std::hex << hold.type << std::dec
                << ",name=" << PoseV2AnimalBankName(hold.type) << ",bank=" << hold.bank
                << ",voice=0x" << std::hex << hold.voice << ",holder_voice=0x" << state.holderVoice << std::dec
                << ",entry_state=" << state.entryState << ",exhausted=" << (state.exhausted ? 1 : 0)
                << ",kept=" << (state.kept ? 1 : 0) << ",demand=" << state.demand << ",next=" << state.next
                << ",count=" << state.count << ",waited_ms=" << waitedMs << ",first_line_ms=" << hold.firstMs
                << ",playhead_ms=" << timeMs << '\n';
        };
        for (const bank::HoldState& state : events.released)
        {
            ++g_poseV2AnimalBank.released;
            g_poseV2AnimalBank.releasedUnloaded += state.loadedMs ? 0 : 1;
            line("release", state.hold, 0);
        }
        for (const bank::Hold& hold : events.asked)
        {
            ++g_poseV2AnimalBank.asked;
            line("ask", hold, 0);
        }
        for (const bank::Hold& hold : events.retried)
        {
            ++g_poseV2AnimalBank.retried;
            line("retry", hold, 0);
        }
        for (const bank::HoldState& state : events.loaded)
        {
            ++g_poseV2AnimalBank.loaded;
            const std::uint64_t waited = state.loadedMs > state.askedMs ? state.loadedMs - state.askedMs : 0;
            g_poseV2AnimalBank.loadWaitSumMs += waited;
            g_poseV2AnimalBank.loadWaitMaxMs = (std::max)(g_poseV2AnimalBank.loadWaitMaxMs, waited);
            line("loaded", state.hold, waited);
        }
        for (const bank::HoldState& state : events.gaveUp)
        {
            ++g_poseV2AnimalBank.gaveUp;
            line("gave_up", state.hold, nowMs > state.askedMs ? nowMs - state.askedMs : 0);
        }
        const std::vector<bank::Hold> active = bank::Active(g_poseV2AnimalBankStates);
        if (active != g_poseV2AnimalBankPublished)
        {
            replay::theatre::SetAnimalBankHolds(active);
            g_poseV2AnimalBankPublished = active;
        }
    }
    }

    // Playback start: the clip's animal lines whose voice is a bank of the
    // engine's animal types, and the first holds (the banks load while the
    // playback gets ready).
    void BeginPoseV2AnimalBanks(std::uint32_t startMs)
    {
        namespace bank = replay::animalbank;
        g_poseV2AnimalBankVoices.clear();
        g_poseV2AnimalBankDue.clear();
        g_poseV2AnimalBankStates.clear();
        g_poseV2AnimalBankPublished.clear();
        g_poseV2AnimalBankNames.clear();
        g_poseV2AnimalBank = {};
        g_poseV2AnimalBankLogs = {};
        replay::theatre::SetAnimalBankHolds({});
        replay::theatre::ResetAnimalBankCounters();
        std::ostream& log = Ctx().log;
        std::string failed = "none";
        const bool checks = replay::theatre::AnimalBankChecks(failed);
        std::vector<bank::TypeBanks> types;
        if (checks)
            replay::theatre::ReadAnimalBankTypes(types);
        for (const bank::TypeBanks& type : types)
            g_poseV2AnimalBankNames[type.type] = type.name;
        g_poseV2AnimalBankVoices = bank::BuildVoiceMap(types);
        struct VoiceUse
        {
            bank::BankOf at;
            std::uint32_t lines = 0, firstMs = 0, lastMs = 0;
        };
        std::map<std::uint32_t, VoiceUse> uses;
        for (const replay::entity::SpeechEvent& speech : Ctx().clip.speechLines.Events())
        {
            const auto it = g_poseV2AnimalBankVoices.find(speech.voiceHash);
            if (!speech.voiceHash || it == g_poseV2AnimalBankVoices.end())
                continue;
            g_poseV2AnimalBankDue.push_back(bank::Due{speech.timeMs, speech.voiceHash, it->second.type, it->second.bank});
            VoiceUse& use = uses[speech.voiceHash];
            if (!use.lines)
                use.firstMs = speech.timeMs;
            use.at = it->second;
            ++use.lines;
            use.firstMs = (std::min)(use.firstMs, speech.timeMs);
            use.lastMs = (std::max)(use.lastMs, speech.timeMs);
        }
        bank::SortDue(g_poseV2AnimalBankDue);
        log << "ANIMAL_BANK_PLAN,checks=" << (checks ? "ok" : failed.c_str())
            << ",installed=" << (replay::theatre::AnimalBankInstalled() ? 1 : 0) << ",types=" << types.size()
            << ",voices=" << g_poseV2AnimalBankVoices.size() << ",lines=" << g_poseV2AnimalBankDue.size()
            << ",distinct=" << uses.size() << ",lead_ms=" << kPoseV2AnimalBankLeadMs << '\n';
        std::size_t listed = 0;
        for (const auto& [voice, use] : uses)
        {
            if (++listed > 16)
                break;
            log << "ANIMAL_BANK_VOICE,voice=0x" << std::hex << voice << ",type=0x" << use.at.type << std::dec
                << ",name=" << PoseV2AnimalBankName(use.at.type) << ",bank=" << use.at.bank << ",lines=" << use.lines
                << ",first_ms=" << use.firstMs << ",last_ms=" << use.lastMs << '\n';
        }
        if (!replay::theatre::AnimalBankInstalled())
            g_poseV2AnimalBankDue.clear(); // no detour: nothing to hold (the pinned gate still reads the banks)
        UpdatePoseV2AnimalBanks(startMs);
    }
    namespace
    {

    // The bank of a line about to be asked with its recorded voice.
    replay::animalbank::PinnedBank PoseV2PinnedBank(const replay::entity::SpeechEvent& line)
    {
        const auto it = g_poseV2AnimalBankVoices.find(line.voiceHash);
        if (it == g_poseV2AnimalBankVoices.end())
            return replay::animalbank::PinnedBank::Unknown;
        return replay::animalbank::JudgePinned(true, line.voiceHash, replay::theatre::ReadAnimalBank(it->second.type).loadedVoice);
    }
    }

    void EndPoseV2AnimalBanks(std::ostream& log)
    {
        replay::theatre::SetAnimalBankHolds({});
        const replay::theatre::AnimalBankCounters engine = replay::theatre::ReadAnimalBankCounters();
        const PoseV2AnimalBankCounts& c = g_poseV2AnimalBank;
        log << "ANIMAL_BANK_HOLD_SUMMARY,installed=" << (replay::theatre::AnimalBankInstalled() ? 1 : 0)
            << ",lines=" << g_poseV2AnimalBankDue.size() << ",asked=" << c.asked << ",loaded=" << c.loaded
            << ",gave_up=" << c.gaveUp << ",retried=" << c.retried << ",released=" << c.released << ",released_unloaded=" << c.releasedUnloaded
            << ",load_wait_avg_ms=" << (c.loaded ? c.loadWaitSumMs / c.loaded : 0) << ",load_wait_max_ms=" << c.loadWaitMaxMs
            << ",picks=" << engine.picks << ",forced=" << engine.forced << ",keeps=" << engine.keeps
            << ",not_kept=" << engine.notKept << ",write_failed=" << engine.writeFailed
            << ",other_manager=" << engine.otherManager
            << ",pinned_bank_ready=" << c.pinnedReady << ",pinned_bank_missing=" << c.pinnedMissing
            << ",pinned_bank_unknown=" << c.pinnedUnknown << ",pinned_started=" << c.pinnedStarted
            << ",pinned_died_loading=" << c.pinnedDiedLoading << ",pinned_died_bank_ready=" << c.pinnedDiedReady
            << ",pinned_died_bank_unknown=" << c.pinnedDiedUnknown
            << ",context_same_voice=" << c.contextSameVoice << ",context_other_voice=" << c.contextOtherVoice << '\n';
        g_poseV2AnimalBankStates.clear();
        g_poseV2AnimalBankPublished.clear();
    }
    // Closed theater: an ambient NPC voice is no voice object, so its line
    // goes with the voice in the request, the engine takes voice source 11 and a
    // fixed shared wave slot (manager +0x730) whose
    // loaded bank seldom holds that wave: the line dies in state 2 (reason 4,
    // sound +0xEE bit 2). A puppet whose line died that way asks its next lines
    // (and that line once more) with no voice: the puppet's own voice from its
    // model's voice group, through the ambient slots that load the line's own
    // wave. Context and variation stay the recorded ones; the voice may differ.
    std::unordered_set<Ped> g_poseV2HumanOwnVoice;
    std::uint64_t g_poseV2OwnVoiceRetries = 0;
    std::uint64_t g_poseV2OwnVoiceRetryCreated = 0;
    namespace
    {
    constexpr std::uint64_t kPoseV2OwnVoiceRetryMaxAgeMs = 1500;

    // A human line killed in state 2 because no wave slot was free (line
    // +0xD0 still 0 at the stop, e.g. a third line while two replayed lines
    // play) is asked again while the window lasts; it starts late (the START
    // line logs the delay) instead of not at all.
    constexpr std::uint64_t kPoseV2SlotWaitMs = 2000;
    constexpr std::uint64_t kPoseV2SlotRetryEveryMs = 100;
    }
    std::uint64_t g_poseV2SlotWaits = 0;
    std::uint64_t g_poseV2SlotRetries = 0;
    std::uint64_t g_poseV2SlotRetryCreated = 0;

    // The start offset of a line asked late (a retry after its start): the
    // playhead past the recorded start, 0 under the minimum offset.

    // The recorded line type and request fields asked again on the direct path
    // (speech_request.h); the comparison of what the engine made.
    // The request fields are always asked.
    replay::speech::request::Tally g_poseV2RequestTally;
    namespace
    {
    // `conversation` false builds the plain request (no recorded duck byte or
    // sound set), the refused line's retry.
    replay::speech::request::Ask PoseV2RequestAsk(const replay::entity::SpeechEvent& line, bool conversation = true)
    {
        replay::speech::request::Ask ask = replay::speech::request::BuildAsk(line.dataType, line.request,
            true, true);
        // Every line gets its recorded duck byte and sound set
        // (SpeechConversationLive=1, speech_conversation.h); a clip without the set
        // in SPRQ takes it from the line's own recorded sounds (an adopted line made
        // them before the recording: none).
        if (g_speechConversationLive && conversation)
        {
            std::uint32_t soundsMs = 0;
            const std::uint32_t soundsRef = line.request.setRead || line.startedBeforeMs ? 0u :
                replay::speech::conversation::SetRefFromSounds(replay::audio::SpeechControllerCallers().layer,
                    Ctx().clip.entitySounds, line.speaker, line.dataType, line.timeMs, soundsMs);
            replay::speech::conversation::Apply(true, line.request, soundsRef, ask);
        }
        return ask;
    }
    std::uint32_t g_poseV2LastAskedType = 0; // the main request's +0xF4, for its log line
    // Counted per replayed human line; the field by field diff once per
    // recorded line type (recorded > replayed, the line's copies).
    void NotePoseV2RequestDiff(const replay::entity::SpeechEvent& line, const replay::audio::PlayedLine& played,
        const replay::speech::request::Ask& ask, std::uint32_t timeMs)
    {
        g_poseV2RequestTally.Note(line.dataType, line.request, played.dataType, played.request, ask);
        if (!g_poseV2RequestTally.TakeLog(line.dataType))
            return;
        Ctx().log << "SPEECH_REQUEST_DIFF,ped=" << replay::entity::IdText(line.speaker)
                  << ",recorded_type=" << line.dataType << ",asked_type=" << ask.lineType
                  << ",asked_fields=" << static_cast<unsigned>(ask.count) << ",got_type=" << played.dataType
                  << ",recorded_read=" << (line.request.read ? 1 : 0)
                  << ",diff=" << replay::speech::request::DiffText(line.dataType, line.request, played.dataType, played.request)
                  << ",context=0x" << std::hex << replay::audio::FinalizeHash(line.contextPartialHash) << std::dec
                  << ",path=" << replay::speech::PathName(line.path, replay::audio::SpeechCorePaths())
                  << ",event_ms=" << line.timeMs << ",playhead_ms=" << timeMs << '\n';
    }
    // Per replayed human line (capped) the line type, the duck bit and the
    // sound set recorded > asked > got, and the playback's counts
    // (speech_conversation.h). The calls' check is logged with the first line.
    void NoteSpeechConversation(const replay::entity::SpeechEvent& line, const replay::audio::PlayedLine& played,
        const replay::speech::request::Ask& ask, std::int32_t askedSource, std::uint32_t timeMs)
    {
        using namespace replay::speech::conversation;
        if (!g_speechConversationChecked)
        {
            g_speechConversationChecked = true;
            std::string failed;
            const bool ok = replay::audio::SpeechSoundSetCallsOk(failed);
            Ctx().log << "SPEECH_CONVERSATION_CHECK,ok=" << (ok ? 1 : 0) << ",failed=" << failed << '\n';
        }
        const SetStatus status = static_cast<SetStatus>(played.setStatus);
        g_speechConversationTally.Note(ask, status, played.request, played.setWrittenRef);
        if (g_speechConversationLogs >= kSpeechConversationLogCap)
            return;
        ++g_speechConversationLogs;
        const replay::speech::request::Fields& rec = line.request;
        Ctx().log << "SPEECH_CONVERSATION,ped=" << replay::entity::IdText(line.speaker)
                  << ",type=" << line.dataType << '>' << ask.lineType << '>' << played.dataType
                  << ",source=" << (rec.read ? static_cast<std::int64_t>(rec.value[replay::speech::request::Source]) : -1)
                  << '>' << askedSource << '>' << played.source
                  << ",duck=" << DuckText(rec, ask, played.request)
                  << ",set_from=" << SetFromName(ask.setFrom) << std::hex
                  << ",set_hash=0x" << ask.setHash
                  << ",set_rec=0x" << (rec.setRead ? rec.setRef : 0u)
                  << ",set_asked=0x" << ask.setRef
                  << ",set_written=0x" << played.setWrittenRef
                  << ",set_got=0x" << (played.request.setRead ? played.request.setRef : 0u) << std::dec
                  << ",set_status=" << SetStatusName(status)
                  << ",context=0x" << std::hex << replay::audio::FinalizeHash(line.contextPartialHash) << std::dec
                  << ",event_ms=" << line.timeMs << ",playhead_ms=" << timeMs << '\n';
    }

    std::uint32_t PoseV2LateOffsetMs(const replay::entity::SpeechEvent& line, std::uint32_t timeMs)
    {
        if (timeMs <= line.timeMs && !line.startedBeforeMs)
            return 0;
        // Includes the time an adopted line had already played (speech_resume.h OffsetAt).
        return replay::speech::resume::OffsetAt(line, timeMs);
    }


    bool RetryPoseV2SlotLine(PoseV2PlayedSpeech& played, const replay::entity::SpeechEvent& line, std::uint32_t timeMs)
    {
        ++g_poseV2SlotRetries;
        ++played.slotRetries;
        void* entity = replay::entities::AddressOf(played.ped);
        const replay::speech::Identity expected = PoseV2SpeechIdentity(line);
        const std::uint32_t requestVoice =
            std::strcmp(played.voiceStatus, "voice_failed") == 0 || played.namedVoice ? line.voiceHash : 0; // named voice
        replay::audio::PlayedLine got;
        std::string detail;
        std::uint32_t result = 0;
        // The retry starts where the recorded line is now (start offset), not from
        // its start late, so the voice stays on the mouth after the wait.
        const std::uint32_t offsetMs = PoseV2LateOffsetMs(line, timeMs);
        // The retries alternate the ambient and the metadata's source.
        const std::int32_t source = replay::speech::slots::RetrySource(false, false,
            g_poseV2AmbientFailed.count(PoseV2AmbientKey(line)) != 0, played.slotRetries);
        replay::speech::request::Ask ask = PoseV2RequestAsk(line);
        const bool called = replay::audio::PlaySpeechDirect(entity, requestVoice, line.contextPartialHash,
            line.variation, &expected, result, got, detail, offsetMs, source, &ask);
        std::int32_t state = -1;
        const bool created = called && result == 74 && got.line && got.identityRead && !got.mismatch &&
            replay::audio::SpeechLineState(entity, got.line, state) && state >= 1 && state <= 5;
        if (created)
        {
            played.askedSource = source;
            played.source = got.source;
            played.resumeOffsetMs = offsetMs;
            ++g_poseV2SlotRetryCreated;
            played.line = got.line;
            replay::theatre::NoteReplayLine(played.line);
            played.requestUs = replay::audio::NowUs();
            played.slotWaitUntilUs = 0;
        }
        const std::uint64_t nowUs = replay::audio::NowUs();
        Ctx().log << "POSE_V2_SPEECH_SLOT_RETRY,ped=" << replay::entity::IdText(line.speaker)
                  << ",try=" << played.slotRetries << ",source=" << source << ",created=" << (created ? 1 : 0)
                  << ",waited_ms=" << (nowUs > played.firstRequestUs ? (nowUs - played.firstRequestUs) / 1000 : 0)
                  << ",status=" << detail << ",offset_ms=" << offsetMs << ",event_ms=" << line.timeMs
                  << ",playhead_ms=" << timeMs << '\n';
        return created;
    }

    // A line the replay started on this puppet is still loading or playing.
    bool PoseV2PuppetSpeaking(Ped ped)
    {
        void* entity = replay::entities::AddressOf(ped);
        for (const PoseV2PlayedSpeech& played : g_poseV2PlayedSpeech)
        {
            std::int32_t state = -1;
            if (played.ped == ped && replay::audio::SpeechLineState(entity, played.line, state) &&
                state >= 1 && state <= 5)
            {
                return true;
            }
        }
        return false;
    }

    // Pins the recorded voice object on the puppet (typed lookup + the
    // engine's apply). Animals are skipped: their voice comes from the
    // animal type, not from a holder (offline evidence).
    std::string PinPoseV2Voice(replay::playback::Puppet& puppet, const replay::entity::SpeechEvent& line,
        std::uint32_t timeMs, const char* when)
    {
        if (!line.voiceHash)
            return "no_voice";
        if (!PED::IS_PED_HUMAN(puppet.clone))
        {
            ++g_poseV2VoiceAnimal;
            return "animal";
        }
        if (puppet.speechVoice == line.voiceHash)
            return "pinned_before";
        if (puppet.failedVoice == line.voiceHash)
            return "voice_failed";
        std::string status;
        const bool pinned = replay::audio::PinSpeechVoice(replay::entities::AddressOf(puppet.clone), line.voiceHash,
            line.voiceObjectType, line.voiceObject, status);
        if (pinned)
        {
            puppet.speechVoice = line.voiceHash;
            ++g_poseV2VoicePinned;
        }
        else
        {
            puppet.failedVoice = line.voiceHash;
            ++g_poseV2VoiceFailed;
        }
        std::ostream& log = Ctx().log;
        log << "POSE_V2_PLAYBACK_VOICE,ped=" << replay::entity::IdText(puppet.packet.id)
            << ",voice=0x" << std::hex << line.voiceHash
            << ",voice_obj_type=0x" << static_cast<unsigned>(line.voiceObjectType) << std::dec
            << ",status=" << status
            << ",when=" << when
            << ",lead_ms=" << (line.timeMs > timeMs ? line.timeMs - timeMs : 0)
            << '\n';
        log.flush();
        return pinned ? "pinned_" + std::string(when) : "voice_failed";
    }

    void PreloadPoseV2Voices(std::uint32_t timeMs)
    {
        for (replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if (puppet.state != replay::playback::PuppetState::Ready || !puppet.clone ||
                !replay::entities::Exists(puppet.clone))
            {
                continue;
            }
            for (const replay::entity::SpeechEvent& line : Ctx().clip.speechLines.Events())
            {
                if (line.speaker != puppet.packet.id || line.timeMs < timeMs)
                    continue;
                if (line.timeMs > timeMs + kPoseV2VoicePreloadMs)
                    break;
                // The puppet's next line. Its voice is not changed under a
                // line of the puppet that is still loading or playing (the
                // change would reach that line too); the line then pins it
                // late, when it is due.
                if (line.voiceHash && puppet.speechVoice != line.voiceHash &&
                    puppet.failedVoice != line.voiceHash && !PoseV2PuppetSpeaking(puppet.clone))
                {
                    PinPoseV2Voice(puppet, line, timeMs, "preload");
                }
                break;
            }
        }
    }

    // Starts of the replayed lines (logged against the recorded start) and
    // the recorded early stops, at their time in normal play.
    // An animal line is asked again with the recorded request through the
    // animal's own vocalization path (audio::PlayAnimalVocal, what the native's
    // handler does with the context hash: the animal vocal entry
    // (speech_identity.h kAnimalVocal) writes it into params +0x150 and enters
    // the Say wrapper with the
    // AI step). The native itself takes a name, which the clip lacks (only the
    // hash). The animal's pipeline (+0x330) picks voice and variation from its
    // type and state, so the phrase may differ: a vocalization of the right kind
    // is preferred over silence. The new line is followed
    // (POSE_V2_SPEECH_LINE_END) and counted.
    std::uint64_t RetryPoseV2AnimalVocal(const PoseV2PlayedSpeech& played, const replay::entity::SpeechEvent& line,
        std::uint32_t timeMs, std::uint64_t ageMs)
    {
        ++g_poseV2AnimalVocalRetries;
        const std::uint32_t context =
            line.requestedContextPartialHash ? line.requestedContextPartialHash : line.contextPartialHash;
        std::uint64_t list = 0;
        std::uint16_t before = 0;
        std::uint64_t lastBefore = 0;
        if (PoseV2SpeechLineList(played.ped, list, before) && list && before > 0 && before <= 64)
            replay::audio::ReadU64(list + (before - 1u) * 8u, lastBefore);
        std::uint32_t result = 0;
        std::string status;
        replay::audio::PlayAnimalVocal(replay::entities::AddressOf(played.ped), context, 0, 0, result, status);
        std::uint16_t after = 0;
        std::uint64_t lastAfter = 0;
        if (PoseV2SpeechLineList(played.ped, list, after) && list && after > 0 && after <= 64)
            replay::audio::ReadU64(list + (after - 1u) * 8u, lastAfter);
        const bool created = lastAfter && lastAfter != lastBefore && lastAfter != played.line;
        if (created)
        {
            ++g_poseV2AnimalVocalCreated;
            TrackPoseV2QueuedLine(played.ped, line.speaker, context);
        }
        std::ostream& log = Ctx().log;
        log << "POSE_V2_ANIMAL_VOCAL,ped=" << replay::entity::IdText(line.speaker)
            << ",context=0x" << std::hex << replay::audio::FinalizeHash(line.contextPartialHash)
            << ",requested=0x" << replay::audio::FinalizeHash(context)
            << ",voice=0x" << line.voiceHash << std::dec
            << ",variation=" << line.variation
            << ",direct_line=0x" << std::hex << played.line << std::dec
            << ",stop_reason=4,age_ms=" << ageMs
            << ",status=" << status
            << ",lines=" << before << "->" << after
            << ",created=" << (created ? 1 : 0)
            << ",event_ms=" << line.timeMs
            << ",playhead_ms=" << timeMs << '\n';
        log.flush();
        return created ? lastAfter : 0;
    }

    // An animal line asked through the native's path (audio::PlayAnimalVocal).
    // Returns the line it created (0 when none): the last entry of the ped's
    // line list, new since the call.
    std::uint64_t PlayPoseV2NativeAnimalLine(Ped ped, std::uint32_t context, std::uint32_t voice,
        std::int32_t variation, std::uint32_t& result, std::string& status)
    {
        std::uint64_t list = 0;
        std::uint16_t before = 0;
        std::uint64_t lastBefore = 0;
        if (PoseV2SpeechLineList(ped, list, before) && list && before > 0 && before <= 64)
            replay::audio::ReadU64(list + (before - 1u) * 8u, lastBefore);
        result = 0;
        if (!replay::audio::PlayAnimalVocal(replay::entities::AddressOf(ped), context, voice, variation, result, status))
            return 0;
        std::uint16_t after = 0;
        std::uint64_t lastAfter = 0;
        if (PoseV2SpeechLineList(ped, list, after) && list && after > 0 && after <= 64)
            replay::audio::ReadU64(list + (after - 1u) * 8u, lastAfter);
        return lastAfter && lastAfter != lastBefore ? lastAfter : 0;
    }
    }

    // What the session heard of each recorded line's end (a clip before SPCH v2
    // has none), the resume counters (POSE_V2_PLAYBACK_SPEECH_TOTAL) and the cut
    // lines.
    std::vector<std::uint32_t> g_poseV2LearnedEndMs;
    replay::speech::resume::Watch g_poseV2SpeechWatch;
    std::uint64_t g_poseV2SpeechResumes = 0;       // Resume steps (jumps, play again)
    std::uint64_t g_poseV2SpeechResumeAsked = 0;   // lines asked again
    std::uint64_t g_poseV2SpeechResumeCreated = 0; // ... that the engine created
    std::uint64_t g_poseV2SpeechResumeWithOffset = 0;
    std::uint64_t g_poseV2SpeechResumeNoPuppet = 0;
    std::uint64_t g_poseV2SpeechResumeAnimalSkipped = 0;
    std::uint64_t g_poseV2SpeechResumeLate = 0;    // asked once the puppet was ready
    std::uint64_t g_poseV2SpeechJumpStops = 0;     // lines the replay stopped for a jump or a pause
    std::uint64_t g_poseV2SpeechLeaves = 0;
    std::uint64_t g_poseV2SpeechCuts = 0;          // audible lines that left early, not by the replay
    std::uint64_t g_poseV2SpeechShortEnds = 0;
    std::uint64_t g_poseV2SpeechLearnedEnds = 0;
    namespace
    {
    replay::log::LogLimit g_poseV2SpeechResumeLogs;
    constexpr std::uint32_t kPoseV2SpeechResumeLogCap = 200;
    constexpr std::uint32_t kPoseV2SpeechCutLogCap = 100;
    replay::log::LogLimit g_poseV2SpeechCutLogs;
    replay::log::LogLimit g_poseV2SpeechSyncLogs;
    constexpr std::uint32_t kPoseV2SpeechSyncLogCap = 3000;
    constexpr std::int64_t kPoseV2SpeechSyncOffMs = 100; // a line counted out of sync past this

    // A replayed line left the speaker's line list: learn the recorded
    // line's end from a natural end heard in continuous normal play, and
    // log a line that was audible and left before its end (POSE_V2_SPEECH_CUT,
    // the evidence for short drops).
    void NotePoseV2LineLeft(const PoseV2PlayedSpeech& played, std::uint32_t timeMs, bool normalPlay)
    {
        const auto& events = Ctx().clip.speechLines.Events();
        if (!played.startLogged || played.eventIndex >= events.size())
            return;
        const replay::entity::SpeechEvent& line = events[played.eventIndex];
        replay::audio::SpeechLineStop stop;
        const bool found = replay::audio::FindLineStop(played.line, stop);
        const std::uint64_t nowUs = replay::audio::NowUs();
        const std::uint32_t heardMs =
            played.startUs && nowUs > played.startUs ? static_cast<std::uint32_t>((nowUs - played.startUs) / 1000) : 0;
        const bool continuous = normalPlay && timeMs >= played.startPlayMs &&
            !replay::speech::PlayheadJumped(played.startPlayMs, timeMs, heardMs, Ctx().transport.Rate());
        if (found && stop.reason == 1 && continuous && !line.endMs && !line.stopMs)
        {
            if (g_poseV2LearnedEndMs.size() < events.size())
                g_poseV2LearnedEndMs.resize(events.size(), 0);
            if (!g_poseV2LearnedEndMs[played.eventIndex])
                ++g_poseV2SpeechLearnedEnds;
            g_poseV2LearnedEndMs[played.eventIndex] = timeMs;
        }
        const replay::speech::resume::LineEnd end = replay::speech::resume::EndOf(line,
            played.eventIndex < g_poseV2LearnedEndMs.size() ? g_poseV2LearnedEndMs[played.eventIndex] : 0, 0);
        const std::uint32_t expectedMs =
            end.known && end.endMs > played.startPlayMs ? end.endMs - played.startPlayMs : 0;
        const bool cut = replay::speech::resume::Cut(found, found ? stop.reason : 0, played.stoppedByReplay);
        const bool shortEnd = !cut && found && stop.reason == 1 && continuous &&
            replay::speech::resume::EndedShort(heardMs, expectedMs);
        // The sync probe: at a natural end heard in continuous normal play, how
        // far the line's audio is from the recorded face (positive: the sound
        // ahead; a line that waited and was asked from its start shows its wait
        // as negative, a playhead held or slowed while it played as positive).
        if (found && stop.reason == 1 && continuous && played.startUs)
        {
            const std::int64_t ahead = replay::speech::resume::AudioAheadMs(line, timeMs, played.resumeOffsetMs, heardMs);
            ++g_poseV2SpeechSyncLines;
            if (ahead < -kPoseV2SpeechSyncOffMs)
                ++g_poseV2SpeechSyncLate;
            else if (ahead > kPoseV2SpeechSyncOffMs)
                ++g_poseV2SpeechSyncAhead;
            if ((ahead < 0 ? -ahead : ahead) > (g_poseV2SpeechSyncWorstMs < 0 ? -g_poseV2SpeechSyncWorstMs : g_poseV2SpeechSyncWorstMs))
            {
                g_poseV2SpeechSyncWorstMs = ahead;
                g_poseV2SpeechSyncWorstEventMs = line.timeMs;
            }
            if (g_poseV2SpeechSyncLogs.Take(kPoseV2SpeechSyncLogCap))
            {
                Ctx().log << "POSE_V2_SPEECH_SYNC,ped=" << replay::entity::IdText(line.speaker)
                          << ",event_ms=" << line.timeMs << ",end_ms=" << line.endMs << ",playhead_ms=" << timeMs
                          << ",play_start_ms=" << played.startPlayMs << ",heard_ms=" << heardMs
                          << ",offset_ms=" << played.resumeOffsetMs << ",resumed=" << (played.resumed ? 1 : 0)
                          << ",ahead_ms=" << ahead << ",transport_lost_ms=" << Ctx().transport.LostMs() << '\n';
            }
        }
        if (!cut && !shortEnd)
            return;
        ++(cut ? g_poseV2SpeechCuts : g_poseV2SpeechShortEnds);
        if (!g_poseV2SpeechCutLogs.Take(kPoseV2SpeechCutLogCap))
            return;
        Ctx().log << "POSE_V2_SPEECH_CUT,ped=" << replay::entity::IdText(line.speaker)
                  << ",line=0x" << std::hex << played.line << std::dec
                  << ",kind=" << (cut ? "cut" : "short_end")
                  << ",reason=" << (found ? static_cast<std::int64_t>(stop.reason) : -1)
                  << ",state=" << (found ? stop.state : -1)
                  << ",callers=0x" << std::hex << (found ? stop.callers[0] : 0u) << "/0x" << (found ? stop.callers[1] : 0u)
                  << std::dec << ",heard_ms=" << heardMs << ",expected_ms=" << expectedMs
                  << ",end_source=" << end.source << ",path=" << played.path
                  << ",resumed=" << (played.resumed ? 1 : 0) << ",offset_ms=" << played.resumeOffsetMs
                  << ",normal=" << (normalPlay ? 1 : 0)
                  << ",context=0x" << std::hex << replay::audio::FinalizeHash(line.contextPartialHash) << std::dec
                  << ",event_ms=" << line.timeMs << ",playhead_ms=" << timeMs << '\n';
    }

    void FollowPoseV2PlayedSpeech(std::uint32_t timeMs, bool normalPlay)
    {
        static std::vector<replay::audio::SpeechLineStart> starts;
        replay::audio::CollectSpeechStartsSince(g_poseV2SpeechPlayStartCursor, starts);
        std::ostream& log = Ctx().log;
        const auto& events = Ctx().clip.speechLines.Events();
        for (const replay::audio::SpeechLineStart& start : starts)
        {
            for (PoseV2PlayedSpeech& played : g_poseV2PlayedSpeech)
            {
                if (played.line != start.line || played.startLogged || played.eventIndex >= events.size())
                    continue;
                played.startLogged = true;
                const replay::entity::SpeechEvent& line = events[played.eventIndex];
                const std::uint64_t now = replay::audio::NowUs();
                const std::uint32_t ageMs = now > start.timeUs ? static_cast<std::uint32_t>((now - start.timeUs) / 1000) : 0;
                const std::uint32_t playMs = timeMs > ageMs ? timeMs - ageMs : 0;
                played.startUs = start.timeUs; // the cut diagnosis
                if (played.animal && played.pinnedAttempt && played.path && std::strcmp(played.path, "native") == 0)
                    ++g_poseV2AnimalBank.pinnedStarted; // heard with the recorded voice
                played.startPlayMs = playMs;
                // The start delay and holes per speaker.
                g_poseV2SoundTallies.speech[replay::entity::IdText(line.speaker)].NoteStart(line.timeMs, playMs,
                    normalPlay && !played.resumed);
                log << "POSE_V2_SPEECH_START,phase=play,ped=" << replay::entity::IdText(line.speaker)
                    << ",line=0x" << std::hex << start.line << std::dec
                    << ",state=" << start.stateAfter
                    << ",event_ms=" << line.timeMs
                    << ",timed_by=" << (line.timedByStart ? "start" : "core")
                    << ",play_start_ms=" << playMs
                    << ",delta_ms=" << static_cast<std::int64_t>(playMs) - static_cast<std::int64_t>(line.timeMs)
                    << '\n';
                if (played.held)
                {
                    // The held line was heard: what proves the player's pain played.
                    ++g_speechHeldStarted;
                    log << "SPEECH_PLAYER_HOLD,phase=start,ped=" << replay::entity::IdText(line.speaker)
                        << ",line=0x" << std::hex << start.line << ",voice=0x" << line.voiceHash
                        << ",context=0x" << replay::audio::FinalizeHash(line.contextPartialHash) << std::dec
                        << ",event_ms=" << line.timeMs << ",play_start_ms=" << playMs << '\n';
                }
                break;
            }
        }
        bool heldAlive = false; // a held line still created, loading or playing (HoldActive)
        for (std::size_t index = g_poseV2PlayedSpeech.size(); index-- > 0;)
        {
            PoseV2PlayedSpeech& played = g_poseV2PlayedSpeech[index];
            if (played.eventIndex >= events.size() || !replay::entities::Exists(played.ped))
            {
                if (!played.judged) // the puppet went away, the line is not judged
                    g_poseV2SpeechLines.Judged(played.animal, replay::speech::LineOutcome::Abandoned);
                g_poseV2PlayedSpeech.erase(g_poseV2PlayedSpeech.begin() + static_cast<std::ptrdiff_t>(index));
                continue;
            }
            void* entity = replay::entities::AddressOf(played.ped);
            std::int32_t state = -1;
            bool listed = replay::audio::SpeechLineState(entity, played.line, state);
            if (played.held && listed && state >= 1 && state <= 5)
                heldAlive = true;
            // An animal line the engine killed while loading is asked again through
            // the animal's own path, once.
            if (played.animal && !played.retried && !played.startLogged && (!listed || state == 6))
            {
                played.retried = true;
                replay::audio::SpeechLineStop stop;
                const bool found = replay::audio::FindLineStop(played.line, stop);
                const std::uint64_t nowUs = replay::audio::NowUs();
                const std::uint64_t ageMs = nowUs > played.requestUs ? (nowUs - played.requestUs) / 1000 : 0;
                bool remade = false;
                // A line asked with the recorded voice that died loading sends this
                // puppet's next lines by context first.
                if (played.pinnedAttempt && found && stop.reason == 4)
                {
                    ++g_poseV2AnimalBank.pinnedDiedLoading;
                    if (played.pinnedBank == replay::animalbank::PinnedBank::Ready)
                        ++g_poseV2AnimalBank.pinnedDiedReady;
                    else if (played.pinnedBank == replay::animalbank::PinnedBank::Unknown)
                        ++g_poseV2AnimalBank.pinnedDiedUnknown;
                    AnimalContextFirst& wait = g_poseV2AnimalContextFirst[played.ped];
                    wait.waitMs = wait.waitMs ? (std::min)(wait.waitMs * 2, kContextFirstMaxMs) : kContextFirstMs;
                    wait.untilMs = GetTickCount64() + wait.waitMs;
                }
                std::uint64_t remadeLine = 0;
                if (normalPlay &&
                    replay::speech::RetryAnimalLine(true, false, false, found, found ? stop.reason : 0u, ageMs))
                {
                    remadeLine = RetryPoseV2AnimalVocal(played, events[played.eventIndex], timeMs, ageMs);
                    remade = remadeLine != 0;
                }
                played.remade = remade; // in the speech_silent detail
                if (remadeLine)
                {
                    // The recorded line is judged on the line the retry made (audible start
                    // or death).
                    played.line = remadeLine;
                    replay::theatre::NoteReplayLine(played.line);
                    played.requestUs = replay::audio::NowUs();
                    played.requestPlayMs = timeMs;
                    played.path = "native_retry";
                    listed = replay::audio::SpeechLineState(entity, played.line, state);
                }
                // A line killed while loading and not made again is silent
                // (horse_sound_report.h).
                if (!remade && normalPlay)
                {
                    replay::horsesound::SpeechTally& tally = PoseV2SpeechTally(events[played.eventIndex].speaker, true);
                    ++tally.killed;
                    if (tally.played)
                        --tally.played;
                    ++tally.reasons[found ? "killed_loading_reason_" + std::to_string(stop.reason) : std::string("killed_loading")];
                }
            }
            // A human line asked with the ambient source that died while loading is
            // asked once more at once with the metadata's source, and that voice and
            // context stay on it.
            if (!played.animal && !played.startLogged && !played.judged && !listed && normalPlay &&
                !played.ambientRetried && played.askedSource == replay::speech::slots::kSourceAmbient)
            {
                replay::audio::SpeechLineStop stop;
                if (replay::audio::FindLineStop(played.line, stop) && stop.reason == 4)
                {
                    played.ambientRetried = true;
                    const replay::entity::SpeechEvent& line = events[played.eventIndex];
                    if (g_poseV2AmbientFailed.insert(PoseV2AmbientKey(line)).second)
                        ++g_poseV2AmbientFallbacks;
                    const std::uint64_t nowUs = replay::audio::NowUs();
                    const std::uint64_t ageMs = nowUs > played.requestUs ? (nowUs - played.requestUs) / 1000 : 0;
                    if (ageMs <= kPoseV2OwnVoiceRetryMaxAgeMs)
                    {
                        ++g_poseV2AmbientKilledRetries;
                        const replay::speech::Identity expected = PoseV2SpeechIdentity(line);
                        const std::uint32_t requestVoice =
                            std::strcmp(played.voiceStatus, "voice_failed") == 0 ? line.voiceHash : 0;
                        replay::audio::PlayedLine got;
                        std::string detail;
                        std::uint32_t result = 0;
                        const std::uint32_t offsetMs = PoseV2LateOffsetMs(line, timeMs);
                        replay::speech::request::Ask ask = PoseV2RequestAsk(line);
                        const bool called = replay::audio::PlaySpeechDirect(entity, requestVoice, line.contextPartialHash,
                            line.variation, &expected, result, got, detail, offsetMs, 0, &ask);
                        std::int32_t newState = -1;
                        const bool created = called && result == 74 && got.line && got.identityRead && !got.mismatch &&
                            replay::audio::SpeechLineState(entity, got.line, newState) && newState >= 1 && newState <= 5;
                        if (created)
                        {
                            ++g_poseV2AmbientKilledRetryCreated;
                            played.line = got.line;
                            replay::theatre::NoteReplayLine(played.line);
                            played.requestUs = nowUs;
                            played.remade = true;
                            played.askedSource = 0;
                            played.source = got.source;
                            played.resumeOffsetMs = offsetMs;
                            played.path = "ambient_retry";
                            listed = true;
                            state = newState;
                        }
                        Ctx().log << "POSE_V2_SPEECH_AMBIENT_RETRY,ped=" << replay::entity::IdText(line.speaker)
                                  << ",created=" << (created ? 1 : 0) << ",age_ms=" << ageMs << ",offset_ms=" << offsetMs
                                  << ",status=" << detail << ",event_ms=" << line.timeMs << ",playhead_ms=" << timeMs << '\n';
                    }
                }
            }
            // Closed theater: an ambient voice line dead for the wave missing in its
            // fixed slot is asked once more with the puppet's own voice, and the
            // puppet's next lines go that way.
            if (!played.animal && !played.startLogged && !played.judged && !listed && normalPlay &&
                !played.ownVoiceRetried && !played.noOwnVoice &&
                std::strcmp(played.voiceStatus, "voice_failed") == 0)
            {
                replay::audio::SpeechLineStop stop;
                if (replay::audio::FindLineStop(played.line, stop) && stop.reason == 4 && stop.fieldsRead && stop.slot &&
                    (stop.soundFlagsEE & 4) != 0)
                {
                    played.ownVoiceRetried = true;
                    g_poseV2HumanOwnVoice.insert(played.ped);
                    const std::uint64_t nowUs = replay::audio::NowUs();
                    const std::uint64_t ageMs = nowUs > played.requestUs ? (nowUs - played.requestUs) / 1000 : 0;
                    if (ageMs <= kPoseV2OwnVoiceRetryMaxAgeMs)
                    {
                        ++g_poseV2OwnVoiceRetries;
                        const replay::entity::SpeechEvent& line = events[played.eventIndex];
                        replay::speech::Identity expected = PoseV2SpeechIdentity(line);
                        expected.voice = 0;
                        expected.voiceObjectHash = 0;
                        expected.voiceObjectType = 0;
                        replay::audio::PlayedLine got;
                        std::string detail;
                        std::uint32_t result = 0;
                        const std::uint32_t offsetMs = PoseV2LateOffsetMs(line, timeMs);
                        replay::speech::request::Ask ask = PoseV2RequestAsk(line);
                        const bool called = replay::audio::PlaySpeechDirect(entity, 0, line.contextPartialHash, line.variation,
                            &expected, result, got, detail, offsetMs, 0, &ask);
                        std::int32_t newState = -1;
                        const bool created = called && result == 74 && got.line && got.identityRead && !got.mismatch &&
                            replay::audio::SpeechLineState(entity, got.line, newState) && newState >= 1 && newState <= 5;
                        if (created)
                        {
                            ++g_poseV2OwnVoiceRetryCreated;
                            played.line = got.line;
                            replay::theatre::NoteReplayLine(played.line);
                            played.requestUs = nowUs;
                            played.remade = true;
                            played.path = "own_voice_retry";
                            listed = true;
                            state = newState;
                        }
                        Ctx().log << "POSE_V2_SPEECH_OWN_VOICE,ped=" << replay::entity::IdText(line.speaker)
                                  << ",phase=retry,created=" << (created ? 1 : 0) << ",age_ms=" << ageMs << ",status=" << detail
                                  << ",event_ms=" << line.timeMs << ",playhead_ms=" << timeMs << '\n';
                    }
                }
            }
            // A human line that found no wave slot waits for one (see
            // RetryPoseV2SlotLine) before it is judged.
            if (!played.animal && !played.startLogged && !played.judged && !listed && normalPlay)
            {
                const std::uint64_t nowUs = replay::audio::NowUs();
                if (!played.slotWaitUntilUs && !played.slotRetries)
                {
                    replay::audio::SpeechLineStop stop;
                    if (replay::audio::FindLineStop(played.line, stop) && stop.reason == 4 && stop.fieldsRead &&
                        !stop.slot && stop.dataType != 3)
                    {
                        ++g_poseV2SlotWaits;
                        played.firstRequestUs = played.requestUs;
                        played.slotWaitUntilUs = nowUs + kPoseV2SlotWaitMs * 1000;
                        played.slotNextRetryUs = nowUs + kPoseV2SlotRetryEveryMs * 1000;
                    }
                }
                if (played.slotWaitUntilUs && nowUs < played.slotWaitUntilUs)
                {
                    if (nowUs >= played.slotNextRetryUs)
                    {
                        played.slotNextRetryUs = nowUs + kPoseV2SlotRetryEveryMs * 1000;
                        if (RetryPoseV2SlotLine(played, events[played.eventIndex], timeMs))
                            listed = replay::audio::SpeechLineState(entity, played.line, state);
                    }
                    if (!listed)
                        continue; // still waiting: kept, not judged
                }
            }
            JudgePoseV2Line(played, listed, state, timeMs, normalPlay);
            if (!listed)
            {
                NotePoseV2LineLeft(played, timeMs, normalPlay); // learned end, cut
                g_poseV2PlayedSpeech.erase(g_poseV2PlayedSpeech.begin() + static_cast<std::ptrdiff_t>(index));
                continue;
            }
            const replay::entity::SpeechEvent& line = events[played.eventIndex];
            if (!normalPlay || played.stopDone || !line.stopMs || timeMs < line.stopMs)
                continue;
            played.stopDone = true;
            played.stoppedByReplay = true; // the recorded stop, not a cut
            std::string error;
            const bool stopped = replay::audio::StopSpeechLine(entity, played.line, line.stopReason, error);
            if (stopped)
                ++g_poseV2SpeechStopsPlayed;
            log << "POSE_V2_SPEECH_STOP,phase=play,ped=" << replay::entity::IdText(line.speaker)
                << ",line=0x" << std::hex << played.line << std::dec
                << ",reason=" << line.stopReason
                << ",status=" << error
                << ",stop_ms=" << line.stopMs
                << ",playhead_ms=" << timeMs
                << '\n';
        }
        g_speechHoldLineAlive = heldAlive;
        log.flush();
    }

    // The puppets' speech entities for the core detour, every tick, and the
    // puppets' own core calls it saw (refused or let through)
    // (puppet_audio_rule.h RefusePuppetOwnSpeech).
    std::uint64_t g_poseV2OwnSpeechCursor = 0;
    }
    std::uint64_t g_poseV2OwnSpeechSeenBase = 0;
    std::uint64_t g_poseV2OwnSpeechRefusedBase = 0;
    namespace
    {
    std::uint64_t g_poseV2OwnSpeechLogged = 0;
    }
    std::uint64_t g_poseV2WorldSpeechBase = 0;
    std::uint64_t g_poseV2WorldSpeechQueuedBase = 0;
    namespace
    {
    constexpr std::uint64_t kPoseV2OwnSpeechLogCap = 200;
    }

    void BeginPoseV2PuppetSpeech()
    {
        std::vector<replay::audio::PuppetOwnSpeech> stale;
        replay::audio::CollectPuppetOwnSpeechSince(g_poseV2OwnSpeechCursor, stale);
        g_poseV2OwnSpeechSeenBase = replay::audio::PuppetOwnSpeechSeen();
        g_poseV2OwnSpeechRefusedBase = replay::audio::PuppetOwnSpeechRefused();
        g_poseV2OwnSpeechLogged = 0;
        g_poseV2WorldSpeechBase = replay::audio::WorldSpeechDuringPlayback();
        g_poseV2WorldSpeechQueuedBase = replay::audio::WorldSpeechQueuedDuringPlayback();
        std::ostream& log = Ctx().log;
        log << "POSE_V2_PUPPET_OWN_SPEECH_MODE,block=" << (replay::audio::PuppetOwnSpeechBlock() ? 1 : 0)
            << ",error=" << (*replay::audio::PuppetOwnSpeechError() ? replay::audio::PuppetOwnSpeechError() : "none")
            << '\n';
    }
    namespace
    {

    void UpdatePoseV2PuppetSpeech()
    {
        static std::vector<std::uint64_t> entities;
        static std::vector<std::uint32_t> owners;
        entities.clear();
        owners.clear();
        for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            if ((puppet.packet.kind != replay::entity::EntityKind::Ped &&
                    puppet.packet.kind != replay::entity::EntityKind::Horse) ||
                puppet.state != replay::playback::PuppetState::Ready || !puppet.clone ||
                !replay::entities::Exists(puppet.clone) || !replay::entities::IsPed(puppet.clone))
                continue;
            if (const std::uint64_t speech = replay::audio::SpeechEntity(replay::entities::AddressOf(puppet.clone)))
            {
                entities.push_back(speech);
                owners.push_back(puppet.packet.id.Packed());
            }
        }
        // Closed theater: while the replay owns the world the hidden player
        // is a live ped too; its own AI speech (grunts, breath, remarks) is
        // refused like a puppet's (the replay speaks through the puppets).
        if (replay::theatre::Owning())
        {
            const Ped player = PLAYER::PLAYER_PED_ID();
            if (player && replay::entities::Exists(player))
                if (const std::uint64_t speech = replay::audio::SpeechEntity(replay::entities::AddressOf(player)))
                {
                    entities.push_back(speech);
                    owners.push_back(0xFFFFFFFFu);
                }
        }
        replay::audio::SetPuppetSpeechEntities(entities, owners);
        static std::vector<replay::audio::PuppetOwnSpeech> own;
        replay::audio::CollectPuppetOwnSpeechSince(g_poseV2OwnSpeechCursor, own);
        std::ostream& log = Ctx().log;
        for (const replay::audio::PuppetOwnSpeech& call : own)
        {
            if (g_poseV2OwnSpeechLogged++ >= kPoseV2OwnSpeechLogCap)
                break;
            log << "POSE_V2_PUPPET_OWN_SPEECH,ped=" << replay::entity::IdText(replay::entity::ReplayId::FromPacked(call.owner))
                << ",context=0x" << std::hex << replay::audio::FinalizeHash(call.context)
                << ",callers=0x" << call.callers[0] << "/0x" << call.callers[1] << "/0x" << call.callers[2] << std::dec
                << ",refused=" << (call.refused ? 1 : 0)
                << ",result=" << call.result
                << '\n';
        }
    }

    // The puppet's line list when a replayed line died inside its request
    // (what else the speaker had: the core's same-context check, code 18).
    void LogPoseV2SpeakerLines(Ped ped, const replay::entity::SpeechEvent& line, std::uint64_t ours)
    {
        std::uint64_t list = 0;
        std::uint16_t count = 0;
        std::string text;
        if (PoseV2SpeechLineList(ped, list, count) && list && count <= 64)
            for (std::uint16_t n = 0; n < count; ++n)
            {
                std::uint64_t other = 0;
                if (!replay::audio::ReadU64(list + n * 8u, other) || !other)
                    continue;
                std::int32_t state = -1;
                std::uint64_t data = 0;
                std::uint32_t context = 0;
                replay::audio::ReadBytes(other + 0x234, &state, sizeof(state));
                if (replay::audio::ReadU64(other + 0x1D8, data) && data)
                    replay::audio::ReadBytes(data + 0x114, &context, sizeof(context));
                bool replayed = other == ours;
                for (const PoseV2PlayedSpeech& played : g_poseV2PlayedSpeech)
                    replayed = replayed || played.line == other;
                char item[64]{};
                std::snprintf(item, sizeof(item), "%s0x%X:s%d:%s", text.empty() ? "" : "|", context, state,
                    other == ours ? "this" : (replayed ? "replay" : "own"));
                text += item;
            }
        std::ostream& log = Ctx().log;
        log << "POSE_V2_SPEECH_SPEAKER_LINES,ped=" << replay::entity::IdText(line.speaker)
            << ",count=" << count
            << ",lines=" << (text.empty() ? "none" : text.c_str())
            << ",event_ms=" << line.timeMs << '\n';
    }
    }

    // One recorded line requested on its speaker's puppet (the body of the
    // speech track; also called for a deferred line), or on the puppet of the
    // recorded player.
    bool PoseV2IsRecordedPlayer(const replay::playback::Puppet& puppet)
    {
        if (g_speechPlayerPacked != 0xFFFFFFFFu)
            return puppet.packet.id.Packed() == g_speechPlayerPacked;
        if (puppet.packet.kind != replay::entity::EntityKind::Ped || !puppet.clone)
            return false;
        // The recorded player's model (PLAC) when the clip has it: the live
        // player may be another character (speech_player_route.h SameCharacter).
        if (const std::uint32_t recorded = Ctx().clip.place.playerModel)
            return static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(puppet.clone)) == recorded;
        const Ped player = PLAYER::PLAYER_PED_ID();
        return player && ENTITY::GET_ENTITY_MODEL(player) == ENTITY::GET_ENTITY_MODEL(puppet.clone);
    }
    namespace
    {

    // `resumed` says the line again after a jump, from `startOffsetMs` (0: from
    // its start); it is not counted again in the per-speaker tallies (the line
    // was due once). True when the engine created the line and the replay
    // follows it.
    bool RequestPoseV2SpeechLine(std::size_t index, std::uint32_t timeMs, std::uint32_t requestMs,
        std::uint32_t startOffsetMs = 0, bool resumed = false, bool fromSlotQueue = false)
    {
        const replay::entity::SpeechEvent& line = Ctx().clip.speechLines.Events()[index];
        g_poseV2SlotGateLast = replay::speech::slots::Decision::Ask;
        g_poseV2LastAskedType = 0;
        // A line adopted already playing (startedBeforeMs) goes on from where it
        // was, also on its normal path.
        if (!startOffsetMs && line.startedBeforeMs)
            startOffsetMs = PoseV2LateOffsetMs(line, timeMs);
        replay::horsesound::SpeechTally resumedTally; // a resumed line's counts go nowhere
        bool lineCreated = false;
        replay::playback::Puppet* speaker = Ctx().puppets.Find(line.speaker);
        const bool present = speaker && speaker->state == replay::playback::PuppetState::Ready &&
            speaker->clone && replay::entities::Exists(speaker->clone);
        std::string status = "cancelled";
        if (!present)
        {
            // Counted, not silent by refusal (horse_sound_report.h).
            replay::horsesound::SpeechTally& tally = resumed ? resumedTally : PoseV2SpeechTally(line.speaker, false);
            ++tally.recorded;
            ++tally.noPuppet;
        }
        if (present)
        {
            void* entity = replay::entities::AddressOf(speaker->clone);
            // The engine plays the player's own speech louder than the same line said
            // by any other ped (about 7.6 dB on the player's puppet for the same
            // recording). The recorded player is the owner of the clip's player-class
            // sounds (SGEN), or the puppet with the player's model; his lines are said
            // by the hidden real player, who stands at his puppet.
            Ped speakerPed = speaker->clone;
            const bool recordedPlayer = PoseV2IsRecordedPlayer(*speaker);
            // Only while the hidden player stands at the puppet; in a mission he stays
            // where the replay was opened and the engine refuses his lines by distance
            // (code 0x29), so the puppet says them there (speech_player_route.h).
            bool routedToPlayer = false;
            bool heldAtPuppet = false;
            // The route's reason for the recorded player's line (KeyOff: not routed).
            replay::speech::route::Why routeWhy = replay::speech::route::Why::KeyOff;
            if (recordedPlayer)
            {
                const Ped player = PLAYER::PLAYER_PED_ID();
                const bool playerValid = player && replay::entities::Exists(player) && replay::entities::IsPed(player);
                // His voice is the live character's: he stands in only for the same
                // character (a John save playing an Arthur clip gave Arthur John's pain).
                const std::uint32_t recordedModel = Ctx().clip.place.playerModel
                    ? Ctx().clip.place.playerModel
                    : static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(speaker->clone));
                const std::uint32_t liveModel = playerValid ? static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(player)) : 0u;
                const bool sameCharacter = replay::speech::route::SameCharacter(recordedModel, liveModel);
                float distanceM = -1.0f;
                float at[3]{};
                float puppetAt[3]{};
                if (playerValid && replay::entities::Position(player, at) && replay::entities::Position(speaker->clone, puppetAt))
                    distanceM = std::sqrt((at[0] - puppetAt[0]) * (at[0] - puppetAt[0]) + (at[1] - puppetAt[1]) * (at[1] - puppetAt[1]) +
                        (at[2] - puppetAt[2]) * (at[2] - puppetAt[2]));
                const replay::speech::route::Decision route =
                    replay::speech::route::Choose(true, true, playerValid, sameCharacter, distanceM);
                routeWhy = route.why;
                if (route.why == replay::speech::route::Why::CharacterMismatch)
                {
                    // His puppet says it with the recorded voice (pinned or named in the
                    // request), never its own voice; silent rather than John for Arthur.
                    ++g_speechCharacterMismatch;
                    if (g_speechCharacterMismatchLogs < 8)
                    {
                        ++g_speechCharacterMismatchLogs;
                        const replay::logfmt::FormatScope logFormat(Ctx().log);
                        Ctx().log << std::fixed << std::setprecision(1) << "SPEECH_PLAYER_ROUTE,phase=line,speaker=puppet,why="
                                  << replay::speech::route::WhyName(route.why) << ",recorded_model=0x" << std::hex
                                  << recordedModel << ",live_model=0x" << liveModel << ",voice=0x" << line.voiceHash
                                  << ",voice_obj=" << (line.voiceObjectPartial || line.voiceObjectType ? 1 : 0)
                                  << ",context=0x" << replay::audio::FinalizeHash(line.contextPartialHash) << std::dec
                                  << ",distance_m=" << distanceM << ",ped=" << replay::entity::IdText(line.speaker)
                                  << ",event_ms=" << line.timeMs << ",playhead_ms=" << timeMs << '\n';
                    }
                }
                else if (route.speaker == replay::speech::route::Speaker::Player)
                {
                    entity = replay::entities::AddressOf(player);
                    speakerPed = player;
                    routedToPlayer = true;
                    ++g_speechOnPlayer;
                }
                else if (replay::speech::route::HoldAtPuppet(route, distanceM >= 0.0f, PlayerFollowMoving(), false,
                             line.voiceHash, line.voiceObjectPartial, line.voiceObjectType))
                {
                    // His pain voice has no wave on the puppet: the hidden player is
                    // brought to the puppet (his horse with him when mounted, as the
                    // follow moves him) and says it there; the follow keeps him there
                    // while the line plays (PlayerSpeechHoldAt).
                    const Ped mover = PED::IS_PED_ON_MOUNT(player) ? PED::GET_MOUNT(player) : player;
                    replay::pose::playback::MoveHiddenEntity(mover && replay::entities::Exists(mover) ? mover : player,
                        puppetAt[0], puppetAt[1], puppetAt[2]);
                    g_speechHoldPuppet = speaker->clone;
                    g_speechHoldPlayer = player;
                    g_speechHoldSinceMs = GetTickCount64();
                    g_speechHoldLineAlive = true;
                    entity = replay::entities::AddressOf(player);
                    speakerPed = player;
                    routedToPlayer = true;
                    heldAtPuppet = true;
                    ++g_speechOnPlayer;
                    ++g_speechHeld;
                    if (g_speechHeldLogs < 16)
                    {
                        ++g_speechHeldLogs;
                        const replay::logfmt::FormatScope logFormat(Ctx().log);
                        Ctx().log << std::fixed << std::setprecision(1) << "SPEECH_PLAYER_ROUTE,phase=line,speaker=player,why="
                                  << replay::speech::route::WhyName(replay::speech::route::Why::Held) << ",distance_m=" << distanceM
                                  << ",mounted=" << (mover != player ? 1 : 0) << ",ped=" << replay::entity::IdText(line.speaker)
                                  << ",voice=0x" << std::hex << line.voiceHash << ",context=0x"
                                  << replay::audio::FinalizeHash(line.contextPartialHash) << std::dec
                                  << ",event_ms=" << line.timeMs << ",playhead_ms=" << timeMs << '\n';
                    }
                }
                else
                {
                    ++g_speechPlayerFar;
                    if (g_speechPlayerFarLogs < 8)
                    {
                        ++g_speechPlayerFarLogs;
                        const replay::logfmt::FormatScope logFormat(Ctx().log);
                        Ctx().log << std::fixed << std::setprecision(1) << "SPEECH_PLAYER_ROUTE,phase=line,speaker=puppet,why="
                                  << replay::speech::route::WhyName(route.why) << ",distance_m=" << distanceM
                                  << ",ped=" << replay::entity::IdText(line.speaker) << ",event_ms=" << line.timeMs
                                  << ",playhead_ms=" << timeMs << '\n';
                    }
                }
            }
            // The recorded voice object is pinned on the puppet (normally
            // preloaded ahead) and the request carries no voice, so the
            // engine resolves exactly that voice from the holder.
            const std::string voiceStatus = PinPoseV2Voice(*speaker, line, timeMs, "late");
            const bool animal = voiceStatus == "animal";
            if (resumed && animal)
            {
                // An animal vocalization is short and goes by the animal's
                // own path, which takes no start offset: not said again.
                ++g_poseV2SpeechResumeAnimalSkipped;
                Ctx().log << "POSE_V2_PLAYBACK_SPEECH,ped=" << replay::entity::IdText(line.speaker)
                          << ",status=resume_skip_animal,event_ms=" << line.timeMs << ",offset_ms=" << startOffsetMs
                          << ",playhead_ms=" << timeMs << '\n';
                return false;
            }
            replay::horsesound::SpeechTally& tally = resumed ? resumedTally : PoseV2SpeechTally(line.speaker, animal);
            ++tally.recorded;
            // An animal line goes through the native's path first: the direct path's
            // gate often returns 64 at playback (the AI checks re-roll) and its created
            // lines get killed while loading (stop reason 4), while the native path
            // creates nearly all of them.
            replay::horsesound::AnimalAttempt attempt =
                replay::horsesound::FirstAttempt(animal, true);
            if (attempt == replay::horsesound::AnimalAttempt::Pinned)
            {
                const auto wait = g_poseV2AnimalContextFirst.find(speaker->clone);
                if (wait != g_poseV2AnimalContextFirst.end())
                {
                    if (GetTickCount64() < wait->second.untilMs)
                        attempt = replay::horsesound::AnimalAttempt::Context;
                    else if (wait->second.untilMs)
                    {
                        wait->second.untilMs = 0; // the recorded voice again (the wait doubles if it dies)
                        ++g_poseV2AnimalContextRetries;
                    }
                }
            }
            // The recorded voice only while its bank is the type's loaded one;
            // otherwise the pinned line would die loading (reason 4), so it goes by
            // context (animal_bank.h).
            replay::animalbank::PinnedBank pinnedBank = replay::animalbank::PinnedBank::Unknown;
            if (attempt == replay::horsesound::AnimalAttempt::Pinned)
            {
                pinnedBank = PoseV2PinnedBank(line);
                if (pinnedBank == replay::animalbank::PinnedBank::Missing)
                {
                    ++g_poseV2AnimalBank.pinnedMissing;
                    attempt = replay::horsesound::AnimalAttempt::Context;
                }
                else
                {
                    ++(pinnedBank == replay::animalbank::PinnedBank::Ready ? g_poseV2AnimalBank.pinnedReady
                                                                           : g_poseV2AnimalBank.pinnedUnknown);
                }
            }
            std::string nativeTrail;
            while (attempt == replay::horsesound::AnimalAttempt::Pinned ||
                attempt == replay::horsesound::AnimalAttempt::Context)
            {
                const bool pinned = attempt == replay::horsesound::AnimalAttempt::Pinned;
                std::uint32_t nativeResult = 0;
                std::string nativeStatus;
                // Pinned asks the resolved context with its voice and variation; context
                // only asks what the game asked (the requested context), which the animal
                // pipeline resolves again.
                const std::uint32_t context = pinned || !line.requestedContextPartialHash
                    ? line.contextPartialHash : line.requestedContextPartialHash;
                const std::uint64_t created = PlayPoseV2NativeAnimalLine(speaker->clone, context,
                    pinned ? line.voiceHash : 0, pinned ? line.variation : 0, nativeResult, nativeStatus);
                nativeTrail += std::string(nativeTrail.empty() ? "" : "|") +
                    replay::horsesound::AttemptName(attempt) + ":" + nativeStatus;
                if (created)
                {
                    replay::audio::PlayedLine got;
                    const bool read = replay::audio::ReadSpeechLineIdentity(entity, created, got);
                    const replay::speech::Identity expectedIdentity = PoseV2SpeechIdentity(line);
                    replay::speech::Identity gotIdentity;
                    gotIdentity.voice = got.voice;
                    gotIdentity.context = got.context;
                    gotIdentity.variation = got.variation;
                    gotIdentity.voiceObjectHash =
                        got.voiceObject ? replay::speech::Finalize(got.voiceObjectPartial) : 0;
                    gotIdentity.voiceObjectType = got.voiceObjectType;
                    // The animal's voice object is its type's, not the
                    // recorded holder's: only voice, context, variation.
                    const std::uint32_t mismatch = read ? (replay::speech::Compare(expectedIdentity, gotIdentity) &
                        (replay::speech::kMismatchVoice | replay::speech::kMismatchContext |
                            replay::speech::kMismatchVariation)) : 0u;
                    ++g_poseV2SpeechPlayed;
                    ++g_poseV2AnimalNativePlayed;
                    ++tally.played;
                    ++(pinned ? tally.pinned : tally.context);
                    if (!pinned && read && line.voiceHash)
                        ++(got.voice == line.voiceHash ? g_poseV2AnimalBank.contextSameVoice : g_poseV2AnimalBank.contextOtherVoice);
                    if (mismatch)
                        ++tally.mismatch;
                    PoseV2PlayedSpeech follow;
                    follow.eventIndex = index;
                    follow.ped = speakerPed;
                    follow.line = created;
                    follow.animal = true;
                    follow.pinnedAttempt = pinned;
                    follow.pinnedBank = pinned ? pinnedBank : replay::animalbank::PinnedBank::Unknown;
                    follow.requestUs = replay::audio::NowUs();
                    lineCreated = true;
                    NotePoseV2LineCreated(follow, timeMs, pinned ? "native" : "native_context", voiceStatus);
                    g_poseV2PlayedSpeech.push_back(follow);
                    TrackPoseV2QueuedLine(speakerPed, line.speaker, line.contextPartialHash);
                    status = std::string("played:") + replay::horsesound::AttemptName(attempt) +
                        (mismatch ? ":other_phrase:" + replay::speech::MismatchText(mismatch) : std::string(":same_phrase")) +
                        ":voice:animal:line:" + nativeTrail;
                    LogPoseV2SpeechId("play", line, created, gotIdentity, got.voiceObject, got.dataType,
                        replay::audio::SpeechCorePaths().direct,
                        timeMs, -1, std::string("status=") + (mismatch ? "native_other" : "native_match") +
                        ",result=" + std::to_string(nativeResult) + ",voice_status=animal");
                    // The line is played; Done ends it here. Leaving the attempt at Pinned
                    // would run the direct path after every native line: a second line, or the
                    // animal gate refusing it because the first one is already queued
                    // (result 64, counted as silent).
                    attempt = replay::horsesound::NextAttempt(attempt, true);
                    break;
                }
                attempt = replay::horsesound::NextAttempt(attempt, false, line.variation > 0);
            }
            // Only a line no native attempt created goes on to the direct path
            // (horse_sound_report.h RunsDirect).
            if (!replay::horsesound::RunsDirect(attempt))
            {
                NotePoseV2LineOutcome(line.speaker.Packed(), line.timeMs, true);
                std::ostream& log = Ctx().log;
                log << "POSE_V2_PLAYBACK_SPEECH,ped=" << replay::entity::IdText(line.speaker)
                    << ",context=0x" << std::hex << replay::audio::FinalizeHash(line.contextPartialHash)
                    << ",voice=0x" << line.voiceHash << std::dec
                    << ",variation=" << line.variation
                    << ",status=" << status
                    << ",event_ms=" << line.timeMs
                    << ",request_ms=" << requestMs
                    << ",playhead_ms=" << timeMs
                    << '\n';
                return lineCreated;
            }
            if (!nativeTrail.empty())
                ++g_poseV2AnimalNativeFallbacks;
            // Direct line entry with the recorded context and variation only; the
            // created line is compared with the recording and stopped when it differs.
            replay::speech::Identity expected = PoseV2SpeechIdentity(line);
            replay::audio::PlayedLine played;
            std::string detail;
            std::uint32_t result = 0;
            // Closed theater: this puppet's ambient voice lines go with its own voice.
            const bool ownVoice = voiceStatus == "voice_failed" &&
                g_poseV2HumanOwnVoice.count(speaker->clone) != 0 && replay::speech::route::OwnVoiceAllowed(routeWhy);
            if (ownVoice)
            {
                expected.voice = 0;
                expected.voiceObjectHash = 0;
                expected.voiceObjectType = 0;
            }
            // An ambient voice is not a voice object (no type holds it): the pin fails
            // and a voiceless request lets the engine pick the puppet's own voice. The
            // request then carries the recorded voice: the selection step keeps any
            // non-zero voice. Animals too: their voice is not on a holder
            // either.
            std::uint32_t requestVoice =
                !ownVoice && (voiceStatus == "voice_failed" || voiceStatus == "animal") ? line.voiceHash : 0;
            // On the hidden player a line that named its voice live is asked with that
            // voice in the request (the pin is on the puppet's holder, not his): the
            // engine otherwise checks the line against the player's own current voice
            // and refuses it with 68 (speech_player_route.h NamedVoiceOnPlayer).
            const std::uint32_t puppetRequestVoice = requestVoice;
            const bool namedOnPlayer = !ownVoice && replay::speech::route::NamedVoiceOnPlayer(routedToPlayer, animal,
                line.voiceHash, line.voiceObjectPartial, line.voiceObjectType);
            if (namedOnPlayer)
                requestVoice = line.voiceHash;
            // A human line is asked only with an ambient slot free (or after the
            // replay's weakest line gave its slot); it waits otherwise
            // (speech_slots.h).
            if (!animal)
            {
                std::string gateDetail;
                const replay::speech::slots::Decision gate = PoseV2SlotGate(index, speakerPed, timeMs, gateDetail);
                g_poseV2SlotGateLast = gate;
                if (gate != replay::speech::slots::Decision::Ask)
                {
                    using replay::speech::slots::Decision;
                    if (gate == Decision::Wait || gate == Decision::Evict)
                    {
                        if (!fromSlotQueue)
                        {
                            PoseV2SlotWait wait;
                            wait.index = index;
                            wait.requestMs = requestMs;
                            wait.sinceUs = replay::audio::NowUs();
                            wait.resumed = resumed;
                            g_poseV2SlotQueue.push_back(wait);
                            ++g_poseV2SlotGateWaits;
                        }
                        --tally.recorded; // counted again when it is asked
                    }
                    else
                    {
                        ++tally.refused;
                        ++tally.reasons[std::string("slot_") + replay::speech::slots::DecisionName(gate)];
                        NotePoseV2LineOutcome(line.speaker.Packed(), line.timeMs, false);
                        ++(gate == Decision::DropInaudible ? g_poseV2SlotGateDropInaudible : g_poseV2SlotGateDropLate);
                    }
                    if (g_poseV2SlotLogs.Take(kPoseV2SlotLogCap))
                        Ctx().log << "SPEECH_SLOT_GATE,ped=" << replay::entity::IdText(line.speaker)
                                  << ",decision=" << replay::speech::slots::DecisionName(gate) << ',' << gateDetail
                                  << ",queued=" << (fromSlotQueue ? 1 : 0) << ",resumed=" << (resumed ? 1 : 0)
                                  << ",context=0x" << std::hex << replay::audio::FinalizeHash(line.contextPartialHash)
                                  << std::dec << ",event_ms=" << line.timeMs << ",playhead_ms=" << timeMs << '\n';
                    return false;
                }
            }
            std::int32_t requestSource = PoseV2RequestSource(line, animal);
            if (requestVoice)
                ++g_poseV2SpeechVoiceInRequest;
            // The recorded line type and request fields (speech_request.h); without
            // them the AI's type 1 camp chatter comes back as type 2 with the ctor's
            // volume type and audibility.
            replay::speech::request::Ask ask = animal ? replay::speech::request::Ask{} : PoseV2RequestAsk(line);
            g_poseV2LastAskedType = ask.lineType;
            bool called = replay::audio::PlaySpeechDirect(entity, requestVoice, line.contextPartialHash,
                line.variation, &expected, result, played, detail, startOffsetMs, requestSource, &ask);
            // A request the engine refused with the ambient source (result 68) is
            // asked again at once with the metadata's source, and that voice and
            // context stay on it for the rest of the playback.
            if (!(called && result == 74 && played.line) && requestSource == replay::speech::slots::kSourceAmbient)
            {
                ++g_poseV2AmbientRefusedRetries;
                if (g_poseV2AmbientFailed.insert(PoseV2AmbientKey(line)).second)
                    ++g_poseV2AmbientFallbacks;
                requestSource = 0;
                played = replay::audio::PlayedLine{};
                detail.clear();
                result = 0;
                called = replay::audio::PlaySpeechDirect(entity, requestVoice, line.contextPartialHash,
                    line.variation, &expected, result, played, detail, startOffsetMs, requestSource, &ask);
                if (called && result == 74 && played.line)
                    ++g_poseV2AmbientRefusedRetryCreated;
            }
            if (namedOnPlayer)
            {
                ++g_speechPlayerNamedVoice;
                const bool namedCreated = called && result == 74 && played.line && played.identityRead && !played.mismatch;
                if (namedCreated)
                    ++g_speechPlayerNamedVoiceCreated;
                if (g_speechPlayerNamedVoiceLogs < 8)
                {
                    ++g_speechPlayerNamedVoiceLogs;
                    Ctx().log << "SPEECH_PLAYER_ROUTE,phase=line,speaker=player,why=named_voice,voice=0x" << std::hex
                              << line.voiceHash << std::dec << ",result=" << result << ",created=" << (namedCreated ? 1 : 0)
                              << ",got_voice_obj=0x" << std::hex << played.voiceObject << std::dec
                              << ",mismatch=" << replay::speech::MismatchText(played.mismatch)
                              << ",source=" << requestSource << ",ped=" << replay::entity::IdText(line.speaker)
                              << ",context=0x" << std::hex << replay::audio::FinalizeHash(line.contextPartialHash) << std::dec
                              << ",event_ms=" << line.timeMs << ",playhead_ms=" << timeMs << '\n';
                }
            }
            // A line of the recorded player the engine did not create on the hidden
            // player (result 68: his own voice is not the recorded one pinned on the
            // puppet) is asked again at once on the puppet, with the same Ask and
            // source, as the Far route does (speech_player_route.h RetryOnPuppet). This
            // is the safety net of the named voice above; the puppet goes with its
            // pinned voice (puppetRequestVoice).
            bool onPlayerNow = routedToPlayer;
            if (replay::speech::route::RetryOnPuppet(routedToPlayer, called && result == 74 && played.line,
                    called && result == 74 && played.line && played.identityRead && !played.mismatch,
                    played.stoppedOnMismatch))
            {
                const std::uint32_t playerResult = result;
                const bool playerStopped = played.stoppedOnMismatch;
                ++g_speechPlayerRefused;
                entity = replay::entities::AddressOf(speaker->clone);
                speakerPed = speaker->clone;
                onPlayerNow = false;
                played = replay::audio::PlayedLine{};
                detail.clear();
                result = 0;
                called = replay::audio::PlaySpeechDirect(entity, puppetRequestVoice, line.contextPartialHash,
                    line.variation, &expected, result, played, detail, startOffsetMs, requestSource, &ask);
                const bool puppetCreated = called && result == 74 && played.line;
                if (puppetCreated)
                    ++g_speechPlayerRefusedPuppetCreated;
                detail += std::string("|after_player_") + (playerStopped ? "other_phrase" : std::to_string(playerResult));
                if (g_speechPlayerRefusedLogs < 8)
                {
                    ++g_speechPlayerRefusedLogs;
                    Ctx().log << "SPEECH_PLAYER_ROUTE,phase=line,speaker=puppet,why=player_refused,player_result="
                              << playerResult << ",player_stopped=" << (playerStopped ? 1 : 0)
                              << ",puppet_result=" << result << ",puppet_created=" << (puppetCreated ? 1 : 0)
                              << ",source=" << requestSource << ",ped=" << replay::entity::IdText(line.speaker)
                              << ",context=0x" << std::hex << replay::audio::FinalizeHash(line.contextPartialHash) << std::dec
                              << ",event_ms=" << line.timeMs << ",playhead_ms=" << timeMs << '\n';
                }
            }
            // A human line still not created after the attempts above, asked with the
            // conversation fields (recorded duck byte, sound set), is asked again at
            // once without them on the speaker that has it now. No engine check before
            // the line is known to read those fields, so this only guards against an
            // unseen refusal; logged.
            if (!animal && g_speechConversationLive && !(called && result == 74 && played.line))
            {
                replay::speech::request::Ask plain = PoseV2RequestAsk(line, false);
                if (!replay::speech::request::SameAsk(ask, plain))
                {
                    const std::uint32_t firstResult = result;
                    ++g_speechConversationPlainRetries;
                    played = replay::audio::PlayedLine{};
                    detail.clear();
                    result = 0;
                    called = replay::audio::PlaySpeechDirect(entity, onPlayerNow ? requestVoice : puppetRequestVoice,
                        line.contextPartialHash, line.variation, &expected, result, played, detail, startOffsetMs,
                        requestSource, &plain);
                    const bool plainCreated = called && result == 74 && played.line;
                    if (plainCreated)
                    {
                        ++g_speechConversationPlainCreated;
                        ask = plain; // what the created line was asked with, for the notes below
                    }
                    detail += "|after_conversation_" + std::to_string(firstResult);
                    if (g_speechConversationPlainLogs < 8)
                    {
                        ++g_speechConversationPlainLogs;
                        Ctx().log << "SPEECH_CONVERSATION_RETRY,why=refused_with_conversation,first_result=" << firstResult
                                  << ",plain_result=" << result << ",created=" << (plainCreated ? 1 : 0)
                                  << ",speaker=" << (onPlayerNow ? "player" : "puppet")
                                  << ",ped=" << replay::entity::IdText(line.speaker) << ",context=0x" << std::hex
                                  << replay::audio::FinalizeHash(line.contextPartialHash) << std::dec
                                  << ",event_ms=" << line.timeMs << ",playhead_ms=" << timeMs << '\n';
                    }
                }
            }
            const bool queued = called && result == 74 && played.line;
            const bool matched = queued && played.identityRead && !played.mismatch;
            if (queued && played.identityRead && !animal)
                NotePoseV2RequestDiff(line, played, ask, timeMs);
            if (queued && played.identityRead && !animal && g_speechConversationLive)
                NoteSpeechConversation(line, played, ask, requestSource, timeMs);
            if (queued && detail.find("_to_6|") != std::string::npos)
                LogPoseV2SpeakerLines(speakerPed, line, played.line);
            std::string verdict;
            if (matched)
            {
                verdict = "match";
                ++g_poseV2SpeechMatches;
                ++g_poseV2SpeechPlayed;
                ++tally.played;
                ++tally.direct;
                PoseV2PlayedSpeech follow;
                follow.eventIndex = index;
                follow.ped = speakerPed;
                follow.line = played.line;
                follow.animal = voiceStatus == "animal";
                follow.requestUs = replay::audio::NowUs();
                follow.resumed = resumed;
                follow.resumeOffsetMs = startOffsetMs;
                follow.askedSource = requestSource;
                follow.source = played.source;
                follow.namedVoice = onPlayerNow && namedOnPlayer;
                follow.held = onPlayerNow && heldAtPuppet;
                follow.noOwnVoice = !replay::speech::route::OwnVoiceAllowed(routeWhy);
                if (requestSource)
                    NotePoseV2LineSource(played.source);
                g_poseV2SlotGateWaitAsked += fromSlotQueue ? 1 : 0;
                lineCreated = true;
                NotePoseV2LineCreated(follow, timeMs, resumed ? "resume" : "direct", voiceStatus);
                g_poseV2PlayedSpeech.push_back(follow);
                TrackPoseV2QueuedLine(speakerPed, line.speaker, line.contextPartialHash);
            }
            else if (queued)
            {
                verdict = "mismatch:" + replay::speech::MismatchText(played.mismatch);
                ++g_poseV2SpeechMismatches;
                ++g_poseV2SpeechCancelled;
                ++tally.refused; // stopped as another phrase
                ++tally.reasons["direct_other_phrase_stopped"];
            }
            else
            {
                verdict = "refused";
                ++g_poseV2SpeechRefused;
                ++g_poseV2SpeechCancelled;
                ++tally.refused;
                ++tally.reasons["direct_result_" + std::to_string(result)];
            }
            NotePoseV2LineOutcome(line.speaker.Packed(), line.timeMs, matched);
            status = verdict + ":voice:" + voiceStatus + ":line:" + detail +
                (nativeTrail.empty() ? std::string() : ":after:" + nativeTrail);
            replay::speech::Identity got;
            got.voice = played.voice;
            got.context = played.context;
            got.variation = played.variation;
            got.voiceObjectHash = played.voiceObject ? replay::speech::Finalize(played.voiceObjectPartial) : 0;
            got.voiceObjectType = played.voiceObjectType;
            LogPoseV2SpeechId("play", line, played.line, got, played.voiceObject, played.dataType,
                replay::audio::SpeechCorePaths().direct,
                timeMs, -1, "status=" + verdict + ",result=" + std::to_string(result) + ",voice_status=" + voiceStatus);
        }
        else
        {
            ++g_poseV2SpeechCancelled;
        }
        std::ostream& log = Ctx().log;
        log << "POSE_V2_PLAYBACK_SPEECH,ped=" << replay::entity::IdText(line.speaker)
            << ",context=0x" << std::hex << replay::audio::FinalizeHash(line.contextPartialHash)
            << ",voice=0x" << line.voiceHash << std::dec
            << ",variation=" << line.variation
            << ",status=" << status
            << ",event_ms=" << line.timeMs
            << ",request_ms=" << requestMs
            << ",playhead_ms=" << timeMs
            << ",offset_ms=" << startOffsetMs
            << ",recorded_type=" << line.dataType
            << ",asked_type=" << g_poseV2LastAskedType
            << '\n';
        log.flush();
        return lineCreated;
    }

    // ---- Speech slots (speech_slots.h) ----------------------------------------

    // The end of a clip's line (recorded, heard this session, or estimated
    // from the speaker's next line; speech_resume.h EndOf).
    replay::speech::resume::LineEnd PoseV2LineEnd(std::size_t index)
    {
        const auto& events = Ctx().clip.speechLines.Events();
        if (index >= events.size())
            return {};
        const replay::entity::SpeechEvent& line = events[index];
        std::uint32_t next = 0;
        for (std::size_t n = index + 1; n < events.size(); ++n)
            if (events[n].speaker == line.speaker)
            {
                next = events[n].timeMs;
                break;
            }
        return replay::speech::resume::EndOf(line,
            index < g_poseV2LearnedEndMs.size() ? g_poseV2LearnedEndMs[index] : 0, next);
    }

    std::uint32_t PoseV2RemainingMs(std::size_t index, std::uint32_t timeMs)
    {
        const auto& events = Ctx().clip.speechLines.Events();
        if (index >= events.size())
            return 0;
        const replay::entity::SpeechEvent& line = events[index];
        const replay::speech::resume::LineEnd end = PoseV2LineEnd(index);
        const std::uint32_t from = timeMs > line.timeMs ? timeMs : line.timeMs;
        return end.endMs > from ? end.endMs - from : 0;
    }

    replay::speech::slots::Decision PoseV2SlotGate(std::size_t index, Ped speakerPed, std::uint32_t timeMs,
        std::string& detail)
    {
        using namespace replay::speech::slots;
        static replay::theatre::AmbientSlots census;
        replay::theatre::ReadAmbientSlots(census);
        if (!census.ok)
        {
            ++g_poseV2SlotCensusUnread;
            detail = "census=unread";
            return Decision::Ask;
        }
        const Vector3 camera = CAM::GET_FINAL_RENDERED_CAM_COORD();
        // The puppets' speech entities and the replay's lines on each.
        static std::vector<std::uint64_t> puppetSpeech;
        puppetSpeech.clear();
        for (const replay::playback::Puppet& puppet : Ctx().puppets.list)
            if (puppet.clone && replay::entities::Exists(puppet.clone) && replay::entities::IsPed(puppet.clone))
                if (const std::uint64_t speech = replay::audio::SpeechEntity(replay::entities::AddressOf(puppet.clone)))
                    puppetSpeech.push_back(speech);
        static std::vector<Holder> holders;
        static std::vector<std::uint64_t> heldRefs;
        static std::vector<std::uint64_t> busyOwners;
        holders.clear();
        heldRefs.clear();
        busyOwners.clear();
        for (std::size_t n = 0; n < g_poseV2PlayedSpeech.size(); ++n)
        {
            const PoseV2PlayedSpeech& played = g_poseV2PlayedSpeech[n];
            if (!replay::entities::Exists(played.ped))
                continue;
            void* entity = replay::entities::AddressOf(played.ped);
            std::int32_t state = -1;
            std::uint64_t ref = 0;
            if (!replay::audio::SpeechLineState(entity, played.line, state) || state < 1 || state > 5 ||
                !replay::audio::ReadU64(played.line + kLineWaveRef, ref) || !ref)
                continue;
            if (const std::uint64_t speech = replay::audio::SpeechEntity(entity))
                busyOwners.push_back(speech);
            for (const replay::theatre::AmbientSlot& slot : census.slots)
            {
                if (slot.ref != ref)
                    continue;
                bool audible = true;
                bool onScreen = false;
                float distance = -1.0f;
                Holder holder;
                holder.id = n;
                holder.priority = PoseV2SpeakerPriority(played.ped, camera, audible, onScreen, distance);
                holders.push_back(holder);
                heldRefs.push_back(ref);
                break;
            }
        }
        int freeSlots = census.FreeCount();
        // A puppet's busy slot with none of the replay's lines on it: given
        // back through the engine's per-owner release, slots short only.
        int released = 0;
        for (const replay::theatre::AmbientSlot& slot : census.slots)
        {
            if (!slot.ref || !slot.owner ||
                std::find(heldRefs.begin(), heldRefs.end(), slot.ref) != heldRefs.end())
                continue;
            const bool puppet = std::find(puppetSpeech.begin(), puppetSpeech.end(), slot.owner) != puppetSpeech.end();
            const bool hasLine = std::find(busyOwners.begin(), busyOwners.end(), slot.owner) != busyOwners.end();
            if (!ReleaseIdle(freeSlots, puppet, hasLine))
                continue;
            std::uint32_t code = 0;
            const replay::theatre::CallResult result = replay::theatre::ReleaseAmbientSlotsOf(slot.owner, code);
            ++(result == replay::theatre::CallResult::Ok ? g_poseV2SlotIdleReleased : g_poseV2SlotIdleFailed);
            ++released;
            if (g_poseV2SlotLogs.Take(kPoseV2SlotLogCap))
                Ctx().log << "SPEECH_SLOT_RELEASE,owner=0x" << std::hex << slot.owner << std::dec
                          << ",result=" << replay::theatre::CallResultName(result) << ",code=0x" << std::hex << code
                          << std::dec << ",free=" << freeSlots << ",playhead_ms=" << timeMs << '\n';
            busyOwners.push_back(slot.owner); // once per owner
        }
        if (released)
        {
            replay::theatre::ReadAmbientSlots(census);
            if (census.ok)
                freeSlots = census.FreeCount();
        }
        if (g_poseV2SlotMinFree < 0 || freeSlots < g_poseV2SlotMinFree)
            g_poseV2SlotMinFree = freeSlots;
        bool audible = true;
        bool onScreen = false;
        float distance = -1.0f;
        const float priority = PoseV2SpeakerPriority(speakerPed, camera, audible, onScreen, distance);
        const std::uint32_t remaining = PoseV2RemainingMs(index, timeMs);
        // How late the line is asked (speech_slots.h kLateGraceMs).
        std::uint32_t lateMs = 0;
        {
            const auto& events = Ctx().clip.speechLines.Events();
            if (index < events.size())
                lateMs = (timeMs > events[index].timeMs ? timeMs - events[index].timeMs : 0) + events[index].startedBeforeMs;
        }
        const Verdict verdict = Decide(freeSlots, priority, audible, remaining, true, holders, lateMs);
        char text[192]{};
        std::snprintf(text, sizeof(text), "free=%d,total=%zu,held_by_replay=%zu,distance_m=%.1f,on_screen=%d,priority=%.2f,remaining_ms=%u,late_ms=%u",
            freeSlots, census.slots.size(), holders.size(), distance, onScreen ? 1 : 0, priority, remaining, lateMs);
        detail = text;
        if (verdict.decision == Decision::Ask && freeSlots < kReserveForAudible)
            ++g_poseV2SlotGateAsks;
        if (verdict.decision == Decision::Evict && verdict.victim < g_poseV2PlayedSpeech.size())
        {
            PoseV2PlayedSpeech& victim = g_poseV2PlayedSpeech[verdict.victim];
            std::string error;
            victim.stoppedByReplay = true;
            const bool stopped = replay::entities::Exists(victim.ped) &&
                replay::audio::StopSpeechLine(replay::entities::AddressOf(victim.ped), victim.line, 2, error);
            g_poseV2SlotGateEvicted += stopped ? 1 : 0;
            if (g_poseV2SlotLogs.Take(kPoseV2SlotLogCap))
            {
                const auto& events = Ctx().clip.speechLines.Events();
                Ctx().log << "SPEECH_SLOT_EVICT,victim="
                          << (victim.eventIndex < events.size() ? replay::entity::IdText(events[victim.eventIndex].speaker) : std::string("?"))
                          << ",stopped=" << (stopped ? 1 : 0) << ",status=" << error << ",for_event_ms="
                          << (index < events.size() ? events[index].timeMs : 0) << ",playhead_ms=" << timeMs << '\n';
            }
        }
        return verdict.decision;
    }

    // The lines waiting for a slot, strongest first, each tick of normal
    // play; a pause or a jump drops them (the resume asks the current ones).
    void PlayPoseV2SlotQueue(std::uint32_t timeMs, bool normalPlay)
    {
        if (g_poseV2SlotQueue.empty())
            return;
        if (!normalPlay)
        {
            g_poseV2SlotGateDropPlay += g_poseV2SlotQueue.size();
            g_poseV2SlotQueue.clear();
            return;
        }
        const auto& events = Ctx().clip.speechLines.Events();
        const Vector3 camera = CAM::GET_FINAL_RENDERED_CAM_COORD();
        static std::vector<replay::speech::slots::Waiting> order;
        order.clear();
        for (std::size_t n = 0; n < g_poseV2SlotQueue.size(); ++n)
        {
            replay::speech::slots::Waiting waiting;
            waiting.id = n;
            waiting.sinceUs = g_poseV2SlotQueue[n].sinceUs;
            const std::size_t index = g_poseV2SlotQueue[n].index;
            const replay::playback::Puppet* speaker = index < events.size() ? Ctx().puppets.Find(events[index].speaker) : nullptr;
            bool audible = true;
            bool onScreen = false;
            float distance = -1.0f;
            if (speaker && speaker->clone)
                waiting.priority = PoseV2SpeakerPriority(speaker->clone, camera, audible, onScreen, distance);
            order.push_back(waiting);
        }
        std::sort(order.begin(), order.end(), replay::speech::slots::Before);
        static std::vector<bool> done;
        done.assign(g_poseV2SlotQueue.size(), false);
        const std::uint64_t nowUs = replay::audio::NowUs();
        for (const replay::speech::slots::Waiting& waiting : order)
        {
            const PoseV2SlotWait wait = g_poseV2SlotQueue[waiting.id];
            if (wait.index >= events.size())
            {
                done[waiting.id] = true;
                continue;
            }
            const std::uint32_t offset = PoseV2LateOffsetMs(events[wait.index], timeMs);
            RequestPoseV2SpeechLine(wait.index, timeMs, wait.requestMs, offset, wait.resumed, true);
            using replay::speech::slots::Decision;
            if (g_poseV2SlotGateLast != Decision::Wait && g_poseV2SlotGateLast != Decision::Evict)
            {
                done[waiting.id] = true;
                g_poseV2SlotGateWaitMaxMs = (std::max)(g_poseV2SlotGateWaitMaxMs,
                    nowUs > wait.sinceUs ? (nowUs - wait.sinceUs) / 1000 : 0);
            }
        }
        std::size_t kept = 0;
        for (std::size_t n = 0; n < g_poseV2SlotQueue.size(); ++n)
            if (!done[n])
                g_poseV2SlotQueue[kept++] = g_poseV2SlotQueue[n];
        g_poseV2SlotQueue.resize(kept);
    }

    // The state 3 handler's failure codes of this tick, by line (the
    // "revalidation" deaths: which check refused, world_theatre.h).
    void CollectPoseV2State3Codes()
    {
        static std::vector<replay::theatre::State3Failure> failures;
        replay::theatre::CollectState3Failures(g_poseV2State3Cursor, failures);
        for (const replay::theatre::State3Failure& failure : failures)
        {
            ++g_poseV2State3ByCode[failure.code];
            if (g_poseV2State3Codes.size() >= 512)
                g_poseV2State3Codes.erase(g_poseV2State3Codes.begin());
            g_poseV2State3Codes[failure.line] = failure.code;
        }
    }
    }
    std::vector<PoseV2DeferredLine> g_poseV2DeferredSpeech;
    std::uint64_t g_poseV2SpeechDeferred = 0;
    std::uint64_t g_poseV2SpeechDeferStops = 0;
    std::uint64_t g_poseV2SpeechDeferDropped = 0;
    std::uint64_t g_poseV2SpeechDeferMaxWaitMs = 0;
    std::uint64_t g_poseV2SpeechDeferOffsetLines = 0;  // deferred lines asked at the playhead's point in them
    std::uint32_t g_poseV2SpeechDeferOffsetMaxMs = 0;
    std::uint64_t g_poseV2SpeechDeferLateDropped = 0;  // ... not asked: under kMinRemainingMs of them left
    // The sync probe (POSE_V2_SPEECH_SYNC): lines measured at their natural end,
    // those whose sound was behind or ahead of the face past 100 ms, the worst.
    std::uint64_t g_poseV2SpeechSyncLines = 0;
    std::uint64_t g_poseV2SpeechSyncLate = 0;
    std::uint64_t g_poseV2SpeechSyncAhead = 0;
    std::int64_t g_poseV2SpeechSyncWorstMs = 0;
    std::uint32_t g_poseV2SpeechSyncWorstEventMs = 0;
    namespace
    {
    constexpr std::uint64_t kPoseV2SpeechDeferMaxMs = 1000;
    }
    // An animal's breaths and calls do not queue. A horse breathes every ~0.5 s;
    // the replayed breath (often another, longer phrase) may still play when the
    // next is due, so the queue would grow and leave holes. Live, the previous
    // one had ended: the new one starts at its time and the old one is stopped.
    std::uint64_t g_poseV2AnimalLineCuts = 0;
    std::uint64_t g_poseV2AnimalDeferDropped = 0;
    namespace
    {
    bool PoseV2AnimalSpeaker(replay::entity::ReplayId id)
    {
        const replay::playback::Puppet* speaker = Ctx().puppets.Find(id);
        if (!speaker || !speaker->clone || !replay::entities::Exists(speaker->clone))
            return false;
        return speaker->packet.kind == replay::entity::EntityKind::Horse || !PED::IS_PED_HUMAN(speaker->clone);
    }
    }
    namespace
    {

    // A line of the speaker still active (state 1 to 5) on its puppet, any
    // context (one speech slot per speech entity; the core's codes 18 and
    // 19), 0 when none.
    std::uint64_t PoseV2SameContextLine(const replay::entity::SpeechEvent& line)
    {
        const replay::playback::Puppet* speaker = Ctx().puppets.Find(line.speaker);
        if (!speaker || speaker->state != replay::playback::PuppetState::Ready || !speaker->clone ||
            !replay::entities::Exists(speaker->clone))
            return 0;
        std::uint64_t list = 0;
        std::uint16_t count = 0;
        if (!PoseV2SpeechLineList(speaker->clone, list, count) || !list || count > 64)
            return 0;
        for (std::uint16_t n = count; n-- > 0;)
        {
            std::uint64_t other = 0;
            if (!replay::audio::ReadU64(list + n * 8u, other) || !other)
                continue;
            std::int32_t state = -1;
            if (!replay::audio::ReadBytes(other + 0x234, &state, sizeof(state)) || state < 1 || state > 5)
                continue;
            return other;
        }
        return 0;
    }

    void PlayPoseV2DeferredSpeech(std::uint32_t timeMs, bool normalPlay)
    {
        if (g_poseV2DeferredSpeech.empty())
            return;
        std::ostream& log = Ctx().log;
        if (!normalPlay)
        {
            g_poseV2SpeechDeferDropped += g_poseV2DeferredSpeech.size();
            g_poseV2DeferredSpeech.clear();
            return;
        }
        const std::uint64_t nowUs = replay::audio::NowUs();
        for (std::size_t n = 0; n < g_poseV2DeferredSpeech.size();)
        {
            PoseV2DeferredLine& deferred = g_poseV2DeferredSpeech[n];
            const replay::entity::SpeechEvent& line = Ctx().clip.speechLines.Events()[deferred.index];
            const std::uint64_t waitedMs = nowUs > deferred.sinceUs ? (nowUs - deferred.sinceUs) / 1000 : 0;
            const std::uint64_t blocker = PoseV2SameContextLine(line);
            if (!blocker)
            {
                g_poseV2SpeechDeferMaxWaitMs = (std::max)(g_poseV2SpeechDeferMaxWaitMs, waitedMs);
                const std::size_t index = deferred.index;
                const std::uint32_t requestMs = deferred.requestMs;
                // At the playhead's point in the line, not from its start: the
                // wait must not become a desync the speaker's next lines inherit
                // (speech_resume.h AskAfterWait).
                const replay::speech::resume::AfterWait ask =
                    replay::speech::resume::AskAfterWait(line, timeMs, PoseV2LineEnd(index));
                log << "POSE_V2_SPEECH_DEFER,ped=" << replay::entity::IdText(line.speaker)
                    << ",phase=request,waited_ms=" << waitedMs << ",stopped=" << (deferred.stopped ? 1 : 0)
                    << ",offset_ms=" << ask.offsetMs << ",dropped=" << (ask.drop ? 1 : 0)
                    << ",event_ms=" << line.timeMs << ",playhead_ms=" << timeMs << '\n';
                g_poseV2DeferredSpeech.erase(g_poseV2DeferredSpeech.begin() + static_cast<std::ptrdiff_t>(n));
                if (ask.drop)
                {
                    ++g_poseV2SpeechDeferLateDropped;
                    continue;
                }
                if (ask.offsetMs)
                {
                    ++g_poseV2SpeechDeferOffsetLines;
                    g_poseV2SpeechDeferOffsetMaxMs = (std::max)(g_poseV2SpeechDeferOffsetMaxMs, ask.offsetMs);
                }
                RequestPoseV2SpeechLine(index, timeMs, requestMs, ask.offsetMs);
                continue;
            }
            if (!deferred.stopped && waitedMs >= kPoseV2SpeechDeferMaxMs)
            {
                deferred.stopped = true;
                const replay::playback::Puppet* speaker = Ctx().puppets.Find(line.speaker);
                std::string error;
                for (PoseV2PlayedSpeech& played : g_poseV2PlayedSpeech) // not a cut
                    if (played.line == blocker)
                        played.stoppedByReplay = true;
                if (speaker && speaker->clone &&
                    replay::audio::StopSpeechLine(replay::entities::AddressOf(speaker->clone), blocker, 2, error))
                    ++g_poseV2SpeechDeferStops;
                log << "POSE_V2_SPEECH_DEFER,ped=" << replay::entity::IdText(line.speaker)
                    << ",phase=stop_old,line=0x" << std::hex << blocker << std::dec << ",status=" << error
                    << ",event_ms=" << line.timeMs << ",playhead_ms=" << timeMs << '\n';
            }
            ++n;
        }
    }
    }
    std::vector<PoseV2PendingResume> g_poseV2PendingResume;

    // The lines the jump stop stopped, and a resume of one of them held back
    // while that copy is still stopping (speech_resume.h StepForOldCopy).
    std::vector<replay::speech::resume::StoppedCopy> g_poseV2StoppedCopies;
    std::uint64_t g_poseV2ResumeOldStopping = 0; // resumes that found the old copy stopping (waits begun)
    std::uint64_t g_poseV2ResumeOldGone = 0;     // of those, asked once it was gone
    std::uint64_t g_poseV2ResumeOldTimeout = 0;  // asked at the end of the window with it still stopping
    std::uint64_t g_poseV2ResumeOldWaitMaxMs = 0;
    std::uint64_t g_poseV2ResumeOldWaitTotalMs = 0;
    std::uint64_t g_poseV2ResumeOldSeen[5] = {}; // OldCopy at the first look, by kind
    std::uint32_t g_poseV2ResumeOldLogs = 0;
    namespace
    {
    constexpr std::uint32_t kPoseV2ResumeOldLogCap = 60;

    void StopPoseV2ReplayedLines(std::uint32_t timeMs, replay::speech::resume::Step step,
        replay::speech::resume::Why why)
    {
        std::uint64_t stopped = 0;
        const std::size_t followed = g_poseV2PlayedSpeech.size();
        for (PoseV2PlayedSpeech& played : g_poseV2PlayedSpeech)
        {
            if (!played.judged)
            {
                played.judged = true;
                g_poseV2SpeechLines.Judged(played.animal, replay::speech::LineOutcome::Abandoned);
            }
            if (!replay::entities::Exists(played.ped))
                continue;
            void* entity = replay::entities::AddressOf(played.ped);
            std::int32_t state = -1;
            if (!replay::audio::SpeechLineState(entity, played.line, state) || state < 1 || state > 5)
                continue;
            std::string error;
            // The copy's own identity before the stop (compared later at its address).
            replay::audio::StoppedLineView own;
            const bool ownRead = !played.animal && replay::audio::ReadStoppedLine(played.line, own) && own.read &&
                own.liveVtable;
            if (replay::audio::StopSpeechLine(entity, played.line, 2, error))
            {
                ++stopped;
                // A human copy a resume may have to wait for (animals are not said again
                // after a jump).
                const auto& events = Ctx().clip.speechLines.Events();
                if (!played.animal && played.eventIndex < events.size())
                {
                    const replay::entity::SpeechEvent& line = events[played.eventIndex];
                    replay::speech::resume::StoppedCopy copy;
                    copy.speaker = line.speaker.Packed();
                    copy.line = played.line;
                    copy.voice = line.voiceHash;
                    copy.context = line.contextPartialHash;
                    copy.variation = line.variation;
                    copy.stopUs = replay::audio::NowUs();
                    copy.ownRead = ownRead;
                    copy.ownVoice = own.voice;
                    copy.ownContext = own.context;
                    copy.ownVariation = own.variation;
                    g_poseV2StoppedCopies.push_back(copy);
                }
            }
        }
        replay::speech::resume::PruneStoppedCopies(g_poseV2StoppedCopies, replay::audio::NowUs());
        g_poseV2PlayedSpeech.clear();
        g_poseV2SpeechDeferDropped += g_poseV2DeferredSpeech.size();
        g_poseV2DeferredSpeech.clear();
        g_poseV2PendingResume.clear();
        g_poseV2SlotGateDropPlay += g_poseV2SlotQueue.size(); // the resume asks again
        g_poseV2SlotQueue.clear();
        g_poseV2SpeechJumpStops += stopped;
        if (step == replay::speech::resume::Step::Leave)
            ++g_poseV2SpeechLeaves;
        if ((stopped || followed) && g_poseV2SpeechResumeLogs.Take(kPoseV2SpeechResumeLogCap))
            Ctx().log << "SPEECH_JUMP_STOP,step=" << replay::speech::resume::StepName(step)
                      << ",why=" << replay::speech::resume::WhyName(why) << ",followed=" << followed
                      << ",stopped=" << stopped << ",playhead_ms=" << timeMs << '\n';
    }

    // The stopped copy of this line, read at its address (speech_resume.h
    // ClassifyOldCopy); Gone when none.
    replay::speech::resume::OldCopy PoseV2OldCopyOf(const replay::entity::SpeechEvent& line, std::uint64_t& oldLine)
    {
        using namespace replay::speech::resume;
        oldLine = 0;
        const StoppedCopy* copy = FindStoppedCopy(g_poseV2StoppedCopies, line.speaker.Packed(), line.voiceHash,
            line.contextPartialHash, line.variation);
        if (!copy)
            return OldCopy::Gone;
        oldLine = copy->line;
        replay::audio::StoppedLineView view;
        const bool read = replay::audio::ReadStoppedLine(copy->line, view);
        const bool same = replay::speech::resume::SameCopyIdentity(*copy, view.voice, view.context, view.variation);
        return ClassifyOldCopy(read && view.read, view.liveVtable, view.state, same);
    }

    // Asks one line again at the playhead; false when its puppet is not
    // ready (the caller keeps it pending).
    // Also false with `waitingOld` set while the line's previous copy is still
    // stopping (asked on a later tick from the pending list, at most
    // kOldCopyWaitMs after `waitSinceUs`, which is set on the first wait).
    bool ResumePoseV2Line(std::size_t index, std::uint32_t timeMs, const char* why, bool late, bool& waitingOld,
        std::uint64_t& waitSinceUs)
    {
        waitingOld = false;
        const auto& events = Ctx().clip.speechLines.Events();
        if (index >= events.size())
            return true;
        const replay::entity::SpeechEvent& line = events[index];
        const replay::playback::Puppet* speaker = Ctx().puppets.Find(line.speaker);
        if (!speaker || speaker->state != replay::playback::PuppetState::Ready || !speaker->clone ||
            !replay::entities::Exists(speaker->clone))
            return false;
        // The end and the offset again at this playhead (a pending line
        // waited some ticks).
        std::uint32_t next = 0;
        for (std::size_t n = index + 1; n < events.size(); ++n)
            if (events[n].speaker == line.speaker)
            {
                next = events[n].timeMs;
                break;
            }
        const replay::speech::resume::LineEnd end = replay::speech::resume::EndOf(line,
            index < g_poseV2LearnedEndMs.size() ? g_poseV2LearnedEndMs[index] : 0, next);
        if (end.endMs <= timeMs || end.endMs - timeMs < replay::speech::resume::kMinRemainingMs)
            return true; // no time left: dropped
        // The copy the jump stop stopped may still be stopping; a new copy asked
        // now often dies in its request.
        {
            std::uint64_t oldLine = 0;
            const replay::speech::resume::OldCopy copy = PoseV2OldCopyOf(line, oldLine);
            const std::uint64_t nowUs = replay::audio::NowUs();
            const bool first = waitSinceUs == 0;
            if (first)
                ++g_poseV2ResumeOldSeen[static_cast<std::size_t>(copy) < 5 ? static_cast<std::size_t>(copy) : 4];
            const std::uint64_t waitedMs = !first && nowUs > waitSinceUs ? (nowUs - waitSinceUs) / 1000 : 0;
            const replay::speech::resume::OldCopyStep stepOld =
                replay::speech::resume::StepForOldCopy(true, copy, waitedMs);
            if (stepOld == replay::speech::resume::OldCopyStep::Wait)
            {
                if (first)
                {
                    waitSinceUs = nowUs ? nowUs : 1;
                    ++g_poseV2ResumeOldStopping;
                }
                waitingOld = true;
                return false;
            }
            if (!first)
            {
                const bool timedOut = stepOld == replay::speech::resume::OldCopyStep::AskTimedOut;
                ++(timedOut ? g_poseV2ResumeOldTimeout : g_poseV2ResumeOldGone);
                g_poseV2ResumeOldWaitMaxMs = (std::max)(g_poseV2ResumeOldWaitMaxMs, waitedMs);
                g_poseV2ResumeOldWaitTotalMs += waitedMs;
                if (g_poseV2ResumeOldLogs++ < kPoseV2ResumeOldLogCap)
                    Ctx().log << "SPEECH_RESUME_WAIT_OLD,ped=" << replay::entity::IdText(line.speaker)
                              << ",old_line=0x" << std::hex << oldLine << std::dec
                              << ",old=" << replay::speech::resume::OldCopyName(copy) << ",waited_ms=" << waitedMs
                              << ",timed_out=" << (timedOut ? 1 : 0) << ",event_ms=" << line.timeMs
                              << ",playhead_ms=" << timeMs << '\n';
            }
        }
        const std::uint32_t offset = replay::speech::resume::OffsetAt(line, timeMs); // with startedBeforeMs
        ++g_poseV2SpeechResumeAsked;
        g_poseV2SpeechResumeWithOffset += offset ? 1 : 0;
        g_poseV2SpeechResumeLate += late ? 1 : 0;
        const bool created = RequestPoseV2SpeechLine(index, timeMs, line.timeMs, offset, true);
        g_poseV2SpeechResumeCreated += created ? 1 : 0;
        if (g_poseV2SpeechResumeLogs.Take(kPoseV2SpeechResumeLogCap))
            Ctx().log << "SPEECH_RESUME,ped=" << replay::entity::IdText(line.speaker) << ",why=" << why
                      << ",offset_ms=" << offset << ",remaining_ms=" << end.endMs - timeMs
                      << ",end_ms=" << end.endMs << ",end_source=" << end.source << ",end_known=" << (end.known ? 1 : 0)
                      << ",late=" << (late ? 1 : 0) << ",created=" << (created ? 1 : 0)
                      << ",context=0x" << std::hex << replay::audio::FinalizeHash(line.contextPartialHash) << std::dec
                      << ",event_ms=" << line.timeMs << ",playhead_ms=" << timeMs << '\n';
        return true;
    }

    void ResumePoseV2SpeechAt(std::uint32_t timeMs, replay::speech::resume::Why why)
    {
        ++g_poseV2SpeechResumes;
        const auto& events = Ctx().clip.speechLines.Events();
        static std::vector<std::uint32_t> leads;
        leads.resize(events.size());
        for (std::size_t n = 0; n < events.size(); ++n)
            leads[n] = replay::speech::StartLead(events[n].timeMs, events[n].coreMs, events[n].timedByStart,
                g_poseV2SpeechLoadMs);
        static std::vector<replay::speech::resume::Active> active;
        replay::speech::resume::ActiveAt(events, timeMs, leads, g_poseV2LearnedEndMs, active);
        const char* whyName = replay::speech::resume::WhyName(why);
        for (const replay::speech::resume::Active& line : active)
        {
            bool waitingOld = false;
            std::uint64_t waitSinceUs = 0;
            if (!ResumePoseV2Line(line.index, timeMs, whyName, false, waitingOld, waitSinceUs))
            {
                g_poseV2SpeechResumeNoPuppet += waitingOld ? 0 : 1;
                g_poseV2PendingResume.push_back({line.index, whyName, waitSinceUs});
            }
        }
    }

    // Pending resumes: asked once the puppet is ready, dropped when the line
    // has no time left.
    void PlayPoseV2PendingResume(std::uint32_t timeMs)
    {
        const auto& events = Ctx().clip.speechLines.Events();
        for (std::size_t n = 0; n < g_poseV2PendingResume.size();)
        {
            PoseV2PendingResume& pending = g_poseV2PendingResume[n];
            const bool gone = pending.index >= events.size() ||
                timeMs > events[pending.index].timeMs + replay::speech::resume::kUnknownEndCapMs;
            bool waitingOld = false;
            // A line held for its old copy is not late (the flag counts puppets not ready).
            if (gone || ResumePoseV2Line(pending.index, timeMs, pending.why, pending.waitSinceUs == 0, waitingOld,
                            pending.waitSinceUs))
            {
                g_poseV2PendingResume.erase(g_poseV2PendingResume.begin() + static_cast<std::ptrdiff_t>(n));
                continue;
            }
            ++n;
        }
    }
    }

    void PlayPoseV2Speech(std::uint32_t timeMs)
    {
        REPLAY_PROFILE_ZONE("play.speech");
        UpdatePoseV2PuppetSpeech();
        FollowPoseV2QueuedLines();
        PreloadPoseV2Voices(timeMs);
        UpdatePoseV2AnimalBanks(timeMs);
        const bool normalPlay = Ctx().hooks->NormalPlay();
        // A jump, a pause or a scrub stops the lines; normal play again says the
        // current ones with their start offset.
        const replay::speech::resume::Step step =
            g_poseV2SpeechWatch.Update(normalPlay, timeMs, replay::audio::NowUs(), Ctx().transport.Rate());
        const replay::speech::resume::Why why = g_poseV2SpeechWatch.LastWhy();
        if (step == replay::speech::resume::Step::Leave || step == replay::speech::resume::Step::Resume)
            StopPoseV2ReplayedLines(timeMs, step, why);
        CollectPoseV2State3Codes();
        FollowPoseV2PlayedSpeech(timeMs, normalPlay);
        PlayPoseV2SlotQueue(timeMs, normalPlay); // lines waiting for a slot first
        PlayPoseV2DeferredSpeech(timeMs, normalPlay);
        if (step == replay::speech::resume::Step::Resume)
        {
            ResumePoseV2SpeechAt(timeMs, why);
            g_poseV2SpeechCursorPlay.Reset(timeMs); // what starts after this playhead goes by the cursor
            return;
        }
        if (normalPlay && !g_poseV2PendingResume.empty())
            PlayPoseV2PendingResume(timeMs);
        std::uint32_t fromMs = 0;
        std::uint32_t toMs = 0;
        if (!g_poseV2SpeechCursorPlay.Advance(timeMs, normalPlay, fromMs, toMs))
            return;
        // A line is requested at its recorded start minus the load time
        // (StartLead), so the candidates reach that far past the slice.
        static std::vector<std::size_t> due;
        Ctx().clip.speechLines.Range(fromMs, toMs + g_poseV2SpeechLoadMs, due);
        for (const std::size_t index : due)
        {
            const replay::entity::SpeechEvent& line = Ctx().clip.speechLines.Events()[index];
            const std::uint32_t lead =
                replay::speech::StartLead(line.timeMs, line.coreMs, line.timedByStart, g_poseV2SpeechLoadMs);
            const std::uint32_t requestMs = line.timeMs > lead ? line.timeMs - lead : 0;
            if (requestMs <= fromMs || requestMs > toMs)
                continue;
            // An animal's new line does not wait: its older waiting lines go, and its
            // line still playing is stopped.
            if (PoseV2AnimalSpeaker(line.speaker))
            {
                const std::size_t before = g_poseV2DeferredSpeech.size();
                g_poseV2DeferredSpeech.erase(std::remove_if(g_poseV2DeferredSpeech.begin(), g_poseV2DeferredSpeech.end(),
                    [&](const PoseV2DeferredLine& d) { return Ctx().clip.speechLines.Events()[d.index].speaker == line.speaker; }),
                    g_poseV2DeferredSpeech.end());
                g_poseV2AnimalDeferDropped += before - g_poseV2DeferredSpeech.size();
                if (const std::uint64_t blocker = PoseV2SameContextLine(line))
                {
                    const replay::playback::Puppet* speaker = Ctx().puppets.Find(line.speaker);
                    std::string error;
                    for (PoseV2PlayedSpeech& played : g_poseV2PlayedSpeech)
                        if (played.line == blocker)
                            played.stoppedByReplay = true;
                    if (speaker && speaker->clone &&
                        replay::audio::StopSpeechLine(replay::entities::AddressOf(speaker->clone), blocker, 2, error))
                        ++g_poseV2AnimalLineCuts;
                }
            }
            // Wait for the speaker's line of the same context (a line already waiting
            // keeps the order).
            bool waiting = false;
            for (const PoseV2DeferredLine& deferred : g_poseV2DeferredSpeech)
                waiting = waiting || Ctx().clip.speechLines.Events()[deferred.index].speaker == line.speaker;
            // Nor ahead of the speaker's line waiting for a slot.
            for (const PoseV2SlotWait& wait : g_poseV2SlotQueue)
                waiting = waiting || Ctx().clip.speechLines.Events()[wait.index].speaker == line.speaker;
            if (waiting || PoseV2SameContextLine(line))
            {
                PoseV2DeferredLine deferred;
                deferred.index = index;
                deferred.requestMs = requestMs;
                deferred.sinceUs = replay::audio::NowUs();
                g_poseV2DeferredSpeech.push_back(deferred);
                ++g_poseV2SpeechDeferred;
                continue;
            }
            RequestPoseV2SpeechLine(index, timeMs, requestMs);
        }
    }
}
