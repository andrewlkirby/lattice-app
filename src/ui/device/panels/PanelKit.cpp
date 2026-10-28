#include "ui/device/panels/PanelKit.h"

#include "devices/ChainEditing.h"
#include "devices/dsp/Biquad.h"
#include "ui/theme/LatticeLookAndFeel.h"
#include "ui/widgets/Icons.h"

#include <cmath>

namespace lattice::ui::panels
{

std::unique_ptr<ValueBinding> bind(DeviceContext& context, const juce::ValueTree& device, const juce::String& parameterId)
{
    const auto* proto = context.specs.forType(device[ids::deviceType].toString());
    const auto* p = proto != nullptr ? proto->findParameter(parameterId) : nullptr;
    jassert(p != nullptr);
    if (p == nullptr)
        return nullptr;
    return ValueBinding::deviceParameter(context.set(), device, p->spec());
}

TuningPtr tuningOf(DeviceContext& context, const juce::ValueTree& device, int* noteOffset)
{
    const auto owner = LiveSet::chainOwnerOf(device);
    if (noteOffset != nullptr)
        *noteOffset = owner.getProperty(ids::noteOffset, 0);
    auto t = context.tunings.findById(owner[ids::tuningId].toString());
    return t != nullptr ? t : context.tunings.defaultTuning();
}

// --- LiveReadout ----------------------------------------------------------------------------------------

LiveReadout::LiveReadout(EngineStateFeed& f, std::int64_t deviceId) : feed(f), id(deviceId)
{
    feed.watchDevice(id);
    feed.addListener(this, EngineStateFeed::devicesAspect);
}

LiveReadout::~LiveReadout()
{
    feed.removeListener(this);
    feed.unwatchDevice(id);
}

void LiveReadout::engineStateChanged(std::uint32_t)
{
    if (onChange)
        onChange();
}

// --- SidechainPicker --------------------------------------------------------------------------------------

SidechainPicker::SidechainPicker(DeviceContext& c, juce::ValueTree device) : context(c), node(std::move(device))
{
    setTitle("Sidechain from");
    setDescription("The track whose sound drives this device (its level, not this track's).");
    preFx.setClickingTogglesState(false);
    preFx.setWantsKeyboardFocus(false);
    preFx.setTooltip("Takes the source before its audio effects (right after its instrument).");
    preFx.onClick = [this] {
        context.undo().beginNewTransaction("Sidechain tap");
        chains::setSidechainSource(context.set(), node, source(),
                                   !static_cast<bool>(node.getChildWithName(ids::deviceState).getProperty(ids::sidechainPreFx, false)));
    };
    addAndMakeVisible(preFx);
    node.addListener(this);
}

SidechainPicker::~SidechainPicker()
{
    node.removeListener(this);
}

std::int64_t SidechainPicker::source() const
{
    return static_cast<juce::int64>(node.getChildWithName(ids::deviceState).getProperty(ids::sidechainTrack, 0));
}

juce::String SidechainPicker::whyNot(int trackIndex) const
{
    auto& set = context.set();
    if (trackIndex < 0 || trackIndex >= set.numTracks())
        return "no such track";
    const auto owner = LiveSet::chainOwnerOf(node);
    if (owner == set.track(trackIndex))
        return "its own track";
    if (set.isGroupTrack(trackIndex))
        return "a group (groups can't be sources)";
    const auto offered = chains::sidechainSources(set, node);
    if (std::find(offered.begin(), offered.end(), set.trackId(trackIndex)) == offered.end())
        return "it already hears this track, so it would feed back";
    return {};
}

juce::PopupMenu SidechainPicker::sourceMenu()
{
    juce::PopupMenu m;
    juce::Component::SafePointer<SidechainPicker> safe(this);
    const auto choose = [safe](std::int64_t id) {
        return [safe, id] {
            if (safe == nullptr)
                return;
            auto& c = safe->context;
            c.undo().beginNewTransaction("Sidechain source");
            chains::setSidechainSource(c.set(), safe->node, id,
                                       safe->node.getChildWithName(ids::deviceState).getProperty(ids::sidechainPreFx, false));
        };
    };
    const auto current = source();
    m.addItem("No source", true, current == 0, choose(0));
    m.addSeparator();
    auto& set = context.set();
    for (int t = 0; t < set.numTracks(); ++t)
    {
        const auto reason = whyNot(t);
        const auto id = set.trackId(t);
        const auto name = set.track(t)[ids::name].toString();
        m.addItem(reason.isEmpty() ? name : name + juce::String::fromUTF8(" \xe2\x80\x94 ") + reason, reason.isEmpty() || id == current,
                  id == current, choose(id));
    }
    return m;
}

int SidechainPicker::preferredWidth() const
{
    return juce::roundToInt(themeOf(*this).controlHeight * 6.0f);
}

juce::Rectangle<int> SidechainPicker::sourceBox() const
{
    const auto& t = themeOf(*this);
    auto r = getLocalBounds();
    r.removeFromTop(juce::roundToInt(t.fontSmall + 4.0f));
    return r.removeFromTop(juce::roundToInt(t.controlHeight));
}

void SidechainPicker::resized()
{
    const auto& t = themeOf(*this);
    auto r = getLocalBounds();
    r.removeFromTop(sourceBox().getBottom() + juce::roundToInt(t.spacing));
    preFx.setBounds(r.removeFromTop(juce::roundToInt(t.controlHeight)));
}

void SidechainPicker::paint(juce::Graphics& g)
{
    const auto& t = themeOf(*this);
    g.setFont(t.font(t.fontSmall));
    g.setColour(t.textDim);
    g.drawText("Sidechain from", getLocalBounds().removeFromTop(juce::roundToInt(t.fontSmall + 4.0f)), juce::Justification::centredLeft, true);
    const auto box = sourceBox().toFloat();
    g.setColour(t.surfaceSunken);
    g.fillRoundedRectangle(box, t.radius);
    g.setColour(t.border);
    g.drawRoundedRectangle(box.reduced(0.5f), t.radius, t.stroke);
    const auto id = source();
    const int index = id != 0 ? context.set().trackIndexForId(id) : -1;
    g.setColour(index >= 0 ? t.text : t.textDim);
    g.setFont(t.font(t.fontNormal));
    auto text = box.reduced(t.padding * 0.5f, 0.0f);
    drawIcon(g, Icon::menu, text.removeFromRight(box.getHeight() * 0.6f), t.textDim, t);
    g.setColour(index >= 0 ? t.text : t.textDim);
    g.drawText(index >= 0 ? context.set().track(index)[ids::name].toString() : juce::String("No source"), text, juce::Justification::centredLeft, true);
    preFx.setToggleState(node.getChildWithName(ids::deviceState).getProperty(ids::sidechainPreFx, false), juce::dontSendNotification);
}

void SidechainPicker::mouseDown(const juce::MouseEvent& e)
{
    if (sourceBox().contains(e.getPosition()))
        sourceMenu().showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withTargetScreenArea(localAreaToGlobal(sourceBox())));
}

// --- EnvelopeEditor ---------------------------------------------------------------------------------------

EnvelopeEditor::EnvelopeEditor(DeviceContext& context, const juce::ValueTree& device, const juce::StringArray& parameterIds)
{
    setTitle("Envelope");
    setDescription("Drag the corners: the peak sets the attack, the next the decay and sustain, the last the release.");
    for (int i = 0; i < 4 && i < parameterIds.size(); ++i)
        if (parameterIds[i].isNotEmpty())
        {
            values[static_cast<std::size_t>(i)] = bind(context, device, parameterIds[i]);
            if (values[static_cast<std::size_t>(i)] != nullptr)
                values[static_cast<std::size_t>(i)]->onChange = [this] { invalidate(); };
        }
}

float EnvelopeEditor::widthFor(float ms, float maxMs)
{
    return std::log1p(juce::jmax(0.0f, ms) / 5.0f) / std::log1p(maxMs / 5.0f);
}

float EnvelopeEditor::msFor(float share, float maxMs)
{
    return 5.0f * std::expm1(juce::jlimit(0.0f, 1.0f, share) * std::log1p(maxMs / 5.0f));
}

ValueBinding* EnvelopeEditor::bindingFor(Handle h) const
{
    switch (h)
    {
    case Handle::attack:
        return values[0].get();
    case Handle::decay:
        return values[1].get();
    case Handle::sustain:
        return values[2].get();
    case Handle::release:
        return values[3].get();
    case Handle::none:
        break;
    }
    return nullptr;
}

juce::Point<float> EnvelopeEditor::handlePosition(Handle h) const
{
    const auto area = contentArea().reduced(themeOf(*this).padding * 0.5f);
    const float seg = area.getWidth() / 4.0f;
    const auto share = [](const ValueBinding* b) { return b != nullptr ? widthFor(b->value(), b->spec().maxValue) : 0.0f; };
    const float sustain = values[2] != nullptr ? values[2]->value() : (values[1] != nullptr ? 0.0f : 1.0f);
    const float xa = area.getX() + seg * share(values[0].get());
    const float xd = xa + seg * share(values[1].get());
    const float ys = area.getBottom() - sustain * area.getHeight();
    switch (h)
    {
    case Handle::attack:
        return {xa, area.getY()};
    case Handle::decay:
        return {xd, ys};
    case Handle::sustain:
        return {xd + seg, ys};
    case Handle::release:
        return {xd + seg + seg * share(values[3].get()), area.getBottom()};
    case Handle::none:
        break;
    }
    return {};
}

EnvelopeEditor::Handle EnvelopeEditor::handleAt(juce::Point<float> p) const
{
    const float reach = themeOf(*this).controlHeight * 0.6f;
    Handle best = Handle::none;
    float bestDistance = reach;
    for (const auto h : {Handle::attack, Handle::decay, Handle::sustain, Handle::release})
        if (bindingFor(h) != nullptr)
            if (const float d = handlePosition(h).getDistanceFrom(p); d < bestDistance)
                best = h, bestDistance = d;
    return best;
}

void EnvelopeEditor::drawContent(juce::Graphics& g, juce::Rectangle<float>)
{
    const auto& t = themeOf(*this);
    const auto area = contentArea().reduced(t.padding * 0.5f);
    const auto start = juce::Point<float>(area.getX(), area.getBottom());
    const auto a = handlePosition(Handle::attack), d = handlePosition(Handle::decay), s = handlePosition(Handle::sustain),
               r = handlePosition(Handle::release);
    juce::Path p;
    p.startNewSubPath(start);
    p.lineTo(a);
    // Decay and release fall exponentially, as the envelopes do.
    const auto fall = [&p](juce::Point<float> from, juce::Point<float> to) {
        for (int i = 1; i <= 16; ++i)
        {
            const float x = static_cast<float>(i) / 16.0f;
            const float y = (1.0f - std::exp(-4.0f * x)) / (1.0f - std::exp(-4.0f));
            p.lineTo(from.x + (to.x - from.x) * x, from.y + (to.y - from.y) * y);
        }
    };
    fall(a, d);
    p.lineTo(s);
    fall(s, r);
    juce::Path fill(p);
    fill.lineTo(r.x, area.getBottom());
    fill.closeSubPath();
    g.setColour(t.accent.withAlpha(0.18f));
    g.fillPath(fill);
    g.setColour(t.accent);
    g.strokePath(p, juce::PathStrokeType(juce::jmax(1.5f, t.stroke * 1.5f)));
    // The sustain stretch is held for as long as the key: dashed.
    g.setColour(t.textDim);
    const float dashes[] = {3.0f, 3.0f};
    g.drawDashedLine({d.x, area.getY(), d.x, area.getBottom()}, dashes, 2, t.stroke);
    g.drawDashedLine({s.x, area.getY(), s.x, area.getBottom()}, dashes, 2, t.stroke);
    const float dot = t.controlHeight * 0.3f;
    for (const auto h : {Handle::attack, Handle::decay, Handle::sustain, Handle::release})
        if (bindingFor(h) != nullptr)
        {
            const auto c = handlePosition(h);
            g.setColour(t.surfaceRaised);
            g.fillEllipse(juce::Rectangle<float>(dot, dot).withCentre(c));
            g.setColour(t.text);
            g.drawEllipse(juce::Rectangle<float>(dot, dot).withCentre(c), t.stroke);
        }
}

void EnvelopeEditor::mouseDown(const juce::MouseEvent& e)
{
    dragging = handleAt(e.position);
    // The decay corner also moves the sustain level: both in one gesture.
    if (auto* b = bindingFor(dragging))
        b->beginGesture();
    if (dragging == Handle::decay && values[2] != nullptr)
        values[2]->beginGesture();
}

void EnvelopeEditor::mouseDrag(const juce::MouseEvent& e)
{
    auto* b = bindingFor(dragging);
    if (b == nullptr)
        return;
    const auto area = contentArea().reduced(themeOf(*this).padding * 0.5f);
    const float seg = area.getWidth() / 4.0f;
    const auto origin = [&](Handle h) { return h == Handle::attack ? area.getX() : handlePosition(h == Handle::decay ? Handle::attack : Handle::sustain).x; };
    const float level = juce::jlimit(0.0f, 1.0f, (area.getBottom() - e.position.y) / juce::jmax(1.0f, area.getHeight()));
    if (dragging == Handle::sustain)
    {
        b->setValue(level);
        return;
    }
    b->setValue(msFor((e.position.x - origin(dragging)) / juce::jmax(1.0f, seg), b->spec().maxValue));
    if (dragging == Handle::decay && values[2] != nullptr)
        values[2]->setValue(level);
}

void EnvelopeEditor::mouseUp(const juce::MouseEvent&)
{
    if (auto* b = bindingFor(dragging))
        b->endGesture();
    if (values[2] != nullptr)
        values[2]->endGesture();
    dragging = Handle::none;
}

void EnvelopeEditor::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (auto* b = bindingFor(handleAt(e.position)))
        b->resetToDefault();
}

// --- Filter curves -----------------------------------------------------------------------------------------

float filterResponseDb(int type, double cutoff, float resonance, double hz)
{
    using dsp::BiquadType;
    const double q = 0.707 * std::pow(30.0, static_cast<double>(juce::jlimit(0.0f, 1.0f, resonance)));
    const auto mag = [&](BiquadType bt, double qq) { return std::abs(dsp::BiquadCoefficients::analogResponse(bt, hz, cutoff, qq, 0.0)); };
    double m = 1.0;
    switch (type)
    {
    case 0: m = mag(BiquadType::LowPass, q); break;
    case 1: m = mag(BiquadType::LowPass, q) * mag(BiquadType::LowPass, 0.54); break;
    case 2: m = mag(BiquadType::HighPass, q); break;
    case 3: m = mag(BiquadType::HighPass, q) * mag(BiquadType::HighPass, 0.54); break;
    case 4: m = mag(BiquadType::BandPass, q); break;
    case 5: m = mag(BiquadType::Notch, juce::jmax(0.5, q * 0.5)); break;
    default: m = mag(BiquadType::LowPass, q) * mag(BiquadType::LowPass, 0.54); break; // the ladder: four poles
    }
    return static_cast<float>(juce::Decibels::gainToDecibels(m, -120.0));
}

void drawFilterResponse(juce::Graphics& g, juce::Rectangle<float> area, const Theme& t, int type, double cutoff, float resonance)
{
    constexpr float low = -36.0f, high = 24.0f;
    drawFrequencyGrid(g, area, t, low, high, 12.0f);
    const auto path = curvePath(area, juce::jmax(64, juce::roundToInt(area.getWidth() / 2.0f)), [&](float x) {
        const double hz = FrequencyAxis::hzAt(area.getX() + x * area.getWidth(), area);
        return (filterResponseDb(type, cutoff, resonance, hz) - low) / (high - low);
    });
    juce::Path fill(path);
    fill.lineTo(area.getBottomRight());
    fill.lineTo(area.getBottomLeft());
    fill.closeSubPath();
    g.setColour(t.accent.withAlpha(0.15f));
    g.fillPath(fill);
    g.setColour(t.accent);
    g.strokePath(path, juce::PathStrokeType(juce::jmax(1.5f, t.stroke * 1.5f)));
}

// --- FunctionDisplay -----------------------------------------------------------------------------------------

FunctionDisplay::FunctionDisplay(DeviceContext& context, const juce::ValueTree& device,
                                 std::function<void(juce::Graphics&, juce::Rectangle<float>, DeviceMirror&, const Theme&)> draw)
    : values(context.specs, device), drawFn(std::move(draw))
{
    values.onChange = [this] { invalidate(); };
}

void FunctionDisplay::drawContent(juce::Graphics& g, juce::Rectangle<float> area)
{
    drawFn(g, area, values, themeOf(*this));
}

} // namespace lattice::ui::panels
