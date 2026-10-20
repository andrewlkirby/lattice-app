#pragma once

// Shared bits of the M2 debug session UI (docs/03-m2-engine-session.md, part 2e). Throwaway: the
// real Live-style UI replaces all of src/app/debug/ in M4.

#include "core/model/SessionTypes.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace lattice::debugui
{

/** What the user has selected in the session grid (moved with the arrow keys). */
struct Selection
{
    int track = 0;
    int scene = 0;
    bool sceneColumn = false; // the scene launch column rather than a clip slot
};

namespace colours
{
inline const juce::Colour background{0xff1e1e1e};
inline const juce::Colour panel{0xff2b2b2b};
inline const juce::Colour emptySlot{0xff333333};
inline const juce::Colour text{0xffdddddd};
inline const juce::Colour dimText{0xff8a8a8a};
inline const juce::Colour play{0xff3ddc68};
inline const juce::Colour record{0xffff4b4b};
inline const juce::Colour selection{0xffffffff};
inline const juce::Colour solo{0xff4aa3ff};
inline const juce::Colour mute{0xffffc94a};
} // namespace colours

inline juce::String quantizationName(Quantization q)
{
    switch (q)
    {
    case Quantization::Global:
        return "Global";
    case Quantization::None:
        return "None";
    case Quantization::Bars8:
        return "8 Bars";
    case Quantization::Bars4:
        return "4 Bars";
    case Quantization::Bars2:
        return "2 Bars";
    case Quantization::Bar1:
        return "1 Bar";
    case Quantization::Half:
        return "1/2";
    case Quantization::Quarter:
        return "1/4";
    case Quantization::Eighth:
        return "1/8";
    case Quantization::Sixteenth:
        return "1/16";
    }
    return {};
}

inline juce::String followActionName(FollowAction a)
{
    static const char* const names[] = {"None",  "Stop", "Play Again", "Previous", "Next",
                                        "First", "Last", "Any",        "Other"};
    return names[static_cast<int>(a)];
}

/** Asks for a line of text without a modal loop; calls onOk with the text if the user confirms. */
inline void askForText(const juce::String& title, const juce::String& prompt, const juce::String& initial,
                       std::function<void(const juce::String&)> onOk)
{
    auto* window = new juce::AlertWindow(title, prompt, juce::MessageBoxIconType::NoIcon);
    window->addTextEditor("text", initial);
    window->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    window->enterModalState(true,
                            juce::ModalCallbackFunction::create([window, onOk = std::move(onOk)](int result) {
                                if (result == 1 && onOk)
                                    onOk(window->getTextEditorContents("text"));
                            }),
                            true);
}

/** askForText for a callback that uses `owner`: it isn't called if owner has gone meanwhile (a
    panel rebuilt while the dialog was open). */
inline void askForText(juce::Component& owner, const juce::String& title, const juce::String& prompt,
                       const juce::String& initial, std::function<void(const juce::String&)> onOk)
{
    askForText(title, prompt, initial,
               [safe = juce::Component::SafePointer<juce::Component>(&owner), onOk = std::move(onOk)](const juce::String& text) {
                   if (safe != nullptr && onOk)
                       onOk(text);
               });
}

} // namespace lattice::debugui
