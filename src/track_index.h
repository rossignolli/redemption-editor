#pragma once

// Lookup of a per entity track by its packed ReplayId without scanning the
// whole history each time. The recording keeps one track per entity ever seen
// (vectors in Recording, appended to, never reordered while recording); a
// linear find per active entity would cost more with every id the buffer has
// seen while only a few entities are near.
//
// The index maps id -> position and follows the vector it was built on:
// entries appended since the last call are indexed then; anything else (the
// vector cleared, cut, replaced or reordered) is caught by checking the entry
// found and the first and last indexed ids, and the index is rebuilt. A found
// position is always checked against the entry's id, so a stale index can cost
// a rebuild, never a wrong track. Pure (unit tested in ReplayCoreTests).

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace replay
{
    template <class Track>
    class TrackIndex
    {
    public:
        // The entry of `tracks` with .id == id, or null.
        Track* Find(std::vector<Track>& tracks, std::uint32_t id)
        {
            Sync(tracks);
            const auto found = positions_.find(id);
            if (found != positions_.end())
            {
                if (found->second < tracks.size() && tracks[found->second].id == id)
                    return &tracks[found->second];
                Rebuild(tracks); // moved under us: once more from scratch
                const auto again = positions_.find(id);
                return again != positions_.end() ? &tracks[again->second] : nullptr;
            }
            return nullptr;
        }

        std::size_t Rebuilds() const { return rebuilds_; }

    private:
        void Sync(const std::vector<Track>& tracks)
        {
            const bool same = indexed_ <= tracks.size() &&
                (indexed_ == 0 || (tracks[0].id == firstId_ && tracks[indexed_ - 1].id == lastId_));
            if (!same)
            {
                Rebuild(tracks);
                return;
            }
            for (; indexed_ < tracks.size(); ++indexed_)
                positions_.emplace(tracks[indexed_].id, indexed_); // the first entry of an id wins, like find_if
            Mark(tracks);
        }

        void Rebuild(const std::vector<Track>& tracks)
        {
            ++rebuilds_;
            positions_.clear();
            indexed_ = 0;
            for (; indexed_ < tracks.size(); ++indexed_)
                positions_.emplace(tracks[indexed_].id, indexed_);
            Mark(tracks);
        }

        void Mark(const std::vector<Track>& tracks)
        {
            firstId_ = tracks.empty() ? 0 : tracks.front().id;
            lastId_ = tracks.empty() ? 0 : tracks.back().id;
        }

        std::unordered_map<std::uint32_t, std::size_t> positions_;
        std::size_t indexed_ = 0;
        std::uint32_t firstId_ = 0;
        std::uint32_t lastId_ = 0;
        std::size_t rebuilds_ = 0;
    };
}
