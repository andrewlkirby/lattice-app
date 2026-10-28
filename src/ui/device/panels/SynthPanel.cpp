// The Synth's panel (docs/05-m4-ui.md, 4d): three pages, as Live's bigger instruments have, with the
// voice and output controls beside them on every page.
// - Oscillators: each oscillator's wave drawn as it's set (shape, pulse width, wavetable position),
//   with its controls; the sub and noise; unison.
// - Filter and envelopes: the filter's response curve; the amp envelope and envelope 2 drawn as
//   their shapes and edited by dragging their corners.
// - Modulation: the LFOs with their shapes, and the modulation matrix as a grid of eight slots.

#include "devices/dsp/Wavetable.h"
#include "ui/device/DevicePanels.h"
#include "ui/device/panels/PanelKit.h"
#include "ui/theme/LatticeLookAndFeel.h"

#include <cmath>

namespace lattice::ui::panels
{
namespace
{
/** A display above a section of controls, as one column of a page. */
class DisplayOverSection final : public juce::Component
{
public:
    DisplayOverSection(std::unique_ptr<juce::Component> picture, std::unique_ptr<ParameterSection> controls)
        : display(std::move(picture)), section(std::move(controls))
    {
        addAndMakeVisible(*display);
        addAndMakeVisible(*section);
    }

    ParameterSection& controls() noexcept { return *section; }

    int displayHeight(int height) const { return juce::roundToInt(static_cast<float>(height) * 0.38f); }
    int preferredWidth(int height) const
    {
        const auto& t = themeOf(*this);
        const int below = height - displayHeight(height) - juce::roundToInt(t.spacing);
        return juce::jmax(juce::roundToInt(t.controlHeight * 5.0f), section->preferredWidth(below));
    }
    void resized() override
    {
        const auto& t = themeOf(*this);
        auto r = getLocalBounds();
        display->setBounds(r.removeFromTop(displayHeight(getHeight())));
        r.removeFromTop(juce::roundToInt(t.spacing));
        section->setBounds(r);
    }

private:
    std::unique_ptr<juce::Component> display;
    std::unique_ptr<ParameterSection> section;
};

/** One cycle of a wave, -1..1, x 0..1. */
float oscillatorSample(int shape, float pw, int table, float position, float x)
{
    constexpr float twoPi = juce::MathConstants<float>::twoPi;
    switch (shape)
    {
    case 0: return std::sin(twoPi * x);
    case 1: return x < 0.25f ? 4.0f * x : x < 0.75f ? 2.0f - 4.0f * x : 4.0f * x - 4.0f;
    case 2: return 1.0f - 2.0f * x;
    case 3: return x < pw ? 1.0f : -1.0f;
    default: break;
    }
    const auto& library = dsp::WaveformLibrary::get();
    if (table < 0 || table >= library.numWavetables())
        return 0.0f;
    return library.wavetable(table).sample(static_cast<std::uint32_t>(x * 4294967295.0f), {}, position);
}

void drawWave(juce::Graphics& g, juce::Rectangle<float> area, const Theme& t, const std::function<float(float)>& wave)
{
    g.setColour(t.border.withAlpha(0.6f));
    g.drawHorizontalLine(juce::roundToInt(area.getCentreY()), area.getX(), area.getRight());
    const auto inner = area.reduced(t.padding * 0.5f, t.padding);
    g.setColour(t.accent);
    g.strokePath(curvePath(inner, juce::jmax(64, juce::roundToInt(inner.getWidth())), [&](float x) { return 0.5f + 0.5f * wave(x); }),
                 juce::PathStrokeType(juce::jmax(1.5f, t.stroke * 1.5f)));
}

std::unique_ptr<juce::Component> oscillatorPreview(DeviceContext& context, const juce::ValueTree& device, int osc)
{
    const juce::String n(osc);
    return std::make_unique<FunctionDisplay>(context, device, [n](juce::Graphics& g, juce::Rectangle<float> area, DeviceMirror& m, const Theme& t) {
        const int shape = juce::roundToInt(m.value("osc" + n + ".shape"));
        const float pw = m.value("osc" + n + ".pw");
        const int table = juce::roundToInt(m.value("osc" + n + ".table"));
        const float position = m.value("osc" + n + ".position");
        // A wavetable's frames aren't all at full scale: draw it at its own peak.
        float peak = 1.0f;
        if (shape == 4)
        {
            peak = 1.0e-3f;
            for (int i = 0; i < 256; ++i)
                peak = juce::jmax(peak, std::abs(oscillatorSample(shape, pw, table, position, static_cast<float>(i) / 256.0f)));
        }
        drawWave(g, area, t, [&](float x) { return oscillatorSample(shape, pw, table, position, x) / peak; });
    });
}

std::unique_ptr<juce::Component> lfoPreview(DeviceContext& context, const juce::ValueTree& device, int lfo)
{
    const juce::String n(lfo);
    return std::make_unique<FunctionDisplay>(context, device, [n](juce::Graphics& g, juce::Rectangle<float> area, DeviceMirror& m, const Theme& t) {
        const int shape = juce::roundToInt(m.value("lfo" + n + ".shape"));
        drawWave(g, area, t, [shape](float x) {
            // Two cycles, as the LFO shapes go: Sine, Triangle, Saw Up, Saw Down, Square, S&H.
            const float p = std::fmod(x * 2.0f, 1.0f);
            switch (shape)
            {
            case 0: return std::sin(juce::MathConstants<float>::twoPi * p);
            case 1: return p < 0.25f ? 4.0f * p : p < 0.75f ? 2.0f - 4.0f * p : 4.0f * p - 4.0f;
            case 2: return 2.0f * p - 1.0f;
            case 3: return 1.0f - 2.0f * p;
            case 4: return p < 0.5f ? 1.0f : -1.0f;
            default:
            {
                static constexpr float steps[] = {0.3f, -0.7f, 0.9f, -0.2f, 0.6f, -0.9f, 0.1f, -0.4f};
                return steps[juce::jlimit(0, 7, static_cast<int>(x * 8.0f))];
            }
            }
        });
    });
}

/** The modulation matrix: a row per slot with its source, destination and amount. */
class ModMatrix final : public juce::Component
{
public:
    ModMatrix(DeviceContext& context, const juce::ValueTree& device)
    {
        setTitle("Modulation matrix");
        for (int slot = 1; slot <= 8; ++slot)
            for (const auto* leaf : {"source", "dest", "amount"})
            {
                auto box = std::make_unique<NumberBox>(bind(context, device, "mod" + juce::String(slot) + "." + leaf));
                addAndMakeVisible(*box);
                boxes.push_back(std::move(box));
            }
    }

    int rows() const { return 8; }
    int preferredWidth(int) const
    {
        const auto& t = themeOf(*this);
        return juce::roundToInt(t.controlHeight * (1.0f + 4.0f + 4.0f + 3.0f));
    }
    juce::Rectangle<int> rowBounds(int row) const
    {
        const auto& t = themeOf(*this);
        const int header = juce::roundToInt(t.fontSmall + t.spacing);
        const int h = juce::jmax(1, (getHeight() - header) / rows());
        return {0, header + row * h, getWidth(), h};
    }
    void resized() override
    {
        const auto& t = themeOf(*this);
        for (int row = 0; row < rows(); ++row)
        {
            auto r = rowBounds(row).reduced(0, 1);
            r.removeFromLeft(juce::roundToInt(t.controlHeight));
            const int unit = r.getWidth() / 11;
            boxes[static_cast<std::size_t>(row * 3)]->setBounds(r.removeFromLeft(unit * 4).reduced(1, 0));
            boxes[static_cast<std::size_t>(row * 3 + 1)]->setBounds(r.removeFromLeft(unit * 4).reduced(1, 0));
            boxes[static_cast<std::size_t>(row * 3 + 2)]->setBounds(r.reduced(1, 0));
        }
    }
    void paint(juce::Graphics& g) override
    {
        const auto& t = themeOf(*this);
        g.setFont(juce::Font(t.font(t.fontSmall)).boldened());
        g.setColour(t.textDim);
        auto header = getLocalBounds().removeFromTop(juce::roundToInt(t.fontSmall + t.spacing));
        header.removeFromLeft(juce::roundToInt(t.controlHeight));
        const int unit = header.getWidth() / 11;
        g.drawText("SOURCE", header.removeFromLeft(unit * 4), juce::Justification::centredLeft, false);
        g.drawText("DESTINATION", header.removeFromLeft(unit * 4), juce::Justification::centredLeft, false);
        g.drawText("AMOUNT", header, juce::Justification::centredLeft, false);
        g.setFont(t.font(t.fontSmall));
        for (int row = 0; row < rows(); ++row)
            g.drawText(juce::String(row + 1), rowBounds(row).withWidth(juce::roundToInt(t.controlHeight)), juce::Justification::centred, false);
    }

private:
    std::vector<std::unique_ptr<NumberBox>> boxes;
};

class SynthPanel final : public DeviceBody
{
public:
    enum class Page
    {
        oscillators,
        filter,
        modulation
    };

    SynthPanel(DeviceContext& c, const juce::ValueTree& d) : DeviceBody(c, d)
    {
        for (const auto& [page, name] : {std::pair{Page::oscillators, "Osc"}, std::pair{Page::filter, "Filter"}, std::pair{Page::modulation, "Mod"}})
        {
            auto tab = std::make_unique<juce::TextButton>(name);
            tab->setClickingTogglesState(false);
            tab->setWantsKeyboardFocus(false);
            tab->setTooltip(page == Page::oscillators ? "The oscillators, sub, noise and unison."
                            : page == Page::filter    ? "The filter and the envelopes."
                                                      : "The LFOs and the modulation matrix.");
            tab->onClick = [this, p = page] { showPage(p); };
            addAndMakeVisible(*tab);
            tabs.push_back(std::move(tab));
        }
        always = section("", {"voice.mode", "voice.polyphony", "voice.glide", "out.volume", "out.pan"});
        addAndMakeVisible(*always);
        showPage(Page::oscillators);
    }

    Page page() const noexcept { return shown; }

    void showPage(Page p)
    {
        shown = p;
        columns.clear();
        switch (p)
        {
        case Page::oscillators:
            for (int osc = 1; osc <= 2; ++osc)
            {
                const juce::String n(osc);
                juce::StringArray ids{"osc" + n + ".shape", "osc" + n + ".level", "osc" + n + ".transpose", "osc" + n + ".detune",
                                      "osc" + n + ".pw", "osc" + n + ".table", "osc" + n + ".position"};
                if (osc == 2)
                    ids.add("osc2.sync");
                columns.push_back(column(std::make_unique<DisplayOverSection>(oscillatorPreview(context, node, osc), section("Osc " + n, ids))));
            }
            columns.push_back(column(section("Sub / Noise", {"sub.level", "sub.octave", "noise.level", "noise.colour"})));
            columns.push_back(column(section("Unison", {"unison.voices", "unison.detune", "unison.spread"})));
            break;
        case Page::filter:
        {
            auto curve = std::make_unique<FunctionDisplay>(context, node, [](juce::Graphics& g, juce::Rectangle<float> area, DeviceMirror& m, const Theme& t) {
                drawFilterResponse(g, area, t, juce::roundToInt(m.value("filter.type")), m.value("filter.cutoff"), m.value("filter.resonance"));
            });
            columns.push_back(column(std::make_unique<DisplayOverSection>(
                std::move(curve), section("Filter", {"filter.type", "filter.cutoff", "filter.resonance", "filter.drive", "filter.keytrack",
                                                     "filter.envAmount", "filter.velocity"}))));
            columns.push_back(column(std::make_unique<DisplayOverSection>(
                std::make_unique<EnvelopeEditor>(context, node, juce::StringArray{"amp.attack", "amp.decay", "amp.sustain", "amp.release"}),
                section("Amp Env", {"amp.attack", "amp.decay", "amp.sustain", "amp.release", "amp.velocity"}))));
            columns.push_back(column(std::make_unique<DisplayOverSection>(
                std::make_unique<EnvelopeEditor>(context, node, juce::StringArray{"env2.attack", "env2.decay", "env2.sustain", "env2.release"}),
                section("Env 2", {"env2.attack", "env2.decay", "env2.sustain", "env2.release"}))));
            break;
        }
        case Page::modulation:
            for (int lfo = 1; lfo <= 2; ++lfo)
            {
                const juce::String n(lfo);
                auto controls = section("LFO " + n, {"lfo" + n + ".shape", "lfo" + n + ".rate", "lfo" + n + ".sync", "lfo" + n + ".syncRate",
                                                     "lfo" + n + ".retrigger"});
                columns.push_back(column(std::make_unique<DisplayOverSection>(lfoPreview(context, node, lfo), std::move(controls))));
            }
            columns.push_back(column(std::make_unique<ModMatrix>(context, node)));
            break;
        }
        for (auto& col : columns)
            addAndMakeVisible(*col.component);
        for (std::size_t i = 0; i < tabs.size(); ++i)
            tabs[i]->setToggleState(static_cast<int>(i) == static_cast<int>(p), juce::dontSendNotification);
        resized();
        widthChanged();
    }

    bool reveal(const juce::String& id) override
    {
        const auto page = id.startsWith("osc") || id.startsWith("sub.") || id.startsWith("noise.") || id.startsWith("unison.") ? Page::oscillators
                          : id.startsWith("filter.") || id.startsWith("amp.") || id.startsWith("env2.")                       ? Page::filter
                          : id.startsWith("lfo") || id.startsWith("mod")                                                      ? Page::modulation
                                                                                                                              : shown;
        if (page != shown)
            showPage(page);
        return controlFor(id) != nullptr;
    }

    int preferredWidth(int height) const override
    {
        const auto& t = themeOf(*this);
        int w = tabWidth() + juce::roundToInt(t.padding);
        for (const auto& col : columns)
            w += col.widthFor(height) + juce::roundToInt(t.padding);
        return w + always->preferredWidth(height);
    }

    void resized() override
    {
        const auto& t = themeOf(*this);
        auto r = getLocalBounds();
        auto strip = r.removeFromLeft(tabWidth());
        for (auto& tab : tabs)
            tab->setBounds(strip.removeFromTop(juce::roundToInt(t.controlHeight)).reduced(0, 1));
        r.removeFromLeft(juce::roundToInt(t.padding));
        always->setBounds(r.removeFromRight(always->preferredWidth(r.getHeight())));
        for (auto& col : columns)
        {
            col.component->setBounds(r.removeFromLeft(col.widthFor(r.getHeight())));
            r.removeFromLeft(juce::roundToInt(t.padding));
        }
    }

private:
    struct Column
    {
        std::unique_ptr<juce::Component> component;
        std::function<int(int)> widthFor;
    };

    int tabWidth() const { return juce::roundToInt(themeOf(*this).controlHeight * 2.4f); }

    std::unique_ptr<ParameterSection> section(const juce::String& title, const juce::StringArray& ids)
    {
        auto s = std::make_unique<ParameterSection>(title);
        s->addParameters(context.set(), node, prototype(), ids);
        return s;
    }
    Column column(std::unique_ptr<DisplayOverSection> c)
    {
        auto* raw = c.get();
        return {std::move(c), [raw](int h) { return raw->preferredWidth(h); }};
    }
    Column column(std::unique_ptr<ParameterSection> c)
    {
        auto* raw = c.get();
        return {std::move(c), [raw](int h) { return raw->preferredWidth(h); }};
    }
    Column column(std::unique_ptr<ModMatrix> c)
    {
        auto* raw = c.get();
        return {std::move(c), [raw](int h) { return raw->preferredWidth(h); }};
    }

    std::vector<std::unique_ptr<juce::TextButton>> tabs;
    std::unique_ptr<ParameterSection> always;
    std::vector<Column> columns;
    Page shown = Page::oscillators;
};
} // namespace

std::unique_ptr<DeviceBody> synth(DeviceContext& c, const juce::ValueTree& d)
{
    return std::make_unique<SynthPanel>(c, d);
}

} // namespace lattice::ui::panels
