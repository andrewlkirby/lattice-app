#include "control/virtual/VirtualPush.h"

#include "control/display/DisplayRenderer.h"
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
constexpr float stripWidth = 0.45f;
constexpr float statusHeight = 0.45f;
constexpr float labelMinPixels = 7.0f; // smaller labels than this go unshown
constexpr float padInset = 0.06f;   // each side of a pad, inside its unit
constexpr float buttonInset = 0.05f;
constexpr float displayButtonHeight = 0.5f; // the buttons above and below the display
constexpr float dragUnitsPerRange = 3.0f;   // an encoder's whole range: three pads' height of drag

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

int VirtualPush::encoderCountFor(int n) noexcept
{
    return n <= 0 ? 0 : n <= 12 ? 8 : n <= 24 ? 16 : 32;
}

VirtualPush::VirtualPush(ui::EngineStateFeed& f, Settings settings)
    : feed(f), config(settings),
      caps(presets::virtualPush(settings.rows, settings.columns, encoderCountFor(settings.encoders)))
{
    caps.velocity = config.fixedVelocity <= 0; // a fixed velocity plays as it is, with no curve
    config.rows = caps.rows;
    config.columns = caps.columns;
    config.encoders = caps.encoders;
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
    const float stripUnits = c.touchStrip ? stripWidth + gapUnits : 0.0f;
    const float widthUnits = leftWidth + gapUnits + stripUnits + static_cast<float>(c.columns) + gapUnits + sceneWidth;
    // The display, as wide as everything below it (its words need the room), and the buttons
    // above and below it.
    const bool display = c.encoders > 0 && c.hasDisplay();
    const float displayUnits = display ? DisplayRenderer::heightFor(widthUnits, c.encoders, c.cellsAcross()) : 0.0f;
    const float topUnits = display ? displayButtonHeight * 2.0f + displayUnits + gapUnits : 0.0f;
    const float heightUnits = topUnits + statusHeight + gapUnits + static_cast<float>(c.rows) + gapUnits + trackHeight;
    const float u = std::max(0.0f, std::min(bounds.getWidth() / widthUnits, bounds.getHeight() / heightUnits));
    l.unit = u;
    // Centred across, at the top: the room left over is below it.
    const juce::Point<float> origin{bounds.getCentreX() - widthUnits * u * 0.5f, bounds.getY()};
    l.used = {origin.x, origin.y, widthUnits * u, heightUnits * u};
    const float gridX = origin.x + (leftWidth + gapUnits + stripUnits) * u;
    const float gridY = origin.y + (topUnits + statusHeight + gapUnits) * u;
    const float gridWidth = static_cast<float>(c.columns) * u;
    l.status = juce::Rectangle<float>(gridX, origin.y + topUnits * u, gridWidth, statusHeight * u).reduced(u * buttonInset, 0.0f);
    if (display)
    {
        l.display = juce::Rectangle<float>(origin.x, origin.y + displayButtonHeight * u, widthUnits * u, displayUnits * u);
        l.encoders = DisplayRenderer::layoutFor(l.display, c.encoders, c.cellsAcross()).cells;
    }
    const float cellWidth = l.display.getWidth() / static_cast<float>(std::max(1, c.cellsAcross()));
    if (c.touchStrip)
        l.strip = juce::Rectangle<float>(origin.x + (leftWidth + gapUnits) * u, gridY, stripWidth * u, static_cast<float>(c.rows) * u)
                      .reduced(u * buttonInset, u * padInset);

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
        else if (id.role == Role::displayUpper || id.role == Role::displayLower)
        {
            const float y = id.role == Role::displayUpper ? origin.y : l.display.getBottom();
            b.bounds = juce::Rectangle<float>(origin.x + static_cast<float>(id.index) * cellWidth, y, cellWidth, displayButtonHeight * u)
                           .reduced(u * buttonInset);
        }
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
    if (places.strip.contains(p))
        return {-1, -1, true};
    for (std::size_t i = 0; i < places.encoders.size(); ++i)
        if (places.encoders[i].contains(p))
            return {-1, -1, false, static_cast<int>(i)};
    return {};
}

float VirtualPush::stripAt(juce::Point<float> p) const
{
    const auto r = places.strip;
    return juce::jlimit(0.0f, 1.0f, (r.getBottom() - p.y) / std::max(1.0f, r.getHeight()));
}

float VirtualPush::pressureFor(juce::Point<float> pressedAt, juce::Point<float> p, float penPressure) const
{
    if (penPressure >= 0.0f)
        return juce::jlimit(0.0f, 1.0f, penPressure);
    return juce::jlimit(0.0f, 1.0f, (pressedAt.y - p.y) / std::max(1.0f, places.unit));
}

float VirtualPush::stepsForDrag(float pixels, bool fine) const
{
    const float range = std::max(1.0f, places.unit * dragUnitsPerRange);
    return pixels / range * static_cast<float>(caps.encoderResolution) / (fine ? 10.0f : 1.0f);
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

void VirtualPush::showText(const SurfaceText& t)
{
    words = t;
    repaint(); // the labels can change on every pad at once (a new layout)
}

void VirtualPush::showDisplay(const DisplayFrame& f)
{
    screen = f;
    repaint(places.display.getSmallestIntegerContainer().expanded(1));
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
    if (t.strip)
        return places.strip.getSmallestIntegerContainer().expanded(2);
    if (t.encoder >= 0)
        return places.display.getSmallestIntegerContainer().expanded(1);
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
    const float labelHeight = std::min(t.fontSmall, places.unit * 0.24f);
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
        if (i < words.pads.size() && words.pads[i].isNotEmpty() && labelHeight >= labelMinPixels)
        {
            const auto fill = ledColour(led, padOff, beat);
            g.setColour((fill.getPerceivedBrightness() > 0.6f ? juce::Colours::black : juce::Colour(0xffe8e8ea)).withAlpha(0.85f));
            g.setFont(t.font(labelHeight));
            g.drawFittedText(words.pads[i], r.reduced(r.getWidth() * 0.08f).toNearestInt(), juce::Justification::bottomLeft, 2, 0.6f);
        }
    }

    if (!places.display.isEmpty() && places.display.intersects(clip))
        DisplayRenderer::draw(g, screen, places.display, caps.cellsAcross(), t);

    if (!places.status.isEmpty() && places.status.intersects(clip))
    {
        g.setColour(t.text);
        g.setFont(t.font(std::min(t.fontNormal, places.status.getHeight() * 0.7f)));
        g.drawFittedText(words.status, places.status.toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
    }

    if (!places.strip.isEmpty() && places.strip.intersects(clip))
    {
        g.setColour(buttonOff);
        g.fillRoundedRectangle(places.strip, radius);
        const bool held = std::any_of(active.begin(), active.end(), [](const auto& a) { return a.second.strip; });
        const float h = std::max(2.0f, places.unit * 0.08f);
        const float y = juce::jlimit(places.strip.getY(), places.strip.getBottom() - h,
                                     places.strip.getBottom() - stripShown * places.strip.getHeight() - h * 0.5f);
        g.setColour(held ? t.focus : t.text.withMultipliedAlpha(0.5f));
        g.fillRoundedRectangle(places.strip.getX(), y, places.strip.getWidth(), h, h * 0.5f);
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
        else if (i < words.buttons.size() && words.buttons[i].isNotEmpty())
        {
            // What the mode makes it (a page, a category): smaller, as these are pad-sized.
            g.setFont(t.font(std::min(t.fontSmall, b.bounds.getHeight() * 0.3f)));
            g.drawFittedText(words.buttons[i], b.bounds.reduced(2.0f).toNearestInt(), juce::Justification::centred, 2, 0.6f);
        }
        else if (b.id.role != Role::scene && b.id.role != Role::trackSelect && b.id.role != Role::displayUpper &&
                 b.id.role != Role::displayLower) // the display labels those
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

void VirtualPush::press(const Target& t, float value)
{
    if (t.strip)
    {
        stripShown = value;
        deliver(SurfaceInput::strip(value));
    }
    else if (t.pad >= 0)
        deliver(SurfaceInput::padDown(t.pad / caps.columns, t.pad % caps.columns, value));
    else
        deliver(SurfaceInput::buttonDown(caps.buttons[static_cast<std::size_t>(t.button)].id));
    repaint(areaOf(t));
}

void VirtualPush::release(const Target& t)
{
    if (t.strip)
    {
        stripShown = 0.5f; // the marker goes back to the middle, as a bend does
        deliver(SurfaceInput::stripRelease());
    }
    else if (t.pad >= 0)
        deliver(SurfaceInput::padUp(t.pad / caps.columns, t.pad % caps.columns));
    else
        deliver(SurfaceInput::buttonUp(caps.buttons[static_cast<std::size_t>(t.button)].id));
    repaint(areaOf(t));
}

void VirtualPush::mouseDown(const juce::MouseEvent& e)
{
    const auto hit = hitTest(e.position);
    if (hit.encoder >= 0)
    {
        Target target;
        target.encoder = hit.encoder;
        target.lastY = e.position.y;
        active[e.source.getIndex()] = target;
        deliver(SurfaceInput::encoderTouch(hit.encoder));
        return;
    }
    if (hit.pad < 0 && hit.button < 0 && !hit.strip)
        return;
    const Target target{hit.pad, hit.button, hit.strip, e.position};
    if (hit.strip)
    {
        active[e.source.getIndex()] = target;
        press(target, stripAt(e.position));
        return;
    }
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

void VirtualPush::mouseDrag(const juce::MouseEvent& e)
{
    const auto it = active.find(e.source.getIndex());
    if (it == active.end())
        return;
    auto& target = it->second;
    if (target.encoder >= 0)
    {
        target.steps += stepsForDrag(target.lastY - e.position.y, e.mods.isShiftDown());
        target.lastY = e.position.y;
        if (const int whole = static_cast<int>(target.steps); whole != 0)
        {
            target.steps -= static_cast<float>(whole);
            deliver(SurfaceInput::encoderTurn(target.encoder, whole));
        }
        return;
    }
    if (target.strip)
    {
        stripShown = stripAt(e.position);
        deliver(SurfaceInput::strip(stripShown));
        repaint(areaOf(target));
    }
    else if (target.pad >= 0)
    {
        const float pressure = pressureFor(target.pressedAt, e.position, e.isPressureValid() ? e.pressure : -1.0f);
        if (const int step = juce::roundToInt(pressure * 127.0f); step != target.pressure)
        {
            target.pressure = step;
            deliver(SurfaceInput::padPressure(target.pad / caps.columns, target.pad % caps.columns, pressure));
        }
    }
}

void VirtualPush::mouseUp(const juce::MouseEvent& e)
{
    if (const auto it = active.find(e.source.getIndex()); it != active.end())
    {
        const auto target = it->second;
        active.erase(it);
        if (target.encoder >= 0)
            deliver(SurfaceInput::encoderRelease(target.encoder));
        else
            release(target);
    }
}

void VirtualPush::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto hit = hitTest(e.position);
    if (hit.encoder < 0 || wheel.deltaY == 0.0f)
    {
        juce::Component::mouseWheelMove(e, wheel);
        return;
    }
    // A notch (about a tenth) moves a fortieth of the range; at least a step.
    const float exact = wheel.deltaY * 0.25f * static_cast<float>(caps.encoderResolution) / (e.mods.isShiftDown() ? 10.0f : 1.0f);
    const int steps = exact > 0.0f ? std::max(1, juce::roundToInt(exact)) : std::min(-1, juce::roundToInt(exact));
    deliver(SurfaceInput::encoderTurn(hit.encoder, steps));
}

} // namespace lattice::control
