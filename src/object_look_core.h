#pragma once

// The look of an object that its model does not carry.
//
// Document text (letters, notes, telegrams, newspapers, posters, photos)
// is a texture dictionary the script puts on the object with
// SET_CUSTOM_TEXTURES_ON_OBJECT(object, txd, p2, p3) after
// REQUEST_STREAMED_TXD(txd). Build 1491.50:
//   native 0xE124889AE0521FCF handler 0x10C0D80 -> body 0x10CECE4: the
//   handle resolved as an object (0xEB39D8), then 0x840A5C(object, &txd,
//   &p2, &p3). The setter releases the old references and stores each hash
//   at object +0x420 (txd), +0x424 (p2), +0x428 (p3) only when the TXD
//   store has a slot for it (0xB41BDC), adding a reference (the store's
//   entry +8); hash 0 leaves 0.
//   SET_OBJECT_TINT_INDEX (0x971DA0055324D033) body 0x10D0650 calls
//   0xE5057C(entity, tint): `and [rcx+0xB8], 0xFFFFFE07; and edx, 0x3F;
//   shl edx, 3; or [rcx+0xB8], edx`: the tint is bits 3..8 of the dword at
//   entity +0xB8 (six readers do `shr 3; and 0x3F` on it), then a redraw.
// The look is stored as appearance state at the create plus changes as
// events. Neither field has a getter native, so the recorder reads them
// (guarded memory, object_look.cpp) and the playback sets them back with the
// natives. Pure (unit tested in ReplayCoreTests).

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace replay
{
    constexpr std::uintptr_t kObjectCustomTxdOffset = 0x420; // +0x424 p2, +0x428 p3
    constexpr std::uintptr_t kEntityTintWordOffset = 0xB8;
    constexpr std::uint32_t kEntityTintShift = 3;
    constexpr std::uint32_t kEntityTintMask = 0x3F;

    struct ObjectLook
    {
        std::uint32_t txd = 0; // object +0x420
        std::uint32_t p2 = 0;  // object +0x424
        std::uint32_t p3 = 0;  // object +0x428
        std::uint8_t tint = 0; // (entity +0xB8 >> 3) & 0x3F

        bool operator==(const ObjectLook& other) const
        {
            return txd == other.txd && p2 == other.p2 && p3 == other.p3 && tint == other.tint;
        }
        bool operator!=(const ObjectLook& other) const { return !(*this == other); }
    };

    // The look a freshly created object of any model has: no custom
    // texture, tint 0.
    inline bool IsDefaultObjectLook(const ObjectLook& look) { return look == ObjectLook{}; }

    inline std::uint8_t TintFromWord(std::uint32_t word)
    {
        return static_cast<std::uint8_t>((word >> kEntityTintShift) & kEntityTintMask);
    }

    // The textures differ (the tint aside): SET_CUSTOM_TEXTURES_ON_OBJECT is due.
    inline bool ObjectTexturesDiffer(const ObjectLook& a, const ObjectLook& b)
    {
        return a.txd != b.txd || a.p2 != b.p2 || a.p3 != b.p3;
    }

    // The look changes of one object (key: its packed id) or one scenery
    // entry (key: its index in Recording::scenery). Only an object that was
    // ever read with a look other than the default gets a track; absence
    // means the default look all along.
    struct ObjectLookTrack
    {
        std::uint32_t id = 0;
        std::vector<std::pair<std::uint32_t, ObjectLook>> states; // time ascending
    };

    // A read at timeMs. No track yet: a default look is not stored (most
    // objects never get one); a first other look makes the track, preceded
    // by the default at previousReadMs when the object had been read before
    // (readBefore: it had the default until then, which a stand-in shown
    // from the start must draw). A track: stored when it differs from the
    // last stored one, never back in time. Returns the track (null when
    // none) and whether it stored. Pure (tested).
    template <class Index>
    inline bool NoteObjectLook(std::vector<ObjectLookTrack>& tracks, Index& index, std::uint32_t key, std::uint32_t timeMs,
        bool readBefore, std::uint32_t previousReadMs, const ObjectLook& look, bool* created = nullptr)
    {
        if (created)
            *created = false;
        ObjectLookTrack* track = index.Find(tracks, key);
        if (!track)
        {
            if (IsDefaultObjectLook(look))
                return false;
            ObjectLookTrack fresh;
            fresh.id = key;
            if (readBefore && previousReadMs < timeMs)
                fresh.states.push_back({previousReadMs, ObjectLook{}});
            fresh.states.push_back({timeMs, look});
            tracks.push_back(std::move(fresh));
            if (created)
                *created = true;
            return true;
        }
        if (!track->states.empty() && (track->states.back().second == look || timeMs < track->states.back().first))
            return false;
        track->states.push_back({timeMs, look});
        return true;
    }

    // The look at timeMs: the last state at or before it. Before the first
    // state: the default for an object puppet (it was read with the default
    // until then, or it did not exist), the first state with holdFirst (a
    // stand-in shown before the recording read its object).
    inline ObjectLook ObjectLookAt(const ObjectLookTrack& track, std::uint32_t timeMs, bool holdFirst)
    {
        ObjectLook look;
        if (track.states.empty())
            return look;
        if (holdFirst)
            look = track.states.front().second;
        for (const auto& state : track.states)
        {
            if (state.first > timeMs)
                break;
            look = state.second;
        }
        return look;
    }

    // The TXD hashes a look needs streamed (non-zero, distinct), at most 3.
    inline std::size_t ObjectLookTxds(const ObjectLook& look, std::uint32_t out[3])
    {
        std::size_t count = 0;
        for (std::uint32_t hash : {look.txd, look.p2, look.p3})
        {
            if (!hash)
                continue;
            bool seen = false;
            for (std::size_t n = 0; n < count; ++n)
                seen = seen || out[n] == hash;
            if (!seen)
                out[count++] = hash;
        }
        return count;
    }

    // A loaded value never reaches the engine out of range.
    inline void SanitizeObjectLook(ObjectLook& look) { look.tint = static_cast<std::uint8_t>(look.tint & kEntityTintMask); }

    // object_look_mismatch (the detector). After the playback set a look on an
    // object (and on a QA mark), the object is read again 0.5 s and 2 s later;
    // a read that is not the look the clip wants at the playhead is the verdict.
    // A read that failed is no verdict (unknown).
    enum class LookCheck : std::uint8_t
    {
        Match = 0,
        TextureMismatch = 1,
        TintMismatch = 2,
        BothMismatch = 3,
    };
    inline LookCheck CheckObjectLook(const ObjectLook& wanted, const ObjectLook& read)
    {
        const bool textures = ObjectTexturesDiffer(wanted, read);
        const bool tint = wanted.tint != read.tint;
        return textures && tint ? LookCheck::BothMismatch
            : textures          ? LookCheck::TextureMismatch
            : tint              ? LookCheck::TintMismatch
                                : LookCheck::Match;
    }
    inline const char* LookCheckText(LookCheck check)
    {
        switch (check)
        {
        case LookCheck::Match: return "match";
        case LookCheck::TextureMismatch: return "texture";
        case LookCheck::TintMismatch: return "tint";
        case LookCheck::BothMismatch: return "texture_tint";
        default: return "?";
        }
    }

    // The re-read schedule after an apply (wall clock, ms).
    constexpr std::uint64_t kLookCheckFirstMs = 500;
    constexpr std::uint64_t kLookCheckSecondMs = 2000;
    // A TXD that does not load within this is given up for the playback.
    constexpr std::uint64_t kLookTxdTimeoutMs = 5000;
    // How many times one look is set again on one object the engine keeps
    // changing back (then only the detector reports it).
    constexpr std::uint32_t kLookMaxApplies = 3;
    // Recording: one read per object at most this often.
    constexpr std::uint32_t kLookReadMs = 500;
}
