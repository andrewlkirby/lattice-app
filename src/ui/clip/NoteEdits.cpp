#include "ui/clip/NoteEdits.h"

#include "tuning/TuningTable.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>

namespace lattice::ui::notes
{
namespace
{
constexpr double eps = 1.0e-9;

double endOf(const NoteData& n) noexcept
{
    return n.start + n.length;
}

/** The selection's span: the first start and the last end. */
std::pair<double, double> spanOf(const Notes& notes, const Indices& selected)
{
    double from = std::numeric_limits<double>::max(), to = std::numeric_limits<double>::lowest();
    for (const auto i : selected)
    {
        from = std::min(from, notes[i].start);
        to = std::max(to, endOf(notes[i]));
    }
    return {from, to};
}

/** The same edit to each selected note. */
template <typename F>
Change each(const Notes& notes, const Indices& selected, F&& f)
{
    Change c;
    for (const auto i : selected)
    {
        auto n = notes[i];
        f(n);
        if (!(n == notes[i]))
            c.changed.emplace_back(i, n);
    }
    return c;
}

} // namespace

std::vector<char> selectionAfter(std::size_t numNotesBefore, const Change& change)
{
    std::vector<char> flags(numNotesBefore + change.added.size(), 0);
    for (const auto& [i, data] : change.changed)
        flags[i] = 1;
    for (std::size_t a = 0; a < change.added.size(); ++a)
        flags[numNotesBefore + a] = 1;
    auto removed = change.removed;
    std::sort(removed.rbegin(), removed.rend());
    for (const auto i : removed)
        flags.erase(flags.begin() + static_cast<std::ptrdiff_t>(i));
    return flags;
}

std::vector<NoteData> applied(std::vector<NoteData> notes, const Change& change)
{
    for (const auto& [i, data] : change.changed)
        notes[i] = data;
    notes.insert(notes.end(), change.added.begin(), change.added.end());
    auto removed = change.removed;
    std::sort(removed.rbegin(), removed.rend());
    for (const auto i : removed)
        notes.erase(notes.begin() + static_cast<std::ptrdiff_t>(i));
    return notes;
}

void apply(LiveSet& set, const juce::ValueTree& clip, const Change& change)
{
    auto node = clip.getChildWithName(ids::notes);
    for (const auto& [i, data] : change.changed)
        set.setNote(node.getChild(static_cast<int>(i)), data);
    for (const auto& n : change.added)
        set.addNote(clip, n);
    auto removed = change.removed;
    std::sort(removed.rbegin(), removed.rend());
    for (const auto i : removed)
        set.removeNote(clip, node.getChild(static_cast<int>(i)));
}

// --- Moving and sizing ------------------------------------------------------------------------------

std::pair<double, int> clampMove(const Notes& notes, const Indices& selected, double quarters, int rows)
{
    if (selected.empty())
        return {0.0, 0};
    double earliest = std::numeric_limits<double>::max();
    int lowest = 127, highest = 0;
    for (const auto i : selected)
    {
        earliest = std::min(earliest, notes[i].start);
        lowest = std::min(lowest, notes[i].pitch);
        highest = std::max(highest, notes[i].pitch);
    }
    return {std::max(quarters, -earliest), juce::jlimit(-lowest, 127 - highest, rows)};
}

Change move(const Notes& notes, const Indices& selected, double quarters, int rows)
{
    const auto [dq, dr] = clampMove(notes, selected, quarters, rows);
    return each(notes, selected, [dq, dr](NoteData& n) {
        n.start += dq;
        n.pitch += dr;
    });
}

Change copyMoved(const Notes& notes, const Indices& selected, double quarters, int rows)
{
    const auto [dq, dr] = clampMove(notes, selected, quarters, rows);
    Change c;
    for (const auto i : selected)
    {
        auto n = notes[i];
        n.start += dq;
        n.pitch += dr;
        c.added.push_back(n);
    }
    return c;
}

Change resizeEnd(const Notes& notes, const Indices& selected, double quarters)
{
    return each(notes, selected, [quarters](NoteData& n) { n.length = std::max(minLength, n.length + quarters); });
}

Change resizeStart(const Notes& notes, const Indices& selected, double quarters)
{
    return each(notes, selected, [quarters](NoteData& n) {
        const double end = endOf(n);
        n.start = juce::jlimit(0.0, end - minLength, n.start + quarters);
        n.length = end - n.start;
    });
}

Change duplicate(const Notes& notes, const Indices& selected)
{
    if (selected.empty())
        return {};
    const auto [from, to] = spanOf(notes, selected);
    return copyMoved(notes, selected, to - from, 0);
}

Change remove(const Indices& selected)
{
    Change c;
    c.removed = selected;
    return c;
}

// --- Note tools --------------------------------------------------------------------------------------

Change split(const Notes& notes, const Indices& selected, double gridStep)
{
    Change c;
    if (gridStep <= 0.0)
        return c;
    for (const auto i : selected)
    {
        const auto& n = notes[i];
        const double end = endOf(n);
        // The grid lines strictly inside the note.
        double line = std::floor(n.start / gridStep + eps) * gridStep + gridStep;
        if (line >= end - eps)
            continue;
        auto first = n;
        first.length = line - n.start;
        c.changed.emplace_back(i, first);
        for (; line < end - eps; line += gridStep)
        {
            auto piece = n;
            piece.start = line;
            piece.length = std::min(gridStep, end - line);
            c.added.push_back(piece);
        }
    }
    return c;
}

Change join(const Notes& notes, const Indices& selected)
{
    std::map<int, Indices> byRow;
    for (const auto i : selected)
        byRow[notes[i].pitch].push_back(i);
    Change c;
    for (auto& [row, members] : byRow)
    {
        if (members.size() < 2)
            continue;
        std::sort(members.begin(), members.end(), [&](auto a, auto b) { return notes[a].start < notes[b].start; });
        auto joined = notes[members.front()];
        double end = endOf(joined);
        for (std::size_t k = 1; k < members.size(); ++k)
        {
            end = std::max(end, endOf(notes[members[k]]));
            c.removed.push_back(members[k]);
        }
        joined.length = end - joined.start;
        c.changed.emplace_back(members.front(), joined);
    }
    return c;
}

Change quantize(const Notes& notes, const Indices& selected, double gridStep, double amount, bool ends)
{
    if (gridStep <= 0.0)
        return {};
    amount = juce::jlimit(0.0, 1.0, amount);
    return each(notes, selected, [=](NoteData& n) {
        const double end = endOf(n);
        const double start = n.start + (std::round(n.start / gridStep) * gridStep - n.start) * amount;
        double newEnd = end - n.start + start; // the length kept
        if (ends)
            newEnd = end + (std::round(end / gridStep) * gridStep - end) * amount;
        n.start = std::max(0.0, start);
        n.length = std::max(minLength, newEnd - n.start);
    });
}

Change transposeSteps(const Notes& notes, const Indices& selected, int steps)
{
    return move(notes, selected, 0.0, steps);
}

Change transposeSemitones(const Notes& notes, const Indices& selected, double semitones, const TuningTable& tuning,
                          int noteOffset)
{
    const double ratio = std::pow(2.0, semitones / 12.0);
    return each(notes, selected, [&](NoteData& n) {
        const int entry = n.pitch + noteOffset;
        if (entry < 0 || entry >= TuningTable::numNotes)
            return;
        const double f = tuning.hz[static_cast<std::size_t>(entry)];
        const int target = tuning.nearestNoteTo(f * ratio);
        if (f <= 0.0 || target < 0)
            return;
        const int pitch = target - noteOffset;
        if (pitch >= 0 && pitch <= 127)
            n.pitch = pitch;
    });
}

Change legato(const Notes& notes, const Indices& selected, double loopEnd)
{
    auto order = selected;
    std::sort(order.begin(), order.end(), [&](auto a, auto b) { return notes[a].start < notes[b].start; });
    Change c;
    for (std::size_t k = 0; k < order.size(); ++k)
    {
        auto n = notes[order[k]];
        // The next note that starts later (notes of a chord share the next start).
        double next = loopEnd;
        for (std::size_t m = k + 1; m < order.size(); ++m)
            if (notes[order[m]].start > n.start + eps)
            {
                next = notes[order[m]].start;
                break;
            }
        if (next <= n.start + eps)
            continue;
        n.length = next - n.start;
        if (!(n == notes[order[k]]))
            c.changed.emplace_back(order[k], n);
    }
    return c;
}

Change reverse(const Notes& notes, const Indices& selected)
{
    if (selected.empty())
        return {};
    const auto [from, to] = spanOf(notes, selected);
    return each(notes, selected, [from = from, to = to](NoteData& n) { n.start = from + to - endOf(n); });
}

Change invert(const Notes& notes, const Indices& selected)
{
    int lo = 127, hi = 0;
    for (const auto i : selected)
    {
        lo = std::min(lo, notes[i].pitch);
        hi = std::max(hi, notes[i].pitch);
    }
    return each(notes, selected, [lo, hi](NoteData& n) { n.pitch = lo + hi - n.pitch; });
}

Change stretch(const Notes& notes, const Indices& selected, double factor)
{
    if (selected.empty() || factor <= 0.0)
        return {};
    const double from = spanOf(notes, selected).first;
    return each(notes, selected, [from, factor](NoteData& n) {
        n.start = from + (n.start - from) * factor;
        n.length = std::max(minLength, n.length * factor);
    });
}

// --- Velocity ----------------------------------------------------------------------------------------

Change setVelocity(const Notes& notes, const Indices& selected, int velocity)
{
    velocity = juce::jlimit(1, 127, velocity);
    return each(notes, selected, [velocity](NoteData& n) { n.velocity = velocity; });
}

Change changeVelocity(const Notes& notes, const Indices& selected, int delta)
{
    return each(notes, selected, [delta](NoteData& n) { n.velocity = juce::jlimit(1, 127, n.velocity + delta); });
}

Change rampVelocity(const Notes& notes, const Indices& selected, int from, int to)
{
    if (selected.empty())
        return {};
    double first = std::numeric_limits<double>::max(), last = std::numeric_limits<double>::lowest();
    for (const auto i : selected)
    {
        first = std::min(first, notes[i].start);
        last = std::max(last, notes[i].start);
    }
    return each(notes, selected, [=](NoteData& n) {
        const double t = last > first ? (n.start - first) / (last - first) : 0.0;
        n.velocity = juce::jlimit(1, 127, juce::roundToInt(from + (to - from) * t));
    });
}

// --- The loop ----------------------------------------------------------------------------------------

Change duplicateLoop(const Notes& notes, double loopStart, double loopEnd)
{
    Change c;
    const double length = loopEnd - loopStart;
    if (length <= 0.0)
        return c;
    for (const auto& n : notes)
        if (n.start >= loopStart - eps && n.start < loopEnd - eps)
        {
            auto copy = n;
            copy.start += length;
            c.added.push_back(copy);
        }
    return c;
}

Indices inArea(const Notes& notes, double from, double to, int lowRow, int highRow)
{
    Indices out;
    for (std::size_t i = 0; i < notes.size(); ++i)
    {
        const auto& n = notes[i];
        if (n.pitch >= lowRow && n.pitch <= highRow && n.start < to && endOf(n) > from)
            out.push_back(i);
    }
    return out;
}

Indices onRow(const Notes& notes, int row)
{
    Indices out;
    for (std::size_t i = 0; i < notes.size(); ++i)
        if (notes[i].pitch == row)
            out.push_back(i);
    return out;
}

Indices all(const Notes& notes)
{
    Indices out(notes.size());
    std::iota(out.begin(), out.end(), std::size_t{0});
    return out;
}

} // namespace lattice::ui::notes
