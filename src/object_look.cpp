#include "object_look.h"

#include "entity_cache.h"
#include "game_sig.h"
#include "guarded_memory.h"
#include "object_check.h"
#include "object_look_core.h"
#include "qa_kit.h"
#include "track_index.h"

#include "main.h"
#include "natives.h"

#include <windows.h>

#include <cstdio>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

namespace replay::objlook
{
    namespace
    {
        Context* g_context = nullptr;
        Context& Ctx() { return *g_context; }

        bool g_recordAndPlay = true;

        // ---- build check (once) --------------------------------------
        // object_look_core.h: the code that keeps the look's fields.
        namespace sig = replay::sig;

        // SET_CUSTOM_TEXTURES_ON_OBJECT's body (handle, txd, p2, p3) and its
        // call of the setter with &txd, &p2, &p3.
        const sig::Signature kObjectLookTxdNative{"object_look_txd_native",
            "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 30 41 8B F1 41 8B E8 44 8B F2 8B D9 "
            "E8 ?? ?? ?? ?? 41 B8 0F 00 00 00",
            0x10CECE4};
        const sig::Signature kObjectLookNativeCall{"object_look_native_call", kObjectLookTxdNative, sig::Offset{0x45},
            "4C 8D 4C 24 20 89 74 24 20 4C 8D 44 24 24 89 6C 24 24 48 8D 54 24 28 44 89 74 24 28 48 8B CF E8 ?? ?? ?? ??",
            0x10CED29};
        // The setter (object, &txd, &p2, &p3).
        const sig::Signature kObjectLookSetter{"object_look_setter", kObjectLookNativeCall, sig::Rel32{0x20, 0x24},
            "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 38 41 83 CC FF 4C 8D 2D ?? ?? ?? ?? 33 DB 4D 8B F9 4D "
            "8B F0 48 8B F2",
            0x840A5C};
        // cmp [rcx+0x420], ebx; je; mov [rcx+0x420], ebx (the setter clears the old txd)
        const sig::Signature kObjectLookSetterClear{"object_look_setter_clear", kObjectLookSetter, sig::Offset{0x2D},
            "39 99 20 04 00 00 74 4A 89 99 20 04 00 00", 0x840A89};
        // mov ecx, [rsi]; mov [rdi+0x420], ecx (txd stored)
        const sig::Signature kObjectLookSetterTxd{"object_look_setter_txd", kObjectLookSetter, sig::Offset{0x15C},
            "8B 0E 89 8F 20 04 00 00", 0x840BB8};
        // mov ecx, [r14]; mov [rdi+0x424], ecx (p2 stored)
        const sig::Signature kObjectLookSetterP2{"object_look_setter_p2", kObjectLookSetter, sig::Offset{0x1C4},
            "41 8B 0E 89 8F 24 04 00 00", 0x840C20};
        // mov ecx, [r15]; mov [rdi+0x428], ecx (p3 stored)
        const sig::Signature kObjectLookSetterP3{"object_look_setter_p3", kObjectLookSetter, sig::Offset{0x22D},
            "41 8B 0F 89 8F 28 04 00 00", 0x840C89};
        // SET_OBJECT_TINT_INDEX's body (handle, tint) and its call of the writer.
        const sig::Signature kObjectLookTintNative{"object_look_tint_native",
            "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8B F2 8B D9 E8 ?? ?? ?? ?? 41 B8 0F 00 00 00 8B D3 48 8B C8 48 8B "
            "F8 E8 ?? ?? ?? ?? 84 C0 74 21 48 85 FF 74 1C 8B D6",
            0x10D0650};
        const sig::Signature kObjectLookTintCall{"object_look_tint_call", kObjectLookTintNative, sig::Offset{0x34},
            "8B D6 48 8B CF E8 ?? ?? ?? ??", 0x10D0684};
        // The tint writer: and [rcx+0xB8], 0xFFFFFE07; and edx, 0x3F; shl edx, 3;
        // ...; or [rcx+0xB8], edx
        const sig::Signature kObjectLookTintWriter{"object_look_tint_writer", kObjectLookTintCall, sig::Rel32{6, 10},
            "48 83 EC 28 81 A1 B8 00 00 00 07 FE FF FF 83 E2 3F C1 E2 03 4C 8B C1 09 91 B8 00 00 00", 0xE5057C};

        struct Site
        {
            const sig::Signature* signature;
            const char* name;
        };
        // A call before what it reaches: the first failure names the cause.
        const Site kSites[] = {
            {&kObjectLookNativeCall, "look_native_body_signature_mismatch"},
            {&kObjectLookSetterClear, "look_setter_clear_signature_mismatch"},
            {&kObjectLookSetterTxd, "look_setter_txd_signature_mismatch"},
            {&kObjectLookSetterP2, "look_setter_p2_signature_mismatch"},
            {&kObjectLookSetterP3, "look_setter_p3_signature_mismatch"},
            {&kObjectLookTintCall, "look_tint_native_signature_mismatch"},
            {&kObjectLookTintWriter, "look_tint_writer_signature_mismatch"},
        };
        bool g_checked = false;
        bool g_checkOk = false;
        std::string g_checkError;

        bool CheckBuild()
        {
            if (g_checked)
                return g_checkOk;
            g_checked = true;
            g_checkOk = true;
            g_checkError.clear();
            for (const Site& site : kSites)
                if (!sig::Find(*site.signature))
                {
                    g_checkOk = false;
                    g_checkError = site.name;
                    break;
                }
            if (g_context)
                Ctx().log << "OBJECT_LOOK,phase=check,status=" << (g_checkOk ? "enabled" : "disabled")
                          << ",error=" << (g_checkError.empty() ? "none" : g_checkError) << ",record_play=" << (g_recordAndPlay ? 1 : 0) << '\n';
            return g_checkOk;
        }

        // ---- guarded read --------------------------------------------
        std::uint64_t g_readFaults = 0;

        // The fields of an object (the caller checked IS_ENTITY_AN_OBJECT).
        bool ReadAddress(const void* address, ObjectLook& look, std::uint32_t& tintWord)
        {
            const auto a = reinterpret_cast<std::uintptr_t>(address);
            std::uint32_t txd[3]{};
            if (!a || !replay::mem::Copy(txd, a + kObjectCustomTxdOffset, sizeof(txd)) ||
                !replay::mem::Read(a + kEntityTintWordOffset, tintWord))
            {
                ++g_readFaults;
                return false;
            }
            look.txd = txd[0];
            look.p2 = txd[1];
            look.p3 = txd[2];
            look.tint = TintFromWord(tintWord);
            return true;
        }

        bool ReadObject(Object object, ObjectLook& look, std::uint32_t& tintWord)
        {
            if (!object || !replay::entities::Exists(object) || !ENTITY::IS_ENTITY_AN_OBJECT(object))
                return false;
            return ReadAddress(replay::entities::AddressOf(object), look, tintWord);
        }

        std::string Hex(std::uint32_t value)
        {
            char text[16];
            std::snprintf(text, sizeof(text), "0x%08X", value);
            return text;
        }

        std::string LookText(const ObjectLook& look)
        {
            return "txd=" + Hex(look.txd) + ",p2=" + Hex(look.p2) + ",p3=" + Hex(look.p3) + ",tint=" + std::to_string(look.tint);
        }

        std::string KeyText(bool scenery, std::uint32_t key)
        {
            return scenery ? "scenery:" + std::to_string(key) : replay::entity::IdText(replay::entity::ReplayId::FromPacked(key));
        }

        // ---- recording -----------------------------------------------
        struct RecordStats
        {
            std::uint32_t reads = 0;
            std::uint32_t unread = 0;     // not an object or a failed guarded read
            std::uint32_t textured = 0;   // reads with a custom texture
            std::uint32_t tinted = 0;     // reads with a tint
            std::uint32_t tracks = 0;
            std::uint32_t changes = 0;
            std::uint32_t sceneryTracks = 0;
            std::uint32_t sceneryChanges = 0;
        } g_rec;
        std::uint32_t g_recordLogs = 0;
        // Scenery: the last read time per entry index.
        std::unordered_map<std::uint32_t, std::uint32_t> g_sceneryReadMs;
        replay::TrackIndex<ObjectLookTrack> g_objectIndex;
        replay::TrackIndex<ObjectLookTrack> g_sceneryIndex;

        void Note(bool scenery, std::uint32_t key, Object object, std::uint32_t timeMs, bool readBefore, std::uint32_t previousMs)
        {
            ObjectLook look;
            std::uint32_t tintWord = 0;
            if (!ReadObject(object, look, tintWord))
            {
                ++g_rec.unread;
                return;
            }
            ++g_rec.reads;
            g_rec.textured += look.txd ? 1u : 0u;
            g_rec.tinted += look.tint ? 1u : 0u;
            if (!g_recordAndPlay)
                return;
            bool created = false;
            std::vector<ObjectLookTrack>& tracks = scenery ? Ctx().clip.sceneryObjectLooks : Ctx().clip.objectLooks;
            if (!NoteObjectLook(tracks, scenery ? g_sceneryIndex : g_objectIndex, key, timeMs, readBefore, previousMs, look, &created))
                return;
            if (created)
                ++(scenery ? g_rec.sceneryTracks : g_rec.tracks);
            else
                ++(scenery ? g_rec.sceneryChanges : g_rec.changes);
            if (g_recordLogs < 64)
            {
                ++g_recordLogs;
                Ctx().log << "OBJECT_LOOK,phase=" << (created ? "record_entry" : "record") << ",id=" << KeyText(scenery, key)
                          << ",model=" << Hex(static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(object))) << ',' << LookText(look)
                          << ",seeded_default=" << (created && readBefore && previousMs < timeMs ? 1 : 0) << ",time_ms=" << timeMs
                          << '\n';
            }
        }

        // ---- playback ------------------------------------------------
        struct TxdState
        {
            std::uint64_t requestedAt = 0;
            bool exists = true;
            bool loaded = false;
            bool failed = false;
        };
        std::unordered_map<std::uint32_t, TxdState> g_txds;

        struct Target
        {
            ObjectLook wanted;
            bool hasWanted = false;
            std::uint32_t applies = 0;    // sets of `wanted` on this object
            bool gaveUp = false;          // a TXD failed or kLookMaxApplies reached
            std::uint64_t checkFirstAt = 0;
            std::uint64_t checkSecondAt = 0;
            std::uint32_t model = 0;
            std::string label;
            bool seen = false;            // touched this tick (stale entries go)
        };
        std::unordered_map<Object, Target> g_targets;
        std::uint64_t g_markSeen = 0;

        struct PlayStats
        {
            std::uint32_t sets = 0;
            std::uint32_t textureSets = 0;
            std::uint32_t tintSets = 0;
            std::uint32_t pending = 0;
            std::uint32_t txdRequested = 0;
            std::uint32_t txdFailed = 0;
            std::uint32_t checks = 0;
            std::uint32_t mismatches = 0;
            std::uint32_t gaveUp = 0;
        } g_play;
        std::uint32_t g_playLogs = 0;
        std::uint32_t g_checkLogs = 0;

        // The TXDs of `look` requested; true when all are loaded. `failed`:
        // one of them does not exist or timed out (the look is given up).
        bool StreamTxds(const ObjectLook& look, std::uint64_t now, bool& failed, std::uint32_t& failedHash)
        {
            failed = false;
            std::uint32_t hashes[3]{};
            const std::size_t count = ObjectLookTxds(look, hashes);
            bool all = true;
            for (std::size_t n = 0; n < count; ++n)
            {
                TxdState& state = g_txds[hashes[n]];
                if (state.failed)
                {
                    failed = true;
                    failedHash = hashes[n];
                    return false;
                }
                if (state.loaded)
                    continue;
                if (!state.requestedAt)
                {
                    state.requestedAt = now ? now : 1;
                    state.exists = TXD::DOES_STREAMED_TXD_EXIST(hashes[n]) != FALSE;
                    if (!state.exists)
                    {
                        state.failed = true;
                        ++g_play.txdFailed;
                        failed = true;
                        failedHash = hashes[n];
                        Ctx().log << "OBJECT_LOOK,phase=play,status=txd_missing,txd=" << Hex(hashes[n]) << '\n';
                        return false;
                    }
                    TXD::REQUEST_STREAMED_TXD(hashes[n], FALSE);
                    ++g_play.txdRequested;
                }
                if (TXD::HAS_STREAMED_TXD_LOADED(hashes[n]))
                {
                    state.loaded = true;
                    continue;
                }
                if (now - state.requestedAt > kLookTxdTimeoutMs)
                {
                    state.failed = true;
                    ++g_play.txdFailed;
                    failed = true;
                    failedHash = hashes[n];
                    Ctx().log << "OBJECT_LOOK,phase=play,status=txd_timeout,txd=" << Hex(hashes[n])
                              << ",waited_ms=" << (now - state.requestedAt) << '\n';
                    return false;
                }
                all = false;
            }
            return all;
        }

        void LogMismatch(const Target& target, const ObjectLook& read, LookCheck check, const char* when, std::uint32_t timeMs)
        {
            ++g_play.mismatches;
            if (g_checkLogs >= 48)
                return;
            ++g_checkLogs;
            // The object check line qa_health counts (ObjectWrong).
            Ctx().log << "POSE_V2_OBJECT_CHECK,id=" << target.label << ",verdict="
                      << replay::objcheck::VerdictText(replay::objcheck::Verdict::LookMismatch) << ",model=0x" << std::hex
                      << target.model << std::dec << ",what=" << LookCheckText(check) << ",when=" << when
                      << ",want_txd=" << Hex(target.wanted.txd) << ",got_txd=" << Hex(read.txd)
                      << ",want_tint=" << static_cast<int>(target.wanted.tint) << ",got_tint=" << static_cast<int>(read.tint)
                      << ",applies=" << target.applies << ",gave_up=" << (target.gaveUp ? 1 : 0) << ",playhead_ms=" << timeMs << '\n';
        }

        void CheckTarget(Object object, Target& target, const char* when, std::uint32_t timeMs)
        {
            ObjectLook read;
            std::uint32_t tintWord = 0;
            if (!target.hasWanted || !ReadObject(object, read, tintWord))
                return;
            ++g_play.checks;
            const LookCheck check = CheckObjectLook(target.wanted, read);
            if (check != LookCheck::Match)
                LogMismatch(target, read, check, when, timeMs);
        }

        // The look at the playhead on one object the replay owns.
        void Apply(Object object, const ObjectLook& wanted, bool scenery, std::uint32_t key, std::uint32_t model, std::uint32_t timeMs)
        {
            const std::uint64_t now = GetTickCount64();
            Target& target = g_targets[object];
            target.seen = true;
            if (!target.hasWanted || target.wanted != wanted)
            {
                target.wanted = wanted;
                target.hasWanted = true;
                target.applies = 0;
                target.gaveUp = false;
                target.checkFirstAt = target.checkSecondAt = 0;
            }
            target.model = model;
            if (target.label.empty())
                target.label = KeyText(scenery, key);
            // The scheduled re-reads (the detector).
            if (target.checkFirstAt && now >= target.checkFirstAt)
            {
                target.checkFirstAt = 0;
                CheckTarget(object, target, "0.5s", timeMs);
            }
            if (target.checkSecondAt && now >= target.checkSecondAt)
            {
                target.checkSecondAt = 0;
                CheckTarget(object, target, "2s", timeMs);
            }
            if (target.gaveUp)
                return;
            ObjectLook read;
            std::uint32_t tintWord = 0;
            if (!ReadObject(object, read, tintWord))
                return;
            if (read == wanted)
                return;
            if (target.applies >= kLookMaxApplies)
            {
                target.gaveUp = true;
                ++g_play.gaveUp;
                return;
            }
            const bool textures = ObjectTexturesDiffer(read, wanted);
            if (textures)
            {
                bool failed = false;
                std::uint32_t failedHash = 0;
                if (!StreamTxds(wanted, now, failed, failedHash))
                {
                    if (failed)
                    {
                        target.gaveUp = true;
                        ++g_play.gaveUp;
                        if (g_playLogs < 64)
                        {
                            ++g_playLogs;
                            Ctx().log << "OBJECT_LOOK,phase=play,status=gave_up,id=" << target.label << ",model=" << Hex(model)
                                      << ",txd_failed=" << Hex(failedHash) << ",playhead_ms=" << timeMs << '\n';
                        }
                    }
                    else
                        ++g_play.pending;
                    return;
                }
                OBJECT::SET_CUSTOM_TEXTURES_ON_OBJECT(object, wanted.txd, static_cast<Any>(wanted.p2), static_cast<Any>(wanted.p3));
                ++g_play.textureSets;
            }
            if (read.tint != wanted.tint)
            {
                OBJECT::SET_OBJECT_TINT_INDEX(object, wanted.tint);
                ++g_play.tintSets;
            }
            ++target.applies;
            ++g_play.sets;
            target.checkFirstAt = now + kLookCheckFirstMs;
            target.checkSecondAt = now + kLookCheckSecondMs;
            ObjectLook after;
            const bool readBack = ReadObject(object, after, tintWord);
            if (g_playLogs < 64)
            {
                ++g_playLogs;
                Ctx().log << "OBJECT_LOOK,phase=play,status=set,id=" << target.label << ",model=" << Hex(model) << ",want_"
                          << LookText(wanted) << ",from_txd=" << Hex(read.txd) << ",from_tint=" << static_cast<int>(read.tint)
                          << ",read_txd=" << (readBack ? Hex(after.txd) : std::string("-"))
                          << ",read_tint=" << (readBack ? std::to_string(after.tint) : std::string("-"))
                          << ",applies=" << target.applies << ",playhead_ms=" << timeMs << '\n';
            }
        }

        const ObjectLookTrack* FindTrack(const std::vector<ObjectLookTrack>& tracks, std::uint32_t key)
        {
            for (const ObjectLookTrack& track : tracks)
                if (track.id == key && !track.states.empty())
                    return &track;
            return nullptr;
        }
    }

    void Attach(Context& context) { g_context = &context; }
    void SetRecordAndPlay(bool on) { g_recordAndPlay = on; }
    bool RecordAndPlay() { return g_recordAndPlay; }
    bool Active() { return g_recordAndPlay && g_context && CheckBuild(); }

    bool ReadLook(Object object, ObjectLook& look, std::uint32_t& tintWord)
    {
        return CheckBuild() && ReadObject(object, look, tintWord);
    }

    void BeginRecording()
    {
        g_rec = {};
        g_recordLogs = 0;
        g_sceneryReadMs.clear();
        if (g_context)
            Active(); // the check line once, at the first recording
    }

    void EndRecording()
    {
        if (!g_context)
            return;
        Ctx().log << "OBJECT_LOOK,phase=record_end,enabled=" << (Active() ? 1 : 0) << ",record_play=" << (g_recordAndPlay ? 1 : 0)
                  << ",reads=" << g_rec.reads << ",unread=" << g_rec.unread
                  << ",textured=" << g_rec.textured << ",tinted=" << g_rec.tinted << ",tracks=" << g_rec.tracks
                  << ",changes=" << g_rec.changes << ",scenery_tracks=" << g_rec.sceneryTracks
                  << ",scenery_changes=" << g_rec.sceneryChanges << ",clip_tracks=" << Ctx().clip.objectLooks.size()
                  << ",clip_scenery_tracks=" << Ctx().clip.sceneryObjectLooks.size() << ",faults=" << g_readFaults << '\n';
        g_sceneryReadMs.clear();
    }

    void SampleObject(std::uint32_t key, Object object, std::uint32_t timeMs, bool& checked, std::uint32_t& checkMs)
    {
        if (checked && timeMs - checkMs < kLookReadMs)
            return;
        if (!Active())
            return;
        const bool readBefore = checked;
        const std::uint32_t previousMs = checkMs;
        checked = true;
        checkMs = timeMs;
        Note(false, key, object, timeMs, readBefore, previousMs);
    }

    void SampleScenery(std::uint32_t index, Object object, std::uint32_t timeMs)
    {
        const auto last = g_sceneryReadMs.find(index);
        const bool readBefore = last != g_sceneryReadMs.end();
        if (readBefore && timeMs - last->second < kLookReadMs)
            return;
        if (!Active())
            return;
        const std::uint32_t previousMs = readBefore ? last->second : 0;
        g_sceneryReadMs[index] = timeMs;
        Note(true, index, object, timeMs, readBefore, previousMs);
    }

    void BeginPlayback()
    {
        g_play = {};
        g_playLogs = 0;
        g_checkLogs = 0;
        g_targets.clear();
        g_markSeen = replay::qa::kit::MarkPresses();
        if (g_context && (!Ctx().clip.objectLooks.empty() || !Ctx().clip.sceneryObjectLooks.empty()))
            Ctx().log << "OBJECT_LOOK,phase=play_begin,enabled=" << (g_recordAndPlay && Active() ? 1 : 0)
                      << ",tracks=" << Ctx().clip.objectLooks.size() << ",scenery_tracks=" << Ctx().clip.sceneryObjectLooks.size()
                      << '\n';
    }

    void EndPlayback()
    {
        std::uint32_t released = 0;
        for (const auto& [hash, state] : g_txds)
            if (state.requestedAt && state.exists)
            {
                TXD::SET_STREAMED_TXD_AS_NO_LONGER_NEEDED(hash);
                ++released;
            }
        if (g_context && (g_play.sets || g_play.pending || !g_txds.empty() || g_play.mismatches))
            Ctx().log << "OBJECT_LOOK,phase=play_end,sets=" << g_play.sets << ",texture_sets=" << g_play.textureSets
                      << ",tint_sets=" << g_play.tintSets << ",pending_ticks=" << g_play.pending
                      << ",txd_requested=" << g_play.txdRequested << ",txd_failed=" << g_play.txdFailed
                      << ",txd_released=" << released << ",checks=" << g_play.checks << ",mismatches=" << g_play.mismatches
                      << ",gave_up=" << g_play.gaveUp << ",targets=" << g_targets.size() << '\n';
        g_txds.clear();
        g_targets.clear();
    }

    void ForgetAfterRestart()
    {
        g_txds.clear();
        g_targets.clear();
    }

    void PlayPuppets(std::uint32_t timeMs)
    {
        if (!g_context || !g_recordAndPlay)
            return;
        // A problem mark: every object with a look is checked now.
        const std::uint64_t marks = replay::qa::kit::MarkPresses();
        const bool marked = marks != g_markSeen;
        g_markSeen = marks;
        if (Ctx().clip.objectLooks.empty() && Ctx().clip.sceneryObjectLooks.empty())
            return;
        if (!Active())
            return;
        for (replay::playback::Puppet& puppet : Ctx().puppets.list)
        {
            const replay::entity::EntityKind kind = puppet.packet.kind;
            if ((kind != replay::entity::EntityKind::Weapon && kind != replay::entity::EntityKind::Prop) ||
                puppet.state != replay::playback::PuppetState::Ready || !puppet.clone || !replay::entities::Exists(puppet.clone))
                continue;
            const ObjectLookTrack* track = FindTrack(Ctx().clip.objectLooks, puppet.packet.id.Packed());
            if (!track)
                continue;
            Apply(puppet.clone, ObjectLookAt(*track, timeMs, false), false, puppet.packet.id.Packed(), puppet.packet.modelHash, timeMs);
        }
        if (marked)
            for (auto& [object, target] : g_targets)
                if (replay::entities::Exists(object))
                    CheckTarget(object, target, "mark", timeMs);
        // Entries of objects gone (puppet rebuilt, stand-in deleted).
        if (g_targets.size() > 64)
            for (auto it = g_targets.begin(); it != g_targets.end();)
                it = replay::entities::Exists(it->first) ? std::next(it) : g_targets.erase(it);
    }

    int SceneryTrackOf(std::uint32_t entry)
    {
        if (!g_context)
            return -1;
        const auto& tracks = Ctx().clip.sceneryObjectLooks;
        for (std::size_t n = 0; n < tracks.size(); ++n)
            if (tracks[n].id == entry && !tracks[n].states.empty())
                return static_cast<int>(n);
        return -1;
    }

    void PlayStand(Object standIn, bool scenery, std::uint32_t key, std::uint32_t model, std::uint32_t timeMs)
    {
        if (!g_context || !g_recordAndPlay || !standIn || !Active())
            return;
        const ObjectLookTrack* track = nullptr;
        if (scenery)
        {
            if (key < Ctx().clip.sceneryObjectLooks.size() && !Ctx().clip.sceneryObjectLooks[key].states.empty())
                track = &Ctx().clip.sceneryObjectLooks[key];
        }
        else
            track = FindTrack(Ctx().clip.objectLooks, key);
        if (!track)
            return;
        // A stand-in is shown before its object was first read (held at the
        // first state); a world object's stand-in only until the prop takes
        // it over, so it keeps the prop's first look.
        const ObjectLook wanted = scenery ? ObjectLookAt(*track, timeMs, true) : track->states.front().second;
        Apply(standIn, wanted, scenery, scenery ? track->id : key, model, timeMs);
    }
}
