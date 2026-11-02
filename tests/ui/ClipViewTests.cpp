// The Clip view and piano roll (docs/05-m4-ui.md, 4c), headless: the grid and time arithmetic,
// every note edit, the layout's zoom and log-spaced rows, the Colundi ruler (labels, anchors,
// unplayable rows), the selection following the notes, the mouse and keys, auditioning through the
// engine, the playhead, and snapshots in every theme.

#include "support/OfflineRenderer.h"
#include "support/TestDevices.h"
#include "ui/clip/ClipCommands.h"
#include "ui/clip/ClipView.h"
#include "ui/clip/NoteEdits.h"
#include "ui/clip/PianoRollLayout.h"
#include "ui/commands/SessionCommands.h"
#include "ui/theme/LatticeLookAndFeel.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace lattice;
using namespace lattice::ui;
using namespace lattice::test;
using Catch::Approx;
using Area = PianoRoll::Hit::Area;

namespace
{
NoteData note(int pitch, double start, double length = 1.0, int velocity = 100)
{
    return {pitch, start, length, velocity, false};
}

/** A set with a clip in track 1's first slot, shown in a Clip view. */
struct Fixture
{
    explicit Fixture(const juce::String& tuningId = {})
    {
        session.newSet(2, 2);
        for (int t = 0; t < 2; ++t)
        {
            auto chain = LiveSet::chainOf(set().track(t));
            while (chain.getNumChildren() > 0)
                set().removeDevice(chain.getChild(0));
            set().insertDevice(chain, -1, TestRecorderDevice::typeId);
            set().setTrackArm(t, false, true);
        }
        if (tuningId.isNotEmpty())
            set().setTrackTuning(0, tuningId);
        clip = set().createClip(0, 0, 4.0, "Clip");
        session.flush();
        renderer.renderFor(480);
        addSessionCommands(commands, session,
                           {[this] { return selection.get(); }, [this](SessionSelection s) { selection.set(s); },
                            [this]() -> EditTarget* { return routeEdits ? &view->pianoRoll() : nullptr; }},
                           {});
        view = std::make_unique<ClipView>(session, feed, tunings, selection);
        addClipCommands(commands, view->pianoRoll());
        view->setLookAndFeel(&laf);
        view->setSize(1200, 600);
        view->flushUpdates();
    }
    ~Fixture() { view->setLookAndFeel(nullptr); }

    LiveSet& set() { return session.set(); }
    PianoRoll& roll() { return view->pianoRoll(); }
    ClipNotes& notes() { return roll().notes(); }
    juce::ValueTree add(const NoteData& n) { return set().addNote(clip, n); }
    void render(std::int64_t samples)
    {
        session.flush();
        renderer.renderFor(samples);
        session.processEngineEvents();
        now += 1.0 / 60.0;
        feed.tick(now);
    }
    /** The centre of a note's rectangle on screen. */
    juce::Point<float> centreOf(std::size_t index) { return roll().noteBounds(index).getCentre(); }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    OfflineRenderer renderer{engine, BlockPattern::Fixed64};
    EngineStateFeed feed{session, engine};
    CommandRegistry commands{&session.undoManager()};
    SessionSelectionModel selection;
    LatticeLookAndFeel laf;
    juce::ValueTree clip;
    std::unique_ptr<ClipView> view;
    bool routeEdits = false;
    double now = 3.0;
};
} // namespace

// --- Time and grid arithmetic -----------------------------------------------------------------------

TEST_CASE("Clip time: bars.beats.sixteenths in 4/4 and 6/8", "[ui][clip]")
{
    const BarsBeats common{4, 4};
    CHECK(common.position(0.0) == "1.1.1");
    CHECK(common.position(5.25) == "2.2.2");
    CHECK(common.length(4.0) == "1.0.0");
    CHECK(common.length(0.25) == "0.0.1");
    CHECK(*common.parsePosition("2.2.2") == Approx(5.25));
    CHECK(*common.parsePosition("3") == Approx(8.0));
    CHECK(*common.parseLength("1.2") == Approx(6.0));
    CHECK(!common.parsePosition("x.1").has_value());

    const BarsBeats sixEight{6, 8}; // a beat is an eighth: two sixteenths
    CHECK(sixEight.quartersPerBar() == Approx(3.0));
    CHECK(sixEight.position(3.5) == "2.2.1");
    CHECK(*sixEight.parsePosition("2.2.1") == Approx(3.5));
}

TEST_CASE("Note grid: adaptive follows the zoom and fixed stays put", "[ui][clip]")
{
    NoteGrid g;
    // 40 pixels a quarter note, lines at least 16 pixels apart: a sixteenth (10 px) is too fine.
    CHECK(g.step(40.0, 4.0, 16.0) == Approx(0.5));
    CHECK(g.step(400.0, 4.0, 16.0) == Approx(0.0625));
    CHECK(g.step(2.0, 4.0, 16.0) == Approx(8.0)); // two bars
    g.narrow();
    CHECK(g.step(40.0, 4.0, 16.0) == Approx(0.25));
    g.widen();
    g.widen();
    CHECK(g.step(40.0, 4.0, 16.0) == Approx(1.0));
    g.triplet = true; // quarter triplets would be 27 pixels apart, under the 32 asked for: half-note triplets
    CHECK(g.step(40.0, 4.0, 16.0) == Approx(2.0 * 2.0 / 3.0));
    CHECK(g.describe(40.0, 4.0, 16.0) == "1/2T");
    g.density = 0;
    CHECK(g.step(40.0, 4.0, 16.0) == Approx(2.0 / 3.0));

    NoteGrid fixed;
    fixed.adaptive = false;
    CHECK(fixed.step(40.0, 4.0, 16.0) == Approx(0.25)); // 1/16 whatever the zoom
    CHECK(fixed.step(4000.0, 4.0, 16.0) == Approx(0.25));
    fixed.widen();
    CHECK(fixed.step(1.0, 4.0, 16.0) == Approx(0.5));
    fixed.snap = false;
    CHECK(fixed.describe(1.0, 4.0, 16.0) == "1/8 (Off)");
    CHECK(NoteGrid::nearest(1.13, 0.25) == Approx(1.25));
    CHECK(NoteGrid::floor(1.24, 0.25) == Approx(1.0));
}

// --- Note edits -------------------------------------------------------------------------------------

TEST_CASE("Note edits: moving keeps the notes in the clip and on the rows", "[ui][clip][editing]")
{
    const notes::Notes n{note(60, 1.0), note(126, 2.0)};
    auto moved = notes::applied(n, notes::move(n, {0, 1}, -3.0, 5));
    CHECK(moved[0].start == Approx(0.0)); // cut short at the clip's start
    CHECK(moved[0].pitch == 61);         // and at the top row
    CHECK(moved[1].pitch == 127);
    CHECK(moved[1].start == Approx(1.0)); // the layout kept

    const auto copies = notes::copyMoved(n, {0}, 2.0, 0);
    CHECK(copies.changed.empty());
    REQUIRE(copies.added.size() == 1);
    CHECK(copies.added[0].start == Approx(3.0));
}

TEST_CASE("Note edits: resizing and duplicating", "[ui][clip][editing]")
{
    const notes::Notes n{note(60, 1.0, 1.0), note(62, 2.0, 0.5)};
    auto longer = notes::applied(n, notes::resizeEnd(n, {0, 1}, -0.75));
    CHECK(longer[0].length == Approx(0.25));
    CHECK(longer[1].length == Approx(notes::minLength)); // never gone
    auto earlier = notes::applied(n, notes::resizeStart(n, {0}, -0.5));
    CHECK(earlier[0].start == Approx(0.5));
    CHECK(earlier[0].length == Approx(1.5)); // the end stays

    const auto dup = notes::duplicate(n, {0, 1}); // the span 1..2.5: copies 1.5 later
    REQUIRE(dup.added.size() == 2);
    CHECK(dup.added[0].start == Approx(2.5));
    CHECK(dup.added[1].start == Approx(3.5));
    const auto flags = notes::selectionAfter(2, dup);
    CHECK(flags == std::vector<char>{0, 0, 1, 1}); // the copies are selected
}

TEST_CASE("Note edits: split at the grid and join the pieces back", "[ui][clip][editing]")
{
    const notes::Notes n{note(60, 0.5, 1.5)};
    const auto split = notes::applied(n, notes::split(n, {0}, 0.5));
    REQUIRE(split.size() == 3);
    CHECK(split[0].start == Approx(0.5));
    CHECK(split[0].length == Approx(0.5));
    CHECK(split[1].start == Approx(1.0));
    CHECK(split[2].start == Approx(1.5));
    CHECK(split[2].length == Approx(0.5));
    // A note inside one grid cell isn't split.
    CHECK(notes::split({note(60, 0.1, 0.2)}, {0}, 0.5).empty());

    const auto joined = notes::applied(split, notes::join(split, {0, 1, 2}));
    REQUIRE(joined.size() == 1);
    CHECK(joined[0].start == Approx(0.5));
    CHECK(joined[0].length == Approx(1.5));
}

TEST_CASE("Note edits: quantize moves starts towards the grid by the amount", "[ui][clip][editing]")
{
    const notes::Notes n{note(60, 1.1, 1.0), note(60, 2.9, 0.3)};
    auto full = notes::applied(n, notes::quantize(n, {0, 1}, 0.5, 1.0, false));
    CHECK(full[0].start == Approx(1.0));
    CHECK(full[0].length == Approx(1.0)); // the length kept
    CHECK(full[1].start == Approx(3.0));
    auto half = notes::applied(n, notes::quantize(n, {0}, 0.5, 0.5, false));
    CHECK(half[0].start == Approx(1.05));
    auto ends = notes::applied(n, notes::quantize(n, {1}, 0.5, 1.0, true));
    CHECK(ends[1].start == Approx(3.0));
    CHECK(ends[1].start + ends[1].length == Approx(3.0 + notes::minLength)); // its end went to 3.0 as well
}

TEST_CASE("Note edits: legato reverse invert and stretch", "[ui][clip][editing]")
{
    const notes::Notes n{note(60, 0.0, 0.25), note(64, 1.0, 0.25), note(67, 1.0, 0.25), note(72, 3.0, 0.5)};
    const notes::Indices all{0, 1, 2, 3};
    const auto legato = notes::applied(n, notes::legato(n, all, 4.0));
    CHECK(legato[0].length == Approx(1.0));
    CHECK(legato[1].length == Approx(2.0)); // a chord's notes share the next start
    CHECK(legato[2].length == Approx(2.0));
    CHECK(legato[3].length == Approx(1.0)); // the last runs to the loop's end

    const auto reversed = notes::applied(n, notes::reverse(n, all)); // span 0..3.5
    CHECK(reversed[3].start == Approx(0.0));
    CHECK(reversed[0].start == Approx(3.25));

    const auto inverted = notes::applied(n, notes::invert(n, all)); // range 60..72
    CHECK(inverted[0].pitch == 72);
    CHECK(inverted[1].pitch == 68);
    CHECK(inverted[3].pitch == 60);

    const auto stretched = notes::applied(n, notes::stretch(n, all, 2.0));
    CHECK(stretched[1].start == Approx(2.0));
    CHECK(stretched[3].start == Approx(6.0));
    CHECK(stretched[3].length == Approx(1.0));
}

TEST_CASE("Note edits: velocities set ramped and changed together", "[ui][clip][editing]")
{
    const notes::Notes n{note(60, 0.0, 1.0, 40), note(61, 1.0, 1.0, 80), note(62, 2.0, 1.0, 120)};
    const auto ramp = notes::applied(n, notes::rampVelocity(n, {0, 1, 2}, 10, 110));
    CHECK(ramp[0].velocity == 10);
    CHECK(ramp[1].velocity == 60);
    CHECK(ramp[2].velocity == 110);
    const auto more = notes::applied(n, notes::changeVelocity(n, {0, 2}, 20));
    CHECK(more[0].velocity == 60);
    CHECK(more[1].velocity == 80);
    CHECK(more[2].velocity == 127); // kept in range
}

TEST_CASE("Note edits: duplicating the loop copies its notes one loop on", "[ui][clip][editing]")
{
    const notes::Notes n{note(60, 0.0), note(62, 3.5), note(64, 4.5)};
    const auto dup = notes::duplicateLoop(n, 0.0, 4.0);
    REQUIRE(dup.added.size() == 2); // not the note after the loop
    CHECK(dup.added[0].start == Approx(4.0));
    CHECK(dup.added[1].start == Approx(7.5));
}

TEST_CASE("Note edits: transposing by semitones finds the nearest Colundi entries", "[ui][clip][editing][tuning]")
{
    TuningRegistry tunings;
    const auto colundi = tunings.findById(TuningRegistry::colundiId);
    REQUIRE(colundi != nullptr);
    const notes::Notes n{note(40, 0.0)};
    const auto up = notes::applied(n, notes::transposeSemitones(n, {0}, 12.0, *colundi, 0));
    // An octave up: the entry nearest twice the frequency.
    CHECK(up[0].pitch == colundi->nearestNoteTo(colundi->hz[40] * 2.0));
    CHECK(up[0].pitch != 40);
    // With the track's offset, rows play entry pitch + offset; the result is still a row.
    const auto offset = notes::applied(n, notes::transposeSemitones(n, {0}, 12.0, *colundi, 5));
    CHECK(offset[0].pitch == colundi->nearestNoteTo(colundi->hz[45] * 2.0) - 5);
    // Table steps are rows.
    CHECK(notes::applied(n, notes::transposeSteps(n, {0}, -3))[0].pitch == 37);
}

TEST_CASE("Note edits: an edit touches only the notes it changes and is one undo step", "[ui][clip][editing]")
{
    Fixture f;
    for (int i = 0; i < 100; ++i)
        f.add(note(60 + i % 12, i * 0.25, 0.25));
    f.view->flushUpdates();
    f.session.undoManager().clearUndoHistory();
    const auto before = LiveSet::notesOf(f.clip);
    auto untouched = f.clip.getChildWithName(ids::notes).getChild(50);

    f.notes().select({3, 7}, false);
    f.roll().transposeSteps(2);
    const auto after = LiveSet::notesOf(f.clip);
    CHECK(after[3].pitch == before[3].pitch + 2);
    CHECK(after[7].pitch == before[7].pitch + 2);
    CHECK(after[50] == before[50]);
    CHECK(f.clip.getChildWithName(ids::notes).getChild(50) == untouched); // the same node
    CHECK(f.notes().selected() == notes::Indices{3, 7});

    f.session.undoManager().undo();
    CHECK(LiveSet::notesOf(f.clip) == before);
    CHECK(!f.session.undoManager().canUndo());
}

TEST_CASE("Clip notes: the selection stays with its notes as notes come and go", "[ui][clip]")
{
    Fixture f;
    for (int i = 0; i < 5; ++i)
        f.add(note(60 + i, i));
    f.view->flushUpdates();
    f.notes().select({1, 3}, false);
    // Another note added (as recording would) and one removed before them.
    f.add(note(70, 0.5));
    CHECK(f.notes().selected() == notes::Indices{1, 3});
    f.session.undoManager().beginNewTransaction("Remove");
    f.set().removeNote(f.clip, f.clip.getChildWithName(ids::notes).getChild(0));
    CHECK(f.notes().selected() == notes::Indices{0, 2});
    CHECK(f.notes().notes()[0].pitch == 61);
    // Undo puts the note back where it was: the flags move back too.
    f.session.undoManager().undo();
    CHECK(f.notes().selected() == notes::Indices{1, 3});
    CHECK(f.notes().notes()[0].pitch == 60);
}

// --- Layout -----------------------------------------------------------------------------------------

TEST_CASE("Piano roll layout: zooming keeps the time under the pointer", "[ui][clip]")
{
    PianoRollLayout l;
    l.setZoomX(40.0);
    l.setScrollQuarters(2.0);
    const double before = l.quartersAt(300.0);
    l.zoomXAround(2.5, 300.0);
    CHECK(l.pixelsPerQuarter() == Approx(100.0));
    CHECK(l.quartersAt(300.0) == Approx(before));
    CHECK(l.xFor(before) == Approx(300.0));
    l.setZoomX(1.0e9);
    CHECK(l.pixelsPerQuarter() == Approx(PianoRollLayout::maxPixelsPerQuarter));
}

TEST_CASE("Piano roll layout: rows highest first and folded to some", "[ui][clip]")
{
    PianoRollLayout l;
    l.setRows(PianoRollLayout::allRows(), 10.0);
    CHECK(l.numRows() == 128);
    CHECK(l.pitchAtRow(0) == 127);
    CHECK(l.rowOf(0) == 127);
    CHECK(l.contentHeight() == Approx(1280.0));
    CHECK(l.pitchAt(5.0) == 127);
    CHECK(l.pitchAt(15.0) == 126);
    l.setScrollY(100.0, 200.0);
    CHECK(l.pitchAt(5.0) == 117);
    CHECK(l.rowTop(l.rowOf(117)) == Approx(0.0));

    l.setRows({64, 60, 67, 60}, 10.0);
    CHECK(l.numRows() == 3);
    CHECK(l.pitchAtRow(0) == 67);
    CHECK(!l.isShown(62));
}

TEST_CASE("Piano roll layout: log-spaced rows sit in proportion to the log of their frequencies", "[ui][clip][tuning]")
{
    TuningRegistry tunings;
    const auto colundi = tunings.findById(TuningRegistry::colundiId);
    REQUIRE(colundi != nullptr);
    PianoRollLayout l;
    l.setRows(PianoRollLayout::allRows(), 12.0, &colundi->hz);
    REQUIRE(l.isLogSpaced());
    // The gap between any two rows' centres over the log of their frequencies' ratio is one constant.
    const auto gapOver = [&](int a, int b) {
        return (l.rowCentre(l.rowOf(a)) - l.rowCentre(l.rowOf(b))) / std::log2(colundi->hz[static_cast<std::size_t>(b)] / colundi->hz[static_cast<std::size_t>(a)]);
    };
    const double k = gapOver(10, 11);
    for (const auto& [a, b] : {std::pair{20, 21}, std::pair{40, 41}, std::pair{100, 101}, std::pair{3, 90}})
        CHECK(gapOver(a, b) == Approx(k).epsilon(1.0e-9));
    // The rows average the row height, and their edges meet.
    CHECK(l.contentHeight() / 128.0 == Approx(12.0).epsilon(0.05));
    for (int r = 0; r + 1 < l.numRows(); ++r)
        CHECK(l.rowBottom(r) == Approx(l.rowTop(r + 1)));
    // Rows that differ in Colundi differ on screen.
    const double small = l.rowCentre(l.rowOf(1)) - l.rowCentre(l.rowOf(2));
    const double big = l.rowCentre(l.rowOf(2)) - l.rowCentre(l.rowOf(3));
    CHECK(small != Approx(big));

    // 12-TET's rows are all one interval apart: log spacing changes nothing.
    PianoRollLayout even;
    const auto tet = tunings.defaultTuning();
    even.setRows(PianoRollLayout::allRows(), 12.0, &tet->hz);
    CHECK(even.rowBottom(0) - even.rowTop(0) == Approx(12.0));
    CHECK(even.rowBottom(60) - even.rowTop(60) == Approx(12.0));
}

// --- The ruler --------------------------------------------------------------------------------------

TEST_CASE("Colundi ruler: labels match the table and anchors are the multiples of 30 Hz", "[ui][clip][tuning]")
{
    TuningRegistry tunings;
    const auto colundi = tunings.findById(TuningRegistry::colundiId);
    REQUIRE(colundi != nullptr);
    CHECK(colundi->baseHz() == Approx(30.0));
    TuningRowContext c;
    c.tuning = colundi.get();
    c.sampleRate = 48000.0;
    for (int p = 0; p < 128; ++p)
    {
        const auto row = tuningRow(c, p);
        const double hz = colundi->hz[static_cast<std::size_t>(p)];
        CHECK(!row.namedNotes);
        CHECK(!row.blackKey);
        CHECK(row.label.startsWith(juce::String(p) + juce::String::fromUTF8(" \xc2\xb7 ")));
        CHECK(row.label.endsWith(juce::String(hz, hz < 1000.0 ? 1 : 0)));
        const double multiple = hz / 30.0;
        const bool exact = std::abs(multiple - std::round(multiple)) * 30.0 < 0.0005;
        CHECK(row.anchor == exact);
        CHECK(row.unplayable == (hz >= 0.45 * 48000.0));
    }
    CHECK(tuningRow(c, 0).anchor); // 30 Hz itself
    CHECK(tuningRowDetails(c, 0).contains("anchor"));
    // 59.9 Hz isn't twice 30.
    CHECK(!tuningRow(c, colundi->nearestNoteTo(59.9)).anchor);

    // The offset: a row plays the entry five up.
    c.noteOffset = 5;
    CHECK(tuningRow(c, 10).entry == 15);
    CHECK(tuningRow(c, 10).label.startsWith("15"));
    CHECK(tuningRow(c, 125).entry == -1);
    CHECK(tuningRow(c, 125).unplayable);

    // Folding down: too high, but heard.
    c.noteOffset = 0;
    c.policy = HighNotePolicy::FoldDown;
    CHECK(tuningRow(c, 127).foldsDown);
    CHECK(!tuningRow(c, 127).unplayable);

    // Highlights are table entries.
    c.highlight = {47};
    CHECK(tuningRow(c, 47).highlighted);
    CHECK(!tuningRow(c, 46).highlighted);
}

TEST_CASE("Colundi ruler: hover tells the cents and ratio above the base", "[ui][clip][tuning]")
{
    TuningRegistry tunings;
    const auto colundi = tunings.findById(TuningRegistry::colundiId);
    TuningRowContext c;
    c.tuning = colundi.get();
    const int fifth = colundi->nearestNoteTo(45.0); // 3/2 of 30 Hz
    const auto text = tuningRowDetails(c, fifth);
    CHECK(text.contains("45.00 Hz"));
    CHECK(text.contains("702.0 ct"));
    CHECK(text.contains(juce::String::fromUTF8("\xc3\x97") + "1.500 of 30.00 Hz"));
}

TEST_CASE("12-TET ruler: note names and black keys", "[ui][clip][tuning]")
{
    TuningRegistry tunings;
    TuningRowContext c;
    c.tuning = tunings.defaultTuning().get();
    CHECK(tuningRow(c, 60).label == "C4");
    CHECK(tuningRow(c, 60).octaveStart);
    CHECK(tuningRow(c, 61).blackKey);
    CHECK(!tuningRow(c, 64).blackKey);
    CHECK(!tuningRow(c, 60).anchor); // no anchors with note names
    c.noteOffset = 1; // the keys follow the note played
    CHECK(tuningRow(c, 60).blackKey);
}

// --- The model ------------------------------------------------------------------------------------------

TEST_CASE("Highlights: a clip's own set or its track's", "[ui][clip][model]")
{
    Fixture f;
    auto& set = f.set();
    CHECK(LiveSet::effectiveHighlight(f.clip).empty());
    set.setHighlight(set.track(0), {7, 3, 3});
    CHECK(LiveSet::highlightOf(set.track(0)) == std::vector<int>{3, 7});
    CHECK(LiveSet::effectiveHighlight(f.clip) == std::vector<int>{3, 7});
    set.setHighlight(f.clip, {12});
    CHECK(LiveSet::effectiveHighlight(f.clip) == std::vector<int>{12});
    set.setHighlight(f.clip, {});
    CHECK(LiveSet::effectiveHighlight(f.clip) == std::vector<int>{3, 7});

    // Saved and loaded.
    set.setHighlight(f.clip, {1, 2});
    const auto xml = set.toXml();
    LiveSet copy;
    REQUIRE(copy.loadFromXml(*xml).wasOk());
    CHECK(LiveSet::effectiveHighlight(copy.clip(0, 0)) == std::vector<int>{1, 2});
    CHECK(LiveSet::highlightOf(copy.track(0)) == std::vector<int>{3, 7});
}

TEST_CASE("Clip time signature: its own or the set's", "[ui][clip][model]")
{
    Fixture f;
    auto& set = f.set();
    CHECK(set.clipTimeSignature(f.clip) == std::pair{4, 4});
    set.setClipTimeSignature(f.clip, 7, 8);
    CHECK(set.clipTimeSignature(f.clip) == std::pair{7, 8});
    set.setClipTimeSignature(f.clip, 5, 3); // not a power of two: refused
    CHECK(set.clipTimeSignature(f.clip) == std::pair{7, 8});
    set.setClipTimeSignature(f.clip, 0, 0);
    set.setTimeSignature(3, 4);
    CHECK(set.clipTimeSignature(f.clip) == std::pair{3, 4});
}

// --- The view -------------------------------------------------------------------------------------------

TEST_CASE("Clip view: it shows the clip in the selected slot and follows it", "[ui][clip]")
{
    Fixture f;
    CHECK(f.view->clip() == f.clip); // the selection starts at track 1, scene 1
    f.selection.set({SessionSelection::Kind::slot, 1, 0});
    f.view->flushUpdates();
    CHECK(!f.roll().hasClip());
    CHECK(f.view->properties().control("Loop Length") == nullptr);

    const auto made = f.set().createClip(1, 0, 8.0);
    f.view->flushUpdates();
    CHECK(f.view->clip() == made);
    auto* length = dynamic_cast<NumberBox*>(f.view->properties().control("Loop Length"));
    REQUIRE(length != nullptr);
    CHECK(length->binding().text() == "2.0.0");
    f.set().deleteClip(1, 0);
    f.view->flushUpdates();
    CHECK(!f.roll().hasClip());
}

TEST_CASE("Clip view: the properties edit the clip as one undo step each", "[ui][clip][editing]")
{
    Fixture f;
    auto& props = f.view->properties();
    auto* length = dynamic_cast<NumberBox*>(props.control("Loop Length"));
    auto* position = dynamic_cast<NumberBox*>(props.control("Loop Position"));
    auto* mode = dynamic_cast<NumberBox*>(props.control("Launch Mode"));
    REQUIRE((length != nullptr && position != nullptr && mode != nullptr));
    length->commitTypeIn("2.0.0");
    CHECK(static_cast<double>(f.clip[ids::loopEnd]) == Approx(8.0));
    position->commitTypeIn("2.1.1");
    CHECK(static_cast<double>(f.clip[ids::loopStart]) == Approx(4.0));
    CHECK(static_cast<double>(f.clip[ids::loopEnd]) == Approx(12.0)); // the length kept
    mode->binding().setValue(1.0f);
    CHECK(static_cast<int>(f.clip[ids::launchMode]) == static_cast<int>(LaunchMode::Gate));
    f.session.undoManager().undo();
    CHECK(static_cast<int>(f.clip[ids::launchMode]) == static_cast<int>(LaunchMode::Trigger));
    f.session.undoManager().undo();
    CHECK(static_cast<double>(f.clip[ids::loopStart]) == Approx(0.0));

    // The tool buttons run the piano roll's tools.
    f.add(note(60, 0.0, 0.25));
    f.add(note(62, 1.0, 0.25));
    f.view->flushUpdates();
    auto* legato = dynamic_cast<juce::Button*>(props.control("Legato Notes"));
    REQUIRE(legato != nullptr);
    legato->onClick(); // what a click runs
    CHECK(LiveSet::notesOf(f.clip)[0].length == Approx(1.0));
}

TEST_CASE("Piano roll: a double-click adds a note on the grid and dragging moves it", "[ui][clip][editing]")
{
    Fixture f;
    auto& roll = f.roll();
    roll.setScroll(0.0, roll.layout().scrollY());
    const auto at = roll.pointFor(1.1, 60);
    REQUIRE(roll.hitAt(at).area == Area::empty);
    roll.doubleClick(at);
    auto all = LiveSet::notesOf(f.clip);
    REQUIRE(all.size() == 1);
    CHECK(all[0].pitch == 60);
    const double step = roll.gridStep();
    CHECK(all[0].start == Approx(NoteGrid::floor(1.1, step)));
    CHECK(all[0].length == Approx(step));

    // Drag it two rows up and a little later: its start lands on a line.
    f.view->flushUpdates();
    const auto from = f.centreOf(0);
    roll.press(from, {});
    CHECK(f.notes().isSelected(0));
    const auto to = juce::Point<float>(roll.pointFor(2.0, 62).x + (from.x - roll.pointFor(all[0].start, 60).x), roll.pointFor(2.0, 62).y);
    roll.drag(to, {});
    roll.release();
    all = LiveSet::notesOf(f.clip);
    CHECK(all[0].pitch == 62);
    CHECK(std::fmod(all[0].start, step) == Approx(0.0).margin(1.0e-9));
    CHECK(all[0].start == Approx(2.0).margin(step));
    f.session.undoManager().undo(); // the drag is one step
    CHECK(LiveSet::notesOf(f.clip)[0].pitch == 60);
}

TEST_CASE("Piano roll: dragging an edge resizes and Alt+drag copies", "[ui][clip][editing]")
{
    Fixture f;
    f.add(note(60, 1.0, 1.0));
    f.view->flushUpdates();
    auto& roll = f.roll();
    roll.setZoomX(100.0, 0.0);
    roll.setScroll(0.0, roll.layout().scrollY());
    const auto r = roll.noteBounds(0);
    const auto edge = juce::Point<float>(r.getRight() - 1.0f, r.getCentreY());
    REQUIRE(roll.hitAt(edge).area == Area::noteEnd);
    roll.press(edge, {});
    roll.drag(edge.translated(100.0f, 0.0f), {}); // one quarter longer
    roll.release();
    CHECK(LiveSet::notesOf(f.clip)[0].length == Approx(2.0));

    roll.press(f.centreOf(0), juce::ModifierKeys::altModifier);
    roll.drag(f.centreOf(0).translated(200.0f, 0.0f), juce::ModifierKeys::altModifier);
    roll.drag(f.centreOf(0).translated(300.0f, 0.0f), juce::ModifierKeys::altModifier); // still one copy
    roll.release();
    const auto all = LiveSet::notesOf(f.clip);
    REQUIRE(all.size() == 2);
    CHECK(all[0].start == Approx(1.0)); // the original stays
    CHECK(all[1].start == Approx(4.0));
    CHECK(f.notes().selected() == notes::Indices{1});
}

TEST_CASE("Piano roll: a marquee selects and draw mode adds and deletes", "[ui][clip][editing]")
{
    Fixture f;
    f.add(note(60, 0.0));
    f.add(note(62, 1.0));
    f.add(note(72, 1.0));
    f.view->flushUpdates();
    auto& roll = f.roll();
    roll.setScroll(0.0, roll.layout().scrollY());
    roll.setRowZoom(1.0f);
    // From before the first note at row 63 to after the second at row 59.
    roll.press(roll.pointFor(-0.0, 63).translated(1.0f, 0.0f), {});
    roll.drag(roll.pointFor(1.5, 59), {});
    CHECK(!roll.marquee().isEmpty());
    roll.release();
    CHECK(f.notes().selected() == notes::Indices{0, 1});

    roll.setDrawMode(true);
    roll.press(roll.pointFor(3.0, 65), {});
    roll.release();
    CHECK(LiveSet::notesOf(f.clip).size() == 4);
    roll.press(f.centreOf(2), {}); // a click on a note deletes it
    roll.release();
    const auto all = LiveSet::notesOf(f.clip);
    CHECK(all.size() == 3);
    CHECK(std::none_of(all.begin(), all.end(), [](const NoteData& n) { return n.pitch == 72; }));
}

TEST_CASE("Piano roll: velocities drag together and ramp along a line", "[ui][clip][editing]")
{
    Fixture f;
    for (int i = 0; i < 4; ++i)
        f.add(note(60, i, 0.5, 64));
    f.view->flushUpdates();
    auto& roll = f.roll();
    roll.setScroll(0.0, roll.layout().scrollY());
    const auto lane = roll.velocityArea().toFloat();
    REQUIRE(!lane.isEmpty());
    f.notes().select({0, 1}, false);
    const auto stem = juce::Point<float>(roll.pointFor(1.0, 60).x, lane.getCentreY());
    REQUIRE(roll.hitAt(stem).area == Area::velocity);
    REQUIRE(roll.hitAt(stem).note == 1);
    roll.press(stem, {});
    roll.drag(stem.translated(0.0f, -lane.getHeight() * 0.25f), {});
    roll.release();
    auto all = LiveSet::notesOf(f.clip);
    CHECK(all[0].velocity == all[1].velocity); // both, by the same amount
    CHECK(all[0].velocity > 64);
    CHECK(all[2].velocity == 64);

    // Shift+drag: a line from low on the first to high on the last.
    f.notes().clearSelection();
    roll.press({roll.pointFor(0.0, 60).x, lane.getBottom() - 1.0f}, juce::ModifierKeys::shiftModifier);
    roll.drag({roll.pointFor(3.0, 60).x, lane.getY() + 3.0f}, juce::ModifierKeys::shiftModifier);
    roll.release();
    all = LiveSet::notesOf(f.clip);
    CHECK(all[0].velocity < all[1].velocity);
    CHECK(all[1].velocity < all[2].velocity);
    CHECK(all[2].velocity < all[3].velocity);
}

TEST_CASE("Piano roll: the loop brace and start marker drag on the grid", "[ui][clip][editing]")
{
    Fixture f;
    auto& roll = f.roll();
    roll.setZoomX(50.0, 0.0);
    roll.setScroll(0.0, roll.layout().scrollY());
    const float y = static_cast<float>(roll.timeArea().getBottom() - 3);
    const auto end = juce::Point<float>(roll.pointFor(4.0, 60).x, y);
    REQUIRE(roll.hitAt(end).area == Area::loopEnd);
    roll.press(end, {});
    roll.drag(end.translated(200.0f, 0.0f), {}); // four quarters on
    roll.release();
    CHECK(static_cast<double>(f.clip[ids::loopEnd]) == Approx(8.0));
    CHECK(static_cast<double>(f.clip[ids::loopStart]) == Approx(0.0));
}

TEST_CASE("Piano roll: the arrows move notes on the grid or a table step and Shift by a page", "[ui][clip][editing][tuning]")
{
    Fixture f(TuningRegistry::colundiId);
    f.add(note(40, 1.0, 1.0));
    f.view->flushUpdates();
    auto& roll = f.roll();
    CHECK(!roll.keyPressed(juce::KeyPress(juce::KeyPress::upKey))); // nothing selected: the Session view's
    f.notes().select({0}, false);
    CHECK(roll.keyPressed(juce::KeyPress(juce::KeyPress::upKey)));
    CHECK(LiveSet::notesOf(f.clip)[0].pitch == 41);
    CHECK(roll.keyPressed(juce::KeyPress(juce::KeyPress::upKey, juce::ModifierKeys::shiftModifier, 0)));
    CHECK(LiveSet::notesOf(f.clip)[0].pitch == 41 + 17); // Colundi's page
    const double step = roll.gridStep();
    CHECK(roll.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
    CHECK(LiveSet::notesOf(f.clip)[0].start == Approx(1.0 + step));
    CHECK(roll.keyPressed(juce::KeyPress(juce::KeyPress::leftKey, juce::ModifierKeys::shiftModifier, 0)));
    CHECK(LiveSet::notesOf(f.clip)[0].length == Approx(1.0 - step));
    CHECK(roll.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
    CHECK(!f.notes().anySelected());
}

TEST_CASE("Piano roll: the Edit commands act on notes while it has the keys", "[ui][clip][editing][commands]")
{
    Fixture f;
    f.add(note(60, 0.0));
    f.add(note(64, 1.0));
    f.view->flushUpdates();
    f.routeEdits = true;
    CHECK(!f.commands.isEnabled("edit.duplicate")); // nothing selected
    CHECK(f.commands.perform("edit.selectAll"));
    CHECK(f.notes().selected() == notes::Indices{0, 1});
    CHECK(f.commands.perform("edit.duplicate"));
    CHECK(LiveSet::notesOf(f.clip).size() == 4);
    CHECK(f.notes().selected() == notes::Indices{2, 3});
    CHECK(f.commands.perform("edit.delete"));
    CHECK(LiveSet::notesOf(f.clip).size() == 2);
    f.notes().select({1}, false);
    CHECK(f.commands.perform("edit.copy"));
    CHECK(f.commands.perform("edit.paste")); // at the insert position: the clip's start
    auto all = LiveSet::notesOf(f.clip);
    REQUIRE(all.size() == 3);
    CHECK(all[2].pitch == 64);
    CHECK(all[2].start == Approx(0.0));
    // The Session view's clips weren't touched.
    CHECK(f.set().clip(0, 1).isValid() == false);

    // Without the keys, Delete deletes the selected clip.
    f.routeEdits = false;
    CHECK(f.commands.perform("edit.delete"));
    CHECK(!f.set().clip(0, 0).isValid());
}

TEST_CASE("Piano roll: the clip commands run its tools", "[ui][clip][commands]")
{
    Fixture f;
    f.add(note(60, 0.0, 1.0));
    f.view->flushUpdates();
    f.notes().select({0}, false);
    for (const auto* id : {"clip.split", "clip.join", "clip.quantize", "clip.legato", "clip.reverse", "clip.invert", "clip.stretchDouble",
                           "clip.loopDouble", "clip.drawMode", "clip.gridNarrow", "clip.gridTriplet", "clip.fold", "clip.logRows"})
        CHECK(f.commands.find(id) != nullptr);
    CHECK(f.commands.perform("clip.stretchDouble"));
    CHECK(LiveSet::notesOf(f.clip)[0].length == Approx(2.0));
    CHECK(f.commands.perform("clip.loopDouble"));
    CHECK(static_cast<double>(f.clip[ids::loopEnd]) == Approx(8.0));
    CHECK(f.commands.perform("clip.drawMode"));
    CHECK(f.roll().isDrawMode());
    CHECK(f.commands.shortcutText("clip.drawMode") == "B");
    CHECK(f.commands.shortcutText("clip.split").contains("E"));
    CHECK(f.commands.conflicts().empty());
}

TEST_CASE("Piano roll: highlighted rows are stored on the clip or its track", "[ui][clip][editing]")
{
    Fixture f(TuningRegistry::colundiId);
    f.add(note(47, 0.0));
    f.view->flushUpdates();
    auto& roll = f.roll();
    roll.toggleHighlight(30);
    CHECK(LiveSet::highlightOf(f.clip) == std::vector<int>{30});
    f.notes().select({0}, false);
    roll.toggleHighlightOfSelectedRows();
    CHECK(LiveSet::highlightOf(f.clip) == std::vector<int>{30, 47});
    CHECK(tuningRow(roll.rowContext(), 47).highlighted);
    roll.moveHighlightToTrack();
    CHECK(!f.clip.hasProperty(ids::highlight));
    CHECK(LiveSet::highlightOf(f.set().track(0)) == std::vector<int>{30, 47});
    CHECK(tuningRow(roll.rowContext(), 47).highlighted);
}

TEST_CASE("Piano roll: clicking a note or a key plays it on the track and it isn't recorded", "[ui][clip][engine]")
{
    Fixture f;
    f.add(note(64, 0.0));
    f.view->flushUpdates();
    auto& roll = f.roll();
    f.renderer.attach(f.session);
    const auto id = f.set().trackId(0);
    REQUIRE(!f.session.isArmed(0));

    roll.press(f.centreOf(0), {});
    f.render(480);
    auto ons = f.renderer.noteOns(id);
    REQUIRE(ons.size() == 1);
    CHECK(ons[0].data1 == 64);
    roll.release();
    f.render(480);
    CHECK(f.renderer.unbalancedNotes(id).empty());

    // A key plays its row and selects its notes.
    roll.press({static_cast<float>(roll.rulerArea().getCentreX()), roll.pointFor(0.0, 64).y}, {});
    f.render(480);
    CHECK(f.renderer.noteOns(id).size() == 2);
    CHECK(f.notes().selected() == notes::Indices{0});
    roll.release();
    f.render(480);
    CHECK(f.renderer.unbalancedNotes(id).empty());
    CHECK(LiveSet::notesOf(f.clip).size() == 1); // nothing recorded
}

TEST_CASE("Piano roll: the playhead follows the playing clip", "[ui][clip]")
{
    Fixture f;
    CHECK(f.roll().playhead() < 0.0);
    f.session.launchSlot(0, 0);
    f.render(OfflineRenderer::samplesForBeats(1.5));
    CHECK(f.roll().playhead() == Approx(1.5).margin(0.1));
    // Past the view's right edge with follow on: it pages on.
    f.roll().setZoomX(400.0, 0.0);
    f.roll().setScroll(0.0, f.roll().layout().scrollY());
    f.render(OfflineRenderer::samplesForBeats(1.5));
    CHECK(f.roll().layout().scrollQuarters() > 0.0);
}

// --- Snapshots and timing -------------------------------------------------------------------------------

TEST_CASE("Clip view snapshot: a Colundi clip in every theme at 100% and 200%", "[.ui-snapshot]")
{
    // lattice_tests "[.ui-snapshot]" writes PNGs to LATTICE_SNAPSHOT_DIR (default: temp).
    const juce::File dir(juce::SystemStats::getEnvironmentVariable(
        "LATTICE_SNAPSHOT_DIR", juce::File::getSpecialLocation(juce::File::tempDirectory).getFullPathName()));
    for (const bool colundi : {true, false})
    {
        Fixture f(colundi ? juce::String(TuningRegistry::colundiId) : juce::String());
        const int base = colundi ? 40 : 60;
        const int steps[] = {0, 3, 5, 7, 10, 12, 7, 5};
        for (int i = 0; i < 8; ++i)
            f.add(note(base + steps[i], i * 0.5, 0.5, 40 + i * 12));
        f.set().setHighlight(f.clip, {base, base + 5, base + 7});
        f.view->flushUpdates();
        f.roll().zoomToClip();
        f.notes().select({2, 3}, false);
        f.session.launchSlot(0, 0);
        f.render(OfflineRenderer::samplesForBeats(1.25));
        for (const auto* theme : {&Theme::dark(), &Theme::light(), &Theme::highContrast()})
        {
            f.laf.setTheme(*theme);
            f.view->sendLookAndFeelChange();
            for (const float scale : {1.0f, 2.0f})
            {
                const auto image = f.view->createComponentSnapshot(f.view->getLocalBounds(), true, scale);
                const auto file = dir.getChildFile("clip-" + juce::String(colundi ? "colundi" : "12tet") + "-" +
                                                   theme->name.removeCharacters(" ").toLowerCase() + "-" + juce::String(juce::roundToInt(scale * 100)) + ".png");
                file.deleteFile();
                juce::FileOutputStream out(file);
                juce::PNGImageFormat png;
                REQUIRE(png.writeImageToStream(image, out));
            }
        }
        if (colundi)
        {
            f.laf.setTheme(Theme::dark());
            f.view->sendLookAndFeelChange();
            f.roll().setRowsLogSpaced(true);
            const auto image = f.view->createComponentSnapshot(f.view->getLocalBounds(), true, 1.0f);
            const auto file = dir.getChildFile("clip-colundi-log-rows.png");
            file.deleteFile();
            juce::FileOutputStream out(file);
            juce::PNGImageFormat png;
            REQUIRE(png.writeImageToStream(image, out));
        }
    }
}

TEST_CASE("Piano roll timing: 10000 notes paint within a frame while zooming and scrolling", "[ui][clip][timing]")
{
    Fixture f;
    juce::Random random(7);
    notes::Change c;
    for (int i = 0; i < 10000; ++i)
        c.added.push_back(note(36 + random.nextInt(48), i * 0.0625, 0.0625 + random.nextInt(8) * 0.0625, 1 + random.nextInt(127)));
    f.session.undoManager().beginNewTransaction("Notes");
    f.notes().apply(f.set(), c);
    f.view->flushUpdates();
    auto& roll = f.roll();
    roll.setSize(1600, 800);
    roll.zoomToClip();
    roll.setZoomX(30.0, 0.0); // about 2000 notes across the view
    juce::Image target(juce::Image::ARGB, roll.getWidth(), roll.getHeight(), true);
    std::vector<double> times;
    for (int frame = 0; frame < 60; ++frame)
    {
        // Every frame changes the view, so the cached layer is redrawn each time.
        if (frame % 2 == 0)
            roll.setZoomX(roll.layout().pixelsPerQuarter() * 1.03, 400.0);
        else
            roll.setScroll(roll.layout().scrollQuarters() + 0.5, roll.layout().scrollY() + 3.0);
        juce::Graphics g(target);
        const auto start = juce::Time::getHighResolutionTicks();
        roll.paintEntireComponent(g, true);
        times.push_back(juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - start));
    }
    std::sort(times.begin(), times.end());
    const double median = times[times.size() / 2];
    WARN("Piano roll with 10000 notes: median frame " << median * 1000.0 << " ms, worst " << times.back() * 1000.0 << " ms");
    CHECK(median < 0.008);
}
