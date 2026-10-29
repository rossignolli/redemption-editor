#include "speech_level_probe.h"
#include "speech_level_core.h"

#include "audio_observer.h"
#include "entity_cache.h"
#include "guarded_memory.h"
#include "log_policy.h"
#include "mix_scene.h"
#include "replay_playback.h"

#include <windows.h>

#include "main.h"
#include "natives.h"

#include <iomanip>
#include <ostream>
#include <string>

// The speech level probe (speech_level_probe.h). Script thread only; every
// engine access is a guarded read or a reading native.

namespace replay::speechlevel
{
    namespace level = replay::speech::level;

    namespace
    {
        bool g_enabled = false;
        level::Schedule g_schedule;
        level::Tally g_tally;
        replay::log::LogLimit g_lines;
        std::uint64_t g_nextTickWall = 0;
        std::uint64_t g_nextTotalWall = 0;
        constexpr std::uint64_t kTickMs = 250;
        constexpr std::uint64_t kRecordTotalEveryMs = 60000;
        constexpr std::uint32_t kLineCap = 1500;

        // Line layout (speech_request.h, speech_level_core.h).
        constexpr std::uint32_t kSpeechLines = 0x78;
        constexpr std::uint32_t kSpeechLineCount = 0x80;
        constexpr std::uint32_t kLineState = 0x234;
        constexpr std::uint32_t kLineData = 0x1D8;
        constexpr std::uint32_t kLineSource = 0x218;
        constexpr std::uint32_t kLineD8 = 0x214;
        constexpr std::uint32_t kLineVolumeType = 0x21C;
        constexpr std::uint32_t kLineAudibility = 0x220;
        constexpr std::uint32_t kLineBits = 0x2BC;
        constexpr std::uint32_t kDataType = 0xE0;
        constexpr std::uint32_t kDataVoice = 0x110;
        constexpr std::uint32_t kDataContext = 0x114;

        void ProbePed(Context& context, Entity handle, const std::string& who, const std::string& id, bool play,
            std::uint32_t clipMs, std::uint64_t wall)
        {
            if (!handle || !replay::entities::Exists(handle) || !replay::entities::IsPed(handle))
                return;
            const std::uint64_t speech = replay::audio::SpeechEntity(replay::entities::AddressOf(handle));
            if (!speech)
                return;
            std::uint64_t list = 0;
            std::uint16_t count = 0;
            if (!replay::mem::Read(speech + kSpeechLines, list) || !list ||
                !replay::mem::Read(speech + kSpeechLineCount, count) || count == 0 || count > 64)
                return;
            bool groupRead = false;
            std::uint32_t group = 0;
            for (std::uint16_t n = 0; n < count; ++n)
            {
                std::uint64_t line = 0;
                std::int32_t state = -1;
                if (!replay::mem::Read(list + n * 8u, line) || !line || !replay::mem::Read(line + kLineState, state) || state != 5)
                    continue;
                if (!g_schedule.Due(line, wall))
                    continue;
                std::uint64_t data = 0;
                std::uint32_t type = 0, voice = 0, contextHash = 0, source = 0, d8 = 0, volumeType = 0, audibility = 0, bits = 0;
                replay::mem::Read(line + kLineData, data);
                replay::mem::Read(line + kLineSource, source);
                replay::mem::Read(line + kLineD8, d8);
                replay::mem::Read(line + kLineVolumeType, volumeType);
                replay::mem::Read(line + kLineAudibility, audibility);
                replay::mem::Read(line + kLineBits, bits);
                bool hasVolume = false;
                float db = level::kSilentDb;
                if (data)
                {
                    replay::mem::Read(data + kDataType, type);
                    replay::mem::Read(data + kDataVoice, voice);
                    replay::mem::Read(data + kDataContext, contextHash);
                    std::uint8_t flags = 0;
                    float linear = 0.0f;
                    if (type == 1 &&
                        replay::mem::Read(data + level::kDataSmoother + level::kSmootherFlags, flags) && (flags & 1) &&
                        replay::mem::Read(data + level::kDataSmoother + level::kSmootherValue, linear))
                    {
                        hasVolume = true;
                        db = level::LinearToDb(linear);
                    }
                }
                if (!groupRead)
                {
                    group = static_cast<std::uint32_t>(AUDIO::_GET_ENTITY_AUDIO_MIX_GROUP(handle));
                    groupRead = true;
                }
                std::uint32_t sceneHash = 0;
                const bool ducked = replay::mixscene::ConversationActsOn(group, sceneHash);
                g_tally.Note(source, hasVolume, db, ducked);
                if (!g_lines.Take(kLineCap))
                    continue;
                std::ostream& log = context.log;
                const std::ios_base::fmtflags oldFlags = log.flags();
                const std::streamsize oldPrecision = log.precision();
                log << "SPEECH_LEVEL,phase=" << (play ? "play" : "record") << ",clip_ms=" << clipMs << ",ped=" << id
                    << ",who=" << who << ",line=0x" << std::hex << line << ",context=0x" << contextHash << ",voice=0x" << voice
                    << std::dec << ",source=" << source << ",type=" << type << ",d8=" << d8 << ",volume_type=" << volumeType
                    << ",audibility=" << audibility << ",duck_bit=" << ((bits >> 21) & 1u) << ",bits=0x" << std::hex << bits
                    << std::dec << ",vol_db=";
                if (hasVolume)
                    log << std::fixed << std::setprecision(1) << db;
                else
                    log << "na";
                log.flags(oldFlags);
                log.precision(oldPrecision);
                log << ",group=0x" << std::hex << group << ",conversation_duck=" << (ducked ? 1 : 0) << ",scene=0x" << sceneHash
                    << std::dec << ",open_scenes=" << replay::mixscene::OpenCount() << '\n';
            }
        }

        void LogTotal(Context& context, const char* phase)
        {
            context.log << "SPEECH_LEVEL_TOTAL,phase=" << phase << ",by_source=" << g_tally.Text()
                        << ",open_scenes=" << replay::mixscene::OpenText() << '\n';
            g_tally.Clear();
        }
    }

    void SetEnabled(bool enabled) { g_enabled = enabled; }
    bool Enabled() { return g_enabled; }

    void Tick(Context& context, bool play, std::uint32_t clipMs)
    {
        if (!g_enabled)
            return;
        const std::uint64_t wall = GetTickCount64();
        if (wall < g_nextTickWall)
            return;
        g_nextTickWall = wall + kTickMs;
        g_schedule.Forget(4096);
        if (play)
        {
            for (const replay::playback::Puppet& puppet : context.puppets.list)
                if (puppet.clone && (puppet.packet.kind == replay::entity::EntityKind::Ped ||
                                        puppet.packet.kind == replay::entity::EntityKind::Horse))
                    ProbePed(context, puppet.clone, "puppet", replay::entity::IdText(puppet.packet.id), true, clipMs, wall);
            ProbePed(context, PLAYER::PLAYER_PED_ID(), "live_player", "live", true, clipMs, wall);
            return;
        }
        for (const replay::recorder::RecordedEntity& entity : context.recorder.entities)
            if (!entity.vehicle)
                ProbePed(context, entity.handle, "recorded", replay::entity::IdText(entity.id), false, clipMs, wall);
        if (!g_nextTotalWall)
            g_nextTotalWall = wall + kRecordTotalEveryMs;
        else if (wall >= g_nextTotalWall)
        {
            g_nextTotalWall = wall + kRecordTotalEveryMs;
            LogTotal(context, "record");
        }
    }

    void BeginPlayback(Context& context)
    {
        if (!g_enabled)
            return;
        LogTotal(context, "record"); // what the recording saw since the last total
        g_schedule.Reset();
        g_nextTickWall = 0;
    }

    void EndPlayback(Context& context)
    {
        if (!g_enabled)
            return;
        LogTotal(context, "play");
        g_schedule.Reset();
        g_nextTotalWall = 0;
    }
}
