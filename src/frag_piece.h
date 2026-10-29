#pragma once

// A loose piece of a fragment object (the board that came off a crate) is
// a new object of the whole object's model; the engine's piece carries
// only the groups that broke off (the PartsBrokeOff 0x6EAE68 bitset; its
// own cache entry has the broken group as its root). Built from the model
// alone, a whole crate would follow the board's track. So the piece keeps
// the groups it carries (section FPCE) and playback breaks a piece off a
// whole object of the model the way the engine made it (its root group,
// BreakOffAboveGroup 0x2965F2C) and then takes off the groups under that
// root it no longer had. Pure rules; the engine side is frag_observer.h
// (ReadObjectFrag, BreakOffObjectGroup) and replay_objects.cpp.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace replay::fragpiece
{
    // The engine's group bitset is 0x80 bytes.
    constexpr int kMaxGroups = 0x80 * 8;
    // Load limit: rows per clip.
    constexpr std::size_t kMaxRows = 4096;

    // Where the carried groups came from.
    enum class Source : std::uint8_t
    {
        Break = 0, // the PartsBrokeOff call that made the piece (groups that left the parent)
        Cache = 1, // the piece's own cache entry when registered (root and groups still present)
    };
    constexpr std::uint8_t kSourceMax = 1;

    inline const char* SourceText(std::uint8_t source)
    {
        return source == static_cast<std::uint8_t>(Source::Break) ? "break"
            : source == static_cast<std::uint8_t>(Source::Cache) ? "cache" : "?";
    }

    // The groups set in an engine bitset, ascending.
    inline std::vector<std::uint16_t> GroupsOfBits(const std::uint8_t* bits, std::size_t bytes)
    {
        std::vector<std::uint16_t> out;
        if (!bits)
            return out;
        const std::size_t limit = bytes * 8 < static_cast<std::size_t>(kMaxGroups) ? bytes * 8 : kMaxGroups;
        for (std::size_t bit = 0; bit < limit; ++bit)
            if ((bits[bit / 8] >> (bit % 8)) & 1)
                out.push_back(static_cast<std::uint16_t>(bit));
        return out;
    }

    // A recorded set: not empty, strictly ascending, inside the bitset.
    inline bool ValidGroups(const std::vector<std::uint16_t>& groups)
    {
        if (groups.empty() || groups.size() > static_cast<std::size_t>(kMaxGroups))
            return false;
        for (std::size_t n = 0; n < groups.size(); ++n)
            if (groups[n] >= kMaxGroups || (n && groups[n] <= groups[n - 1]))
                return false;
        return true;
    }

    // The piece's root: its lowest group (a parent's index is always lower
    // than its children's); -1 when empty.
    inline int Root(const std::vector<std::uint16_t>& groups) { return groups.empty() ? -1 : groups.front(); }

    inline bool Has(const std::vector<std::uint16_t>& groups, int group)
    {
        for (std::uint16_t g : groups)
            if (g == group)
                return true;
        return false;
    }

    // Which groups of the fragType lie under `root` (root included), from
    // the parent table (parents[g], -1 for the type's root). A parent that is
    // not lower than its child is malformed and ends the branch.
    inline std::vector<std::uint8_t> Subtree(const std::int16_t* parents, int groupCount, int root)
    {
        std::vector<std::uint8_t> in(groupCount > 0 ? static_cast<std::size_t>(groupCount) : 0, 0);
        if (!parents || root < 0 || root >= groupCount)
            return in;
        in[static_cast<std::size_t>(root)] = 1;
        for (int g = root + 1; g < groupCount; ++g)
        {
            const int parent = parents[g];
            if (parent >= root && parent < g && in[static_cast<std::size_t>(parent)])
                in[static_cast<std::size_t>(g)] = 1;
        }
        return in;
    }

    // `group` lies under `ancestor` (not itself) in the parent table; a
    // parent that is not lower than its child ends the walk (Subtree).
    inline bool Under(const std::int16_t* parents, int groupCount, int group, int ancestor)
    {
        if (!parents || group <= 0 || group >= groupCount)
            return false;
        for (int child = group, walk = parents[group]; walk >= 0 && walk < child; child = walk, walk = parents[walk])
            if (walk == ancestor)
                return true;
        return false;
    }

    // Two pieces of one object due together: the other piece (its groups
    // `theirs`) came off before this one (`mine`) in the recording when it
    // carried this piece's root (this piece then breaks off it), or when its
    // root lies under this piece's root and this piece did not carry it (it
    // left the object first). Breaking this root first in that case would
    // take the other root along.
    inline bool CameOffBefore(const std::int16_t* parents, int groupCount, const std::vector<std::uint16_t>& mine,
        const std::vector<std::uint16_t>& theirs)
    {
        const int root = Root(mine);
        const int other = Root(theirs);
        if (root < 0 || other < 0 || root == other || root >= groupCount || other >= groupCount)
            return false;
        return (Under(parents, groupCount, root, other) && Has(theirs, root)) ||
            (Under(parents, groupCount, other, root) && !Has(mine, other));
    }

    // How a whole object of the model becomes the recorded piece.
    struct Plan
    {
        bool ok = false;
        int root = -1;
        // The group to break off as the new object: the root, or -1 when the
        // piece holds the type's root (group 0: the object itself stays).
        int breakOff = -1;
        // Then taken off the piece, ascending: the topmost groups under the
        // root it does not carry (each goes with what is under it).
        std::vector<std::uint16_t> remove;
        // Carried groups that are not under the root (not one subtree): left
        // out, counted for the log.
        int stray = 0;
    };

    inline Plan PlanPiece(const std::int16_t* parents, int groupCount, const std::vector<std::uint16_t>& carried)
    {
        Plan plan;
        if (!parents || groupCount <= 0 || groupCount > kMaxGroups || !ValidGroups(carried))
            return plan;
        plan.root = Root(carried);
        if (plan.root >= groupCount)
            return plan;
        const std::vector<std::uint8_t> under = Subtree(parents, groupCount, plan.root);
        std::vector<std::uint8_t> kept(static_cast<std::size_t>(groupCount), 0);
        for (std::uint16_t g : carried)
        {
            if (g < groupCount && under[g])
                kept[g] = 1;
            else
                ++plan.stray;
        }
        for (int g = plan.root + 1; g < groupCount; ++g)
        {
            if (!under[static_cast<std::size_t>(g)] || kept[static_cast<std::size_t>(g)])
                continue;
            if (kept[static_cast<std::size_t>(parents[g])])
                plan.remove.push_back(static_cast<std::uint16_t>(g));
        }
        plan.breakOff = plan.root > 0 ? plan.root : -1;
        plan.ok = true;
        return plan;
    }

    // The groups a piece carries, read from its own cache entry: its root
    // and every group under it still present (gone: bit g set when the
    // group's slot is -1). Empty when the root is out of range or gone.
    inline std::vector<std::uint16_t> CarriedFromCache(const std::int16_t* parents, int groupCount, int root,
        const std::uint8_t* gone)
    {
        std::vector<std::uint16_t> out;
        if (!parents || !gone || groupCount <= 0 || groupCount > kMaxGroups || root < 0 || root >= groupCount ||
            ((gone[root / 8] >> (root % 8)) & 1))
            return out;
        std::vector<std::uint8_t> present(static_cast<std::size_t>(groupCount), 0);
        present[static_cast<std::size_t>(root)] = 1;
        out.push_back(static_cast<std::uint16_t>(root));
        for (int g = root + 1; g < groupCount; ++g)
        {
            const int parent = parents[g];
            if (parent < root || parent >= g || !present[static_cast<std::size_t>(parent)] || ((gone[g / 8] >> (g % 8)) & 1))
                continue;
            present[static_cast<std::size_t>(g)] = 1;
            out.push_back(static_cast<std::uint16_t>(g));
        }
        return out;
    }
}
