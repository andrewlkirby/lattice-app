#pragma once

#include "devices/framework/DeviceSpecs.h"
#include "ui/theme/Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace lattice::ui
{

/** A private copy of a device that follows the document's values, for panels that draw what the
    device does (an EQ's curve, a compressor's transfer curve, a reverb's decay times) without
    reaching into the engine's instance. It's never processed. Message thread. */
class DeviceMirror final : private juce::ValueTree::Listener
{
public:
    DeviceMirror(const DeviceSpecs& specs, juce::ValueTree device);
    ~DeviceMirror() override;

    StudioDevice& device() noexcept { return *instance; }
    template <typename T>
    T* as() noexcept
    {
        return dynamic_cast<T*>(instance.get());
    }
    /** The parameter's plain value (its default if the device has no such parameter). */
    float value(const juce::String& parameterId) const;
    /** Prepares the copy (for curves that depend on the sample rate). */
    void prepare(double sampleRate);

    /** Called after any of the device's values changed (in the document, by undo, or anywhere). */
    std::function<void()> onChange;

private:
    void syncAll();
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeRedirected(juce::ValueTree&) override;

    juce::ValueTree node;
    std::unique_ptr<StudioDevice> instance;
};

/** A panel's picture of what a device does: a curve, an envelope, a waveform. What it shows is
    drawn into a cached image, redrawn only when it changes (docs/05-m4-ui.md, "Rendering and
    performance"); live parts (meters, positions) are drawn over it on every paint. */
class DeviceDisplay : public juce::Component
{
public:
    DeviceDisplay();

    /** What it shows changed: the cached picture is drawn again on the next paint. */
    void invalidate();
    /** The area inside the well, where the picture goes. */
    juce::Rectangle<float> contentArea() const;

    void paint(juce::Graphics&) final;
    void resized() override { invalidate(); }
    void lookAndFeelChanged() override { invalidate(); }

protected:
    /** Draws the cached picture. */
    virtual void drawContent(juce::Graphics& g, juce::Rectangle<float> area) = 0;
    /** Draws what changes often, over the picture. */
    virtual void drawOverlay(juce::Graphics&, juce::Rectangle<float>) {}

private:
    juce::Image cache;
    bool dirty = true;
};

/** The log-frequency axis the EQ, filter and spectrum displays share: 20 Hz to 20 kHz. */
struct FrequencyAxis
{
    static constexpr double lowest = 20.0;
    static constexpr double highest = 20000.0;
    static float xFor(double hz, juce::Rectangle<float> area);
    static double hzAt(float x, juce::Rectangle<float> area);
};

/** Grid lines for a frequency display: decades and their 2s and 5s, labelled at 100, 1k and 10k;
    and level lines every `dbStep` between `dbLow` and `dbHigh` (no level lines when dbStep is 0). */
void drawFrequencyGrid(juce::Graphics& g, juce::Rectangle<float> area, const Theme& t, float dbLow, float dbHigh, float dbStep);
/** y for a level in dB on a display from dbLow (bottom) to dbHigh (top). */
float yForDb(float db, juce::Rectangle<float> area, float dbLow, float dbHigh);

/** A path through `points` samples of fn(x), x from 0 to 1 across the area, y from fn (0 bottom,
    1 top; clipped). */
juce::Path curvePath(juce::Rectangle<float> area, int points, const std::function<float(float)>& fn);

} // namespace lattice::ui
