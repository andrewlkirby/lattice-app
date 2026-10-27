#include "ui/clip/NoteGrid.h"

#include <cmath>

namespace lattice::ui
{

// --- BarsBeats ------------------------------------------------------------------------------------

juce::String BarsBeats::position(double quarters) const
{
    return format(quarters, 1);
}

juce::String BarsBeats::length(double quarters) const
{
    return format(quarters, 0);
}

juce::String BarsBeats::format(double quarters, int origin) const
{
    const bool negative = quarters < -1.0e-9;
    double rest = std::abs(quarters);
    const double sixteenth = 0.25;
    const double sixteenthsPerBeat = quartersPerBeat() / sixteenth;
    // Round to a thousandth of a sixteenth first, so 0.9999999 shows as the next sixteenth.
    rest = std::round(rest / sixteenth * 1000.0) / 1000.0 * sixteenth;
    const auto bars = static_cast<int>(std::floor(rest / quartersPerBar() + 1.0e-9));
    rest -= bars * quartersPerBar();
    const auto beats = static_cast<int>(std::floor(rest / quartersPerBeat() + 1.0e-9));
    rest -= beats * quartersPerBeat();
    double sixteenths = juce::jmax(0.0, rest / sixteenth);
    if (sixteenths >= sixteenthsPerBeat - 1.0e-9)
        sixteenths = 0.0;
    juce::String six = std::abs(sixteenths - std::round(sixteenths)) < 1.0e-6 ? juce::String(juce::roundToInt(sixteenths) + origin)
                                                                              : juce::String(sixteenths + origin, 2);
    return (negative ? "-" : "") + juce::String(bars + origin) + "." + juce::String(beats + origin) + "." + six;
}

std::optional<double> BarsBeats::parse(const juce::String& text, int origin) const
{
    auto t = text.trim();
    const bool negative = t.startsWithChar('-');
    t = t.trimCharactersAtStart("-+");
    juce::StringArray fields;
    fields.addTokens(t, ".", {});
    if (fields.isEmpty() || fields.size() > 3)
        return std::nullopt;
    double values[3] = {static_cast<double>(origin), static_cast<double>(origin), static_cast<double>(origin)};
    for (int i = 0; i < fields.size(); ++i)
    {
        const auto f = fields[i].trim();
        if (f.isEmpty() || !f.containsOnly("0123456789"))
            return std::nullopt;
        values[i] = f.getDoubleValue();
    }
    const double quarters = (values[0] - origin) * quartersPerBar() + (values[1] - origin) * quartersPerBeat() +
                            (values[2] - origin) * 0.25;
    return negative ? -quarters : quarters;
}

// --- NoteGrid -------------------------------------------------------------------------------------

double NoteGrid::valueStep(int index, double quartersPerBar) noexcept
{
    index = juce::jlimit(0, numValues - 1, index);
    if (index <= 5)
        return 0.0625 * std::pow(2.0, index); // 1/64 .. 1/2 notes
    return quartersPerBar * std::pow(2.0, index - 6);
}

juce::String NoteGrid::valueName(int index)
{
    static const char* names[numValues] = {"1/64", "1/32", "1/16", "1/8", "1/4", "1/2", "1 Bar", "2 Bars", "4 Bars", "8 Bars"};
    return names[juce::jlimit(0, numValues - 1, index)];
}

int NoteGrid::valueIndex(double pixelsPerQuarter, double quartersPerBar, double minPixels) const noexcept
{
    if (!adaptive)
        return juce::jlimit(0, numValues - 1, fixedIndex);
    const double wanted = minPixels * std::pow(2.0, -juce::jlimit(minDensity, maxDensity, density));
    const double tripletScale = triplet ? 2.0 / 3.0 : 1.0;
    for (int i = 0; i < numValues; ++i)
        if (valueStep(i, quartersPerBar) * tripletScale * pixelsPerQuarter >= wanted)
            return i;
    return numValues - 1;
}

double NoteGrid::step(double pixelsPerQuarter, double quartersPerBar, double minPixels) const noexcept
{
    return valueStep(valueIndex(pixelsPerQuarter, quartersPerBar, minPixels), quartersPerBar) * (triplet ? 2.0 / 3.0 : 1.0);
}

juce::String NoteGrid::describe(double pixelsPerQuarter, double quartersPerBar, double minPixels) const
{
    auto text = valueName(valueIndex(pixelsPerQuarter, quartersPerBar, minPixels));
    if (triplet)
        text << "T";
    if (!snap)
        text << " (Off)";
    return text;
}

void NoteGrid::narrow() noexcept
{
    if (adaptive)
        density = juce::jmin(maxDensity, density + 1);
    else
        fixedIndex = juce::jmax(0, fixedIndex - 1);
}

void NoteGrid::widen() noexcept
{
    if (adaptive)
        density = juce::jmax(minDensity, density - 1);
    else
        fixedIndex = juce::jmin(numValues - 1, fixedIndex + 1);
}

double NoteGrid::nearest(double quarters, double step) noexcept
{
    return step > 0.0 ? std::round(quarters / step) * step : quarters;
}

double NoteGrid::floor(double quarters, double step) noexcept
{
    // A hair of tolerance, so a time on a line (as arithmetic left it) stays on it.
    return step > 0.0 ? std::floor(quarters / step + 1.0e-9) * step : quarters;
}

} // namespace lattice::ui
