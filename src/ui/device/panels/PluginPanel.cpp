// A plugin's panel (docs/07-m6-plugins-external.md, 6b): where it is (loading, missing and why),
// its window, Configure mode, a menu of every parameter to put on its pages or take off (with MPE,
// its default pages, and whether plugin windows stay on top), and the knobs of its pages, in its
// own groups. Rebuilt when the plugin loads, or its pages or names change.

#include "hosting/PluginHost.h"
#include "hosting/PluginWindows.h"
#include "ui/device/DevicePanels.h"
#include "ui/theme/LatticeLookAndFeel.h"

namespace lattice::ui::panels
{

namespace
{
class PluginBody final : public DeviceBody, private juce::ChangeListener, private juce::ValueTree::Listener
{
public:
    PluginBody(DeviceContext& c, const juce::ValueTree& device) : DeviceBody(c, device)
    {
        status.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(status);
        for (auto* b : {&window, &configure, &pages})
            addAndMakeVisible(*b);
        window.setTitle("Plugin window");
        window.setClickingTogglesState(false);
        window.onClick = [this] { toggleWindow(); };
        configure.setTitle("Configure");
        configure.setTooltip("Parameters you touch in the plugin's window join the device's pages");
        configure.onClick = [this] {
            if (context.plugins != nullptr)
                context.plugins->setConfiguring(LiveSet::idOf(node), !context.plugins->isConfiguring(LiveSet::idOf(node)));
        };
        pages.setTitle("Pages");
        pages.onClick = [this] { showPagesMenu(); };
        if (context.plugins != nullptr)
        {
            context.plugins->addChangeListener(this);
        }
        node.addListener(this);
        refresh();
    }

    ~PluginBody() override
    {
        node.removeListener(this);
        if (context.plugins != nullptr)
            context.plugins->removeChangeListener(this);
    }

    int preferredWidth(int height) const override
    {
        const auto& t = themeOf(*this);
        const int header = juce::roundToInt(t.controlHeight * 14.0f);
        const int bodyHeight = height - juce::roundToInt(t.controlHeight + t.spacing);
        int w = 0;
        for (const auto& s : sections)
            w += s->preferredWidth(bodyHeight) + juce::roundToInt(t.spacing);
        return juce::jmax(header, w);
    }

    void resized() override
    {
        const auto& t = themeOf(*this);
        auto r = getLocalBounds();
        auto top = r.removeFromTop(juce::roundToInt(t.controlHeight));
        const int button = juce::roundToInt(t.controlHeight * 3.2f);
        pages.setBounds(top.removeFromRight(button));
        top.removeFromRight(juce::roundToInt(t.spacing));
        configure.setBounds(top.removeFromRight(button));
        top.removeFromRight(juce::roundToInt(t.spacing));
        window.setBounds(top.removeFromRight(button));
        top.removeFromRight(juce::roundToInt(t.spacing));
        status.setBounds(top);
        r.removeFromTop(juce::roundToInt(t.spacing));
        for (auto& s : sections)
        {
            s->setBounds(r.removeFromLeft(s->preferredWidth(r.getHeight())));
            r.removeFromLeft(juce::roundToInt(t.spacing));
        }
    }

    void flushUpdates() override { refresh(); }

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override { refresh(); }
    void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property) override
    {
        if (tree.hasType(ids::deviceState) && tree.getParent() == node && property == pluginTypes::configured)
            refresh();
    }

    std::int64_t id() const { return LiveSet::idOf(node); }

    void refresh()
    {
        auto* host = context.plugins;
        const auto state = host != nullptr ? host->statusOf(node) : hosting::PluginStatus{hosting::PluginStatus::State::missing, "No plugin host"};
        juce::String text;
        switch (state.state)
        {
        case hosting::PluginStatus::State::loading: text = "Loading..."; break;
        case hosting::PluginStatus::State::missing:
        case hosting::PluginStatus::State::failed: text = state.message; break;
        case hosting::PluginStatus::State::ready:
        case hosting::PluginStatus::State::none: break;
        }
        if (const auto plugin = hosting::PluginHost::pluginOf(node); plugin && text.isEmpty())
            text = plugin->description.pluginFormatName + (plugin->description.manufacturerName.isNotEmpty()
                                                               ? juce::String(juce::CharPointer_UTF8(" \xc2\xb7 ")) + plugin->description.manufacturerName
                                                               : juce::String());
        status.setText(text, juce::dontSendNotification);
        status.setTooltip(text);

        const bool ready = state.state == hosting::PluginStatus::State::ready;
        auto* live = ready ? host->liveDevice(id()) : nullptr;
        window.setEnabled(live != nullptr && live->plugin().hasEditor());
        window.setButtonText(host != nullptr && host->windows().isOpen(id()) ? "Hide" : "Show");
        configure.setEnabled(ready);
        configure.setToggleState(host != nullptr && host->isConfiguring(id()), juce::dontSendNotification);
        configure.setButtonText("Configure");
        pages.setButtonText("Pages");
        pages.setEnabled(ready);

        // The knobs, if what they describe changed.
        const auto* spec = host != nullptr ? host->describe(node) : nullptr;
        if (spec == shown)
            return;
        shown = spec;
        for (auto& s : sections)
            removeChildComponent(s.get());
        sections.clear();
        if (spec != nullptr)
            for (const auto& group : spec->parameterGroups())
            {
                auto section = std::make_unique<ParameterSection>(group.name);
                for (const int index : group.parameters)
                    section->add(makeParameterControl(context.set(), node, spec->parameter(index).spec()));
                addAndMakeVisible(*section);
                sections.push_back(std::move(section));
            }
        resized();
        widthChanged();
    }

    void toggleWindow()
    {
        auto* host = context.plugins;
        if (host == nullptr)
            return;
        if (host->windows().isOpen(id()))
            host->windows().close(id());
        else if (auto* live = host->liveDevice(id()))
            host->windows().open(*live, node);
        refresh();
    }

    void showPagesMenu()
    {
        auto* host = context.plugins;
        if (host == nullptr)
            return;
        const auto onPages = juce::StringArray::fromTokens(node.getChildWithName(ids::deviceState)[pluginTypes::configured].toString(), " ", {});
        juce::PopupMenu parameters;
        std::map<juce::String, juce::PopupMenu> byGroup;
        juce::StringArray groupOrder;
        for (const auto& p : host->allParameters(node))
        {
            if (!groupOrder.contains(p.group))
                groupOrder.add(p.group);
            juce::PopupMenu::Item item(p.name);
            item.setTicked(onPages.contains(p.id));
            item.setAction([safe = juce::Component::SafePointer<PluginBody>(this), id = p.id, on = onPages.contains(p.id)] {
                if (safe == nullptr || safe->context.plugins == nullptr)
                    return;
                if (on)
                    safe->context.plugins->removeFromPages(safe->node, id);
                else
                    safe->context.plugins->addToPages(safe->node, id);
            });
            byGroup[p.group].addItem(std::move(item));
        }
        for (const auto& g : groupOrder)
        {
            if (g.isEmpty())
                for (juce::PopupMenu::MenuItemIterator it(byGroup[g]); it.next();)
                    parameters.addItem(it.getItem());
            else
                parameters.addSubMenu(g, byGroup[g]);
        }

        juce::PopupMenu m;
        m.addSubMenu("Parameters on its pages", parameters);
        m.addItem("Save these pages as the plugin's default", [safe = juce::Component::SafePointer<PluginBody>(this)] {
            if (safe != nullptr && safe->context.plugins != nullptr)
                safe->context.plugins->saveDefaultPages(safe->node);
        });
        if (host->takesVstPresets(node))
        {
            m.addSeparator();
            m.addItem("Load VST3 Preset...", [this] { chooseVstPreset(true); });
            m.addItem("Save VST3 Preset...", [this] { chooseVstPreset(false); });
        }
        m.addSeparator();
        const bool mpe = host->mpeOf(node);
        const auto* live = host->liveDevice(id());
        const int mtsChannel = live != nullptr ? live->mtsChannel() : -1;
        // Tracks with different tunings: MTS-ESP gives this one's a channel, which MPE would need.
        const auto mpeName = mtsChannel >= 0 ? "MPE (off while its tuning is on MTS-ESP channel " + juce::String(mtsChannel + 1) + ")"
                                             : juce::String("MPE");
        m.addItem(mpeName, true, mpe, [safe = juce::Component::SafePointer<PluginBody>(this), mpe] {
            if (safe != nullptr && safe->context.plugins != nullptr)
                safe->context.plugins->setMpe(safe->node, !mpe);
        });
        const bool onTop = host->windows().alwaysOnTop();
        m.addItem("Plugin windows always on top", true, onTop, [safe = juce::Component::SafePointer<PluginBody>(this), onTop] {
            if (safe != nullptr && safe->context.plugins != nullptr)
            {
                safe->context.plugins->windows().setAlwaysOnTop(!onTop);
            }
        });
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&pages));
    }

    void chooseVstPreset(bool load)
    {
        const auto name = hosting::PluginHost::pluginOf(node) ? hosting::PluginHost::pluginOf(node)->description.name : juce::String("Plugin");
        chooser = std::make_unique<juce::FileChooser>(load ? "Load VST3 Preset" : "Save VST3 Preset",
                                                      juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                                          .getChildFile("VST3 Presets")
                                                          .getChildFile(name),
                                                      "*.vstpreset");
        const int chooserFlags = load ? juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                      : juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting;
        chooser->launchAsync(chooserFlags, [safe = juce::Component::SafePointer<PluginBody>(this), load](const juce::FileChooser& fc) {
            const auto file = fc.getResult();
            if (safe == nullptr || safe->context.plugins == nullptr || file == juce::File())
                return;
            const bool ok = load ? safe->context.plugins->loadVstPreset(safe->node, file)
                                 : safe->context.plugins->saveVstPreset(safe->node, file.withFileExtension("vstpreset"));
            if (!ok && safe->context.confirm)
                safe->context.confirm(load ? "Couldn't load the preset" : "Couldn't save the preset",
                                      load ? file.getFileName() + " isn't a preset this plugin reads." : "The plugin gave no preset to save.",
                                      {}, nullptr);
        });
    }

    std::unique_ptr<juce::FileChooser> chooser;
    juce::Label status;
    juce::TextButton window, configure, pages;
    const StudioDevice* shown = nullptr;
    std::vector<std::unique_ptr<ParameterSection>> sections;
};
} // namespace

std::unique_ptr<DeviceBody> plugin(DeviceContext& context, const juce::ValueTree& device)
{
    return std::make_unique<PluginBody>(context, device);
}

} // namespace lattice::ui::panels
