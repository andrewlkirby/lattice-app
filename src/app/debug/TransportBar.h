#pragma once

#include "app/debug/SessionUi.h"
#include "engine/SessionController.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace lattice::debugui
{

/** Play, stop, session record, tempo with tap, time signature, global quantization, metronome,
    fixed recording length, the bar.beat.sixteenth position, undo/redo, and the file and
    track/scene buttons. */
class TransportBar final : public juce::Component
{
public:
    explicit TransportBar(SessionController& session);

    /** The UI tick: position, play/record lights. */
    void update();
    /** Reads the document (call when it changed). */
    void refresh();

    std::function<void()> onNew;
    std::function<void()> onOpen;
    std::function<void()> onSave;
    std::function<void()> onSaveAs;
    std::function<void()> onImportMidi;
    /** The scene a session-record press records into. */
    std::function<int()> selectedScene;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    SessionController& session;

    juce::TextButton play{juce::String::fromUTF8("\xe2\x96\xb6")};   // ▶
    juce::TextButton stop{juce::String::fromUTF8("\xe2\x96\xa0")};   // ■
    juce::TextButton record{juce::String::fromUTF8("\xe2\x97\x8f")}; // ●
    juce::Slider tempo{juce::Slider::LinearBar, juce::Slider::TextBoxLeft};
    juce::TextButton tap{"Tap"};
    juce::ComboBox sigNum;
    juce::ComboBox sigDen;
    juce::ComboBox quantization;
    juce::TextButton metronome{"Metro"};
    juce::ComboBox fixedLength;
    juce::Label position;
    juce::TextButton undo{"Undo"};
    juce::TextButton redo{"Redo"};
    juce::TextButton newSet{"New"};
    juce::TextButton open{"Open..."};
    juce::TextButton save{"Save"};
    juce::TextButton saveAs{"Save as..."};
    juce::TextButton addTrack{"+ Track"};
    juce::TextButton addScene{"+ Scene"};
    juce::TextButton importMidi{"Import MIDI..."};
    juce::String shownPosition;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransportBar)
};

} // namespace lattice::debugui
