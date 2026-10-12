#include "app/debug/TransportBar.h"

#include "engine/Transport.h"

namespace lattice::debugui
{

TransportBar::TransportBar(SessionController& s) : session(s)
{
    for (auto* b : {&play, &stop, &record, &tap, &metronome, &undo, &redo, &newSet, &open, &save, &saveAs,
                    &addTrack, &addScene, &importMidi})
    {
        b->setWantsKeyboardFocus(false);
        addAndMakeVisible(*b);
    }
    for (auto* c : {&sigNum, &sigDen, &quantization, &fixedLength})
    {
        c->setWantsKeyboardFocus(false);
        addAndMakeVisible(*c);
    }

    play.setTooltip("Play (Space)");
    play.setColour(juce::TextButton::buttonOnColourId, colours::play.darker(0.3f));
    play.onClick = [this] {
        session.play();
    };
    stop.setTooltip("Stop (Space); stop again to return to bar 1");
    stop.onClick = [this] {
        session.stop();
    };
    record.setTooltip("Session record (F9)");
    record.setColour(juce::TextButton::buttonOnColourId, colours::record);
    record.onClick = [this] {
        session.setSessionRecord(!session.isSessionRecordOn(), selectedScene ? selectedScene() : 0);
    };

    tempo.setRange(LiveSet::minTempo, LiveSet::maxTempo, 0.01);
    tempo.setTextValueSuffix(" BPM");
    tempo.setWantsKeyboardFocus(false);
    tempo.onDragStart = [this] {
        session.undoManager().beginNewTransaction("Tempo");
    };
    tempo.onValueChange = [this] {
        session.set().setTempo(tempo.getValue());
    };
    addAndMakeVisible(tempo);
    tap.onClick = [this] {
        session.tapTempo();
    };

    for (int n = 1; n <= 16; ++n)
        sigNum.addItem(juce::String(n), n);
    for (const int d : {2, 4, 8, 16})
        sigDen.addItem(juce::String(d), d);
    const auto setSignature = [this] {
        session.undoManager().beginNewTransaction("Time signature");
        session.set().setTimeSignature(sigNum.getSelectedId(), sigDen.getSelectedId());
    };
    sigNum.onChange = setSignature;
    sigDen.onChange = setSignature;

    for (int q = 0; q < numQuantizationValues; ++q)
        quantization.addItem(quantizationName(static_cast<Quantization>(q)), q + 1);
    quantization.setTooltip("Global launch quantization");
    quantization.onChange = [this] {
        session.undoManager().beginNewTransaction("Quantization");
        session.set().setGlobalQuantization(static_cast<Quantization>(quantization.getSelectedId() - 1));
    };

    metronome.setColour(juce::TextButton::buttonOnColourId, colours::solo);
    metronome.onClick = [this] {
        session.undoManager().beginNewTransaction("Metronome");
        session.set().setMetronomeOn(!session.set().metronomeOn());
    };

    fixedLength.addItem("Rec: open", 100);
    for (const int bars : {1, 2, 4, 8, 16})
        fixedLength.addItem("Rec: " + juce::String(bars) + (bars == 1 ? " bar" : " bars"), bars);
    fixedLength.setTooltip("Fixed recording length");
    fixedLength.onChange = [this] {
        session.undoManager().beginNewTransaction("Fixed length");
        const int id = fixedLength.getSelectedId();
        session.set().setFixedRecordBars(id == 100 ? 0 : id);
    };

    position.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 16.0f, juce::Font::bold));
    position.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(position);

    undo.onClick = [this] {
        session.undoManager().undo();
    };
    redo.onClick = [this] {
        session.undoManager().redo();
    };
    newSet.onClick = [this] {
        if (onNew)
            onNew();
    };
    open.onClick = [this] {
        if (onOpen)
            onOpen();
    };
    save.onClick = [this] {
        if (onSave)
            onSave();
    };
    saveAs.onClick = [this] {
        if (onSaveAs)
            onSaveAs();
    };
    importMidi.onClick = [this] {
        if (onImportMidi)
            onImportMidi();
    };
    addTrack.onClick = [this] {
        session.undoManager().beginNewTransaction("Add track");
        session.set().addTrack();
    };
    addScene.onClick = [this] {
        session.undoManager().beginNewTransaction("Add scene");
        session.set().addScene();
    };

    refresh();
    update();
}

void TransportBar::refresh()
{
    const auto& set = session.set();
    tempo.setValue(set.tempo(), juce::dontSendNotification);
    sigNum.setSelectedId(set.timeSigNumerator(), juce::dontSendNotification);
    sigDen.setSelectedId(set.timeSigDenominator(), juce::dontSendNotification);
    quantization.setSelectedId(static_cast<int>(set.globalQuantization()) + 1, juce::dontSendNotification);
    metronome.setToggleState(set.metronomeOn(), juce::dontSendNotification);
    fixedLength.setSelectedId(set.fixedRecordBars() == 0 ? 100 : set.fixedRecordBars(),
                              juce::dontSendNotification);
    undo.setEnabled(session.undoManager().canUndo());
    redo.setEnabled(session.undoManager().canRedo());
}

void TransportBar::update()
{
    const auto t = session.transport();
    play.setToggleState(t.playing, juce::dontSendNotification);
    record.setToggleState(t.sessionRecord, juce::dontSendNotification);

    const auto p = Transport::positionOf(t.beat, t.barOrigin, t.barOriginIndex, t.timeSigNum, t.timeSigDen);
    const auto text = juce::String(p.bar) + "." + juce::String(p.beat) + "." + juce::String(p.sixteenth);
    if (text != shownPosition)
    {
        shownPosition = text;
        position.setText(text, juce::dontSendNotification);
    }
    // A scene can change the tempo while playing.
    if (!tempo.isMouseButtonDown() && tempo.getValue() != session.set().tempo())
        tempo.setValue(session.set().tempo(), juce::dontSendNotification);
    undo.setEnabled(session.undoManager().canUndo());
    redo.setEnabled(session.undoManager().canRedo());
}

void TransportBar::paint(juce::Graphics& g)
{
    g.fillAll(colours::panel);
}

void TransportBar::resized()
{
    auto area = getLocalBounds().reduced(6, 4);
    auto top = area.removeFromTop(28);
    area.removeFromTop(4);
    auto bottom = area;

    const auto place = [](juce::Rectangle<int>& row, juce::Component& c, int width, int gap = 4) {
        c.setBounds(row.removeFromLeft(width));
        row.removeFromLeft(gap);
    };
    place(top, play, 36);
    place(top, stop, 36);
    place(top, record, 36, 12);
    place(top, tempo, 120);
    place(top, tap, 40, 12);
    place(top, sigNum, 52, 2);
    place(top, sigDen, 52, 12);
    place(top, quantization, 80);
    place(top, metronome, 56);
    place(top, fixedLength, 100, 12);
    place(top, position, 100);

    place(bottom, undo, 56);
    place(bottom, redo, 56, 16);
    place(bottom, newSet, 50);
    place(bottom, open, 64);
    place(bottom, save, 50);
    place(bottom, saveAs, 76, 16);
    place(bottom, addTrack, 70);
    place(bottom, addScene, 70);
    place(bottom, importMidi, 100);
}

} // namespace lattice::debugui
