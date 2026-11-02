// The MIDI effects' panels (docs/05-m4-ui.md, 4d): Arpeggiator, and Chord, which shows the notes it
// makes as table entries with their Hz in the track's tuning.

#include "ui/device/DevicePanels.h"
#include "ui/device/panels/PanelKit.h"
#include "ui/theme/LatticeLookAndFeel.h"

namespace lattice::ui::panels
{
namespace
{
class ArpeggiatorPanel final : public PanelRow
{
public:
    ArpeggiatorPanel(DeviceContext& c, const juce::ValueTree& d) : PanelRow(c, d)
    {
        addSection("Arpeggiator", {"arp.style", "arp.rate", "arp.gate", "arp.range", "arp.distance", "arp.transpose"});
        addSection("Retrigger", {"arp.retrigger", "arp.retriggerRate", "arp.hold"});
    }
};

/** The notes a chord makes from the note the track played last (or the table's middle): each as
    its table entry and its frequency in the track's tuning, so Colundi chords read as what they are. */
class ChordNotes final : public DeviceDisplay, private EngineStateFeed::Listener
{
public:
    ChordNotes(DeviceContext& c, const juce::ValueTree& d) : context(c), device(d), mirror(c.specs, d)
    {
        setTitle("Chord notes");
        setDescription("The notes the chord makes from the last note played, as table entries with their frequencies.");
        mirror.onChange = [this] { invalidate(); };
        context.feed.addListener(this, EngineStateFeed::notesAspect);
    }
    ~ChordNotes() override { context.feed.removeListener(this); }

    /** The note the chord is shown on. */
    int rootNote() const
    {
        const auto owner = LiveSet::chainOwnerOf(device);
        const int track = owner.hasType(ids::track) ? context.set().trackIndexForId(LiveSet::idOf(owner)) : -1;
        const int last = track >= 0 ? context.feed.lastNote(track) : -1;
        return last >= 0 ? last : 60;
    }

    /** "47 · 440.0 Hz" (a table tuning) or "A4 · 440.0 Hz" (note names) for a note on this track. */
    juce::String describe(int note) const
    {
        int offset = 0;
        const auto tuning = tuningOf(context, device, &offset);
        const int entry = note + offset;
        if (tuning == nullptr || entry < 0 || entry >= TuningTable::numNotes || tuning->hz[static_cast<std::size_t>(entry)] <= 0.0)
            return juce::String(note) + juce::String::fromUTF8(" \xc2\xb7 -");
        const double hz = tuning->hz[static_cast<std::size_t>(entry)];
        const auto name = tuning->usesNoteNames ? juce::MidiMessage::getMidiNoteName(entry, true, true, 4) : juce::String(entry);
        return name + juce::String::fromUTF8(" \xc2\xb7 ") + juce::String(hz, hz < 100.0 ? 2 : 1) + " Hz";
    }

protected:
    void drawContent(juce::Graphics& g, juce::Rectangle<float> area) override
    {
        const auto& t = themeOf(*this);
        const int root = rootNote();
        auto r = area.reduced(t.padding * 0.5f);
        const float row = juce::jmax(t.fontSmall + 3.0f, r.getHeight() / 7.0f);
        g.setFont(t.font(juce::jmin(t.fontNormal, row - 3.0f)));
        const auto line = [&](const juce::String& left, const juce::String& right, bool on) {
            auto l = r.removeFromTop(row);
            g.setColour(on ? t.textDim : t.textDisabled);
            g.drawText(left, l.removeFromLeft(l.getWidth() * 0.3f), juce::Justification::centredLeft, false);
            g.setColour(on ? t.text : t.textDisabled);
            g.drawText(right, l, juce::Justification::centredLeft, true);
        };
        line("Root", describe(root), true);
        for (int i = 1; i <= 6; ++i)
        {
            const int shift = juce::roundToInt(mirror.value("chord.shift" + juce::String(i)));
            if (shift == 0)
                line(juce::String(i), "Off", false);
            else
                line((shift > 0 ? "+" : "") + juce::String(shift), describe(root + shift), true);
        }
    }

private:
    void engineStateChanged(std::uint32_t) override { invalidate(); }

    DeviceContext& context;
    juce::ValueTree device;
    DeviceMirror mirror;
};

class ChordPanel final : public PanelRow
{
public:
    ChordPanel(DeviceContext& c, const juce::ValueTree& d) : PanelRow(c, d)
    {
        // Each shift above its velocity, side by side.
        for (int i = 1; i <= 6; ++i)
            addSection({}, {"chord.shift" + juce::String(i), "chord.velocity" + juce::String(i)});
        addDisplay(std::make_unique<ChordNotes>(context, node), 1.0f);
    }
};
} // namespace

std::unique_ptr<DeviceBody> arpeggiator(DeviceContext& c, const juce::ValueTree& d)
{
    return std::make_unique<ArpeggiatorPanel>(c, d);
}

std::unique_ptr<DeviceBody> chord(DeviceContext& c, const juce::ValueTree& d)
{
    return std::make_unique<ChordPanel>(c, d);
}

} // namespace lattice::ui::panels
