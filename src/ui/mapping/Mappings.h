#pragma once

#include "devices/framework/DeviceSpecs.h"
#include "engine/SessionController.h"
#include "ui/commands/CommandRegistry.h"
#include "ui/widgets/ValueBinding.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <map>
#include <memory>

namespace lattice::ui
{

/** MIDI Map and Key Map (docs/05-m4-ui.md, 4g): any control, clip slot, scene or command played
    from a MIDI note or CC, or from a computer key, so any controller works before M5's drivers.

    Mappings are saved in the set (a MAPPINGS node under its root, undoable). Each names its
    source and its target by an address that outlives the widgets (ValueBinding::target):
    "device:<id>/<parameter>", "node:<id>/<property>", "master/<property>",
    "send:<track id>/<return id>", "slot:<track id>/<scene>", "scene:<scene id>",
    "command:<command id>".

    - A CC moves a value within the mapping's range (min..max of its travel). Absolute CCs take
      over by Pickup (nothing happens until the control passes the value) or Value Scaling (the
      value follows, closing the gap as the control moves); relative encoders step it (two's
      complement, binary offset or sign-magnitude).
    - A note, or a key, toggles a value between its range's ends; on a slot, scene or command it
      presses it (a note's end releases a Gate clip).
    - A turn of a knob is one undo step: values change inside a gesture that ends when the control
      rests.

    In map mode a target is chosen (setLearnTarget) and the next note, CC or key is mapped to it;
    a source already mapped moves to the new target. MIDI comes from the inputs whose Remote switch
    is on (the Preferences' MIDI tab). Message thread only. */
class Mappings final : private juce::Timer
{
public:
    enum class Mode
    {
        off,
        midi,
        key
    };
    enum class Takeover
    {
        pickup,
        scaling
    };
    enum class Encoder
    {
        absolute,
        twosComplement, // 1..63 up, 127..65 down
        binaryOffset,   // 64 is still: 65.. up, ..63 down
        signMagnitude   // 1..63 up, 65..127 down
    };

    Mappings(SessionController& session, CommandRegistry& commands, DeviceSpecs& specs);
    ~Mappings() override;

    void setMode(Mode m);
    Mode mode() const noexcept { return current; }
    /** What the next note, CC or key maps to (and its name, shown in the list). */
    void setLearnTarget(const juce::String& target, const juce::String& name);
    const juce::String& learnTarget() const noexcept { return learning; }

    /** The mappings, in the order made. */
    std::vector<juce::ValueTree> all() const;
    /** The mappings to a target (a MIDI one and a key one at most). */
    std::vector<juce::ValueTree> forTarget(const juce::String& target) const;
    /** "CC 21 · Ch 1", "Note C3 · Ch 10", "Key Ctrl+1". */
    static juce::String sourceText(const juce::ValueTree& mapping);
    void remove(const juce::ValueTree& mapping);
    void setRange(const juce::ValueTree& mapping, double min, double max);
    void setTakeover(const juce::ValueTree& mapping, Takeover t);
    void setEncoder(const juce::ValueTree& mapping, Encoder e);

    /** A MIDI message from an input: learnt in MIDI map mode, played otherwise. True if it was
        used (learnt or played by a mapping). */
    bool handleMidi(const juce::String& input, const juce::MidiMessage& message);
    /** A key: learnt in Key Map mode, played otherwise. True if it was used. */
    bool handleKey(const juce::KeyPress& key);
    /** Keys the computer keyboard plays while it's on, which Key Map leaves to it. */
    std::function<bool(const juce::KeyPress&)> isKeyboardKey;
    /** Where MIDI from the remote inputs comes from (AudioEngine::takeRemoteMidi), read about 60
        times a second. */
    std::function<std::vector<std::pair<juce::String, juce::MidiMessage>>()> pullMidi;

    /** Tests: ends the gestures under way now (as a control at rest does). */
    void endGestures();

    static const juce::Identifier mappingsType, mappingType;
    static constexpr int restMs = 400; // a control resting this long has ended its gesture

    /** After any change to the mappings or the mode (the views repaint). */
    std::function<void()> onChange;

private:
    struct Source
    {
        bool key = false;
        juce::String input; // MIDI: the input's identifier
        int channel = 0, number = 0;
        bool note = false;
        juce::String keyText;
    };
    static Source sourceOf(const juce::ValueTree& mapping);
    static bool sameSource(const Source& a, const Source& b);
    juce::ValueTree list(bool create);
    void learn(const Source& source);
    /** Plays a mapping: a CC's value (0..127), or a press (value > 0) and release (0). */
    void play(const juce::ValueTree& mapping, int value, bool isNote);
    ValueBinding* bindingFor(const juce::String& target);
    bool press(const juce::String& target, bool down);
    void timerCallback() override;
    void changed();

    SessionController& session;
    CommandRegistry& commands;
    DeviceSpecs& specs;
    Mode current = Mode::off;
    juce::String learning, learningName;
    std::map<juce::String, std::unique_ptr<ValueBinding>> bindings; // by target
    struct Held
    {
        double last = -1.0; // the control's last position (0..1), for pickup and scaling
        bool picked = false;
        juce::int64 touched = 0;
    };
    std::map<juce::String, Held> state; // by mapping uid
    juce::int64 nextUid = 1;
};

} // namespace lattice::ui
