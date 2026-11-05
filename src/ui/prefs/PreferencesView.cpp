#include "ui/prefs/PreferencesView.h"

#include "control/virtual/VirtualPush.h"
#include "engine/WorkerPool.h"
#include "ui/controllers/LearnWizard.h"
#include "ui/controllers/SurfaceManager.h"
#include "ui/prefs/AppSettings.h"
#include "ui/theme/LatticeLookAndFeel.h"

namespace lattice::ui
{
namespace
{
using Context = PreferencesView::Context;

/** A tab: a heading, then rows of a label and a control, top to bottom. */
class Page : public juce::Component
{
public:
    explicit Page(juce::String heading) : title(std::move(heading)) { setTitle(title); }

    void paint(juce::Graphics& g) override
    {
        const auto& t = themeOf(*this);
        g.setColour(t.text);
        g.setFont(juce::Font(t.font(t.fontLarge)).boldened());
        g.drawText(title, getLocalBounds().removeFromTop(headingHeight()), juce::Justification::centredLeft, true);
        g.setFont(t.font(t.fontNormal));
        for (const auto& [label, area] : labels)
        {
            g.setColour(t.textDim);
            g.drawFittedText(label, area, juce::Justification::centredLeft, 2);
        }
        for (const auto& [note, area] : notes)
        {
            g.setColour(t.textDim);
            g.setFont(t.font(t.fontSmall));
            g.drawFittedText(note, area, juce::Justification::topLeft, 6);
        }
    }

    void resized() override
    {
        const auto& t = themeOf(*this);
        auto area = getLocalBounds();
        area.removeFromTop(headingHeight() + juce::roundToInt(t.spacing * 2.0f));
        const int h = juce::roundToInt(t.controlHeight);
        const int labelWidth = juce::roundToInt(t.controlHeight * 8.0f);
        labels.clear();
        notes.clear();
        for (auto& row : rows)
        {
            if (row.control == nullptr) // a note
            {
                const int lines = juce::jmax(1, row.lines);
                const auto r = area.removeFromTop(juce::roundToInt((t.fontSmall + 3.0f) * static_cast<float>(lines)));
                notes.push_back({row.label, r});
                area.removeFromTop(juce::roundToInt(t.spacing));
                continue;
            }
            if (row.fill) // a list: the rest of the page
            {
                auto r = area;
                if (row.label.isNotEmpty())
                {
                    labels.push_back({row.label, r.removeFromTop(h)});
                    r.removeFromTop(juce::roundToInt(t.spacing));
                }
                row.control->setBounds(r);
                break;
            }
            const int height = row.height > 0 ? row.height : h;
            auto r = area.removeFromTop(height);
            area.removeFromTop(juce::roundToInt(t.spacing * 1.5f));
            if (row.label.isNotEmpty())
                labels.push_back({row.label, r.removeFromLeft(labelWidth).withHeight(h)});
            if (row.width > 0)
                r = r.removeFromLeft(juce::roundToInt(t.controlHeight * row.width));
            row.control->setBounds(r);
        }
    }

    void lookAndFeelChanged() override
    {
        std::function<void(juce::Component&)> recolour = [&](juce::Component& c) {
            if (auto* editor = dynamic_cast<juce::TextEditor*>(&c))
                editor->applyColourToAllText(themeOf(*this).text);
            for (auto* child : c.getChildren())
                recolour(*child);
        };
        recolour(*this);
        themeChanged();
        repaint();
    }

protected:
    virtual void themeChanged() {}

    /** A row: label, control (width in control heights; 0: the rest of the row). fill: the
        control takes the rest of the page (a list). */
    void row(const juce::String& label, juce::Component& control, float width = 0.0f, bool fill = false)
    {
        control.setTitle(label.isNotEmpty() ? label : control.getTitle());
        addAndMakeVisible(control);
        rows.push_back({label, &control, width, 0, fill, 0});
    }
    void note(const juce::String& text, int lines = 2) { rows.push_back({text, nullptr, 0.0f, 0, false, lines}); }
    int headingHeight() const { return juce::roundToInt(themeOf(*this).fontLarge * 1.6f); }

    struct Row
    {
        juce::String label;
        juce::Component* control = nullptr;
        float width = 0.0f;
        int height = 0;
        bool fill = false;
        int lines = 0;
    };
    juce::String title;
    std::vector<Row> rows;
    std::vector<std::pair<juce::String, juce::Rectangle<int>>> labels, notes;
};

void styleButton(juce::Button& b, std::function<void()> click)
{
    b.setWantsKeyboardFocus(true);
    b.onClick = std::move(click);
}

// --- Audio ---------------------------------------------------------------------------------------

class AudioPage final : public Page, private juce::Timer, private juce::ChangeListener
{
public:
    explicit AudioPage(Context& c) : Page("Audio"), context(c)
    {
        tone.setButtonText("Test Tone");
        tone.setToggleState(context.engine.testTone().isEnabled(), juce::dontSendNotification);
        styleButton(tone, [this] { context.engine.testTone().setEnabled(tone.getToggleState()); });
        frequency.setRange(20.0, 20000.0, 1.0);
        frequency.setSkewFactorFromMidPoint(1000.0);
        frequency.setTextValueSuffix(" Hz");
        frequency.setValue(context.settings.getDoubleValue(prefs::toneFrequencyKey, 440.0), juce::dontSendNotification);
        level.setRange(-60.0, 0.0, 0.5);
        level.setTextValueSuffix(" dB");
        level.setValue(context.settings.getDoubleValue(prefs::toneLevelKey, -12.0), juce::dontSendNotification);
        for (auto* s : {&frequency, &level})
        {
            s->setSliderStyle(juce::Slider::LinearHorizontal);
            s->setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, 20);
            s->onValueChange = [this] { applyTone(); };
        }
        readout.setReadOnly(true);
        readout.setMultiLine(true);
        resetStats.setButtonText("Reset");
        styleButton(resetStats, [this] {
            context.engine.callbackStats().reset();
            context.engine.inputLatency().reset();
        });
        context.engine.getDeviceManager().addChangeListener(this);
        build();
        startTimerHz(4);
    }
    ~AudioPage() override { context.engine.getDeviceManager().removeChangeListener(this); }

private:
    void build()
    {
        rows.clear();
        if (!context.engine.isOpening() && selector == nullptr)
        {
            // Inputs (none: Lattice has no audio tracks yet) and stereo outputs; MIDI is its own tab.
            selector = std::make_unique<juce::AudioDeviceSelectorComponent>(context.engine.getDeviceManager(), 0, 0, 1, 2, false, false, true, false);
            selector->setTitle("Audio device");
        }
        if (selector != nullptr)
        {
            addAndMakeVisible(*selector);
            rows.push_back({"", selector.get(), 0.0f, juce::roundToInt(themeOf(*this).controlHeight * 7.0f), false, 0});
        }
        else
            note("The audio device is opening...", 1);
        row("Latency", readout);
        rows.back().height = juce::roundToInt(themeOf(*this).fontNormal * 4.5f);
        row("", resetStats, 3.0f);
        row("", tone, 5.0f);
        row("Tone frequency", frequency);
        row("Tone level", level);
        resized();
    }

    void applyTone()
    {
        context.engine.testTone().setFrequency(static_cast<float>(frequency.getValue()));
        context.engine.testTone().setGain(juce::Decibels::decibelsToGain(static_cast<float>(level.getValue())));
        context.settings.setValue(prefs::toneFrequencyKey, frequency.getValue());
        context.settings.setValue(prefs::toneLevelKey, level.getValue());
    }

    void changeListenerCallback(juce::ChangeBroadcaster*) override
    {
        prefs::saveDeviceState(context.settings, context.engine); // the device changed: remember it
    }

    void timerCallback() override
    {
        if (selector == nullptr && !context.engine.isOpening())
            build();
        auto* device = context.engine.isOpening() ? nullptr : context.engine.getDeviceManager().getCurrentAudioDevice();
        juce::String text;
        if (device == nullptr)
            text = "No audio device open." + (context.audioError.isNotEmpty() ? "\n" + context.audioError : juce::String());
        else
        {
            const double sr = device->getCurrentSampleRate();
            const int buffer = device->getCurrentBufferSizeSamples();
            const auto ms = [sr](int samples) { return juce::String(1000.0 * samples / juce::jmax(1.0, sr), 1) + " ms"; };
            text << "Output: " << ms(device->getOutputLatencyInSamples() + buffer) << " (driver " << ms(device->getOutputLatencyInSamples())
                 << " + buffer " << ms(buffer) << ")\n";
            const auto midi = context.engine.inputLatency().summary();
            text << "MIDI to sound, measured: "
                 << (midi.count > 0 ? juce::String(midi.p50Ms, 1) + " ms median, " + juce::String(midi.p99Ms, 1) + " ms at worst (99%)"
                                    : juce::String("play a note to measure"))
                 << "\n";
            const auto load = context.engine.callbackStats().summary();
            text << "Audio load: " << juce::String(load.p50 * 100.0, 0) << "% median, " << juce::String(load.max * 100.0, 0) << "% at most, "
                 << static_cast<int>(load.overruns) << " late buffers";
        }
        if (readout.getText() != text)
            readout.setText(text, false);
    }

    Context& context;
    std::unique_ptr<juce::AudioDeviceSelectorComponent> selector;
    juce::TextEditor readout;
    juce::TextButton resetStats;
    juce::ToggleButton tone;
    juce::Slider frequency, level;
};

// --- MIDI ----------------------------------------------------------------------------------------

class MidiPage final : public Page
{
public:
    explicit MidiPage(Context& c) : Page("MIDI"), context(c)
    {
        list.setTitle("MIDI inputs");
        connection = juce::MidiDeviceListConnection::make([this] { rebuild(); });
        rebuild();
    }

    /** The switches, by input (for tests). */
    juce::ToggleButton* trackSwitch(const juce::String& identifier)
    {
        for (auto& r : inputRows)
            if (r->identifier == identifier)
                return &r->track;
        return nullptr;
    }
    juce::ToggleButton* remoteSwitch(const juce::String& identifier)
    {
        for (auto& r : inputRows)
            if (r->identifier == identifier)
                return &r->remote;
        return nullptr;
    }
    /** Tests: inputs other than the system's. */
    void rebuildWith(const juce::Array<juce::MidiDeviceInfo>& inputs)
    {
        devices = inputs;
        layOut();
    }

private:
    struct InputRow : juce::Component
    {
        juce::String identifier, name;
        juce::ToggleButton track{"Track"}, remote{"Remote"}, sync{"Sync"};
        InputRow()
        {
            for (auto* b : {&track, &remote, &sync})
                addAndMakeVisible(*b);
            sync.setEnabled(false);
            sync.setTooltip("Following an external MIDI clock comes in M6.");
            track.setTooltip("Notes from this input reach the armed tracks.");
            remote.setTooltip("MIDI Map (Ctrl+M) listens to this input.");
        }
        void paint(juce::Graphics& g) override
        {
            const auto& t = themeOf(*this);
            g.setColour(t.text);
            g.setFont(t.font(t.fontNormal));
            g.drawText(name, getLocalBounds().withRight(getWidth() / 2), juce::Justification::centredLeft, true);
        }
        void resized() override
        {
            auto r = getLocalBounds().withLeft(getWidth() / 2);
            const int w = r.getWidth() / 3;
            track.setBounds(r.removeFromLeft(w));
            remote.setBounds(r.removeFromLeft(w));
            sync.setBounds(r);
        }
    };

    void rebuild()
    {
        devices = juce::MidiInput::getAvailableDevices();
        layOut();
    }

    void layOut()
    {
        inputRows.clear();
        list.removeAllChildren();
        auto& manager = context.engine.getDeviceManager();
        const auto remotes = prefs::remoteInputs(context.settings);
        for (const auto& d : devices)
        {
            auto r = std::make_unique<InputRow>();
            r->identifier = d.identifier;
            r->name = d.name;
            r->setTitle(d.name);
            const bool open = !context.engine.isOpening() && manager.isMidiInputDeviceEnabled(d.identifier);
            r->track.setToggleState(open && !prefs::nonTrackInputs(context.settings).contains(d.identifier), juce::dontSendNotification);
            r->remote.setToggleState(remotes.contains(d.identifier), juce::dontSendNotification);
            // An input is open while either switch is on; with only Remote on, its notes don't
            // reach the tracks.
            const auto apply = [this, id = d.identifier, row = r.get()] {
                const bool track = row->track.getToggleState(), remote = row->remote.getToggleState();
                prefs::setRemoteInput(context.settings, id, remote);
                prefs::setNonTrackInput(context.settings, id, remote && !track);
                if (!context.engine.isOpening())
                {
                    context.engine.getDeviceManager().setMidiInputDeviceEnabled(id, track || remote);
                    if (const auto xml = context.engine.getDeviceManager().createStateXml())
                        context.settings.setValue(prefs::deviceStateKey, xml.get());
                }
                if (context.midiChanged)
                    context.midiChanged();
            };
            r->track.onClick = apply;
            r->remote.onClick = apply;
            if (context.engine.isSurfaceInput(d.identifier))
            {
                // A control surface's port: the surface decides what goes where.
                r->track.setEnabled(false);
                r->remote.setEnabled(false);
                r->track.setTooltip("A control surface uses this input (Controllers page): its notes reach the tracks only if it passes them on.");
                r->remote.setTooltip("A control surface uses this input: MIDI Map hears what the surface doesn't use.");
            }
            list.addAndMakeVisible(*r);
            inputRows.push_back(std::move(r));
        }
        rows.clear();
        if (devices.isEmpty())
            note("No MIDI inputs are connected. The computer keyboard and the on-screen keys still play the armed track.", 2);
        else
            note("Track: the input's notes reach the armed tracks. Remote: MIDI Map (Ctrl+M) listens to it. Sync (following an external clock) comes in M6.", 2);
        row("", list, 0.0f, true);
        resized();
    }

    void resized() override
    {
        Page::resized();
        const int h = juce::roundToInt(themeOf(*this).controlHeight * 1.2f);
        for (int i = 0; i < static_cast<int>(inputRows.size()); ++i)
            inputRows[static_cast<std::size_t>(i)]->setBounds(0, i * h, list.getWidth(), h);
    }

    Context& context;
    juce::Array<juce::MidiDeviceInfo> devices;
    juce::Component list;
    std::vector<std::unique_ptr<InputRow>> inputRows;
    juce::MidiDeviceListConnection connection;
};

// --- Control surfaces (part of the Controllers page) ---------------------------------------------

/** What a surface port sends, as it arrives: its bytes and what the surface makes of them. */
class SurfaceMonitorWindow final : public juce::DocumentWindow
{
public:
    SurfaceMonitorWindow(SurfaceManager& m, juce::Colour background, std::function<void()> closed)
        : juce::DocumentWindow("Surface Monitor", background, juce::DocumentWindow::closeButton), manager(m), onClosed(std::move(closed))
    {
        log.setMultiLine(true);
        log.setReadOnly(true);
        log.setTitle("Surface Monitor");
        log.setSize(560, 360);
        setContentNonOwned(&log, true);
        setResizable(true, false);
        setUsingNativeTitleBar(true);
        centreWithSize(getWidth(), getHeight());
        manager.setMonitor([this](const SurfaceManager::Seen& s) { add(s); });
        add("Press pads, buttons and knobs on a surface: each message shows here with what it is.");
    }
    ~SurfaceMonitorWindow() override { manager.setMonitor(nullptr); }
    void closeButtonPressed() override
    {
        if (onClosed)
            juce::MessageManager::callAsync(onClosed); // not from inside this window's own callback
    }

private:
    void add(const SurfaceManager::Seen& s)
    {
        const auto& entries = manager.entries();
        const auto name = s.entry >= 0 && s.entry < static_cast<int>(entries.size()) ? entries[static_cast<std::size_t>(s.entry)].definition : juce::String();
        juce::String bytes;
        for (int i = 0; i < s.message.getRawDataSize() && i < 12; ++i)
            bytes << juce::String::toHexString(s.message.getRawData()[i]).toUpperCase().paddedLeft('0', 2) << " ";
        add(name + "   " + bytes.trimEnd() + "   " + (s.meaning.isNotEmpty() ? s.meaning : juce::String("not the surface's: MIDI Map (and the tracks, if passed on)")));
    }
    void add(const juce::String& line)
    {
        lines.add(line);
        while (lines.size() > 300)
            lines.remove(0);
        log.setText(lines.joinIntoString("\n"), false);
        log.moveCaretToEnd();
    }

    SurfaceManager& manager;
    std::function<void()> onClosed;
    juce::TextEditor log;
    juce::StringArray lines;
};

/** The control surfaces: one row each (its definition, ports and state), and adding one. */
class SurfacesPanel final : public juce::Component
{
public:
    explicit SurfacesPanel(SurfaceManager& m) : manager(m)
    {
        setTitle("Control surfaces");
        add.setTitle("Add a control surface");
        add.onClick = [this] { chooseDefinition(); };
        folder.setTitle("Open the surface definitions folder");
        folder.onClick = [this] {
            const auto f = manager.userFolder();
            f.createDirectory();
            f.startAsProcess();
        };
        learnButton.setTitle("Learn a controller");
        learnButton.onClick = [this] { showLearn(); };
        reload.setTitle("Read the definitions again");
        reload.onClick = [this] {
            manager.reloadDefinitions();
            rebuild();
        };
        for (auto* b : {&add, &learnButton, &folder, &reload})
            addAndMakeVisible(*b);
        manager.onChanged = [this] { rebuild(); };
        rebuild();
    }
    ~SurfacesPanel() override { manager.onChanged = nullptr; }

    /** Tests: each surface's line. */
    int numRows() const noexcept { return static_cast<int>(rows.size()); }
    juce::String rowText(int i) const { return rows[static_cast<std::size_t>(i)]->text; }
    const juce::String& footerText() const noexcept { return footer; }

    void paint(juce::Graphics& g) override
    {
        const auto& t = themeOf(*this);
        g.setFont(t.font(t.fontSmall));
        g.setColour(t.textDim);
        g.drawFittedText(footer, footerArea, juce::Justification::topLeft, 6);
    }
    void resized() override
    {
        const auto& t = themeOf(*this);
        const int h = juce::roundToInt(t.controlHeight);
        auto r = getLocalBounds();
        auto top = r.removeFromTop(h);
        add.setBounds(top.removeFromLeft(h * 6));
        top.removeFromLeft(juce::roundToInt(t.spacing));
        learnButton.setBounds(top.removeFromLeft(h * 6));
        top.removeFromLeft(juce::roundToInt(t.spacing));
        folder.setBounds(top.removeFromLeft(h * 7));
        top.removeFromLeft(juce::roundToInt(t.spacing));
        reload.setBounds(top.removeFromLeft(h * 6));
        r.removeFromTop(juce::roundToInt(t.spacing));
        for (auto& row : rows)
            row->setBounds(r.removeFromTop(juce::roundToInt(static_cast<float>(h) * 1.2f)));
        footerArea = r.reduced(0, juce::roundToInt(t.spacing));
    }

private:
    struct Row final : juce::Component
    {
        juce::String text;
        juce::ToggleButton notes{"Notes to tracks"};
        juce::TextButton monitor{"Monitor"}, remove{"Remove"};
        Row()
        {
            notes.setTooltip("What the surface doesn't use of its notes also plays the armed tracks (a keyboard's keys).");
            for (auto* c : std::initializer_list<juce::Component*>{&notes, &monitor, &remove})
                addAndMakeVisible(*c);
        }
        void paint(juce::Graphics& g) override
        {
            const auto& t = themeOf(*this);
            g.setColour(t.text);
            g.setFont(t.font(t.fontNormal));
            g.drawFittedText(text, getLocalBounds().withRight(getWidth() - controlsWidth()), juce::Justification::centredLeft, 2);
        }
        int controlsWidth() const { return juce::roundToInt(themeOf(*this).controlHeight * 17.0f); }
        void resized() override
        {
            const int h = juce::roundToInt(themeOf(*this).controlHeight);
            auto r = getLocalBounds().removeFromRight(controlsWidth()).withSizeKeepingCentre(controlsWidth(), h);
            remove.setBounds(r.removeFromRight(h * 4));
            r.removeFromRight(h / 4);
            monitor.setBounds(r.removeFromRight(h * 4));
            r.removeFromRight(h / 4);
            notes.setBounds(r);
        }
    };

    void rebuild()
    {
        rows.clear();
        const auto& entries = manager.entries();
        for (int i = 0; i < static_cast<int>(entries.size()); ++i)
        {
            const auto& e = entries[static_cast<std::size_t>(i)];
            auto row = std::make_unique<Row>();
            const auto* d = manager.definition(e.definition);
            row->text = e.definition + (d == nullptr ? " (no such definition)" : !d->verified ? " (unverified)" : "") + "   " +
                        (manager.isConnected(i) ? juce::String("connected") : juce::String("not connected")) + "   " + e.input +
                        (e.output.isNotEmpty() && e.output != e.input ? " / " + e.output : juce::String());
            row->setTitle(row->text);
            row->notes.setToggleState(e.notesToTracks, juce::dontSendNotification);
            row->notes.onClick = [this, i, b = &row->notes] { manager.setNotesToTracks(i, b->getToggleState()); };
            row->monitor.onClick = [this] { showMonitor(); };
            // Not from inside the row's own button: removing rebuilds the rows.
            row->remove.onClick = [safe = juce::Component::SafePointer<SurfacesPanel>(this), i] {
                juce::MessageManager::callAsync([safe, i] {
                    if (safe != nullptr)
                        safe->manager.remove(i);
                });
            };
            addAndMakeVisible(*row);
            rows.push_back(std::move(row));
        }
        footer = entries.empty() ? "No control surfaces yet. A controller Lattice has a definition for is offered when it's connected; "
                                   "Add puts any definition on any ports (Keyboard + 8 knobs: a keyboard's knobs)."
                                 : "A surface's port plays the modes; what the surface doesn't use goes to MIDI Map (Ctrl+M). "
                                   "Definitions marked unverified haven't been tried on their device yet: Monitor shows what it sends.";
        for (const auto& p : manager.problems())
            footer << "\n" << p;
        resized();
        repaint();
    }

    void chooseDefinition()
    {
        juce::PopupMenu menu;
        const auto& all = manager.definitions();
        for (int i = 0; i < static_cast<int>(all.size()); ++i)
        {
            const auto& d = all[static_cast<std::size_t>(i)];
            const bool found = manager.portsFor(d).input.isNotEmpty();
            menu.addItem(i + 1, d.name + (found ? "  (connected now)" : juce::String()) + (d.verified ? juce::String() : "  (unverified)"));
        }
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&add), [safe = juce::Component::SafePointer<SurfacesPanel>(this)](int chosen) {
            if (safe == nullptr || chosen <= 0 || chosen > static_cast<int>(safe->manager.definitions().size()))
                return;
            const auto d = safe->manager.definitions()[static_cast<std::size_t>(chosen - 1)];
            const auto found = safe->manager.portsFor(d);
            if (found.input.isNotEmpty())
                safe->manager.add({d.name, found.input, found.output, d.grid.pads.empty()});
            else
                safe->choosePorts(d);
        });
    }

    /** No port matches the definition (a template, or a device named otherwise): choose them. */
    void choosePorts(const control::generic::SurfaceDefinition& d)
    {
        const auto inputs = juce::MidiInput::getAvailableDevices();
        if (inputs.isEmpty())
        {
            footer = "No MIDI inputs are connected, so there's nothing to put " + d.name + " on.";
            repaint();
            return;
        }
        juce::PopupMenu in;
        for (int i = 0; i < inputs.size(); ++i)
            in.addItem(i + 1, inputs[i].name);
        const auto name = d.name;
        const bool keys = d.grid.pads.empty();
        in.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&add), [safe = juce::Component::SafePointer<SurfacesPanel>(this), name, keys, inputs](int chosenIn) {
            if (safe == nullptr || chosenIn <= 0)
                return;
            const auto input = inputs[chosenIn - 1].name;
            const auto outputs = juce::MidiOutput::getAvailableDevices();
            juce::PopupMenu out;
            out.addItem(1, "No output (no lights)");
            for (int i = 0; i < outputs.size(); ++i)
                out.addItem(i + 2, outputs[i].name, true, outputs[i].name == input);
            out.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&safe->add), [safe, name, keys, input, outputs](int chosenOut) {
                if (safe == nullptr || chosenOut <= 0)
                    return;
                safe->manager.add({name, input, chosenOut >= 2 ? outputs[chosenOut - 2].name : juce::String(), keys});
            });
        });
    }

    void showLearn()
    {
        if (learnWindow == nullptr)
        {
            auto close = [safe = juce::Component::SafePointer<SurfacesPanel>(this)] {
                // Not from inside the wizard's own button.
                juce::MessageManager::callAsync([safe] {
                    if (safe != nullptr)
                        safe->learnWindow.reset();
                });
            };
            struct Window final : juce::DocumentWindow
            {
                Window(juce::Colour background, std::function<void()> closed)
                    : juce::DocumentWindow("Learn a Controller", background, juce::DocumentWindow::closeButton), onClosed(std::move(closed))
                {
                }
                void closeButtonPressed() override { onClosed(); }
                std::function<void()> onClosed;
            };
            learnWindow = std::make_unique<Window>(themeOf(*this).surface, close);
            learnWindow->setContentOwned(new LearnWizard(manager, close), true);
            learnWindow->setUsingNativeTitleBar(true);
            learnWindow->setResizable(true, false);
            learnWindow->centreWithSize(learnWindow->getWidth(), learnWindow->getHeight());
        }
        learnWindow->setVisible(true);
        learnWindow->toFront(true);
    }

    void showMonitor()
    {
        if (monitorWindow == nullptr)
            monitorWindow = std::make_unique<SurfaceMonitorWindow>(manager, themeOf(*this).surface, [safe = juce::Component::SafePointer<SurfacesPanel>(this)] {
                if (safe != nullptr)
                    safe->monitorWindow.reset();
            });
        monitorWindow->setVisible(true);
        monitorWindow->toFront(true);
    }

    SurfaceManager& manager;
    juce::TextButton add{"Add..."}, learnButton{"Learn..."}, folder{"Definitions Folder"}, reload{"Read Again"};
    std::vector<std::unique_ptr<Row>> rows;
    juce::String footer;
    juce::Rectangle<int> footerArea;
    std::unique_ptr<SurfaceMonitorWindow> monitorWindow;
    std::unique_ptr<juce::DocumentWindow> learnWindow;
};

// --- Controllers ---------------------------------------------------------------------------------

class ControllersPage final : public Page
{
public:
    explicit ControllersPage(Context& c) : Page("Controllers"), context(c)
    {
        rows.setTitle("Virtual Push rows");
        columns.setTitle("Virtual Push columns");
        for (int n = 4; n <= 16; ++n)
        {
            rows.addItem(juce::String(n), n);
            columns.addItem(juce::String(n), n);
        }
        rows.setSelectedId(juce::jlimit(4, 16, context.settings.getIntValue(prefs::virtualRowsKey, 8)), juce::dontSendNotification);
        columns.setSelectedId(juce::jlimit(4, 16, context.settings.getIntValue(prefs::virtualColumnsKey, 8)), juce::dontSendNotification);
        rows.onChange = [this] { save(prefs::virtualRowsKey, rows.getSelectedId()); };
        columns.onChange = [this] { save(prefs::virtualColumnsKey, columns.getSelectedId()); };
        encoders.setTitle("Virtual Push encoders");
        encoders.addItem("None", 1);
        for (const int n : {8, 16, 32})
            encoders.addItem(juce::String(n) + ", with a display", n + 1);
        encoders.setSelectedId(control::VirtualPush::encoderCountFor(context.settings.getIntValue(prefs::virtualEncodersKey, 8)) + 1,
                               juce::dontSendNotification);
        encoders.onChange = [this] { save(prefs::virtualEncodersKey, encoders.getSelectedId() - 1); };
        velocity.setTitle("Virtual Push pad velocity");
        velocity.addItem("From where the pad is clicked", 1);
        for (const int v : {32, 64, 100, 127})
            velocity.addItem("Always " + juce::String(v), v + 1);
        const int saved = context.settings.getIntValue(prefs::virtualVelocityKey, 0);
        velocity.setSelectedId(saved > 0 ? saved + 1 : 1, juce::dontSendNotification);
        if (velocity.getSelectedId() == 0)
            velocity.setSelectedId(1, juce::dontSendNotification);
        velocity.onChange = [this] { save(prefs::virtualVelocityKey, velocity.getSelectedId() - 1); };
        curve.setTitle("Velocity curve");
        for (const auto& [name, value] : {std::pair{"Softest", -100}, {"Soft", -50}, {"Linear", 0}, {"Hard", 50}, {"Hardest", 100}})
            curve.addItem(name, value + 101);
        curve.setSelectedId(juce::jlimit(-100, 100, context.settings.getIntValue(prefs::velocityCurveKey, 0)) + 101, juce::dontSendNotification);
        if (curve.getSelectedId() == 0)
            curve.setSelectedId(101, juce::dontSendNotification);
        curve.onChange = [this] { save(prefs::velocityCurveKey, curve.getSelectedId() - 101); };
        pressure.setTitle("Pad pressure");
        pressure.addItem("Off", 1);
        pressure.addItem("Each note its own (polyphonic)", 2);
        pressure.addItem("The whole channel", 3);
        pressure.setSelectedId(juce::jlimit(0, 2, context.settings.getIntValue(prefs::pressureKey, 1)) + 1, juce::dontSendNotification);
        pressure.onChange = [this] { save(prefs::pressureKey, pressure.getSelectedId() - 1); };
        threshold.setTitle("Pressure threshold");
        for (const int pct : {0, 10, 20, 30, 50})
            threshold.addItem(pct == 0 ? juce::String("None") : juce::String(pct) + "%", pct + 1);
        threshold.setSelectedId(context.settings.getIntValue(prefs::pressureThresholdKey, 0) + 1, juce::dontSendNotification);
        if (threshold.getSelectedId() == 0)
            threshold.setSelectedId(1, juce::dontSendNotification);
        threshold.onChange = [this] { save(prefs::pressureThresholdKey, threshold.getSelectedId() - 1); };
        strip.setTitle("Touch strip");
        strip.addItem("Pitch bend (springs back)", 1);
        strip.addItem("Mod wheel", 2);
        strip.addItem("Scroll the Note mode pads", 3);
        strip.setSelectedId(juce::jlimit(0, 2, context.settings.getIntValue(prefs::stripKey, 0)) + 1, juce::dontSendNotification);
        strip.onChange = [this] { save(prefs::stripKey, strip.getSelectedId() - 1); };

        row("Virtual Push rows", rows, 5.0f);
        row("Virtual Push columns", columns, 5.0f);
        row("Virtual Push encoders", encoders, 10.0f);
        row("Pad velocity", velocity, 14.0f);
        row("Velocity curve", curve, 8.0f);
        row("Pad pressure", pressure, 14.0f);
        row("Pressure threshold", threshold, 5.0f);
        row("Touch strip", strip, 14.0f);
        note("Virtual Push (View menu, Ctrl+Alt+P) plays the session from the screen. Session mode: pads launch clips. Note mode: "
             "pads play the selected track in its tuning (Colundi's table, or In Key and Chromatic for 12-TET); Scale opens the "
             "layout menu. Right-click latches a pad or a button; drag up on a held pad to press it harder.",
             3);
        note("Its encoders are the display's cells: drag one up or down (Shift: finely) or use the mouse wheel. Device mode: the "
             "selected device's parameters, the buttons above choosing the device and those below its pages. Mix mode: the volume, "
             "pan or sends of the tracks in view.",
             3);
        if (context.surfaces != nullptr)
        {
            surfaces = std::make_unique<SurfacesPanel>(*context.surfaces);
            row("Control surfaces", *surfaces, 0.0f, true);
        }
        else
            note("Control surfaces start with the app's window.", 1);
    }

    /** Tests: the surfaces list (null without a surface manager). */
    SurfacesPanel* surfacesPanel() noexcept { return surfaces.get(); }

private:
    void save(const char* key, int value)
    {
        context.settings.setValue(key, value);
        if (context.controllersChanged)
            context.controllersChanged();
    }

    Context& context;
    juce::ComboBox rows, columns, encoders, velocity, curve, pressure, threshold, strip;
    std::unique_ptr<SurfacesPanel> surfaces;
};

// --- Look & Feel ---------------------------------------------------------------------------------

class LookPage final : public Page
{
public:
    explicit LookPage(Context& c) : Page("Look & Feel"), context(c)
    {
        themeBox.setTitle("Theme");
        for (const auto* t : {&Theme::dark(), &Theme::light(), &Theme::highContrast()})
            names.add(t->name);
        for (const auto& f : context.themesFolder.findChildFiles(juce::File::findFiles, false, "*.json"))
        {
            names.add(f.getFileNameWithoutExtension());
            files.set(f.getFileNameWithoutExtension(), f.getFullPathName());
        }
        themeBox.addItemList(names, 1);
        themeBox.setText(context.theme ? context.theme().name : juce::String(), juce::dontSendNotification);
        themeBox.onChange = [this] { chooseTheme(themeBox.getText()); };
        row("Theme", themeBox, 10.0f);

        zoom.setTitle("Zoom");
        zoom.setRange(prefs::minZoom * 100.0, prefs::maxZoom * 100.0, 10.0);
        zoom.setTextValueSuffix(" %");
        zoom.setSliderStyle(juce::Slider::LinearHorizontal);
        zoom.setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, 20);
        zoom.setValue(prefs::zoom(context.settings) * 100.0, juce::dontSendNotification);
        // Applied when the drag ends: every window lays itself out again at the new size.
        zoom.onDragEnd = [this] { setZoom(zoom.getValue() / 100.0); };
        zoom.onValueChange = [this] {
            if (!zoom.isMouseButtonDown())
                setZoom(zoom.getValue() / 100.0);
        };
        row("Zoom", zoom, 14.0f);

        openFolder.setButtonText("Open Themes Folder");
        styleButton(openFolder, [this] {
            context.themesFolder.createDirectory();
            context.themesFolder.revealToUser();
        });
        row("", openFolder, 7.0f);
        note("A theme is a JSON file in the Themes folder: { \"name\": \"Mine\", \"base\": \"Dark\", \"colours\": { \"accent\": \"#f2a33a\" } }. "
             "Any colour or size not in it comes from the base theme.",
             3);
    }

    void setZoom(double z)
    {
        context.settings.setValue(prefs::zoomKey, z);
        prefs::applyZoom(z);
    }

    void chooseTheme(const juce::String& name)
    {
        if (const auto* builtIn = Theme::builtIn(name))
        {
            context.settings.removeValue(prefs::userThemeKey);
            if (context.setTheme)
                context.setTheme(*builtIn);
            return;
        }
        const juce::File file(files[name]);
        juce::StringArray problems;
        auto theme = Theme::fromJson(juce::JSON::parse(file), problems);
        if (theme.name.isEmpty())
            theme.name = name;
        context.settings.setValue(prefs::userThemeKey, file.getFullPathName());
        if (context.setTheme)
            context.setTheme(theme);
    }

private:
    Context& context;
    juce::StringArray names;
    juce::StringPairArray files;
    juce::ComboBox themeBox;
    juce::Slider zoom;
    juce::TextButton openFolder;
};

// --- Library -------------------------------------------------------------------------------------

class LibraryPage final : public Page, private juce::ChangeListener
{
public:
    explicit LibraryPage(Context& c) : Page("Library"), context(c)
    {
        add.setButtonText("Add Folder...");
        styleButton(add, [this] {
            if (context.chooseFolder)
                context.chooseFolder([this](const juce::File& folder) {
                    context.library.addPlace(folder);
                    refresh();
                });
        });
        remove.setButtonText("Remove");
        styleButton(remove, [this] {
            const auto now = context.library.places();
            const int i = list.getSelectedRow();
            if (i > 0 && i < now.size()) // not the User Library
                context.library.removePlace(now[i]);
            refresh();
        });
        rescan.setButtonText("Rescan All");
        styleButton(rescan, [this] { context.library.rescanAll(); });
        list.setTitle("Places");
        places = context.library.places();
        model.page = this; // before the list asks it anything
        list.setModel(&model);
        note("The browser indexes these folders and watches them, so new files show up by themselves. The User Library is where "
             "presets and tunings you save go; it can't be removed.",
             2);
        row("", add, 5.0f);
        row("", remove, 5.0f);
        row("", rescan, 5.0f);
        row("", list, 0.0f, true);
        context.library.addChangeListener(this);
        refresh();
    }
    ~LibraryPage() override { context.library.removeChangeListener(this); }

    void refresh()
    {
        places = context.library.places();
        list.updateContent();
        list.repaint();
    }
    const juce::Array<juce::File>& shownPlaces() const noexcept { return places; }

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override { refresh(); }

    struct Model : juce::ListBoxModel
    {
        LibraryPage* page = nullptr;
        int getNumRows() override { return page->places.size(); }
        void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override
        {
            const auto& t = themeOf(page->list);
            if (selected)
                g.fillAll(t.selection.withAlpha(0.4f));
            g.setColour(t.text);
            g.setFont(t.font(t.fontNormal));
            const auto& f = page->places[row];
            g.drawText((row == 0 ? "User Library: " : juce::String()) + f.getFullPathName(), 6, 0, width - 12, height,
                       juce::Justification::centredLeft, true);
        }
    };

    Context& context;
    juce::Array<juce::File> places;
    Model model;
    juce::ListBox list;
    juce::TextButton add, remove, rescan;
};

// --- CPU -----------------------------------------------------------------------------------------

class CpuPage final : public Page
{
public:
    explicit CpuPage(Context& c) : Page("CPU"), context(c)
    {
        multi.setButtonText("Multi-threaded audio");
        multi.setToggleState(context.settings.getBoolValue(prefs::multiThreadedKey, true), juce::dontSendNotification);
        styleButton(multi, [this] {
            context.settings.setValue(prefs::multiThreadedKey, multi.getToggleState());
            apply();
        });
        const int cores = juce::SystemStats::getNumCpus();
        workers.setTitle("Worker Threads");
        workers.addItem("One per core (" + juce::String(WorkerPool::defaultWorkers()) + ")", 1);
        for (int n = 1; n <= cores; ++n)
            workers.addItem(juce::String(n), n + 1);
        workers.setSelectedId(context.settings.getIntValue(prefs::workersKey, 0) + 1, juce::dontSendNotification);
        workers.onChange = [this] {
            context.settings.setValue(prefs::workersKey, workers.getSelectedId() - 1);
            apply();
        };
        row("", multi, 10.0f);
        row("Worker threads", workers, 8.0f);
        note("Tracks are processed in parallel on worker threads beside the audio thread. Fewer leaves more of the computer to other "
             "programs; one per physical core is usually best.",
             2);
        refreshEnabled();
    }

private:
    void apply()
    {
        context.engine.setWorkerThreads(prefs::workerThreads(context.settings));
        refreshEnabled();
    }
    void refreshEnabled() { workers.setEnabled(multi.getToggleState()); }

    Context& context;
    juce::ToggleButton multi;
    juce::ComboBox workers;
};

// --- Tuning --------------------------------------------------------------------------------------

class TuningPage final : public Page
{
public:
    explicit TuningPage(Context& c) : Page("Tuning"), context(c)
    {
        tuning.setTitle("New tracks' tuning");
        const auto current = context.settings.getValue(prefs::defaultTuningKey);
        int id = 1;
        for (const auto& t : context.tunings.getAll())
        {
            tuning.addItem(t->name, id);
            ids.add(t->id);
            if (t->id == current || (current.isEmpty() && t == context.tunings.defaultTuning()))
                tuning.setSelectedId(id, juce::dontSendNotification);
            ++id;
        }
        tuning.onChange = [this] {
            const auto chosen = ids[tuning.getSelectedId() - 1];
            context.settings.setValue(prefs::defaultTuningKey, chosen);
            prefs::applyNewTrackDefaults(context.settings);
        };
        highNotes.setTitle("New tracks' high notes");
        highNotes.addItem("Mute", 1);
        highNotes.addItem("Fold Down", 2);
        highNotes.setSelectedId(context.settings.getIntValue(prefs::defaultHighNotesKey, 0) + 1, juce::dontSendNotification);
        highNotes.onChange = [this] {
            context.settings.setValue(prefs::defaultHighNotesKey, highNotes.getSelectedId() - 1);
            prefs::applyNewTrackDefaults(context.settings);
        };
        row("New tracks' tuning", tuning, 10.0f);
        row("New tracks' high notes", highNotes, 10.0f);
        note("For tracks made from now on (and new sets). A track's own tuning is changed in its header, the mixer or the Tuning view.", 2);
    }

    juce::ComboBox& tuningBox() noexcept { return tuning; }
    juce::ComboBox& highNotesBox() noexcept { return highNotes; }

private:
    Context& context;
    juce::StringArray ids;
    juce::ComboBox tuning, highNotes;
};

// --- Computer keyboard ---------------------------------------------------------------------------

class KeyboardPage final : public Page
{
public:
    explicit KeyboardPage(Context& c) : Page("Computer Keyboard"), context(c)
    {
        keys.setTitle("Note keys");
        keys.setText(context.settings.getValue(prefs::noteKeysKey, ComputerKeyboardInput::noteKeys), false);
        keys.setInputRestrictions(ComputerKeyboardInput::numNoteKeys);
        keys.onTextChange = [this] { applyKeys(); };
        reset.setButtonText("Default");
        styleButton(reset, [this] {
            keys.setText(ComputerKeyboardInput::noteKeys, false);
            applyKeys();
        });
        row("Note keys (lowest first)", keys, 10.0f);
        row("", reset, 4.0f);
        note("Seventeen different keys play notes from the range's first. Z and X move the range by its step, C and V change the velocity, "
             "M turns the keyboard on and off, so those can't be note keys.",
             2);
        pages.setTitle("Range steps");
        for (const auto& t : context.tunings.getAll())
            tunings.push_back(t);
        model.page = this; // before the list asks it anything
        pages.setModel(&model);
        row("Range step (Z, X) for each tuning", pages, 0.0f, true);
    }

    bool keysValid() const { return ComputerKeyboardInput::isValidNoteKeys(keys.getText()); }
    void themeChanged() override
    {
        const bool ok = keysValid();
        keys.applyColourToAllText(ok ? themeOf(*this).text : themeOf(*this).error);
    }
    juce::TextEditor& keysEditor() noexcept { return keys; }
    void setStep(const juce::String& tuningId, int step)
    {
        context.settings.setValue(prefs::pageKeyPrefix + tuningId, step);
        if (context.keyboardChanged)
            context.keyboardChanged();
        pages.repaint();
    }

private:
    void applyKeys()
    {
        const bool ok = keysValid();
        keys.setColour(juce::TextEditor::textColourId, ok ? themeOf(*this).text : themeOf(*this).error);
        keys.applyColourToAllText(ok ? themeOf(*this).text : themeOf(*this).error);
        if (!ok)
            return;
        context.settings.setValue(prefs::noteKeysKey, keys.getText().toLowerCase());
        if (context.keyboardChanged)
            context.keyboardChanged();
    }

    /** One row per tuning: its name, and a menu of steps (its own, or 1 to 24). */
    struct Model : juce::ListBoxModel
    {
        KeyboardPage* page = nullptr;
        int getNumRows() override { return static_cast<int>(page->tunings.size()); }
        void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override
        {
            const auto& t = themeOf(page->pages);
            if (selected)
                g.fillAll(t.selection.withAlpha(0.4f));
            const auto& tuning = page->tunings[static_cast<std::size_t>(row)];
            const int step = prefs::pageSizeFor(page->context.settings, tuning->id);
            g.setColour(t.text);
            g.setFont(t.font(t.fontNormal));
            g.drawText(tuning->name, 6, 0, width / 2, height, juce::Justification::centredLeft, true);
            g.setColour(step > 0 ? t.accent : t.textDim);
            g.drawText(step > 0 ? juce::String(step) + " keys" : "Its own (" + juce::String(tuning->keyboardPageSize) + " keys)", width / 2, 0,
                       width / 2 - 6, height, juce::Justification::centredLeft, true);
        }
        void listBoxItemClicked(int row, const juce::MouseEvent&) override
        {
            const auto tuning = page->tunings[static_cast<std::size_t>(row)];
            juce::PopupMenu menu;
            const int step = prefs::pageSizeFor(page->context.settings, tuning->id);
            menu.addItem("Its own (" + juce::String(tuning->keyboardPageSize) + ")", true, step == 0,
                         [p = page, id = tuning->id] { p->setStep(id, 0); });
            for (int n = 1; n <= 24; ++n)
                menu.addItem(juce::String(n), true, step == n, [p = page, id = tuning->id, n] { p->setStep(id, n); });
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&page->pages));
        }
    };

    Context& context;
    juce::TextEditor keys;
    juce::TextButton reset;
    juce::ListBox pages;
    Model model;
    std::vector<TuningPtr> tunings;
};

// --- Shortcuts -----------------------------------------------------------------------------------

class ShortcutsPage final : public Page
{
public:
    explicit ShortcutsPage(Context& c) : Page("Shortcuts"), context(c), editor(c.commands.keyMappings(), true)
    {
        editor.setTitle("Shortcuts");
        importBtn.setButtonText("Import...");
        exportBtn.setButtonText("Export...");
        styleButton(importBtn, [this] {
            if (context.chooseFile)
                context.chooseFile("Import shortcuts", "*.xml", false, [this](const juce::File& f) { importFrom(f); });
        });
        styleButton(exportBtn, [this] {
            if (context.chooseFile)
                context.chooseFile("Export shortcuts", "*.xml", true, [this](const juce::File& f) { exportTo(f); });
        });
        themeChanged();
        note("Click + beside a command and press a key to add it; a key another command has is asked about first. "
             "Reset to Defaults takes back every change.",
             2);
        row("", importBtn, 4.0f);
        row("", exportBtn, 4.0f);
        row("", editor, 0.0f, true);
    }

    void themeChanged() override
    {
        // JUCE's editor draws its rows in its own colours (black text by default).
        const auto& t = themeOf(*this);
        editor.setColours(t.surfaceSunken, t.text);
    }

    bool exportTo(const juce::File& file)
    {
        const auto xml = context.commands.shortcutsToXml();
        return xml != nullptr && xml->writeTo(file.withFileExtension("xml"));
    }
    bool importFrom(const juce::File& file)
    {
        const auto xml = juce::XmlDocument::parse(file);
        if (xml == nullptr)
            return false;
        context.commands.resetShortcutsToDefaults();
        context.commands.restoreShortcuts(*xml);
        return true;
    }

private:
    Context& context;
    juce::KeyMappingEditorComponent editor;
    juce::TextButton importBtn, exportBtn;
};
} // namespace

juce::String PreferencesView::pageName(Page p)
{
    switch (p)
    {
    case Page::audio: return "Audio";
    case Page::midi: return "MIDI";
    case Page::controllers: return "Controllers";
    case Page::look: return "Look & Feel";
    case Page::library: return "Library";
    case Page::cpu: return "CPU";
    case Page::tuning: return "Tuning";
    case Page::keyboard: return "Computer Keyboard";
    case Page::shortcuts: return "Shortcuts";
    }
    return {};
}

PreferencesView::PreferencesView(Context c) : context(std::move(c))
{
    setTitle("Preferences");
    setWantsKeyboardFocus(true);
    show(static_cast<Page>(juce::jlimit(0, numPages - 1, context.settings.getIntValue(lastPageKey, 0))));
}

PreferencesView::~PreferencesView() = default;

void PreferencesView::show(Page p)
{
    shown = p;
    context.settings.setValue(lastPageKey, static_cast<int>(p));
    if (current != nullptr)
        removeChildComponent(current.get());
    switch (p)
    {
    case Page::audio: current = std::make_unique<AudioPage>(context); break;
    case Page::midi: current = std::make_unique<MidiPage>(context); break;
    case Page::controllers: current = std::make_unique<ControllersPage>(context); break;
    case Page::look: current = std::make_unique<LookPage>(context); break;
    case Page::library: current = std::make_unique<LibraryPage>(context); break;
    case Page::cpu: current = std::make_unique<CpuPage>(context); break;
    case Page::tuning: current = std::make_unique<TuningPage>(context); break;
    case Page::keyboard: current = std::make_unique<KeyboardPage>(context); break;
    case Page::shortcuts: current = std::make_unique<ShortcutsPage>(context); break;
    }
    addAndMakeVisible(*current);
    current->sendLookAndFeelChange(); // made before it was in the window: its text takes the theme now
    resized();
    repaint();
}

void PreferencesView::resized()
{
    const auto& t = themeOf(*this);
    auto area = getLocalBounds();
    tabs = area.removeFromLeft(juce::roundToInt(t.controlHeight * 8.0f));
    if (current != nullptr)
        current->setBounds(area.reduced(juce::roundToInt(t.padding * 2.0f)));
}

void PreferencesView::paint(juce::Graphics& g)
{
    const auto& t = themeOf(*this);
    g.fillAll(t.surface);
    g.setColour(t.surfaceRaised);
    g.fillRect(tabs);
    const int h = juce::roundToInt(t.controlHeight * 1.3f);
    g.setFont(t.font(t.fontNormal));
    for (int i = 0; i < numPages; ++i)
    {
        const auto r = juce::Rectangle<int>(tabs.getX(), tabs.getY() + i * h, tabs.getWidth(), h);
        const bool on = static_cast<int>(shown) == i;
        if (on)
        {
            g.setColour(t.surface);
            g.fillRect(r);
            g.setColour(t.accent);
            g.fillRect(r.withWidth(juce::jmax(2, juce::roundToInt(t.stroke * 2.0f))));
        }
        g.setColour(on ? t.text : t.textDim);
        g.drawText(pageName(static_cast<Page>(i)), r.reduced(juce::roundToInt(t.padding * 1.5f), 0), juce::Justification::centredLeft, true);
    }
}

void PreferencesView::mouseDown(const juce::MouseEvent& e)
{
    if (!tabs.contains(e.getPosition()))
        return;
    const int i = (e.y - tabs.getY()) / juce::roundToInt(themeOf(*this).controlHeight * 1.3f);
    if (i >= 0 && i < numPages)
        show(static_cast<Page>(i));
}

} // namespace lattice::ui
