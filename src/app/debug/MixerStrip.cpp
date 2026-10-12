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

namespace
{
constexpr int baseHeight = 230;
constexpr int sendsPerRow = 3;
constexpr int sendRowHeight = 38;
} // namespace

int MixerStrip::heightFor(int numReturns)
{
    return baseHeight + (numReturns + sendsPerRow - 1) / sendsPerRow * sendRowHeight;
}

MixerStrip::MixerStrip(SessionController& s, TuningRegistry& tunings, Kind k, int i)
    : session(s), registry(tunings), kind(k), index(i)
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
        switch (kind)
        {
        case Kind::Master:
            session.set().setMasterVolumeDb(volume.getValue());
            break;
        case Kind::Return:
            session.set().setReturnVolumeDb(index, volume.getValue());
            break;
        case Kind::Track:
            session.set().setTrackVolumeDb(index, volume.getValue());
            break;
        }
    };
    addAndMakeVisible(volume);

    pan.setRange(-1.0, 1.0, 0.01);
    pan.setDoubleClickReturnValue(true, 0.0);
    pan.onDragStart = [this] {
        session.undoManager().beginNewTransaction("Pan");
    };
    pan.onValueChange = [this] {
        switch (kind)
        {
        case Kind::Master:
            session.set().setMasterPan(pan.getValue());
            break;
        case Kind::Return:
            session.set().setReturnPan(index, pan.getValue());
            break;
        case Kind::Track:
            session.set().setTrackPan(index, pan.getValue());
            break;
        }
    };
    addAndMakeVisible(pan);
    addAndMakeVisible(meter);

    if (kind != Kind::Master)
    {
        mute.setClickingTogglesState(false);
        mute.setColour(juce::TextButton::buttonOnColourId, colours::mute);
        mute.onClick = [this] {
            session.undoManager().beginNewTransaction("Mute");
            const bool on = !static_cast<bool>(node()[ids::mute]);
            if (kind == Kind::Return)
                session.set().setReturnMute(index, on);
            else
                session.set().setTrackMute(index, on);
        };
        solo.setColour(juce::TextButton::buttonOnColourId, colours::solo);
        solo.onClick = [this] {
            session.undoManager().beginNewTransaction("Solo");
            const bool on = !static_cast<bool>(node()[ids::solo]);
            if (kind == Kind::Return)
                session.set().setReturnSolo(index, on);
            else
                session.set().setTrackSolo(index, on, juce::ModifierKeys::currentModifiers.isCommandDown());
        };
        addAndMakeVisible(mute);
        addAndMakeVisible(solo);
    }

    if (kind == Kind::Track)
    {
        arm.setColour(juce::TextButton::buttonOnColourId, colours::record);
        arm.onClick = [this] {
            session.undoManager().beginNewTransaction("Arm");
            session.set().setTrackArm(index, !static_cast<bool>(node()[ids::arm]),
                                      juce::ModifierKeys::currentModifiers.isCommandDown());
        };
        tuning.setTooltip("Tuning");
        tuning.onChange = [this] {
            const int selected = tuning.getSelectedId() - 1;
            const auto& all = registry.getAll();
            if (selected < 0 || selected >= static_cast<int>(all.size()))
                return;
            if (all[static_cast<std::size_t>(selected)]->id == node()[ids::tuningId].toString())
                return;
            session.undoManager().beginNewTransaction("Tuning");
            session.set().setTrackTuning(index, all[static_cast<std::size_t>(selected)]->id);
        };
        addAndMakeVisible(arm);
        addAndMakeVisible(tuning);
        rebuildTuningMenu();

        // A send per return.
        for (int r = 0; r < session.set().numReturns(); ++r)
        {
            auto send = std::make_unique<juce::Slider>(juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox);
            send->setRange(LiveSet::minVolumeDb, 0.0, 0.1);
            send->setSkewFactorFromMidPoint(-18.0);
            send->setDoubleClickReturnValue(true, LiveSet::minVolumeDb);
            send->setPopupDisplayEnabled(true, true, nullptr);
            send->textFromValueFunction = [](double db) {
                return db <= LiveSet::minVolumeDb ? juce::String("-inf") : juce::String(db, 1) + " dB";
            };
            send->onDragStart = [this] {
                session.undoManager().beginNewTransaction("Send");
            };
            send->onValueChange = [this, r, slider = send.get()] {
                session.set().setSendLevel(index, r, slider->getValue());
            };
            send->setTooltip("Send to " + session.set().returnTrack(r)[ids::name].toString());
            addAndMakeVisible(*send);
            sends.push_back(std::move(send));
        }
    }
    else if (kind == Kind::Return)
    {
        preFader.setClickingTogglesState(false);
        preFader.setTooltip("Sends to this return are taken before the tracks' faders");
        preFader.setColour(juce::TextButton::buttonOnColourId, colours::solo.darker(0.3f));
        preFader.onClick = [this] {
            session.undoManager().beginNewTransaction("Pre/post fader");
            session.set().setReturnPreFader(index, !static_cast<bool>(node()[ids::preFader]));
        };
        addAndMakeVisible(preFader);
    }
    else
    {
        addReturn.setTooltip("Add a return track");
        addReturn.onClick = [this] {
            session.undoManager().beginNewTransaction("Add return");
            session.set().addReturn();
        };
        addAndMakeVisible(addReturn);
    }

    for (auto* c : getChildren())
        c->setWantsKeyboardFocus(false);
    refresh();
}

juce::ValueTree MixerStrip::node() const
{
    switch (kind)
    {
    case Kind::Master:
        return session.set().master();
    case Kind::Return:
        return session.set().returnTrack(index);
    case Kind::Track:
        break;
    }
    return session.set().track(index);
}

void MixerStrip::rebuildTuningMenu()
{
    if (kind != Kind::Track)
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
    if (kind == Kind::Master)
        return;

    mute.setToggleState(n[ids::mute], juce::dontSendNotification);
    solo.setToggleState(n[ids::solo], juce::dontSendNotification);
    if (kind == Kind::Return)
    {
        preFader.setToggleState(n[ids::preFader], juce::dontSendNotification);
        repaint(); // the name
        return;
    }

    arm.setToggleState(n[ids::arm], juce::dontSendNotification);
    for (std::size_t r = 0; r < sends.size(); ++r)
        if (!sends[r]->isMouseButtonDown())
            sends[r]->setValue(session.set().sendLevel(index, static_cast<int>(r)), juce::dontSendNotification);

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
    switch (kind)
    {
    case Kind::Master:
        meter.setPeaks(session.takeMasterPeaks());
        break;
    case Kind::Return:
        meter.setPeaks(session.takeReturnPeaks(index));
        break;
    case Kind::Track:
        meter.setPeaks(session.takeTrackPeaks(index));
        break;
    }
}

void MixerStrip::setHighlighted(bool on)
{
    if (on != highlighted)
    {
        highlighted = on;
        repaint();
    }
}

void MixerStrip::mouseDown(const juce::MouseEvent& e)
{
    if (kind == Kind::Return && e.mods.isPopupMenu())
    {
        juce::PopupMenu menu;
        menu.addItem("Rename...", [this] {
            askForText("Rename return", "Name", node()[ids::name].toString(), [this](const juce::String& name) {
                session.undoManager().beginNewTransaction("Rename return");
                session.set().setReturnName(index, name);
            });
        });
        menu.addItem("Delete return", [this] {
            session.undoManager().beginNewTransaction("Delete return");
            session.set().deleteReturn(index);
        });
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this));
        return;
    }
    if (onSelect)
        onSelect();
}

void MixerStrip::paint(juce::Graphics& g)
{
    g.setColour(highlighted ? colours::panel.brighter(0.12f) : colours::panel);
    g.fillRect(getLocalBounds().reduced(1, 0));
    g.setColour(colours::dimText);
    g.setFont(juce::FontOptions(12.0f));
    if (kind == Kind::Master)
        g.drawText("Master", getLocalBounds().removeFromTop(20), juce::Justification::centred);
    else if (kind == Kind::Return)
    {
        g.setColour(colours::text);
        g.drawText(node()[ids::name].toString(), getLocalBounds().removeFromTop(22).reduced(4, 0),
                   juce::Justification::centred, true);
    }
    // Send labels: the return's letter under each knob.
    g.setColour(colours::dimText);
    g.setFont(juce::FontOptions(10.0f));
    for (std::size_t r = 0; r < sends.size(); ++r)
        g.drawText(session.set().returnTrack(static_cast<int>(r))[ids::name].toString().substring(0, 1),
                   sends[r]->getBounds().translated(0, sends[r]->getHeight() - 2).withHeight(12),
                   juce::Justification::centred);
}

void MixerStrip::resized()
{
    auto area = getLocalBounds().reduced(6, 4);
    switch (kind)
    {
    case Kind::Master:
        area.removeFromTop(18);
        addReturn.setBounds(area.removeFromTop(22));
        break;
    case Kind::Return:
        area.removeFromTop(18);
        preFader.setBounds(area.removeFromTop(22).withSizeKeepingCentre(48, 22));
        break;
    case Kind::Track:
        tuning.setBounds(area.removeFromTop(22));
        break;
    }
    area.removeFromTop(4);

    // Sends, in rows.
    if (!sends.empty())
    {
        const int rows = (static_cast<int>(sends.size()) + sendsPerRow - 1) / sendsPerRow;
        auto block = area.removeFromTop(rows * sendRowHeight);
        const int w = block.getWidth() / sendsPerRow;
        for (std::size_t r = 0; r < sends.size(); ++r)
        {
            const int row = static_cast<int>(r) / sendsPerRow, col = static_cast<int>(r) % sendsPerRow;
            sends[r]->setBounds(block.getX() + col * w, block.getY() + row * sendRowHeight, w, sendRowHeight - 12);
        }
    }

    pan.setBounds(area.removeFromTop(36).withSizeKeepingCentre(36, 36));
    area.removeFromTop(4);

    if (kind != Kind::Master)
    {
        auto buttons = area.removeFromBottom(24);
        const int w = buttons.getWidth() / (kind == Kind::Track ? 3 : 2);
        mute.setBounds(buttons.removeFromLeft(w).reduced(1, 0));
        solo.setBounds(buttons.removeFromLeft(w).reduced(1, 0));
        if (kind == Kind::Track)
            arm.setBounds(buttons.reduced(1, 0));
        area.removeFromBottom(4);
    }

    meter.setBounds(area.removeFromRight(10).withTrimmedBottom(20));
    area.removeFromRight(4);
    volume.setBounds(area);
}

} // namespace lattice::debugui
