#include "ui/clip/PianoRoll.h"

#include "ui/theme/LatticeLookAndFeel.h"
#include "ui/widgets/Toggle.h"

#include <algorithm>
#include <cmath>

namespace lattice::ui
{
namespace
{
using Area = PianoRoll::Hit::Area;

/** The notes copied with Copy, from the first one's start. Shared by every piano roll. */
notes::Notes& clipboard()
{
    static notes::Notes copied;
    return copied;
}

juce::Colour colourOf(const juce::ValueTree& node, juce::Colour fallback)
{
    if (!node.hasProperty(ids::colour))
        return fallback;
    return juce::Colour(static_cast<juce::uint32>(static_cast<juce::int64>(node[ids::colour])));
}

notes::Indices indicesOf(const std::vector<char>& flags)
{
    notes::Indices out;
    for (std::size_t i = 0; i < flags.size(); ++i)
        if (flags[i] != 0)
            out.push_back(i);
    return out;
}
} // namespace

PianoRoll::PianoRoll(SessionController& s, EngineStateFeed& f, TuningRegistry& t) : session(s), feed(f), tunings(t)
{
    setTitle("Piano roll");
    setOpaque(true);
    setWantsKeyboardFocus(true);
    for (auto* bar : {&vertical, &horizontal})
    {
        bar->setAutoHide(false);
        bar->addListener(this);
        addAndMakeVisible(*bar);
    }
    clipNotes.onChange = [this] {
        contentChanged();
        triggerAsyncUpdate(); // a fold follows the notes in use
    };
    feed.addListener(this, EngineStateFeed::slotsAspect | EngineStateFeed::transportAspect);
    rowsChanged();
}

PianoRoll::~PianoRoll()
{
    endAudition();
    feed.removeListener(this);
    if (trackTree.isValid())
        trackTree.removeListener(this);
}

// --- The clip -------------------------------------------------------------------------------------

void PianoRoll::setClip(const juce::ValueTree& c)
{
    const bool same = c == clipNotes.clip();
    endAudition();
    gesture = Gesture::none;
    if (!same)
    {
        clipNotes.setClip(c);
        clipNotes.clearSelection();
    }
    const auto newTrack = c.isValid() ? c.getParent().getParent().getParent() : juce::ValueTree();
    if (newTrack != trackTree)
    {
        if (trackTree.isValid())
            trackTree.removeListener(this);
        trackTree = newTrack;
        if (trackTree.isValid())
            trackTree.addListener(this);
    }
    rowsChanged();
    if (!same)
    {
        insertQuarters = c.isValid() ? static_cast<double>(c[ids::loopStart]) : 0.0;
        zoomToClip();
    }
    contentChanged();
}

int PianoRoll::track() const
{
    return trackTree.isValid() ? session.set().trackIndexForId(LiveSet::idOf(trackTree)) : -1;
}

void PianoRoll::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property)
{
    // The track's tuning and highlight decide the rows; its colour, the notes' when the clip has none.
    if (tree == trackTree && (property == ids::tuningId || property == ids::noteOffset || property == ids::highNotePolicy ||
                              property == ids::highlight || property == ids::colour))
    {
        rowsChanged();
        contentChanged();
    }
}

TuningRowContext PianoRoll::rowContext() const
{
    TuningRowContext c;
    if (!trackTree.isValid())
    {
        c.tuning = tunings.defaultTuning().get();
        return c;
    }
    auto tuning = tunings.findById(trackTree[ids::tuningId].toString());
    if (tuning == nullptr)
        tuning = tunings.defaultTuning();
    shownTuning = tuning; // keeps the table alive while the context points into it
    c.tuning = shownTuning.get();
    c.noteOffset = trackTree[ids::noteOffset];
    c.policy = static_cast<HighNotePolicy>(static_cast<int>(trackTree[ids::highNotePolicy]));
    if (const int t = track(); t >= 0)
        if (const auto processor = session.processorForTrack(t); processor != nullptr && processor->tuning().getSampleRate() > 0.0)
            c.sampleRate = processor->tuning().getSampleRate();
    if (hasClip())
        c.highlight = LiveSet::effectiveHighlight(clip());
    return c;
}

double PianoRoll::quartersPerBar() const
{
    if (!hasClip())
        return session.set().beatsPerBar();
    const auto [num, den] = session.set().clipTimeSignature(clip());
    return num * 4.0 / den;
}

double PianoRoll::gridMinPixels() const
{
    return themeOf(*this).controlHeight * 0.75;
}

double PianoRoll::gridStep() const
{
    return noteGrid.step(view.pixelsPerQuarter(), quartersPerBar(), gridMinPixels());
}

void PianoRoll::gridChanged()
{
    contentChanged();
}

void PianoRoll::setGridAdaptive(bool adaptive)
{
    if (noteGrid.adaptive && !adaptive)
        noteGrid.fixedIndex = noteGrid.valueIndex(view.pixelsPerQuarter(), quartersPerBar(), gridMinPixels());
    noteGrid.adaptive = adaptive;
    gridChanged();
}

// --- Modes and view --------------------------------------------------------------------------------

void PianoRoll::setDrawMode(bool on)
{
    drawMode = on;
    repaint();
}

void PianoRoll::setFolded(bool on)
{
    folded = on;
    rowsChanged();
}

void PianoRoll::setRowsLogSpaced(bool on)
{
    logRows = on;
    rowsChanged();
}

void PianoRoll::setRowZoom(float zoom)
{
    // Keep the row at the view's middle where it is.
    const double middle = area.getHeight() * 0.5;
    const int pitch = view.pitchAt(middle);
    rowScale = juce::jlimit(0.5f, 4.0f, zoom);
    rowsChanged();
    if (const int row = view.rowOf(pitch); row >= 0)
        view.setScrollY(view.scrollY() + view.rowCentre(row) - middle, area.getHeight());
    updateScrollBars();
    contentChanged();
}

void PianoRoll::setVelocityLaneVisible(bool visible)
{
    showVelocities = visible;
    resized();
}

void PianoRoll::rowsChanged()
{
    const auto& t = themeOf(*this);
    const double rowHeight = std::round(t.fontSmall + 4.0f) * rowScale;
    std::vector<int> rows;
    if (folded && hasClip())
        for (const auto& n : clipNotes.notes())
            rows.push_back(n.pitch);
    if (rows.empty())
        rows = PianoRollLayout::allRows();

    const auto context = rowContext();
    if (logRows && context.tuning != nullptr)
    {
        // The frequencies the rows play (each note plus the track's offset).
        std::array<double, 128> hz{};
        for (int p = 0; p < 128; ++p)
        {
            const int e = p + context.noteOffset;
            hz[static_cast<std::size_t>(p)] = e >= 0 && e < 128 ? context.tuning->hz[static_cast<std::size_t>(e)] : 0.0;
        }
        view.setRows(std::move(rows), rowHeight, &hz);
    }
    else
        view.setRows(std::move(rows), rowHeight);
    view.setScrollY(view.scrollY(), area.getHeight());
    updateScrollBars();
    contentChanged();
}

void PianoRoll::zoomToClip()
{
    if (area.isEmpty())
        return;
    if (!hasClip())
        return;
    const double loopStart = clip()[ids::loopStart], loopEnd = clip()[ids::loopEnd];
    const double length = juce::jmax(LiveSet::minLoopLength, loopEnd - loopStart);
    view.setZoomX(area.getWidth() / (length * 1.05));
    view.setScrollQuarters(loopStart);

    // The notes in the middle (or the tuning's middle C, without notes).
    int lo = 127, hi = 0;
    for (const auto& n : clipNotes.notes())
    {
        lo = juce::jmin(lo, n.pitch);
        hi = juce::jmax(hi, n.pitch);
    }
    if (lo > hi)
    {
        const auto context = rowContext();
        lo = hi = context.tuning != nullptr ? context.tuning->nearestNoteTo(261.6255653005986) - context.noteOffset : 60;
    }
    const int top = view.rowOf(hi), bottom = view.rowOf(lo);
    if (top >= 0 && bottom >= 0)
    {
        const double middle = (view.rowCentre(top) + view.rowCentre(bottom)) * 0.5 + view.scrollY();
        view.setScrollY(middle - area.getHeight() * 0.5, area.getHeight());
    }
    updateScrollBars();
    contentChanged();
}

void PianoRoll::setZoomX(double pixelsPerQuarter, double anchorX)
{
    view.zoomXAround(pixelsPerQuarter / view.pixelsPerQuarter(), anchorX);
    updateScrollBars();
    contentChanged();
}

void PianoRoll::setScroll(double quarters, double y)
{
    view.setScrollQuarters(quarters);
    view.setScrollY(y, area.getHeight());
    updateScrollBars();
    contentChanged();
}

void PianoRoll::updateScrollBars()
{
    vertical.setRangeLimits(0.0, juce::jmax(view.contentHeight(), static_cast<double>(area.getHeight())), juce::dontSendNotification);
    vertical.setCurrentRange(view.scrollY(), area.getHeight(), juce::dontSendNotification);
    double end = view.quartersAt(area.getWidth());
    if (hasClip())
        end = juce::jmax(end, static_cast<double>(clip()[ids::loopEnd]) + quartersPerBar());
    for (const auto& n : clipNotes.notes())
        end = juce::jmax(end, n.start + n.length);
    horizontal.setRangeLimits(0.0, end, juce::dontSendNotification);
    horizontal.setCurrentRange(view.scrollQuarters(), area.getWidth() / view.pixelsPerQuarter(), juce::dontSendNotification);
}

void PianoRoll::scrollBarMoved(juce::ScrollBar* bar, double start)
{
    if (bar == &vertical)
        view.setScrollY(start, area.getHeight());
    else
        view.setScrollQuarters(start);
    contentChanged();
}

void PianoRoll::handleAsyncUpdate()
{
    if (folded)
        rowsChanged();
    updateScrollBars();
}

void PianoRoll::contentChanged()
{
    contentDirty = true;
    repaint();
}

// --- Editing ---------------------------------------------------------------------------------------

void PianoRoll::applyEdit(const juce::String& name, const notes::Change& change)
{
    if (!hasClip() || change.empty())
        return;
    session.undoManager().beginNewTransaction(name);
    clipNotes.apply(session.set(), change);
}

void PianoRoll::editSelected(const juce::String& name, notes::Change (*make)(const notes::Notes&, const notes::Indices&))
{
    applyEdit(name, make(clipNotes.notes(), clipNotes.selectedOrAll()));
}

void PianoRoll::transposeSteps(int steps)
{
    applyEdit("Transpose", notes::transposeSteps(clipNotes.notes(), clipNotes.selectedOrAll(), steps));
}

void PianoRoll::transposeSemitones(double semitones)
{
    const auto context = rowContext();
    if (context.tuning != nullptr)
        applyEdit("Transpose", notes::transposeSemitones(clipNotes.notes(), clipNotes.selectedOrAll(), semitones, *context.tuning,
                                                         context.noteOffset));
}

void PianoRoll::nudge(int direction)
{
    const double step = gridStep() > 0.0 ? gridStep() : 0.25;
    applyEdit("Move notes", notes::move(clipNotes.notes(), clipNotes.selected(), step * direction, 0));
}

void PianoRoll::resizeBy(int direction)
{
    const double step = gridStep() > 0.0 ? gridStep() : 0.25;
    applyEdit("Resize notes", notes::resizeEnd(clipNotes.notes(), clipNotes.selected(), step * direction));
}

void PianoRoll::split()
{
    applyEdit("Split notes", notes::split(clipNotes.notes(), clipNotes.selected(), gridStep()));
}

void PianoRoll::join()
{
    applyEdit("Join notes", notes::join(clipNotes.notes(), clipNotes.selected()));
}

void PianoRoll::quantize()
{
    applyEdit("Quantize", notes::quantize(clipNotes.notes(), clipNotes.selectedOrAll(), gridStep(), quantizeAmount, quantizeEnds));
}

void PianoRoll::legato()
{
    if (hasClip())
        applyEdit("Legato", notes::legato(clipNotes.notes(), clipNotes.selectedOrAll(), clip()[ids::loopEnd]));
}

void PianoRoll::reverse()
{
    editSelected("Reverse", notes::reverse);
}

void PianoRoll::invert()
{
    editSelected("Invert", notes::invert);
}

void PianoRoll::stretch(double factor)
{
    applyEdit(factor > 1.0 ? "Stretch notes" : "Squeeze notes", notes::stretch(clipNotes.notes(), clipNotes.selectedOrAll(), factor));
}

void PianoRoll::doubleLoop()
{
    if (!hasClip())
        return;
    const double start = clip()[ids::loopStart], end = clip()[ids::loopEnd];
    session.undoManager().beginNewTransaction("Double loop");
    session.set().setClipLoop(clip(), start, start + (end - start) * 2.0);
}

void PianoRoll::halveLoop()
{
    if (!hasClip())
        return;
    const double start = clip()[ids::loopStart], end = clip()[ids::loopEnd];
    session.undoManager().beginNewTransaction("Halve loop");
    session.set().setClipLoop(clip(), start, start + juce::jmax(LiveSet::minLoopLength, (end - start) * 0.5));
}

void PianoRoll::duplicateLoop()
{
    if (!hasClip())
        return;
    const double start = clip()[ids::loopStart], end = clip()[ids::loopEnd];
    session.undoManager().beginNewTransaction("Duplicate loop");
    clipNotes.apply(session.set(), notes::duplicateLoop(clipNotes.notes(), start, end));
    session.set().setClipLoop(clip(), start, start + (end - start) * 2.0);
    updateScrollBars();
}

void PianoRoll::rampVelocities()
{
    // From the first note's velocity to the last one's, across the selection.
    const auto selected = clipNotes.selectedOrAll();
    if (selected.size() < 2)
        return;
    const auto& all = clipNotes.notes();
    auto first = selected.front(), last = selected.front();
    for (const auto i : selected)
    {
        if (all[i].start < all[first].start)
            first = i;
        if (all[i].start > all[last].start)
            last = i;
    }
    applyEdit("Ramp velocities", notes::rampVelocity(all, selected, all[first].velocity, all[last].velocity));
}

void PianoRoll::selectAll()
{
    clipNotes.select(notes::all(clipNotes.notes()), false);
}

void PianoRoll::toggleHighlight(int pitch)
{
    if (!hasClip())
        return;
    const int entry = pitch + static_cast<int>(trackTree[ids::noteOffset]);
    auto set = LiveSet::effectiveHighlight(clip());
    if (const auto it = std::find(set.begin(), set.end(), entry); it != set.end())
        set.erase(it);
    else
        set.push_back(entry);
    session.undoManager().beginNewTransaction("Highlight");
    session.set().setHighlight(clip(), set);
    rowsChanged();
}

void PianoRoll::toggleHighlightOfSelectedRows()
{
    if (!hasClip())
        return;
    const int offset = trackTree[ids::noteOffset];
    std::vector<int> rows;
    for (const auto i : clipNotes.selectedOrAll())
        rows.push_back(clipNotes.notes()[i].pitch + offset);
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    auto set = LiveSet::effectiveHighlight(clip());
    const bool allIn = std::all_of(rows.begin(), rows.end(), [&](int r) { return std::binary_search(set.begin(), set.end(), r); });
    for (const int r : rows)
    {
        const auto it = std::find(set.begin(), set.end(), r);
        if (allIn && it != set.end())
            set.erase(it);
        else if (!allIn && it == set.end())
            set.push_back(r);
    }
    session.undoManager().beginNewTransaction("Highlight");
    session.set().setHighlight(clip(), set);
    rowsChanged();
}

void PianoRoll::clearHighlight()
{
    if (!hasClip())
        return;
    session.undoManager().beginNewTransaction("Clear highlight");
    session.set().setHighlight(clip(), {});
    if (trackTree.isValid())
        session.set().setHighlight(trackTree, {});
    rowsChanged();
}

void PianoRoll::moveHighlightToTrack()
{
    if (!hasClip() || !trackTree.isValid())
        return;
    session.undoManager().beginNewTransaction("Highlight for the track");
    session.set().setHighlight(trackTree, LiveSet::effectiveHighlight(clip()));
    session.set().setHighlight(clip(), {});
    rowsChanged();
}

bool PianoRoll::canEdit(const juce::String& id) const
{
    if (!hasClip())
        return false;
    if (id == "edit.duplicate" || id == "edit.delete" || id == "edit.copy" || id == "edit.cut")
        return clipNotes.anySelected();
    if (id == "edit.paste")
        return !clipboard().empty();
    if (id == "edit.selectAll")
        return clipNotes.size() > 0;
    return false;
}

void PianoRoll::edit(const juce::String& id)
{
    const auto copySelected = [this] {
        auto& copied = clipboard();
        copied.clear();
        double from = std::numeric_limits<double>::max();
        for (const auto i : clipNotes.selected())
        {
            copied.push_back(clipNotes.notes()[i]);
            from = juce::jmin(from, copied.back().start);
        }
        for (auto& n : copied)
            n.start -= from;
    };
    if (id == "edit.duplicate")
        applyEdit("Duplicate notes", notes::duplicate(clipNotes.notes(), clipNotes.selected()));
    else if (id == "edit.delete")
        applyEdit("Delete notes", notes::remove(clipNotes.selected()));
    else if (id == "edit.copy")
        copySelected();
    else if (id == "edit.cut")
    {
        copySelected();
        applyEdit("Cut notes", notes::remove(clipNotes.selected()));
    }
    else if (id == "edit.paste")
    {
        notes::Change c;
        double end = insertQuarters;
        for (auto n : clipboard())
        {
            n.start += insertQuarters;
            end = juce::jmax(end, n.start + n.length);
            c.added.push_back(n);
        }
        applyEdit("Paste notes", c);
        insertQuarters = end; // the next paste follows this one
    }
    else if (id == "edit.selectAll")
        selectAll();
    updateScrollBars();
}

// --- Audition --------------------------------------------------------------------------------------

void PianoRoll::audition(int pitch, int velocity)
{
    if (pitch == auditioning)
        return;
    endAudition();
    if (const int t = track(); t >= 0 && pitch >= 0)
    {
        session.previewNote(t, pitch, velocity);
        auditioning = pitch;
    }
}

void PianoRoll::endAudition()
{
    if (auditioning < 0)
        return;
    if (const int t = track(); t >= 0)
        session.previewNote(t, auditioning, 0);
    session.endPreviews();
    auditioning = -1;
}

// --- Geometry --------------------------------------------------------------------------------------

juce::Rectangle<float> PianoRoll::noteBounds(std::size_t index) const
{
    const auto& all = clipNotes.notes();
    if (index >= all.size())
        return {};
    const auto& n = all[index];
    const int row = view.rowOf(n.pitch);
    if (row < 0)
        return {};
    const float x = xAt(n.start);
    const float w = juce::jmax(2.0f, static_cast<float>(n.length * view.pixelsPerQuarter()));
    const float top = static_cast<float>(area.getY() + view.rowTop(row));
    const float bottom = static_cast<float>(area.getY() + view.rowBottom(row));
    return {x, top, w, juce::jmax(1.0f, bottom - top)};
}

juce::Point<float> PianoRoll::pointFor(double quarters, int pitch) const
{
    const int row = view.rowOf(pitch);
    return {xAt(quarters), static_cast<float>(area.getY() + (row >= 0 ? view.rowCentre(row) : 0.0))};
}

PianoRoll::Hit PianoRoll::hitAt(juce::Point<float> p) const
{
    Hit hit;
    const auto& t = themeOf(*this);
    const float grab = juce::jmax(3.0f, t.spacing * 1.5f);
    hit.quarters = quartersAtX(p.x);

    if (timeline.toFloat().contains(p) && hasClip())
    {
        const auto& c = clip();
        const float half = static_cast<float>(timeline.getCentreY());
        const float loopStartX = xAt(c[ids::loopStart]), loopEndX = xAt(c[ids::loopEnd]);
        const float startX = xAt(c[ids::startMarker]);
        if (p.y < half && std::abs(p.x - startX) <= grab * 2.0f)
            hit.area = Area::startMarker;
        else if (p.y >= half && std::abs(p.x - loopStartX) <= grab)
            hit.area = Area::loopStart;
        else if (p.y >= half && std::abs(p.x - loopEndX) <= grab)
            hit.area = Area::loopEnd;
        else if (p.y >= half && p.x > loopStartX && p.x < loopEndX)
            hit.area = Area::loopBody;
        else
            hit.area = Area::time;
        return hit;
    }
    if (ruler.toFloat().contains(p))
    {
        hit.area = Area::key;
        hit.pitch = view.pitchAt(p.y - static_cast<float>(area.getY()));
        return hit;
    }
    if (velocities.toFloat().contains(p))
    {
        hit.area = Area::velocity;
        // The stem nearest the pointer, within grabbing distance; a selected one first.
        float best = grab * 1.5f;
        const auto& all = clipNotes.notes();
        for (std::size_t i = 0; i < all.size(); ++i)
        {
            const float d = std::abs(xAt(all[i].start) - p.x);
            if (d < best || (d <= best && clipNotes.isSelected(i)))
            {
                best = d;
                hit.note = static_cast<int>(i);
            }
        }
        return hit;
    }
    if (!area.toFloat().contains(p))
        return hit;

    hit.area = Area::empty;
    hit.pitch = view.pitchAt(p.y - static_cast<float>(area.getY()));
    // The topmost note under the pointer: the last one drawn (selected notes draw last).
    const auto& all = clipNotes.notes();
    int found = -1;
    for (std::size_t i = 0; i < all.size(); ++i)
    {
        if (all[i].pitch != hit.pitch)
            continue;
        const auto r = noteBounds(i);
        if (p.x >= r.getX() && p.x < r.getRight() && (found < 0 || clipNotes.isSelected(i) || !clipNotes.isSelected(static_cast<std::size_t>(found))))
            found = static_cast<int>(i);
    }
    if (found >= 0)
    {
        const auto r = noteBounds(static_cast<std::size_t>(found));
        const float edge = juce::jmin(grab, r.getWidth() / 4.0f);
        hit.note = found;
        hit.area = p.x >= r.getRight() - edge ? Area::noteEnd : p.x < r.getX() + edge ? Area::noteStart : Area::note;
    }
    return hit;
}

juce::String PianoRoll::describeAt(juce::Point<float> p) const
{
    const auto hit = hitAt(p);
    const auto context = rowContext();
    if (hit.area == Area::key && hit.pitch >= 0)
        return tuningRowDetails(context, hit.pitch);
    if ((hit.area == Area::note || hit.area == Area::noteStart || hit.area == Area::noteEnd) && hit.note >= 0)
    {
        const auto& n = clipNotes.notes()[static_cast<std::size_t>(hit.note)];
        const auto [num, den] = hasClip() ? session.set().clipTimeSignature(clip()) : std::pair{4, 4};
        const BarsBeats bb{num, den};
        return tuningRowDetails(context, n.pitch) + "\nVelocity " + juce::String(n.velocity) + juce::String::fromUTF8(" \xc2\xb7 at ") +
               bb.position(n.start) + juce::String::fromUTF8(" \xc2\xb7 length ") + bb.length(n.length);
    }
    if (hit.area == Area::empty && hit.pitch >= 0)
        return tuningRowDetails(context, hit.pitch);
    return {};
}

// --- Gestures --------------------------------------------------------------------------------------

void PianoRoll::press(juce::Point<float> p, juce::ModifierKeys mods)
{
    if (!hasClip())
        return;
    pressPoint = p;
    pressed = hitAt(p);
    pressNotes = clipNotes.notes();
    pressSelection = clipNotes.selectionFlags();
    pressLoopStart = clip()[ids::loopStart];
    pressLoopEnd = clip()[ids::loopEnd];
    pressStartMarker = clip()[ids::startMarker];
    lastDragRows = 0;
    gesture = Gesture::none;
    const bool adding = mods.isShiftDown() || mods.isCommandDown();
    auto& um = session.undoManager();

    switch (pressed.area)
    {
    case Area::startMarker:
        um.beginNewTransaction("Move start marker");
        gesture = Gesture::startMarker;
        break;
    case Area::loopStart:
    case Area::loopEnd:
    case Area::loopBody:
        um.beginNewTransaction("Change loop");
        gesture = pressed.area == Area::loopStart ? Gesture::loopStart : pressed.area == Area::loopEnd ? Gesture::loopEnd : Gesture::loopBody;
        break;
    case Area::key:
        if (pressed.pitch >= 0)
        {
            const auto row = notes::onRow(pressNotes, pressed.pitch);
            clipNotes.select(row, adding);
            audition(pressed.pitch, 100);
            gesture = Gesture::key;
        }
        break;
    case Area::note:
    case Area::noteStart:
    case Area::noteEnd:
    {
        const auto index = static_cast<std::size_t>(pressed.note);
        if (drawMode && pressed.area == Area::note)
        {
            // Draw mode: a click on a note deletes it.
            applyEdit("Delete note", notes::remove({index}));
            break;
        }
        if (mods.isCommandDown() && clipNotes.isSelected(index))
            clipNotes.deselect({index});
        else if (!clipNotes.isSelected(index))
            clipNotes.select({index}, adding);
        pressSelection = clipNotes.selectionFlags();
        um.beginNewTransaction(pressed.area == Area::note ? "Move notes" : "Resize notes");
        gesture = pressed.area == Area::noteStart ? Gesture::resizeStart
                  : pressed.area == Area::noteEnd ? Gesture::resizeEnd
                  : mods.isAltDown()              ? Gesture::copyNotes
                                                  : Gesture::moveNotes;
        if (clipNotes.isSelected(index))
            audition(pressNotes[index].pitch, pressNotes[index].velocity);
        break;
    }
    case Area::empty:
    {
        const double step = snapStep();
        const double at = juce::jmax(0.0, step > 0.0 ? NoteGrid::floor(pressed.quarters, step) : pressed.quarters);
        insertQuarters = at;
        if (drawMode && pressed.pitch >= 0)
        {
            const double length = gridStep() > 0.0 ? gridStep() : 0.25;
            notes::Change c;
            c.added.push_back({pressed.pitch, at, length, 100, false});
            applyEdit("Draw note", c);
            pressNotes = clipNotes.notes();
            pressed.note = static_cast<int>(pressNotes.size()) - 1;
            gesture = Gesture::drawNote;
            audition(pressed.pitch, 100);
            break;
        }
        if (!adding)
            clipNotes.clearSelection();
        pressSelection = clipNotes.selectionFlags();
        gesture = Gesture::marquee;
        break;
    }
    case Area::velocity:
        um.beginNewTransaction("Change velocity");
        if (mods.isShiftDown())
            gesture = Gesture::velocityRamp;
        else if (drawMode)
            gesture = Gesture::velocityDraw;
        else if (pressed.note >= 0)
        {
            if (!clipNotes.isSelected(static_cast<std::size_t>(pressed.note)))
                clipNotes.select({static_cast<std::size_t>(pressed.note)}, false);
            pressSelection = clipNotes.selectionFlags();
            gesture = Gesture::velocity;
        }
        break;
    case Area::time:
    case Area::none:
        break;
    }
}

void PianoRoll::drag(juce::Point<float> p, juce::ModifierKeys)
{
    if (!hasClip() || gesture == Gesture::none)
        return;
    const double step = snapStep();
    const double raw = quartersAtX(p.x) - quartersAtX(pressPoint.x);
    const auto snapTime = [step](double q) { return step > 0.0 ? NoteGrid::nearest(q, step) : q; };
    auto& set = session.set();
    const auto selection = indicesOf(pressSelection);

    switch (gesture)
    {
    case Gesture::moveNotes:
    case Gesture::copyNotes:
    {
        // The pressed note's start lands on the grid; the others keep their places around it.
        const auto& anchor = pressNotes[static_cast<std::size_t>(pressed.note)];
        const double dq = snapTime(anchor.start + raw) - anchor.start;
        const int pitchNow = view.pitchAt(p.y - static_cast<float>(area.getY()));
        const int dr = pitchNow >= 0 ? pitchNow - anchor.pitch : 0;
        if (gesture == Gesture::copyNotes)
        {
            if (std::abs(p.x - pressPoint.x) < 3.0f && dr == 0)
                return; // not a drag yet: an Alt+click
            // The first move makes the copies; after that they're what moves.
            clipNotes.apply(set, notes::copyMoved(pressNotes, selection, dq, dr));
            const auto copies = clipNotes.selected();
            pressNotes = clipNotes.notes();
            for (std::size_t k = 0; k < copies.size(); ++k)
                pressNotes[copies[k]] = pressNotes[selection[k]];
            pressSelection = clipNotes.selectionFlags();
            pressed.note = static_cast<int>(copies[static_cast<std::size_t>(std::find(selection.begin(), selection.end(),
                                                                                       static_cast<std::size_t>(pressed.note)) -
                                                                             selection.begin())]);
            gesture = Gesture::moveNotes;
            drag(p, {});
            return;
        }
        const auto [cq, cr] = notes::clampMove(pressNotes, selection, dq, dr);
        notes::Change c;
        for (const auto i : selection)
        {
            auto n = pressNotes[i];
            n.start += cq;
            n.pitch += cr;
            c.changed.emplace_back(i, n);
        }
        notes::apply(set, clip(), c);
        if (cr != lastDragRows)
        {
            audition(anchor.pitch + cr, anchor.velocity);
            lastDragRows = cr;
        }
        break;
    }
    case Gesture::resizeEnd:
    case Gesture::resizeStart:
    {
        const auto& anchor = pressNotes[static_cast<std::size_t>(pressed.note)];
        notes::Change c;
        if (gesture == Gesture::resizeEnd)
        {
            const double end = anchor.start + anchor.length;
            c = notes::resizeEnd(pressNotes, selection, snapTime(end + raw) - end);
        }
        else
            c = notes::resizeStart(pressNotes, selection, snapTime(anchor.start + raw) - anchor.start);
        // Values relative to the press, so notes not changed by this step go back.
        notes::Change all;
        for (const auto i : selection)
            all.changed.emplace_back(i, pressNotes[i]);
        for (const auto& [i, d] : c.changed)
            for (auto& entry : all.changed)
                if (entry.first == i)
                    entry.second = d;
        notes::apply(set, clip(), all);
        break;
    }
    case Gesture::drawNote:
    {
        const auto index = static_cast<std::size_t>(pressed.note);
        auto n = pressNotes[index];
        const double minimum = gridStep() > 0.0 ? gridStep() : notes::minLength;
        n.length = juce::jmax(minimum, snapTime(quartersAtX(p.x)) - n.start);
        notes::Change c;
        c.changed.emplace_back(index, n);
        notes::apply(set, clip(), c);
        break;
    }
    case Gesture::marquee:
    {
        marqueeRect = juce::Rectangle<float>(pressPoint, p).getIntersection(area.toFloat());
        const double from = quartersAtX(marqueeRect.getX()), to = quartersAtX(marqueeRect.getRight());
        const int high = view.pitchAt(marqueeRect.getY() - static_cast<float>(area.getY()));
        const int low = view.pitchAt(marqueeRect.getBottom() - static_cast<float>(area.getY()));
        auto chosen = pressSelection;
        for (const auto i : notes::inArea(pressNotes, from, to, juce::jmin(low, high), juce::jmax(low, high)))
            if (view.isShown(pressNotes[i].pitch))
                chosen[i] = 1;
        clipNotes.setSelectionFlags(chosen);
        break;
    }
    case Gesture::startMarker:
        set.setClipProperty(clip(), ids::startMarker, juce::jmax(0.0, snapTime(pressStartMarker + raw)));
        break;
    case Gesture::loopStart:
        set.setClipLoop(clip(), juce::jmin(snapTime(pressLoopStart + raw), pressLoopEnd - LiveSet::minLoopLength), pressLoopEnd);
        break;
    case Gesture::loopEnd:
        set.setClipLoop(clip(), pressLoopStart, juce::jmax(snapTime(pressLoopEnd + raw), pressLoopStart + LiveSet::minLoopLength));
        break;
    case Gesture::loopBody:
    {
        const double start = juce::jmax(0.0, snapTime(pressLoopStart + raw));
        set.setClipLoop(clip(), start, start + (pressLoopEnd - pressLoopStart));
        break;
    }
    case Gesture::velocity:
    {
        const int delta = juce::roundToInt((pressPoint.y - p.y) / juce::jmax(1.0f, static_cast<float>(velocities.getHeight())) * 127.0f);
        notes::Change c;
        for (const auto i : selection)
        {
            auto n = pressNotes[i];
            n.velocity = juce::jlimit(1, 127, n.velocity + delta);
            c.changed.emplace_back(i, n);
        }
        notes::apply(set, clip(), c);
        break;
    }
    case Gesture::velocityDraw:
    case Gesture::velocityRamp:
    {
        // Draw: the stems the pointer passes get its height. Ramp: a line from the press to the pointer.
        const auto velocityAt = [this](float y) {
            return juce::jlimit(1, 127, juce::roundToInt((static_cast<float>(velocities.getBottom()) - y) /
                                                         juce::jmax(1.0f, static_cast<float>(velocities.getHeight())) * 127.0f));
        };
        const float x0 = juce::jmin(pressPoint.x, p.x), x1 = juce::jmax(pressPoint.x, p.x);
        const bool onlySelected = gesture == Gesture::velocityRamp && std::any_of(pressSelection.begin(), pressSelection.end(), [](char f) { return f != 0; });
        notes::Change c;
        const auto& now = clipNotes.notes();
        for (std::size_t i = 0; i < pressNotes.size(); ++i)
        {
            const float x = xAt(pressNotes[i].start);
            if (onlySelected && pressSelection[i] == 0)
                continue;
            auto n = now[i];
            if (x >= x0 && x <= x1)
            {
                if (gesture == Gesture::velocityRamp)
                {
                    const float t = p.x != pressPoint.x ? (x - pressPoint.x) / (p.x - pressPoint.x) : 0.0f;
                    n.velocity = velocityAt(pressPoint.y + (p.y - pressPoint.y) * t);
                }
                else
                    n.velocity = velocityAt(p.y);
            }
            else if (gesture == Gesture::velocityRamp)
                n.velocity = pressNotes[i].velocity; // outside the line now: as it was
            if (!(n == now[i]))
                c.changed.emplace_back(i, n);
        }
        notes::apply(set, clip(), c);
        if (gesture == Gesture::velocityDraw)
            pressPoint.x = p.x; // drawing continues from here
        break;
    }
    case Gesture::key:
    {
        const int pitch = view.pitchAt(p.y - static_cast<float>(area.getY()));
        if (pitch >= 0 && pitch != auditioning)
            audition(pitch, 100);
        break;
    }
    case Gesture::none:
        break;
    }
    updateScrollBars();
}

void PianoRoll::release()
{
    endAudition();
    if (gesture == Gesture::copyNotes)
    {
        // An Alt+click without a drag selected the note; nothing to copy.
    }
    gesture = Gesture::none;
    marqueeRect = {};
    repaint();
}

void PianoRoll::doubleClick(juce::Point<float> p)
{
    const auto hit = hitAt(p);
    if (!hasClip() || drawMode)
        return;
    if (hit.area == Area::empty && hit.pitch >= 0)
    {
        const double step = snapStep();
        const double at = juce::jmax(0.0, step > 0.0 ? NoteGrid::floor(hit.quarters, step) : hit.quarters);
        notes::Change c;
        c.added.push_back({hit.pitch, at, gridStep() > 0.0 ? gridStep() : 0.25, 100, false});
        applyEdit("Add note", c);
    }
    else if (hit.area == Area::note && hit.note >= 0)
        applyEdit("Delete note", notes::remove({static_cast<std::size_t>(hit.note)}));
}

// --- Mouse and keys --------------------------------------------------------------------------------

void PianoRoll::mouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    if (e.mods.isPopupMenu())
    {
        showMenu(e.position);
        return;
    }
    press(e.position, e.mods);
}

void PianoRoll::mouseDrag(const juce::MouseEvent& e)
{
    if (!e.mods.isPopupMenu())
        drag(e.position, e.mods);
}

void PianoRoll::mouseUp(const juce::MouseEvent&)
{
    release();
}

void PianoRoll::mouseDoubleClick(const juce::MouseEvent& e)
{
    doubleClick(e.position);
}

void PianoRoll::mouseMove(const juce::MouseEvent& e)
{
    const auto hit = hitAt(e.position);
    using MC = juce::MouseCursor;
    setMouseCursor(hit.area == Area::noteStart || hit.area == Area::noteEnd || hit.area == Area::loopStart ||
                           hit.area == Area::loopEnd
                       ? MC::LeftRightResizeCursor
                   : drawMode && (hit.area == Area::empty || hit.area == Area::note) ? MC::CrosshairCursor
                                                                                    : MC::NormalCursor);
    const auto text = describeAt(e.position);
    if (text != getHelpText())
    {
        setHelpText(text);
        if (onHover)
            onHover(text);
    }
}

void PianoRoll::mouseExit(const juce::MouseEvent&)
{
    setHelpText({});
}

void PianoRoll::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const float amount = std::abs(w.deltaY) > std::abs(w.deltaX) ? w.deltaY : -w.deltaX;
    if (e.mods.isCommandDown() && e.mods.isShiftDown())
        setRowZoom(rowScale * (amount > 0.0f ? 1.15f : 1.0f / 1.15f));
    else if (e.mods.isCommandDown())
        setZoomX(view.pixelsPerQuarter() * (amount > 0.0f ? 1.2 : 1.0 / 1.2), e.position.x - static_cast<float>(area.getX()));
    else if (e.mods.isShiftDown() || std::abs(w.deltaX) > std::abs(w.deltaY))
        setScroll(view.scrollQuarters() - amount * area.getWidth() * 0.25 / view.pixelsPerQuarter(), view.scrollY());
    else
        setScroll(view.scrollQuarters(), view.scrollY() - amount * view.averageRowHeight() * 12.0);
}

bool PianoRoll::keyPressed(const juce::KeyPress& key)
{
    if (!hasClip())
        return false;
    const auto mods = key.getModifiers();
    const bool any = clipNotes.anySelected();
    const int code = key.getKeyCode();
    if (code == juce::KeyPress::escapeKey && any)
    {
        clipNotes.clearSelection();
        return true;
    }
    if (!any || mods.isCommandDown() || mods.isAltDown())
        return false; // the Session view's arrows, and everything else, go on to the commands
    const int page = [this] {
        const auto context = rowContext();
        return context.tuning != nullptr ? juce::jmax(1, context.tuning->keyboardPageSize) : 12;
    }();
    if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey)
    {
        transposeSteps((code == juce::KeyPress::upKey ? 1 : -1) * (mods.isShiftDown() ? page : 1));
        const auto selected = clipNotes.selected();
        if (!selected.empty())
        {
            const auto& n = clipNotes.notes()[selected.front()];
            audition(n.pitch, n.velocity);
            endAudition(); // a short blip: the note's new pitch
        }
        return true;
    }
    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey)
    {
        const int direction = code == juce::KeyPress::rightKey ? 1 : -1;
        if (mods.isShiftDown())
            resizeBy(direction);
        else
            nudge(direction);
        return true;
    }
    return false;
}

void PianoRoll::showMenu(juce::Point<float> p)
{
    const auto hit = hitAt(p);
    juce::PopupMenu menu;
    if (hit.area == Area::key && hit.pitch >= 0)
    {
        const auto context = rowContext();
        const auto row = tuningRow(context, hit.pitch);
        menu.addItem(row.highlighted ? "Stop Highlighting This Row" : "Highlight This Row", [this, pitch = hit.pitch] { toggleHighlight(pitch); });
        menu.addItem("Highlight the Notes' Rows", hasClip(), false, [this] { toggleHighlightOfSelectedRows(); });
        menu.addItem("Use This Highlight for the Track", clip().hasProperty(ids::highlight), false, [this] { moveHighlightToTrack(); });
        menu.addItem("Clear Highlights", [this] { clearHighlight(); });
    }
    else if (commands != nullptr)
    {
        if (hit.note >= 0 && !clipNotes.isSelected(static_cast<std::size_t>(hit.note)))
            clipNotes.select({static_cast<std::size_t>(hit.note)}, false);
        auto& manager = commands->manager();
        for (const auto* id : {"edit.cut", "edit.copy", "edit.paste", "edit.duplicate", "edit.delete", "edit.selectAll"})
            menu.addCommandItem(&manager, commands->numericIdOf(id));
        menu.addSeparator();
        for (const auto* id : {"clip.split", "clip.join", "clip.quantize", "clip.legato", "clip.reverse", "clip.invert",
                               "clip.stretchDouble", "clip.stretchHalve", "clip.rampVelocities"})
            if (commands->numericIdOf(id) != 0)
                menu.addCommandItem(&manager, commands->numericIdOf(id));
    }
    if (menu.getNumItems() > 0)
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition());
}

// --- The playhead ----------------------------------------------------------------------------------

double PianoRoll::playheadQuarters() const
{
    const int t = track();
    if (!hasClip() || t < 0)
        return -1.0;
    const auto& play = feed.trackPlay(t);
    return play.playingClipId == LiveSet::idOf(clip()) ? play.position : -1.0;
}

void PianoRoll::engineStateChanged(std::uint32_t)
{
    const double now = playheadQuarters();
    if (now == shownPlayhead)
        return;
    // Follow: a page on when the playhead leaves the view (not while editing).
    if (follow && now >= 0.0 && gesture == Gesture::none && area.getWidth() > 0)
    {
        const double left = view.scrollQuarters(), right = view.quartersAt(area.getWidth());
        if (now >= right || now < left)
        {
            shownPlayhead = now;
            setScroll(juce::jmax(0.0, now - (right - left) * 0.05), view.scrollY());
            return;
        }
    }
    repaintPlayhead(shownPlayhead);
    shownPlayhead = now;
    repaintPlayhead(now);
}

void PianoRoll::repaintPlayhead(double quarters)
{
    if (quarters < 0.0)
        return;
    const int x = juce::roundToInt(xAt(quarters));
    repaint(x - 2, timeline.getY(), 5, (showVelocities ? velocities.getBottom() : area.getBottom()) - timeline.getY());
}

// --- Painting --------------------------------------------------------------------------------------

juce::Colour PianoRoll::noteColour() const
{
    const auto& t = themeOf(*this);
    return colourOf(clip(), colourOf(trackTree, t.accent));
}

void PianoRoll::resized()
{
    const auto& t = themeOf(*this);
    auto r = getLocalBounds();
    const int bar = juce::roundToInt(t.spacing * 3.0f);
    const int rulerWidth = juce::roundToInt(t.controlHeight * 4.5f);
    const int timeHeight = juce::roundToInt(t.controlHeight * 1.1f);

    horizontal.setBounds(r.removeFromBottom(bar).withTrimmedLeft(rulerWidth).withTrimmedRight(bar));
    auto right = r.removeFromRight(bar);
    auto top = r.removeFromTop(timeHeight);
    corner = top.removeFromLeft(rulerWidth);
    timeline = top;
    velocities = {};
    if (showVelocities)
    {
        const int laneHeight = juce::jlimit(juce::roundToInt(t.controlHeight * 2.0f), juce::roundToInt(t.controlHeight * 4.0f), r.getHeight() / 4);
        velocities = r.removeFromBottom(laneHeight).withTrimmedLeft(rulerWidth);
        r.removeFromBottom(1);
    }
    ruler = r.removeFromLeft(rulerWidth);
    area = r;
    vertical.setBounds(right.withTrimmedTop(timeHeight).withHeight(area.getHeight()));

    const bool first = content.isNull();
    view.setScrollY(view.scrollY(), area.getHeight());
    updateScrollBars();
    if (first && hasClip())
        zoomToClip();
    contentChanged();
}

void PianoRoll::paint(juce::Graphics& g)
{
    const auto& t = themeOf(*this);
    g.fillAll(t.surface);
    if (!hasClip())
    {
        g.setColour(t.textDim);
        g.setFont(t.font(t.fontNormal));
        g.drawFittedText("No clip selected. Select a clip in the Session view, or double-click an empty slot to make one.",
                         getLocalBounds().reduced(juce::roundToInt(t.padding * 2.0f)), juce::Justification::centred, 3);
        return;
    }

    // The grid, notes and velocities, from the cache (redrawn only after they or the view change).
    const auto contentBounds = area.getUnion(velocities);
    const float scale = juce::jmax(1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
    const int w = juce::roundToInt(static_cast<float>(contentBounds.getWidth()) * scale);
    const int h = juce::roundToInt(static_cast<float>(contentBounds.getHeight()) * scale);
    if (w > 0 && h > 0)
    {
        if (content.isNull() || content.getWidth() != w || content.getHeight() != h)
        {
            content = juce::Image(juce::Image::ARGB, w, h, false);
            contentDirty = true;
        }
        if (contentDirty)
        {
            juce::Graphics cg(content);
            cg.addTransform(juce::AffineTransform::translation(static_cast<float>(-contentBounds.getX()), static_cast<float>(-contentBounds.getY()))
                                .scaled(scale));
            renderContent(cg, contentBounds);
            contentDirty = false;
        }
        g.drawImage(content, contentBounds.toFloat());
    }

    paintRuler(g);
    paintTimeline(g);

    // The corner: the grid now, and the draw mode.
    g.setColour(t.surfaceRaised);
    g.fillRect(corner);
    g.setColour(t.textDim);
    g.setFont(t.font(t.fontSmall));
    g.drawFittedText(noteGrid.describe(view.pixelsPerQuarter(), quartersPerBar(), gridMinPixels()) + (drawMode ? "  Draw" : ""),
                     corner.reduced(juce::roundToInt(t.spacing)), juce::Justification::centredLeft, 1);

    if (!marqueeRect.isEmpty())
    {
        g.setColour(t.selection.withAlpha(0.2f));
        g.fillRect(marqueeRect);
        g.setColour(t.selection);
        g.drawRect(marqueeRect, 1.0f);
    }
    paintPlayhead(g);
    if (hasKeyboardFocus(false))
    {
        g.setColour(t.focus);
        g.drawRect(area, 1);
    }
}

void PianoRoll::renderContent(juce::Graphics& g, juce::Rectangle<int> bounds)
{
    const auto& t = themeOf(*this);
    g.setColour(t.surfaceSunken);
    g.fillRect(bounds);
    const auto context = rowContext();
    const auto top = static_cast<float>(area.getY());
    const auto left = static_cast<float>(area.getX()), right = static_cast<float>(area.getRight());

    // Rows: black keys darker in 12-TET; highlighted entries tinted; rows the track can't play shaded.
    const int firstRow = view.rowAt(0.0), lastRow = view.rowAt(area.getHeight());
    for (int row = firstRow; row <= lastRow && row < view.numRows(); ++row)
    {
        const auto info = tuningRow(context, view.pitchAtRow(row));
        const float y0 = top + static_cast<float>(view.rowTop(row)), y1 = top + static_cast<float>(view.rowBottom(row));
        auto fill = info.namedNotes && info.blackKey ? t.surfaceSunken : t.surface;
        if (info.highlighted)
            fill = fill.interpolatedWith(t.accent, 0.22f);
        if (info.unplayable)
            fill = fill.interpolatedWith(t.background, 0.6f);
        g.setColour(fill);
        g.fillRect(left, y0, right - left, y1 - y0);
        g.setColour(t.border.withAlpha(info.octaveStart ? 0.9f : 0.35f));
        g.fillRect(left, y1 - 1.0f, right - left, 1.0f);
    }

    // Time: outside the loop darker; then the grid, beat and bar lines.
    const auto& c = clip();
    const bool looping = c[ids::looping];
    const float loopStartX = xAt(c[ids::loopStart]), loopEndX = xAt(c[ids::loopEnd]);
    g.setColour(t.background.withAlpha(0.45f));
    if (looping && loopStartX > left)
        g.fillRect(left, top, loopStartX - left, static_cast<float>(area.getHeight()));
    if (loopEndX < right)
        g.fillRect(juce::jmax(left, loopEndX), top, right - juce::jmax(left, loopEndX), static_cast<float>(area.getHeight()));

    const double bar = quartersPerBar();
    const auto [num, den] = session.set().clipTimeSignature(c);
    juce::ignoreUnused(num);
    const double beat = 4.0 / den;
    const double step = gridStep();
    const double from = view.scrollQuarters(), to = view.quartersAt(area.getWidth());
    const auto lines = [&](double every, juce::Colour colour) {
        if (every <= 0.0 || every * view.pixelsPerQuarter() < 3.0)
            return;
        g.setColour(colour);
        for (double q = NoteGrid::floor(from, every); q <= to; q += every)
        {
            const float x = xAt(q);
            if (x >= left)
                g.fillRect(x, top, 1.0f, static_cast<float>(area.getHeight()));
        }
    };
    lines(step, t.border.withAlpha(0.25f));
    lines(beat, t.border.withAlpha(0.5f));
    lines(bar, t.border);

    // Notes: the clip's colour, paler for softer notes; selected ones in the selection colour, on top.
    const auto& all = clipNotes.notes();
    const auto base = noteColour();
    const float ppq = static_cast<float>(view.pixelsPerQuarter());
    const auto drawNotes = [&](bool selected) {
        for (std::size_t i = 0; i < all.size(); ++i)
        {
            if (clipNotes.isSelected(i) != selected)
                continue;
            const auto& n = all[i];
            const float x = xAt(n.start);
            const float w = juce::jmax(2.0f, static_cast<float>(n.length) * ppq);
            if (x > right || x + w < left)
                continue;
            const int row = view.rowOf(n.pitch);
            if (row < firstRow || row > lastRow)
                continue;
            const float y0 = top + static_cast<float>(view.rowTop(row)), y1 = top + static_cast<float>(view.rowBottom(row));
            const auto r = juce::Rectangle<float>(x, y0, w, juce::jmax(1.0f, y1 - y0)).reduced(0.0f, (y1 - y0) > 4.0f ? 1.0f : 0.0f);
            auto fill = n.muted ? t.textDisabled : base.withMultipliedSaturation(0.35f + 0.65f * static_cast<float>(n.velocity) / 127.0f);
            if (selected)
                fill = t.selection;
            g.setColour(fill);
            g.fillRect(r);
            g.setColour(selected ? t.text : fill.darker(0.5f));
            g.drawRect(r, 1.0f);
        }
    };
    drawNotes(false);
    drawNotes(true);

    // Velocities: a stem per note, its height the velocity.
    if (!velocities.isEmpty())
    {
        g.setColour(t.surfaceSunken.darker(0.15f));
        g.fillRect(velocities);
        g.setColour(t.border);
        g.fillRect(velocities.getX(), velocities.getY(), velocities.getWidth(), 1);
        const float vb = static_cast<float>(velocities.getBottom()), vh = static_cast<float>(velocities.getHeight() - 3);
        const auto stems = [&](bool selected) {
            for (std::size_t i = 0; i < all.size(); ++i)
            {
                if (clipNotes.isSelected(i) != selected || !view.isShown(all[i].pitch))
                    continue;
                const float x = xAt(all[i].start);
                if (x < left - 3.0f || x > right + 3.0f)
                    continue;
                const float y = vb - vh * static_cast<float>(all[i].velocity) / 127.0f;
                g.setColour(selected ? t.selection : base);
                g.fillRect(x, y, 1.5f, vb - y);
                g.fillRect(x - 2.0f, y - 1.5f, 5.0f, 3.0f);
            }
        };
        stems(false);
        stems(true);
    }
}

void PianoRoll::paintRuler(juce::Graphics& g)
{
    const auto& t = themeOf(*this);
    juce::Graphics::ScopedSaveState saved(g);
    g.reduceClipRegion(ruler);
    g.setColour(t.surfaceRaised);
    g.fillRect(ruler);
    const auto context = rowContext();
    const auto top = static_cast<float>(area.getY());
    const auto x0 = static_cast<float>(ruler.getX()), width = static_cast<float>(ruler.getWidth());
    const float keyWidth = width * 0.35f;
    g.setFont(t.font(t.fontSmall));
    float lastLabelBottom = -1.0e9f;
    // Keys: white ones the lighter of the text and background colours, black ones the darker, in any theme.
    const bool lightText = t.text.getPerceivedBrightness() > t.background.getPerceivedBrightness();
    const auto whiteKey = (lightText ? t.text : t.background).interpolatedWith(t.surfaceRaised, 0.15f);
    const auto blackKey = lightText ? t.background : t.text.interpolatedWith(t.surfaceRaised, 0.2f);
    const int firstRow = view.rowAt(0.0), lastRow = view.rowAt(area.getHeight());
    for (int row = firstRow; row <= lastRow && row < view.numRows(); ++row)
    {
        const auto info = tuningRow(context, view.pitchAtRow(row));
        const float y0 = top + static_cast<float>(view.rowTop(row)), y1 = top + static_cast<float>(view.rowBottom(row));
        const float h = y1 - y0;
        auto fill = t.surfaceRaised;
        if (info.namedNotes)
            fill = info.blackKey ? blackKey : whiteKey;
        if (info.highlighted)
            fill = fill.interpolatedWith(t.accent, 0.35f);
        if (info.unplayable)
            fill = fill.interpolatedWith(t.background, 0.7f);
        g.setColour(fill);
        if (info.namedNotes)
            g.fillRect(x0 + width - keyWidth, y0, keyWidth, h);
        else
            g.fillRect(x0, y0, width, h);
        g.setColour(t.border.withAlpha(info.octaveStart ? 0.9f : 0.4f));
        g.fillRect(x0, y1 - 1.0f, width, 1.0f);
        if (info.anchor)
        {
            g.setColour(t.play);
            g.fillRect(x0 + width - juce::jmax(3.0f, t.spacing), y0, juce::jmax(3.0f, t.spacing), h);
        }
        if (info.unplayable)
        {
            // A diagonal mark: "won't sound".
            g.setColour(t.textDisabled);
            g.drawLine(x0 + 2.0f, y1 - 1.0f, x0 + 2.0f + h, y0 + 1.0f, 1.0f);
        }

        // Every row's label when rows are tall enough; otherwise only anchors and octave starts, where
        // there's room below the last label.
        const float needed = t.fontSmall + 1.0f;
        const float labelY = (y0 + y1 - needed) * 0.5f;
        if (h >= needed || ((info.anchor || info.octaveStart) && labelY >= lastLabelBottom))
        {
            g.setColour(info.unplayable ? t.textDisabled : info.anchor ? t.text : t.textDim);
            g.setFont(info.anchor ? juce::Font(t.font(t.fontSmall)).boldened() : juce::Font(t.font(t.fontSmall)));
            const float textLeft = x0 + 4.0f + (info.unplayable ? h + 2.0f : 0.0f);
            const float textRight = x0 + (info.namedNotes ? width - keyWidth : width - juce::jmax(3.0f, t.spacing)) - 3.0f;
            g.drawText(info.label, juce::Rectangle<float>(textLeft, labelY, juce::jmax(0.0f, textRight - textLeft), needed),
                       juce::Justification::centredRight, true);
            lastLabelBottom = labelY + needed;
        }
    }
    g.setColour(t.border);
    g.fillRect(ruler.getRight() - 1, ruler.getY(), 1, ruler.getHeight());
}

void PianoRoll::paintTimeline(juce::Graphics& g)
{
    const auto& t = themeOf(*this);
    juce::Graphics::ScopedSaveState saved(g);
    g.reduceClipRegion(timeline);
    g.setColour(t.surfaceRaised);
    g.fillRect(timeline);
    const auto& c = clip();
    const auto [num, den] = session.set().clipTimeSignature(c);
    const BarsBeats bb{num, den};
    const double bar = bb.quartersPerBar();
    const double from = view.scrollQuarters(), to = view.quartersAt(timeline.getWidth());
    const float half = static_cast<float>(timeline.getCentreY());

    // Bar numbers, as many as fit, clear of the start marker.
    const float markerWidth = half - static_cast<float>(timeline.getY()) - 2.0f;
    g.setFont(t.font(t.fontSmall));
    const double labelEvery = bar * std::pow(2.0, std::ceil(std::log2(juce::jmax(1.0, t.fontSmall * 3.0 / (bar * view.pixelsPerQuarter())))));
    for (double q = NoteGrid::floor(from, labelEvery); q <= to; q += labelEvery)
    {
        const float x = xAt(q);
        g.setColour(t.border);
        g.fillRect(x, static_cast<float>(timeline.getY()), 1.0f, half - static_cast<float>(timeline.getY()));
        g.setColour(t.textDim);
        g.drawText(juce::String(juce::roundToInt(q / bar) + 1),
                   juce::Rectangle<float>(x + markerWidth + 3.0f, static_cast<float>(timeline.getY()), t.fontSmall * 4.0f, half - static_cast<float>(timeline.getY())),
                   juce::Justification::centredLeft, false);
    }

    // The loop brace, and the start marker.
    const bool looping = c[ids::looping];
    const float ls = xAt(c[ids::loopStart]), le = xAt(c[ids::loopEnd]);
    const auto brace = juce::Rectangle<float>(ls, half + 2.0f, juce::jmax(1.0f, le - ls), static_cast<float>(timeline.getBottom()) - half - 4.0f);
    g.setColour(looping ? t.accent : t.textDisabled);
    g.fillRect(brace);
    g.setColour(t.accentText);
    g.setFont(t.font(t.fontSmall));
    if (brace.getWidth() > t.fontSmall * 4.0f)
        g.drawText(looping ? "Loop" : "End", brace.reduced(4.0f, 0.0f), juce::Justification::centredLeft, false);
    const float sx = xAt(c[ids::startMarker]);
    juce::Path marker;
    marker.addTriangle(sx, static_cast<float>(timeline.getY()) + 1.0f, sx + markerWidth, static_cast<float>(timeline.getY()) + 1.0f, sx,
                       half - 1.0f);
    g.setColour(t.text);
    g.fillPath(marker);
    g.setColour(t.border);
    g.fillRect(timeline.getX(), timeline.getBottom() - 1, timeline.getWidth(), 1);
}

void PianoRoll::paintPlayhead(juce::Graphics& g)
{
    const double q = playheadQuarters();
    if (q < 0.0)
        return;
    const float x = xAt(q);
    if (x < static_cast<float>(area.getX()) || x > static_cast<float>(area.getRight()))
        return;
    const auto& t = themeOf(*this);
    g.setColour(t.play);
    const int bottom = showVelocities ? velocities.getBottom() : area.getBottom();
    g.fillRect(x - 0.5f, static_cast<float>(timeline.getY()), 1.5f, static_cast<float>(bottom - timeline.getY()));
}

} // namespace lattice::ui
