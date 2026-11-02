// The audio effects' panels (docs/05-m4-ui.md, 4d, "Custom panels"): EQ Eight, Compressor, Reverb,
// Delay, Auto Filter, Saturator, Chorus, Utility and Limiter.

#include "devices/effects/Compressor.h"
#include "devices/effects/EqEight.h"
#include "devices/effects/Reverb.h"
#include "devices/effects/Saturator.h"
#include "ui/device/DevicePanels.h"
#include "ui/device/panels/PanelKit.h"
#include "ui/theme/LatticeLookAndFeel.h"

#include <cmath>
#include <deque>

namespace lattice::ui::panels
{
namespace
{
// --- Shared pieces -------------------------------------------------------------------------------------

/** Shows some controls only while a switch is in one position (Sync: synced rates; otherwise
    times), as Live's panels do. */
class Switched
{
public:
    Switched(DeviceContext& context, const juce::ValueTree& device, const juce::String& switchId, std::function<void()> changed)
        : binding(bind(context, device, switchId)), onChanged(std::move(changed))
    {
        binding->onChange = [this] { apply(); };
    }

    /** Shown while the switch's value satisfies `when`. */
    void add(juce::Component* control, std::function<bool(float)> when)
    {
        if (control != nullptr)
            controls.push_back({control, std::move(when)});
        apply();
    }
    void apply()
    {
        for (auto& [c, when] : controls)
            c->setVisible(when(binding->value()));
        if (onChanged)
            onChanged();
    }

private:
    std::unique_ptr<ValueBinding> binding;
    std::function<void()> onChanged;
    std::vector<std::pair<juce::Component*, std::function<bool(float)>>> controls;
};

juce::Component* controlIn(ParameterSection* section, const juce::String& parameterId)
{
    for (auto* c : section->controls())
        if (parameterIdOf(*c) == parameterId)
            return c;
    return nullptr;
}

/** A vertical gain-reduction meter (0 at the top, falling down to -24 dB), from the feed. */
class ReductionMeter final : public juce::Component
{
public:
    ReductionMeter(EngineStateFeed& feed, std::int64_t deviceId) : live(feed, deviceId)
    {
        setTitle("Gain Reduction");
        setDescription("How far the device is turning the sound down now.");
        live.onChange = [this] { repaint(); };
    }

    float reductionDb() const
    {
        const auto* r = live.readout();
        return r != nullptr ? r->gainReductionDb : 0.0f;
    }

    void paint(juce::Graphics& g) override
    {
        const auto& t = themeOf(*this);
        auto r = getLocalBounds().toFloat();
        auto label = r.removeFromBottom(t.fontSmall + 4.0f);
        r.removeFromTop(t.fontSmall + 4.0f);
        const auto well = r.withSizeKeepingCentre(juce::jmin(r.getWidth(), t.controlHeight * 0.6f), r.getHeight());
        g.setColour(t.surfaceSunken);
        g.fillRoundedRectangle(well, t.radius);
        const float db = juce::jlimit(-24.0f, 0.0f, reductionDb());
        g.setColour(t.accent);
        g.fillRect(well.withHeight(well.getHeight() * (-db / 24.0f)).reduced(1.0f, 0.0f));
        g.setColour(t.border);
        g.drawRoundedRectangle(well.reduced(0.5f), t.radius, t.stroke);
        g.setFont(t.font(t.fontSmall));
        g.setColour(t.textDim);
        g.drawText("GR", getLocalBounds().removeFromTop(juce::roundToInt(t.fontSmall + 4.0f)), juce::Justification::centred, false);
        g.setColour(t.text);
        g.drawText(db < -0.05f ? juce::String(db, 1) : juce::String("0.0"), label, juce::Justification::centred, false);
    }

private:
    LiveReadout live;
};

/** A row panel that's a list of sections, by title and parameters, with an optional display first. */
class SectionsPanel : public PanelRow
{
public:
    using PanelRow::PanelRow;

    /** Lays out again after something showed or hid (a Switched). */
    void reflow()
    {
        resized();
        for (auto* c : getChildren())
            c->resized();
        widthChanged();
    }
};

// --- EQ Eight ------------------------------------------------------------------------------------------

constexpr float eqRange = 15.0f; // the curve's dB range up and down

bool shapeHasGain(int shape)
{
    const auto s = static_cast<EqEightDevice::Shape>(shape);
    return s == EqEightDevice::Shape::LowShelf || s == EqEightDevice::Shape::Bell || s == EqEightDevice::Shape::HighShelf;
}

/** The EQ curve with a handle per band and the spectrum behind it. Drag a handle to move the
    band's frequency (and gain); the wheel over it changes its Q; double-click turns it on or off. */
class EqDisplay final : public DeviceDisplay
{
public:
    EqDisplay(DeviceContext& context, const juce::ValueTree& device, std::function<void(int)> onSelected)
        : mirror(context.specs, device), live(context.feed, LiveSet::idOf(device)), onSelect(std::move(onSelected))
    {
        setTitle("EQ curve");
        setDescription("Drag a band's handle to move it; the wheel changes its Q; double-click turns it on or off.");
        double rate = 48000.0;
        if (const auto* d = context.session.snapshots().deviceFor(LiveSet::idOf(device)); d != nullptr && d->isPrepared())
            rate = d->preparedSampleRate();
        mirror.prepare(rate);
        mirror.onChange = [this] { invalidate(); };
        live.onChange = [this] { repaint(); };
        for (int b = 0; b < EqEightDevice::numBands; ++b)
        {
            const juce::String n(b + 1);
            auto& band = bands[static_cast<std::size_t>(b)];
            band.on = bind(context, device, "band" + n + ".on");
            band.shape = bind(context, device, "band" + n + ".shape");
            band.frequency = bind(context, device, "band" + n + ".frequency");
            band.gain = bind(context, device, "band" + n + ".gain");
            band.q = bind(context, device, "band" + n + ".q");
        }
    }

    int selectedBand() const noexcept { return selected; }
    void selectBand(int b)
    {
        selected = juce::jlimit(0, EqEightDevice::numBands - 1, b);
        repaint();
        if (onSelect)
            onSelect(selected);
    }

    juce::Point<float> handlePosition(int b) const
    {
        const auto area = contentArea();
        const auto& band = bands[static_cast<std::size_t>(b)];
        const float gain = shapeHasGain(juce::roundToInt(band.shape->value())) ? band.gain->value() : 0.0f;
        return {FrequencyAxis::xFor(band.frequency->value(), area), yForDb(gain, area, -eqRange, eqRange)};
    }

    int bandAt(juce::Point<float> p) const
    {
        const float reach = themeOf(*this).controlHeight * 0.6f;
        int best = -1;
        float bestDistance = reach;
        for (int b = 0; b < EqEightDevice::numBands; ++b)
            if (const float d = handlePosition(b).getDistanceFrom(p); d < bestDistance)
                best = b, bestDistance = d;
        return best;
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        dragging = bandAt(e.position);
        if (dragging < 0)
            return;
        selectBand(dragging);
        auto& band = bands[static_cast<std::size_t>(dragging)];
        band.frequency->beginGesture();
        band.gain->beginGesture();
        if (band.on->value() < 0.5f)
            band.on->setValue(1.0f); // touching a band that's off turns it on, in the same step
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (dragging < 0)
            return;
        const auto area = contentArea();
        auto& band = bands[static_cast<std::size_t>(dragging)];
        band.frequency->setValue(static_cast<float>(FrequencyAxis::hzAt(e.position.x, area)));
        if (shapeHasGain(juce::roundToInt(band.shape->value())))
            band.gain->setValue(-eqRange + (area.getBottom() - e.position.y) / juce::jmax(1.0f, area.getHeight()) * 2.0f * eqRange);
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (dragging >= 0)
        {
            bands[static_cast<std::size_t>(dragging)].frequency->endGesture();
            bands[static_cast<std::size_t>(dragging)].gain->endGesture();
        }
        dragging = -1;
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        if (const int b = bandAt(e.position); b >= 0)
        {
            auto& on = *bands[static_cast<std::size_t>(b)].on;
            on.setValue(on.value() > 0.5f ? 0.0f : 1.0f);
        }
    }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        const int b = bandAt(e.position);
        if (b < 0)
            return;
        auto& q = *bands[static_cast<std::size_t>(b)].q;
        q.setNormalised(q.normalised() + (wheel.deltaY > 0.0f ? 0.02f : -0.02f));
    }

protected:
    void drawContent(juce::Graphics& g, juce::Rectangle<float> area) override
    {
        const auto& t = themeOf(*this);
        drawFrequencyGrid(g, area, t, -eqRange, eqRange, 6.0f);
        const auto* eq = mirror.as<EqEightDevice>();
        if (eq == nullptr)
            return;
        bool midSide = false;
        for (const auto& band : bands)
            midSide = midSide || (band.on->value() > 0.5f && juce::roundToInt(mirror.value(band.target())) != 0);
        const auto curve = [&](EqEightDevice::Target target) {
            return curvePath(area, juce::jmax(96, juce::roundToInt(area.getWidth() / 2.0f)), [&](float x) {
                const double hz = FrequencyAxis::hzAt(area.getX() + x * area.getWidth(), area);
                const float db = static_cast<float>(juce::Decibels::gainToDecibels(std::abs(eq->response(hz, target)), -100.0));
                return (db + eqRange) / (2.0f * eqRange);
            });
        };
        const float width = juce::jmax(1.5f, t.stroke * 1.5f);
        if (midSide)
        {
            g.setColour(t.textDim);
            const float dashes[] = {4.0f, 3.0f};
            juce::Path dashed;
            juce::PathStrokeType(width).createDashedStroke(dashed, curve(EqEightDevice::Target::Side), dashes, 2);
            g.fillPath(dashed);
        }
        g.setColour(t.accent);
        g.strokePath(curve(EqEightDevice::Target::Mid), juce::PathStrokeType(width));
    }

    void drawOverlay(juce::Graphics& g, juce::Rectangle<float> area) override
    {
        const auto& t = themeOf(*this);
        // The spectrum after the EQ (from the engine), faint behind the handles: -90..0 dB.
        if (const auto* r = live.readout(); r != nullptr && !r->spectrumDb.empty())
        {
            const int bins = static_cast<int>(r->spectrumDb.size());
            const double binHz = r->sampleRate / (2.0 * (bins - 1));
            juce::Path p;
            p.startNewSubPath(area.getBottomLeft());
            const int columns = juce::jmax(2, juce::roundToInt(area.getWidth() / 2.0f));
            for (int c = 0; c <= columns; ++c)
            {
                const float x = area.getX() + area.getWidth() * static_cast<float>(c) / columns;
                const double lo = FrequencyAxis::hzAt(x - area.getWidth() / columns * 0.5f, area);
                const double hi = FrequencyAxis::hzAt(x + area.getWidth() / columns * 0.5f, area);
                const int b0 = juce::jlimit(1, bins - 1, static_cast<int>(lo / binHz));
                const int b1 = juce::jlimit(b0, bins - 1, static_cast<int>(std::ceil(hi / binHz)));
                float db = -120.0f;
                for (int b = b0; b <= b1; ++b)
                    db = juce::jmax(db, r->spectrumDb[static_cast<std::size_t>(b)]);
                p.lineTo(x, area.getBottom() - juce::jlimit(0.0f, 1.0f, (db + 90.0f) / 90.0f) * area.getHeight());
            }
            p.lineTo(area.getBottomRight());
            p.closeSubPath();
            g.setColour(t.textDim.withAlpha(0.25f));
            g.fillPath(p);
        }
        // The handles, numbered; the selected one filled.
        const float size = t.controlHeight * 0.75f;
        g.setFont(juce::Font(t.font(t.fontSmall)).boldened());
        for (int b = 0; b < EqEightDevice::numBands; ++b)
        {
            const auto& band = bands[static_cast<std::size_t>(b)];
            const bool on = band.on->value() > 0.5f;
            const auto r = juce::Rectangle<float>(size, size).withCentre(handlePosition(b));
            if (b == selected)
            {
                g.setColour(on ? t.accent : t.textDisabled);
                g.fillEllipse(r);
                g.setColour(on ? t.accentText : t.background);
            }
            else
            {
                g.setColour(t.surfaceRaised.withAlpha(0.85f));
                g.fillEllipse(r);
                g.setColour(on ? t.accent : t.textDisabled);
                g.drawEllipse(r, t.stroke * 1.5f);
            }
            g.drawText(juce::String(b + 1), r, juce::Justification::centred, false);
        }
    }

private:
    struct Band
    {
        std::unique_ptr<ValueBinding> on, shape, frequency, gain, q;
        juce::String target() const { return frequency->spec().id.upToFirstOccurrenceOf(".", false, false) + ".target"; }
    };

    DeviceMirror mirror;
    LiveReadout live;
    std::function<void(int)> onSelect;
    std::array<Band, EqEightDevice::numBands> bands;
    int selected = 1;
    int dragging = -1;
};

/** The selected band's controls, and a row of on switches for all eight. */
class EqBandControls final : public juce::Component
{
public:
    EqBandControls(DeviceContext& c, juce::ValueTree d) : context(c), device(std::move(d))
    {
        for (int b = 0; b < EqEightDevice::numBands; ++b)
        {
            auto toggle = std::make_unique<Toggle>(bind(context, device, "band" + juce::String(b + 1) + ".on"), Toggle::Role::accent,
                                                   juce::String(b + 1));
            addAndMakeVisible(*toggle);
            switches.push_back(std::move(toggle));
        }
        show(1);
    }

    void show(int band)
    {
        shown = band;
        const juce::String n(band + 1);
        section = std::make_unique<ParameterSection>("Band " + n);
        section->addParameters(context.set(), device, *context.specs.forType(device[ids::deviceType].toString()),
                               {"band" + n + ".shape", "band" + n + ".frequency", "band" + n + ".gain", "band" + n + ".q",
                                "band" + n + ".target"});
        addAndMakeVisible(*section);
        resized();
    }
    int shownBand() const noexcept { return shown; }

    int preferredWidth(int height) const
    {
        const auto& t = themeOf(*this);
        const int strip = juce::roundToInt(t.controlHeight * 8.0f);
        return juce::jmax(strip, section->preferredWidth(height - juce::roundToInt(t.controlHeight + t.spacing)));
    }

    void resized() override
    {
        const auto& t = themeOf(*this);
        auto r = getLocalBounds();
        auto strip = r.removeFromTop(juce::roundToInt(t.controlHeight));
        const int w = juce::jmax(1, strip.getWidth() / EqEightDevice::numBands);
        for (auto& s : switches)
            s->setBounds(strip.removeFromLeft(w).reduced(1, 0));
        r.removeFromTop(juce::roundToInt(t.spacing));
        if (section != nullptr)
            section->setBounds(r);
    }

private:
    DeviceContext& context;
    juce::ValueTree device;
    std::vector<std::unique_ptr<Toggle>> switches;
    std::unique_ptr<ParameterSection> section;
    int shown = 1;
};

class EqPanel final : public SectionsPanel
{
public:
    EqPanel(DeviceContext& c, const juce::ValueTree& d) : SectionsPanel(c, d)
    {
        auto controls = std::make_unique<EqBandControls>(context, node);
        auto* bandControls = controls.get();
        auto* display = addDisplay(std::make_unique<EqDisplay>(context, node, [this, bandControls](int band) {
                                       bandControls->show(band);
                                       widthChanged();
                                   }),
                                   2.4f);
        addPart(std::move(controls), [bandControls](int h) { return bandControls->preferredWidth(h); });
        addSection("Output", {"eq.output", "eq.scale"});
        curve = display;
    }

    bool reveal(const juce::String& id) override
    {
        // A band's parameter shows when its band is selected.
        if (id.startsWith("band") && !id.endsWith(".on"))
        {
            curve->selectBand(id.fromFirstOccurrenceOf("band", false, false).getIntValue() - 1);
            reflow();
        }
        return controlFor(id) != nullptr;
    }

private:
    EqDisplay* curve = nullptr;
};

// --- Compressor ----------------------------------------------------------------------------------------

class CompressorPanel final : public SectionsPanel
{
public:
    CompressorPanel(DeviceContext& c, const juce::ValueTree& d) : SectionsPanel(c, d)
    {
        addDisplay(std::make_unique<FunctionDisplay>(
                       context, node,
                       [](juce::Graphics& g, juce::Rectangle<float> area, DeviceMirror& m, const Theme& t) {
                           // Output level against input level, -60..0 dB each way.
                           const auto* comp = m.as<CompressorDevice>();
                           constexpr float low = -60.0f;
                           g.setColour(t.border.withAlpha(0.6f));
                           for (float db = low; db <= 0.0f; db += 12.0f)
                           {
                               const float x = area.getX() + (db - low) / -low * area.getWidth();
                               const float y = area.getBottom() - (db - low) / -low * area.getHeight();
                               g.drawVerticalLine(juce::roundToInt(x), area.getY(), area.getBottom());
                               g.drawHorizontalLine(juce::roundToInt(y), area.getX(), area.getRight());
                           }
                           g.setColour(t.textDim);
                           const float dashes[] = {3.0f, 3.0f};
                           g.drawDashedLine({area.getBottomLeft(), area.getTopRight()}, dashes, 2, t.stroke);
                           const float threshold = m.value("comp.threshold");
                           const float tx = area.getX() + (threshold - low) / -low * area.getWidth();
                           g.setColour(t.accent.withAlpha(0.35f));
                           g.drawVerticalLine(juce::roundToInt(tx), area.getY(), area.getBottom());
                           if (comp == nullptr)
                               return;
                           g.setColour(t.accent);
                           g.strokePath(curvePath(area, 120, [&](float x) { return (comp->staticCurve(low + x * -low) - low) / -low; }),
                                        juce::PathStrokeType(juce::jmax(1.5f, t.stroke * 1.5f)));
                           g.setFont(t.font(t.fontSmall));
                           g.setColour(t.textDim);
                           g.drawText("in", area.reduced(3.0f), juce::Justification::bottomRight, false);
                           g.drawText("out", area.reduced(3.0f), juce::Justification::topLeft, false);
                       }),
                   1.0f);
        auto meter = std::make_unique<ReductionMeter>(context.feed, LiveSet::idOf(node));
        addPart(std::move(meter), [this](int) { return juce::roundToInt(themeOf(*this).controlHeight * 1.6f); });
        addSection("Compressor", {"comp.threshold", "comp.ratio", "comp.attack", "comp.release", "comp.knee", "comp.makeup",
                                  "comp.mix", "comp.detection"});
        auto picker = std::make_unique<SidechainPicker>(context, node);
        auto* p = picker.get();
        addPart(std::move(picker), [p](int) { return p->preferredWidth(); });
        addSection("Sidechain", {"comp.sidechain", "comp.sidechainGain", "comp.eq", "comp.eqType", "comp.eqFrequency", "comp.eqQ",
                                 "comp.eqGain"});
    }
};

// --- Reverb -------------------------------------------------------------------------------------------

class ReverbPanel final : public SectionsPanel
{
public:
    ReverbPanel(DeviceContext& c, const juce::ValueTree& d) : SectionsPanel(c, d)
    {
        auto* reverbSection = addSection("Reverb", {"reverb.size", "reverb.decay", "reverb.predelay", "reverb.predelaySync",
                                                    "reverb.predelayBeats"});
        addDisplay(std::make_unique<FunctionDisplay>(
                       context, node,
                       [](juce::Graphics& g, juce::Rectangle<float> area, DeviceMirror& m, const Theme& t) {
                           // Decay time against frequency: the low, mid and high bands with their
                           // crossovers, on log scales (0.1 s to 120 s).
                           const auto* reverb = m.as<ReverbDevice>();
                           drawFrequencyGrid(g, area, t, 0.0f, 0.0f, 0.0f);
                           if (reverb == nullptr)
                               return;
                           const double lowX = m.value("reverb.lowCrossover"), highX = m.value("reverb.highCrossover");
                           const double decays[3] = {reverb->bandDecay(0), reverb->bandDecay(1), reverb->bandDecay(2)};
                           const auto yFor = [&](double seconds) {
                               return static_cast<float>(std::log(juce::jlimit(0.1, 120.0, seconds) / 0.1) / std::log(1200.0));
                           };
                           const auto at = [&](double hz) {
                               // Half an octave of crossfade around each crossover, in log time.
                               const auto blend = [](double a, double b, double w) { return std::exp(std::log(a) + (std::log(b) - std::log(a)) * w); };
                               const auto w = [](double f, double x) { return juce::jlimit(0.0, 1.0, 0.5 + std::log2(f / x)); };
                               return blend(blend(decays[0], decays[1], w(hz, lowX)), decays[2], w(hz, highX));
                           };
                           auto p = curvePath(area, 120, [&](float x) { return yFor(at(FrequencyAxis::hzAt(area.getX() + x * area.getWidth(), area))); });
                           juce::Path fill(p);
                           fill.lineTo(area.getBottomRight());
                           fill.lineTo(area.getBottomLeft());
                           fill.closeSubPath();
                           g.setColour(t.accent.withAlpha(0.15f));
                           g.fillPath(fill);
                           g.setColour(t.accent);
                           g.strokePath(p, juce::PathStrokeType(juce::jmax(1.5f, t.stroke * 1.5f)));
                           g.setColour(t.text);
                           g.setFont(t.font(t.fontSmall));
                           const auto label = [&](double hz, double seconds) {
                               const float x = FrequencyAxis::xFor(hz, area);
                               const float y = area.getBottom() - yFor(seconds) * area.getHeight();
                               g.drawText(juce::String(seconds, seconds < 10.0 ? 2 : 1) + " s",
                                          juce::Rectangle<float>(x - 30.0f, y - t.fontSmall - 4.0f, 60.0f, t.fontSmall + 2.0f),
                                          juce::Justification::centred, false);
                           };
                           label(std::sqrt(20.0 * lowX), decays[0]);
                           label(std::sqrt(lowX * highX), decays[1]);
                           label(std::sqrt(highX * 20000.0), decays[2]);
                       }),
                   2.0f);
        addSection("Decay", {"reverb.lowDecay", "reverb.highDecay", "reverb.lowCrossover", "reverb.highCrossover"});
        addSection("Character", {"reverb.diffusion", "reverb.shape", "reverb.early", "reverb.late", "reverb.modulation"});
        addSection("Output", {"reverb.width", "reverb.lowCut", "reverb.highCut", "reverb.freeze", "reverb.quality", "reverb.mix"});
        sync = std::make_unique<Switched>(context, node, "reverb.predelaySync", [this] { reflow(); });
        sync->add(controlIn(reverbSection, "reverb.predelay"), [](float v) { return v < 0.5f; });
        sync->add(controlIn(reverbSection, "reverb.predelayBeats"), [](float v) { return v >= 0.5f; });
    }

private:
    std::unique_ptr<Switched> sync;
};

// --- Delay --------------------------------------------------------------------------------------------

class DelayPanel final : public SectionsPanel
{
public:
    DelayPanel(DeviceContext& c, const juce::ValueTree& d) : SectionsPanel(c, d)
    {
        auto* times = addSection("Delay", {"delay.mode", "delay.sync", "delay.link", "delay.syncLeft", "delay.syncRight",
                                           "delay.msLeft", "delay.msRight"});
        addDisplay(std::make_unique<FunctionDisplay>(context, node,
                                                     [](juce::Graphics& g, juce::Rectangle<float> area, DeviceMirror& m, const Theme& t) {
                                                         // The feedback path's band: a low cut and a high cut.
                                                         constexpr float low = -36.0f, high = 12.0f;
                                                         drawFrequencyGrid(g, area, t, low, high, 12.0f);
                                                         const double lc = m.value("delay.lowCut"), hc = m.value("delay.highCut");
                                                         const auto p = curvePath(area, 120, [&](float x) {
                                                             const double hz = FrequencyAxis::hzAt(area.getX() + x * area.getWidth(), area);
                                                             const float db = filterResponseDb(2, lc, 0.0f, hz) + filterResponseDb(0, hc, 0.0f, hz);
                                                             return (db - low) / (high - low);
                                                         });
                                                         g.setColour(t.accent);
                                                         g.strokePath(p, juce::PathStrokeType(juce::jmax(1.5f, t.stroke * 1.5f)));
                                                     }),
                   1.6f);
        addSection("Feedback", {"delay.feedback", "delay.lowCut", "delay.highCut"});
        addSection("Modulation", {"delay.modRate", "delay.modDepth", "delay.freeze", "delay.mix"});
        sync = std::make_unique<Switched>(context, node, "delay.sync", [this] { reflow(); });
        for (const auto* id : {"delay.syncLeft", "delay.syncRight"})
            sync->add(controlIn(times, id), [](float v) { return v >= 0.5f; });
        for (const auto* id : {"delay.msLeft", "delay.msRight"})
            sync->add(controlIn(times, id), [](float v) { return v < 0.5f; });
    }

private:
    std::unique_ptr<Switched> sync;
};

// --- Auto Filter ---------------------------------------------------------------------------------------

class AutoFilterPanel final : public SectionsPanel
{
public:
    AutoFilterPanel(DeviceContext& c, const juce::ValueTree& d) : SectionsPanel(c, d)
    {
        addDisplay(std::make_unique<FunctionDisplay>(context, node,
                                                     [](juce::Graphics& g, juce::Rectangle<float> area, DeviceMirror& m, const Theme& t) {
                                                         drawFilterResponse(g, area, t, juce::roundToInt(m.value("af.type")),
                                                                            m.value("af.cutoff"), m.value("af.resonance"));
                                                     }),
                   1.8f);
        addSection("Filter", {"af.type", "af.cutoff", "af.resonance", "af.drive"});
        auto* lfo = addSection("LFO", {"af.lfoAmount", "af.lfoShape", "af.lfoRate", "af.lfoSync", "af.lfoSyncRate", "af.lfoPhase"});
        addSection("Envelope", {"af.envAmount", "af.envAttack", "af.envRelease", "af.mix"});
        sync = std::make_unique<Switched>(context, node, "af.lfoSync", [this] { reflow(); });
        sync->add(controlIn(lfo, "af.lfoRate"), [](float v) { return v < 0.5f; });
        sync->add(controlIn(lfo, "af.lfoSyncRate"), [](float v) { return v >= 0.5f; });
    }

private:
    std::unique_ptr<Switched> sync;
};

// --- Saturator ----------------------------------------------------------------------------------------

class SaturatorPanel final : public SectionsPanel
{
public:
    SaturatorPanel(DeviceContext& c, const juce::ValueTree& d) : SectionsPanel(c, d)
    {
        addDisplay(std::make_unique<FunctionDisplay>(
                       context, node,
                       [](juce::Graphics& g, juce::Rectangle<float> area, DeviceMirror& m, const Theme& t) {
                           // The curve: output against input, -1..1 each way, with the drive and output gain.
                           g.setColour(t.border.withAlpha(0.6f));
                           g.drawVerticalLine(juce::roundToInt(area.getCentreX()), area.getY(), area.getBottom());
                           g.drawHorizontalLine(juce::roundToInt(area.getCentreY()), area.getX(), area.getRight());
                           g.setColour(t.textDim);
                           const float dashes[] = {3.0f, 3.0f};
                           g.drawDashedLine({area.getBottomLeft(), area.getTopRight()}, dashes, 2, t.stroke);
                           const auto curve = static_cast<SaturatorDevice::Curve>(juce::roundToInt(m.value("sat.curve")));
                           const float drive = juce::Decibels::decibelsToGain(m.value("sat.drive"));
                           const float out = juce::Decibels::decibelsToGain(m.value("sat.output"));
                           const float mix = m.value("sat.mix");
                           g.setColour(t.accent);
                           g.strokePath(curvePath(area, 160,
                                                  [&](float x) {
                                                      const float in = x * 2.0f - 1.0f;
                                                      const float wet = SaturatorDevice::shape(curve, in * drive) * out;
                                                      return (in + (wet - in) * mix + 1.0f) * 0.5f;
                                                  }),
                                        juce::PathStrokeType(juce::jmax(1.5f, t.stroke * 1.5f)));
                       }),
                   1.0f);
        addSection("Saturator", {"sat.curve", "sat.drive", "sat.output", "sat.oversampling", "sat.mix"});
    }
};

// --- Limiter ---------------------------------------------------------------------------------------------

/** The gain reduction over the last few seconds, newest at the right. */
class ReductionHistory final : public DeviceDisplay
{
public:
    ReductionHistory(EngineStateFeed& f, std::int64_t deviceId) : feed(f), live(f, deviceId)
    {
        setTitle("Gain reduction history");
        live.onChange = [this] { push(); };
    }

    void push()
    {
        const auto* r = live.readout();
        history.push_back(r != nullptr ? r->gainReductionDb : 0.0f);
        // About four seconds at the display's rate.
        const auto keep = static_cast<std::size_t>(juce::jlimit(60.0, 1000.0, 4.0 / juce::jmax(1.0e-3, feed.frameSeconds())));
        while (history.size() > keep)
            history.pop_front();
        repaint();
    }

protected:
    void drawContent(juce::Graphics& g, juce::Rectangle<float> area) override
    {
        const auto& t = themeOf(*this);
        g.setColour(t.border.withAlpha(0.6f));
        for (float db = 0.0f; db >= -24.0f; db -= 6.0f)
            g.drawHorizontalLine(juce::roundToInt(area.getY() + -db / 24.0f * area.getHeight()), area.getX(), area.getRight());
        g.setFont(t.font(t.fontSmall));
        g.setColour(t.textDim);
        g.drawText("-6", area.withHeight(t.fontSmall * 2.0f).translated(2.0f, area.getHeight() * 0.25f - t.fontSmall), juce::Justification::left, false);
    }
    void drawOverlay(juce::Graphics& g, juce::Rectangle<float> area) override
    {
        if (history.empty())
            return;
        const auto& t = themeOf(*this);
        juce::Path p;
        p.startNewSubPath(area.getTopRight());
        const float step = area.getWidth() / static_cast<float>(juce::jmax<std::size_t>(1, history.size() - 1));
        for (std::size_t i = history.size(); i-- > 0;)
        {
            const float x = area.getRight() - step * static_cast<float>(history.size() - 1 - i);
            p.lineTo(x, area.getY() + juce::jlimit(0.0f, 1.0f, -history[i] / 24.0f) * area.getHeight());
        }
        p.lineTo(area.getRight() - step * static_cast<float>(history.size() - 1), area.getY());
        p.closeSubPath();
        g.setColour(t.accent.withAlpha(0.5f));
        g.fillPath(p);
    }

private:
    EngineStateFeed& feed;
    LiveReadout live;
    std::deque<float> history;
};

class LimiterPanel final : public SectionsPanel
{
public:
    LimiterPanel(DeviceContext& c, const juce::ValueTree& d) : SectionsPanel(c, d)
    {
        addDisplay(std::make_unique<ReductionHistory>(context.feed, LiveSet::idOf(node)), 2.0f);
        addPart(std::make_unique<ReductionMeter>(context.feed, LiveSet::idOf(node)),
                [this](int) { return juce::roundToInt(themeOf(*this).controlHeight * 1.6f); });
        addSection("Limiter", {"limiter.gain", "limiter.ceiling", "limiter.release"});
    }
};

/** A panel of sections by the device's own parameter groups (Chorus, Utility), arranged by hand. */
class GroupedPanel final : public SectionsPanel
{
public:
    GroupedPanel(DeviceContext& c, const juce::ValueTree& d, std::vector<std::pair<juce::String, juce::StringArray>> sections)
        : SectionsPanel(c, d)
    {
        for (const auto& [title, ids] : sections)
            addSection(title, ids);
    }
};
} // namespace

std::unique_ptr<DeviceBody> eqEight(DeviceContext& c, const juce::ValueTree& d)
{
    return std::make_unique<EqPanel>(c, d);
}

std::unique_ptr<DeviceBody> compressor(DeviceContext& c, const juce::ValueTree& d)
{
    return std::make_unique<CompressorPanel>(c, d);
}

std::unique_ptr<DeviceBody> reverb(DeviceContext& c, const juce::ValueTree& d)
{
    return std::make_unique<ReverbPanel>(c, d);
}

std::unique_ptr<DeviceBody> delay(DeviceContext& c, const juce::ValueTree& d)
{
    return std::make_unique<DelayPanel>(c, d);
}

std::unique_ptr<DeviceBody> autoFilter(DeviceContext& c, const juce::ValueTree& d)
{
    return std::make_unique<AutoFilterPanel>(c, d);
}

std::unique_ptr<DeviceBody> saturator(DeviceContext& c, const juce::ValueTree& d)
{
    return std::make_unique<SaturatorPanel>(c, d);
}

std::unique_ptr<DeviceBody> chorus(DeviceContext& c, const juce::ValueTree& d)
{
    return std::make_unique<GroupedPanel>(
        c, d, std::vector<std::pair<juce::String, juce::StringArray>>{
                  {"Chorus", {"chorus.voices", "chorus.rate", "chorus.depth", "chorus.delay"}},
                  {"Output", {"chorus.feedback", "chorus.width", "chorus.mix"}}});
}

std::unique_ptr<DeviceBody> utility(DeviceContext& c, const juce::ValueTree& d)
{
    return std::make_unique<GroupedPanel>(
        c, d, std::vector<std::pair<juce::String, juce::StringArray>>{
                  {"Level", {"utility.gain", "utility.balance"}},
                  {"Stereo", {"utility.width", "utility.mono", "utility.invertLeft", "utility.invertRight", "utility.dc"}}});
}

std::unique_ptr<DeviceBody> limiter(DeviceContext& c, const juce::ValueTree& d)
{
    return std::make_unique<LimiterPanel>(c, d);
}

} // namespace lattice::ui::panels
