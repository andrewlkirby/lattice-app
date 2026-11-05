#include "control/modes/MixMode.h"

#include "control/modes/PadFaders.h"
#include "ui/mapping/Targets.h"
#include "ui/widgets/Meter.h"

#include <algorithm>
#include <cmath>

namespace lattice::control
{

namespace
{
std::uint32_t colourOf(const juce::ValueTree& node)
{
    return rgbOf(static_cast<std::uint32_t>(static_cast<juce::int64>(node[ids::colour])));
}

juce::String sendLetter(int r)
{
    return juce::String::charToString(static_cast<juce::juce_wchar>('A' + r));
}
} // namespace

MixMode::MixMode(ModeHost& h) : Mode(h) {}

int MixMode::controls() const
{
    return caps().encoders > 0 ? caps().encoders : caps().columns;
}

std::vector<int> MixMode::tracks() const
{
    return host.tracksInView(controls());
}

int MixMode::selectedTrack() const
{
    const auto& s = host.context().selection.get();
    const auto& set = host.context().session.set();
    return s.kind != ui::SessionSelection::Kind::scene && s.track >= 0 && s.track < set.numTracks() ? s.track : -1;
}

void MixMode::setControl(Control c, int sendReturn)
{
    const int returns = host.context().session.set().numReturns();
    if (c == Control::send && returns == 0)
        return;
    what = c;
    sendIndex = returns > 0 ? juce::jlimit(0, returns - 1, sendReturn) : 0;
    host.encoders().endGestures();
    host.requestRefresh();
}

std::vector<MixMode::Choice> MixMode::choices() const
{
    std::vector<Choice> out{{Control::volume, 0, "Volume"}, {Control::pan, 0, "Pan"}};
    const int returns = host.context().session.set().numReturns();
    if (padsOwned())
    {
        // Each send its own choice (the scene buttons); a column per track, so no Track page.
        for (int r = 0; r < returns; ++r)
            out.push_back({Control::send, r, "Send " + sendLetter(r)});
        return out;
    }
    if (returns > 0)
        out.push_back({Control::send, what == Control::send ? sendIndex : 0, "Send " + sendLetter(what == Control::send ? sendIndex : 0)});
    out.push_back({Control::track, 0, "Track"});
    return out;
}

bool MixMode::isChosen(const Choice& c) const
{
    return c.control == what && (c.control != Control::send || c.send == sendIndex);
}

void MixMode::press(const Choice& c)
{
    // Sends pressed again: the next return's.
    if (c.control == Control::send && what == Control::send && !padsOwned())
        setControl(Control::send, (sendIndex + 1) % std::max(1, host.context().session.set().numReturns()));
    else
        setControl(c.control, c.send);
}

juce::String MixMode::controlName(Control c, int send) const
{
    switch (c)
    {
    case Control::volume: return "Volume";
    case Control::pan: return "Pan";
    case Control::send:
    {
        const auto& set = host.context().session.set();
        const auto name = send < set.numReturns() ? set.returnTrack(send)[ids::name].toString() : juce::String();
        return name.isNotEmpty() ? name : "Send " + sendLetter(send);
    }
    case Control::track: break;
    }
    return {};
}

// --- Input ----------------------------------------------------------------------------------------------

bool MixMode::handle(const SurfaceInput& input)
{
    using Kind = SurfaceInput::Kind;
    const auto role = input.button.role;
    if (input.kind == Kind::buttonDown)
    {
        if (role == Role::displayUpper)
        {
            const auto all = choices();
            if (input.button.index < static_cast<int>(all.size()))
                press(all[static_cast<std::size_t>(input.button.index)]);
            return true;
        }
        if (role == Role::displayLower)
        {
            const auto shown = tracks();
            if (input.button.index < static_cast<int>(shown.size()))
                host.pressTrack(shown[static_cast<std::size_t>(input.button.index)]);
            return true;
        }
        if (!padsOwned())
            return false;
        switch (role)
        {
        case Role::scene:
        {
            const auto all = choices();
            if (input.button.index < static_cast<int>(all.size()))
                press(all[static_cast<std::size_t>(input.button.index)]);
            return true;
        }
        case Role::navLeft: host.scrollTracks(host.shiftHeld() ? -controls() : -1); return true;
        case Role::navRight: host.scrollTracks(host.shiftHeld() ? controls() : 1); return true;
        default: return false;
        }
    }
    if (input.kind == Kind::buttonUp)
        return role == Role::displayUpper || role == Role::displayLower ||
               (padsOwned() && (role == Role::scene || role == Role::navLeft || role == Role::navRight));
    if (!padsOwned())
        return false;
    if (input.kind == Kind::padDown)
    {
        if (auto* binding = host.encoders().binding(input.column))
            binding->setNormalised(faders::positionOfRow(input.row, caps().rows)); // a step of its own
        return true;
    }
    return input.kind == Kind::padUp || input.kind == Kind::padPressure;
}

// --- Output ---------------------------------------------------------------------------------------------

void MixMode::encoderTargets(std::vector<juce::String>& targets)
{
    const auto& set = host.context().session.set();
    if (what == Control::track)
    {
        const int t = selectedTrack();
        if (t < 0)
            return;
        const auto node = set.track(t);
        targets.push_back(ui::ValueBinding::targetOf(node, ids::volumeDb));
        targets.push_back(ui::ValueBinding::targetOf(node, ids::pan));
        for (int r = 0; r < set.numReturns(); ++r)
            targets.push_back(ui::sendTarget(set.trackId(t), LiveSet::idOf(set.returnTrack(r))));
        targets.resize(std::min(targets.size(), static_cast<std::size_t>(controls())));
        return;
    }
    for (const int t : tracks())
    {
        const auto node = set.track(t);
        switch (what)
        {
        case Control::volume: targets.push_back(ui::ValueBinding::targetOf(node, ids::volumeDb)); break;
        case Control::pan: targets.push_back(ui::ValueBinding::targetOf(node, ids::pan)); break;
        case Control::send:
            targets.push_back(sendIndex < set.numReturns() ? ui::sendTarget(set.trackId(t), LiveSet::idOf(set.returnTrack(sendIndex)))
                                                           : juce::String());
            break;
        case Control::track: break;
        }
    }
}

void MixMode::display(DisplayFrame& f)
{
    auto& c = host.context();
    const auto& set = c.session.set();
    const auto all = choices();
    for (int i = 0; i < caps().countOf(Role::displayUpper) && i < static_cast<int>(all.size()); ++i)
        f.upper.push_back({all[static_cast<std::size_t>(i)].label, 0u, isChosen(all[static_cast<std::size_t>(i)])});

    const auto shown = tracks();
    const int selected = selectedTrack();
    const auto meterOf = [&c](int t) {
        const auto peaks = c.feed.trackPeaks(t);
        // In 64ths, so a level that hardly moves doesn't draw the display again.
        return std::round(ui::Meter::proportionForDb(ui::Meter::toDb(std::max(peaks[0], peaks[1]))) * 64.0f) / 64.0f;
    };
    auto& bank = host.encoders();
    for (int i = 0; i < controls(); ++i)
    {
        DisplayCell cell;
        if (auto* b = bank.binding(i))
        {
            const int t = what == Control::track ? selected : (i < static_cast<int>(shown.size()) ? shown[static_cast<std::size_t>(i)] : -1);
            if (t >= 0)
            {
                const auto node = set.track(t);
                cell.caption = node[ids::name].toString();
                cell.colour = colourOf(node);
                cell.selected = t == selected && what != Control::track;
                if (what != Control::track || i == 0)
                    cell.meter = meterOf(t);
            }
            const auto c2 = what == Control::track ? (i == 0 ? Control::volume : i == 1 ? Control::pan : Control::send) : what;
            cell.name = controlName(c2, what == Control::track ? i - 2 : sendIndex);
            cell.value = b->text();
            cell.position = b->normalised();
            cell.bipolar = c2 == Control::pan;
            cell.detail = b->textFor(b->spec().minValue) + juce::String::fromUTF8(" \xe2\x80\x93 ") + b->textFor(b->spec().maxValue);
        }
        f.cells.push_back(std::move(cell));
    }
    f.detail = bank.touchedEncoder();
    if (what == Control::track && selected < 0)
        f.message = "No track selected";
    else if (shown.empty())
        f.message = "No tracks";

    for (int i = 0; i < caps().countOf(Role::displayLower) && i < static_cast<int>(shown.size()); ++i)
    {
        const auto node = set.track(shown[static_cast<std::size_t>(i)]);
        f.lower.push_back({node[ids::name].toString(), colourOf(node), shown[static_cast<std::size_t>(i)] == selected});
    }
}

void MixMode::describe(SurfaceText& text)
{
    const auto all = choices();
    for (const auto& c : all)
        if (isChosen(c))
            text.status = "Mix: " + c.label;
    if (padsOwned()) // the choices on the scene buttons
        for (int i = 0; i < caps().countOf(Role::scene) && i < static_cast<int>(all.size()); ++i)
            labelButton(text, {Role::scene, i}, all[static_cast<std::size_t>(i)].label);
}

void MixMode::render(LedFrame& frame)
{
    const auto& colours = host.colours();
    const auto available = LedState::solid(dimmed(colours.white, colours.dim));
    const auto lit = LedState::solid(colours.white);
    const auto all = choices();
    const auto chosenLed = [&](int i) {
        return i < static_cast<int>(all.size()) ? (isChosen(all[static_cast<std::size_t>(i)]) ? lit : available) : LedState::off();
    };
    for (int i = 0; i < caps().countOf(Role::displayUpper); ++i)
        setButton(frame, {Role::displayUpper, i}, chosenLed(i));
    const auto shown = tracks();
    for (int i = 0; i < caps().countOf(Role::displayLower); ++i)
        setButton(frame, {Role::displayLower, i},
                  i < static_cast<int>(shown.size()) ? host.trackLed(shown[static_cast<std::size_t>(i)]) : LedState::off());
    if (!padsOwned())
        return;

    const auto& set = host.context().session.set();
    for (int column = 0; column < caps().columns && column < static_cast<int>(shown.size()); ++column)
        if (const auto* b = host.encoders().binding(column))
            faders::renderColumn(frame, caps(), column, b->normalised(), what == Control::pan,
                                 colourOf(set.track(shown[static_cast<std::size_t>(column)])), 0.12f);
    for (int i = 0; i < caps().countOf(Role::scene); ++i)
        setButton(frame, {Role::scene, i}, chosenLed(i));
    setButton(frame, {Role::navLeft, 0}, available);
    setButton(frame, {Role::navRight, 0}, available);
}

} // namespace lattice::control
