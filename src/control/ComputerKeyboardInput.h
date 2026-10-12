#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <functional>
#include <optional>
#include <vector>

namespace lattice
{

struct TuningTable;

/** Plays notes from the computer keyboard, with Live's layout (docs/02-m1-tuning-core.md):
    - A W S E D F T G Y H U J K O L P ; play 17 consecutive notes from the range's first note.
    - Z / X move the range down / up by the tuning's page size (12 for 12-TET, 17 for Colundi).
    - C / V lower / raise the velocity by 20 (1..127).
    - M turns the keyboard on and off.
    Holding a key never retriggers its note. Turning the keyboard off, shifting the range, or
    calling releaseAll() (e.g. when the window loses focus) releases every held note.

    UI-independent: the window's key handler feeds keyDown/keyUp, and MIDI goes out through the
    sender. Message thread only. */
class ComputerKeyboardInput
{
public:
    using Sender = std::function<void(const juce::MidiMessage&)>;

    static constexpr int numNoteKeys = 17;
    static constexpr const char* noteKeys = "awsedftgyhujkolp;";
    static constexpr int defaultVelocity = 100;
    static constexpr int velocityStep = 20;

    /** The note a key plays with the range starting at baseNote, or nullopt if the key isn't a note
        key or the note would be outside 0..127. Case-insensitive. */
    static std::optional<int> noteForKey(juce::juce_wchar key, int baseNote) noexcept;

    explicit ComputerKeyboardInput(Sender sender);

    /** Adopts a tuning's page size and moves the range to its default page. Releases held notes. */
    void configureForTuning(const TuningTable& tuning);

    /** Returns true if the key was handled. keyCode identifies the physical key for keyUp. */
    bool keyDown(juce::juce_wchar character, int keyCode);
    void keyUp(int keyCode);

    void setEnabled(bool shouldBeEnabled);
    bool isEnabled() const noexcept { return enabled; }

    void setPageSize(int size);
    int getPageSize() const noexcept { return pageSize; }
    void setBaseNote(int note);
    int getBaseNote() const noexcept { return baseNote; }
    int getLastNote() const noexcept { return juce::jmin(baseNote + numNoteKeys - 1, 127); }
    int getVelocity() const noexcept { return velocity; }

    void releaseAll();

    /** Key codes of the keys currently holding notes (for polling key-up). */
    std::vector<int> heldKeyCodes() const;

    /** Called after any change to enabled state, range, or velocity (for the UI readout). */
    std::function<void()> onStateChanged;

private:
    void shiftRange(int direction);
    void changed();

    struct Held
    {
        int keyCode;
        int note;
    };

    Sender send;
    std::vector<Held> held;
    bool enabled = true;
    int pageSize = 12;
    int baseNote = 60;
    int velocity = defaultVelocity;
    int channel = 1;
};

} // namespace lattice
