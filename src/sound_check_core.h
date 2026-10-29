#pragma once

// The always-on sound check and the census-level sound capture, pure part
// (unit tested in ReplayCoreTests).
//
// The census (sound_census.h) sees every top-level sound the engine creates.
// Cheap mode, on in every recording and playback with SoundCheck (Debug=1):
// - a recording counts the sounds per (owner recorded id, metadata ref,
//   caller path) per kWindowMs window and keeps that table in the clip
//   (section SCEN, Summary below), no log line per sound;
// - a playback counts the same way (owners: the puppets, by their recorded
//   id) and at its end compares the two (Diff): what the replay lost, what
//   it made less of, what it added and what the engine refused, grouped by
//   owner class and caller path, with the clip times
//   (ReplayDebug\<clip>.sound-check.txt, SOUND_MISSING_SUMMARY).
// The capture (sound_capture.h) records, from the same rows, the sounds of
// recorded vehicles, train cars and objects that no other track covers
// (GeneralSound, section GSND) and plays them on the owner's puppet unless
// the puppet makes the same sound itself (OwnCreates, the ped audio rule).

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace replay::census
{
    constexpr std::uint32_t kWindowMs = 250;
    constexpr std::size_t kParamsBytes = 0x140; // audSoundInitParams bytes copied (census rows, captured sounds)
    constexpr std::uint32_t kNoOwner = 0xFFFF0000u; // the packed id of "no recorded entity"
    // Caller path kept per sound: the return addresses above the creator
    // (frame 0 of the census row is the census detour itself).
    constexpr std::uint32_t kPathDepth = 5;

    // ------------------------------------------------------------ classes

    enum class OwnerClass : std::uint8_t
    {
        World = 0,      // no recorded owner (ambient, birds, wind, the UI)
        Player = 1,     // the player (live in the recording, its puppet in the replay)
        Ped = 2,
        Animal = 3,
        Horse = 4,
        Vehicle = 5,
        Train = 6,
        Object = 7,     // props
        Weapon = 8,     // weapon objects
        LivePlayer = 9, // the hidden live player during a playback
    };
    constexpr std::uint8_t kOwnerClassCount = 10;

    inline const char* ClassToken(OwnerClass cls)
    {
        switch (cls)
        {
        case OwnerClass::World: return "world";
        case OwnerClass::Player: return "player";
        case OwnerClass::Ped: return "ped";
        case OwnerClass::Animal: return "animal";
        case OwnerClass::Horse: return "horse";
        case OwnerClass::Vehicle: return "vehicle";
        case OwnerClass::Train: return "train";
        case OwnerClass::Object: return "object";
        case OwnerClass::Weapon: return "weapon";
        case OwnerClass::LivePlayer: return "live_player";
        }
        return "?";
    }

    // Report headings (English, the QA package).
    inline const char* ClassTitle(OwnerClass cls)
    {
        switch (cls)
        {
        case OwnerClass::World: return "world (no owner)";
        case OwnerClass::Player: return "player";
        case OwnerClass::Ped: return "people";
        case OwnerClass::Animal: return "animals";
        case OwnerClass::Horse: return "horses";
        case OwnerClass::Vehicle: return "vehicles";
        case OwnerClass::Train: return "train";
        case OwnerClass::Object: return "objects";
        case OwnerClass::Weapon: return "weapons";
        case OwnerClass::LivePlayer: return "hidden player";
        }
        return "?";
    }

    // Report order: the most noticed losses first.
    inline int ClassOrder(OwnerClass cls)
    {
        static constexpr OwnerClass kOrder[] = {OwnerClass::Train, OwnerClass::Vehicle, OwnerClass::Horse,
            OwnerClass::Animal, OwnerClass::Player, OwnerClass::Ped, OwnerClass::Weapon, OwnerClass::Object,
            OwnerClass::LivePlayer, OwnerClass::World};
        for (int n = 0; n < static_cast<int>(sizeof(kOrder) / sizeof(kOrder[0])); ++n)
            if (kOrder[n] == cls)
                return n;
        return 99;
    }

    // The ignore-list class of a key.
    inline const char* IgnoreClass(OwnerClass cls)
    {
        switch (cls)
        {
        case OwnerClass::World: return "none";
        case OwnerClass::Player: return "player";
        case OwnerClass::LivePlayer: return "live_player";
        default: return "entity";
        }
    }

    // Where a sound's audio entity is, from its owner (record and play).
    enum class AudioBase : std::uint8_t
    {
        Entity = 0,       // inside the entity (objects; + offset)
        PedHolder = 1,    // inside the ped's audio holder (kPedAudioSpan)
        WeaponAudio = 2,  // the weapon object's CWeapon and its audio at +0x100 (+0x200 span)
        VehicleAudio = 3, // the vehicle's audio entity, [vehicle + 0x900]
        Image = 4,        // a static audio entity of the image (offset = RVA)
        Field = 5,        // [entity + field] + offset (learned by FindFieldBase)
    };
    constexpr std::uint8_t kAudioBaseCount = 6;

    // The static audio entities of the image (game build 1491.50) a recording
    // names by RVA, with the vtable each one has. Evidence: the (RVA, vtable)
    // pairs the recorder stored with every Image based generic sound (SGEN)
    // of the 116 QA clips, and for 0x5BFB5B0 (two SNDS impacts, no SGEN row)
    // its static constructor 0x31015E4 in the live dump (base ctor 0x3102228,
    // then the vtable 0x3831F88 at +0; the same read of 0x30FFD20 gives the
    // recorded 0x3832238 of 0x5C10540). The clip sections without a vtable
    // (SNDS globalAudio, ESND audioRva) are played only on one of these, and
    // only while the live object there still has that vtable
    // (audio::TrustedImageAudio): a clip cannot make any other image address
    // an audio object.
    struct ImageAudio
    {
        std::uint32_t rva;
        std::uint32_t vtableRva;
    };
    inline constexpr ImageAudio kImageAudio[] = {
        {0x5B9F960u, 0x3833708u}, {0x5BFB5B0u, 0x3831F88u}, {0x5C0F880u, 0x3834948u}, // 0x5C0F880: frontend
        {0x5C10540u, 0x3832238u}, // the static collision audio
        {0x5C500B0u, 0x3832328u}, {0x5C505C0u, 0x3832418u}, {0x5C77390u, 0x3833618u}, {0x5C79E80u, 0x38326C8u},
        {0x5C7CE90u, 0x3832148u}, {0x5C81C20u, 0x383D4A8u}, // 0x5C81C20: script audio
        {0x5C90070u, 0x383D3B0u}, {0x5C9A660u, 0x383ECF0u}, {0x5CA7BB0u, 0x383F010u}, // 0x5CA7BB0: ambient
    };
    // The vtable RVA the static audio entity at `rva` must have; false for an
    // RVA that is not one of them.
    inline bool KnownImageAudio(std::uint64_t rva, std::uint32_t& vtableRva)
    {
        for (const ImageAudio& known : kImageAudio)
            if (known.rva == rva)
            {
                vtableRva = known.vtableRva;
                return true;
            }
        vtableRva = 0;
        return false;
    }

    inline const char* BaseToken(AudioBase base)
    {
        switch (base)
        {
        case AudioBase::Entity: return "entity";
        case AudioBase::PedHolder: return "ped_holder";
        case AudioBase::WeaponAudio: return "weapon_audio";
        case AudioBase::VehicleAudio: return "vehicle_audio";
        case AudioBase::Image: return "image";
        case AudioBase::Field: return "field";
        }
        return "?";
    }

    // A vehicle's audio entity is the pointer at vehicle+0x900
    // (build 1491.50: 222 sites read it and call into the vehicle audio
    // code 0x310xxxx..0x313xxxx; the train whistle 0x154D500 reads the
    // engine car's [+0x900] and calls 0x313F42C on it, which writes +0x1318,
    // so the object is longer than 0x1318 bytes).
    constexpr std::uint32_t kVehicleAudioField = 0x900;
    constexpr std::uint32_t kVehicleAudioSpan = 0x1800;
    constexpr std::uint32_t kFieldScanBytes = 0x1400; // entity bytes FindFieldBase reads
    constexpr std::uint32_t kFieldSpan = 0x2000;      // audio within this of the pointer

    // ------------------------------------------------------------ summary

    struct SummaryKey
    {
        std::uint32_t owner = kNoOwner;
        OwnerClass cls = OwnerClass::World;
        std::uint64_t ref = 0;
        std::uint32_t path[kPathDepth] = {};

        bool operator==(const SummaryKey& other) const
        {
            return owner == other.owner && cls == other.cls && ref == other.ref &&
                std::memcmp(path, other.path, sizeof(path)) == 0;
        }
    };

    inline std::uint64_t KeyHash(const SummaryKey& key)
    {
        std::uint64_t h = 1469598103934665603ull;
        const auto mix = [&h](std::uint64_t v) {
            h ^= v;
            h *= 1099511628211ull;
        };
        mix(key.owner);
        mix(static_cast<std::uint64_t>(key.cls));
        mix(key.ref);
        for (const std::uint32_t p : key.path)
            mix(p);
        return h;
    }

    struct SummaryCell
    {
        std::uint32_t window = 0; // clip ms / kWindowMs
        std::uint16_t key = 0;
        std::uint16_t count = 0;
        std::uint8_t refused = 0;
    };

    constexpr std::size_t kMaxKeys = 0xFFFF;

    // Unique per change of any summary (an accumulator never mistakes a
    // moved-in summary for the one it indexed).
    inline std::uint32_t NextGeneration()
    {
        static std::atomic<std::uint32_t> counter{0}; // the clip reader runs on a thread
        return counter.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    // A recording's census (clip section SCEN): the keys, the counts per
    // window and the windows the recording clock ran through.
    struct Summary
    {
        std::vector<SummaryKey> keys;
        std::vector<SummaryCell> cells;   // window ascending in a recording
        std::vector<std::uint32_t> covered; // windows, ascending, unique
        std::uint32_t generation = NextGeneration(); // changed when keys or cells move (not saved)
        std::uint64_t overflow = 0;       // rows not counted (key table full)

        void Clear()
        {
            keys.clear();
            cells.clear();
            covered.clear();
            overflow = 0;
            generation = NextGeneration();
        }
        bool Empty() const { return keys.empty() && cells.empty() && covered.empty(); }
        std::size_t Bytes() const
        {
            return keys.capacity() * sizeof(SummaryKey) + cells.capacity() * sizeof(SummaryCell) +
                covered.capacity() * sizeof(std::uint32_t);
        }

        // Keeps only the keys some cell uses (the buffer drops old windows).
        void Compact()
        {
            std::vector<std::int32_t> remap(keys.size(), -1);
            std::vector<SummaryKey> kept;
            for (SummaryCell& cell : cells)
            {
                if (cell.key >= remap.size())
                    continue;
                if (remap[cell.key] < 0)
                {
                    remap[cell.key] = static_cast<std::int32_t>(kept.size());
                    kept.push_back(keys[cell.key]);
                }
                cell.key = static_cast<std::uint16_t>(remap[cell.key]);
            }
            keys = std::move(kept);
            generation = NextGeneration();
        }

        // The ring buffer drops what is older than keepFromMs.
        void DropBefore(std::uint32_t keepFromMs)
        {
            const std::uint32_t first = keepFromMs / kWindowMs;
            const auto cellEnd = std::find_if(cells.begin(), cells.end(),
                [first](const SummaryCell& cell) { return cell.window >= first; });
            const auto coverEnd = std::lower_bound(covered.begin(), covered.end(), first);
            if (cellEnd == cells.begin() && coverEnd == covered.begin())
                return;
            cells.erase(cells.begin(), cellEnd);
            covered.erase(covered.begin(), coverEnd);
            if (keys.size() > 1024)
                Compact();
            else
                generation = NextGeneration();
        }

        // The window [baseMs, endMs] of a recording as a clip of its own
        // (recording_window.cpp): window times move by baseMs (by the
        // window's middle, so a cut inside a window keeps it once).
        void CutInto(Summary& out, std::uint32_t baseMs, std::uint32_t endMs) const
        {
            out.Clear();
            const auto shift = [baseMs](std::uint32_t window, std::uint32_t& to) {
                const std::uint64_t middle = static_cast<std::uint64_t>(window) * kWindowMs + kWindowMs / 2;
                if (middle < baseMs)
                    return false;
                to = static_cast<std::uint32_t>((middle - baseMs) / kWindowMs);
                return true;
            };
            const auto inside = [endMs](std::uint32_t window) {
                return static_cast<std::uint64_t>(window) * kWindowMs <= endMs;
            };
            std::vector<std::int32_t> remap(keys.size(), -1);
            // The cells of the window being written, by key.
            std::unordered_map<std::uint16_t, std::size_t> open;
            std::uint32_t openWindow = UINT32_MAX;
            const auto folding = [&](const SummaryCell& copy) {
                if (copy.window != openWindow)
                {
                    open.clear();
                    openWindow = copy.window;
                }
                const auto found = open.find(copy.key);
                if (found == open.end())
                {
                    open[copy.key] = out.cells.size();
                    return false;
                }
                SummaryCell& same = out.cells[found->second];
                same.count = static_cast<std::uint16_t>((std::min)(0xFFFF, same.count + copy.count));
                same.refused = static_cast<std::uint8_t>((std::min)(0xFF, same.refused + copy.refused));
                return true;
            };
            for (const SummaryCell& cell : cells)
            {
                std::uint32_t window = 0;
                if (!inside(cell.window) || !shift(cell.window, window) || cell.key >= keys.size())
                    continue;
                if (remap[cell.key] < 0)
                {
                    remap[cell.key] = static_cast<std::int32_t>(out.keys.size());
                    out.keys.push_back(keys[cell.key]);
                }
                SummaryCell copy = cell;
                copy.window = window;
                copy.key = static_cast<std::uint16_t>(remap[cell.key]);
                // Two source windows folding onto one (never with whole
                // windows, kept for a cut that is not a multiple).
                if (folding(copy))
                    continue;
                out.cells.push_back(copy);
            }
            for (const std::uint32_t window : covered)
            {
                std::uint32_t moved = 0;
                if (inside(window) && shift(window, moved) && (out.covered.empty() || out.covered.back() < moved))
                    out.covered.push_back(moved);
            }
            out.overflow = overflow;
        }
    };

    // Counts rows into a summary (script thread). A recording's rows come
    // in clip time order (the clock between two ticks), so only the open
    // window's cells are indexed; a playback jumps (scrub, loop), so all are,
    // and every pass of normal play over a window is counted (Passes).
    class Accumulator
    {
    public:
        void Attach(Summary* summary, bool anyOrder)
        {
            m_summary = summary;
            m_anyOrder = anyOrder;
            m_generation = summary ? summary->generation - 1 : 0;
            m_passes.clear();
            m_lastCovered = UINT32_MAX;
            Sync();
        }
        Summary* Target() const { return m_summary; }

        void Add(double clipMs, const SummaryKey& key, bool refused)
        {
            if (!m_summary || clipMs < 0.0)
                return;
            Sync();
            const std::uint32_t window = static_cast<std::uint32_t>(clipMs) / kWindowMs;
            std::uint16_t index = 0;
            if (!KeyIndex(key, index))
            {
                ++m_summary->overflow;
                return;
            }
            std::vector<SummaryCell>& cells = m_summary->cells;
            const std::uint64_t cellKey = (static_cast<std::uint64_t>(window) << 16) | index;
            if (!m_anyOrder && window != m_openWindow)
            {
                m_open.clear();
                m_openWindow = window;
            }
            const auto found = m_open.find(cellKey);
            std::size_t at = SIZE_MAX;
            if (found != m_open.end() && found->second < cells.size())
                at = found->second;
            else if (!m_anyOrder && !cells.empty() && cells.back().window > window)
            {
                // A row older than the open window (rare): the last cells.
                for (std::size_t n = cells.size(), seen = 0; n-- > 0 && seen < 512; ++seen)
                {
                    if (cells[n].window == window && cells[n].key == index)
                    {
                        at = n;
                        break;
                    }
                    if (cells[n].window < window)
                        break;
                }
            }
            if (at == SIZE_MAX)
            {
                SummaryCell cell;
                cell.window = window;
                cell.key = index;
                if (!m_anyOrder && !cells.empty() && cells.back().window > window)
                {
                    // Keep the recording's cells window ascending.
                    auto position = std::upper_bound(cells.begin(), cells.end(), window,
                        [](std::uint32_t w, const SummaryCell& c) { return w < c.window; });
                    at = static_cast<std::size_t>(position - cells.begin());
                    cells.insert(position, cell);
                    m_open.clear();
                    m_openWindow = UINT32_MAX;
                }
                else
                {
                    at = cells.size();
                    cells.push_back(cell);
                    m_open[cellKey] = at;
                }
            }
            SummaryCell& cell = cells[at];
            if (cell.count < 0xFFFF)
                ++cell.count;
            if (refused && cell.refused < 0xFF)
                ++cell.refused;
        }

        // The clip clock ran (normal time) from fromMs to toMs.
        void Cover(double fromMs, double toMs)
        {
            if (!m_summary || toMs < 0.0)
                return;
            Sync();
            if (fromMs < 0.0)
                fromMs = 0.0;
            if (toMs < fromMs)
                fromMs = toMs; // a jump back: only the new window
            std::uint32_t first = static_cast<std::uint32_t>(fromMs) / kWindowMs;
            const std::uint32_t last = static_cast<std::uint32_t>(toMs) / kWindowMs;
            if (last - first > 64)
                first = last; // a jump forward: not played through
            std::vector<std::uint32_t>& covered = m_summary->covered;
            for (std::uint32_t window = first; window <= last; ++window)
            {
                if (m_anyOrder)
                {
                    if (window != m_lastCovered)
                        ++m_passes[window];
                    m_lastCovered = window;
                    if (!std::binary_search(covered.begin(), covered.end(), window))
                        covered.insert(std::lower_bound(covered.begin(), covered.end(), window), window);
                }
                else if (covered.empty() || covered.back() < window)
                    covered.push_back(window);
            }
        }
        // A pause, scrub or seek: the next covered window is a new pass.
        void Break() { m_lastCovered = UINT32_MAX; }

        const std::unordered_map<std::uint32_t, std::uint16_t>& Passes() const { return m_passes; }

    private:
        void Sync()
        {
            if (!m_summary || m_generation == m_summary->generation)
                return;
            m_generation = m_summary->generation;
            m_keys.clear();
            for (std::size_t n = 0; n < m_summary->keys.size(); ++n)
                m_keys.emplace(KeyHash(m_summary->keys[n]), static_cast<std::uint16_t>(n));
            m_open.clear();
            m_openWindow = UINT32_MAX;
            if (m_anyOrder)
                for (std::size_t n = 0; n < m_summary->cells.size(); ++n)
                    m_open[(static_cast<std::uint64_t>(m_summary->cells[n].window) << 16) | m_summary->cells[n].key] = n;
        }

        bool KeyIndex(const SummaryKey& key, std::uint16_t& index)
        {
            const std::uint64_t hash = KeyHash(key);
            auto range = m_keys.equal_range(hash);
            for (auto it = range.first; it != range.second; ++it)
            {
                if (it->second < m_summary->keys.size() && m_summary->keys[it->second] == key)
                {
                    index = it->second;
                    return true;
                }
            }
            if (m_summary->keys.size() >= kMaxKeys)
                return false;
            index = static_cast<std::uint16_t>(m_summary->keys.size());
            m_summary->keys.push_back(key);
            m_keys.emplace(hash, index);
            return true;
        }

        Summary* m_summary = nullptr;
        bool m_anyOrder = false;
        std::uint32_t m_generation = 0;
        std::unordered_multimap<std::uint64_t, std::uint16_t> m_keys;
        std::unordered_map<std::uint64_t, std::size_t> m_open;
        std::uint32_t m_openWindow = UINT32_MAX;
        std::unordered_map<std::uint32_t, std::uint16_t> m_passes;
        std::uint32_t m_lastCovered = UINT32_MAX;
    };

    // ------------------------------------------------------------ ignore list

    // Keys that differ between a recording and its playback with nothing
    // happening (learned from a still run, classes as there), and refs listed
    // in ReplayDebug\sound-ignore.txt (one 0x... per line, developer build).
    struct IgnoreList
    {
        std::vector<std::pair<std::string, std::uint64_t>> keys; // (class, ref)
        std::vector<std::uint64_t> refs;

        bool Has(OwnerClass cls, std::uint64_t ref) const
        {
            if (std::find(refs.begin(), refs.end(), ref) != refs.end())
                return true;
            const char* name = IgnoreClass(cls);
            for (const auto& key : keys)
                if (key.second == ref && key.first == name)
                    return true;
            return false;
        }

        // Parses "class|0xref" or "0xref" per line; '#' starts a comment.
        void AddText(const std::string& text)
        {
            std::size_t start = 0;
            while (start <= text.size())
            {
                std::size_t end = text.find('\n', start);
                if (end == std::string::npos)
                    end = text.size();
                std::string line = text.substr(start, end - start);
                start = end + 1;
                const std::size_t hash = line.find('#');
                if (hash != std::string::npos)
                    line.resize(hash);
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                    line.pop_back();
                std::size_t first = line.find_first_not_of(" \t\"");
                if (first == std::string::npos)
                    continue;
                line = line.substr(first);
                std::string cls;
                const std::size_t bar = line.find('|');
                if (bar != std::string::npos)
                {
                    cls = line.substr(0, bar);
                    line = line.substr(bar + 1);
                }
                char* tail = nullptr;
                const unsigned long long value = std::strtoull(line.c_str(), &tail, 16);
                if (!value || tail == line.c_str())
                    continue;
                if (cls.empty())
                    refs.push_back(value);
                else
                    keys.emplace_back(cls, value);
            }
        }
    };

    // The keys that differed in a still run (8 s, nothing happening): the
    // keys of recorded entities and of the player there.
    inline IgnoreList DefaultIgnoreList()
    {
        IgnoreList list;
        list.AddText(
            "entity|0x800003000da07f\n"
            "live_player|0x800005000dbbce\n"
            "live_player|0x800005000e195f\n");
        return list;
    }

    // ------------------------------------------------------------ diff

    enum class FindingKind : std::uint8_t
    {
        Missing,  // recorded, never in the replay
        Fewer,    // 2 or more missing and under half matched
        Extra,    // only in the replay (2 or more)
        More,     // the replay made over twice as many (3 or more extra)
        Refused,  // the engine refused half or more of the replay's requests
        Variant,  // a missing and an extra of the same sound in another bank (ref low bits): not counted
    };

    inline const char* FindingToken(FindingKind kind)
    {
        switch (kind)
        {
        case FindingKind::Missing: return "missing";
        case FindingKind::Fewer: return "fewer";
        case FindingKind::Extra: return "extra";
        case FindingKind::More: return "more";
        case FindingKind::Refused: return "refused";
        case FindingKind::Variant: return "variant";
        }
        return "?";
    }

    inline const char* FindingTitle(FindingKind kind)
    {
        switch (kind)
        {
        case FindingKind::Missing: return "MISSING";
        case FindingKind::Fewer: return "FEWER";
        case FindingKind::Extra: return "EXTRA (replay only)";
        case FindingKind::More: return "MORE";
        case FindingKind::Refused: return "REFUSED";
        case FindingKind::Variant: return "variant";
        }
        return "?";
    }

    struct Finding
    {
        FindingKind kind = FindingKind::Missing;
        std::uint32_t owner = kNoOwner;
        OwnerClass cls = OwnerClass::World;
        std::uint64_t ref = 0;
        std::uint32_t path[kPathDepth] = {};
        double recorded = 0.0;
        double played = 0.0;
        double matched = 0.0;
        std::uint32_t requests = 0; // replay requests (refused count below)
        std::uint32_t refused = 0;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> spans; // clip ms [from, to) where it fell short
        // Clip ms [from, to) where the replay made more than
        // the recording (unmatched replay counts), and for a variant the
        // other ref of the pair and why it is one.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> extraSpans;
        std::uint64_t otherRef = 0;
        bool sameRequest = false; // paired by the same caller path at the same times (not by the bank)
        bool lostSide = false;    // the recorded side of a variant pair
    };

    struct DiffTotals
    {
        double recordedOwned = 0.0;
        double playedOwned = 0.0;
        double recordedWorld = 0.0; // no owner and the live player (not judged)
        double playedWorld = 0.0;
        std::uint32_t coveredMs = 0;      // compared time (both sides ran)
        std::uint32_t recordedMs = 0;     // recorded time
        std::uint32_t counted[6] = {};    // per FindingKind
        std::uint32_t ignored = 0;        // keys on the ignore list
        std::uint64_t playRefused = 0;    // every refused replay request (owned)
        bool recordedCensus = false;      // the clip had a census
    };

    struct DiffResult
    {
        std::vector<Finding> findings;
        DiffTotals totals;
    };

    // Low bits of a ref: the same sound in another metadata bank (the bytes
    // above the low 32 bits are the bank, 0x800002.. to 0x800006..).
    inline std::uint64_t RefLow(std::uint64_t ref) { return ref & 0xFFFFFFFFull; }

    // The frames of the shared create-and-play queue path
    // (puppet_audio_rule.h: the drain 0x258A348 returns to 0x258A469, the
    // audio update calls it at 0x2F1CEF3 and 0x2F1D239). Every queued
    // one-shot shows them, so two refs on that path are not one request.
    inline bool QueueFrame(std::uint32_t frame)
    {
        return frame == 0x258a469u || frame == 0x2f1d23eu || frame == 0x2f1cef8u || frame == 0x2ee86b1u ||
            frame == 0x25a74dbu || frame == 0;
    }

    // Two findings asked by the same call: the whole path equal, and at
    // least one frame past the creator that is not the shared queue's.
    inline bool SameRequestPath(const std::uint32_t a[kPathDepth], const std::uint32_t b[kPathDepth])
    {
        if (std::memcmp(a, b, sizeof(std::uint32_t) * kPathDepth) != 0 || !a[0])
            return false;
        for (std::uint32_t n = 1; n < kPathDepth; ++n)
            if (!QueueFrame(a[n]))
                return true;
        return false;
    }

    // About the same number: the extra makes up at least half of what is
    // short and at most twice it (plus one for a border window).
    inline bool SubstituteCounts(double shortBy, double overBy)
    {
        return shortBy >= 1.0 - 1e-9 && overBy >= shortBy * 0.5 - 1e-9 && overBy <= shortBy * 2.0 + 1.0 + 1e-9;
    }

    // At the same times: at least half of the short windows have an extra
    // window within one window (the replay's frame of delay).
    inline bool SpansMeet(const std::vector<std::pair<std::uint32_t, std::uint32_t>>& shortSpans,
        const std::vector<std::pair<std::uint32_t, std::uint32_t>>& extraSpans)
    {
        std::uint32_t total = 0, met = 0;
        for (const auto& lost : shortSpans)
        {
            for (std::uint32_t at = lost.first; at < lost.second; at += kWindowMs)
            {
                ++total;
                for (const auto& extra : extraSpans)
                {
                    if (extra.first <= at + kWindowMs && at < extra.second + kWindowMs)
                    {
                        ++met;
                        break;
                    }
                }
            }
        }
        return total && met * 2 >= total;
    }

    // The ref a pair is keyed by. A ref of the known form (set in the low
    // dword, entry in bits 32 to 39, flag bit 55) is one sound with or without
    // the flag, as soundowner::SoundKey has it: the generic player creates a
    // RefSet packet again with the flag (sound_generic_core.h
    // RefOfStableId), so a world or object sound recorded as 0x7000ff81a
    // comes back as 0x800007000ff81a. Any other ref is kept whole.
    inline std::uint64_t PairRef(std::uint64_t ref)
    {
        constexpr std::uint64_t kFlag = 1ull << 55;
        constexpr std::uint64_t kKnown = 0xFFFFFFFFull | (0xFFull << 32) | kFlag;
        if ((ref & ~kKnown) == 0 && (ref & 0xFFFFFFFFull) != 0)
            return ref & ~kFlag;
        return ref;
    }

    // The return addresses of the speech line controller's
    // sound creations (0x2FA10CF, the wave in 0x2FA10A4; 0x2FA0DCC, a type 2
    // layer in 0x2FA0D88; puppet_audio_rule.h SpeechControllerCaller).
    inline bool SpeechControllerPath(const std::uint32_t path[kPathDepth])
    {
        for (std::uint32_t n = 0; n < kPathDepth; ++n)
            if (path[n] == 0x2FA10CFu || path[n] == 0x2FA0DCCu)
                return true;
        return false;
    }

    // A line of the recorded player said by the hidden live player
    // (SpeechPlayerOnPlayer, SPEECH_PLAYER_ROUTE speaker=player) makes its
    // sounds on the hidden player's speech entity: counted as the recorded
    // player's, the census compares them with the recorded line. `proxyOn`:
    // the route is on.
    inline void ProxySpeechKey(bool proxyOn, std::uint32_t recordedPlayer, SummaryKey& key)
    {
        if (!proxyOn || recordedPlayer == kNoOwner || key.cls != OwnerClass::LivePlayer || !SpeechControllerPath(key.path))
            return;
        key.owner = recordedPlayer;
        key.cls = OwnerClass::Player;
    }

    // The comparison. `passes`: how often normal playback went through each
    // window (counts are per pass). Only windows both sides covered count.
    // Linear in the cells (a 140 s busy clip: tens of thousands), run once
    // at the end of a playback on the script thread.
    inline DiffResult Diff(const Summary& recorded, const Summary& played,
        const std::unordered_map<std::uint32_t, std::uint16_t>& passes, const IgnoreList& ignore)
    {
        DiffResult result;
        DiffTotals& totals = result.totals;
        totals.recordedCensus = !recorded.covered.empty();
        totals.recordedMs = static_cast<std::uint32_t>(recorded.covered.size()) * kWindowMs;
        // The windows both sides covered, as a bitmap, and the passes.
        std::uint32_t maxWindow = 0;
        for (const std::uint32_t window : recorded.covered)
            maxWindow = (std::max)(maxWindow, window);
        std::vector<std::uint8_t> both(static_cast<std::size_t>(maxWindow) + 2, 0);
        std::vector<std::uint16_t> passCount(both.size(), 0);
        std::uint32_t bothCount = 0;
        for (const std::uint32_t window : recorded.covered)
        {
            const auto pass = passes.find(window);
            if (pass == passes.end() || pass->second == 0)
                continue;
            both[window] = 1;
            passCount[window] = pass->second;
            ++bothCount;
        }
        totals.coveredMs = bothCount * kWindowMs;
        const auto inBoth = [&](std::uint32_t window) { return window < both.size() && both[window] != 0; };
        const auto judged = [](OwnerClass cls, std::uint32_t owner) {
            return owner != kNoOwner && cls != OwnerClass::World && cls != OwnerClass::LivePlayer;
        };

        struct Side
        {
            std::vector<std::pair<std::uint32_t, double>> counts; // window, count (sorted below)
            double total = 0.0;
            std::uint32_t requests = 0;
            std::uint32_t refused = 0;
            OwnerClass cls = OwnerClass::World;
            std::int32_t bestKey = -1; // the key (path) with the most
            double bestCount = 0.0;
        };
        struct Pair
        {
            std::uint32_t owner = kNoOwner;
            std::uint64_t ref = 0;   // PairRef
            std::uint64_t shown = 0; // the first ref seen (the recorded one when there is one)
            Side rec;
            Side play;
        };
        std::vector<Pair> pairs;
        std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> pairIndex; // hash(owner, ref) -> pairs
        const auto pairOf = [&](std::uint32_t owner, std::uint64_t rawRef) -> Pair& {
            const std::uint64_t ref = PairRef(rawRef);
            const std::uint64_t hash = (ref * 0x9E3779B97F4A7C15ull) ^ owner;
            std::vector<std::uint32_t>& bucket = pairIndex[hash];
            for (const std::uint32_t index : bucket)
                if (pairs[index].owner == owner && pairs[index].ref == ref)
                    return pairs[index];
            bucket.push_back(static_cast<std::uint32_t>(pairs.size()));
            pairs.push_back(Pair{});
            pairs.back().owner = owner;
            pairs.back().ref = ref;
            pairs.back().shown = rawRef;
            return pairs.back();
        };
        // Per side: each key's pair once, and its count for the best path.
        const auto fold = [&](const Summary& summary, bool play) {
            std::vector<std::int32_t> keyPair(summary.keys.size(), -2); // -2 not seen, -1 not judged
            std::vector<double> keyCount(summary.keys.size(), 0.0);
            for (const SummaryCell& cell : summary.cells)
            {
                if (cell.key >= summary.keys.size() || !inBoth(cell.window))
                    continue;
                const SummaryKey& key = summary.keys[cell.key];
                const double n = play ? static_cast<double>(passCount[cell.window]) : 1.0;
                const double count = cell.count / (n > 0.0 ? n : 1.0);
                if (keyPair[cell.key] == -2)
                {
                    if (!judged(key.cls, key.owner))
                        keyPair[cell.key] = -1;
                    else
                    {
                        Pair& pair = pairOf(key.owner, key.ref);
                        keyPair[cell.key] = static_cast<std::int32_t>(&pair - pairs.data());
                    }
                }
                if (keyPair[cell.key] < 0)
                {
                    (play ? totals.playedWorld : totals.recordedWorld) += count;
                    continue;
                }
                Side& side = play ? pairs[keyPair[cell.key]].play : pairs[keyPair[cell.key]].rec;
                if (!side.counts.empty() && side.counts.back().first == cell.window)
                    side.counts.back().second += count;
                else
                    side.counts.push_back({cell.window, count});
                side.total += count;
                side.cls = key.cls;
                keyCount[cell.key] += count;
                if (play)
                {
                    side.requests += cell.count;
                    side.refused += cell.refused;
                    totals.playRefused += cell.refused;
                }
                (play ? totals.playedOwned : totals.recordedOwned) += count;
            }
            for (std::size_t k = 0; k < summary.keys.size(); ++k)
            {
                if (keyPair[k] < 0)
                    continue;
                Side& side = play ? pairs[keyPair[k]].play : pairs[keyPair[k]].rec;
                if (keyCount[k] > side.bestCount)
                {
                    side.bestCount = keyCount[k];
                    side.bestKey = static_cast<std::int32_t>(k);
                }
            }
        };
        fold(recorded, false);
        fold(played, true);

        // What each pair's replay side made over its recorded
        // side (finding or not), for the same-request pairing below.
        struct Leftover
        {
            std::uint32_t owner = kNoOwner;
            std::uint64_t ref = 0;
            std::uint32_t path[kPathDepth] = {};
            double overBy = 0.0;
            double left = 0.0; // not yet given to a lost sound
            std::vector<std::pair<std::uint32_t, std::uint32_t>> spans;
        };
        std::vector<Leftover> leftovers;

        for (Pair& pair : pairs)
        {
            const OwnerClass cls = pair.rec.total > 0.0 ? pair.rec.cls : pair.play.cls;
            if (ignore.Has(cls, pair.ref))
            {
                ++totals.ignored;
                continue;
            }
            // The replay's windows sorted (a playback counts out of order),
            // merged per window.
            std::vector<std::pair<std::uint32_t, double>>& left = pair.play.counts;
            std::sort(left.begin(), left.end(),
                [](const std::pair<std::uint32_t, double>& a, const std::pair<std::uint32_t, double>& b) { return a.first < b.first; });
            std::size_t merged = 0;
            for (std::size_t n = 0; n < left.size(); ++n)
            {
                if (merged && left[merged - 1].first == left[n].first)
                    left[merged - 1].second += left[n].second;
                else
                    left[merged++] = left[n];
            }
            left.resize(merged);
            const auto at = [&](std::int64_t window) -> double* {
                if (window < 0)
                    return nullptr;
                const auto it = std::lower_bound(left.begin(), left.end(), static_cast<std::uint32_t>(window),
                    [](const std::pair<std::uint32_t, double>& entry, std::uint32_t w) { return entry.first < w; });
                return it != left.end() && it->first == window ? &it->second : nullptr;
            };
            // Greedy match, the same window first, then one window either side
            // (a sound at a window border, the replay's frame of delay).
            double matched = 0.0;
            std::vector<std::uint32_t> shortWindows;
            for (const auto& rec : pair.rec.counts)
            {
                double need = rec.second;
                for (const std::int64_t delta : {0, -1, 1})
                {
                    if (need <= 1e-9)
                        break;
                    double* have = at(static_cast<std::int64_t>(rec.first) + delta);
                    if (!have || *have <= 1e-9)
                        continue;
                    const double take = (std::min)(need, *have);
                    *have -= take;
                    need -= take;
                    matched += take;
                }
                if (need > 0.5)
                    shortWindows.push_back(rec.first);
            }
            Finding finding;
            finding.owner = pair.owner;
            finding.cls = cls;
            finding.ref = pair.shown;
            finding.recorded = pair.rec.total;
            finding.played = pair.play.total;
            finding.matched = matched;
            finding.requests = pair.play.requests;
            finding.refused = pair.play.refused;
            const Side& pathSide = pair.rec.bestKey >= 0 ? pair.rec : pair.play;
            const Summary& pathSummary = pair.rec.bestKey >= 0 ? recorded : played;
            if (pathSide.bestKey >= 0)
                std::memcpy(finding.path, pathSummary.keys[pathSide.bestKey].path, sizeof(finding.path));
            for (const std::uint32_t window : shortWindows)
            {
                const std::uint32_t from = window * kWindowMs;
                if (!finding.spans.empty() && finding.spans.back().second == from)
                    finding.spans.back().second = from + kWindowMs;
                else
                    finding.spans.push_back({from, from + kWindowMs});
            }
            // The replay's windows with counts left over.
            for (const auto& entry : left)
            {
                if (entry.second <= 0.5)
                    continue;
                const std::uint32_t from = entry.first * kWindowMs;
                if (!finding.extraSpans.empty() && finding.extraSpans.back().second == from)
                    finding.extraSpans.back().second = from + kWindowMs;
                else
                    finding.extraSpans.push_back({from, from + kWindowMs});
            }
            const double missing = pair.rec.total - matched;
            const double extra = pair.play.total - matched;
            if (extra > 0.5 && pair.play.bestKey >= 0 && !finding.extraSpans.empty())
            {
                Leftover over;
                over.owner = pair.owner;
                over.ref = pair.shown;
                std::memcpy(over.path, played.keys[pair.play.bestKey].path, sizeof(over.path));
                over.overBy = extra;
                over.left = extra;
                over.spans = finding.extraSpans;
                leftovers.push_back(std::move(over));
            }
            bool any = false;
            if (pair.rec.total > 0.0 && pair.play.total < 0.5)
            {
                finding.kind = FindingKind::Missing;
                any = true;
            }
            else if (pair.rec.total > 0.0 && missing >= 2.0 - 1e-9 && matched < pair.rec.total / 2.0)
            {
                finding.kind = FindingKind::Fewer;
                any = true;
            }
            else if (pair.rec.total < 0.5 && pair.play.total >= 2.0 - 1e-9)
            {
                finding.kind = FindingKind::Extra;
                any = true;
            }
            else if (pair.rec.total > 0.0 && extra >= 3.0 - 1e-9 && pair.play.total > 2.0 * pair.rec.total)
            {
                finding.kind = FindingKind::More;
                any = true;
            }
            if (any)
                result.findings.push_back(finding);
            if (pair.rec.total > 0.0 && finding.refused >= 2 && finding.refused * 2 >= finding.requests)
            {
                Finding refused = finding;
                refused.kind = FindingKind::Refused;
                refused.spans.clear();
                result.findings.push_back(refused);
            }
        }

        // A missing sound and an extra one of the same owner whose refs
        // differ only in the bank: the engine picked another variant.
        for (Finding& lost : result.findings)
        {
            if (lost.kind != FindingKind::Missing && lost.kind != FindingKind::Fewer)
                continue;
            for (Finding& added : result.findings)
            {
                if ((added.kind != FindingKind::Extra && added.kind != FindingKind::More) || added.owner != lost.owner ||
                    added.ref == lost.ref || RefLow(added.ref) != RefLow(lost.ref))
                    continue;
                lost.kind = FindingKind::Variant;
                added.kind = FindingKind::Variant;
                lost.otherRef = added.ref;
                added.otherRef = lost.ref;
                lost.lostSide = true;
                break;
            }
        }
        // The same request resolving to another sound. A missing (or fewer)
        // and an extra (or more) of one owner, refs that differ in the set
        // too, asked by the same caller path (not the shared create-and-play
        // queue path, SameRequestPath), at the same times (the extra's
        // windows within one window of the missing ones) and in about the
        // same number: the engine picked another sound for the same call (a
        // weapon's shot layer at 0x2EFAEAF: the ref 0x2EF9F7C picks from the
        // fire info and the weapon audio's state, which the puppet's weapon
        // does not have). The other side is any pair the replay made more of
        // (not only a finding: an excess of 1 per pass is no EXTRA line), and
        // an excess larger than the loss keeps its own MORE line.
        for (Finding& lost : result.findings)
        {
            if (lost.kind != FindingKind::Missing && lost.kind != FindingKind::Fewer)
                continue;
            const double shortBy = lost.recorded - lost.matched;
            for (Leftover& over : leftovers)
            {
                if (over.owner != lost.owner || PairRef(over.ref) == PairRef(lost.ref) || !SameRequestPath(lost.path, over.path))
                    continue;
                if (over.left < shortBy * 0.5 - 1e-9 || !SpansMeet(lost.spans, over.spans))
                    continue;
                over.left -= (std::min)(over.left, shortBy);
                lost.kind = FindingKind::Variant;
                lost.otherRef = over.ref;
                lost.sameRequest = true;
                lost.lostSide = true;
                for (Finding& added : result.findings)
                {
                    if ((added.kind != FindingKind::Extra && added.kind != FindingKind::More) || added.owner != lost.owner ||
                        added.ref != over.ref)
                        continue;
                    if (SubstituteCounts(shortBy, over.overBy))
                    {
                        added.kind = FindingKind::Variant;
                        added.otherRef = lost.ref;
                        added.sameRequest = true;
                    }
                    break;
                }
                break;
            }
        }
        for (const Finding& finding : result.findings)
            ++totals.counted[static_cast<int>(finding.kind)];
        std::stable_sort(result.findings.begin(), result.findings.end(), [](const Finding& a, const Finding& b) {
            if (ClassOrder(a.cls) != ClassOrder(b.cls))
                return ClassOrder(a.cls) < ClassOrder(b.cls);
            if (a.kind != b.kind)
                return a.kind < b.kind;
            return (a.recorded - a.matched) + (a.played - a.matched) > (b.recorded - b.matched) + (b.played - b.matched);
        });
        return result;
    }

    // ------------------------------------------------------------ report

    inline std::string PathText(const std::uint32_t path[kPathDepth])
    {
        std::string text;
        char part[16]{};
        for (std::uint32_t n = 0; n < kPathDepth; ++n)
        {
            std::snprintf(part, sizeof(part), "%s%x", n ? ";" : "", path[n]);
            text += part;
        }
        return text;
    }

    inline std::string SecondsText(std::uint32_t ms)
    {
        char text[32]{};
        std::snprintf(text, sizeof(text), "%u.%u s", ms / 1000, (ms % 1000) / 100);
        return text;
    }

    inline std::string CountText(double value)
    {
        char text[32]{};
        if (std::fabs(value - std::round(value)) < 0.05)
            std::snprintf(text, sizeof(text), "%.0f", value);
        else
            std::snprintf(text, sizeof(text), "%.1f", value);
        return text;
    }

    inline std::string SpansText(const std::vector<std::pair<std::uint32_t, std::uint32_t>>& spans, std::size_t limit = 8)
    {
        std::string text;
        for (std::size_t n = 0; n < spans.size() && n < limit; ++n)
        {
            if (n)
                text += "; ";
            if (spans[n].second - spans[n].first <= kWindowMs)
                text += SecondsText(spans[n].first);
            else
                text += SecondsText(spans[n].first) + " to " + SecondsText(spans[n].second);
        }
        if (spans.size() > limit)
            text += "; and " + std::to_string(spans.size() - limit) + " more spans";
        return text;
    }

    struct ReportHeader
    {
        std::string clip;      // file name of the clip
        std::string when;      // local time text
        std::string version;   // build version
        std::uint32_t durationMs = 0;
        std::string capture;   // one line on the census-level capture (may be empty)
        std::string coverage;  // FormatCoverage
    };

    // ------------------------------------------------------------ coverage

    // What the playhead did over each window of a playback, so the report
    // says, by span with times, what was compared and why the rest was not
    // (outside the trim, slow motion, pause, scrub).
    enum : std::uint8_t
    {
        kCoverNormal = 1, // compared, the playhead at x1
        kCoverSlow = 2,   // compared, the playhead slower than x1 (slow motion marker)
        kCoverFast = 4,   // compared, the playhead faster than x1
        kCoverPaused = 8, // the playhead stood here paused (or preparing)
        kCoverScrub = 16, // the playhead went over it scrubbing or jumping
    };

    inline std::uint8_t RateFlag(double rate)
    {
        return rate < 0.99 ? kCoverSlow : rate > 1.01 ? kCoverFast : kCoverNormal;
    }

    struct PlayCoverage
    {
        std::vector<std::uint8_t> flags; // per window
        double slowestRate = 1.0;
        double fastestRate = 1.0;

        void Clear()
        {
            flags.clear();
            slowestRate = fastestRate = 1.0;
        }

        // The windows from fromMs to toMs (a jump of more than 64 windows
        // marks only the last one, as Accumulator::Cover).
        void Note(double fromMs, double toMs, std::uint8_t flag, double rate = 1.0)
        {
            if (toMs < 0.0)
                return;
            if (fromMs < 0.0)
                fromMs = 0.0;
            if (toMs < fromMs)
                fromMs = toMs;
            std::uint32_t first = static_cast<std::uint32_t>(fromMs) / kWindowMs;
            const std::uint32_t last = static_cast<std::uint32_t>(toMs) / kWindowMs;
            if (last - first > 64)
                first = last;
            if (last >= flags.size())
                flags.resize(static_cast<std::size_t>(last) + 1, 0);
            for (std::uint32_t window = first; window <= last; ++window)
                flags[window] = static_cast<std::uint8_t>(flags[window] | flag);
            if (flag == kCoverSlow)
                slowestRate = (std::min)(slowestRate, rate);
            if (flag == kCoverFast)
                fastestRate = (std::max)(fastestRate, rate);
        }

        std::uint8_t At(std::uint32_t window) const { return window < flags.size() ? flags[window] : 0; }
    };

    enum class CoverClass : std::uint8_t
    {
        Normal = 0,  // compared at x1
        Slow = 1,    // compared, some pass in slow motion
        Fast = 2,    // compared, some pass faster than x1
        Stood = 3,   // not compared: only paused or scrubbed over
        Never = 4,   // not compared: the playhead never came here
    };
    constexpr std::uint8_t kCoverClassCount = 5;

    // A recorded window's class: compared when normal playback (any rate)
    // went through it and the recording covered it (the Diff's "both").
    inline CoverClass ClassOfWindow(std::uint8_t flags, bool compared)
    {
        if (compared)
            return (flags & kCoverSlow) ? CoverClass::Slow : (flags & kCoverFast) ? CoverClass::Fast : CoverClass::Normal;
        return (flags & (kCoverPaused | kCoverScrub | kCoverNormal | kCoverSlow | kCoverFast)) ? CoverClass::Stood
                                                                                                : CoverClass::Never;
    }

    // The coverage block of the report: one line per class with its spans
    // (clip times) and seconds. `passes`: the census's normal-play passes
    // per window (Accumulator::Passes), the same "both" as Diff.
    inline std::string FormatCoverage(const Summary& recorded, const PlayCoverage& cover,
        const std::unordered_map<std::uint32_t, std::uint16_t>& passes, std::size_t spanLimit = 10)
    {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> spans[kCoverClassCount];
        std::uint32_t ms[kCoverClassCount] = {};
        std::uint32_t recordedMs = 0;
        for (const std::uint32_t window : recorded.covered)
        {
            const auto pass = passes.find(window);
            const bool compared = pass != passes.end() && pass->second != 0;
            const CoverClass cls = ClassOfWindow(cover.At(window), compared);
            const std::uint8_t c = static_cast<std::uint8_t>(cls);
            const std::uint32_t from = window * kWindowMs;
            std::vector<std::pair<std::uint32_t, std::uint32_t>>& list = spans[c];
            if (!list.empty() && list.back().second == from)
                list.back().second = from + kWindowMs;
            else
                list.push_back({from, from + kWindowMs});
            ms[c] += kWindowMs;
            recordedMs += kWindowMs;
        }
        const auto spanText = [&](const std::vector<std::pair<std::uint32_t, std::uint32_t>>& list) {
            if (list.empty())
                return std::string("none");
            std::string text;
            for (std::size_t n = 0; n < list.size() && n < spanLimit; ++n)
                text += std::string(n ? "; " : "") + SecondsText(list[n].first) + " to " + SecondsText(list[n].second);
            if (list.size() > spanLimit)
                text += "; and " + std::to_string(list.size() - spanLimit) + " more spans";
            return text;
        };
        const std::uint32_t comparedMs = ms[0] + ms[1] + ms[2];
        std::string out = "Compared span: " + SecondsText(comparedMs) + " of " + SecondsText(recordedMs) + " recorded; " +
            SecondsText(recordedMs - comparedMs) + " left out (below, by span and why). Every window the playhead went through "
            "while playing counts, slow motion included; a sound counts once per pass.\n";
        char rate[48]{};
        out += "  compared at normal speed (" + SecondsText(ms[0]) + "): " + spanText(spans[0]) + "\n";
        if (ms[1])
        {
            std::snprintf(rate, sizeof(rate), "playhead down to x%.2f", cover.slowestRate);
            out += "  compared in slow motion (" + SecondsText(ms[1]) + ", " + rate + "): " + spanText(spans[1]) + "\n";
        }
        if (ms[2])
        {
            std::snprintf(rate, sizeof(rate), "playhead up to x%.2f", cover.fastestRate);
            out += "  compared faster than normal (" + SecondsText(ms[2]) + ", " + rate + "): " + spanText(spans[2]) + "\n";
        }
        out += "  left out, the playhead only stood there paused or went over it scrubbing or jumping (" + SecondsText(ms[3]) +
            "): " + spanText(spans[3]) + "\n";
        out += "  left out, never reached by the playhead (outside the clip's trim, or not played before Stop) (" +
            SecondsText(ms[4]) + "): " + spanText(spans[4]) + "\n\n";
        return out;
    }

    inline std::string IdText(std::uint32_t packed)
    {
        char text[16]{};
        std::snprintf(text, sizeof(text), "%u:%u", packed >> 16, packed & 0xFFFFu);
        return text;
    }

    // ReplayDebug\<clip>.sound-check.txt (English, no dashes).
    inline std::string FormatReport(const DiffResult& diff, const ReportHeader& header)
    {
        const DiffTotals& t = diff.totals;
        std::string out;
        out += "Replay sounds compared with the recording: " + header.clip + "\n";
        out += "Generated " + header.when + " by version " + header.version + ".\n";
        if (!t.recordedCensus)
        {
            out += "\nThis clip was recorded without the sound census (a version older than 0.174, or SoundCheck=0).\n"
                   "There is nothing to compare with: record it again to get the report.\n";
            return out;
        }
        if (!header.coverage.empty())
            out += header.coverage;
        else
            out += "Compared span: " + SecondsText(t.coveredMs) + " of " + SecondsText(t.recordedMs) +
                " recorded (what the playhead played through, slow motion included; not paused, scrubbed or jumped over).\n\n";
        const std::uint32_t lost = t.counted[static_cast<int>(FindingKind::Missing)];
        const std::uint32_t fewer = t.counted[static_cast<int>(FindingKind::Fewer)];
        const std::uint32_t extra = t.counted[static_cast<int>(FindingKind::Extra)] + t.counted[static_cast<int>(FindingKind::More)];
        const std::uint32_t refused = t.counted[static_cast<int>(FindingKind::Refused)];
        out += "SUMMARY: " + std::to_string(lost) + " sounds missing, " + std::to_string(fewer) + " played fewer times, " +
            std::to_string(extra) + " extra, " + std::to_string(refused) + " refused by the engine.\n";
        out += "Sound requests with an owner: recording " + CountText(t.recordedOwned) + ", replay " + CountText(t.playedOwned) +
            ". Without an owner (wind, birds, NPCs outside the clip, interface): recording " + CountText(t.recordedWorld) +
            ", replay " + CountText(t.playedWorld) + " (not counted).\n";
        if (t.ignored)
            out += "Ignored by the list (known noise): " + std::to_string(t.ignored) + ".\n";
        if (!header.capture.empty())
            out += header.capture + "\n";
        if (lost + fewer + extra + refused == 0)
            out += "\nNo difference with an owner. If a sound still seems missing by ear, it comes from a path the census does not see "
                   "(inner layers of a sound), or it is a matter of volume, not presence.\n";
        int lastClass = -1;
        for (const Finding& f : diff.findings)
        {
            if (f.kind == FindingKind::Variant)
                continue;
            if (ClassOrder(f.cls) != lastClass)
            {
                lastClass = ClassOrder(f.cls);
                out += std::string("\n== ") + ClassTitle(f.cls) + " ==\n";
            }
            char line[256]{};
            std::snprintf(line, sizeof(line), "%s  owner %s  ref 0x%llx  recorded %s, replay %s", FindingTitle(f.kind),
                IdText(f.owner).c_str(), static_cast<unsigned long long>(f.ref), CountText(f.recorded).c_str(),
                CountText(f.played).c_str());
            out += line;
            if (f.kind == FindingKind::Refused)
                out += ", refused " + std::to_string(f.refused) + " of " + std::to_string(f.requests);
            out += "\n    path c=" + PathText(f.path);
            if (!f.spans.empty() && f.kind != FindingKind::Extra && f.kind != FindingKind::More)
                out += "\n    when: " + SpansText(f.spans);
            if (!f.extraSpans.empty() && (f.kind == FindingKind::Extra || f.kind == FindingKind::More))
                out += "\n    over at: " + SpansText(f.extraSpans);
            out += "\n";
        }
        std::uint32_t variants = t.counted[static_cast<int>(FindingKind::Variant)];
        if (variants)
        {
            // Each pair, recorded ref first.
            out += "\n== variants (not counted: the engine picked another sound for the same request) ==\n";
            for (const Finding& f : diff.findings)
            {
                if (f.kind != FindingKind::Variant || !f.lostSide)
                    continue;
                char line[320]{};
                std::snprintf(line, sizeof(line), "variant  owner %s (%s)  recorded 0x%llx (%s, replay %s), replay made 0x%llx instead; %s\n",
                    IdText(f.owner).c_str(), ClassToken(f.cls), static_cast<unsigned long long>(f.ref),
                    CountText(f.recorded).c_str(), CountText(f.played).c_str(), static_cast<unsigned long long>(f.otherRef),
                    f.sameRequest ? "same caller path at the same times" : "same set entry in another bank");
                out += line;
                out += "    path c=" + PathText(f.path) + "\n";
            }
        }
        out += "\nHow to read: \"owner\" is the recorded id (the same as in the log and in ReplayClipTool), \"ref\" is the sound in the metadata,\n"
               "\"path\" is the engine addresses that requested the sound. Send this file together with RedemptionEditor.log.\n";
        return out;
    }

    // SOUND_MISSING_SUMMARY (kept with Debug=0: the _SUMMARY suffix).
    inline std::string FormatSummaryLine(const DiffResult& diff, const std::string& clip, const std::string& report)
    {
        const DiffTotals& t = diff.totals;
        char text[640]{};
        std::snprintf(text, sizeof(text),
            "SOUND_MISSING_SUMMARY,clip=%s,census=%d,missing=%u,fewer=%u,extra=%u,more=%u,refused=%u,variants=%u,"
            "ignored=%u,rec_owned=%.0f,play_owned=%.0f,rec_world=%.0f,play_world=%.0f,covered_ms=%u,recorded_ms=%u,report=%s",
            clip.c_str(), t.recordedCensus ? 1 : 0, t.counted[0], t.counted[1], t.counted[2], t.counted[3], t.counted[4],
            t.counted[5], t.ignored, t.recordedOwned, t.playedOwned, t.recordedWorld, t.playedWorld, t.coveredMs,
            t.recordedMs, report.c_str());
        std::string line = text;
        // The first few findings, for the log reader.
        int listed = 0;
        for (const Finding& f : diff.findings)
        {
            if (f.kind == FindingKind::Variant || listed >= 6)
                continue;
            char item[160]{};
            std::snprintf(item, sizeof(item), "%s%s:%s:%s:0x%llx", listed ? "|" : ",top=", FindingToken(f.kind),
                ClassToken(f.cls), IdText(f.owner).c_str(), static_cast<unsigned long long>(f.ref));
            line += item;
            ++listed;
        }
        return line;
    }

    // ------------------------------------------------------------ on-screen notice

    // What the report found, short, for the notice at the playback's stop
    // (nobody reads a file to know a sound went missing).
    // Missing counts MISSING and FEWER findings, extra counts EXTRA and
    // MORE (one per sound, as the report's SUMMARY), grouped by owner with
    // the clip time of the owner's first one; the busiest owners first, at
    // most `perSide` of them per side. Empty below `minFindings` (0: never).
    constexpr std::uint32_t kNoticeMinFindings = 3;
    constexpr std::size_t kNoticeOwnersPerSide = 2;

    inline std::string NoticeClock(std::uint32_t ms)
    {
        char text[24]{};
        std::snprintf(text, sizeof(text), "%u:%02u.%u", ms / 60000u, (ms / 1000u) % 60u, (ms % 1000u) / 100u);
        return text;
    }

    inline std::string FormatNotice(const DiffResult& diff, std::uint32_t minFindings = kNoticeMinFindings,
        std::size_t perSide = kNoticeOwnersPerSide)
    {
        struct OwnerCount
        {
            std::uint32_t owner = kNoOwner;
            OwnerClass cls = OwnerClass::World;
            std::uint32_t count = 0;
            std::uint32_t firstMs = 0xFFFFFFFFu;
        };
        std::vector<OwnerCount> sides[2];
        std::uint32_t totals[2] = {};
        for (const Finding& f : diff.findings)
        {
            int side = -1;
            if (f.kind == FindingKind::Missing || f.kind == FindingKind::Fewer)
                side = 0;
            else if (f.kind == FindingKind::Extra || f.kind == FindingKind::More)
                side = 1;
            if (side < 0)
                continue;
            ++totals[side];
            const auto& spans = side == 0 ? f.spans : (f.extraSpans.empty() ? f.spans : f.extraSpans);
            std::uint32_t firstMs = 0xFFFFFFFFu;
            for (const auto& span : spans)
                firstMs = (std::min)(firstMs, span.first);
            auto it = std::find_if(sides[side].begin(), sides[side].end(),
                [&](const OwnerCount& c) { return c.owner == f.owner; });
            if (it == sides[side].end())
            {
                sides[side].push_back(OwnerCount{f.owner, f.cls, 0, 0xFFFFFFFFu});
                it = sides[side].end() - 1;
            }
            ++it->count;
            it->firstMs = (std::min)(it->firstMs, firstMs);
        }
        if (minFindings == 0 || totals[0] + totals[1] < minFindings)
            return std::string();
        std::string out = "Sound check:";
        static const char* const kWords[2] = {"missing", "extra"};
        bool any = false;
        for (int side = 0; side < 2; ++side)
        {
            if (!totals[side])
                continue;
            auto& owners = sides[side];
            std::stable_sort(owners.begin(), owners.end(), [](const OwnerCount& a, const OwnerCount& b) {
                return a.count != b.count ? a.count > b.count : a.firstMs < b.firstMs;
            });
            out += std::string(any ? ", " : " ") + std::to_string(totals[side]) + " " + kWords[side] + " (";
            for (std::size_t n = 0; n < owners.size() && n < perSide; ++n)
            {
                const OwnerCount& c = owners[n];
                out += (n ? "; " : "") + std::string(c.owner == kNoOwner ? "world" : ClassToken(c.cls));
                if (c.owner != kNoOwner)
                    out += " " + IdText(c.owner);
                if (c.firstMs != 0xFFFFFFFFu)
                    out += " at " + NoticeClock(c.firstMs);
                out += ": " + std::to_string(c.count);
            }
            if (owners.size() > perSide)
                out += "; +" + std::to_string(owners.size() - perSide) + " more";
            out += ")";
            any = true;
        }
        out += ". Details in ReplayDebug.";
        return out;
    }

    // ------------------------------------------------------------ anti-double

    // The sounds a puppet made in this playback (census rows on its audio,
    // not started by the capture itself): a captured sound is not played
    // when its puppet made the same ref from beforeMs before its time until
    // it is due (each create answers for one captured sound), as the ped
    // audio rule. A loop restarted mid-span (scrub, pause) is
    // left to the puppet if the puppet made that ref at any time.
    class OwnCreates
    {
    public:
        void Clear()
        {
            m_list.clear();
            m_ever.clear();
        }
        // `sound`, the census row's sound (0:
        // unknown), what Recent names as the live twin of a world packet.
        void Add(std::uint32_t owner, std::uint64_t ref, std::uint32_t timeMs, const float* position = nullptr,
            std::uint64_t sound = 0)
        {
            Entry entry{owner, ref, timeMs, false, false, {0.0f, 0.0f, 0.0f}, sound};
            if (position)
            {
                entry.hasPosition = true;
                entry.position[0] = position[0];
                entry.position[1] = position[1];
                entry.position[2] = position[2];
            }
            m_list.push_back(entry);
            const std::pair<std::uint32_t, std::uint64_t> key{owner, ref};
            if (std::find(m_ever.begin(), m_ever.end(), key) == m_ever.end())
                m_ever.push_back(key);
        }
        // Drops creates newer than nowMs (a jump back) or older than keepMs.
        void Forget(std::uint32_t nowMs, std::uint32_t keepMs)
        {
            m_list.erase(std::remove_if(m_list.begin(), m_list.end(), [&](const Entry& entry) {
                return entry.timeMs > nowMs || nowMs - entry.timeMs > keepMs; }), m_list.end());
        }
        bool Take(std::uint32_t owner, std::uint64_t ref, std::uint32_t startMs, std::uint32_t nowMs, std::uint32_t beforeMs)
        {
            const std::uint32_t from = startMs > beforeMs ? startMs - beforeMs : 0;
            for (Entry& entry : m_list)
            {
                if (entry.used || entry.owner != owner || entry.ref != ref || entry.timeMs < from || entry.timeMs > nowMs)
                    continue;
                entry.used = true;
                return true;
            }
            return false;
        }
        // As Take, any entry of the same sound set (the low dword of the
        // key). The step track's call picks the entry from the puppet's own
        // state, so a recorded step packet whose set the call made is that
        // layer made again, not a layer the call lacks.
        bool TakeSet(std::uint32_t owner, std::uint64_t key, std::uint32_t startMs, std::uint32_t nowMs,
            std::uint32_t beforeMs)
        {
            const std::uint64_t set = key & 0xFFFFFFFFull;
            if (!set)
                return false;
            const std::uint32_t from = startMs > beforeMs ? startMs - beforeMs : 0;
            for (Entry& entry : m_list)
            {
                if (entry.used || entry.owner != owner || (entry.ref & 0xFFFFFFFFull) != set || entry.timeMs < from ||
                    entry.timeMs > nowMs)
                    continue;
                entry.used = true;
                return true;
            }
            return false;
        }
        // World sounds: another source made this sound within withinMs before
        // nowMs, used or not, and (both positions known) within nearM of
        // `position`. `sound` (optional) gets the matching create's sound (the
        // newest of the matches).
        bool Recent(std::uint32_t owner, std::uint64_t ref, std::uint32_t nowMs, std::uint32_t withinMs, const float* position,
            float nearM, std::uint64_t* sound = nullptr) const
        {
            bool found = false;
            std::uint32_t newest = 0;
            const std::uint32_t from = nowMs > withinMs ? nowMs - withinMs : 0;
            for (const Entry& entry : m_list)
            {
                if (entry.owner != owner || entry.ref != ref || entry.timeMs < from || entry.timeMs > nowMs)
                    continue;
                if (position && entry.hasPosition)
                {
                    const float dx = entry.position[0] - position[0];
                    const float dy = entry.position[1] - position[1];
                    const float dz = entry.position[2] - position[2];
                    if (dx * dx + dy * dy + dz * dz > nearM * nearM)
                        continue;
                }
                if (!sound)
                    return true;
                if (!found || entry.timeMs >= newest)
                {
                    newest = entry.timeMs;
                    *sound = entry.sound;
                }
                found = true;
            }
            return found;
        }
        bool Ever(std::uint32_t owner, std::uint64_t ref) const
        {
            return std::find(m_ever.begin(), m_ever.end(), std::make_pair(owner, ref)) != m_ever.end();
        }
        std::size_t Size() const { return m_list.size(); }

    private:
        struct Entry
        {
            std::uint32_t owner;
            std::uint64_t ref;
            std::uint32_t timeMs;
            bool used;
            bool hasPosition;
            float position[3];
            std::uint64_t sound;
        };
        std::vector<Entry> m_list;
        std::vector<std::pair<std::uint32_t, std::uint64_t>> m_ever;
    };

    // ------------------------------------------------------------ capture

    // A top-level sound of a recorded vehicle, train car or object that no
    // other track covers (clip section GSND): replayed on the owner's puppet
    // by ref with the recorded init params.
    struct GeneralSound
    {
        std::uint32_t entity = 0; // packed recorded id
        OwnerClass cls = OwnerClass::Object;
        std::uint32_t startMs = 0;
        std::uint32_t endMs = 0;
        bool ended = false;
        bool oneShot = false;     // created and played by the engine; never cut
        bool preexisting = false; // alive before its owner entered the recording
        bool follows = true;      // placed on the puppet every frame (no recorded position)
        std::uint64_t ref = 0;
        AudioBase base = AudioBase::Entity;
        std::uint16_t field = 0;   // AudioBase::Field: the entity offset of the pointer
        std::int32_t offset = 0;   // from the base (RVA for Image)
        std::uint32_t vtableRva = 0; // the audio entity's vtable when recorded (checked before use)
        std::uint32_t path[kPathDepth] = {};
        std::vector<std::uint8_t> params; // 0x140 bytes or none
    };

    inline std::uint32_t BaseSpan(AudioBase base)
    {
        switch (base)
        {
        case AudioBase::Entity: return 0x1000;
        case AudioBase::PedHolder: return 0x4820;
        case AudioBase::WeaponAudio: return 0x200; // 0x100 is short of the audio at +0x100
        case AudioBase::VehicleAudio: return kVehicleAudioSpan;
        case AudioBase::Image: return 0;
        case AudioBase::Field: return kFieldSpan;
        }
        return 0;
    }

    // What a clip may hold (checked on load and again before the offset
    // becomes a pointer): the bases the recorder makes, 8-byte aligned.
    inline bool GeneralSoundValid(const GeneralSound& sound, std::uint64_t imageSize, std::size_t paramsBytes)
    {
        if (!sound.ref || static_cast<std::uint8_t>(sound.base) >= kAudioBaseCount ||
            static_cast<std::uint8_t>(sound.cls) >= kOwnerClassCount || (sound.offset & 7) != 0 ||
            sound.endMs < sound.startMs || (!sound.params.empty() && sound.params.size() != paramsBytes) ||
            sound.entity == kNoOwner || sound.entity == 0)
            return false;
        if (!sound.vtableRva || (imageSize && sound.vtableRva >= imageSize))
            return false;
        if (sound.base == AudioBase::Image)
            return sound.offset > 0 && (imageSize == 0 || static_cast<std::uint64_t>(sound.offset) < imageSize) &&
                sound.field == 0;
        if (sound.base == AudioBase::Field)
            return sound.field < kFieldScanBytes && (sound.field & 7u) == 0 && sound.offset >= 0 &&
                static_cast<std::uint32_t>(sound.offset) < kFieldSpan;
        return sound.field == 0 && sound.offset >= 0 && static_cast<std::uint32_t>(sound.offset) < BaseSpan(sound.base);
    }

    // The owner classes captured by default: what has no other sound track
    // (a frozen vehicle or train puppet makes few sounds of its own). Peds,
    // animals, horses and weapons have their own tracks; capture=2 in the
    // ini (SoundCapture=2) takes them too, for the bench.
    inline bool CaptureClass(OwnerClass cls, int mode)
    {
        if (mode <= 0)
            return false;
        switch (cls)
        {
        case OwnerClass::Vehicle:
        case OwnerClass::Train:
        case OwnerClass::Object:
            return true;
        case OwnerClass::Ped:
        case OwnerClass::Animal:
        case OwnerClass::Horse:
        case OwnerClass::Player:
        case OwnerClass::Weapon:
            return mode >= 2;
        default:
            return false;
        }
    }

    // The persistent cores the entity sound track (ESND) already watches:
    // 0x251688D is the return in 0x251686C after the creator call, then
    // 0x2589214 (in the by-ref core 0x2589124) or 0x25890E7 (in the by-hash
    // core 0x2589058, through 0x2516808 which jumps to 0x251686C).
    inline bool PersistentCorePath(const std::uint32_t path[kPathDepth])
    {
        return path[0] == 0x251688Du && (path[1] == 0x2589214u || path[1] == 0x25890E7u);
    }

    // Learns where an audio entity hangs off its entity: the qword in the
    // entity's first `size` bytes that points closest below `audio` (within
    // kFieldSpan), 8-byte aligned, at or above the game heaps' floor.
    inline bool FindFieldBase(const std::uint8_t* entity, std::size_t size, std::uint64_t audio, std::uint16_t& field,
        std::int32_t& offset, std::uint64_t heapFloor = 0x100000000ull)
    {
        bool found = false;
        std::uint64_t best = 0;
        for (std::size_t at = 0; at + 8 <= size && at < kFieldScanBytes; at += 8)
        {
            std::uint64_t value = 0;
            std::memcpy(&value, entity + at, 8);
            if (value < heapFloor || (value & 7u) != 0 || value > audio || audio - value >= kFieldSpan)
                continue;
            const std::uint64_t delta = audio - value;
            if (!found || delta < best)
            {
                found = true;
                best = delta;
                field = static_cast<std::uint16_t>(at);
                offset = static_cast<std::int32_t>(delta);
            }
        }
        return found;
    }
    // A sound the engine requests again every frame (a train car asking for
    // the same ref once per frame, one-shots 16 ms apart) is one sound kept
    // alive, not one per request: played one by one they would take the 32
    // slots in half a second (32 stacked copies, then no_slot for the rest).
    // A run of one-shots of the same owner, ref and audio entity whose starts
    // follow each other within gapMs becomes one sound from the first start
    // to the last start plus gapMs, played once and kept (restarted only when
    // it ends inside the run), placed on the puppet every frame. Runs shorter
    // than minRun stay as they are. The order of the result is by start time.
    constexpr std::uint32_t kRepeatGapMs = 100;
    constexpr std::size_t kRepeatMinRun = 3;

    struct CoalesceStats
    {
        std::size_t runs = 0;   // runs merged
        std::size_t merged = 0; // one-shots that went into a run
    };

    inline CoalesceStats CoalesceRepeatedOneShots(std::vector<GeneralSound>& sounds, std::uint32_t gapMs = kRepeatGapMs,
        std::size_t minRun = kRepeatMinRun)
    {
        CoalesceStats stats;
        std::vector<GeneralSound> sorted = sounds;
        std::stable_sort(sorted.begin(), sorted.end(),
            [](const GeneralSound& a, const GeneralSound& b) { return a.startMs < b.startMs; });
        const auto sameSource = [](const GeneralSound& a, const GeneralSound& b) {
            return a.entity == b.entity && a.ref == b.ref && a.base == b.base && a.field == b.field && a.offset == b.offset;
        };
        std::vector<bool> taken(sorted.size(), false);
        std::vector<GeneralSound> out;
        out.reserve(sorted.size());
        for (std::size_t first = 0; first < sorted.size(); ++first)
        {
            if (taken[first])
                continue;
            taken[first] = true;
            GeneralSound head = sorted[first];
            if (!head.oneShot)
            {
                out.push_back(head);
                continue;
            }
            std::vector<std::size_t> run{first};
            std::uint32_t lastStart = head.startMs;
            for (std::size_t next = first + 1; next < sorted.size(); ++next)
            {
                if (sorted[next].startMs > lastStart + gapMs)
                    break;
                if (taken[next] || !sorted[next].oneShot || !sameSource(head, sorted[next]))
                    continue;
                run.push_back(next);
                lastStart = sorted[next].startMs;
            }
            if (run.size() < minRun)
            {
                out.push_back(head);
                continue;
            }
            for (const std::size_t index : run)
                taken[index] = true;
            head.oneShot = false;
            head.ended = true;
            head.endMs = lastStart + gapMs;
            out.push_back(head);
            ++stats.runs;
            stats.merged += run.size();
        }
        std::stable_sort(out.begin(), out.end(),
            [](const GeneralSound& a, const GeneralSound& b) { return a.startMs < b.startMs; });
        sounds.swap(out);
        return stats;
    }
}
