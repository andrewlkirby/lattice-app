#include "control/virtual/VirtualPush.h"

#include "control/surface/Presets.h"
#include "ui/theme/LatticeLookAndFeel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace lattice::control
{

namespace
{
// The left panel, two buttons a row, grouped as on a Push: transport, editing, the track
// modifiers, the modes, the note layout, then the arrows.
constexpr std::array<std::pair<Role, Role>, 15> leftPanel{{
    {Role::shift, Role::undo},
    {Role::play, Role::record},
    {Role::metronome, Role::tapTempo},
    {Role::stopAll, Role::newClip},
    {Role::deleteClip, Role::duplicate},
    {Role::stopClip, Role::mute},
    {Role::solo, Role::quantize},
    {Role::fixedLength, Role::doubleLoop},
    {Role::modeSession, Role::modeNote},
    {Role::modeDevice, Role::modeMix},
    {Role::modeBrowse, Role::modeClip},
    {Role::layout, Role::scale},
    {Role::octaveUp, Role::octaveDown},
    {Role::navUp, Role::navDown},
    {Role::navLeft, Role::navRight},
}};

// In grid units (a pad and its gap).
constexpr float leftWidth = 2.0f;   // two buttons, a unit each
constexpr float gapUnits = 0.3f;    // between the panel, the grid and the scene buttons
constexpr float sceneWidth = 0.8f;
constexpr float trackHeight = 0.6f;
constexpr float padInset = 0.06f;   // each side of a pad, inside its unit
constexpr float buttonInset = 0.05f;

bool isArrow(Role r)
{
    return r == Role::navUp || r == Role::navDown || r == Role::navLeft || r == Role::navRight;
}

void arrow(juce::Graphics& g, juce::Rectangle<float> r, Role role)
{
    const auto s = std::min(r.getWidth(), r.getHeight()) * 0.22f;
    const auto c = r.getCentre();
    juce::Path p;
    switch (role)
    {
    case Role::navUp: p.addTriangle(c.x - s, c.y + s * 0.6f, c.x + s, c.y + s * 0.6f, c.x, c.y - s * 0.6f); break;
    case Role::navDown: p.addTriangle(c.x - s, c.y - s * 0.6f, c.x + s, c.y - s * 0.6f, c.x, c.y + s * 0.6f); break;
    case Role::navLeft: p.addTriangle(c.x + s * 0.6f, c.y - s, c.x + s * 0.6f, c.y + s, c.x - s * 0.6f, c.y); break;
    default: p.addTriangle(c.x - s * 0.6f, c.y - s, c.x - s * 0.6f, c.y + s, c.x + s * 0.6f, c.y); break;
    }
    g.fillPath(p);
}
} // namespace

juce::String VirtualPush::labelFor(ButtonId id)
{
    switch (id.role)
    {
    case Role::play: return "Play";
    case Role::record: return "Record";
    case Role::shift: return "Shift";
    case Role::modeSession: return "Session";
    case Role::modeNote: return "Note";
    case Role::modeDevice: return "Device";
    case Role::modeMix: return "Mix";
    case Role::modeBrowse: return "Browse";
    case Role::modeClip: return "Clip";
    case Role::navUp: return "Up";
    case Role::navDown: return "Down";
    case Role::navLeft: return "Left";
    case Role::navRight: return "Right";
    case Role::scene: return "Scene " + juce::String(id.index + 1);
    case Role::trackSelect: return "Track " + juce::String(id.index + 1);
    case Role::displayUpper: return "Upper " + juce::String(id.index + 1);
    case Role::displayLower: return "Lower " + juce::String(id.index + 1);
    case Role::octaveUp: return "Octave +";
    case Role::octaveDown: return "Octave -";
    case Role::layout: return "Layout";
    case Role::scale: return "Scale";
    case Role::fixedLength: return "Fixed Length";
    case Role::duplicate: return "Duplicate";
    case Role::deleteClip: return "Delete";
    case Role::undo: return "Undo";
    case Role::quantize: return "Quantize";
    case Role::doubleLoop: return "Double Loop";
    case Role::newClip: return "New";
    case Role::metronome: return "Metronome";
    case Role::tapTempo: return "Tap Tempo";
    case Role::stopClip: return "Stop Clip";
    case Role::mute: return "Mute";
    case Role::solo: return "Solo";
    case Role::stopAll: return "Stop All";
    case Role::count: break;
    }
    return {};
}

VirtualPush::VirtualPush(ui::EngineStateFeed& f, Settings settings)
    : feed(f), config(settings), caps(presets::virtualPush(settings.rows, settings.columns))
{
    config.rows = caps.rows;
    config.columns = caps.columns;
    frame = LedFrame::blank(caps);
    setTitle("Virtual Push");
    setDescription("An on-screen controller: pads, scene and track buttons, transport and modes. Right-click latches a pad or button.");
    setWantsKeyboardFocus(false);
    setOpaque(true);
}

VirtualPush::~VirtualPush() = default;

VirtualPush::Layout VirtualPush::layoutFor(const SurfaceCapabilities& c, juce::Rectangle<float> bounds)
{
    Layout l;
    const float widthUnits = leftWidth + gapUnits + static_cast<float>(c.columns) + gapUnits + sceneWidth;
    const float heightUnits = static_cast<float>(c.rows) + gapUnits + trackHeight;
    const float u = std::max(0.0f, std::min(bounds.getWidth() / widthUnits, bounds.getHeight() / heightUnits));
    l.unit = u;
    // Centred across, at the top: the room left over is below it.
    const juce::Point<float> origin{bounds.getCentreX() - widthUnits * u * 0.5f, bounds.getY()};
    const float gridX = origin.x + (leftWidth + gapUnits) * u;
    const float gridY = origin.y;

    for (int r = 0; r < c.rows; ++r)
        for (int col = 0; col < c.columns; ++col)
            l.pads.push_back(juce::Rectangle<float>(gridX + static_cast<float>(col) * u, gridY + static_cast<float>(r) * u, u, u)
                                 .reduced(u * padInset));

    l.buttons.resize(c.buttons.size());
    const float gridRight = gridX + static_cast<float>(c.columns) * u;
    const float gridBottom = gridY + static_cast<float>(c.rows) * u;
    const float leftRow = static_cast<float>(c.rows) * u / static_cast<float>(leftPanel.size());
    for (std::size_t i = 0; i < c.buttons.size(); ++i)
    {
        const auto id = c.buttons[i].id;
        auto& b = l.buttons[i];
        b.id = id;
        b.label = labelFor(id);
        if (id.role == Role::scene)
            b.bounds = juce::Rectangle<float>(gridRight + gapUnits * u, gridY + static_cast<float>(id.index) * u, sceneWidth * u, u)
                           .reduced(u * buttonInset, u * 0.15f);
        else if (id.role == Role::trackSelect)
            b.bounds = juce::Rectangle<float>(gridX + static_cast<float>(id.index) * u, gridBottom + gapUnits * u, u, trackHeight * u)
                           .reduced(u * buttonInset);
        else
            for (std::size_t row = 0; row < leftPanel.size(); ++row)
                if (leftPanel[row].first == id.role || leftPanel[row].second == id.role)
                {
                    const float x = origin.x + (leftPanel[row].first == id.role ? 0.0f : u);
                    b.bounds = juce::Rectangle<float>(x, gridY + static_cast<float>(row) * leftRow, u, leftRow).reduced(u * buttonInset);
                }
    }
    return l;
}

void VirtualPush::resized()
{
    places = layoutFor(caps, getLocalBounds().toFloat().reduced(ui::themeOf(*this).padding));
}

void VirtualPush::lookAndFeelChanged()
{
    resized();
    repaint();
}

VirtualPush::Hit VirtualPush::hitTest(juce::Point<float> p) const
{
    for (std::size_t i = 0; i < places.pads.size(); ++i)
        if (places.pads[i].contains(p))
            return {static_cast<int>(i), -1};
    for (std::size_t i = 0; i < places.buttons.size(); ++i)
        if (places.buttons[i].bounds.contains(p))
            return {-1, static_cast<int>(i)};
    return {};
}

float VirtualPush::velocityAt(int pad, juce::Point<float> p) const
{
    if (config.fixedVelocity > 0)
        return static_cast<float>(juce::jlimit(1, 127, config.fixedVelocity)) / 127.0f;
    if (pad < 0 || pad >= static_cast<int>(places.pads.size()))
        return 100.0f / 127.0f;
    const auto r = places.pads[static_cast<std::size_t>(pad)];
    // The top of a pad is the loudest, its bottom a fifth as loud.
    const float down = juce::jlimit(0.0f, 1.0f, (p.y - r.getY()) / std::max(1.0f, r.getHeight()));
    return juce::jlimit(1.0f / 127.0f, 1.0f, 1.0f - 0.8f * down);
}

// --- LEDs --------------------------------------------------------------------------------------------------

void VirtualPush::showLeds(const LedFrame& f, const LedFrame::Changes& changed)
{
    const bool sameShape = f.pads.size() == frame.pads.size() && f.buttons.size() == frame.buttons.size();
    frame = f;
    if (!sameShape)
        repaint();
    else
    {
        for (const int p : changed.pads)
            repaint(areaOf({p, -1}));
        for (const int b : changed.buttons)
            repaint(areaOf({-1, b}));
    }
    updateAnimator();
}

void VirtualPush::updateAnimator()
{
    if (frame.anyAnimated() && isShowing())
    {
        if (animator == nullptr)
            animator = std::make_unique<juce::VBlankAttachment>(this, [this](double) {
                for (std::size_t i = 0; i < frame.pads.size(); ++i)
                    if (frame.pads[i].animation != LedState::Animation::solid && !frame.pads[i].isOff())
                        repaint(areaOf({static_cast<int>(i), -1}));
                for (std::size_t i = 0; i < frame.buttons.size(); ++i)
                    if (frame.buttons[i].animation != LedState::Animation::solid && !frame.buttons[i].isOff())
                        repaint(areaOf({-1, static_cast<int>(i)}));
            });
    }
    else
        animator.reset();
}

double VirtualPush::beatNow() const
{
    const auto& transport = feed.transport();
    if (transport.playing)
        return transport.beat;
    // Stopped: blink at the set's tempo all the same.
    return juce::Time::getMillisecondCounterHiRes() * 0.001 * transport.tempo / 60.0;
}

juce::Colour VirtualPush::ledColour(const LedState& led, juce::Colour off, double beat) const
{
    if (led.isOff())
        return off;
    // The LED's colour at full brightness, lit by its brightness: a dim light is a little of its
    // colour over the unlit pad, on a dark theme or a light one.
    const auto r = static_cast<float>((led.rgb >> 16) & 0xffu), g = static_cast<float>((led.rgb >> 8) & 0xffu),
               b = static_cast<float>(led.rgb & 0xffu);
    const float peak = std::max({r, g, b, 1.0f});
    const auto full = juce::Colour::fromFloatRGBA(r / peak, g / peak, b / peak, 1.0f);
    return off.interpolatedWith(full, peak / 255.0f * animationLevel(led.animation, beat));
}

juce::Rectangle<int> VirtualPush::areaOf(const Target& t) const
{
    if (t.pad >= 0 && t.pad < static_cast<int>(places.pads.size()))
        return places.pads[static_cast<std::size_t>(t.pad)].getSmallestIntegerContainer().expanded(2);
    if (t.button >= 0 && t.button < static_cast<int>(places.buttons.size()))
        return places.buttons[static_cast<std::size_t>(t.button)].bounds.getSmallestIntegerContainer().expanded(2);
    return {};
}

void VirtualPush::paint(juce::Graphics& g)
{
    const auto& t = ui::themeOf(*this);
    g.fillAll(t.surface);
    // Unlit pads and buttons are dark in every theme, as on a device: a light's brightness only
    // reads against dark (a light theme's pale grey would wash out white and dim lights).
    const auto unlit = [](juce::Colour c, float most) { return c.getPerceivedBrightness() <= most ? c : juce::Colour(0xff26272b); };
    const auto padOff = unlit(t.surfaceSunken, 0.3f);
    const auto buttonOff = unlit(t.surfaceRaised, 0.35f).brighter(padOff == t.surfaceSunken ? 0.0f : 0.15f);
    const auto clip = g.getClipBounds().toFloat();
    const double beat = beatNow();
    const float radius = std::max(1.0f, places.unit * 0.08f);
    const auto isPressed = [this](const Target& target) {
        const auto same = [&](const Target& o) { return o.pad == target.pad && o.button == target.button; };
        return std::any_of(active.begin(), active.end(), [&](const auto& a) { return same(a.second); }) ||
               std::any_of(latched.begin(), latched.end(), same);
    };

    for (std::size_t i = 0; i < places.pads.size(); ++i)
    {
        const auto r = places.pads[i];
        if (!r.intersects(clip))
            continue;
        const auto led = i < frame.pads.size() ? frame.pads[i] : LedState{};
        g.setColour(ledColour(led, padOff, beat));
        g.fillRoundedRectangle(r, radius);
        if (isPressed({static_cast<int>(i), -1}))
        {
            g.setColour(t.text);
            g.drawRoundedRectangle(r.reduced(1.0f), radius, std::max(1.5f, places.unit * 0.04f));
        }
    }

    for (std::size_t i = 0; i < places.buttons.size(); ++i)
    {
        const auto& b = places.buttons[i];
        if (b.bounds.isEmpty() || !b.bounds.intersects(clip))
            continue;
        const auto led = i < frame.buttons.size() ? frame.buttons[i] : LedState{};
        const auto fill = ledColour(led, buttonOff, beat);
        g.setColour(fill);
        g.fillRoundedRectangle(b.bounds, radius);
        if (isPressed({-1, static_cast<int>(i)}))
        {
            g.setColour(t.focus);
            g.drawRoundedRectangle(b.bounds.reduced(1.0f), radius, std::max(1.5f, places.unit * 0.04f));
        }
        const auto ink = fill.getPerceivedBrightness() > 0.6f ? juce::Colours::black : juce::Colour(0xffe8e8ea);
        g.setColour(ink.withMultipliedAlpha(led.isOff() ? 0.6f : 1.0f));
        if (isArrow(b.id.role))
            arrow(g, b.bounds, b.id.role);
        else if (b.id.role != Role::scene && b.id.role != Role::trackSelect)
        {
            g.setFont(t.font(std::min(t.fontSmall, b.bounds.getHeight() * 0.42f)));
            g.drawFittedText(b.label, b.bounds.reduced(2.0f).toNearestInt(), juce::Justification::centred, 2, 0.7f);
        }
    }
}

// --- Input -------------------------------------------------------------------------------------------------

bool VirtualPush::isLatched(int index, bool isButton) const
{
    return std::any_of(latched.begin(), latched.end(),
                       [&](const Target& t) { return isButton ? t.button == index : t.pad == index; });
}

void VirtualPush::press(const Target& t, float velocity)
{
    if (onInput)
    {
        if (t.pad >= 0)
            onInput(SurfaceInput::padDown(t.pad / caps.columns, t.pad % caps.columns, velocity));
        else
            onInput(SurfaceInput::buttonDown(caps.buttons[static_cast<std::size_t>(t.button)].id));
    }
    repaint(areaOf(t));
}

void VirtualPush::release(const Target& t)
{
    if (onInput)
    {
        if (t.pad >= 0)
            onInput(SurfaceInput::padUp(t.pad / caps.columns, t.pad % caps.columns));
        else
            onInput(SurfaceInput::buttonUp(caps.buttons[static_cast<std::size_t>(t.button)].id));
    }
    repaint(areaOf(t));
}

void VirtualPush::mouseDown(const juce::MouseEvent& e)
{
    const auto hit = hitTest(e.position);
    if (hit.pad < 0 && hit.button < 0)
        return;
    const Target target{hit.pad, hit.button};
    const auto same = [&](const Target& o) { return o.pad == target.pad && o.button == target.button; };
    if (const auto it = std::find_if(latched.begin(), latched.end(), same); it != latched.end())
    {
        // Any click on a latched pad or button lets it go.
        latched.erase(it);
        release(target);
        return;
    }
    if (e.mods.isPopupMenu())
    {
        latched.push_back(target);
        press(target, velocityAt(hit.pad, e.position));
        return;
    }
    const int source = e.source.getIndex();
    if (const auto it = active.find(source); it != active.end())
        release(it->second);
    active[source] = target;
    press(target, velocityAt(hit.pad, e.position));
}

void VirtualPush::mouseUp(const juce::MouseEvent& e)
{
    if (const auto it = active.find(e.source.getIndex()); it != active.end())
    {
        const auto target = it->second;
        active.erase(it);
        release(target);
    }
}

} // namespace lattice::control
