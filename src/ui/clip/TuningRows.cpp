#include "ui/clip/TuningRows.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

namespace lattice::ui
{

TuningRow tuningRow(const TuningRowContext& c, int pitch)
{
    TuningRow row;
    row.pitch = pitch;
    const auto* t = c.tuning;
    const int entry = pitch + c.noteOffset;
    row.entry = t != nullptr && entry >= 0 && entry < TuningTable::numNotes ? entry : -1;
    row.namedNotes = t == nullptr || t->usesNoteNames;
    if (row.namedNotes)
    {
        static constexpr bool black[12] = {false, true, false, true, false, false, true, false, true, false, true, false};
        const int key = row.entry >= 0 ? row.entry : pitch; // the note it plays names the key
        row.blackKey = black[key % 12];
        row.octaveStart = key % 12 == 0;
    }
    if (row.entry < 0)
    {
        row.unplayable = true;
        row.label = t == nullptr ? juce::MidiMessage::getMidiNoteName(pitch, true, true, 4) : juce::String("-");
        return row;
    }

    const auto e = static_cast<std::size_t>(row.entry);
    row.hz = t->hz[e];
    if (row.namedNotes)
        row.label = t->labels[e].isNotEmpty() ? t->labels[e] : juce::MidiMessage::getMidiNoteName(row.entry, true, true, 4);
    else
    {
        row.label = juce::String(row.entry) + juce::String(juce::CharPointer_UTF8(" \xc2\xb7 ")) +
                    juce::String(row.hz, row.hz < 1000.0 ? 1 : 0);
        row.octaveStart = t->keyboardPageSize > 0 && row.entry % t->keyboardPageSize == 0;
    }
    row.anchor = t->isAnchor(row.entry);
    row.highlighted = std::binary_search(c.highlight.begin(), c.highlight.end(), row.entry);

    const bool tooHigh = c.sampleRate > 0.0 && row.hz >= TrackTuning::maxPlayableFraction * c.sampleRate;
    if (row.hz <= 0.0)
        row.unplayable = true;
    else if (tooHigh)
    {
        if (c.policy == HighNotePolicy::FoldDown && t->periodRatio > 1.0)
            row.foldsDown = true;
        else
            row.unplayable = true;
    }
    return row;
}

juce::String tuningRowDetails(const TuningRowContext& c, int pitch)
{
    const auto row = tuningRow(c, pitch);
    if (row.entry < 0 || c.tuning == nullptr)
        return "Outside the tuning's table";
    const auto dot = juce::String(juce::CharPointer_UTF8(" \xc2\xb7 "));
    juce::String text = "#" + juce::String(row.entry) + dot;
    if (row.hz <= 0.0)
        return text + "unmapped";
    text << juce::String(row.hz, 2) << " Hz";
    const double base = c.tuning->baseHz();
    if (base > 0.0)
    {
        const double ratio = row.hz / base;
        text << dot << juce::String(1200.0 * std::log2(ratio), 1) << " ct" << dot
             << juce::String(juce::CharPointer_UTF8("\xc3\x97")) << juce::String(ratio, 3) << " of "
             << juce::String(base, base < 100.0 ? 2 : 1) << " Hz";
    }
    if (row.anchor)
        text << dot << "anchor";
    if (row.unplayable)
        text << dot << "too high to play";
    else if (row.foldsDown)
        text << dot << "folded down";
    return text;
}

} // namespace lattice::ui
