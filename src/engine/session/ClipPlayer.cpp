#include "engine/session/ClipPlayer.h"

#include <algorithm>
#include <cmath>

namespace lattice
{
namespace
{
constexpr std::uint8_t noteOnStatus = 0x90;  // channel 1
constexpr std::uint8_t noteOffStatus = 0x80; // channel 1
} // namespace

void ClipPlayer::start(const ClipData* clip, double beat, double startPosition,
                       bool shouldBeOpenEnded) noexcept
{
    data = clip;
    id = clip->id;
    loop = loopOf(*clip);
    openEnded = shouldBeOpenEnded;
    overrideActive = false;
    anchorBeat = beat;
    launchedAt = beat;
    anchorPos = std::max(0.0, startPosition);
    iteration = 0;
    updateIndices();
    next = firstPassBegin;
}

void ClipPlayer::stop(std::int64_t sample, MidiEventSink& out) noexcept
{
    releaseAll(sample, out);
    data = nullptr;
    id = 0;
    openEnded = false;
    overrideActive = false;
}

void ClipPlayer::releaseAll(std::int64_t sample, MidiEventSink& out) noexcept
{
    if (numSounding == 0)
        return;
    for (std::size_t p = 0; p < sounding.size(); ++p)
    {
        if (sounding[p])
        {
            out.add(sample, noteOffStatus, static_cast<std::uint8_t>(p), 0);
            sounding[p] = false;
        }
    }
    numSounding = 0;
}

void ClipPlayer::updateIndices() noexcept
{
    firstPassBegin = data->firstEventAtOrAfter(anchorPos);
    loopBegin = data->firstEventAtOrAfter(loop.start);
    endIndex = openEnded ? data->events.size() : data->firstEventAtOrAfter(loop.end);
}

double ClipPlayer::iterationStart(int n) const noexcept
{
    if (n == 0)
        return 0.0;
    if (openEnded)
        return infinity;
    const double firstPass = std::max(0.0, loop.end - anchorPos);
    return firstPass + (n - 1) * (loop.end - loop.start);
}

double ClipPlayer::eventTime(std::size_t index) const noexcept
{
    const double position = data->events[index].position;
    if (iteration == 0)
        return position - anchorPos;
    return iterationStart(iteration) + (position - loop.start);
}

ClipPlayer::Status ClipPlayer::render(const Transport& transport, std::int64_t from, std::int64_t to,
                                      MidiEventSink& out) noexcept
{
    if (data == nullptr)
        return Status::Playing;

    for (;;)
    {
        const double nextEvent = next < endIndex ? eventTime(next) : infinity;
        const double wrap = iterationStart(iteration + 1);
        if (nextEvent == infinity && wrap == infinity)
            break;

        if (nextEvent < wrap)
        {
            const auto sample = transport.sampleAtBeat(anchorBeat + nextEvent);
            if (sample >= to)
                break;
            const auto at = std::max(sample, from);
            const auto& e = data->events[next++];
            if (e.isNoteOn())
            {
                if (sounding[e.pitch])
                    out.add(at, noteOffStatus, e.pitch, 0);
                else
                    ++numSounding;
                out.add(at, noteOnStatus, e.pitch, e.velocity);
                sounding[e.pitch] = true;
            }
            else if (sounding[e.pitch])
            {
                out.add(at, noteOffStatus, e.pitch, 0);
                sounding[e.pitch] = false;
                --numSounding;
            }
            continue;
        }

        const auto sample = transport.sampleAtBeat(anchorBeat + wrap);
        if (sample >= to)
            break;
        const auto at = std::max(sample, from);
        releaseAll(at, out);
        if (!loop.looping)
        {
            stop(at, out);
            return Status::Ended;
        }
        ++iteration;
        next = loopBegin;
    }
    return Status::Playing;
}

double ClipPlayer::positionAt(double beat) const noexcept
{
    if (data == nullptr)
        return 0.0;
    const double t = std::max(0.0, beat - anchorBeat);
    const double firstPass = loop.end - anchorPos;
    if (openEnded || t < firstPass)
        return anchorPos + t;
    if (!loop.looping)
        return loop.end;
    const double length = loop.end - loop.start;
    return loop.start + std::fmod(t - firstPass, length);
}

void ClipPlayer::seekTo(const Transport& transport, std::int64_t sample) noexcept
{
    const double t = std::max(0.0, transport.beatAt(sample) - anchorBeat);
    iteration = 0;
    if (!openEnded)
    {
        const double firstPass = loop.end - anchorPos;
        const double length = loop.end - loop.start;
        if (t >= firstPass && loop.looping && length > 0.0)
            iteration = 1 + static_cast<int>(std::floor((t - firstPass) / length));
        // Guard against rounding putting t just before the computed iteration's start.
        while (iteration > 0 && iterationStart(iteration) > t)
            --iteration;
    }

    const std::size_t begin = iteration == 0 ? firstPassBegin : loopBegin;
    const double position = iteration == 0 ? anchorPos + t : loop.start + (t - iterationStart(iteration));
    next = std::max(begin, data->firstEventAtOrAfter(position - 1.0e-6));
    while (next < endIndex && transport.sampleAtBeat(anchorBeat + eventTime(next)) < sample)
        ++next;
}

ClipPlayer::Status ClipPlayer::replaceClipData(const ClipData* newClip, const Transport& transport,
                                               std::int64_t sample, MidiEventSink& out) noexcept
{
    if (data == nullptr)
        return Status::Playing;

    const double beat = transport.beatAt(sample);
    const double position = positionAt(beat);

    Loop newLoop = loopOf(*newClip);
    if (overrideActive)
    {
        if (newLoop == clipLoopAtOverride)
            newLoop = loop; // the model hasn't caught up yet: keep the recording's loop
        else
            overrideActive = false;
    }

    data = newClip;

    // Release sounding notes the edit removed.
    for (std::size_t p = 0; p < sounding.size(); ++p)
    {
        if (sounding[p] && !newClip->noteCovers(static_cast<int>(p), position))
        {
            out.add(sample, noteOffStatus, static_cast<std::uint8_t>(p), 0);
            sounding[p] = false;
            --numSounding;
        }
    }

    if (!openEnded && !(newLoop == loop))
    {
        // Re-anchor at the current position under the new loop.
        double startPos = position;
        if (startPos >= newLoop.end)
        {
            if (!newLoop.looping)
            {
                stop(sample, out);
                return Status::Ended;
            }
            const double length = newLoop.end - newLoop.start;
            startPos = newLoop.start + std::fmod(startPos - newLoop.start, length);
            releaseAll(sample, out); // the position jumped
        }
        loop = newLoop;
        anchorBeat = beat;
        anchorPos = startPos;
    }

    updateIndices();
    seekTo(transport, sample);
    return Status::Playing;
}

void ClipPlayer::finishOpenEnded(double loopLength) noexcept
{
    if (data == nullptr)
        return;
    openEnded = false;
    overrideActive = true;
    clipLoopAtOverride = loopOf(*data);
    loop = {0.0, std::max(loopLength, 1.0 / 16.0), true};
    // The recording started at position 0, so the anchor stays: the first wrap is at loopLength.
    updateIndices();
}

} // namespace lattice
