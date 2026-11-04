#include "control/modes/StepSequencer.h"

#include <algorithm>
#include <cmath>

namespace lattice::control
{

namespace steps = ui::steps;

namespace
{
constexpr double epsilon = 1.0e-9;
constexpr float tiedDim = 0.2f;
constexpr float mutedDim = 0.35f;
constexpr float tintDim = 0.12f;
constexpr float playheadDim = 0.35f;
} // namespace

// --- Zones -------------------------------------------------------------------------------------------

SequencerZones SequencerZones::drum(int rows, int columns, bool padsOnly) noexcept
{
    SequencerZones z;
    if (rows <= 0 || columns <= 0)
        return z;
    if (padsOnly)
    {
        z.pads = {0, 0, rows, columns};
        return z;
    }
    if (fitsAll(rows, columns))
    {
        const int stepRows = rows / 2, padColumns = columns / 2;
        z.steps = {0, 0, stepRows, columns};
        z.pads = {stepRows, 0, rows - stepRows, padColumns};
        z.loop = {stepRows, padColumns, rows - stepRows, columns - padColumns};
        return z;
    }
    if (rows >= 2)
    {
        z.steps = {0, 0, rows - 1, columns};
        z.loop = {rows - 1, 0, 1, columns};
    }
    else
    {
        z.steps = {0, 0, rows, columns};
    }
    return z;
}

SequencerZones SequencerZones::melodic(int rows, int columns) noexcept
{
    SequencerZones z;
    if (rows <= 0 || columns <= 0)
        return z;
    const int loopRows = rows >= 2 ? std::max(1, rows / 8) : 0;
    z.steps = {0, 0, rows - loopRows, columns};
    z.loop = {rows - loopRows, 0, loopRows, columns};
    return z;
}

// --- Pages -------------------------------------------------------------------------------------------

int StepSequencer::stepsPerPage(const Target& target) const noexcept
{
    return std::max(1, target.oneRow ? area.steps.count() : area.steps.columns);
}

double StepSequencer::pageLength(const Target& target) const noexcept
{
    return stepsPerPage(target) * target.stepLength;
}

int StepSequencer::pages(const Target& target) const
{
    if (!target.clip.isValid())
        return 1;
    const double end = target.clip[ids::loopEnd];
    return std::max(1, static_cast<int>(std::ceil(end / pageLength(target) - epsilon)));
}

int StepSequencer::playingStep(const Target& target) const
{
    if (!target.clip.isValid() || target.track < 0 || target.stepLength <= 0.0)
        return -1;
    const auto& play = host.context().feed.trackPlay(target.track);
    if (play.playingClipId == 0 || play.playingClipId != LiveSet::idOf(target.clip))
        return -1;
    return static_cast<int>(std::floor(play.position / target.stepLength + epsilon));
}

int StepSequencer::page(const Target& target) const
{
    if (!target.clip.isValid())
        return 0;
    const int perPage = stepsPerPage(target);
    if (follow)
        if (const int playing = playingStep(target); playing >= 0)
            return playing / perPage;
    if (LiveSet::idOf(target.clip) != viewedClip)
        return static_cast<int>(std::floor(static_cast<double>(target.clip[ids::loopStart]) / pageLength(target) + epsilon));
    return std::max(0, viewed);
}

std::optional<steps::Step> StepSequencer::stepAt(const Target& target, int row, int column) const
{
    const int at = area.steps.indexOf(row, column);
    if (at < 0 || target.pitches.empty())
        return std::nullopt;
    const int first = page(target) * stepsPerPage(target);
    if (target.oneRow)
        return steps::Step{target.pitches.front(), first + at};
    const int fromBottom = area.steps.row + area.steps.rows - 1 - row;
    if (fromBottom >= static_cast<int>(target.pitches.size()))
        return std::nullopt;
    const int pitch = target.pitches[static_cast<std::size_t>(fromBottom)];
    if (pitch < 0 || pitch > 127)
        return std::nullopt;
    return steps::Step{pitch, first + (column - area.steps.column)};
}

// --- Input -------------------------------------------------------------------------------------------

juce::ValueTree StepSequencer::ensureClip(const Target& target)
{
    if (target.clip.isValid())
        return target.clip;
    auto& set = host.context().session.set();
    if (target.track < 0 || target.scene < 0 || target.track >= set.numTracks() || target.scene >= set.numScenes())
        return {};
    return set.createClip(target.track, target.scene, pageLength(target));
}

bool StepSequencer::press(const Target& target, int row, int column)
{
    if (const int p = area.loop.indexOf(row, column); p >= 0)
    {
        pressLoop(target, p);
        return true;
    }
    const auto step = stepAt(target, row, column);
    if (!step)
        return area.steps.contains(row, column);

    auto& session = host.context().session;
    auto& set = session.set();
    const auto all = target.clip.isValid() ? LiveSet::notesOf(target.clip) : std::vector<NoteData>{};
    const auto on = steps::notesIn(all, *step, target.stepLength);
    Held h;
    h.pad = row * host.capabilities().columns + column;
    h.step = *step;
    h.hadNotes = !on.empty();
    for (const auto i : on)
        h.before.push_back(all[i]);
    h.at = clock();
    if (!h.hadNotes)
    {
        session.beginEdit("Add step", target.clip.isValid() ? target.clip : set.track(target.track));
        if (const auto clip = ensureClip(target); clip.isValid())
        {
            const auto change = steps::toggle(LiveSet::notesOf(clip), *step, target.stepLength);
            ui::notes::apply(set, clip, change);
            h.before = change.added;
        }
    }
    held.push_back(std::move(h));
    host.requestRefresh();
    return true;
}

bool StepSequencer::release(const Target& target, int row, int column)
{
    const int pad = row * host.capabilities().columns + column;
    const auto it = std::find_if(held.begin(), held.end(), [pad](const Held& h) { return h.pad == pad; });
    if (it == held.end())
    {
        if (const int p = area.loop.indexOf(row, column); p >= 0)
        {
            heldPages.erase(std::remove(heldPages.begin(), heldPages.end(), p), heldPages.end());
            return true;
        }
        return area.steps.contains(row, column);
    }
    const auto h = *it;
    held.erase(it);
    if (h.hadNotes && target.clip.isValid())
    {
        // Let go at once and left as it was: the step's notes go.
        const auto all = LiveSet::notesOf(target.clip);
        const auto on = steps::notesIn(all, h.step, target.stepLength);
        std::vector<NoteData> now;
        for (const auto i : on)
            now.push_back(all[i]);
        if (now == h.before && clock() - h.at < holdMs && !on.empty())
        {
            auto& session = host.context().session;
            session.beginEdit("Remove step", target.clip);
            ui::notes::Change change;
            change.removed = on;
            ui::notes::apply(session.set(), target.clip, change);
        }
    }
    host.requestRefresh();
    return true;
}

void StepSequencer::pressLoop(const Target& target, int p)
{
    if (!target.clip.isValid())
        return;
    const double length = pageLength(target);
    if (host.shiftHeld())
    {
        viewed = p;
        viewedClip = LiveSet::idOf(target.clip);
        follow = false;
        host.requestRefresh();
        return;
    }
    const int first = heldPages.empty() ? p : std::min(heldPages.front(), p);
    const int last = heldPages.empty() ? p : std::max(heldPages.front(), p);
    auto& session = host.context().session;
    session.beginEdit("Set loop", target.clip);
    session.set().setClipLoop(target.clip, first * length, (last + 1) * length);
    viewed = first;
    viewedClip = LiveSet::idOf(target.clip);
    follow = true;
    heldPages.push_back(p);
    host.requestRefresh();
}

void StepSequencer::reset()
{
    held.clear();
    heldPages.clear();
}

std::vector<steps::Step> StepSequencer::heldSteps() const
{
    std::vector<steps::Step> out;
    for (const auto& h : held)
        out.push_back(h.step);
    return out;
}

// --- Output ------------------------------------------------------------------------------------------

void StepSequencer::render(const Target& target, LedFrame& frame) const
{
    const auto& colours = host.colours();
    const int columns = host.capabilities().columns;
    const auto all = target.clip.isValid() ? LiveSet::notesOf(target.clip) : std::vector<NoteData>{};
    const double loopStart = target.clip.isValid() ? static_cast<double>(target.clip[ids::loopStart]) : 0.0;
    const double loopEnd = target.clip.isValid() ? static_cast<double>(target.clip[ids::loopEnd]) : pageLength(target);
    const int playing = playingStep(target);
    const int shown = page(target);
    const int perPage = stepsPerPage(target);
    const auto offStep = LedState::solid(dimmed(colours.white, colours.padDim * 0.5f));

    // The steps, a row's states at a time.
    if (!target.pitches.empty() && !area.steps.empty())
    {
        const int rowsOfSteps = target.oneRow ? 1 : std::min(area.steps.rows, static_cast<int>(target.pitches.size()));
        for (int k = 0; k < rowsOfSteps; ++k)
        {
            const int pitch = target.pitches[static_cast<std::size_t>(k)];
            if (pitch < 0 || pitch > 127)
                continue;
            const auto states = steps::row(all, pitch, target.stepLength, shown * perPage, perPage);
            const bool tinted = !target.oneRow && k < static_cast<int>(target.tinted.size()) && target.tinted[static_cast<std::size_t>(k)];
            const bool playable = target.oneRow || k >= static_cast<int>(target.playable.size()) || target.playable[static_cast<std::size_t>(k)];
            for (int i = 0; i < perPage; ++i)
            {
                // Where step i of row k is on the grid.
                const int r = target.oneRow ? area.steps.row + i / area.steps.columns : area.steps.row + area.steps.rows - 1 - k;
                const int c = area.steps.column + (target.oneRow ? i % area.steps.columns : i);
                const int index = shown * perPage + i;
                const double at = index * target.stepLength;
                const bool inLoop = at >= loopStart - epsilon && at < loopEnd - epsilon;
                const bool isHeld = std::any_of(held.begin(), held.end(), [&](const Held& h) { return h.pad == r * columns + c; });
                LedState led = LedState::off();
                switch (states[static_cast<std::size_t>(i)])
                {
                case steps::State::on: led = LedState::solid(index == playing ? colours.play : target.colour); break;
                case steps::State::muted: led = LedState::solid(dimmed(target.colour, mutedDim)); break;
                case steps::State::tied: led = LedState::solid(dimmed(target.colour, tiedDim)); break;
                case steps::State::off:
                    if (index == playing)
                        led = LedState::solid(dimmed(colours.play, playheadDim));
                    else if (inLoop && playable)
                        led = tinted ? LedState::solid(dimmed(target.colour, tintDim)) : offStep;
                    break;
                }
                if (isHeld)
                    led = LedState::solid(colours.white);
                frame.pad(r, c, columns) = led;
            }
        }
    }

    // The loop selector: a pad per page.
    const double length = pageLength(target);
    const int playingPage = playing >= 0 ? playing / perPage : -1;
    for (int p = 0; p < area.loop.count(); ++p)
    {
        const int r = area.loop.row + p / area.loop.columns, c = area.loop.column + p % area.loop.columns;
        const double from = p * length;
        const bool inLoop = from < loopEnd - epsilon && from + length > loopStart + epsilon;
        LedState led = LedState::off();
        if (!target.clip.isValid())
            led = p == 0 ? offStep : LedState::off();
        else if (p == shown)
            led = LedState::solid(colours.white);
        else if (p == playingPage)
            led = LedState::solid(colours.play);
        else if (inLoop)
            led = LedState::solid(target.colour);
        else
            led = offStep;
        frame.pad(r, c, columns) = led;
    }
}

void StepSequencer::encoderTargets(const Target& target, std::vector<juce::String>& targets) const
{
    if (held.empty() || !target.clip.isValid())
        return;
    const auto on = heldSteps();
    for (const auto field : {steps::Field::velocity, steps::Field::length, steps::Field::nudge})
        targets.push_back(steps::target(target.clip, on, target.stepLength, field));
}

void StepSequencer::display(const Target& target, DisplayFrame& frame) const
{
    juce::ignoreUnused(target);
    frame.title = held.size() == 1 ? juce::String("Step ") + juce::String(held.front().step.index + 1)
                                   : juce::String(static_cast<int>(held.size())) + " steps";
    auto& bank = host.encoders();
    const int cells = std::max(0, host.capabilities().encoders);
    for (int i = 0; i < cells; ++i)
    {
        frame.cells.push_back(bank.binding(i) != nullptr ? cellFor(*bank.binding(i)) : DisplayCell{});
    }
    frame.detail = bank.touchedEncoder();
}

} // namespace lattice::control
