#include "ui/prefs/PreferencesView.h"

#include "engine/WorkerPool.h"
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
        frequency.setValue(context.settings.getDoubleValue("testToneFrequency", 440.0), juce::dontSendNotification);
        level.setRange(-60.0, 0.0, 0.5);
        level.setTextValueSuffix(" dB");
        level.setValue(context.settings.getDoubleValue("testToneLevelDb", -12.0), juce::dontSendNotification);
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
        context.settings.setValue("testToneFrequency", frequency.getValue());
        context.settings.setValue("testToneLevelDb", level.getValue());
    }

    void changeListenerCallback(juce::ChangeBroadcaster*) override
    {
        // The device changed: remember it (as the debug UI's Audio tab did), unless still opening.
        if (context.engine.isOpening())
            return;
        if (const auto xml = context.engine.getDeviceManager().createStateXml())
            context.settings.setValue("audioDeviceState", xml.get());
    }

    void timerCallback() override
    {
        if (selector == nullptr && !context.engine.isOpening())
            build();
        auto* device = context.engine.isOpening() ? nullptr : context.engine.getDeviceManager().getCurrentAudioDevice();
        juce::String text;
        if (device == nullptr)
            text = "No audio device open.";
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
                        context.settings.setValue("audioDeviceState", xml.get());
                }
                if (context.midiChanged)
                    context.midiChanged();
            };
            r->track.onClick = apply;
            r->remote.onClick = apply;
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

// --- Controllers ---------------------------------------------------------------------------------

class ControllersPage final : public Page
{
public:
    ControllersPage() : Page("Controllers")
    {
        note("Ableton Push 2 and 3 (with their displays) and Maschine MK3 come in M5. Until then, any MIDI controller can play "
             "tracks (MIDI tab: Track) and control anything with MIDI Map (Ctrl+M; MIDI tab: Remote).",
             4);
    }
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
            const auto places = context.library.places();
            const int i = list.getSelectedRow();
            if (i > 0 && i < places.size()) // not the User Library
                context.library.removePlace(places[i]);
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
    case Page::controllers: current = std::make_unique<ControllersPage>(); break;
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
