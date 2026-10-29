#pragma once

// Bench scenario text (pure data, unit tested in ReplayCoreTests).
//
// A scenario is one step per line: an op and its whitespace-separated
// arguments; '#' starts a comment. The parser checks every op name and its
// argument count up front, so a scenario never stops halfway on a typo.

#include <cstddef>
#include <string>
#include <vector>

namespace replay::bench
{
    struct Step
    {
        int line = 0;
        std::string op;
        std::vector<std::string> args;
    };

    struct Script
    {
        std::vector<Step> steps;
    };

    // False with a "line N: ..." message on the first bad line.
    bool ParseScript(const std::string& text, Script& out, std::string& error);

    bool ArgFloat(const Step& step, std::size_t index, float& value);
    bool ArgInt(const Step& step, std::size_t index, int& value);
}
