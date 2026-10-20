#pragma once

#include "control/ComputerKeyboardInput.h"
#include "ui/commands/CommandRegistry.h"
#include "ui/feed/EngineStateFeed.h"
#include "ui/widgets/NumberBox.h"
#include "ui/widgets/Toggle.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace lattice::ui
{

/** A button that runs a command: it shows the command's name and shortcut as its tooltip, and the
    info view describes the command. */
class CommandButton final : public juce::TextButton
{
public:
    CommandButton(CommandRegistry& registry, const juce::String& commandId, const juce::String& caption);
    const juce::String& commandId() const noexcept { return id; }

private:
    CommandRegistry& commands;
    juce::String id;
};

/** The audio load as the control bar shows it: the 99th-percentile callback time as a share of the
    buffer, a small bar, and a count of dropouts (callbacks over time plus device xruns) that turns
    red once there's one. The tooltip gives the breakdown. */
class LoadMeter final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit LoadMeter(int workerThreads);
    void setLoad(const EngineStateFeed::Load& load);
    juce::String text() const;
    std::uint64_t dropouts() const noexcept { return drops; }
    void paint(juce::Graphics&) override;

private:
    int workers;
    double p99 = 0.0;
    std::uint64_t drops = 0;
};

/** The control bar along the top of the window (docs/05-m4-ui.md, 4a): tap tempo and tempo, time
    signature, metronome, global quantization, the bar.beat.sixteenth position, play, stop and
    session record, the computer keyboard with its range and velocity, the MIDI and Key Map mode
    buttons (4g), and the audio load. Every control is a bound widget or a command. The set's own
    properties are bound once: loading a set copies into the same tree, so the bindings stay. */
class ControlBar final : public juce::Component, private EngineStateFeed::Listener
{
public:
    ControlBar(SessionController& session, CommandRegistry& registry, EngineStateFeed& feed, int workerThreads);
    ~ControlBar() override;

    /** The computer keyboard whose state the bar shows and switches (may be null). */
    void setComputerKeyboard(ComputerKeyboardInput* keyboard);

    juce::String positionText() const { return position.getText(); }
    juce::String keyboardText() const { return keyboardInfo.getText(); }
    Toggle& playButton() noexcept { return play; }
    Toggle& recordButton() noexcept { return record; }
    LoadMeter& loadMeter() noexcept { return load; }

    int preferredHeight() const;
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void engineStateChanged(std::uint32_t aspects) override;
    void refreshKeyboard();

    SessionController& session;
    CommandRegistry& commands;
    EngineStateFeed& feed;

    CommandButton tap;
    std::unique_ptr<NumberBox> tempo, signatureTop, signatureBottom, quantization;
    std::unique_ptr<Toggle> metronome;
    juce::Label position;
    Toggle play{juce::String::fromUTF8("\xe2\x96\xb6"), Toggle::Role::play};       // ▶
    CommandButton stop;                                                          // ■
    Toggle record{juce::String::fromUTF8("\xe2\x97\x8f"), Toggle::Role::record};   // ●
    Toggle keys{"Keys", Toggle::Role::accent};
    juce::Label keyboardInfo;
    Toggle midiMap{"MIDI", Toggle::Role::accent}, keyMap{"KEY", Toggle::Role::accent};
    LoadMeter load;

    ComputerKeyboardInput* keyboard = nullptr;
    std::function<void()> previousKeyboardCallback;
};

} // namespace lattice::ui
