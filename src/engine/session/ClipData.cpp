#include "engine/session/ClipData.h"

#include "core/model/LiveSet.h"

#include <algorithm>

namespace lattice
{

std::size_t ClipData::firstEventAtOrAfter(double position) const noexcept
{
    const auto it = std::lower_bound(events.begin(), events.end(), position,
                                     [](const ClipEvent& e, double p) { return e.position < p; });
    return static_cast<std::size_t>(it - events.begin());
}

bool ClipData::noteCovers(int pitch, double position) const noexcept
{
    // Notes are sorted by start; only those starting at or before position can cover it.
    const auto end = std::upper_bound(notes.begin(), notes.end(), position,
                                      [](double p, const ClipNoteSpan& n) { return p < n.start; });
    for (auto it = notes.begin(); it != end; ++it)
        if (it->pitch == pitch && position < it->end)
            return true;
    return false;
}

std::shared_ptr<const ClipData> ClipData::fromTree(const juce::ValueTree& clip)
{
    auto data = std::make_shared<ClipData>();
    data->id = LiveSet::idOf(clip);

    data->loopStart = std::max(0.0, static_cast<double>(clip[ids::loopStart]));
    data->loopEnd =
        std::max(static_cast<double>(clip[ids::loopEnd]), data->loopStart + LiveSet::minLoopLength);
    data->startMarker =
        std::clamp(static_cast<double>(clip[ids::startMarker]), 0.0, data->loopEnd - LiveSet::minLoopLength);
    data->looping = clip[ids::looping];
    data->launchMode = static_cast<LaunchMode>(juce::jlimit(0, 2, static_cast<int>(clip[ids::launchMode])));
    data->quantization = static_cast<Quantization>(
        juce::jlimit(-1, numQuantizationValues - 1, static_cast<int>(clip[ids::quantization])));
    data->legato = clip[ids::legato];
    data->followA = static_cast<FollowAction>(
        juce::jlimit(0, numFollowActions - 1, static_cast<int>(clip[ids::followA])));
    data->followB = static_cast<FollowAction>(
        juce::jlimit(0, numFollowActions - 1, static_cast<int>(clip[ids::followB])));
    data->followChanceA = juce::jlimit(0.0, 1.0, static_cast<double>(clip[ids::followChanceA]) / 100.0);
    data->followTime = std::max(0.0, static_cast<double>(clip[ids::followTime]));

    // Collect unmuted notes, then cut same-pitch overlaps.
    auto& spans = data->notes;
    const auto notesNode = clip.getChildWithName(ids::notes);
    spans.reserve(static_cast<std::size_t>(notesNode.getNumChildren()));
    struct Raw
    {
        ClipNoteSpan span;
        std::uint8_t velocity;
    };
    std::vector<Raw> raw;
    raw.reserve(spans.capacity());
    for (const auto& n : notesNode)
    {
        if (static_cast<bool>(n[ids::muted]))
            continue;
        const double start = std::max(0.0, static_cast<double>(n[ids::start]));
        const double length = n[ids::length];
        if (!(length > 0.0))
            continue;
        Raw r;
        r.span.start = start;
        r.span.end = start + length;
        r.span.pitch = static_cast<std::uint8_t>(juce::jlimit(0, 127, static_cast<int>(n[ids::pitch])));
        r.velocity = static_cast<std::uint8_t>(juce::jlimit(1, 127, static_cast<int>(n[ids::velocity])));
        raw.push_back(r);
    }

    std::sort(raw.begin(), raw.end(), [](const Raw& a, const Raw& b) {
        return a.span.pitch != b.span.pitch ? a.span.pitch < b.span.pitch : a.span.start < b.span.start;
    });
    for (std::size_t i = 0; i < raw.size(); ++i)
    {
        auto& r = raw[i];
        if (i + 1 < raw.size() && raw[i + 1].span.pitch == r.span.pitch)
            r.span.end = std::min(r.span.end, raw[i + 1].span.start);
    }
    // Drop notes left with no length (two notes starting together: the later one wins).
    raw.erase(
        std::remove_if(raw.begin(), raw.end(), [](const Raw& r) { return !(r.span.end > r.span.start); }),
        raw.end());

    data->events.reserve(raw.size() * 2);
    for (const auto& r : raw)
    {
        spans.push_back(r.span);
        data->events.push_back({r.span.start, r.span.pitch, r.velocity});
        data->events.push_back({r.span.end, r.span.pitch, 0});
    }
    std::sort(spans.begin(), spans.end(),
              [](const ClipNoteSpan& a, const ClipNoteSpan& b) { return a.start < b.start; });
    std::stable_sort(data->events.begin(), data->events.end(), [](const ClipEvent& a, const ClipEvent& b) {
        if (a.position != b.position)
            return a.position < b.position;
        return !a.isNoteOn() && b.isNoteOn(); // note-offs first
    });
    return data;
}

} // namespace lattice
