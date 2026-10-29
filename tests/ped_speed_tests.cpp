// The puppets' PED_VARIABLES speed (ped_speed_core.h): the recorded track
// (note, lookup, ring, cut, validation), the motion fallback, the choice,
// the lines; the wrapper variable text of the volume probe and its sites.
#include "ped_speed_core.h"
#include "ped_audio_state.h"
#include "volume_probe_core.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    using namespace replay::pedspeed;

    int g_pedSpeedFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_pedSpeedFailures;
    }

    bool Near(float a, float b, float tol = 1e-3f) { return std::fabs(a - b) <= tol; }
    bool Has(const std::string& line, const char* part) { return line.find(part) != std::string::npos; }

    void TestConstants()
    {
        Check(kSpeedVariable == 0x8BAE9C31u, "speed is 0x8BAE9C31 (the immediate at 0x31A3F7A)");
        bool labelled = false;
        for (const auto& known : replay::pedaudiostate::kKnownVariables)
            labelled = labelled || (known.name == kSpeedVariable && std::string(known.label) == "speed");
        Check(labelled, "the PED_VARS label of the speed");
        Check(replay::pedaudiostate::kUpdatePedVariablesRva == 0x31A3ABC &&
                  replay::pedaudiostate::kSpeedStoreRva == 0x31A3F7A && replay::pedaudiostate::kSpeedSourceRva == 0x31A3EF4,
            "UpdatePedVariables and its speed sites");
        Check(replay::pedaudiostate::kMaxSpeedTargets >= 128, "room for every puppet");
        Check(Plausible(0.0f) && Plausible(9.2f) && !Plausible(-0.1f) && !Plausible(NAN) && !Plausible(1000.0f), "plausible");
    }

    void TestTrack()
    {
        Track t;
        Check(t.Note(0x10002u, 0, 6.6f) && t.Note(0x10002u, 250, 7.0f) && t.Note(0x10002u, 500, 8.0f), "note in order");
        Check(t.Note(0x00001u, 100, 1.0f), "a smaller owner goes first");
        Check(t.series.size() == 2 && t.series[0].owner == 0x00001u && t.series[1].owner == 0x10002u, "owners ascending");
        Check(!t.Note(0x10002u, 750, NAN) && !t.Note(0x10002u, 750, -1.0f), "implausible refused");
        // A clock that went back drops the newer tail.
        Check(t.Note(0x10002u, 400, 7.5f), "rewound note");
        Check(t.Find(0x10002u)->points.size() == 3 && t.Find(0x10002u)->points[2].timeMs == 400, "tail dropped");
        Check(t.Points() == 4 && t.Valid(), "points and valid");

        float v = 0.0f;
        Check(t.At(0x10002u, 125, v) && Near(v, 6.8f), "linear between points");
        Check(t.At(0x10002u, 0, v) && Near(v, 6.6f), "at a point");
        Check(t.At(0x10002u, 400 + kHoldMs, v) && Near(v, 7.5f), "held after the last within kHoldMs");
        Check(!t.At(0x10002u, 401 + kHoldMs, v), "not past kHoldMs");
        Check(!t.At(0x99999u, 0, v), "unknown owner");
        Track late;
        late.Note(5, 2000, 3.0f);
        Check(late.At(5, 2000 - kHoldMs, v) && Near(v, 3.0f) && !late.At(5, 1000, v), "before the first: within kHoldMs");
        // A hole longer than 2 * kHoldMs (the owner not scanned) is not bridged.
        Track hole;
        hole.Note(7, 0, 2.0f);
        hole.Note(7, 10000, 8.0f);
        Check(hole.At(7, 500, v) && Near(v, 2.0f), "hole: the point before holds");
        Check(hole.At(7, 9500, v) && Near(v, 8.0f), "hole: the point after holds");
        Check(!hole.At(7, 5000, v), "hole: nothing in the middle");

        // The ring keeps the value in force at keepFrom.
        Track ring;
        for (std::uint32_t ms = 0; ms <= 3000; ms += 250)
            ring.Note(9, ms, static_cast<float>(ms) / 1000.0f);
        ring.Note(11, 100, 4.0f);
        ring.DropBefore(1100);
        Check(ring.Find(9)->points.front().timeMs == 1000 && ring.Find(9)->points.size() == 9, "ring: one before keepFrom stays");
        // Owner 11's last point (100) is more than kHoldMs before keepFrom
        // (1100): nothing reads it again, it goes.
        Check(ring.Find(11) == nullptr, "ring: an owner silent for longer than kHoldMs before keepFrom goes");
        {
            Track held;
            held.Note(21, 1100 - kHoldMs, 2.0f); // exactly kHoldMs before keepFrom: still in force
            held.Note(22, 1099 - kHoldMs, 3.0f); // one ms more: gone
            held.Note(23, 200, 1.0f);
            held.Note(23, 1050, 5.0f); // the value in force at keepFrom
            held.DropBefore(1100);
            Check(held.Find(21) && held.Find(21)->points.size() == 1, "ring: an owner's last value within kHoldMs stays");
            Check(held.Find(22) == nullptr, "ring: past kHoldMs it goes");
            Check(held.Find(23) && held.Find(23)->points.size() == 1 && held.Find(23)->points[0].timeMs == 1050,
                "ring: the value in force stays");
            float at = 0.0f;
            Check(held.At(21, 1100, at) && Near(at, 2.0f), "ring: the kept value still answers at keepFrom");
            // The cap is given back: a ring that saw kMaxOwners owners takes new ones once they went.
            Track full;
            for (std::uint32_t owner = 1; owner <= kMaxOwners; ++owner)
                full.Note(owner, 0, 1.0f);
            Check(full.series.size() == kMaxOwners && !full.Note(static_cast<std::uint32_t>(kMaxOwners + 1), 10, 1.0f), "ring: kMaxOwners");
            full.DropBefore(10000);
            Check(full.Empty() && full.Note(static_cast<std::uint32_t>(kMaxOwners + 1), 10000, 1.0f), "ring: the owners' cap given back");
        }
        ring.DropBefore(1100);
        Check(ring.Find(9)->points.front().timeMs == 1000, "ring: idempotent");

        // A saved window: kept owners, the value at base moved to 0.
        Track cut;
        ring.CutInto(cut, 1125, 2000, [](std::uint32_t owner) { return owner == 9; });
        Check(cut.series.size() == 1 && cut.series[0].owner == 9, "cut: kept owners only");
        Check(cut.series[0].points.front().timeMs == 0 && Near(cut.series[0].points.front().speed, 1.125f), "cut: base interpolated at 0");
        Check(cut.series[0].points[1].timeMs == 125 && Near(cut.series[0].points[1].speed, 1.25f) &&
                  cut.series[0].points.back().timeMs == 875,
            "cut: shifted, end inclusive");
        Check(cut.Valid(), "cut is valid");
        Track cutAll;
        ring.CutInto(cutAll, 1000, 2000, [](std::uint32_t) { return true; });
        Check(cutAll.series.size() == 1 && cutAll.Find(9) && cutAll.Find(9)->points.front().timeMs == 0 &&
                  Near(cutAll.Find(9)->points.front().speed, 1.0f),
            "cut: a point at base is not doubled");
        Check(cutAll.Find(11) == nullptr, "cut: an owner silent for longer than kHoldMs before base is left out");

        // Validation of a loaded section.
        Track bad = t;
        bad.series[1].points[1].timeMs = bad.series[1].points[0].timeMs;
        Check(!bad.Valid(), "valid: times strictly ascending");
        bad = t;
        std::swap(bad.series[0], bad.series[1]);
        Check(!bad.Valid(), "valid: owners ascending");
        bad = t;
        bad.series[0].points[0].speed = 500.0f;
        Check(!bad.Valid(), "valid: plausible values");
        bad = t;
        bad.series[0].points.clear();
        Check(!bad.Valid(), "valid: no empty owner");
    }

    void TestMotion()
    {
        MotionEstimator m;
        float v = 0.0f;
        Check(!m.Speed(v), "motion: nothing yet");
        // 7 m/s along x, ticks of 16 ms.
        for (std::uint32_t ms = 1000; ms <= 1400; ms += 16)
            m.Note(ms, 7.0f * static_cast<float>(ms) / 1000.0f, 5.0f, 300.0f);
        Check(m.Speed(v) && Near(v, 7.0f, 0.01f), "motion: steady speed");
        // The same tick again replaces the sample.
        m.Note(1400, 7.0f * 1.4f, 5.0f, 300.0f);
        Check(m.Speed(v) && Near(v, 7.0f, 0.01f), "motion: same tick");
        // A jump back (seek) starts over.
        m.Note(200, 0.0f, 0.0f, 0.0f);
        Check(!m.Speed(v), "motion: reset on a seek back");
        // A span under kMinSpanMs gives nothing.
        m.Note(250, 1.0f, 0.0f, 0.0f);
        Check(!m.Speed(v), "motion: too short a span");
        m.Note(310, 2.0f, 0.0f, 0.0f);
        Check(m.Speed(v) && Near(v, 2.0f / 0.11f, 0.05f), "motion: the longest span within the window");
        // A gap over kResetGapMs starts over.
        m.Note(310 + MotionEstimator::kResetGapMs + 1, 0.0f, 0.0f, 0.0f);
        Check(!m.Speed(v), "motion: reset on a gap");
        // A teleport is not a speed.
        MotionEstimator t;
        t.Note(0, 0.0f, 0.0f, 0.0f);
        t.Note(200, 1000.0f, 0.0f, 0.0f);
        Check(!t.Speed(v), "motion: implausible speed refused");
        MotionEstimator n;
        n.Note(0, 0.0f, 0.0f, 0.0f);
        n.Note(100, NAN, 0.0f, 0.0f);
        Check(n.count == 0, "motion: a bad position resets");

        // A warp starts over instead of becoming 20 to 150 m/s for up to
        // kWindowMs (a 30 m warp in one tick reads 120 m/s, under kMaxSpeed).
        MotionEstimator w;
        for (std::uint32_t ms = 0; ms <= 300; ms += 16)
            w.Note(ms, 7.0f * static_cast<float>(ms) / 1000.0f, 0.0f, 0.0f);
        Check(w.Speed(v) && Near(v, 7.0f, 0.05f), "warp: steady before");
        w.Note(316, 30.0f, 0.0f, 0.0f);
        Check(w.count == 1 && !w.Speed(v), "warp: a 28 m step in one tick starts over");
        for (std::uint32_t ms = 332; ms <= 450; ms += 16)
            w.Note(ms, 30.0f + 7.0f * static_cast<float>(ms - 316) / 1000.0f, 0.0f, 0.0f);
        Check(w.Speed(v) && Near(v, 7.0f, 0.05f), "warp: the speed after it, from the new place");
        // Real motion is no warp: a sprinting horse (15 m/s, 16 ms ticks), a
        // fall at 40 m/s with 33 ms ticks, 3 m in 100 ms (30 m/s).
        MotionEstimator fast;
        for (std::uint32_t ms = 0; ms <= 300; ms += 16)
            fast.Note(ms, 15.0f * static_cast<float>(ms) / 1000.0f, 0.0f, 0.0f);
        Check(fast.count == 19 && fast.Speed(v) && Near(v, 15.0f, 0.05f), "warp: a sprint is not one");
        MotionEstimator fall;
        for (std::uint32_t ms = 0; ms <= 330; ms += 33)
            fall.Note(ms, 0.0f, 0.0f, 100.0f - 40.0f * static_cast<float>(ms) / 1000.0f);
        Check(fall.count == 11 && fall.Speed(v) && Near(v, 40.0f, 0.1f), "warp: a fall is not one");
        MotionEstimator hop;
        hop.Note(0, 0.0f, 0.0f, 0.0f);
        hop.Note(100, 3.0f, 0.0f, 0.0f);
        Check(hop.count == 2 && hop.Speed(v) && Near(v, 30.0f, 0.05f), "warp: 3 m in 100 ms is motion");
        Check(MotionEstimator::IsWarp(MotionEstimator::Sample{0, 0.0f, 0.0f, 0.0f}, 40, 2.5f, 0.0f, 0.0f) &&
                  !MotionEstimator::IsWarp(MotionEstimator::Sample{0, 0.0f, 0.0f, 0.0f}, 60, 2.5f, 0.0f, 0.0f) &&
                  !MotionEstimator::IsWarp(MotionEstimator::Sample{0, 0.0f, 0.0f, 0.0f}, 1, 1.9f, 0.0f, 0.0f),
            "warp: farther than kWarpMetres and faster than kWarpSpeed");
    }

    void TestChoose()
    {
        Track t;
        t.Note(3, 0, 6.0f);
        t.Note(3, 250, 7.0f);
        MotionEstimator m;
        m.Note(0, 0.0f, 0.0f, 0.0f);
        m.Note(200, 1.0f, 0.0f, 0.0f);
        Target a = Choose(t, 3, 125, m);
        Check(a.source == Source::Section && Near(a.speed, 6.5f) && a.recKnown && a.derivedKnown && Near(a.derived, 5.0f),
            "choose: the section first");
        Target b = Choose(t, 4, 125, m);
        Check(b.source == Source::Derived && Near(b.speed, 5.0f) && !b.recKnown, "choose: an owner without points: the motion");
        MotionEstimator still;
        Target c = Choose(Track{}, 4, 125, still);
        Check(c.source == Source::None, "choose: none");
        Check(std::string(SourceName(Source::Section)) == "section" && std::string(SourceName(Source::Derived)) == "derived" &&
                  std::string(SourceName(Source::None)) == "none",
            "source names");
    }

    void TestSlots()
    {
        struct Op
        {
            char kind;
            std::uint32_t slot;
            std::uintptr_t holder;
            float value;
        };
        std::vector<Op> ops;
        SlotTable<4> table;
        const auto valid = [](std::uintptr_t h) { return h != 0xBAD; };
        const auto clear = [&ops](std::uint32_t slot) { ops.push_back({'c', slot, 0, 0.0f}); };
        const auto speed = [&ops](std::uint32_t slot, float v) { ops.push_back({'s', slot, 0, v}); };
        const auto set = [&ops](std::uint32_t slot, std::uintptr_t h, float v) { ops.push_back({'n', slot, h, v}); };
        {
            const std::uintptr_t h[] = {0xA0, 0xB0, 0xA0, 0xBAD, 0xC0};
            const float v[] = {7.0f, 3.0f, 9.0f, 1.0f, NAN};
            const std::uint32_t end = table.Apply(h, v, 5, valid, clear, speed, set);
            Check(end == 2 && ops.size() == 2 && ops[0].kind == 'n' && ops[0].slot == 0 && ops[0].holder == 0xA0 &&
                      ops[0].value == 7.0f && ops[1].kind == 'n' && ops[1].slot == 1 && ops[1].holder == 0xB0,
                "slots: new holders, a duplicate, an invalid holder and a bad speed left out");
        }
        ops.clear();
        {
            // A leaves, B stays with a new speed, C comes: A is cleared first, C takes A's slot.
            const std::uintptr_t h[] = {0xB0, 0xC0};
            const float v[] = {4.0f, 5.0f};
            const std::uint32_t end = table.Apply(h, v, 2, valid, clear, speed, set);
            Check(end == 2 && ops.size() == 3 && ops[0].kind == 's' && ops[0].slot == 1 && ops[0].value == 4.0f &&
                      ops[1].kind == 'c' && ops[1].slot == 0 && ops[2].kind == 'n' && ops[2].slot == 0 && ops[2].holder == 0xC0,
                "slots: a staying holder keeps its slot, a leaving one is cleared before its slot is reused");
            Check(table.SlotOf(0xB0) == 1 && table.SlotOf(0xC0) == 0 && table.SlotOf(0xA0) == 4, "slots: lookup");
        }
        ops.clear();
        {
            const std::uintptr_t h[] = {0xC0};
            const float v[] = {6.0f};
            Check(table.Apply(h, v, 1, valid, clear, speed, set) == 1 && ops.size() == 2 && ops[1].kind == 'c' && ops[1].slot == 1,
                "slots: the end shrinks");
        }
        ops.clear();
        {
            const std::uintptr_t h[] = {0x1, 0x2, 0x3, 0x4, 0x5};
            const float v[] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
            const std::uint32_t end = table.Apply(h, v, 5, valid, clear, speed, set);
            Check(end == 4 && table.SlotOf(0x5) == 4 && table.SlotOf(0xC0) == 4, "slots: full, the rest left to the engine");
        }
        ops.clear();
        Check(table.Apply(nullptr, nullptr, 0, valid, clear, speed, set) == 0 && ops.size() == 4, "slots: an empty list clears all");
    }

    void TestLines()
    {
        Track t;
        t.Note(3, 0, 7.21f);
        MotionEstimator m;
        const Target tg = Choose(t, 3, 0, m);
        const std::string line = OwnerLine(5000, "0:1", "horse", tg, true, 0.0f, true, 7.21f, true);
        Check(Has(line, "PED_SPEED_REPLAY,phase=play,clip_ms=5000,owner=0:1,kind=horse,source=section,published=1") &&
                  Has(line, "target=7.21,rec=7.21,derived=none,engine=0.00,block=7.21"),
            "owner line");
        Totals totals;
        totals.AddTarget(tg);
        totals.AddTarget(Target{});
        totals.AddCheck(tg, true, 0.0f, true, 7.21f);
        Target both = tg;
        both.derivedKnown = true;
        both.derived = 6.9f;
        totals.AddCheck(both, false, 0.0f, true, 5.0f);
        Check(totals.puppetTicks == 2 && totals.fromSection == 1 && totals.none == 1 && totals.checked == 2 &&
                  totals.agree == 1 && totals.both == 1,
            "totals");
        DetourCounts d;
        d.calls = 900;
        d.hits = 300;
        d.writes = 299;
        d.failed = 1;
        const std::string total = TotalLine(true, true, "", true, 12, 4800, totals, d);
        Check(Has(total, "PED_SPEED_REPLAY_TOTAL,enabled=1,installed=1,error=none,section=PSPD,rec_owners=12,rec_points=4800") &&
                  Has(total, "block_agree=1/2") && Has(total, "engine_mean=0.00") && Has(total, "derived_err_mean=0.31") &&
                  Has(total, "detour_calls=900,detour_hits=300,detour_writes=299,detour_failed=1"),
            "total line");
        const std::string off = TotalLine(false, false, "update_ped_variables_signature_mismatch", false, 0, 0, Totals{}, {});
        Check(Has(off, "enabled=0,installed=0,error=update_ped_variables_signature_mismatch,section=none") &&
                  Has(off, "engine_mean=none,derived_err_mean=none"),
            "total line, off");
        Check(Has(RecordLine(true, 30, 9000, 9000, 3), "PED_SPEED_RECORD_TOTAL,enabled=1,owners=30,points=9000,noted=9000,refused=3"),
            "record line");
    }

    void TestWrapperVar()
    {
        using namespace replay::volumeprobe;
        WrapperVar w;
        Check(WrapperVarText(w) == "-", "wvol: not a wrapper");
        w.home = VarHome::None;
        Check(WrapperVarText(w) == "none", "wvol: no volume variable");
        w.home = VarHome::Internal;
        w.ref = 23;
        Check(WrapperVarText(w) == "int:23", "wvol: internal");
        w.home = VarHome::Ped;
        w.owner = "0:1";
        w.name = kSpeedVariable;
        w.label = "speed";
        w.value = 7.214f;
        Check(WrapperVarText(w) == "ped:0:1:speed=7.21", "wvol: a PED_VARIABLES float");
        w.label = nullptr;
        Check(WrapperVarText(w) == "ped:0:1:0x8bae9c31=7.21", "wvol: unlabelled");
        w.home = VarHome::Pair;
        w.name = 0x01401A30u;
        w.value = 0.55f;
        Check(WrapperVarText(w) == "pair:0x01401a30=0.55", "wvol: the sound's own variable");
        w.home = VarHome::Unreadable;
        Check(WrapperVarText(w) == "unreadable", "wvol: unreadable");
        // A float* outside the scanned blocks and the sound's own pairs has no
        // name.
        w.home = VarHome::Other;
        w.value = 3.07f;
        Check(WrapperVarText(w) == "other=3.07", "wvol: other");
        // The pairs of a slot: the value of pair n is at slot + 8 n + 4.
        Check(PairSlotHit(0x1000, 3, 0x1004) && PairSlotHit(0x1000, 3, 0x1014), "pair: values of pairs 0 and 2");
        Check(!PairSlotHit(0x1000, 3, 0x101C), "pair: past the count");
        Check(!PairSlotHit(0x1000, 3, 0x1000) && !PairSlotHit(0x1000, 3, 0x1008), "pair: a name is not a value");
        Check(!PairSlotHit(0x1000, 3, 0xFFC) && !PairSlotHit(0, 3, 0x4), "pair: before the slot, no slot");
        Check(!PairSlotHit(0x1000, 0, 0x1004) && !PairSlotHit(0x1000, kMaxVarPairs + 1, 0x1004), "pair: count bounds");
        Check(PairSlotHit(0x1000, kMaxVarPairs, 0x1000 + kVarPairCount - 4), "pair: the last pair before the count");
        // The sites of the wrapper part and the RIP target of its vtable.
        std::size_t wrapperSites = 0;
        for (const Site& site : kSites)
        {
            if (site.part == Part::Wrapper)
                ++wrapperSites;
            // No probe site on the prologue the speed override hooks.
            Check(site.rva + site.size <= replay::pedaudiostate::kUpdatePedVariablesRva ||
                      site.rva >= replay::pedaudiostate::kUpdatePedVariablesRva + 5u,
                (std::string("site on UpdatePedVariables' prologue: ") + site.name).c_str());
        }
        Check(wrapperSites == 15, "fifteen wrapper sites (eight, and the seven of the pairs' slot)");
        const Site& vt = kSites[SiteIndex("wrapper_vtable")];
        const std::int32_t disp = static_cast<std::int32_t>(vt.bytes[3] | (vt.bytes[4] << 8) | (vt.bytes[5] << 16) |
            (static_cast<std::uint32_t>(vt.bytes[6]) << 24));
        Check(vt.rva + 7 + disp == kWrapperVtableRva, "the wrapper vtable lea points at 0x360F1C0");
        Check(kSites[SiteIndex("wrapper_volume_ref")].bytes[3] == (kWrapperVolumeRef & 0xFF) &&
                  kSites[SiteIndex("wrapper_apply")].bytes[3] == (kWrapperVolumeRef & 0xFF) &&
                  kSites[SiteIndex("wrapper_apply")].bytes[4] == (kWrapperVolumeRef >> 8),
            "+0x108 in Init and in ApplyVariables");
        Check(kSites[SiteIndex("wrapper_internal")].bytes[2] == (kInternalVariableLimit >> 8), "internal below 0x400");
        Check(std::string(PartName(Part::Wrapper)) == "wrapper", "part name");
    }
}

int RunPedSpeedTests()
{
    TestConstants();
    TestTrack();
    TestMotion();
    TestChoose();
    TestSlots();
    TestLines();
    TestWrapperVar();
    if (g_pedSpeedFailures == 0)
        std::printf("ped speed tests passed\n");
    return g_pedSpeedFailures;
}
