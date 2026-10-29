// Audio wind in the replay (wind_replay_core.h): the gate's sites and
// decision, the track (sampling, pairing, ring cut), the comparison and its
// log lines.
#include "wind_replay_core.h"
#include "soundfx_probe_core.h"

#include <cmath>
#include <cstdio>
#include <set>
#include <string>

namespace
{
    using namespace replay::windreplay;

    int g_windFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_windFailures;
    }

    bool Has(const std::string& line, const char* part) { return line.find(part) != std::string::npos; }

    void TestSites()
    {
        std::set<std::string> names;
        for (const Site& site : kSites)
        {
            Check(site.size > 0 && site.size <= sizeof(site.bytes), "site size");
            Check(names.insert(site.name).second, "site names are unique");
            // Nothing of the gate's prologue (hooked by this module) is a
            // site: once the detour is in, those bytes are its jump.
            Check(site.rva + site.size <= kGateRva || site.rva >= kGateRva + sizeof(kGateSignature), "no site on the hooked prologue");
        }
        Check(SiteIndex("gate_call") < kSiteCount && SiteIndex("simple_stamp") < kSiteCount, "site lookup");
        Check(sizeof(kGateSignature) >= 5, "the signature covers the detour's jump");
        Check(kWeatherGateReturnRva == kWeatherGateCallRva + 5, "return address after the call");
        Check(kWeatherRva + kWindLinePrev == 0x5C95104u && kWeatherRva + kWindLineNext == 0x5C95108u,
            "wind rows' weather index (0x31CF393)");
        Check(kWeatherRva + kWindStrength == 0x5C94FB4u, "wind strength W+0x4F44");
        Check(kWeatherRva + kSimpleStamp == 0x5C95114u, "simple path stamp W+0x50A4");
        // The gate is not one of the sound ports any detour of the project
        // hooks (two detours on one prologue would chain on MinHook's terms).
        for (const auto& port : replay::soundfx::kPorts)
            Check(port.rva != kGateRva, "the gate is not a sound port");
        // Bytes the site table holds match the game binary (spot check).
        const Site& call = kSites[SiteIndex("gate_call")];
        Check(call.bytes[7] == 0xE8 && call.bytes[12] == 0x84 && call.bytes[13] == 0xC0 && call.bytes[14] == 0x0F &&
                call.bytes[15] == 0x84,
            "call; test al,al; je");
    }

    void TestGate()
    {
        Check(!SceneFresh(0, 1000), "no scene published");
        Check(SceneFresh(1000, 1000) && SceneFresh(1000, 1000 + kSceneStaleMs), "fresh scene");
        Check(!SceneFresh(1000, 1001 + kSceneStaleMs), "stale scene");
        Check(!SceneFresh(2000, 1000), "a clock that went back is not fresh");
        Check(AskScene(kWeatherGateReturnRva, 5000, 5100), "the weather pre-update's call asks about the scene");
        Check(!AskScene(kOtherGateReturnRva, 5000, 5100), "the other audio update keeps the live player");
        Check(!AskScene(0x1363C9Cu, 5000, 5100), "any other caller keeps its own position");
        Check(!AskScene(kWeatherGateReturnRva, 0, 5100), "outside the replay: the live player");
        Check(std::string(SceneSourceName(SceneSource::Puppet)) == "puppet" &&
                std::string(SceneSourceName(SceneSource::Camera)) == "camera" &&
                std::string(SceneSourceName(SceneSource::Focus)) == "focus" &&
                std::string(SceneSourceName(SceneSource::None)) == "none",
            "scene source names");
        GateCounts a;
        a.sceneCalls = 10;
        a.sceneUnloaded = 1;
        GateCounts b;
        b.liveCalls = 3;
        b.liveUnloaded = 3;
        a.Add(b);
        Check(GateText(a) == "scene:10/1,live:3/3", "gate text");
    }

    Sample S(std::uint32_t t, float strength, std::uint32_t l0 = 0, std::uint32_t l1 = 0, std::uint8_t flags = kSampleLines)
    {
        Sample s;
        s.timeMs = t;
        s.strength = strength;
        s.lines[0] = l0;
        s.lines[1] = l1;
        s.flags = flags;
        return s;
    }

    void TestTrack()
    {
        Track t;
        Check(SampleDue(t, 0), "first sample due");
        Append(t, S(0, 0.6f));
        Check(!SampleDue(t, 100) && SampleDue(t, kSamplePeriodMs), "one sample per period");
        Append(t, S(250, 0.62f));
        Append(t, S(500, 0.64f));
        Append(t, S(750, 0.66f));
        Check(SampleDue(t, 400), "a clock that went back is due");
        Append(t, S(400, 0.5f)); // rewound: the newer tail goes
        Check(t.samples.size() == 3 && t.samples.back().timeMs == 400, "append drops the newer tail");
        Append(t, S(650, 0.55f));
        Check(At(t, 0) && At(t, 0)->timeMs == 0, "pair at 0");
        Check(At(t, 300) && At(t, 300)->timeMs == 250, "nearest: 250 for 300");
        Check(At(t, 560) && At(t, 560)->timeMs == 650, "nearest: 650 for 560");
        Check(At(t, 650 + kMatchMs) != nullptr && At(t, 651 + kMatchMs) == nullptr, "nothing paired past the match window");
        Track empty;
        Check(At(empty, 0) == nullptr, "empty track pairs nothing");
        // Ring buffer and window cut.
        Track cut;
        t.CutInto(cut, 250, 650);
        Check(cut.samples.size() == 3 && cut.samples[0].timeMs == 0 && cut.samples[2].timeMs == 400, "cut moves by base");
        t.DropBefore(400);
        Check(t.samples.size() == 2 && t.samples[0].timeMs == 400, "drop before");
        Check((kSampleFlagsKnown & kSampleSimple) && (kSampleFlagsKnown & kSampleLines), "known flags");
    }

    void TestNames()
    {
        Check(WeatherText(0x27EA2814u) == "BLIZZARD" && WeatherText(0x7F622122u) == "GROUNDBLIZZARD" &&
                WeatherText(0x23FB812Bu) == "SNOWLIGHT",
            "the clip 1150 weathers by name");
        Check(WeatherText(0x12345678u) == "0x12345678" && WeatherText(0) == "none", "unknown weather in hex");
        const std::uint32_t lines[2] = {0x27EA2814u, 0x7F622122u};
        Check(LinesText(lines, true) == "BLIZZARD;GROUNDBLIZZARD" && LinesText(lines, false) == "none", "lines text");
    }

    void TestCompare()
    {
        Check(Agree(0.07f, 0.09f) && Agree(0.60f, 0.70f) && !Agree(0.075f, 0.731f) && !Agree(0.438f, 0.886f),
            "the old measure disagrees, small differences agree");
        // A quiet stretch 2.5 times as loud (+8 dB) does not agree; small
        // differences between two quiet stretches on the normal path do.
        Check(!Agree(0.04f, 0.10f) && !Agree(0.03f, 0.08f) && Agree(0.058f, 0.039f) && Agree(0.040f, 0.033f),
            "quiet stretches: absolute tolerance 0.03");
        Check(!Agree(0.5f, NAN) && !Agree(0.0f, 0.2f), "nan and zero");
        Check(std::fabs(RatioDb(0.1f, 1.0f) - 20.0f) < 0.01f && std::fabs(RatioDb(0.0f, 0.0f)) < 0.01f, "ratio dB");

        // Frozen blizzard rows while the live weather is light snow.
        const std::uint32_t snow = 0x23FB812Bu, blizzard = 0x27EA2814u;
        const std::uint32_t wx[2] = {snow, snow};
        Block b;
        const Sample r1 = S(60000, 0.075f, snow, snow, kSampleLines);
        const Sample r2 = S(60250, 0.070f, snow, snow, kSampleLines);
        b.Add(S(60000, 0.731f, blizzard, blizzard, kSampleLines | kSampleSimple), &r1, wx);
        b.Add(S(60250, 0.729f, blizzard, blizzard, kSampleLines | kSampleSimple), &r2, wx);
        b.Add(S(60500, 0.728f, blizzard, blizzard, kSampleLines | kSampleSimple), nullptr, wx);
        Check(b.samples == 3 && b.matched == 2 && b.simplePlay == 3 && b.simpleRec == 0, "block counts");
        Check(std::fabs(b.RecMean() - 0.0725f) < 1e-4f && std::fabs(b.PlayMatchedMean() - 0.730f) < 1e-4f, "block means");
        Check(!b.Agrees() && b.linesCompared == 2 && b.linesSame == 0, "frozen rows: disagree, other weather");
        const std::string line = BlockLine(b, true, GateCounts{}, SceneSource::Puppet);
        Check(Has(line, "WIND_REPLAY,phase=play,from_ms=60000,to_ms=60500,samples=3") && Has(line, "rec_str=0.073") &&
                Has(line, "play_str=0.730") && Has(line, "agree=0") && Has(line, "rec_lines=SNOWLIGHT;SNOWLIGHT") &&
                Has(line, "play_lines=BLIZZARD;BLIZZARD") && Has(line, "wx=SNOWLIGHT;SNOWLIGHT") &&
                Has(line, "simple_play=3") && Has(line, "scene=puppet"),
            "block line");
        // A clip without the WNDA section.
        const std::string old = BlockLine(b, false, GateCounts{}, SceneSource::Camera);
        Check(Has(old, "rec_str=none") && Has(old, "rec_lines=none") && Has(old, "scene=camera"), "block line without WNDA");

        // Rows that follow the weather: the level agrees.
        Block c;
        c.Add(S(60000, 0.081f, snow, snow), &r1, wx);
        c.Add(S(60250, 0.066f, snow, snow), &r2, wx);
        Check(c.Agrees() && c.linesSame == 2, "fixed: agree, same weather");

        Totals t;
        t.AddPair(0.075f, 0.731f);
        t.AddPair(0.070f, 0.729f);
        t.AddBlock(b);
        t.AddBlock(c);
        t.AddBlock(Block{});
        Check(t.blocks == 2 && t.blocksMatched == 2 && t.blocksAgree == 1 && t.samples == 5 && t.matched == 4, "totals");
        Check(t.maxAtMs == 60000 && std::fabs(t.maxPlay - 0.730f) < 1e-4f, "worst block");
        GateCounts g;
        g.sceneCalls = 300;
        const std::string total = TotalLine(t, true, 400, true, g);
        Check(Has(total, "WIND_REPLAY_TOTAL,fix=1,recorded=1,rec_samples=400,samples=5,paired=4,blocks=2") &&
                Has(total, "blocks_agree=1/2") && Has(total, "worst_block_ms=60000") && Has(total, "lines_same=2/4") &&
                Has(total, "gate=scene:300/0,live:0/0"),
            "total line");
        const std::string totalOld = TotalLine(t, false, 0, true, g);
        Check(Has(totalOld, "recorded=0") && Has(totalOld, "play_mean=") && !Has(totalOld, "blocks_agree"), "total without WNDA");
    }
}

int RunWindReplayTests()
{
    TestSites();
    TestGate();
    TestTrack();
    TestNames();
    TestCompare();
    if (g_windFailures == 0)
        std::printf("wind replay tests passed\n");
    return g_windFailures;
}
