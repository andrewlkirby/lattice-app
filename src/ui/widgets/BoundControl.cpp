#include "ui/widgets/BoundControl.h"

#include "ui/theme/LatticeLookAndFeel.h"

#include <cmath>

namespace lattice::ui
{
namespace
{
constexpr int stepBurstMs = 500; // steps closer together than this are one undo step

/** The value as screen readers see it: the plain value with its range, and the formatted text. */
class BindingValueInterface final : public juce::AccessibilityValueInterface
{
public:
    explicit BindingValueInterface(BoundControl& c) : control(c) {}

    bool isReadOnly() const override { return !control.isEnabled(); }
    double getCurrentValue() const override { return control.binding().value(); }
    juce::String getCurrentValueAsString() const override { return control.binding().text(); }
    void setValue(double newValue) override { control.binding().setValue(static_cast<float>(newValue)); }
    void setValueAsString(const juce::String& newValue) override { control.binding().setFromText(newValue); }
    AccessibleValueRange getRange() const override
    {
        const auto& s = control.binding().spec();
        const double step = s.interval > 0.0f ? s.interval : (s.maxValue - s.minValue) / 100.0;
        return {{s.minValue, s.maxValue}, step};
    }

private:
    BoundControl& control;
};
} // namespace

BoundControl::BoundControl(std::unique_ptr<ValueBinding> b) : bound(std::move(b))
{
    jassert(bound != nullptr);
    setWantsKeyboardFocus(true);            // Tab reaches it,
    setMouseClickGrabsKeyboardFocus(false); // but a click doesn't take the arrows, Enter and Space from the view
    setTitle(bound->spec().name);
    setHelpText(bound->spec().group.isNotEmpty() ? bound->spec().group + " > " + bound->spec().name : bound->spec().name);
    bound->onChange = [this] {
        repaint();
        if (auto* handler = getAccessibilityHandler())
            handler->notifyAccessibilityEvent(juce::AccessibilityEvent::valueChanged);
    };
}

BoundControl::~BoundControl()
{
    if (registry != nullptr)
        registry->remove(this);
    bound->onChange = nullptr;
    if (dragging || stepping)
        bound->endGesture();
}

float BoundControl::pixelsForFullTravel(bool fine) const
{
    const auto& s = bound->spec();
    if (s.isDiscrete() && s.interval > 0.0f)
    {
        // About 10 px a step, within limits, so short lists aren't twitchy and long ones are reachable.
        const float steps = (s.maxValue - s.minValue) / s.interval;
        return juce::jlimit(60.0f, 300.0f, steps * 10.0f) * (fine ? 3.0f : 1.0f);
    }
    return fine ? 2000.0f : 200.0f;
}

void BoundControl::parentHierarchyChanged()
{
    ControlRegistry::place(*this, bound.get(), registry);
}

void BoundControl::beginDrag()
{
    finishSteps();
    bound->beginGesture();
    dragPosition = bound->normalised();
    dragging = true;
    repaint();
}

void BoundControl::dragBy(float pixels, bool fine)
{
    if (!dragging || pixels == 0.0f)
        return;
    // Clamped at the ends, so turning back responds at once.
    dragPosition = juce::jlimit(0.0f, 1.0f, dragPosition + pixels / pixelsForFullTravel(fine));
    bound->setNormalised(dragPosition);
}

void BoundControl::endDrag()
{
    if (!dragging)
        return;
    dragging = false;
    bound->endGesture();
    repaint();
}

void BoundControl::stepBy(int steps, bool fine)
{
    if (steps == 0 || dragging)
        return;
    if (!stepping)
    {
        bound->beginGesture();
        stepping = true;
    }
    startTimer(stepBurstMs);
    const auto& s = bound->spec();
    if (s.isDiscrete() && s.interval > 0.0f)
        bound->setValue(bound->value() + static_cast<float>(steps) * s.interval);
    else
        bound->setNormalised(bound->normalised() + static_cast<float>(steps) * (fine ? 0.001f : 0.01f));
}

void BoundControl::finishSteps()
{
    stopTimer();
    if (!stepping)
        return;
    stepping = false;
    bound->endGesture();
}

void BoundControl::timerCallback()
{
    finishSteps();
}

void BoundControl::mouseDown(const juce::MouseEvent& e)
{
    if (!isEnabled())
        return;
    if (e.mods.isPopupMenu())
    {
        showMenu();
        return;
    }
    moved = false;
    lastY = e.position.y;
    beginDrag();
    // The pointer hides and can move without limit while dragging, as in Live.
    e.source.enableUnboundedMouseMovement(true);
}

void BoundControl::mouseDrag(const juce::MouseEvent& e)
{
    if (!dragging)
        return;
    const float dy = lastY - e.position.y;
    lastY = e.position.y;
    if (dy != 0.0f)
        moved = true;
    dragBy(dy, e.mods.isShiftDown());
}

void BoundControl::mouseUp(const juce::MouseEvent& e)
{
    if (!dragging)
        return;
    e.source.enableUnboundedMouseMovement(false);
    endDrag();
    if (!moved)
        clicked();
}

void BoundControl::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (isEnabled() && !e.mods.isPopupMenu())
        bound->resetToDefault();
}

void BoundControl::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (!isEnabled())
        return;
    float delta = std::abs(wheel.deltaY) >= std::abs(wheel.deltaX) ? wheel.deltaY : -wheel.deltaX;
    if (wheel.isReversed)
        delta = -delta;
    if (delta != 0.0f)
        stepBy(delta > 0.0f ? 1 : -1, e.mods.isShiftDown());
}

bool BoundControl::keyPressed(const juce::KeyPress& key)
{
    if (!isEnabled())
        return false;
    const bool fine = key.getModifiers().isShiftDown();
    const int code = key.getKeyCode();
    if (code == juce::KeyPress::upKey || code == juce::KeyPress::rightKey)
    {
        stepBy(1, fine);
        return true;
    }
    if (code == juce::KeyPress::downKey || code == juce::KeyPress::leftKey)
    {
        stepBy(-1, fine);
        return true;
    }
    if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey)
    {
        bound->resetToDefault();
        return true;
    }
    if (code == juce::KeyPress::returnKey)
    {
        showTypeInEditor();
        return true;
    }
    return false; // letters go on to the computer keyboard
}

void BoundControl::showTypeInEditor()
{
    if (editor != nullptr || !isEnabled())
        return;
    const auto& theme = themeOf(*this);
    editor = std::make_unique<juce::TextEditor>();
    editor->setFont(juce::Font(theme.font(theme.fontNormal)));
    editor->setJustification(juce::Justification::centred);
    editor->setIndents(2, 0);
    editor->setText(bound->text(), false);
    editor->selectAll();
    editor->onReturnKey = [this] { closeEditor(true); };
    editor->onEscapeKey = [this] { closeEditor(false); };
    editor->onFocusLost = [this] { closeEditor(true); };
    auto area = typeInArea();
    if (area.getHeight() < static_cast<int>(theme.controlHeight))
        area = area.withSizeKeepingCentre(area.getWidth(), static_cast<int>(theme.controlHeight));
    editor->setBounds(area.getIntersection(getLocalBounds()));
    addAndMakeVisible(*editor);
    focusBeforeEditor = juce::Component::getCurrentlyFocusedComponent();
    editor->grabKeyboardFocus();
}

void BoundControl::commitTypeIn(const juce::String& text)
{
    bound->setFromText(text);
}

void BoundControl::closeEditor(bool commit)
{
    if (editor == nullptr)
        return;
    const auto text = editor->getText();
    // The editor is closing from inside one of its own callbacks: delete it once they return.
    auto* closing = editor.release();
    removeChildComponent(closing);
    juce::MessageManager::callAsync([closing] { delete closing; });
    if (auto* before = focusBeforeEditor.getComponent(); before != nullptr && before->isShowing())
        before->grabKeyboardFocus();
    focusBeforeEditor = nullptr;
    if (commit)
        commitTypeIn(text);
    repaint();
}

void BoundControl::showMenu()
{
    juce::PopupMenu menu;
    menu.addSectionHeader(bound->spec().name);
    juce::Component::SafePointer<BoundControl> safe(this);
    menu.addItem("Reset to default", [safe] {
        if (safe != nullptr)
            safe->bound->resetToDefault();
    });
    menu.addItem("Type a value...", [safe] {
        if (safe != nullptr)
            safe->showTypeInEditor();
    });
    if (extendMenu)
        extendMenu(menu);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this));
}

std::unique_ptr<juce::AccessibilityHandler> BoundControl::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler>(
        *this, juce::AccessibilityRole::slider, juce::AccessibilityActions(),
        juce::AccessibilityHandler::Interfaces{std::make_unique<BindingValueInterface>(*this)});
}

} // namespace lattice::ui
