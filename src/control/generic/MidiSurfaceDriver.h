#pragma once

#include "control/surface/Surface.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_events/juce_events.h>

#include <mutex>
#include <vector>

namespace lattice::control::generic
{

/** Where a driver's MIDI goes: the device's output port (tests record it). */
class MidiSender
{
public:
    virtual ~MidiSender() = default;
    virtual void send(const juce::MidiMessage& message) = 0;
};

/** A surface on MIDI ports, as the surface manager runs it: a definition file's
    (GenericMidiDriver) or a native driver's (Push). Its input arrives on the device's MIDI thread
    (receive()): pads play on the fast path there, then everything goes on to the controller on the
    message thread, in order. */
class MidiSurfaceDriver : public SurfaceDriver, private juce::AsyncUpdater
{
public:
    ~MidiSurfaceDriver() override { cancelPendingUpdate(); }

    /** Sends what the device needs to start (programmer mode, its palette), then every light as
        shown. */
    virtual void connect() = 0;
    /** Every light off, then what puts the device back as it was. */
    virtual void disconnect() = 0;

    /** The device's input thread. True if the message is the surface's (a pad, a button, an
        encoder, a pressure); false for one to pass on (MIDI Map, the tracks). seconds: when it
        arrived (MidiInputQueue::nowSeconds()'s clock; default now). */
    virtual bool receive(const juce::MidiMessage& message, double seconds = -1.0) = 0;

    /** What a message is on this device, for the Surface Monitor ("pad row 1, column 2",
        "button scene[3]", "encoder 4"); empty if it isn't the surface's. Any thread. */
    virtual juce::String describe(const juce::MidiMessage& message) const = 0;

    /** Anything wrong beside the MIDI, for the Preferences list (Push: "the display needs
        WinUSB..."); empty when all is well. Message thread. */
    virtual juce::String status() const { return {}; }

    /** Message thread: hands what has arrived to the controller now (otherwise it goes soon). */
    void deliverPending()
    {
        std::vector<SurfaceInput> now;
        {
            const std::scoped_lock lock(pendingLock);
            now.swap(pending);
        }
        for (const auto& in : now)
            if (onInput)
                onInput(in);
    }

protected:
    /** The input thread: the fast path, then the controller (soon, on the message thread). */
    void input(SurfaceInput in, double seconds);

private:
    void handleAsyncUpdate() override { deliverPending(); }

    std::mutex pendingLock;
    std::vector<SurfaceInput> pending;
};

} // namespace lattice::control::generic
