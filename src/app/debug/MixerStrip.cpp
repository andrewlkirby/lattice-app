#include "app/debug/MixerStrip.h"

#include <cmath>

namespace lattice::debugui
{
namespace
{
constexpr float meterFloorDb = -60.0f;
constexpr float meterTopDb = 6.0f;
constexpr float falloffPerTick = 0.82f; // ~ -26 dB/s at 30 Hz

float meterFraction(float level)
{
    if (level <= 0.0f)
        return 0.0f;
    const float db = juce::Decibels::gainToDecibels(level, meterFloorDb);
    return juce::jlimit(0.0f, 1.0f, (db - meterFloorDb) / (meterTopDb - meterFloorDb));
}
} // namespace

void Meter::setPeaks(std::array<float, 2> peaks)
{
    bool changed = false;
    for (std::size_t c = 0; c < shown.size(); ++c)
    {
        float next = std::max(peaks[c], shown[c] * falloffPerTick);
        if (next < 1.0e-4f)
            next = 0.0f;
        if (std::abs(meterFraction(next) - meterFraction(shown[c])) > 0.002f ||
            (next == 0.0f) != (shown[c] == 0.0f))
            changed = true;
        shown[c] = next;
    }
    if (changed)
        repaint();
}

void Meter::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff151515));
    g.fillRect(r);
    const float width = (r.getWidth() - 1.0f) / 2.0f;
    for (std::size_t c = 0; c < shown.size(); ++c)
    {
        auto bar = r.withWidth(width).withX(r.getX() + static_cast<float>(c) * (width + 1.0f));
        const float fraction = meterFraction(shown[c]);
        auto lit = bar.removeFromBottom(bar.getHeight() * fraction);
        g.setColour(shown[c] >= 1.0f ? colours::record : colours::play.darker(0.2f));
        g.fillRect(lit);
    }
    // 0 dB mark
    const float zeroY = r.getBottom() - r.getHeight() * meterFraction(1.0f);
    g.setColour(colours::dimText);
    g.drawHorizontalLine(static_cast<int>(zeroY), r.getX(), r.getRight());
}

MixerStrip::MixerStrip(SessionController& s, TuningRegistry& tunings, int trackIndex)
    : session(s), registry(tunings), track(trackIndex)
{
    volume.setRange(LiveSet::minVolumeDb, LiveSet::maxVolumeDb, 0.1);
    volume.setSkewFactorFromMidPoint(-12.0);
    volume.setDoubleClickReturnValue(true, 0.0);
    volume.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, 18);
    volume.textFromValueFunction = [](double db) {
        return db <= LiveSet::minVolumeDb ? juce::String("-inf") : juce::String(db, 1) + " dB";
    };
    volume.onDragStart = [this] {
        session.undoManager().beginNewTransaction("Volume");
    };
    volume.onValueChange = [this] {
        if (isMaster())
            session.set().setMasterVolumeDb(volume.getValue());
        else
            session.set().setTrackVolumeDb(track, volume.getValue());
    };
    addAndMakeVisible(volume);

    pan.setRange(-1.0, 1.0, 0.01);
    pan.setDoubleClickReturnValue(true, 0.0);
    pan.onDragStart = [this] {
        session.undoManager().beginNewTransaction("Pan");
    };
    pan.onValueChange = [this] {
        if (isMaster())
            session.set().setMasterPan(pan.getValue());
        else
            session.set().setTrackPan(track, pan.getValue());
    };
    addAndMakeVisible(pan);
    addAndMakeVisible(meter);

    for (auto* c : {static_cast<juce::Component*>(&volume), static_cast<juce::Component*>(&pan),
                    static_cast<juce::Component*>(&mute), static_cast<juce::Component*>(&solo),
                    static_cast<juce::Component*>(&arm), static_cast<juce::Component*>(&tuning)})
        c->setWantsKeyboardFocus(false);

    if (!isMaster())
    {
        mute.setClickingTogglesState(false);
        mute.setColour(juce::TextButton::buttonOnColourId, colours::mute);
        mute.onClick = [this] {
            session.undoManager().beginNewTransaction("Mute");
            session.set().setTrackMute(track, !static_cast<bool>(node()[ids::mute]));
        };
        solo.setColour(juce::TextButton::buttonOnColourId, colours::solo);
        solo.onClick = [this] {
            session.undoManager().beginNewTransaction("Solo");
            session.set().setTrackSolo(track, !static_cast<bool>(node()[ids::solo]),
                                       juce::ModifierKeys::currentModifiers.isCommandDown());
        };
        arm.setColour(juce::TextButton::buttonOnColourId, colours::record);
        arm.onClick = [this] {
            session.undoManager().beginNewTransaction("Arm");
            session.set().setTrackArm(track, !static_cast<bool>(node()[ids::arm]),
                                      juce::ModifierKeys::currentModifiers.isCommandDown());
        };
        tuning.setTooltip("Tuning");
        tuning.onChange = [this] {
            const int index = tuning.getSelectedId() - 1;
            const auto& all = registry.getAll();
            if (index < 0 || index >= static_cast<int>(all.size()))
                return;
            if (all[static_cast<std::size_t>(index)]->id == node()[ids::tuningId].toString())
                return;
            session.undoManager().beginNewTransaction("Tuning");
            session.set().setTrackTuning(track, all[static_cast<std::size_t>(index)]->id);
        };
        addAndMakeVisible(mute);
        addAndMakeVisible(solo);
        addAndMakeVisible(arm);
        addAndMakeVisible(tuning);
        rebuildTuningMenu();
    }
    refresh();
}

juce::ValueTree MixerStrip::node() const
{
    return isMaster() ? session.set().master() : session.set().track(track);
}

void MixerStrip::rebuildTuningMenu()
{
    if (isMaster())
        return;
    tuning.clear(juce::dontSendNotification);
    const auto& all = registry.getAll();
    for (std::size_t i = 0; i < all.size(); ++i)
        tuning.addItem(all[i]->name, static_cast<int>(i) + 1);
    refresh();
}

void MixerStrip::refresh()
{
    const auto n = node();
    if (!n.isValid())
        return;
    volume.setValue(n[ids::volumeDb], juce::dontSendNotification);
    pan.setValue(n[ids::pan], juce::dontSendNotification);
    if (isMaster())
        return;

    mute.setToggleState(n[ids::mute], juce::dontSendNotification);
    solo.setToggleState(n[ids::solo], juce::dontSendNotification);
    arm.setToggleState(n[ids::arm], juce::dontSendNotification);

    const auto id = n[ids::tuningId].toString();
    const auto& all = registry.getAll();
    int selected = 0;
    for (std::size_t i = 0; i < all.size(); ++i)
        if (all[i]->id == id || (id.isEmpty() && all[i] == registry.defaultTuning()))
            selected = static_cast<int>(i) + 1;
    tuning.setSelectedId(selected, juce::dontSendNotification);
}

void MixerStrip::updateMeter()
{
    meter.setPeaks(isMaster() ? session.takeMasterPeaks() : session.takeTrackPeaks(track));
}

void MixerStrip::paint(juce::Graphics& g)
{
    g.setColour(colours::panel);
    g.fillRect(getLocalBounds().reduced(1, 0));
    g.setColour(colours::dimText);
    g.setFont(juce::FontOptions(12.0f));
    if (isMaster())
        g.drawText("Master", getLocalBounds().removeFromTop(20), juce::Justification::centred);
}

void MixerStrip::resized()
{
    auto area = getLocalBounds().reduced(6, 4);
    if (isMaster())
        area.removeFromTop(18);
    else
        tuning.setBounds(area.removeFromTop(22));
    area.removeFromTop(4);

    pan.setBounds(area.removeFromTop(36).withSizeKeepingCentre(36, 36));
    area.removeFromTop(4);

    if (!isMaster())
    {
        auto buttons = area.removeFromBottom(24);
        const int w = buttons.getWidth() / 3;
        mute.setBounds(buttons.removeFromLeft(w).reduced(1, 0));
        solo.setBounds(buttons.removeFromLeft(w).reduced(1, 0));
        arm.setBounds(buttons.reduced(1, 0));
        area.removeFromBottom(4);
    }

    meter.setBounds(area.removeFromRight(10).withTrimmedBottom(20));
    area.removeFromRight(4);
    volume.setBounds(area);
}

} // namespace lattice::debugui
