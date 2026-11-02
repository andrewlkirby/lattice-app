#pragma once

// What several custom panels share (docs/05-m4-ui.md, 4d): bindings for displays you can drag,
// watching a device's live readout, the sidechain source picker, an envelope you edit by dragging,
// and filter response curves. Private to src/ui/device.

#include "tuning/TuningRegistry.h"
#include "ui/device/DeviceBody.h"
#include "ui/device/DeviceDisplays.h"
#include "ui/widgets/ValueBinding.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <memory>

namespace lattice::ui::panels
{

/** A binding to one of a device's parameters by ID (for a display dragged like a control). */
std::unique_ptr<ValueBinding> bind(DeviceContext& context, const juce::ValueTree& device, const juce::String& parameterId);

/** Reads a device's live readout (gain reduction, spectrum) from the feed while it exists, and
    calls onChange when the readout changes. */
class LiveReadout final : private EngineStateFeed::Listener
{
public:
    LiveReadout(EngineStateFeed& feed, std::int64_t deviceId);
    ~LiveReadout() override;

    const EngineStateFeed::DeviceReadout* readout() const { return feed.device(id); }
    std::function<void()> onChange;

private:
    void engineStateChanged(std::uint32_t) override;

    EngineStateFeed& feed;
    std::int64_t id;
};

/** "Sidechain from": the track a device listens to, chosen from a menu that lists every track and
    says why the ones it can't take aren't offered (its own track, a group, one that would feed back
    into it). With Pre FX, the source is taken before its audio effects. */
class SidechainPicker final : public juce::Component, private juce::ValueTree::Listener
{
public:
    SidechainPicker(DeviceContext& context, juce::ValueTree device);
    ~SidechainPicker() override;

    /** The menu (public for tests): every track; the ones not offered disabled, with the reason. */
    juce::PopupMenu sourceMenu();
    /** Why a track can't be the source ("" if it can). */
    juce::String whyNot(int trackIndex) const;
    std::int64_t source() const;
    int preferredWidth() const;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;

private:
    juce::Rectangle<int> sourceBox() const;
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override { repaint(); }

    DeviceContext& context;
    juce::ValueTree node;
    juce::TextButton preFx{"Pre FX"};
};

/** An ADSR envelope drawn as its shape, edited by dragging its corners: the peak sets the attack,
    the next corner the decay and the sustain level, the last the release. (Sustain may be absent:
    an attack-release envelope.) Times are drawn on a log scale, so short and long ones both show. */
class EnvelopeEditor final : public DeviceDisplay
{
public:
    /** IDs: attack, decay, sustain, release (decay and sustain may be empty). */
    EnvelopeEditor(DeviceContext& context, const juce::ValueTree& device, const juce::StringArray& parameterIds);

    enum class Handle
    {
        none,
        attack,
        decay,
        sustain,
        release
    };
    /** The handle near a point (local coordinates). */
    Handle handleAt(juce::Point<float> p) const;
    juce::Point<float> handlePosition(Handle h) const;

    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;

    /** A time in ms as a share (0..1) of a segment's width, and back. */
    static float widthFor(float ms, float maxMs);
    static float msFor(float share, float maxMs);

protected:
    void drawContent(juce::Graphics&, juce::Rectangle<float> area) override;

private:
    ValueBinding* bindingFor(Handle h) const;
    float segment() const { return contentArea().getWidth() / 4.0f; }

    std::array<std::unique_ptr<ValueBinding>, 4> values; // attack, decay, sustain, release
    Handle dragging = Handle::none;
};

/** The magnitude (dB) of a filter at a frequency: types as the Synth's and Auto Filter's (Low-pass
    12, Low-pass 24, High-pass 12, High-pass 24, Band-pass, Notch, Ladder), resonance 0..1. */
float filterResponseDb(int type, double cutoff, float resonance, double hz);

/** Draws a filter's response on a frequency grid (-36..+24 dB). */
void drawFilterResponse(juce::Graphics& g, juce::Rectangle<float> area, const Theme& t, int type, double cutoff, float resonance);

/** A display drawing fn(area) with the device's values, redrawn when they change. */
class FunctionDisplay final : public DeviceDisplay
{
public:
    FunctionDisplay(DeviceContext& context, const juce::ValueTree& device,
                    std::function<void(juce::Graphics&, juce::Rectangle<float>, DeviceMirror&, const Theme&)> draw);

    DeviceMirror& mirror() noexcept { return values; }

protected:
    void drawContent(juce::Graphics& g, juce::Rectangle<float> area) override;

private:
    DeviceMirror values;
    std::function<void(juce::Graphics&, juce::Rectangle<float>, DeviceMirror&, const Theme&)> drawFn;
};

/** The tuning table a device's track plays (12-TET A440 if it has none), and the track's note offset. */
TuningPtr tuningOf(DeviceContext& context, const juce::ValueTree& device, int* noteOffset = nullptr);

} // namespace lattice::ui::panels
