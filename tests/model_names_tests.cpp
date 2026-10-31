// The editor's entity labels (src/model_names.h): the game's model name by
// joaat hash, the hash in hex when unknown, a number for a shared model.
#include "model_names.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
    int g_failures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }

    // GET_HASH_KEY: joaat of the lower case name.
    std::uint32_t Joaat(const char* text)
    {
        std::uint32_t h = 0;
        for (; *text; ++text)
        {
            const char c = *text >= 'A' && *text <= 'Z' ? static_cast<char>(*text - 'A' + 'a') : *text;
            h += static_cast<std::uint8_t>(c);
            h += h << 10;
            h ^= h >> 6;
        }
        h += h << 3;
        h ^= h >> 11;
        return h + (h << 15);
    }

    std::string LabelOf(std::uint32_t hash, int number, std::size_t size = 64)
    {
        char out[128] = {};
        replay::models::Label(hash, number, out, size);
        return out;
    }

    void TestTable()
    {
        bool sorted = true;
        bool hashed = true;
        for (std::size_t n = 0; n < std::size(replay::models::kModelNames); ++n)
        {
            const replay::models::ModelName& entry = replay::models::kModelNames[n];
            sorted = sorted && (n == 0 || replay::models::kModelNames[n - 1].hash < entry.hash);
            hashed = hashed && Joaat(entry.name) == entry.hash;
        }
        Check(std::size(replay::models::kModelNames) > 1000, "the table has the entity models");
        Check(sorted, "the table is sorted by hash, no repeats (the lookup is a binary search)");
        Check(hashed, "every name hashes (joaat) to its hash");
    }

    void TestNames()
    {
        Check(LabelOf(0x0D7114C9u, 0) == "player_zero", "Arthur is player_zero, not Arthur");
        Check(LabelOf(Joaat("player_three"), 0) == "player_three", "John is player_three, not John");
        Check(LabelOf(Joaat("cs_johnmarston"), 0) == "cs_johnmarston", "a story character by its model");
        Check(LabelOf(Joaat("a_m_m_valtownfolk_01"), 0) == "a_m_m_valtownfolk_01", "an ambient ped by its model");
        Check(LabelOf(Joaat("a_c_horse_kentuckysaddle_grey"), 0) == "a_c_horse_kentuckysaddle_grey", "a horse by its model");
        Check(LabelOf(Joaat("w_repeater_carbine01"), 0) == "w_repeater_carbine01", "a weapon by its model");
        Check(replay::models::NameOf(Joaat("p_hammer01x")) == nullptr, "props are not in the table");
    }

    void TestLabels()
    {
        Check(LabelOf(0x1A2B3C4Du, 0) == "0x1A2B3C4D", "an unknown model: the hash in hex");
        Check(LabelOf(0u, 0) == "0x00000000", "no model: the hash in hex");
        Check(LabelOf(Joaat("a_m_m_valtownfolk_01"), 2) == "a_m_m_valtownfolk_01 #2", "a shared model: its number");
        Check(LabelOf(0xABCDEF01u, 3) == "0xABCDEF01 #3", "an unknown shared model: hex and number");
        Check(LabelOf(Joaat("a_m_m_valtownfolk_01"), 0, 8) == "a_m_m_v", "cut to the room, terminated");
        char untouched[4] = {'x', 'y', 'z', '\0'};
        replay::models::Label(0x0D7114C9u, 0, untouched, 0);
        Check(std::strcmp(untouched, "xyz") == 0, "no room: nothing written");
    }
}

int RunModelNamesTests()
{
    g_failures = 0;
    TestTable();
    TestNames();
    TestLabels();
    std::printf("model names tests: %s (%d failures)\n", g_failures ? "FAILED" : "ok", g_failures);
    return g_failures;
}
