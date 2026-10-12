#include "control/ComputerKeyboardInput.h"

#include "tuning/TuningTable.h"

#include <algorithm>
#include <cstring>

namespace lattice
{

std::optional<int> ComputerKeyboardInput::noteForKey(juce::juce_wchar key, int base) noexcept
{
    const auto lower = juce::CharacterFunctions::toLowerCase(key);
    if (lower <= 0 || lower > 127)
        return std::nullopt;
    const char* found = std::strchr(noteKeys, static_cast<int>(lower));
    if (found == nullptr)
        return std::nullopt;

    const int note = base + static_cast<int>(found - noteKeys);
    if (note < 0 || note > 127)
        return std::nullopt;
    return note;
}

ComputerKeyboardInput::ComputerKeyboardInput(Sender sender) : send(std::move(sender))
{
}

void ComputerKeyboardInput::configureForTuning(const TuningTable& tuning)
{
    releaseAll();
    pageSize = std::max(1, tuning.keyboardPageSize);
    baseNote = std::clamp(tuning.defaultKeyboardBase(), 0, 127);
    changed();
}

bool ComputerKeyboardInput::keyDown(juce::juce_wchar character, int keyCode)
{
    const auto c = juce::CharacterFunctions::toLowerCase(character);

    if (c == 'm')
    {
        setEnabled(!enabled);
        return true;
    }
    if (!enabled)
        return false;

    switch (c)
    {
    case 'z':
        shiftRange(-1);
        return true;
    case 'x':
        shiftRange(+1);
        return true;
    case 'c':
        velocity = std::clamp(velocity - velocityStep, 1, 127);
        changed();
        return true;
    case 'v':
        velocity = std::clamp(velocity + velocityStep, 1, 127);
        changed();
        return true;
    default:
        break;
    }

    const auto note = noteForKey(c, baseNote);
    if (!note)
        return c > 0 && c <= 127 && std::strchr(noteKeys, static_cast<int>(c)) != nullptr; // note key past 127

    // Auto-repeat: the key is already holding a note.
    if (std::any_of(held.begin(), held.end(), [&](const Held& h) { return h.keyCode == keyCode; }))
        return true;

    held.push_back({keyCode, *note});
    send(juce::MidiMessage::noteOn(channel, *note, static_cast<juce::uint8>(velocity)));
    return true;
}

void ComputerKeyboardInput::keyUp(int keyCode)
{
    const auto it = std::find_if(held.begin(), held.end(), [&](const Held& h) { return h.keyCode == keyCode; });
    if (it == held.end())
        return;
    const int note = it->note;
    held.erase(it);
    send(juce::MidiMessage::noteOff(channel, note));
}

void ComputerKeyboardInput::setEnabled(bool shouldBeEnabled)
{
    if (enabled == shouldBeEnabled)
        return;
    if (!shouldBeEnabled)
        releaseAll();
    enabled = shouldBeEnabled;
    changed();
}

void ComputerKeyboardInput::setPageSize(int size)
{
    pageSize = std::max(1, size);
    changed();
}

void ComputerKeyboardInput::setBaseNote(int note)
{
    releaseAll();
    baseNote = std::clamp(note, 0, 127);
    changed();
}

void ComputerKeyboardInput::shiftRange(int direction)
{
    const int target = baseNote + direction * pageSize;
    if (target < 0 || target > 127)
        return; // already at the end of the range
    releaseAll();
    baseNote = target;
    changed();
}

void ComputerKeyboardInput::releaseAll()
{
    const auto notes = std::move(held);
    held.clear();
    for (const auto& h : notes)
        send(juce::MidiMessage::noteOff(channel, h.note));
}

std::vector<int> ComputerKeyboardInput::heldKeyCodes() const
{
    std::vector<int> codes;
    codes.reserve(held.size());
    for (const auto& h : held)
        codes.push_back(h.keyCode);
    return codes;
}

void ComputerKeyboardInput::changed()
{
    if (onStateChanged)
        onStateChanged();
}

} // namespace lattice
