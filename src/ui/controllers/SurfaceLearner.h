#pragma once

#include "control/generic/SurfaceDefinition.h"

#include <optional>
#include <vector>

namespace lattice::ui
{

/** The learn wizard's work (docs/06-m5-controllers.md, "Learn wizard"): a surface definition from
    what a device sends while someone presses and turns its controls, a step at a time. The window
    shows instruction() and feeds it what the device sends (heard()); the result is a definition
    file to check and edit (definition()).

    1. Ports and a name, and the grid's size (0 x 0: no grid).
    2. The grid: the top-left pad, the one to its right, then the bottom-right. Evenly numbered
       grids are worked out from those; any other is learned pad by pad.
    3. Encoders, one at a time: turned clockwise, then anticlockwise; the encoding is worked out
       from the values (14-bit pairs and NRPNs from the messages).
    4. Buttons: each one pressed, then given a role.
    5. Lights: the common ways of lighting are tried on the device (testPattern()); the one that
       looks right is kept. */
class SurfaceLearner
{
public:
    enum class Step
    {
        grid,     // pressing the grid's pads
        encoders, // turning encoders
        buttons,  // pressing buttons and giving them roles
        lights,   // choosing how the lights are driven
        done
    };

    /** Starts over for a device on these ports, with a grid of rows x columns (0: none). */
    void start(const juce::String& name, const juce::String& input, const juce::String& output, int rows, int columns);
    Step step() const noexcept { return now; }
    /** What to do now, in a sentence. */
    juce::String instruction() const;
    /** What the device sent last, said plainly ("note 81 on channel 1"). */
    const juce::String& lastHeard() const noexcept { return heardText; }

    /** Message thread: what the device sent. */
    void heard(const juce::MidiMessage& message);

    // --- Encoders -------------------------------------------------------------------------------
    /** Turned clockwise so far; next: anticlockwise. */
    void encoderTurnedClockwise();
    /** The encoder is done: its encoding worked out. False (and a reason in lastHeard()) if the
        values didn't say; it can be turned again. */
    bool encoderDone();
    int encodersLearned() const noexcept { return static_cast<int>(result.encoders.size()); }
    /** No more encoders: on to the buttons. */
    void encodersFinished();

    // --- Buttons --------------------------------------------------------------------------------
    /** The button pressed last that has no role yet. */
    bool hasPendingButton() const noexcept { return pending.has_value(); }
    /** Gives it a role (a numbered one with its index). False if that role is taken. */
    bool assign(control::ButtonId id);
    int buttonsLearned() const noexcept { return static_cast<int>(result.buttons.size()); }
    void buttonsFinished();

    // --- Lights ---------------------------------------------------------------------------------
    struct LightChoice
    {
        juce::String name;
        control::generic::LedOutput led;
    };
    /** The ways of lighting to try, most common first; the last is "no lights". */
    static const std::vector<LightChoice>& lightChoices();
    /** What lights the grid's corners red, green, blue and white (and every button) one way, to
        send to the device's output. */
    std::vector<juce::MidiMessage> testPattern(int choice) const;
    void chooseLights(int choice);

    // --- Grid -----------------------------------------------------------------------------------
    /** The grid's pads learned so far (row by row). */
    int padsLearned() const noexcept { return static_cast<int>(pads.size()); }
    bool learningPadByPad() const noexcept { return padByPad; }

    /** The definition so far (unverified: it's been learned, not checked). */
    control::generic::SurfaceDefinition definition() const;

private:
    using MidiAddress = control::generic::MidiAddress;
    static std::optional<MidiAddress> addressOf(const juce::MidiMessage& m);
    bool taken(const MidiAddress& a) const;
    void gridPress(const MidiAddress& a, const juce::MidiMessage& m);
    void finishGrid();
    void encoderMessage(const juce::MidiMessage& m);

    Step now = Step::done;
    control::generic::SurfaceDefinition result;
    int rows = 0, columns = 0;
    juce::String heardText;

    // The grid.
    std::vector<MidiAddress> pads; // the presses (corners), or every pad (pad by pad)
    bool padByPad = false;
    std::vector<int> velocities;

    // The encoder being turned.
    struct Turning
    {
        std::optional<int> cc;     // the CC it sends (the MSB's)
        int channel = 0;
        bool lsbSeen = false;      // its CC + 32 too: a 14-bit pair
        int nrpn = -1;             // an NRPN parameter, if that's what it sends
        int nrpnMsb = 0;
        std::vector<int> clockwise, anticlockwise;
        bool backwards = false;
    } turning;

    std::optional<MidiAddress> pending; // a button pressed, no role yet
};

} // namespace lattice::ui
