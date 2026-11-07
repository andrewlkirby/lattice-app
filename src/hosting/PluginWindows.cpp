#include "hosting/PluginWindows.h"

#include "core/model/LiveSet.h"
#include "devices/framework/PluginTypes.h"
#include "hosting/PluginDevice.h"

namespace lattice::hosting
{

class PluginWindows::Window final : public juce::DocumentWindow
{
public:
    Window(PluginWindows& o, std::int64_t deviceId, juce::AudioProcessorEditor* editor, juce::ValueTree node, bool onTop)
        : juce::DocumentWindow(node[ids::name].toString().isNotEmpty() ? node[ids::name].toString() : editor->getAudioProcessor()->getName(),
                               juce::Colours::black, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
          owner(o),
          id(deviceId),
          device(std::move(node))
    {
        setUsingNativeTitleBar(true);
        setContentOwned(editor, true);
        setResizable(editor->isResizable(), false);
        setAlwaysOnTop(onTop);

        // Where it was, if that's still on a screen; else near the middle of the main one.
        const auto saved = juce::StringArray::fromTokens(device[pluginTypes::window].toString(), " ", {});
        auto bounds = getBounds();
        if (saved.size() >= 2)
            bounds.setPosition(saved[0].getIntValue(), saved[1].getIntValue());
        else
            bounds = bounds.withCentre(juce::Desktop::getInstance().getDisplays().getPrimaryDisplay() != nullptr
                                           ? juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userBounds.getCentre().toInt()
                                           : juce::Point<int>(400, 300));
        if (saved.size() >= 4 && editor->isResizable())
            bounds.setSize(saved[2].getIntValue(), saved[3].getIntValue());
        if (juce::Desktop::getInstance().getDisplays().getDisplayForRect(bounds) == nullptr)
            bounds.setPosition(100, 100);
        setBounds(bounds);
        remembering = true;
        setVisible(true);
        toFront(true);
    }

    void closeButtonPressed() override
    {
        // Not from inside its own callback: the window goes once this returns.
        juce::MessageManager::callAsync([w = juce::Component::SafePointer<Window>(this)] {
            if (w != nullptr)
                w->owner.close(w->id);
        });
    }

    void moved() override
    {
        juce::DocumentWindow::moved();
        remember();
    }
    void resized() override
    {
        juce::DocumentWindow::resized();
        remember();
    }

private:
    void remember()
    {
        if (!remembering || !device.isValid())
            return;
        const auto b = getBounds();
        device.setProperty(pluginTypes::window,
                           juce::String(b.getX()) + " " + juce::String(b.getY()) + " " + juce::String(b.getWidth()) + " " +
                               juce::String(b.getHeight()),
                           nullptr); // where a window is isn't an edit
    }

    PluginWindows& owner;
    std::int64_t id;
    juce::ValueTree device;
    bool remembering = false;
};

PluginWindows::PluginWindows() = default;

PluginWindows::~PluginWindows()
{
    closeAll();
}

bool PluginWindows::open(HostedPluginDevice& device, juce::ValueTree deviceNode)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (const auto it = windows.find(device.deviceId()); it != windows.end())
    {
        it->second->toFront(true);
        return true;
    }
    auto& plugin = device.plugin();
    if (!plugin.hasEditor())
        return false;
    auto* editor = plugin.createEditorAndMakeActive();
    if (editor == nullptr)
        return false;
    windows[device.deviceId()] = std::make_unique<Window>(*this, device.deviceId(), editor, std::move(deviceNode), onTop);
    if (onChange)
        onChange();
    return true;
}

void PluginWindows::close(std::int64_t deviceId)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (windows.erase(deviceId) > 0 && onChange)
        onChange();
}

void PluginWindows::closeAll()
{
    const bool any = !windows.empty();
    windows.clear();
    if (any && onChange)
        onChange();
}

juce::DocumentWindow* PluginWindows::windowFor(std::int64_t deviceId) const
{
    const auto it = windows.find(deviceId);
    return it != windows.end() ? it->second.get() : nullptr;
}

void PluginWindows::setAlwaysOnTop(bool on)
{
    if (onTop != on && onAlwaysOnTopChanged)
        onAlwaysOnTopChanged(on);
    onTop = on;
    for (auto& [id, w] : windows)
        w->setAlwaysOnTop(on);
}

} // namespace lattice::hosting
