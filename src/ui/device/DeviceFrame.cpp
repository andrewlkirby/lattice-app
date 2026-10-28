#include "ui/device/DeviceFrame.h"

#include "devices/racks/Rack.h"
#include "devices/racks/RackEditing.h"
#include "ui/device/DevicePanels.h"
#include "ui/device/DeviceView.h"
#include "ui/session/SessionView.h"
#include "ui/theme/LatticeLookAndFeel.h"

namespace lattice::ui
{
namespace
{
constexpr auto samplerType = "lattice.sampler";

/** The frame a component belongs to (the nearest one above it, or itself). */
const DeviceFrame* frameOf(const juce::Component* c)
{
    for (; c != nullptr; c = c->getParentComponent())
        if (const auto* f = dynamic_cast<const DeviceFrame*>(c))
            return f;
    return nullptr;
}

/** "Instrument Rack › Chain 1 › " for a device inside rack chains (outermost first). */
juce::String rackPath(const DeviceContext& context, const juce::ValueTree& device)
{
    juce::String path;
    for (auto n = device.getParent(); n.isValid(); n = n.getParent())
        if (n.hasType(ids::rackChain))
        {
            const auto rack = n.getParent().getParent();
            path = context.nameOf(rack) + juce::String::fromUTF8(" \xe2\x80\xba ") + n[ids::name].toString() +
                   juce::String::fromUTF8(" \xe2\x80\xba ") + path;
        }
    return path;
}

/** The racks a device sits in, innermost first (for "Map to macro"). */
std::vector<juce::ValueTree> enclosingRacks(const juce::ValueTree& device)
{
    std::vector<juce::ValueTree> out;
    for (auto n = device.getParent(); n.isValid(); n = n.getParent())
        if (n.hasType(ids::device) && RackDevice::isRackType(n[ids::deviceType].toString()))
            out.push_back(n);
    return out;
}
} // namespace

DeviceFrame::DeviceFrame(DeviceContext& c, juce::ValueTree device) : context(c), node(std::move(device))
{
    setTitle(context.nameOf(node));
    const auto type = node[ids::deviceType].toString();
    const bool rack = RackDevice::isRackType(type);

    power.setClickingTogglesState(false);
    power.onClick = [this] {
        context.undo().beginNewTransaction("Device on/off");
        context.set().setDeviceEnabled(node, !static_cast<bool>(node.getProperty(ids::enabled, true)));
    };
    fold.onClick = [this] { LiveSet::setDeviceFolded(node, !isFolded()); };
    hotSwap.onClick = [this] {
        if (context.view != nullptr)
            context.view->setHotSwapTarget(context.view->hotSwapTarget() == node ? juce::ValueTree() : node);
    };
    save.onClick = [this] {
        askForText(*this, "Save Preset", context.nameOf(node), [this](const juce::String& name) {
            juce::File written;
            const auto result = context.presets.saveUserPreset(node, name, written);
            if (result.failed())
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Could not save the preset",
                                                       result.getErrorMessage());
            else if (context.presetsChanged)
                context.presetsChanged();
        });
    };
    allParams.onClick = [this] { LiveSet::setDeviceShowsAll(node, !showsAllParameters()); };
    menu.onClick = [this] { showMenu(); };
    for (auto* b : {&power, &fold, &hotSwap, &save, &allParams, &menu})
        addAndMakeVisible(*b);
    allParams.setVisible(!rack && hasOwnPanel(type));

    rebuildBody();
    node.addListener(this);
}

DeviceFrame::~DeviceFrame()
{
    cancelPendingUpdate();
    node.removeListener(this);
    if (content != nullptr)
        content->removeMouseListener(this);
}

bool DeviceFrame::isFolded() const
{
    return node.getProperty(ids::folded, false);
}

bool DeviceFrame::showsAllParameters() const
{
    return node.getProperty(ids::showAll, false);
}

int DeviceFrame::titleHeight() const
{
    return juce::roundToInt(themeOf(*this).controlHeight);
}

juce::Rectangle<int> DeviceFrame::titleBar() const
{
    return isFolded() ? getLocalBounds() : getLocalBounds().removeFromTop(titleHeight());
}

void DeviceFrame::rebuildBody()
{
    const bool all = showsAllParameters();
    if (content == nullptr || all != builtAll)
    {
        if (content != nullptr)
            content->removeMouseListener(this);
        content = createDeviceBody(context, node, all);
        builtAll = all;
        addAndMakeVisible(*content);
        // Clicks anywhere in it select the device (and map mode takes them; see mouseDown).
        content->addMouseListener(this, true);
        if (isMapping())
            content->setInterceptsMouseClicks(false, false);

        // Every control inside a rack can be mapped to the rack's macros from its menu.
        const auto racksAround = enclosingRacks(node);
        if (!racksAround.empty())
            for (auto* c : boundControlsIn(*content))
            {
                if (frameOf(c) != this)
                    continue; // a nested device's (it adds its own)
                auto* bound = dynamic_cast<BoundControl*>(c);
                if (auto* labelled = dynamic_cast<LabelledBox*>(c))
                    bound = &labelled->box();
                if (bound == nullptr || !bound->binding().spec().modulatable)
                    continue;
                bound->extendMenu = [this, racksAround, id = bound->binding().spec().id](juce::PopupMenu& m) {
                    const auto deviceId = LiveSet::idOf(node);
                    for (const auto& r : racksAround)
                    {
                        m.addSeparator();
                        juce::PopupMenu sub;
                        const int count = racks::macroCount(r);
                        for (int macro = 0; macro < count; ++macro)
                            sub.addItem(racks::macroName(r, macro), [this, r, macro, id] {
                                context.undo().beginNewTransaction("Map to macro");
                                racks::addMapping(context.set(), r, macro, node, id);
                            });
                        if (count == 0)
                            sub.addItem("(the rack has no macros)", false, false, nullptr);
                        m.addSubMenu("Map to " + context.nameOf(r) + " Macro", sub);
                        for (const auto& mapping : racks::mappingsOf(r))
                            if (static_cast<juce::int64>(mapping[ids::targetDevice]) == deviceId && mapping[ids::parameter].toString() == id)
                                m.addItem("Unmap from " + racks::macroName(r, mapping[ids::macro]), [this, mapping] {
                                    context.undo().beginNewTransaction("Remove mapping");
                                    racks::removeMapping(context.set(), mapping);
                                });
                    }
                };
            }
    }
    content->setVisible(!isFolded());
    fold.setIcon(isFolded() ? Icon::foldClosed : Icon::foldOpen);
    allParams.setToggleState(all, juce::dontSendNotification);
    power.setToggleState(node.getProperty(ids::enabled, true), juce::dontSendNotification);
    if (context.view != nullptr)
        context.view->requestLayout();
    resized();
    repaint();
}

void DeviceFrame::flushUpdates()
{
    handleUpdateNowIfNeeded();
    if (content != nullptr)
        content->flushUpdates();
}

int DeviceFrame::preferredWidth(int height) const
{
    const auto& t = themeOf(*this);
    if (isFolded())
        return titleHeight() + juce::roundToInt(t.stroke * 2.0f);
    const int pad = juce::roundToInt(t.padding);
    const float nameWidth = juce::GlyphArrangement::getStringWidth(juce::Font(t.font(t.fontNormal)).boldened(), context.nameOf(node));
    const int buttons = 6 * titleHeight();
    const int titleWidth = juce::roundToInt(nameWidth) + buttons + pad * 2;
    const int bodyHeight = height - titleHeight() - pad * 2;
    const int bodyWidth = content != nullptr ? content->preferredWidth(bodyHeight) : 0;
    return juce::jmax(titleWidth, bodyWidth + pad * 2);
}

void DeviceFrame::resized()
{
    const auto& t = themeOf(*this);
    const int h = titleHeight();
    if (isFolded())
    {
        auto strip = getLocalBounds().reduced(juce::roundToInt(t.stroke), 0);
        power.setBounds(strip.removeFromTop(h));
        fold.setBounds(strip.removeFromTop(h));
        for (auto* b : {&hotSwap, &save, &allParams, &menu})
            b->setBounds({});
        return;
    }
    auto bar = getLocalBounds().removeFromTop(h);
    power.setBounds(bar.removeFromLeft(h));
    fold.setBounds(bar.removeFromLeft(h));
    menu.setBounds(bar.removeFromRight(h));
    if (allParams.isVisible())
        allParams.setBounds(bar.removeFromRight(h));
    save.setBounds(bar.removeFromRight(h));
    hotSwap.setBounds(bar.removeFromRight(h));
    if (content != nullptr)
        content->setBounds(getLocalBounds().withTrimmedTop(h).reduced(juce::roundToInt(t.padding)));
}

void DeviceFrame::paint(juce::Graphics& g)
{
    const auto& t = themeOf(*this);
    const bool on = node.getProperty(ids::enabled, true);
    const auto bounds = getLocalBounds().toFloat();
    g.setColour(t.surface);
    g.fillRoundedRectangle(bounds, t.radius);
    const auto bar = titleBar();
    g.setColour(t.surfaceRaised);
    g.fillRoundedRectangle(bar.toFloat(), t.radius);
    if (!isFolded())
        g.fillRect(bar.withTrimmedTop(bar.getHeight() / 2)); // square bottom corners under the bar

    g.setColour(on ? t.text : t.textDisabled);
    g.setFont(juce::Font(t.font(t.fontNormal)).boldened());
    const auto name = context.nameOf(node);
    if (isFolded())
    {
        // The name runs down the strip, below the switches.
        const auto area = bar.withTrimmedTop(titleHeight() * 2 + juce::roundToInt(t.padding)).toFloat();
        juce::GlyphArrangement glyphs;
        glyphs.addFittedText(juce::Font(t.font(t.fontNormal)).boldened(), name, 0.0f, 0.0f, area.getHeight(), area.getWidth(),
                             juce::Justification::centredLeft, 1);
        glyphs.draw(g, juce::AffineTransform::rotation(juce::MathConstants<float>::halfPi)
                           .translated(area.getRight(), area.getY()));
    }
    else
    {
        auto text = bar.withTrimmedLeft(titleHeight() * 2 + juce::roundToInt(t.spacing)).withRight(hotSwap.getX());
        g.drawText(name, text, juce::Justification::centredLeft, true);
    }
    g.setColour(t.border);
    g.drawRoundedRectangle(bounds.reduced(0.5f), t.radius, t.stroke);
}

void DeviceFrame::paintOverChildren(juce::Graphics& g)
{
    const auto& t = themeOf(*this);
    const bool on = node.getProperty(ids::enabled, true);
    if (!on && !isFolded() && content != nullptr)
    {
        // An off device's body is dimmed (its controls still work, as in Live).
        g.setColour(t.background.withAlpha(0.35f));
        g.fillRect(content->getBounds());
    }
    if (context.view != nullptr)
    {
        // Map mode: the parameters that can be mapped light up; those mapped to the macro, more.
        const auto& mapRack = context.view->mapModeRack();
        if (isMapping() && content->isVisible())
        {
            const auto deviceId = LiveSet::idOf(node);
            const auto mapped = racks::mappingsOf(mapRack, context.view->mapModeMacro());
            for (auto* c : boundControlsIn(*content))
            {
                const auto* binding = bindingOf(*c);
                if (frameOf(c) != this || binding == nullptr || !binding->spec().modulatable || !c->isShowing())
                    continue;
                const auto r = getLocalArea(c, c->getLocalBounds()).toFloat();
                const bool isMapped = std::any_of(mapped.begin(), mapped.end(), [&](const juce::ValueTree& m) {
                    return static_cast<juce::int64>(m[ids::targetDevice]) == deviceId && m[ids::parameter].toString() == binding->spec().id;
                });
                g.setColour(t.accent.withAlpha(isMapped ? 0.45f : 0.18f));
                g.fillRoundedRectangle(r, t.radius);
                g.setColour(t.accent);
                g.drawRoundedRectangle(r.reduced(0.5f), t.radius, t.stroke);
            }
        }
        const bool isSelected = context.view->selectedDevice() == node;
        const bool swapping = context.view->hotSwapTarget() == node;
        if (isSelected || swapping)
        {
            g.setColour(swapping ? t.accent : t.selection);
            g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), t.radius, juce::jmax(2.0f, t.stroke * 2.0f));
        }
    }
    hotSwap.setToggleState(context.view != nullptr && context.view->hotSwapTarget() == node, juce::dontSendNotification);
}

bool DeviceFrame::isMapping() const
{
    if (context.view == nullptr || content == nullptr)
        return false;
    const auto& mapRack = context.view->mapModeRack();
    return mapRack.isValid() && node.isAChildOf(mapRack) && !RackDevice::isRackType(node[ids::deviceType].toString());
}

void DeviceFrame::mapModeChanged()
{
    // While mapping, the body's controls take no clicks: they come here, and pick what to map.
    if (content != nullptr)
    {
        const bool mapping = isMapping();
        content->setInterceptsMouseClicks(!mapping, !mapping);
    }
    repaint();
}

void DeviceFrame::mouseDown(const juce::MouseEvent& e)
{
    if (frameOf(e.eventComponent) != this || context.view == nullptr)
        return;
    // Map mode: a click on a parameter maps it instead of changing it.
    if (isMapping() && e.eventComponent == this)
        for (auto* c : boundControlsIn(*content))
            if (frameOf(c) == this && c->isShowing() && getLocalArea(c, c->getLocalBounds()).contains(e.getPosition()))
                if (const auto* b = bindingOf(*c); b != nullptr && b->spec().modulatable)
                {
                    context.view->mapTo(node, b->spec().id);
                    return;
                }
    context.view->select(node);
    if (e.eventComponent == this && e.mods.isPopupMenu() && titleBar().contains(e.getPosition()))
        showMenu();
}

void DeviceFrame::mouseDrag(const juce::MouseEvent& e)
{
    // Dragging the title bar moves the device.
    if (e.eventComponent != this || !titleBar().contains(e.getMouseDownPosition()) || e.getDistanceFromDragStart() < 6)
        return;
    if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this);
        container != nullptr && !container->isDragAndDropActive())
    {
        const auto image = createComponentSnapshot(titleBar());
        container->startDragging(drag::move(LiveSet::idOf(node)), this, juce::ScaledImage(image));
    }
}

void DeviceFrame::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (e.eventComponent == this && titleBar().contains(e.getPosition()) && !e.mods.isPopupMenu())
        LiveSet::setDeviceFolded(node, !isFolded());
}

juce::PopupMenu DeviceFrame::buildMenu()
{
    juce::PopupMenu m;
    const auto type = node[ids::deviceType].toString();
    m.addItem("Rename...", [this] {
        askForText(*this, "Rename Device", context.nameOf(node), [this](const juce::String& name) {
            context.undo().beginNewTransaction("Rename device");
            context.set().setDeviceName(node, name);
        });
    });
    m.addItem("Save as Preset...", [this] { save.onClick(); });
    const auto list = context.presets.presetsFor(type);
    if (!list.empty())
    {
        juce::PopupMenu presetMenu;
        for (const auto& info : list)
        {
            const auto& all = context.presets.all();
            int index = -1;
            for (std::size_t i = 0; i < all.size(); ++i)
                if (all[i].name == info.name && all[i].deviceType == info.deviceType && all[i].factory == info.factory)
                    index = static_cast<int>(i);
            presetMenu.addItem(info.name + (info.factory ? "" : "  (user)"), [this, index] {
                if (context.view != nullptr)
                {
                    context.view->setHotSwapTarget(node);
                    context.view->insertHere(drag::preset(index));
                }
            });
        }
        m.addSubMenu("Replace with Preset", presetMenu);
    }
    m.addSeparator();
    m.addItem(isFolded() ? "Unfold" : "Fold", [this] { LiveSet::setDeviceFolded(node, !isFolded()); });
    if (allParams.isVisible())
        m.addItem("Show All Parameters", true, showsAllParameters(), [this] { LiveSet::setDeviceShowsAll(node, !showsAllParameters()); });
    m.addItem("Hot-Swap", true, context.view != nullptr && context.view->hotSwapTarget() == node, [this] { hotSwap.onClick(); });
    m.addSeparator();
    for (const auto& [id, name] : {std::pair{"edit.copy", "Copy"}, std::pair{"edit.duplicate", "Duplicate"},
                                   std::pair{"edit.delete", "Delete"}})
        m.addItem(name, [this, command = juce::String(id)] {
            if (context.view != nullptr)
            {
                context.view->select(node);
                context.view->edit(command);
            }
        });
    return m;
}

void DeviceFrame::showMenu()
{
    buildMenu().showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&menu));
}

juce::Component* DeviceFrame::revealParameter(const juce::String& parameterId)
{
    if (context.specs.forType(node[ids::deviceType].toString())->findParameter(parameterId) == nullptr)
        return nullptr;
    if (isFolded())
        LiveSet::setDeviceFolded(node, false);
    flushUpdates();
    if (content != nullptr && !content->reveal(parameterId))
    {
        LiveSet::setDeviceShowsAll(node, true);
        flushUpdates();
    }
    return content != nullptr ? content->controlFor(parameterId) : nullptr;
}

void DeviceFrame::describe(ControlRegistry::Entry& entry) const
{
    entry.owner = rackPath(context, node) + context.nameOf(node) + juce::String::fromUTF8(" \xc2\xb7 ") + context.ownerNameOf(node);
    if (entry.binding == nullptr || context.view == nullptr)
        return;
    juce::Component::SafePointer<DeviceView> view(context.view);
    entry.reveal = [view, device = node, id = entry.binding->spec().id] {
        if (view != nullptr)
            view->revealParameter(device, id);
    };
}

bool DeviceFrame::isSampler() const
{
    return node[ids::deviceType].toString() == samplerType;
}

void DeviceFrame::loadSample(const juce::File& file)
{
    if (isSampler() && isSampleFile(file))
        panels::loadSampleInto(context, node, file);
}

bool DeviceFrame::isInterestedInDragSource(const SourceDetails& details)
{
    return isSampler() && drag::is(details.description, "sample");
}

void DeviceFrame::itemDropped(const SourceDetails& details)
{
    loadSample(juce::File(drag::payload(details.description)));
}

bool DeviceFrame::isInterestedInFileDrag(const juce::StringArray& files)
{
    return isSampler() && files.size() == 1 && isSampleFile(juce::File(files[0]));
}

void DeviceFrame::filesDropped(const juce::StringArray& files, int, int)
{
    loadSample(juce::File(files[0]));
}

void DeviceFrame::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property)
{
    if (tree != node)
        return;
    if (property == ids::folded || property == ids::showAll)
        triggerAsyncUpdate();
    else if (property == ids::enabled)
    {
        power.setToggleState(node.getProperty(ids::enabled, true), juce::dontSendNotification);
        repaint();
    }
    else if (property == ids::name)
    {
        setTitle(context.nameOf(node));
        if (context.view != nullptr)
            context.view->requestLayout();
        repaint();
    }
}

} // namespace lattice::ui
