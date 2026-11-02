#include "ui/widgets/Accessible.h"

namespace lattice::ui::accessible
{
namespace
{
class DescribedValue final : public juce::AccessibilityTextValueInterface
{
public:
    explicit DescribedValue(std::function<juce::String()> d) : describe(std::move(d)) {}

    bool isReadOnly() const override { return true; }
    juce::String getCurrentValueAsString() const override { return describe ? describe() : juce::String(); }
    void setValueAsString(const juce::String&) override {}

private:
    std::function<juce::String()> describe;
};
} // namespace

std::unique_ptr<juce::AccessibilityHandler> viewHandler(juce::Component& view, juce::AccessibilityRole role,
                                                        std::function<juce::String()> describe)
{
    return std::make_unique<juce::AccessibilityHandler>(
        view, role, juce::AccessibilityActions().addAction(juce::AccessibilityActionType::focus, [&view] { view.grabKeyboardFocus(); }),
        juce::AccessibilityHandler::Interfaces{std::make_unique<DescribedValue>(std::move(describe))});
}

void selectionChanged(juce::Component& view)
{
    auto* handler = view.getAccessibilityHandler();
    if (handler == nullptr)
        return;
    handler->notifyAccessibilityEvent(juce::AccessibilityEvent::valueChanged);
    // Said aloud only for the view the keys are in (a selection that follows from elsewhere is
    // not), and only when it reads differently (a drag changes the notes many times a second).
    if (!view.hasKeyboardFocus(true))
        return;
    auto* value = handler->getValueInterface();
    if (value == nullptr)
        return;
    static const juce::Identifier lastSaid{"latticeAccessibleLastSaid"};
    const auto text = value->getCurrentValueAsString();
    if (text.isEmpty() || view.getProperties()[lastSaid].toString() == text)
        return;
    view.getProperties().set(lastSaid, text);
    juce::AccessibilityHandler::postAnnouncement(text, juce::AccessibilityHandler::AnnouncementPriority::medium);
}

namespace
{
const juce::Identifier viewFlag{"latticeAccessibleView"};
}

void markAsView(juce::Component& view)
{
    view.getProperties().set(viewFlag, true);
}

bool isView(const juce::Component* component)
{
    return component != nullptr && static_cast<bool>(component->getProperties()[viewFlag]);
}

bool focusFirstIn(juce::Component& area)
{
    if (!area.isShowing())
        return false;
    if (area.getWantsKeyboardFocus())
    {
        area.grabKeyboardFocus();
        return area.hasKeyboardFocus(false);
    }
    const auto traverser = area.createKeyboardFocusTraverser();
    for (auto* c : traverser->getAllComponents(&area))
        if (c->isShowing() && c->isEnabled())
        {
            c->grabKeyboardFocus();
            if (c->hasKeyboardFocus(false))
                return true;
        }
    return false;
}

juce::String valueOf(juce::Component& view)
{
    auto* handler = view.getAccessibilityHandler();
    if (handler == nullptr)
        return {};
    auto* value = handler->getValueInterface();
    return value != nullptr ? value->getCurrentValueAsString() : juce::String();
}

} // namespace lattice::ui::accessible
