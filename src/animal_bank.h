#pragma once

// The recorded voice's wave bank of an animal, loaded and held ahead of its
// lines.
//
// The speech manager (0x5BB9750, offline 1491.50) keeps one loaded bank per
// animal type: 108 records of 0x48 bytes from +0x890, the record of type T
// at +0x890 + T * 0x48 with its bank reference at +0x898 (the holder; +0x8A8
// the reference's object, whose +0x20 is the bank slot entry), the type's
// demand (float, +0x8B0), the next bank number to load (+0x8B4) and the
// number of banks of the type (+0x8B4's modulus, +0x8B8). The manager's
// update 0x2FC5E54 ends in 0x2FC65E0: with no load pending (+0x860/+0x870)
// it asks 0x2F985B0 which type to load, finds a slot (0x2FA5C48: the horse
// type 0x3B has its own slots, +0x850/+0x858; the others share the large
// bank class +0x840/+0x848), builds the bank name "<TYPE>_NEAR_%.2u" from
// the type's name (table at .data 0x3E276A0) and that number (0x2F9BD4C),
// loads it (0x2FB1730: slot vtable +0x28, then the slot entry's +0x30 = the
// joaat of the name, +0x38 = 0) and moves the number on ((n % count) + 1).
// When the load completes, the type's holder takes the new slot.
//
// The voice of an animal line IS that name's hash: 0x6D4D0CB7, 0xC66C3EF8
// and 0x94C95BAF (horse voices) are joaat("HORSE_NEAR_01" ... "_03"). A line
// asked with a voice (the native "pinned" path) gets the type's slot (voice
// source 9, 0x2FBCE38 -> 0x2FBD004) and finds its wave only when that slot
// holds the voice's bank; otherwise the sound sets +0xEE bit 2 and the line
// dies loading (reason 4). A line asked by context gets the voice of the
// loaded bank (0x2F9DB84 -> 0x2FA6674 reads the entry's +0x30 when its state
// is 5). The manager rotates the bank when a context's variations were all
// played from it (+0x38, 0x2FCEF8C) or when the type is not loaded, so a
// replay's manager left alone plays recorded horses with the wrong voices.
//
// The clip already says which bank each animal line needs (its voice), so the
// replay plans, per animal type, the bank of the next line due within the lead
// and has the manager load and keep that one: a detour on the type picker
// 0x2F985B0 (its one caller is 0x2FC6722) returns the held type with the bank
// number set when the type does not hold the voice, and returns no type (0:
// the loader skips, 0x2FC672C) instead of rotating a type that holds it.
// Nothing else of the manager is written.
// This header is pure (unit tested in scenery_speech_tests.cpp).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace replay::animalbank
{
    // ---- the speech manager's animal records (offline 1491.50) ----------

    constexpr int kTypeCount = 0x6C;      // 0x2F98697 (cmp ebp, 0x6C) and 0x2F9BD6D
    constexpr int kFirstLoadable = 2;     // 0x2FC6729: lea ecx, [rsi - 2]; cmp ecx, 0x69
    constexpr int kLastLoadable = 0x6B;
    constexpr int kHorseType = 0x3B;      // 0x2F98643 and 0x2FA5C6E (its own slots)
    constexpr std::uint32_t kRecordStride = 0x48;
    constexpr std::uint32_t kHolder = 0x898;      // + T * 0x48: the bank reference
    constexpr std::uint32_t kHolderObject = 0x8A8; // holder +0x10: 0 = no bank
    constexpr std::uint32_t kDemand = 0x8B0;      // float
    constexpr std::uint32_t kNextBank = 0x8B4;    // u32, 1 .. count
    constexpr std::uint32_t kBankCount = 0x8B8;   // u32
    constexpr std::uint32_t kObjectEntry = 0x20;  // reference object -> bank slot entry (0x2C39B4C)
    constexpr std::uint32_t kEntryState = 0x20;   // 5: loaded (0x2FA66AD)
    constexpr std::uint32_t kEntryVoice = 0x30;   // the bank name's hash (0x2FB174D, read at 0x2FA66C0)
    constexpr std::uint32_t kEntryExhausted = 0x38;
    constexpr std::uint32_t kEntryLoaded = 5;
    // The sorted demand list (0x2FC1E70 writes {type, demand} for every type
    // and sorts it; the release pass 0x2FC65FA keeps the bank of a type in
    // its first u16 [+0x848] entries, 8 bytes each).
    constexpr std::uint32_t kKeepList = 0x26D8;
    constexpr std::uint32_t kKeepCount = 0x848;
    constexpr std::uint32_t kMaxBanks = 99; // "%.2u"

    inline std::uint32_t RecordOffset(int type, std::uint32_t field)
    {
        return field + static_cast<std::uint32_t>(type) * kRecordStride;
    }

    inline bool Loadable(int type) { return type >= kFirstLoadable && type <= kLastLoadable; }

    // joaat of the text, lower-cased (the engine's 0x2609C84).
    inline std::uint32_t NameHash(const char* text)
    {
        std::uint32_t hash = 0;
        for (; text && *text; ++text)
        {
            char c = *text;
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
            hash += static_cast<std::uint8_t>(c);
            hash += hash << 10;
            hash ^= hash >> 6;
        }
        hash += hash << 3;
        hash ^= hash >> 11;
        hash += hash << 15;
        return hash;
    }

    // The voice of bank `bank` of a type: the hash of the name 0x2F9BD4C
    // builds ("%s_NEAR_%.2u", the format at .rdata 0x37FEC88).
    inline std::uint32_t BankVoice(const char* typeName, std::uint32_t bank)
    {
        if (!typeName || !*typeName || bank == 0 || bank > kMaxBanks)
            return 0;
        char text[64]{};
        std::snprintf(text, sizeof(text), "%s_NEAR_%.2u", typeName, static_cast<unsigned>(bank));
        return NameHash(text);
    }

    // A type's name is upper-case letters, digits and underscores (what the
    // replay accepts from the engine's table; anything else is not a name).
    inline bool PlausibleName(const char* text, std::size_t size)
    {
        if (!text || size == 0 || !text[0])
            return false;
        for (std::size_t n = 0; n < size; ++n)
        {
            const char c = text[n];
            if (!c)
                return n > 0;
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
                return false;
        }
        return false; // no terminator within size
    }

    struct TypeBanks
    {
        int type = 0;
        char name[32] = {};
        std::uint32_t count = 0;
    };

    struct BankOf
    {
        int type = 0;
        std::uint32_t bank = 0; // 1 .. count
    };

    // voice -> (type, bank), from the types read off the engine.
    using VoiceMap = std::unordered_map<std::uint32_t, BankOf>;

    inline VoiceMap BuildVoiceMap(const std::vector<TypeBanks>& types)
    {
        VoiceMap map;
        for (const TypeBanks& type : types)
        {
            if (!Loadable(type.type) || type.count == 0 || type.count > kMaxBanks)
                continue;
            for (std::uint32_t bank = 1; bank <= type.count; ++bank)
            {
                const std::uint32_t voice = BankVoice(type.name, bank);
                if (voice)
                    map.emplace(voice, BankOf{type.type, bank});
            }
        }
        return map;
    }

    // ---- the plan --------------------------------------------------------

    // An animal line of the clip whose voice is an animal bank.
    struct Due
    {
        std::uint32_t timeMs = 0;
        std::uint32_t voice = 0;
        int type = 0;
        std::uint32_t bank = 0;
    };

    struct Hold
    {
        int type = 0;
        std::uint32_t voice = 0;
        std::uint32_t bank = 0;
        std::uint32_t firstMs = 0; // the line that asked for it
        bool operator==(const Hold& other) const
        {
            return type == other.type && voice == other.voice && bank == other.bank;
        }
    };

    constexpr std::size_t kMaxHolds = 8;

    inline void SortDue(std::vector<Due>& due)
    {
        std::stable_sort(due.begin(), due.end(), [](const Due& a, const Due& b) { return a.timeMs < b.timeMs; });
    }

    // Per type, the bank of its next line from the playhead on (within
    // leadMs): the manager holds one bank per type, so the bank of the line
    // about to play wins and the next bank is asked as soon as the last line
    // of the current one has been asked. `due` sorted by time.
    inline std::vector<Hold> PlanHolds(const std::vector<Due>& due, std::uint32_t timeMs, std::uint32_t leadMs,
        std::size_t maxHolds = kMaxHolds)
    {
        std::vector<Hold> holds;
        const auto first = std::lower_bound(due.begin(), due.end(), timeMs,
            [](const Due& line, std::uint32_t at) { return line.timeMs < at; });
        const std::uint64_t until = static_cast<std::uint64_t>(timeMs) + leadMs;
        for (auto it = first; it != due.end() && it->timeMs <= until && holds.size() < maxHolds; ++it)
        {
            const bool seen = std::any_of(holds.begin(), holds.end(), [&](const Hold& hold) { return hold.type == it->type; });
            if (seen || !Loadable(it->type) || !it->voice || !it->bank)
                continue;
            holds.push_back(Hold{it->type, it->voice, it->bank, it->timeMs});
        }
        return holds;
    }

    // ---- the picker detour ----------------------------------------------

    // A published hold as the detour sees the type's record in the manager.
    struct HoldView
    {
        Hold hold;
        std::uint32_t loadedVoice = 0; // the bank the type's holder has (0: none)
        std::uint32_t count = 0;       // banks of the type
        // The manager keeps the type's bank: the horse type always (its slots
        // are outside the release pass), another type only with demand above
        // 0 and in the kept part of the demand list (0x2FC65FA); otherwise a
        // forced load would be released on the next update.
        bool kept = false;
    };

    enum class PickKind : std::uint8_t
    {
        Pass,  // the engine's own choice
        Force, // the held type, with its next bank number set to the hold's
        Keep,  // the engine chose a held type that holds its voice: no load
    };

    struct PickDecision
    {
        PickKind kind = PickKind::Pass;
        int type = 0;          // the value the picker returns
        std::uint32_t bank = 0; // Force: written to the type's next bank number
    };

    inline PickDecision DecidePick(int picked, const HoldView* views, std::size_t count)
    {
        PickDecision decision;
        decision.type = picked;
        for (std::size_t n = 0; n < count; ++n)
        {
            const HoldView& view = views[n];
            if (!Loadable(view.hold.type) || !view.hold.voice || view.loadedVoice == view.hold.voice || !view.kept ||
                view.hold.bank == 0 || view.hold.bank > view.count)
            {
                continue;
            }
            decision.kind = PickKind::Force;
            decision.type = view.hold.type;
            decision.bank = view.hold.bank;
            return decision;
        }
        for (std::size_t n = 0; n < count; ++n)
        {
            if (views[n].hold.type == picked && views[n].hold.voice && views[n].loadedVoice == views[n].hold.voice)
            {
                decision.kind = PickKind::Keep;
                decision.type = 0;
                return decision;
            }
        }
        return decision;
    }

    // Published holds packed for the detour: type, bank, voice.
    inline std::uint64_t Pack(const Hold& hold)
    {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(hold.type) & 0xFFu) << 48) |
            (static_cast<std::uint64_t>(hold.bank & 0xFFu) << 32) | hold.voice;
    }

    inline Hold Unpack(std::uint64_t packed)
    {
        Hold hold;
        hold.type = static_cast<int>((packed >> 48) & 0xFFu);
        hold.bank = static_cast<std::uint32_t>((packed >> 32) & 0xFFu);
        hold.voice = static_cast<std::uint32_t>(packed & 0xFFFFFFFFu);
        return hold;
    }

    // ---- the script thread's follow of each hold -------------------------

    // A hold is given up when the type has not loaded the bank this long
    // after it was asked (no free slot, the type not kept): its lines go by
    // context and the manager's own choice is left alone. Asked again when
    // the plan asks another bank of that type.
    constexpr std::uint64_t kGiveUpMs = 6000;
    // A given-up hold still in the plan is asked again this long after it
    // gave up: one failed load (no free slot while the playback was getting
    // ready) would leave the bank unheld for the whole stretch of its lines.
    // The retry keeps the picker taken at most kGiveUpMs of every kGiveUpMs +
    // this.
    constexpr std::uint64_t kRetryAfterGiveUpMs = 10000;

    struct HoldState
    {
        Hold hold;
        std::uint64_t askedMs = 0;
        std::uint64_t loadedMs = 0; // 0 while not loaded
        std::uint64_t gaveUpMs = 0;
        bool gaveUp = false;
    };

    // What changed in one follow step (for the log and the counters).
    struct FollowEvents
    {
        std::vector<Hold> asked;
        std::vector<HoldState> loaded;   // first time the voice was seen loaded
        std::vector<HoldState> gaveUp;
        std::vector<HoldState> released; // left the plan
        std::vector<Hold> retried;       // given up, still planned, asked again
    };

    // `loadedVoiceOf(type)` reads the type's loaded voice. States for holds
    // that left the plan are released; new ones are asked.
    template <typename LoadedVoiceOf>
    FollowEvents Follow(std::vector<HoldState>& states, const std::vector<Hold>& planned, std::uint64_t nowMs,
        LoadedVoiceOf loadedVoiceOf)
    {
        FollowEvents events;
        for (std::size_t n = states.size(); n-- > 0;)
        {
            const bool still = std::any_of(planned.begin(), planned.end(), [&](const Hold& hold) { return hold == states[n].hold; });
            if (!still)
            {
                events.released.push_back(states[n]);
                states.erase(states.begin() + static_cast<std::ptrdiff_t>(n));
            }
        }
        for (const Hold& hold : planned)
        {
            const bool known = std::any_of(states.begin(), states.end(), [&](const HoldState& state) { return state.hold == hold; });
            if (known)
                continue;
            HoldState state;
            state.hold = hold;
            state.askedMs = nowMs;
            states.push_back(state);
            events.asked.push_back(hold);
        }
        for (HoldState& state : states)
        {
            if (state.gaveUp)
            {
                if (nowMs < state.gaveUpMs + kRetryAfterGiveUpMs)
                    continue;
                state.gaveUp = false;
                state.askedMs = nowMs;
                events.retried.push_back(state.hold);
            }
            const bool loaded = loadedVoiceOf(state.hold.type) == state.hold.voice;
            if (loaded && !state.loadedMs)
            {
                state.loadedMs = nowMs ? nowMs : 1;
                events.loaded.push_back(state);
            }
            else if (!loaded && !state.loadedMs && nowMs >= state.askedMs + kGiveUpMs)
            {
                state.gaveUp = true;
                state.gaveUpMs = nowMs;
                events.gaveUp.push_back(state);
            }
        }
        return events;
    }

    // The holds the detour acts on: those not given up.
    inline std::vector<Hold> Active(const std::vector<HoldState>& states)
    {
        std::vector<Hold> active;
        for (const HoldState& state : states)
            if (!state.gaveUp && active.size() < kMaxHolds)
                active.push_back(state.hold);
        return active;
    }

    // ---- the pinned request ----------------------------------------------

    // Before an animal line is asked with its recorded voice: the voice's
    // bank is the type's loaded one (the line would find its wave), it is
    // not (the line would die loading: ask by context instead), or the
    // voice is no animal bank the replay knows (ask as before).
    enum class PinnedBank : std::uint8_t
    {
        Unknown,
        Ready,
        Missing,
    };

    inline PinnedBank JudgePinned(bool known, std::uint32_t voice, std::uint32_t loadedVoice)
    {
        if (!known || !voice)
            return PinnedBank::Unknown;
        return loadedVoice == voice ? PinnedBank::Ready : PinnedBank::Missing;
    }

    inline const char* PinnedBankName(PinnedBank bank)
    {
        switch (bank)
        {
        case PinnedBank::Ready: return "ready";
        case PinnedBank::Missing: return "missing";
        default: return "unknown";
        }
    }
}
