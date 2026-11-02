#include "ui/widgets/Meter.h"

#include "ui/theme/LatticeLookAndFeel.h"

#include <cmath>

namespace lattice::ui
{

Meter::Meter(int channelCount) : numChannels(juce::jlimit(1, maxChannels, channelCount))
{
    setTitle("Level");
}

float Meter::toDb(float linear) noexcept
{
    return linear > 0.0f ? juce::jmax(floorDb, 20.0f * std::log10(linear)) : floorDb;
}

float Meter::proportionForDb(float db) noexcept
{
    return juce::jlimit(0.0f, 1.0f, (db - floorDb) / (topDb - floorDb));
}

void Meter::setLevels(int channel, float peak, float rms)
{
    if (channel < 0 || channel >= numChannels)
        return;
    auto& c = state[static_cast<std::size_t>(channel)];
    c.peak = juce::jmax(0.0f, peak);
    c.rms = juce::jmax(0.0f, rms);
    const float db = toDb(c.peak);
    if (db >= c.heldDb)
    {
        c.heldDb = db;
        c.heldFor = 0.0;
    }
    if (c.peak > 1.0f && !clip)
        clip = true;
    repaint();
}

void Meter::advance(double seconds)
{
    for (int i = 0; i < numChannels; ++i)
    {
        auto& c = state[static_cast<std::size_t>(i)];
        c.heldFor += seconds;
        if (c.heldFor > holdSeconds)
        {
            // Falls once the hold is over, never below the current peak.
            const double falling = juce::jmin(seconds, c.heldFor - holdSeconds);
            c.heldDb = juce::jmax(toDb(c.peak), c.heldDb - fallDbPerSecond * static_cast<float>(falling));
        }
    }
}

float Meter::peakDb(int channel) const
{
    return toDb(state[static_cast<std::size_t>(juce::jlimit(0, numChannels - 1, channel))].peak);
}

float Meter::rmsDb(int channel) const
{
    return toDb(state[static_cast<std::size_t>(juce::jlimit(0, numChannels - 1, channel))].rms);
}

float Meter::heldPeakDb(int channel) const
{
    return state[static_cast<std::size_t>(juce::jlimit(0, numChannels - 1, channel))].heldDb;
}

void Meter::resetClip()
{
    clip = false;
    repaint();
}

void Meter::paint(juce::Graphics& g)
{
    const auto& t = themeOf(*this);
    auto area = getLocalBounds().toFloat();
    const float clipHeight = juce::jmin(4.0f, area.getHeight() * 0.05f);
    auto clipLight = area.removeFromTop(clipHeight);
    area.removeFromTop(1.0f);
    g.setColour(clip ? t.meterClip : t.surfaceSunken);
    g.fillRect(clipLight);

    const float gap = 1.0f;
    const float barWidth = (area.getWidth() - gap * static_cast<float>(numChannels - 1)) / static_cast<float>(numChannels);
    const float bottom = area.getBottom();
    const auto yFor = [&](float db) { return bottom - proportionForDb(db) * area.getHeight(); };

    // One bar's colours, zone by zone: below midDb, to highDb, to 0 dB, above.
    const auto fillBar = [&](float x, float levelDb, float alpha) {
        if (levelDb <= floorDb)
            return;
        const float zones[] = {floorDb, midDb, highDb, 0.0f, topDb};
        const juce::Colour colours[] = {t.meterLow, t.meterMid, t.meterHigh, t.meterClip};
        for (int z = 0; z < 4; ++z)
        {
            const float lo = zones[z], hi = juce::jmin(zones[z + 1], levelDb);
            if (hi <= lo)
                break;
            g.setColour(colours[z].withMultipliedAlpha(alpha));
            g.fillRect(juce::Rectangle<float>(x, yFor(hi), barWidth, yFor(lo) - yFor(hi)));
        }
    };

    for (int i = 0; i < numChannels; ++i)
    {
        const float x = area.getX() + static_cast<float>(i) * (barWidth + gap);
        g.setColour(t.surfaceSunken);
        g.fillRect(juce::Rectangle<float>(x, area.getY(), barWidth, area.getHeight()));
        fillBar(x, peakDb(i), 0.45f);
        fillBar(x, rmsDb(i), 1.0f);
        const float held = heldPeakDb(i);
        if (held > floorDb)
        {
            g.setColour(held > 0.0f ? t.meterClip : t.text);
            g.fillRect(juce::Rectangle<float>(x, yFor(held) - 1.0f, barWidth, 1.0f));
        }
    }

    // A tick at 0 dB.
    g.setColour(t.textDim.withMultipliedAlpha(0.6f));
    g.fillRect(juce::Rectangle<float>(area.getX(), yFor(0.0f), area.getWidth(), 1.0f));
}

} // namespace lattice::ui
