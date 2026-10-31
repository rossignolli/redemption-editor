// audio_observer_speech.cpp: speech played: the direct say, voice pinning, animal vocals, line reads and stops, the puppets' own speech.
// Split from audio_observer.cpp (moved code); shared internals in audio_observer_internal.h.

#include "audio_observer_internal.h"

namespace replay::audio
{
    namespace
    {
        // The start offset in ms (PlaySpeechDirect in audio_observer.h has the
        // chain to the sound's init params).
        constexpr std::size_t kParamsStartOffset = 0x15C;

        // Speech replay path (the ambient speech native's).
        const sig::Signature kSpeechParamsCtor{"audio_speech_params_ctor",
            "40 53 48 83 EC 20 8B 15 ?? ?? ?? ?? B8 02 00 00 00 89 41 10",
            0x2F91EA4};
        const sig::Signature kSpeechParamsDtor{"audio_speech_params_dtor",
            "40 53 48 83 EC 20 48 8B D9 48 8B 89 A8 00 00 00 48 85 C9 74 0C", 0x143E98};

        // Direct line entry (speech_identity.h kDirectEntry): copies the params
        // and calls the core, without the AI Say step that marks the request
        // as AI speech (+0x19D) and so enables the AI checks. The engine's
        // conversation code enters here, so a recorded line plays without AI
        // gating. The request scope is the one the Say wrapper builds and
        // releases (its ctor, release and dtor).
        const sig::Signature kSpeechScopeCtor{"audio_speech_scope_ctor",
            "40 53 48 83 EC 20 BA E8 03 00 00 48 8B D9 E8 ?? ?? ?? ?? 33 C0", 0x2F92204};
        const sig::Signature kSpeechScopeDtor{"audio_speech_scope_dtor", kSpeechSay, sig::Rel32{0x3C, 0x40},
            "48 FF 25 ?? ?? ?? ??", 0x2595DA4};
        // Line start: the AI step calls it on the line the core attached to
        // the scope (+0x28) unless that line is in state 6 (+0x234), before the
        // wrapper releases the scope.
        const sig::Signature kSpeechLineStart{"audio_speech_line_start",
            "40 53 48 83 EC 20 48 8B D9 48 81 C1 88 01 00 00 E8 ?? ?? ?? ?? 84 C0", 0x2FB5F0C};

        // Ambient voice setter behind SET_AMBIENT_VOICE_NAME (its handler jumps
        // here with the ped and the name): after hashing the name it looks the
        // voice object up (store, hash, type 0x22), asks the speech entity for
        // its voice holder (vtable +0x128) and applies the voice (holder,
        // voice, 1, 1, callback). The replay repeats the steps after the hash
        // with the recorded voice hash; every address is read from these
        // verified bytes: the store, the typed store lookup (store, hash,
        // type; returns the object only when [obj+0] == type), the callback
        // and the apply (holder, voice, 1, 1, callback; writes holder +0x30).
        const sig::Signature kVoiceSetter{"audio_voice_setter",
            "48 85 C9 0F 84 ?? ?? ?? ?? 48 89 5C 24 08 57 48 83 EC 40 48 8B FA E8 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? "
            "?? F6 80 18 02 00 00 02 74 10 48 8B 80 98 00 00 00 48 8B 98 38 44 00 00 EB 02 33 DB 48 85 DB 74 60 33 D2 "
            "48 8B CF E8 ?? ?? ?? ?? 41 B0 22 48 8D 0D ?? ?? ?? ?? 8B D0 E8 ?? ?? ?? ?? 48 8B F8 48 85 C0 74 3D 48 8B "
            "13 48 8B CB FF 92 28 01 00 00 48 85 C0 74 2C 83 64 24 38 00 48 8D 0D ?? ?? ?? ?? 48 89 4C 24 30 41 B1 01 "
            "48 8D 4C 24 30 45 8A C1 48 89 4C 24 20 48 8B D7 48 8B C8 E8 ?? ?? ?? ??", 0xF73D80};
        const sig::Signature kVoiceStore{"audio_voice_store", kVoiceSetter, sig::Rel32{0x54, 0x58}, "", 0x5B87450};
        const sig::Signature kVoiceLookup{"audio_voice_lookup", kVoiceSetter, sig::Rel32{0x5B, 0x5F},
            "40 53 48 83 EC 20 41 0F B6 D8 45 33 C9 44 8B C3 E8 ?? ?? ?? ??",
            0x252B584};
        const sig::Signature kVoiceCallback{"audio_voice_callback", kVoiceSetter, sig::Rel32{0x80, 0x84},
            "", 0x34350B0};
        const sig::Signature kVoiceApply{"audio_voice_apply", kVoiceSetter, sig::Rel32{0xA0, 0xA4},
            "48 89 5C 24 08 48 89 6C 24 18 56 57 41 56 48 83 EC 50 80 3D ?? ?? ?? ?? 00 45 8A F1 41 8A E8 48 8B DA 48 "
            "8B F9 74 0B 48 3B 51 30 74 05 40 B6 01 EB 03 40 32 F6 48 85 D2 75 10 48 8B 1D ?? ?? ?? ?? 48 85 DB 0F 84 "
            "?? ?? ?? ?? 48 39 59 30 0F 84 ?? ?? ?? ?? 48 8B D3 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 40 84 ED 74 09 8B "
            "05 ?? ?? ?? ?? 89 43 10 48 8B 57 30 48 85 D2 74 0C 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 67 74 00 48 89 "
            "5F 30 48 8B 9C 24 90 00 00 00",
            0x2F96CC4};
        using VoiceHolderFn = void*(__fastcall*)(void*);

        std::uintptr_t g_speechCtor = 0;
        std::uintptr_t g_speechDtor = 0;
        std::uintptr_t g_speechDirect = 0;
        std::uintptr_t g_scopeCtor = 0;
        std::uintptr_t g_scopeRelease = 0;
        std::uintptr_t g_scopeDtor = 0;
        std::uintptr_t g_lineStart = 0;

        // The stop function to call from the script thread: the detour's
        // trampoline when the stop is hooked (signature already checked),
        // else the function itself after its signature check. Null when the
        // signature does not match (mismatched lines are then left alone
        // and only logged).
        std::uintptr_t LineStopFunction()
        {
            if (const SpeechLineStopFn original = g_originalStop.load(std::memory_order_acquire))
                return reinterpret_cast<std::uintptr_t>(original);
            static std::uintptr_t direct = 0;
            static bool checked = false;
            if (!checked)
            {
                checked = true;
                direct = sig::Find(kSpeechLineStop);
            }
            return direct;
        }

        struct DirectEvidence
        {
            bool attached = false;
            std::int32_t stateBefore = -1;
            std::int32_t stateAfter = -1;
            std::int64_t startResult = -1;
        };

        // Identity of the line the scope holds (all reads inside the
        // caller's SEH frame).
        void ReadPlayedLine(std::uint8_t* line, PlayedLine& played)
        {
            played.line = reinterpret_cast<std::uint64_t>(line);
            auto* data = *reinterpret_cast<std::uint8_t**>(line + 0x1D8);
            if (!data)
                return;
            played.voice = *reinterpret_cast<std::uint32_t*>(data + 0x110);
            played.context = *reinterpret_cast<std::uint32_t*>(data + 0x114);
            played.variation = *reinterpret_cast<std::int32_t*>(data + 0x118);
            played.dataType = *reinterpret_cast<std::uint32_t*>(data + 0xE0);
            // speech_slots.h: the voice source the slot was allocated by
            // (line +0x218 = params +0xF8, copied by the line init).
            played.source = *reinterpret_cast<std::int32_t*>(line + 0x218);
            // The request fields the line init copied (inside the caller's SEH
            // frame like the reads above).
            replay::speech::request::FromLineBlock(line + replay::speech::request::kLineBlockFrom,
                replay::speech::request::kLineBlockBytes, played.request);
            // The sound set the line plays from.
            replay::speech::request::SetFromDataBlock(data + replay::speech::request::kDataSetFrom,
                replay::speech::request::kDataSetBytes, played.request);
            auto* voiceObject = *reinterpret_cast<std::uint8_t**>(line + 0x1E8);
            if (voiceObject)
            {
                played.voiceObject = reinterpret_cast<std::uint64_t>(voiceObject);
                played.voiceObjectType = voiceObject[0];
                played.voiceObjectPartial = *reinterpret_cast<std::uint32_t*>(voiceObject + 8);
            }
            played.identityRead = true;
        }

        // speech_conversation.h: the SpeechParams applier's own calls for the
        // sound set, each found with the applier's call sites and writes, the
        // context applier's keep test (it fills +0xC0 only while it is 0) and
        // the data copy the recorder reads. One missing disables the set for
        // the session (nothing written, status disabled) and names the site
        // (SPEECH_CONVERSATION_CHECK). The set metadata is `lea rax,
        // [metadata]; ret`; the ref lookup (metadata, out, hash, -1): *out =
        // chunk << 24 | offset, -1 if none; the set object lookup ({ref,
        // 0xFFFFF}): the type 0x29 object of the ref, or null.
        const sig::Signature kSetApplier{"audio_set_applier",
            "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 8B 6A 14", 0x2F967E8};
        const sig::Signature kApplierHashWrite{"applier_hash_write", kSetApplier, sig::Offset{0x23},
            "89 A9 C8 00 00 00", 0x2F9680B};
        const sig::Signature kApplierMetadataCall{"applier_metadata_call", kSetApplier, sig::Offset{0x29},
            "E8 ?? ?? ?? ??", 0x2F96811};
        const sig::Signature kApplierRefCall{"applier_ref_call", kSetApplier, sig::Offset{0x3D},
            "E8 ?? ?? ?? ??", 0x2F96825};
        const sig::Signature kApplierRefWrite{"applier_ref_write", kSetApplier, sig::Offset{0x48},
            "89 8B CC 00 00 00", 0x2F96830};
        const sig::Signature kApplierObjectCall{"applier_object_call", kSetApplier, sig::Offset{0x5B},
            "E8 ?? ?? ?? ??", 0x2F96843};
        const sig::Signature kApplierObjectWrite{"applier_object_write", kSetApplier, sig::Offset{0x65},
            "48 89 83 C0 00 00 00", 0x2F9684D};
        const sig::Signature kSetMetadata{"metadata_prologue", kApplierMetadataCall, sig::Rel32{0x1, 0x5},
            "48 8D 05 ?? ?? ?? ?? C3", 0x252AD8C};
        const sig::Signature kSetRefByHash{"ref_by_hash_prologue", kApplierRefCall, sig::Rel32{0x1, 0x5},
            "40 53 48 83 EC 40 41 8B C0 C6 44 24 28 00 44 89 4C 24 20 4C 8D 44 24 34 48 8B DA 4C 8D 4C 24 30 8B D0 E8 "
            "?? ?? ?? ??",
            0x252B408};
        const sig::Signature kSetObject{"object_prologue", kApplierObjectCall, sig::Rel32{0x1, 0x5},
            "48 83 EC 28 45 33 C9 8B D1 48 8D 0D ?? ?? ?? ??", 0x252B194};
        const sig::Signature kContextApplier{"audio_context_applier",
            "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 33 ED 48 89 91 B8 00 00 00", 0x2F965D0};
        const sig::Signature kContextKeepsObject{"context_keeps_object", kContextApplier, sig::Offset{0x23},
            "48 39 A9 C0 00 00 00 75 50", 0x2F965F3};
        const sig::Signature kLineDataInit{"audio_line_data_init",
            "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 F3 0F 10 0D ?? ?? ?? ?? 49 8B F0", 0x2FA92C0};
        const sig::Signature kDataCopiesSet{"data_copies_set", kLineDataInit, sig::Offset{0x67},
            "0F 10 86 C0 00 00 00 F3 0F 7F 87 E8 00 00 00", 0x2FA9327};
        const sig::Signature* const kSetCheckSites[] = {&kSetMetadata, &kSetRefByHash, &kSetObject, &kApplierHashWrite,
            &kApplierMetadataCall, &kApplierRefCall, &kApplierRefWrite, &kApplierObjectCall, &kApplierObjectWrite,
            &kContextKeepsObject, &kDataCopiesSet};

        std::uintptr_t g_setMetadata = 0;
        std::uintptr_t g_setRefByHash = 0;
        std::uintptr_t g_setObject = 0;

        bool g_setChecked = false;
        bool g_setOk = false;
        const char* g_setFailed = "unchecked";

        bool EnsureSetCalls()
        {
            if (g_setChecked)
                return g_setOk;
            g_setChecked = true;
            g_setFailed = "none";
            for (const sig::Signature* site : kSetCheckSites)
            {
                if (!sig::Find(*site))
                {
                    g_setFailed = site->name;
                    break;
                }
            }
            g_setOk = std::strcmp(g_setFailed, "none") == 0;
            if (g_setOk)
            {
                g_setMetadata = sig::Find(kSetMetadata);
                g_setRefByHash = sig::Find(kSetRefByHash);
                g_setObject = sig::Find(kSetObject);
            }
            return g_setOk;
        }

        // The applier's calls: the metadata, then the ref of the hash.
        bool GuardedSetRefByHash(std::uint32_t hash, std::uint32_t& ref, std::uint32_t& code)
        {
            __try
            {
                void* metadata = nullptr;
                if (!engine::Call(engine::kSpeechSetMetadata, g_setMetadata, metadata))
                    return engine::Failed(code);
                std::uint32_t out[2] = {0xFFFFFFFFu, replay::speech::conversation::kRefHighAny};
                std::uint32_t* got = nullptr;
                if (!engine::Call(engine::kSpeechSetRefByHash, g_setRefByHash, got, metadata, out, hash, -1))
                    return engine::Failed(code);
                ref = got ? *got : 0xFFFFFFFFu;
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // The applier's next call: the set object of the ref ({ref, 0xFFFFF}).
        bool GuardedSetObject(std::uint32_t ref, std::uint64_t& object, std::uint32_t& code)
        {
            const std::uint64_t key = (static_cast<std::uint64_t>(replay::speech::conversation::kRefHighAny) << 32) | ref;
            void* set = nullptr;
            if (!engine::Call(engine::kSpeechSetObject, g_setObject, set, key))
                return engine::Failed(code);
            object = reinterpret_cast<std::uint64_t>(set);
            return true;
        }

        // The object of a recorded ref (no name looked up) checked before it
        // goes into params: its count and name table read the way the name
        // search reads them, the hashes ascending (speech_conversation.h
        // SetTableAscending). A real set passes; a ref from another metadata
        // layout that landed on a 0x29 byte inside another object does not.
        // Read with the plain floor (replay::mem::kLowest): the pointer is the
        // engine's own lookup result, bounded by its chunk lookup to a loaded
        // chunk.
        bool SetObjectLooksReal(std::uint64_t object)
        {
            using namespace replay::speech::conversation;
            std::uint32_t count = 0;
            if (!replay::mem::Copy(&count, static_cast<std::uintptr_t>(object + kSetCountOffset), sizeof(count)) ||
                count == 0 || count > kSetMaxEntries)
                return false;
            std::vector<std::uint8_t> table(static_cast<std::size_t>(count) * kSetEntryBytes);
            if (!replay::mem::Copy(table.data(), static_cast<std::uintptr_t>(object + kSetEntriesOffset), table.size()))
                return false;
            return SetTableAscending(table.data(), count);
        }

        // What GuardedSayDirect writes into params after the ctor.
        struct SetWrite
        {
            bool write = false;
            std::uint32_t hash = 0;
            std::uint32_t ref = 0;
            std::uint64_t object = 0;
        };

        // Resolves the asked set the applier's way, on the script thread,
        // before the request (an exception here leaves the line as before).
        // Only a found object is written: without one the applier leaves
        // +0xC0 at 0 and the context applier then overwrites +0xC8/+0xCC.
        replay::speech::conversation::SetStatus ResolveSoundSet(const replay::speech::request::Ask& ask, SetWrite& out)
        {
            using replay::speech::conversation::SetStatus;
            out = SetWrite{};
            if (ask.setFrom == replay::speech::request::SetFrom::None)
                return SetStatus::NotAsked;
            if (!EnsureSetCalls())
                return SetStatus::Disabled;
            std::uint32_t code = 0;
            std::uint32_t hashRef = 0xFFFFFFFFu;
            bool hashLooked = false;
            if (ask.setHash)
            {
                if (!GuardedSetRefByHash(ask.setHash, hashRef, code))
                    return SetStatus::Exception;
                hashLooked = true;
            }
            bool byHash = false;
            const std::uint32_t ref = replay::speech::conversation::RefToResolve(ask, hashLooked, hashRef, byHash);
            if (!ref)
                return SetStatus::NoObject;
            std::uint64_t object = 0;
            if (!GuardedSetObject(ref, object, code))
                return SetStatus::Exception;
            if (!object)
                return SetStatus::NoObject;
            // By hash the object is the applier's own chain; by a recorded
            // ref it is checked first (a bad one would be read by the line's
            // set searches on the audio side, outside this guard).
            if (!byHash && !SetObjectLooksReal(object))
                return SetStatus::BadObject;
            out.write = true;
            out.hash = ask.setHash; // the applier writes the hash it was given (0 from the sounds)
            out.ref = ref;
            out.object = object;
            return byHash ? SetStatus::ByHash : SetStatus::ByRef;
        }

        bool GuardedSayDirect(std::uint8_t* speech, std::uint8_t* params, std::uint8_t* scope,
            std::uint32_t voice, std::uint32_t context, std::int32_t variation,
            const replay::speech::Identity* expected, std::uintptr_t stop,
            std::uint32_t& result, DirectEvidence& evidence, PlayedLine& played, std::uint32_t& code,
            std::uint32_t startOffsetMs, std::int32_t source, const replay::speech::request::Ask* ask,
            const SetWrite* set)
        {
            __try
            {
                if (!engine::Call(engine::kSpeechParamsCtor, g_speechCtor, params))
                    return engine::Failed(code);
                // speech_request.h: the recorded line type (+0xF4, 0 after the
                // ctor: the validator would write 2) and the recorded
                // request fields, each already range checked by BuildAsk.
                if (ask)
                {
                    if (ask->lineType)
                        *reinterpret_cast<std::uint32_t*>(params + replay::speech::request::kParamsLineType) = ask->lineType;
                    for (std::uint8_t n = 0; n < ask->count && n < replay::speech::request::kFieldCount; ++n)
                        *reinterpret_cast<std::uint32_t*>(params + ask->writes[n].offset) = ask->writes[n].value;
                    // speech_script_path.h: flag bytes the line init turns into
                    // line bits (the duck byte +0x19E).
                    for (std::uint8_t n = 0; n < ask->byteCount && n < replay::speech::request::kMaxByteWrites; ++n)
                        if (ask->bytes[n].offset < 0x200)
                            params[ask->bytes[n].offset] = ask->bytes[n].value;
                }
                // speech_conversation.h: the recorded sound set, the applier's
                // three writes (kApplierHashWrite, kApplierRefWrite,
                // kApplierObjectWrite); the params copy keeps them and the
                // context applier skips a params with +0xC0 set
                // (kContextKeepsObject).
                if (set && set->write)
                {
                    *reinterpret_cast<std::uint32_t*>(params + replay::speech::conversation::kParamsSetHash) = set->hash;
                    *reinterpret_cast<std::uint32_t*>(params + replay::speech::conversation::kParamsSetRef) = set->ref;
                    *reinterpret_cast<std::uint64_t*>(params + replay::speech::conversation::kParamsSetObject) = set->object;
                }
                // speech_slots.h: the ctor leaves +0xF8 at 0 (it zeroes
                // +0xF4..+0xFB) and the context metadata only fills it when
                // still 0: 4 asks an ambient slot by priority with
                // the line's own wave, instead of the bank reuse of source 3.
                if (source > 0)
                    *reinterpret_cast<std::int32_t*>(params + replay::speech::slots::kParamsSource) = source;
                *reinterpret_cast<std::uint32_t*>(params + kParamsContext) = context;
                *reinterpret_cast<std::uint32_t*>(params + kParamsVoice) = voice;
                if (startOffsetMs) // the ctor left 0
                    *reinterpret_cast<std::uint32_t*>(params + kParamsStartOffset) = startOffsetMs;
                if (variation > 0)
                {
                    *reinterpret_cast<std::int32_t*>(params + kParamsVariation) = variation;
                    params[0x192] = 0;
                    params[0x198] = 0;
                }
                // +0x18D is not set: in the variation choice it only enables a
                // random re-draw when the recorded variation is refused. Skip
                // the recently-played check: the params byte +0x18C becomes the
                // line's bit (+0x2BC bit 7) that the repeat check (code 22)
                // tests before refusing a line heard
                // moments ago, as the recorded one always was; a replay ignores
                // the speech history. Without it every replayed line is stopped
                // in state 3. It also enables the re-draw, so the created line
                // is compared below before it may start.
                params[0x18C] = 1;
                if (!engine::Call(engine::kSpeechScopeCtor, g_scopeCtor, scope) ||
                    !engine::Call(engine::kSpeechDirectEntry, g_speechDirect, result, speech, scope, params))
                    return engine::Failed(code);
                // As the AI step does after the core: start the line the
                // scope now holds, unless it is in state 6 or it is not the
                // recorded line.
                auto* line = *reinterpret_cast<std::uint8_t**>(scope + 0x28);
                evidence.attached = line != nullptr;
                if (line)
                {
                    evidence.stateBefore = *reinterpret_cast<std::int32_t*>(line + 0x234);
                    ReadPlayedLine(line, played);
                    if (expected && played.identityRead)
                    {
                        replay::speech::Identity got;
                        got.voice = played.voice;
                        got.context = played.context;
                        got.variation = played.variation;
                        got.voiceObjectHash = played.voiceObject ? replay::speech::Finalize(played.voiceObjectPartial) : 0;
                        got.voiceObjectType = played.voiceObjectType;
                        played.mismatch = replay::speech::Compare(*expected, got);
                    }
                    else if (expected)
                    {
                        played.mismatch = replay::speech::kMismatchVoice | replay::speech::kMismatchContext |
                            replay::speech::kMismatchVariation;
                    }
                }
                if (line && played.mismatch && stop && *reinterpret_cast<std::int32_t*>(line + 0x234) != 6)
                {
                    if (!engine::Call(engine::kSpeechLineStop, stop, line, 2))
                        return engine::Failed(code);
                    played.stoppedOnMismatch = true;
                }
                else if (line && !played.mismatch && *reinterpret_cast<std::int32_t*>(line + 0x234) != 6)
                {
                    std::uint32_t started = 0;
                    if (!engine::Call(engine::kSpeechLineStart, g_lineStart, started, line))
                        return engine::Failed(code);
                    evidence.startResult = started;
                }
                if (line)
                    evidence.stateAfter = *reinterpret_cast<std::int32_t*>(line + 0x234);
                if (!engine::Call(engine::kSpeechScopeRelease, g_scopeRelease, scope) ||
                    !engine::Call(engine::kSpeechScopeDtor, g_scopeDtor, scope) ||
                    !engine::Call(engine::kSpeechParamsDtor, g_speechDtor, params))
                    return engine::Failed(code);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        std::uint8_t* g_voiceStore = nullptr;
        void* g_voiceCallback = nullptr;
        std::uintptr_t g_voiceLookup = 0;
        std::uintptr_t g_voiceApply = 0;
        bool g_voiceChecked = false;
        std::string g_voiceCheckError;

        struct VoiceCallback
        {
            void* target = nullptr;
            std::uint32_t value = 0;
        };

        // The setter, then the lookup and apply its calls reach, each with
        // its prologue, and the store and the callback its operands name.
        bool EnsureVoiceCalls(std::string& error)
        {
            if (g_voiceChecked)
            {
                error = g_voiceCheckError;
                return g_voiceApply != 0;
            }
            g_voiceChecked = true;
            const std::uintptr_t store = sig::Find(kVoiceStore);
            const std::uintptr_t callback = sig::Find(kVoiceCallback);
            if (!sig::Find(kVoiceSetter) || !store || !callback)
            {
                g_voiceCheckError = error = "voice_setter_signature_mismatch";
                return false;
            }
            const std::uintptr_t lookup = sig::Find(kVoiceLookup);
            const std::uintptr_t apply = sig::Find(kVoiceApply);
            if (!lookup)
            {
                g_voiceCheckError = error = "voice_lookup_signature_mismatch";
                return false;
            }
            if (!apply)
            {
                g_voiceCheckError = error = "voice_apply_signature_mismatch";
                return false;
            }
            g_voiceStore = reinterpret_cast<std::uint8_t*>(store);
            g_voiceCallback = reinterpret_cast<void*>(callback);
            g_voiceLookup = lookup;
            g_voiceApply = apply;
            return true;
        }

        // stage 1: lookup, 3: holder, 4: apply, 5: done. `source` 1 = typed
        // lookup (the only source).
        bool GuardedPinVoice(std::uint8_t* speech, std::uint32_t voiceHash, std::uint8_t voiceType,
            std::uint8_t* recordedObject, int& stage, int& source, std::uint32_t& code)
        {
            __try
            {
                stage = 1;
                void* voice = nullptr;
                if (!engine::Call(engine::kVoiceLookup, g_voiceLookup, voice, g_voiceStore, voiceHash, voiceType))
                    return engine::Failed(code);
                source = 1;
                // The recorded voice object pointer is never dereferenced: it
                // belongs to the session that recorded the clip and can point
                // at freed memory after a save load (same class as the decal
                // info pointer). Kept only in the log.
                (void)recordedObject;
                if (!voice)
                    return false;
                stage = 3;
                auto* vtable = *reinterpret_cast<VoiceHolderFn**>(speech);
                void* holder = vtable[0x128 / sizeof(void*)](speech);
                if (!holder)
                    return false;
                stage = 4;
                VoiceCallback callback;
                callback.target = g_voiceCallback;
                if (!engine::Call(engine::kVoiceApply, g_voiceApply, holder, voice, 1, 1, &callback))
                    return engine::Failed(code);
                stage = 5;
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool GuardedVoiceLookup(std::uint32_t hash, std::uint8_t type, void*& object)
        {
            return engine::Call(engine::kVoiceLookup, g_voiceLookup, object, g_voiceStore, hash, type);
        }
    }

        const sig::Signature kRefContainer{"audio_ref_container",
            "48 83 EC 28 45 33 C9 8B D1 48 8D 0D ?? ?? ?? ?? 45 8D 41 29", 0x252B194};
        bool GuardedRefContainer(std::uintptr_t fn, std::uint32_t key, void*& out)
        {
            return engine::Call(engine::kRefContainer, fn, out, key);
        }

    std::uint64_t RefusedPlayRequests() { return g_refusedPlayRequests.load(std::memory_order_relaxed); }

    const char* IndirectRefState(std::uint64_t soundRef)
    {
        if (((soundRef >> 32) & 0xF00000) != 0x800000)
            return "direct";
        const std::uintptr_t lookup = sig::Find(kRefContainer);
        if (!lookup)
            return "container_signature_mismatch";
        void* container = nullptr;
        if (!GuardedRefContainer(lookup, static_cast<std::uint32_t>(soundRef), container))
            return "container_exception";
        return container ? "indirect_found" : "indirect_missing";
    }

    bool PinSpeechVoice(void* pedEntity, std::uint32_t voiceHash, std::uint8_t voiceType,
        std::uint64_t recordedObject, std::string& status)
    {
        if (!EnsureVoiceCalls(status))
            return false;
        std::uint8_t* speech = reinterpret_cast<std::uint8_t*>(SpeechEntity(pedEntity));
        if (!speech)
        {
            status = "no_speech_entity";
            return false;
        }
        int stage = 0;
        int source = 0;
        std::uint32_t code = 0;
        if (!GuardedPinVoice(speech, voiceHash, voiceType ? voiceType : 0x22,
                reinterpret_cast<std::uint8_t*>(recordedObject), stage, source, code))
        {
            char text[48]{};
            if (code)
                std::snprintf(text, sizeof(text), "exception_%08X_stage_%d", code, stage);
            else
                std::snprintf(text, sizeof(text), stage <= 2 ? "voice_not_found" : "no_voice_holder");
            status = text;
            // Evidence: which metadata type holds this name.
            if (stage <= 2 && !code)
            {
                static std::vector<std::uint32_t> probed;
                if (std::find(probed.begin(), probed.end(), voiceHash) == probed.end() && probed.size() < 64)
                {
                    probed.push_back(voiceHash);
                    char found[24] = "none";
                    for (int type = 1; type < 0x80; ++type)
                    {
                        void* object = nullptr;
                        if (GuardedVoiceLookup(voiceHash, static_cast<std::uint8_t>(type), object) && object)
                        {
                            std::snprintf(found, sizeof(found), "0x%02X", type);
                            break;
                        }
                    }
                    status += ",found_type=";
                    status += found;
                }
            }
            return false;
        }
        (void)source;
        status = "pinned_lookup";
        return true;
    }

    bool SpeechSoundSetCallsOk(std::string& failed)
    {
        const bool ok = EnsureSetCalls();
        failed = g_setFailed;
        return ok;
    }

    bool PlaySpeechDirect(void* pedEntity, std::uint32_t voiceHash, std::uint32_t contextPartialHash,
        std::int32_t variation, const replay::speech::Identity* expected, std::uint32_t& result,
        PlayedLine& played, std::string& error, std::uint32_t startOffsetMs, std::int32_t source,
        const replay::speech::request::Ask* ask)
    {
        if (!g_speechDirect)
        {
            const std::uintptr_t ctor = sig::Find(kSpeechParamsCtor), dtor = sig::Find(kSpeechParamsDtor),
                                 direct = sig::Find(replay::speech::kDirectEntry), scopeCtor = sig::Find(kSpeechScopeCtor),
                                 scopeRelease = sig::Find(kSpeechScopeRelease), scopeDtor = sig::Find(kSpeechScopeDtor),
                                 lineStart = sig::Find(kSpeechLineStart);
            if (!ctor || !dtor || !direct || !scopeCtor || !scopeRelease || !scopeDtor || !lineStart)
            {
                error = "speech_direct_signature_mismatch";
                return false;
            }
            g_speechCtor = ctor;
            g_speechDtor = dtor;
            g_scopeCtor = scopeCtor;
            g_scopeRelease = scopeRelease;
            g_scopeDtor = scopeDtor;
            g_lineStart = lineStart;
            g_speechDirect = direct;
        }
        std::uint8_t* speech = reinterpret_cast<std::uint8_t*>(SpeechEntity(pedEntity));
        if (!speech)
        {
            error = "no_speech_entity";
            return false;
        }
        alignas(16) std::uint8_t params[0x200] = {};
        // The Say wrapper keeps the scope in 0x40 bytes of its frame.
        alignas(16) std::uint8_t scope[0x80] = {};
        std::uint32_t code = 0;
        DirectEvidence evidence;
        played = PlayedLine{};
        const std::uintptr_t stop = LineStopFunction();
        // The asked sound set, resolved first.
        SetWrite set;
        const replay::speech::conversation::SetStatus setStatus =
            ask ? ResolveSoundSet(*ask, set) : replay::speech::conversation::SetStatus::NotAsked;
        const bool said = GuardedSayDirect(speech, params, scope, voiceHash, contextPartialHash, variation, expected, stop,
            result, evidence, played, code, startOffsetMs, source, ask, &set);
        played.setStatus = static_cast<std::uint8_t>(setStatus);
        played.setWrittenRef = set.write ? set.ref : 0;
        if (!said)
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        char text[128]{};
        std::snprintf(text, sizeof(text), "result_%u|attached_%d|state_%d_to_%d|start_%lld%s", result,
            evidence.attached ? 1 : 0, evidence.stateBefore, evidence.stateAfter,
            static_cast<long long>(evidence.startResult),
            played.mismatch ? (played.stoppedOnMismatch ? "|mismatch_stopped" : "|mismatch_no_stop") : "");
        error = text;
        return true;
    }
    namespace
    {

    // An animal's vocalization asked the way the native
    // PLAY_ANIMAL_VOCALIZATION's handler asks it, with the context hash itself
    // instead of a name: the speech params (its ctor), +0x18D = 1 and +0x5C = 1
    // as the handler sets them, +0x130 = 0 (its third argument), then the
    // animal speech entity's vtable slot +0x288 (speech_identity.h
    // kAnimalVocal: writes the 32-bit context into params +0x150, +0x130 from
    // +0x18D, and enters the Say wrapper, which runs the animal pipeline and
    // the AI step that starts the line), and the params dtor. The native's
    // slot +0x290 hashes the name and jumps there. Checked (speech_identity.h
    // CheckAnimalVocalEntry): the vtable is the animal one, its slot is the
    // vocal entry found by its signature.
        constexpr std::size_t kAnimalVocalSlot = replay::speech::kAnimalVocalHashSlot;
        bool GuardedAnimalVocal(std::uintptr_t vocal, std::uintptr_t ctor, std::uintptr_t dtor, std::uint8_t* speech,
            std::uint8_t* params, std::uint32_t context, std::uint32_t voice, std::int32_t variation,
            std::uint32_t& result, std::uint32_t& code)
        {
            __try
            {
                if (!engine::Call(engine::kSpeechParamsCtor, ctor, params))
                    return engine::Failed(code);
                params[0x18D] = 1;
                *reinterpret_cast<std::uint32_t*>(params + 0x5C) = 1;
                *reinterpret_cast<std::uint32_t*>(params + 0x130) = 0;
                // The recorded voice and variation, kept by the animal
                // pipeline's choice (it keeps a non-zero +0x14C and a positive
                // +0x158), as the direct path sets them.
                if (voice)
                    *reinterpret_cast<std::uint32_t*>(params + kParamsVoice) = voice;
                if (variation > 0)
                {
                    *reinterpret_cast<std::int32_t*>(params + kParamsVariation) = variation;
                    params[0x192] = 0;
                    params[0x198] = 0;
                }
                if (!engine::Call(engine::kSpeechAnimalVocal, vocal, result, speech, context, params) ||
                    !engine::Call(engine::kSpeechParamsDtor, dtor, params))
                    return engine::Failed(code);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    bool PlayAnimalVocal(void* pedEntity, std::uint32_t contextPartialHash, std::uint32_t voiceHash,
        std::int32_t variation, std::uint32_t& result, std::string& error)
    {
        result = 0;
        const std::uintptr_t ctor = sig::Find(kSpeechParamsCtor), dtor = sig::Find(kSpeechParamsDtor),
                             vocal = sig::Find(replay::speech::kAnimalVocal);
        if (!ctor || !dtor || !vocal)
        {
            error = "signature_mismatch";
            return false;
        }
        const std::uint64_t speech = SpeechEntity(pedEntity);
        std::uint64_t vtable = 0;
        std::uint64_t slot = 0;
        if (!speech || !ReadU64(speech, vtable))
        {
            error = "no_speech_entity";
            return false;
        }
        // The decision is the pure check; a refusal names the RVA it found.
        const std::uint64_t module = sig::ImageBase();
        const std::uint64_t vtableRva = vtable >= module ? vtable - module : ~0ull;
        const std::uint64_t animalVtable = sig::Find(replay::speech::kAnimalSpeechVtable);
        if (animalVtable && vtable == animalVtable)
            ReadU64(vtable + kAnimalVocalSlot, slot);
        const replay::speech::AnimalVocalEntry entry =
            replay::speech::CheckAnimalVocalEntry(vtable, slot, animalVtable, vocal);
        if (entry != replay::speech::AnimalVocalEntry::Ok)
        {
            char text[64]{};
            std::snprintf(text, sizeof(text), "%s:vt_%llx",
                entry == replay::speech::AnimalVocalEntry::NotAnimalSpeech ? "not_animal_speech" : "slot_other",
                static_cast<unsigned long long>(vtableRva));
            error = text;
            return false;
        }
        alignas(16) std::uint8_t params[0x200] = {};
        std::uint32_t code = 0;
        if (!GuardedAnimalVocal(vocal, ctor, dtor, reinterpret_cast<std::uint8_t*>(speech), params, contextPartialHash, voiceHash,
                variation, result, code))
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        error = "result_" + std::to_string(result);
        return true;
    }
    namespace
    {

        bool FindPedLine(void* pedEntity, std::uint64_t line);

        bool GuardedReadPlayedLine(std::uint8_t* line, PlayedLine& played, std::uint32_t& code)
        {
            __try
            {
                ReadPlayedLine(line, played);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

    // A stopped line read at its address. The line vtable comes from the line
    // ctor's own bytes (its only caller the line allocator): lea rax, [rip +
    // disp] at +0x14, then mov [rcx], rax. Plain guarded reads, script thread
    // only.
        const sig::Signature kSpeechLineCtor{"audio_speech_line_ctor",
            "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 BE 02 00 00 00 48 8D 05 ?? ?? ?? ?? 48 89 01", 0x2F91F90};
        const sig::Signature kSpeechLineVtable{"audio_speech_line_vtable", kSpeechLineCtor, sig::Rel32{0x17, 0x1B},
            "", 0x37FD5D0};

        std::uint64_t SpeechLineVtable()
        {
            static const std::uint64_t vtable = sig::Find(kSpeechLineVtable);
            return vtable;
        }
    }

    bool ReadStoppedLine(std::uint64_t line, StoppedLineView& out)
    {
        out = StoppedLineView{};
        const std::uint64_t vtable = SpeechLineVtable();
        std::uint64_t first = 0;
        if (!line || !vtable || !ReadU64(line, first))
            return false;
        out.read = true;
        out.liveVtable = first == vtable;
        if (!out.liveVtable)
            return true;
        std::uint64_t data = 0;
        if (!ReadBytes(line + 0x234, &out.state, sizeof(out.state)) || !ReadU64(line + 0x1D8, data) || !data ||
            !ReadBytes(data + 0x110, &out.voice, 4) || !ReadBytes(data + 0x114, &out.context, 4) ||
            !ReadBytes(data + 0x118, &out.variation, 4))
        {
            out.read = false;
            return false;
        }
        return true;
    }

    // The identity of a line still in the ped's line list.
    bool ReadSpeechLineIdentity(void* pedEntity, std::uint64_t line, PlayedLine& out)
    {
        out = PlayedLine{};
        if (!line || !FindPedLine(pedEntity, line))
            return false;
        std::uint32_t code = 0;
        if (!GuardedReadPlayedLine(reinterpret_cast<std::uint8_t*>(line), out, code))
        {
            out = PlayedLine{};
            return false;
        }
        return out.identityRead;
    }
    namespace
    {

        // The line is one of this ped's lines (guarded list walk).
        bool FindPedLine(void* pedEntity, std::uint64_t line)
        {
            const std::uint64_t speech = SpeechEntity(pedEntity);
            std::uint64_t list = 0;
            std::uint16_t count = 0;
            if (!speech || !line || !ReadU64(speech + 0x78, list) || !list ||
                !ReadBytes(speech + 0x80, &count, sizeof(count)) || count > 64)
            {
                return false;
            }
            for (std::uint16_t slot = 0; slot < count; ++slot)
            {
                std::uint64_t entry = 0;
                if (ReadU64(list + slot * 8u, entry) && entry == line)
                    return true;
            }
            return false;
        }

        bool GuardedStopLine(std::uintptr_t stop, void* line, std::uint32_t reason, std::uint32_t& code)
        {
            return engine::Call(engine::kSpeechLineStop, stop, line, reason) || engine::Failed(code);
        }
    }

    bool SpeechLineState(void* pedEntity, std::uint64_t line, std::int32_t& state)
    {
        state = -1;
        return FindPedLine(pedEntity, line) && ReadBytes(line + 0x234, &state, sizeof(state));
    }

    bool StopSpeechLine(void* pedEntity, std::uint64_t line, std::uint32_t reason, std::string& error)
    {
        const std::uintptr_t stop = LineStopFunction();
        if (!stop)
        {
            error = "stop_signature_mismatch";
            return false;
        }
        std::int32_t state = -1;
        if (!SpeechLineState(pedEntity, line, state))
        {
            error = "line_gone";
            return false;
        }
        if (state == 6)
        {
            error = "already_stopped";
            return false;
        }
        std::uint32_t code = 0;
        if (!GuardedStopLine(stop, reinterpret_cast<void*>(line), reason, code))
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        error = "stopped_state_" + std::to_string(state);
        return true;
    }

    void SetPuppetSpeechEntities(const std::vector<std::uint64_t>& entities, const std::vector<std::uint32_t>& owners)
    {
        const std::uint32_t count = static_cast<std::uint32_t>((std::min)(entities.size(), kMaxPuppetSpeech));
        if (count < g_puppetSpeechCount.load(std::memory_order_relaxed))
            g_puppetSpeechCount.store(count, std::memory_order_release);
        for (std::uint32_t n = 0; n < count; ++n)
        {
            g_puppetSpeechOwner[n].store(n < owners.size() ? owners[n] : replay::puppetaudio::kNoOwner,
                std::memory_order_relaxed);
            g_puppetSpeech[n].store(entities[n], std::memory_order_release);
        }
        g_puppetSpeechCount.store(count, std::memory_order_release);
    }

    void SetPuppetOwnSpeechBlock(bool block) { g_puppetOwnSpeechBlock.store(block, std::memory_order_relaxed); }
    bool PuppetOwnSpeechBlock()
    {
        return g_puppetOwnSpeechBlock.load(std::memory_order_relaxed) &&
            g_ownSpeechRelease.load(std::memory_order_acquire) != 0;
    }
    const char* PuppetOwnSpeechError() { return g_ownSpeechError; }
    std::uint64_t PuppetOwnSpeechSeen() { return g_puppetOwnSpeechSeen.load(std::memory_order_relaxed); }
    std::uint64_t PuppetOwnSpeechRefused() { return g_puppetOwnSpeechRefused.load(std::memory_order_relaxed); }
    std::uint64_t WorldSpeechDuringPlayback() { return g_worldSpeechDuringPlayback.load(std::memory_order_relaxed); }
    std::uint64_t WorldSpeechQueuedDuringPlayback() { return g_worldSpeechQueuedDuringPlayback.load(std::memory_order_relaxed); }

    void CollectPuppetOwnSpeechSince(std::uint64_t& cursor, std::vector<PuppetOwnSpeech>& out)
    {
        CollectSince(g_ownSpeech, kOwnSpeechRing, g_nextOwnSpeech, cursor, out);
    }

    bool FindLineStop(std::uint64_t line, SpeechLineStop& out)
    {
        const std::uint64_t end = g_nextStop.load(std::memory_order_acquire);
        const std::uint64_t first = end > kStopRing ? end - kStopRing : 0;
        for (std::uint64_t index = end; index-- > first;)
        {
            SpeechLineStop value;
            if (replay::ring::TryRead(g_stops[index % kStopRing], index, value) == replay::ring::Read::Ready && value.line == line)
            {
                out = value;
                return true;
            }
        }
        return false;
    }

    void CollectSpeechStopsSince(std::uint64_t& cursor, std::vector<SpeechLineStop>& out)
    {
        CollectSince(g_stops, kStopRing, g_nextStop, cursor, out);
    }

    void CollectSpeechStartsSince(std::uint64_t& cursor, std::vector<SpeechLineStart>& out)
    {
        CollectSince(g_starts, kStartRing, g_nextStart, cursor, out);
    }

    bool SpeechStartObserved()
    {
        return g_audibleTarget != nullptr;
    }
}
