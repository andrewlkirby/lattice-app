#include "ui/shell/ControlBar.h"

#include "engine/Transport.h"
#include "ui/theme/LatticeLookAndFeel.h"

namespace lattice::ui
{
namespace
{
const juce::StringArray quantizationNames{"None", "8 Bars", "4 Bars", "2 Bars", "1 Bar", "1/2", "1/4", "1/8", "1/16"};
const int denominators[] = {1, 2, 4, 8, 16};

int denominatorIndex(int denominator)
{
    for (int i = 0; i < 5; ++i)
        if (denominators[i] == denominator)
            return i;
    return 2;
}
} // namespace

// --- CommandButton ---------------------------------------------------------------------------------

CommandButton::CommandButton(CommandRegistry& registry, const juce::String& commandId, const juce::String& caption)
    : juce::TextButton(caption), commands(registry), id(commandId)
{
    setMouseClickGrabsKeyboardFocus(false); // Space stays Play/Stop
    getProperties().set("commandId", id);
    if (const auto* c = commands.find(id))
    {
        setTitle(c->name);
        const auto keys = commands.shortcutText(id);
        setTooltip(keys.isNotEmpty() ? c->name + " (" + keys + ")" : c->name);
    }
    onClick = [this] { commands.perform(id); };
}

// --- LoadMeter -------------------------------------------------------------------------------------

LoadMeter::LoadMeter(int workerThreads) : workers(workerThreads)
{
    setTitle("Audio load");
    setHelpText("How long the audio callback takes, as a share of the time it has (99th percentile), and dropouts.");
}

void LoadMeter::setLoad(const EngineStateFeed::Load& l)
{
    p99 = l.callback.p99;
    drops = l.callback.overruns + static_cast<std::uint64_t>(juce::jmax(0, l.deviceXruns));
    setTooltip("Audio callback, share of the buffer period: median " + juce::String(juce::roundToInt(l.callback.p50 * 100.0)) +
               "%, 99th percentile " + juce::String(juce::roundToInt(l.callback.p99 * 100.0)) + "%, 99.9th " +
               juce::String(juce::roundToInt(l.callback.p999 * 100.0)) + "%, worst " +
               juce::String(juce::roundToInt(l.callback.max * 100.0)) + "%\n" + juce::String(l.callback.overruns) +
               " callbacks over time, " + juce::String(juce::jmax(0, l.deviceXruns)) + " device dropouts\n" +
               juce::String(workers) + (workers == 1 ? " worker thread" : " worker threads"));
    repaint();
}

juce::String LoadMeter::text() const
{
    return juce::String(juce::roundToInt(p99 * 100.0)) + "%";
}

void LoadMeter::paint(juce::Graphics& g)
{
    const auto& t = themeOf(*this);
    auto area = getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(t.surfaceSunken);
    g.fillRoundedRectangle(area, t.radius);
    g.setColour(t.border);
    g.drawRoundedRectangle(area, t.radius, t.stroke);

    auto inner = area.reduced(t.padding * 0.5f, t.padding * 0.5f);
    auto dropsArea = drops > 0 ? inner.removeFromRight(juce::jmax(inner.getHeight(), 24.0f)) : juce::Rectangle<float>();
    auto bar = inner.removeFromBottom(3.0f);
    const auto level = static_cast<float>(juce::jlimit(0.0, 1.0, p99));
    g.setColour(t.knobTrack);
    g.fillRect(bar);
    g.setColour(level > 0.7f ? t.meterClip : level > 0.5f ? t.meterHigh : t.meterLow);
    g.fillRect(bar.withWidth(bar.getWidth() * level));

    g.setFont(t.font(t.fontSmall));
    g.setColour(t.text);
    g.drawText("CPU " + text(), inner.toNearestInt(), juce::Justification::centred);
    if (drops > 0)
    {
        g.setColour(t.error);
        g.fillRoundedRectangle(dropsArea.reduced(1.0f), t.radius);
        g.setColour(textOn(t.error, t));
        g.drawText(juce::String(drops), dropsArea.toNearestInt(), juce::Justification::centred);
    }
}

// --- ControlBar ------------------------------------------------------------------------------------

ControlBar::ControlBar(SessionController& s, CommandRegistry& registry, EngineStateFeed& f, int workerThreads)
    : session(s), commands(registry), feed(f), tap(registry, "transport.tapTempo", "Tap"),
      stop(registry, "transport.stop", juce::String::fromUTF8("\xe2\x96\xa0")), load(workerThreads)
{
    auto& set = session.set();
    auto* undo = &session.undoManager();
    auto root = set.state();

    tempo = std::make_unique<NumberBox>(ValueBinding::property(
        ParameterSpec::continuous("tempo", "Tempo", static_cast<float>(LiveSet::minTempo), static_cast<float>(LiveSet::maxTempo),
                                  120.0f)
            .withInterval(0.01f)
            .withFormatter([](float v) { return juce::String(v, 2); }),
        undo, root, ids::tempo, [&set](double v) { set.setTempo(v); }));
    signatureTop = std::make_unique<NumberBox>(ValueBinding::property(
        ParameterSpec::integer("timeSigNum", "Time Signature (beats in a bar)", 1, 99, 4), undo, root, ids::timeSigNum,
        [&set](double v) { set.setTimeSignature(juce::roundToInt(v), set.timeSigDenominator()); }));
    signatureBottom = std::make_unique<NumberBox>(std::make_unique<ValueBinding>(
        ParameterSpec::choice("timeSigDen", "Time Signature (beat length)", {"1", "2", "4", "8", "16"}, 2),
        [&set] { return static_cast<double>(denominatorIndex(set.timeSigDenominator())); },
        [&set](double v) { set.setTimeSignature(set.timeSigNumerator(), denominators[juce::jlimit(0, 4, juce::roundToInt(v))]); },
        undo, root, [](const juce::ValueTree&, const juce::Identifier& id) { return id == ids::timeSigDen; }));
    quantization = std::make_unique<NumberBox>(ValueBinding::property(
        ParameterSpec::choice("globalQuantization", "Global Launch Quantization", quantizationNames,
                              static_cast<int>(Quantization::Bar1)),
        undo, root, ids::globalQuantization, [&set](double v) { set.setGlobalQuantization(static_cast<Quantization>(juce::roundToInt(v))); }));
    metronome = std::make_unique<Toggle>(ValueBinding::property(ParameterSpec::toggle("metronomeOn", "Metronome", false), undo, root,
                                                                ids::metronomeOn, [&set](double v) { set.setMetronomeOn(v > 0.5); }),
                                         Toggle::Role::accent, "Metro");
    for (auto* c : std::initializer_list<juce::Component*>{tempo.get(), signatureTop.get(), signatureBottom.get(), quantization.get(),
                                                           metronome.get(), &tap})
        addAndMakeVisible(*c);

    position.setTitle("Position");
    position.setHelpText("Where the transport is: bar, beat, sixteenth.");
    position.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(position);

    // Play and record show the transport; a click runs the command.
    for (auto* button : {&play, &record})
    {
        button->setClickingTogglesState(false);
        button->setMouseClickGrabsKeyboardFocus(false);
        addAndMakeVisible(*button);
    }
    play.setTitle("Play");
    play.getProperties().set("commandId", "transport.play");
    play.onClick = [this] { commands.perform("transport.play"); };
    record.setTitle("Session Record");
    record.getProperties().set("commandId", "transport.sessionRecord");
    record.onClick = [this] { commands.perform("transport.sessionRecord"); };
    addAndMakeVisible(stop);

    keys.setTitle("Computer MIDI keyboard");
    keys.setHelpText("Plays the armed tracks from the computer keyboard: A W S E D F T G Y H U J K O L P ; play notes, Z and X "
                     "change the range, C and V the velocity, M turns it on and off.");
    keys.setMouseClickGrabsKeyboardFocus(false);
    keys.onClick = [this] {
        if (keyboard != nullptr)
            keyboard->setEnabled(keys.getToggleState());
    };
    addAndMakeVisible(keys);
    keyboardInfo.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(keyboardInfo);

    for (auto* m : {&midiMap, &keyMap})
    {
        m->setEnabled(false);
        addAndMakeVisible(*m);
    }
    midiMap.setTitle("MIDI Map mode");
    midiMap.setHelpText("Maps any control to a MIDI note or CC (coming in 4g).");
    keyMap.setTitle("Key Map mode");
    keyMap.setHelpText("Maps any control to a computer key (coming in 4g).");
    addAndMakeVisible(load);

    refreshKeyboard();
    feed.addListener(this, EngineStateFeed::transportAspect | EngineStateFeed::loadAspect);
    engineStateChanged(EngineStateFeed::transportAspect | EngineStateFeed::loadAspect);
}

ControlBar::~ControlBar()
{
    feed.removeListener(this);
    setComputerKeyboard(nullptr);
}

void ControlBar::setComputerKeyboard(ComputerKeyboardInput* k)
{
    if (keyboard != nullptr)
        keyboard->onStateChanged = previousKeyboardCallback;
    keyboard = k;
    previousKeyboardCallback = {};
    if (keyboard != nullptr)
    {
        // Keep what the keyboard's owner listens with, and add the bar's readout.
        previousKeyboardCallback = keyboard->onStateChanged;
        keyboard->onStateChanged = [this, previous = previousKeyboardCallback] {
            if (previous)
                previous();
            refreshKeyboard();
        };
    }
    refreshKeyboard();
}

void ControlBar::refreshKeyboard()
{
    keys.setEnabled(keyboard != nullptr);
    if (keyboard == nullptr)
    {
        keyboardInfo.setText({}, juce::dontSendNotification);
        return;
    }
    keys.setToggleState(keyboard->isEnabled(), juce::dontSendNotification);
    keyboardInfo.setText("#" + juce::String(keyboard->getBaseNote()) + juce::String::fromUTF8("\xe2\x80\x93") + "#" +
                             juce::String(keyboard->getLastNote()) + "  vel " + juce::String(keyboard->getVelocity()),
                         juce::dontSendNotification);
    keyboardInfo.setEnabled(keyboard->isEnabled());
}

void ControlBar::engineStateChanged(std::uint32_t aspects)
{
    if ((aspects & EngineStateFeed::transportAspect) != 0)
    {
        const auto& t = feed.transport();
        play.setToggleState(t.playing, juce::dontSendNotification);
        record.setToggleState(t.sessionRecord, juce::dontSendNotification);
        const auto p = Transport::positionOf(t.beat, t.barOrigin, t.barOriginIndex, t.timeSigNum, t.timeSigDen);
        const auto text = juce::String(p.bar) + ". " + juce::String(p.beat) + ". " + juce::String(p.sixteenth);
        if (text != position.getText())
            position.setText(text, juce::dontSendNotification);
    }
    if ((aspects & EngineStateFeed::loadAspect) != 0)
        load.setLoad(feed.load());
}

int ControlBar::preferredHeight() const
{
    const auto& t = themeOf(*this);
    return juce::roundToInt(t.controlHeight + t.padding * 2.0f);
}

void ControlBar::paint(juce::Graphics& g)
{
    const auto& t = themeOf(*this);
    g.fillAll(t.surface);
    g.setColour(t.border);
    g.fillRect(getLocalBounds().removeFromBottom(1));
    position.setColour(juce::Label::textColourId, t.text);
    keyboardInfo.setColour(juce::Label::textColourId, keyboardInfo.isEnabled() ? t.textDim : t.textDisabled);
}

void ControlBar::resized()
{
    const auto& t = themeOf(*this);
    const int h = juce::roundToInt(t.controlHeight);
    const int gap = juce::roundToInt(t.spacing);
    const int group = juce::roundToInt(t.spacing * 4.0f);
    auto area = getLocalBounds().reduced(juce::roundToInt(t.padding), 0).withSizeKeepingCentre(getWidth() - juce::roundToInt(t.padding * 2.0f), h);
    const auto place = [&](juce::Component& c, int width, int after) {
        c.setBounds(area.removeFromLeft(width));
        area.removeFromLeft(after);
    };
    place(tap, h * 2, gap);
    place(*tempo, h * 3, group);
    place(*signatureTop, h + h / 2, gap / 2);
    place(*signatureBottom, h + h / 2, group);
    place(*metronome, h * 2 + h / 2, gap);
    place(*quantization, h * 3, group);

    // The right end: the load meter, the mapping modes, the computer keyboard.
    load.setBounds(area.removeFromRight(h * 4));
    area.removeFromRight(group);
    keyMap.setBounds(area.removeFromRight(h * 2));
    area.removeFromRight(gap);
    midiMap.setBounds(area.removeFromRight(h * 2));
    area.removeFromRight(group);
    keyboardInfo.setBounds(area.removeFromRight(h * 5));
    keys.setBounds(area.removeFromRight(h * 2));

    // The transport in the middle of what's left.
    const int transportWidth = h * 4 + h * 3 + gap * 3;
    auto middle = area.withSizeKeepingCentre(juce::jmin(area.getWidth(), transportWidth), h);
    position.setBounds(middle.removeFromLeft(h * 4));
    middle.removeFromLeft(gap);
    for (juce::Component* c : {static_cast<juce::Component*>(&play), static_cast<juce::Component*>(&stop),
                               static_cast<juce::Component*>(&record)})
    {
        c->setBounds(middle.removeFromLeft(h));
        middle.removeFromLeft(gap);
    }
}

} // namespace lattice::ui
