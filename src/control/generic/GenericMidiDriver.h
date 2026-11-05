#pragma once

#include "control/generic/SurfaceDefinition.h"

#include <juce_events/juce_events.h>

#include <array>
#include <mutex>

namespace lattice::control::generic
{

/** Where a driver's MIDI goes: the device's output port (tests record it). */
class MidiSender
{
public:
    virtual ~MidiSender() = default;
    virtual void send(const juce::MidiMessage& message) = 0;
};

/** A controller described by a definition file (docs/06-m5-controllers.md, "Generic MIDI
    controllers"): its MIDI turned into pads, buttons and encoders for the modes, and the modes'
    lights and words turned into its MIDI.

    - Input arrives on the device's MIDI thread (receive()): pads play on the fast path there,
      then everything goes on to the controller on the message thread, in order. A message the
      definition doesn't know is left for whoever else listens (MIDI Map, the tracks).
    - Lights: only what changed is sent, each light as the definition says (on/off values, a
      palette's nearest colour, or an RGB sysex). Blinking and pulsing use the device's own
      channels for them where it has them; otherwise the light is steady.
    - A text display shows the frame as displayText() puts it, a changed line at a time. */
class GenericMidiDriver final : public SurfaceDriver, private juce::AsyncUpdater
{
public:
    GenericMidiDriver(SurfaceDefinition definition, MidiSender& output);
    ~GenericMidiDriver() override;

    const SurfaceDefinition& definition() const noexcept { return def; }
    const SurfaceCapabilities& capabilities() const override { return caps; }
    void showLeds(const LedFrame& frame, const LedFrame::Changes& changed) override;
    void showDisplay(const DisplayFrame& frame) override;

    /** Sends the definition's init messages (programmer mode, say), then every light as shown. */
    void connect();
    /** Every light off, then the exit messages. */
    void disconnect();

    /** The device's input thread. True if the message is the surface's (a pad, a button, an
        encoder, a pressure); false for one to pass on. seconds: when it arrived
        (MidiInputQueue::nowSeconds()'s clock; default now). */
    bool receive(const juce::MidiMessage& message, double seconds = -1.0);
    /** Message thread: hands what has arrived to the controller now (otherwise it goes soon). */
    void deliverPending();

    /** What a message is on this device, for the Surface Monitor ("pad row 1, column 2",
        "button scene[3]", "encoder 4"); empty if it isn't the surface's. Any thread. */
    juce::String describe(const juce::MidiMessage& message) const;

private:
    enum class Part : std::uint8_t
    {
        none,
        pad,
        button,
        encoder,
        encoderLsb,
        touch
    };
    struct Target
    {
        Part part = Part::none;
        std::uint16_t index = 0;
    };
    Target& target(MidiAddress::Type type, int channel, int number) noexcept
    {
        return targets[static_cast<std::size_t>((static_cast<int>(type) * 16 + channel) * 128 + number)];
    }
    const Target& target(MidiAddress::Type type, int channel, int number) const noexcept
    {
        return targets[static_cast<std::size_t>((static_cast<int>(type) * 16 + channel) * 128 + number)];
    }
    void input(SurfaceInput in, double seconds);
    void handleAsyncUpdate() override { deliverPending(); }
    void sendLight(const MidiAddress& at, const LedOutput& led, LedState state);
    const LedOutput& buttonLed(const SurfaceDefinition::Button& b) const noexcept;
    bool encoderValue(int index, int value);

    SurfaceDefinition def;
    SurfaceCapabilities caps;
    MidiSender& out;
    std::array<Target, 2 * 16 * 128> targets{};

    // The input thread's own.
    std::vector<bool> held;                 // pads down (channel pressure goes to them)
    std::vector<int> msb;                   // 14-bit encoders: the high part waiting for its low part
    struct Nrpn
    {
        int parameter = -1, high = -1;
        int parameterMsb = 0;
    };
    std::array<Nrpn, 16> nrpn{};

    std::mutex pendingLock;
    std::vector<SurfaceInput> pending;

    // The message thread's.
    LedFrame shown;
    bool shownAny = false;
    std::vector<juce::String> lines;
};

} // namespace lattice::control::generic
