#include "ui/device/ChainView.h"

#include "devices/ChainEditing.h"
#include "ui/device/DeviceView.h"
#include "ui/theme/LatticeLookAndFeel.h"

namespace lattice::ui
{

ChainView::ChainView(DeviceContext& c, juce::ValueTree devicesNode) : context(c), devices(std::move(devicesNode))
{
    setTitle("Device chain");
    add.onClick = [this] {
        if (context.view == nullptr)
            return;
        context.view->focusChain(devices);
        context.view->insertMenu(devices, -1).showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&add));
    };
    addAndMakeVisible(add);
    devices.addListener(this);
    rebuild();
}

ChainView::~ChainView()
{
    cancelPendingUpdate();
    devices.removeListener(this);
}

void ChainView::flushUpdates()
{
    handleUpdateNowIfNeeded();
    for (auto& f : shown)
        f->flushUpdates();
}

void ChainView::rebuild()
{
    // Keep the frames of devices that are still here, so they keep their state (pages, rack chains).
    std::vector<std::unique_ptr<DeviceFrame>> next;
    for (const auto& d : devices)
    {
        if (!d.hasType(ids::device))
            continue;
        auto it = std::find_if(shown.begin(), shown.end(), [&](const auto& f) { return f != nullptr && f->device() == d; });
        if (it != shown.end())
            next.push_back(std::move(*it));
        else
        {
            auto frame = std::make_unique<DeviceFrame>(context, d);
            addAndMakeVisible(*frame);
            next.push_back(std::move(frame));
        }
    }
    shown = std::move(next);
    if (context.view != nullptr)
        context.view->requestLayout();
    resized();
}

int ChainView::endWidth() const
{
    const auto& t = themeOf(*this);
    return juce::roundToInt(t.controlHeight * (shown.empty() ? 9.0f : 3.0f));
}

int ChainView::preferredWidth(int height) const
{
    const auto& t = themeOf(*this);
    int w = 0;
    for (const auto& f : shown)
        w += f->preferredWidth(height) + juce::roundToInt(t.spacing);
    return w + endWidth();
}

void ChainView::resized()
{
    const auto& t = themeOf(*this);
    int x = 0;
    for (auto& f : shown)
    {
        f->setBounds(x, 0, f->preferredWidth(getHeight()), getHeight());
        x += f->getWidth() + juce::roundToInt(t.spacing);
    }
    const int size = juce::roundToInt(t.controlHeight);
    auto end = juce::Rectangle<int>(x, 0, juce::jmax(0, getWidth() - x), getHeight());
    add.setBounds(end.withSizeKeepingCentre(size, size).withX(end.getX() + juce::roundToInt(t.padding)));
}

void ChainView::paint(juce::Graphics& g)
{
    const auto& t = themeOf(*this);
    const int x = shown.empty() ? 0 : shown.back()->getRight() + juce::roundToInt(t.spacing);
    auto hint = juce::Rectangle<int>(x, 0, juce::jmax(0, getWidth() - x), getHeight()).reduced(juce::roundToInt(t.spacing));
    // The chain new devices go into shows it.
    const bool insertsHere = context.view != nullptr && context.view->insertionChain() == devices;
    g.setColour(insertsHere ? t.accent.withAlpha(0.6f) : t.border);
    g.drawRoundedRectangle(hint.toFloat(), t.radius, t.stroke);
    if (shown.empty())
    {
        g.setColour(t.textDim);
        g.setFont(t.font(t.fontSmall));
        g.drawFittedText("Drop devices, presets or samples here, or click + to insert one.",
                         hint.reduced(juce::roundToInt(t.padding)).withTrimmedLeft(juce::roundToInt(t.controlHeight + t.padding)),
                         juce::Justification::centredLeft, 4);
    }
}

void ChainView::paintOverChildren(juce::Graphics& g)
{
    if (dropIndex < 0)
        return;
    const auto& t = themeOf(*this);
    const int gap = juce::roundToInt(t.spacing);
    const int x = dropIndex < static_cast<int>(shown.size()) ? shown[static_cast<std::size_t>(dropIndex)]->getX() - gap / 2
                  : shown.empty()                           ? gap
                                                            : shown.back()->getRight() + gap / 2;
    g.setColour(t.selection);
    g.fillRect(x - 1, 0, juce::jmax(2, juce::roundToInt(t.stroke * 2.0f)), getHeight());
}

void ChainView::mouseDown(const juce::MouseEvent& e)
{
    if (context.view == nullptr)
        return;
    context.view->focusChain(devices);
    if (e.mods.isPopupMenu())
        context.view->insertMenu(devices, indexAt(e.x)).showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition());
}

int ChainView::indexAt(int x) const
{
    for (std::size_t i = 0; i < shown.size(); ++i)
        if (x < shown[i]->getX() + shown[i]->getWidth() / 2)
            return static_cast<int>(i);
    return static_cast<int>(shown.size());
}

void ChainView::setDropIndex(int index)
{
    if (index != dropIndex)
    {
        dropIndex = index;
        repaint();
    }
}

bool ChainView::isInterestedInDragSource(const SourceDetails& details)
{
    const auto& d = details.description;
    if (drag::is(d, "move"))
    {
        // Not into itself (a rack's own chain).
        const auto device = context.set().deviceById(drag::payload(d).getLargeIntValue());
        return device.isValid() && !devices.isAChildOf(device);
    }
    return drag::is(d, "device") || drag::is(d, "preset") || drag::is(d, "sample");
}

void ChainView::itemDragMove(const SourceDetails& details)
{
    setDropIndex(indexAt(details.localPosition.x));
}

void ChainView::itemDragExit(const SourceDetails&)
{
    setDropIndex(-1);
}

void ChainView::itemDropped(const SourceDetails& details)
{
    const int index = indexAt(details.localPosition.x);
    setDropIndex(-1);
    if (context.view != nullptr)
        context.view->insert(details.description.toString(), devices, index);
}

bool ChainView::isInterestedInFileDrag(const juce::StringArray& files)
{
    return std::any_of(files.begin(), files.end(), [](const juce::String& f) {
        const juce::File file(f);
        return isSampleFile(file) || file.hasFileExtension(presets::fileExtension);
    });
}

void ChainView::fileDragMove(const juce::StringArray&, int x, int)
{
    setDropIndex(indexAt(x));
}

void ChainView::fileDragExit(const juce::StringArray&)
{
    setDropIndex(-1);
}

void ChainView::filesDropped(const juce::StringArray& files, int x, int)
{
    int index = indexAt(x);
    setDropIndex(-1);
    if (context.view == nullptr)
        return;
    for (const auto& f : files)
    {
        const juce::File file(f);
        if (file.hasFileExtension(presets::fileExtension))
        {
            presets::Preset p;
            if (presets::read(file, p).failed())
                continue;
            context.undo().beginNewTransaction("Load preset");
            if (chains::insertCopy(context.set(), devices, p.device, index, context.registry()).isValid())
                ++index;
        }
        else if (isSampleFile(file))
        {
            context.view->insert(drag::sample(file), devices, index);
            break; // one Sampler per chain
        }
    }
}

void ChainView::valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree&)
{
    if (parent == devices)
        triggerAsyncUpdate();
}

void ChainView::valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree&, int)
{
    if (parent == devices)
        triggerAsyncUpdate();
}

void ChainView::valueTreeChildOrderChanged(juce::ValueTree& parent, int, int)
{
    if (parent == devices)
        triggerAsyncUpdate();
}

} // namespace lattice::ui
