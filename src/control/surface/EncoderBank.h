#pragma once

#include "control/surface/Display.h"
#include "devices/framework/DeviceSpecs.h"
#include "ui/widgets/ValueBinding.h"

#include <juce_events/juce_events.h>

#include <memory>
#include <vector>

namespace lattice::control
{

/** A device's encoders and what they control (docs/06-m5-controllers.md, 5c): each is given a
    value's address (ValueBinding::target: a device parameter, a track's volume, a send...) and
    edits it through the same bindings MIDI Map and the window's knobs use.

    - A turn is one gesture, so one undo step: it begins at the touch (or the first step, on an
      encoder without touch) and ends at the release (or once the encoder has rested restMs).
    - Relative steps move the value along its knob's travel: the whole range in `resolution`
      steps (ten times as many with fine). A value with few settings (a switch, a choice, a
      small whole number) moves one setting every stepsPerSetting steps instead, so it has
      detents. Steps smaller than a setting add up within the gesture.
    - An absolute control (encoderSet) picks the value up: nothing changes until it reaches the
      value (or passes it between two moves), so it never jumps.
    - An encoder whose address changes (a new page, another track) ends its gesture first.

    Message thread. */
class EncoderBank final : private juce::Timer
{
public:
    EncoderBank(LiveSet& set, DeviceSpecs& specs, juce::UndoManager* undo);
    ~EncoderBank() override;

    /** What each encoder controls now (an address; empty: nothing). Encoders past the end of the
        list control nothing. Cheap when nothing changed (it's called on every refresh). */
    void assign(const std::vector<juce::String>& targets);
    const juce::String& target(int encoder) const;
    /** The binding an encoder edits through, or null. */
    ui::ValueBinding* binding(int encoder) const;

    void turn(int encoder, int steps, int resolution, bool fine);
    void set(int encoder, float position);
    void touch(int encoder);
    void release(int encoder);
    bool isTouched(int encoder) const;
    /** The encoder held now (touched), or -1. */
    int touchedEncoder() const;
    /** Ends every gesture under way (a mode change, the controller going away). */
    void endGestures();

    static constexpr int restMs = 400;
    static constexpr int stepsPerSetting = 10;
    /** A value with more settings than this moves smoothly (by resolution) instead. */
    static constexpr int detentSettings = 24;
    /** The normalised distance one step moves a value (for one with detents: a setting's share
        of stepsPerSetting). */
    static double stepSize(const ParameterSpec& spec, int resolution, bool fine);
    /** How many settings a value has (0: it's continuous, or has too many for detents). */
    static int detents(const ParameterSpec& spec);

private:
    struct Encoder
    {
        juce::String target;
        std::unique_ptr<ui::ValueBinding> binding;
        bool touched = false;
        double position = -1.0; // along the travel, with what's below a setting kept (< 0: from the value)
        double written = -1.0;  // the binding's normalised value after the last write
        double absoluteLast = -1.0;
        float detentFrom = 0.0f; // a value with detents: where the gesture's steps count from
        int detentSteps = 0;
        bool picked = false;
        juce::uint32 movedAt = 0;
    };
    Encoder* at(int encoder);
    const Encoder* at(int encoder) const;
    bool valid(const Encoder& e) const;
    void begin(Encoder& e);
    void end(Encoder& e);
    void moveTo(Encoder& e, double position);
    void timerCallback() override;

    LiveSet& liveSet;
    DeviceSpecs& specs;
    juce::UndoManager* undo;
    std::vector<Encoder> encoders;

    JUCE_DECLARE_NON_COPYABLE(EncoderBank)
};

/** A display cell showing what a binding controls: its name, value, knob and range. */
DisplayCell cellFor(const ui::ValueBinding& binding);

} // namespace lattice::control
