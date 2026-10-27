#include "ui/clip/ClipPropertiesPanel.h"

#include "ui/clip/NoteGrid.h"
#include "ui/session/SessionView.h"
#include "ui/theme/LatticeLookAndFeel.h"

namespace lattice::ui
{
namespace
{
const juce::StringArray launchModes{"Trigger", "Gate", "Toggle"};
const juce::StringArray quantizations{"Global", "None", "8 Bars", "4 Bars", "2 Bars", "1 Bar", "1/2", "1/4", "1/8", "1/16"};
const juce::StringArray followActions{"None", "Stop", "Play Again", "Previous", "Next", "First", "Last", "Any", "Other"};
const juce::StringArray denominators{"1", "2", "4", "8", "16"};
} // namespace

/** The clip's name; a click renames it. */
class ClipPropertiesPanel::NameButton final : public juce::Button
{
public:
    NameButton(LiveSet& s, juce::ValueTree c) : juce::Button("Name"), set(s), clip(std::move(c))
    {
        setTitle("Clip Name");
        setHelpText("Click to rename the clip.");
        setMouseClickGrabsKeyboardFocus(false);
        setWantsKeyboardFocus(false);
        onClick = [this] {
            askForText(*this, "Rename clip", clip[ids::name].toString(), [this](const juce::String& typed) {
                if (auto* um = set.undoManager())
                    um->beginNewTransaction("Rename clip");
                set.setClipProperty(clip, ids::name, typed.trim());
            });
        };
    }
    void paintButton(juce::Graphics& g, bool highlighted, bool) override
    {
        const auto& t = themeOf(*this);
        const auto r = getLocalBounds().toFloat();
        g.setColour(highlighted ? t.surfaceRaised : t.surfaceSunken);
        g.fillRoundedRectangle(r, t.radius);
        g.setColour(t.text);
        g.setFont(t.font(t.fontNormal));
        const auto name = clip[ids::name].toString();
        g.drawFittedText(name.isNotEmpty() ? name : juce::String("(no name)"), getLocalBounds().reduced(juce::roundToInt(t.spacing), 0),
                         juce::Justification::centredLeft, 1);
    }

private:
    LiveSet& set;
    juce::ValueTree clip;
};

/** The clip's colour; a click offers the palette. */
class ClipPropertiesPanel::ColourSwatch final : public juce::Button
{
public:
    ColourSwatch(LiveSet& s, juce::ValueTree c) : juce::Button("Colour"), set(s), clip(std::move(c))
    {
        setTitle("Clip Colour");
        setHelpText("Click to choose the clip's colour.");
        setMouseClickGrabsKeyboardFocus(false);
        setWantsKeyboardFocus(false);
        onClick = [this] {
            juce::PopupMenu menu;
            for (int i = 0; i < LiveSet::paletteSize(); ++i)
            {
                const auto argb = LiveSet::paletteColour(i);
                menu.addColouredItem(i + 1, "Colour " + juce::String(i + 1), juce::Colour(argb), true, false, nullptr);
            }
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                               [safe = juce::Component::SafePointer<ColourSwatch>(this)](int chosen) {
                                   if (safe == nullptr || chosen <= 0)
                                       return;
                                   if (auto* um = safe->set.undoManager())
                                       um->beginNewTransaction("Clip colour");
                                   safe->set.setColour(safe->clip, LiveSet::paletteColour(chosen - 1));
                               });
        };
    }
    void paintButton(juce::Graphics& g, bool highlighted, bool) override
    {
        const auto& t = themeOf(*this);
        const auto r = getLocalBounds().toFloat().reduced(1.0f);
        const auto colour = juce::Colour(static_cast<juce::uint32>(static_cast<juce::int64>(clip.getProperty(ids::colour, static_cast<juce::int64>(0xff808080)))));
        g.setColour(colour);
        g.fillRoundedRectangle(r, t.radius);
        g.setColour(highlighted ? t.focus : t.border);
        g.drawRoundedRectangle(r, t.radius, t.stroke);
    }

private:
    LiveSet& set;
    juce::ValueTree clip;
};

ClipPropertiesPanel::ClipPropertiesPanel(SessionController& s, PianoRoll& r) : session(s), roll(r)
{
    setTitle("Clip properties");
}

ClipPropertiesPanel::~ClipPropertiesPanel()
{
    clip.removeListener(this);
}

void ClipPropertiesPanel::setClip(const juce::ValueTree& c)
{
    if (c != clip)
    {
        clip.removeListener(this);
        clip = c;
        clip.addListener(this);
    }
    build();
}

void ClipPropertiesPanel::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property)
{
    // The time signature changes how every time reads; the name and colour are drawn.
    if (tree != clip)
        return;
    if (property == ids::clipTimeSigNum || property == ids::clipTimeSigDen)
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<ClipPropertiesPanel>(this)] {
            if (safe != nullptr)
                safe->build();
        });
    else if (property == ids::name || property == ids::colour)
        repaint();
}

NumberBox* ClipPropertiesPanel::box(std::unique_ptr<ValueBinding> binding)
{
    auto b = std::make_unique<NumberBox>(std::move(binding));
    b->setTitle(b->binding().spec().name);
    b->setMouseClickGrabsKeyboardFocus(false);
    auto* raw = b.get();
    owned.push_back(std::move(b));
    addAndMakeVisible(*raw);
    return raw;
}

juce::Button* ClipPropertiesPanel::action(const juce::String& caption, const juce::String& help, std::function<void()> run)
{
    auto b = std::make_unique<juce::TextButton>(caption);
    b->setTitle(caption);
    b->setHelpText(help);
    b->setWantsKeyboardFocus(false);
    b->setMouseClickGrabsKeyboardFocus(false);
    b->onClick = std::move(run);
    auto* raw = b.get();
    owned.push_back(std::move(b));
    addAndMakeVisible(*raw);
    return raw;
}

Toggle* ClipPropertiesPanel::mode(const juce::String& caption, const juce::String& help, bool on, std::function<void(bool)> set)
{
    auto b = std::make_unique<Toggle>(caption);
    b->setTitle(caption);
    b->setHelpText(help);
    b->setToggleState(on, juce::dontSendNotification);
    b->setWantsKeyboardFocus(false);
    b->setMouseClickGrabsKeyboardFocus(false);
    auto* raw = b.get();
    b->onClick = [raw, set = std::move(set)] { set(raw->getToggleState()); };
    owned.push_back(std::move(b));
    addAndMakeVisible(*raw);
    return raw;
}

void ClipPropertiesPanel::heading(const juce::String& text)
{
    rows.push_back({text, {}, true});
}

void ClipPropertiesPanel::row(const juce::String& label, std::vector<juce::Component*> controls)
{
    rows.push_back({label, std::move(controls), false});
}

void ClipPropertiesPanel::build()
{
    rows.clear();
    owned.clear();
    if (!clip.isValid())
    {
        resized();
        repaint();
        return;
    }
    auto& set = session.set();
    auto* undo = &session.undoManager();
    auto c = clip;
    const auto [num, den] = set.clipTimeSignature(c);
    const BarsBeats bb{num, den};
    const auto matchesClip = [c](std::initializer_list<juce::Identifier> props) {
        std::vector<juce::Identifier> list(props);
        return [c, list](const juce::ValueTree& tree, const juce::Identifier& p) {
            return tree == c && std::find(list.begin(), list.end(), p) != list.end();
        };
    };
    // In sixteenths (the range's minimum is a whole number of them, so the steps fall on them).
    const auto timeSpec = [bb](const juce::String& id, const juce::String& name, bool isLength, float minimum) {
        return ParameterSpec::continuous(id, name, minimum, 4096.0f, isLength ? 4.0f : 0.0f, ParamUnit::Beats)
            .withCentre(32.0f)
            .withInterval(0.25f)
            .inGroup("Clip")
            .withFormatter([bb, isLength](float v) { return isLength ? bb.length(v) : bb.position(v); })
            .withParser([bb, isLength](const juce::String& text) -> std::optional<float> {
                const auto q = isLength ? bb.parseLength(text) : bb.parsePosition(text);
                return q ? std::optional<float>(static_cast<float>(*q)) : std::nullopt;
            });
    };
    const auto choice = [&](const juce::String& id, const juce::String& name, const juce::StringArray& names, juce::Identifier prop,
                            int offset) {
        return box(std::make_unique<ValueBinding>(
            ParameterSpec::choice(id, name, names, offset).inGroup("Launch"),
            [c, prop, offset] { return static_cast<double>(static_cast<int>(c[prop]) + offset); },
            [&set, c, prop, offset](double v) { set.setClipProperty(c, prop, juce::roundToInt(v) - offset); }, undo, c,
            matchesClip({prop})));
    };

    // --- Clip -----------------------------------------------------------------------------------
    heading("Clip");
    auto name = std::make_unique<NameButton>(set, c);
    auto colour = std::make_unique<ColourSwatch>(set, c);
    auto* nameRaw = name.get();
    auto* colourRaw = colour.get();
    owned.push_back(std::move(name));
    owned.push_back(std::move(colour));
    addAndMakeVisible(*nameRaw);
    addAndMakeVisible(*colourRaw);
    row({}, {nameRaw, colourRaw});
    auto* numerator = box(std::make_unique<ValueBinding>(
        ParameterSpec::integer("clipTimeSigNum", "Signature Numerator", 1, 99, 4).inGroup("Clip"),
        [&set, c] { return static_cast<double>(set.clipTimeSignature(c).first); },
        [&set, c](double v) { set.setClipTimeSignature(c, juce::roundToInt(v), set.clipTimeSignature(c).second); }, undo, c,
        matchesClip({ids::clipTimeSigNum, ids::clipTimeSigDen})));
    auto* denominator = box(std::make_unique<ValueBinding>(
        ParameterSpec::choice("clipTimeSigDen", "Signature Denominator", denominators, 2).inGroup("Clip"),
        [&set, c] { return static_cast<double>(denominators.indexOf(juce::String(set.clipTimeSignature(c).second))); },
        [&set, c](double v) {
            set.setClipTimeSignature(c, set.clipTimeSignature(c).first, denominators[juce::jlimit(0, 4, juce::roundToInt(v))].getIntValue());
        },
        undo, c, matchesClip({ids::clipTimeSigNum, ids::clipTimeSigDen})));
    row("Signature", {numerator, denominator});

    // --- Launch ---------------------------------------------------------------------------------
    heading("Launch");
    row("Mode", {choice("launchMode", "Launch Mode", launchModes, ids::launchMode, 0)});
    row("Quantization", {choice("quantization", "Launch Quantization", quantizations, ids::quantization, 1)});
    auto* legatoToggle = std::make_unique<Toggle>(ValueBinding::property(ParameterSpec::toggle("legato", "Legato", false).inGroup("Launch"), undo, c,
                                                                         ids::legato, [&set, c](double v) { set.setClipProperty(c, ids::legato, v > 0.5); }),
                                                  Toggle::Role::accent, "Legato")
                              .release();
    legatoToggle->setMouseClickGrabsKeyboardFocus(false);
    owned.emplace_back(legatoToggle);
    addAndMakeVisible(*legatoToggle);
    row({}, {legatoToggle});
    row("Follow", {choice("followA", "Follow Action A", followActions, ids::followA, 0),
                   choice("followB", "Follow Action B", followActions, ids::followB, 0)});
    row("Chance A", {box(ValueBinding::property(ParameterSpec::integer("followChanceA", "Follow Chance A", 0, 100, 100, ParamUnit::Percent).inGroup("Launch"),
                                                undo, c, ids::followChanceA,
                                                [&set, c](double v) { set.setClipProperty(c, ids::followChanceA, juce::roundToInt(v)); }))});
    row("Follow Time", {box(ValueBinding::property(timeSpec("followTime", "Follow Time", true, 0.0f), undo, c, ids::followTime,
                                                   [&set, c](double v) { set.setClipProperty(c, ids::followTime, v); }))});

    // --- Loop -----------------------------------------------------------------------------------
    heading("Loop");
    auto* loopToggle = std::make_unique<Toggle>(ValueBinding::property(ParameterSpec::toggle("looping", "Loop", true).inGroup("Loop"), undo, c, ids::looping,
                                                                       [&set, c](double v) { set.setClipProperty(c, ids::looping, v > 0.5); }),
                                                Toggle::Role::accent, "Loop")
                            .release();
    loopToggle->setMouseClickGrabsKeyboardFocus(false);
    owned.emplace_back(loopToggle);
    addAndMakeVisible(*loopToggle);
    row({}, {loopToggle});
    row("Start", {box(ValueBinding::property(timeSpec("startMarker", "Start", false, 0.0f), undo, c, ids::startMarker,
                                             [&set, c](double v) { set.setClipProperty(c, ids::startMarker, v); }))});
    row("End", {box(std::make_unique<ValueBinding>(
                   timeSpec("loopEnd", "End", false, 0.25f), [c] { return static_cast<double>(c[ids::loopEnd]); },
                   [&set, c](double v) { set.setClipLoop(c, c[ids::loopStart], v); }, undo, c, matchesClip({ids::loopEnd})))});
    row("Position", {box(std::make_unique<ValueBinding>(
                        timeSpec("loopStart", "Loop Position", false, 0.0f), [c] { return static_cast<double>(c[ids::loopStart]); },
                        [&set, c](double v) {
                            // Moves the loop, keeping its length.
                            const double length = static_cast<double>(c[ids::loopEnd]) - static_cast<double>(c[ids::loopStart]);
                            set.setClipLoop(c, v, v + length);
                        },
                        undo, c, matchesClip({ids::loopStart, ids::loopEnd})))});
    row("Length", {box(std::make_unique<ValueBinding>(
                      timeSpec("loopLength", "Loop Length", true, 0.25f),
                      [c] { return static_cast<double>(c[ids::loopEnd]) - static_cast<double>(c[ids::loopStart]); },
                      [&set, c](double v) { set.setClipLoop(c, c[ids::loopStart], static_cast<double>(c[ids::loopStart]) + v); }, undo, c,
                      matchesClip({ids::loopStart, ids::loopEnd})))});
    row({}, {action("Double", "Doubles the loop's length.", [this] { roll.doubleLoop(); }),
             action("Halve", "Halves the loop's length.", [this] { roll.halveLoop(); }),
             action("Dupl.", "Duplicates the loop: copies its notes after it and doubles its length.", [this] { roll.duplicateLoop(); })});

    // --- Notes ----------------------------------------------------------------------------------
    heading("Notes");
    auto* amount = box(std::make_unique<ValueBinding>(
        ParameterSpec::integer("quantizeAmount", "Quantize Amount", 0, 100, 100, ParamUnit::Percent).inGroup("Notes"),
        [this] { return roll.quantizeAmount * 100.0; }, [this](double v) { roll.quantizeAmount = v / 100.0; }, nullptr, juce::ValueTree(),
        nullptr));
    row({}, {action("Quantize", "Moves the selected notes (or all) towards the grid by the amount (Ctrl+U).", [this] { roll.quantize(); }),
             amount,
             mode("Ends", "Quantize the notes' ends too.", roll.quantizeEnds, [this](bool on) { roll.quantizeEnds = on; })});
    auto* legatoNotes = action("Legato", "Each note lasts until the next one starts.", [this] { roll.legato(); });
    legatoNotes->setTitle("Legato Notes"); // the Launch section has a Legato too
    row({}, {legatoNotes,
             action("Reverse", "Mirrors the notes in time.", [this] { roll.reverse(); }),
             action("Invert", "Turns the notes upside down.", [this] { roll.invert(); })});
    row({}, {action(juce::String::fromUTF8("\xc3\x97" "2"), "Stretches the notes to twice as long.", [this] { roll.stretch(2.0); }),
             action(juce::String::fromUTF8("\xc3\xb7" "2"), "Squeezes the notes to half as long.", [this] { roll.stretch(0.5); }),
             action("Split", "Splits the selected notes at the grid (Ctrl+E).", [this] { roll.split(); }),
             action("Join", "Joins the selected notes of each row (Ctrl+J).", [this] { roll.join(); })});
    row("Steps", {action("-1", "Moves the notes down one table step.", [this] { roll.transposeSteps(-1); }),
                  action("+1", "Moves the notes up one table step.", [this] { roll.transposeSteps(1); })});
    auto* semitones = box(std::make_unique<ValueBinding>(
        ParameterSpec::integer("transposeAmount", "Transpose Semitones", 1, 48, 12, ParamUnit::Semitones).inGroup("Notes"),
        [this] { return roll.transposeAmount; }, [this](double v) { roll.transposeAmount = v; }, nullptr, juce::ValueTree(), nullptr));
    row("Semitones", {action("-", "Moves the notes down by the semitones, to the nearest entries of the tuning.",
                             [this] { roll.transposeSemitones(-roll.transposeAmount); }),
                      semitones,
                      action("+", "Moves the notes up by the semitones, to the nearest entries of the tuning.",
                             [this] { roll.transposeSemitones(roll.transposeAmount); })});
    row({}, {mode("Draw", "Draw mode (B): a click adds a note, a click on one deletes it.", roll.isDrawMode(),
                  [this](bool on) { roll.setDrawMode(on); }),
             mode("Fold", "Shows only the rows with notes.", roll.isFolded(), [this](bool on) { roll.setFolded(on); }),
             mode("Log", "Spaces the rows by their frequencies, so the gaps show the real intervals.", roll.areRowsLogSpaced(),
                  [this](bool on) { roll.setRowsLogSpaced(on); })});

    resized();
    repaint();
}

int ClipPropertiesPanel::rowHeight() const
{
    return juce::roundToInt(themeOf(*this).controlHeight);
}

int ClipPropertiesPanel::preferredHeight() const
{
    const auto& t = themeOf(*this);
    int h = juce::roundToInt(t.padding * 2.0f);
    for (const auto& r : rows)
        h += (r.heading ? juce::roundToInt(t.fontSmall + t.spacing * 2.0f) : rowHeight()) + juce::roundToInt(t.spacing);
    return h;
}

juce::Component* ClipPropertiesPanel::control(const juce::String& title) const
{
    for (const auto& c : owned)
        if (c->getTitle() == title)
            return c.get();
    return nullptr;
}

juce::Button* ClipPropertiesPanel::button(const juce::String& caption) const
{
    for (const auto& c : owned)
        if (auto* b = dynamic_cast<juce::Button*>(c.get()); b != nullptr && b->getButtonText() == caption)
            return b;
    return nullptr;
}

void ClipPropertiesPanel::resized()
{
    const auto& t = themeOf(*this);
    auto area = getLocalBounds().reduced(juce::roundToInt(t.padding));
    const int gap = juce::roundToInt(t.spacing);
    const int labelWidth = juce::roundToInt(t.controlHeight * 3.2f);
    for (auto& r : rows)
    {
        if (r.heading)
        {
            area.removeFromTop(juce::roundToInt(t.fontSmall + t.spacing * 2.0f) + gap);
            continue;
        }
        auto line = area.removeFromTop(rowHeight());
        area.removeFromTop(gap);
        if (r.label.isNotEmpty())
            line.removeFromLeft(labelWidth);
        const int n = static_cast<int>(r.controls.size());
        if (n == 0)
            continue;
        const int each = (line.getWidth() - gap * (n - 1)) / n;
        for (int i = 0; i < n; ++i)
        {
            // The colour swatch beside the name is square.
            const bool swatch = dynamic_cast<ColourSwatch*>(r.controls[static_cast<std::size_t>(i)]) != nullptr;
            const int w = swatch ? line.getHeight() : (n == 2 && dynamic_cast<ColourSwatch*>(r.controls[1]) != nullptr ? line.getWidth() - line.getHeight() - gap : each);
            r.controls[static_cast<std::size_t>(i)]->setBounds(line.removeFromLeft(w));
            line.removeFromLeft(gap);
        }
    }
}

void ClipPropertiesPanel::paint(juce::Graphics& g)
{
    const auto& t = themeOf(*this);
    g.fillAll(t.surface);
    if (!clip.isValid())
        return;
    auto area = getLocalBounds().reduced(juce::roundToInt(t.padding));
    const int gap = juce::roundToInt(t.spacing);
    const int labelWidth = juce::roundToInt(t.controlHeight * 3.2f);
    for (const auto& r : rows)
    {
        if (r.heading)
        {
            auto line = area.removeFromTop(juce::roundToInt(t.fontSmall + t.spacing * 2.0f));
            area.removeFromTop(gap);
            g.setColour(t.textDim);
            g.setFont(juce::Font(t.font(t.fontSmall)).boldened());
            g.drawText(r.label.toUpperCase(), line, juce::Justification::bottomLeft, false);
            g.setColour(t.border);
            g.fillRect(line.getX(), line.getBottom(), line.getWidth(), 1);
            continue;
        }
        auto line = area.removeFromTop(rowHeight());
        area.removeFromTop(gap);
        if (r.label.isNotEmpty())
        {
            g.setColour(t.textDim);
            g.setFont(t.font(t.fontSmall));
            g.drawText(r.label, line.removeFromLeft(labelWidth), juce::Justification::centredLeft, true);
        }
    }
}

} // namespace lattice::ui
