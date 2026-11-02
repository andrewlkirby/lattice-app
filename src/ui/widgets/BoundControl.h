#pragma once

#include "ui/widgets/ControlRegistry.h"
#include "ui/widgets/ValueBinding.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace lattice::ui
{

/** What every widget bound to a value does (docs/05-m4-ui.md, "Common behavior for every widget"):
    - Drag up and down to change it (relative, so it never jumps); hold Shift for fine changes.
      One drag is one undo step.
    - The mouse wheel and the arrow keys step it; a burst of steps is one undo step.
    - Double-click, or Delete when focused: back to the default.
    - Enter when focused, or "Type a value..." in the right-click menu: type a value in place.
    - Right-click: Reset, Type a value, and whatever the owner adds (e.g. map to a rack macro).
    - Screen readers see it as a slider with its name and its value as text.
    Knob, NumberBox and Fader draw it; the drag, step and edit logic is here, with the gesture
    calls public so tests can drive them without a mouse. */
class BoundControl : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    explicit BoundControl(std::unique_ptr<ValueBinding> binding);
    ~BoundControl() override;

    ValueBinding& binding() noexcept { return *bound; }
    const ValueBinding& binding() const noexcept { return *bound; }

    // --- Gestures (the mouse handlers call these) ---------------------------------------------------
    void beginDrag();
    /** pixels: how far the pointer moved towards "more" (up, for a knob) since the last call. */
    void dragBy(float pixels, bool fine);
    void endDrag();
    bool isDragging() const noexcept { return dragging; }
    /** One step up or down (arrow keys, wheel): one interval for a discrete value, 1% of the travel
        otherwise (0.1% when fine). Steps close together in time are one undo step. */
    void stepBy(int steps, bool fine);
    /** Ends a burst of steps now (normally a short pause ends it). */
    void finishSteps();

    /** The pixels of travel from one end of the range to the other. */
    virtual float pixelsForFullTravel(bool fine) const;

    void showTypeInEditor();
    bool isTypingIn() const noexcept { return editor != nullptr; }
    /** For tests: what the type-in editor would commit. */
    void commitTypeIn(const juce::String& text);
    void showMenu();

    /** Adds the owner's items to the right-click menu. */
    std::function<void(juce::PopupMenu&)> extendMenu;

    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed(const juce::KeyPress&) override;
    void focusGained(FocusChangeType) override { repaint(); }
    void focusLost(FocusChangeType) override { repaint(); }
    void enablementChanged() override { repaint(); }
    /** Registers with the control registry of the window it's in (and leaves the old one). */
    void parentHierarchyChanged() override;

    /** What screen readers get: a slider with the value's name, range and text. Public so tests
        can check it without a window (JUCE only hands it out for a component in one). */
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

protected:
    /** Where the type-in editor goes (the value text). */
    virtual juce::Rectangle<int> typeInArea() const { return getLocalBounds(); }
    /** A click that didn't drag (a NumberBox with choices opens their menu). */
    virtual void clicked() {}

private:
    void timerCallback() override;
    void closeEditor(bool commit);

    std::unique_ptr<ValueBinding> bound;
    bool dragging = false;
    bool moved = false;
    float dragPosition = 0.0f; // normalised, unclamped while dragging past an end
    float lastY = 0.0f;
    bool stepping = false;
    std::unique_ptr<juce::TextEditor> editor;
    juce::Component::SafePointer<juce::Component> focusBeforeEditor; // gets the keys back when the editor closes
    ControlRegistry* registry = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BoundControl)
};

} // namespace lattice::ui
