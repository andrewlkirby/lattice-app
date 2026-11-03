#include "control/surface/Presets.h"

#include <initializer_list>

namespace lattice::control::presets
{

namespace
{
void add(SurfaceCapabilities& caps, std::initializer_list<Role> roles, LedKind led = LedKind::single)
{
    for (const auto r : roles)
        caps.buttons.push_back({ButtonId{r, 0}, led});
}

void addNumbered(SurfaceCapabilities& caps, Role role, int count, LedKind led)
{
    for (int i = 0; i < count; ++i)
        caps.buttons.push_back({ButtonId{role, i}, led});
}
} // namespace

SurfaceCapabilities push2()
{
    SurfaceCapabilities c;
    c.name = "Ableton Push 2";
    c.rows = c.columns = 8;
    c.aftertouch = Aftertouch::polyphonic;
    c.encoders = 8;
    c.encoderTouch = true;
    c.tempoEncoder = true;
    c.displayWidth = 960;
    c.displayHeight = 160;
    c.touchStrip = true;
    // The buttons along the display select tracks in Session and Mix modes (displayLower).
    addNumbered(c, Role::displayUpper, 8, LedKind::rgb);
    addNumbered(c, Role::displayLower, 8, LedKind::rgb);
    addNumbered(c, Role::scene, 8, LedKind::rgb);
    add(c, {Role::play, Role::record, Role::newClip, Role::duplicate, Role::fixedLength, Role::quantize, Role::doubleLoop,
            Role::deleteClip, Role::undo, Role::mute, Role::solo, Role::stopClip},
        LedKind::rgb);
    add(c, {Role::shift, Role::modeSession, Role::modeNote, Role::modeDevice, Role::modeMix, Role::modeBrowse,
            Role::modeClip, Role::navUp, Role::navDown, Role::navLeft, Role::navRight, Role::octaveUp, Role::octaveDown,
            Role::layout, Role::scale, Role::metronome, Role::tapTempo});
    return c;
}

SurfaceCapabilities push3()
{
    auto c = push2();
    c.name = "Ableton Push 3";
    c.aftertouch = Aftertouch::mpe;
    return c;
}

SurfaceCapabilities launchpadProMk3()
{
    SurfaceCapabilities c;
    c.name = "Novation Launchpad Pro MK3";
    c.rows = c.columns = 8;
    c.aftertouch = Aftertouch::polyphonic;
    addNumbered(c, Role::scene, 8, LedKind::rgb);
    addNumbered(c, Role::trackSelect, 8, LedKind::rgb);
    add(c, {Role::shift, Role::navLeft, Role::navRight, Role::navUp, Role::navDown, Role::modeSession, Role::modeNote,
            Role::deleteClip, Role::duplicate, Role::quantize, Role::fixedLength, Role::play, Role::record, Role::mute,
            Role::solo, Role::modeMix, Role::modeDevice, Role::stopClip},
        LedKind::rgb);
    return c;
}

SurfaceCapabilities apc64()
{
    SurfaceCapabilities c;
    c.name = "Akai APC64";
    c.rows = c.columns = 8;
    c.aftertouch = Aftertouch::polyphonic;
    c.encoders = 8; // its touch strips, as absolute controls
    c.encodersRelative = false;
    c.encoderTouch = true;
    c.textLines = 2;
    c.textCharacters = 16;
    addNumbered(c, Role::scene, 8, LedKind::rgb);
    addNumbered(c, Role::trackSelect, 8, LedKind::rgb);
    add(c, {Role::shift, Role::play, Role::record, Role::navUp, Role::navDown, Role::navLeft, Role::navRight,
            Role::modeSession, Role::modeNote, Role::modeDevice, Role::modeMix, Role::mute, Role::solo, Role::stopClip,
            Role::deleteClip, Role::duplicate, Role::quantize, Role::undo, Role::fixedLength, Role::metronome,
            Role::tapTempo, Role::stopAll});
    return c;
}

SurfaceCapabilities apc40Mk2()
{
    SurfaceCapabilities c;
    c.name = "Akai APC40 MKII";
    c.rows = 5;
    c.columns = 8;
    c.velocity = false;
    c.encoders = 16;
    c.encodersRelative = false;
    c.tempoEncoder = true;
    addNumbered(c, Role::scene, 5, LedKind::rgb);
    addNumbered(c, Role::trackSelect, 8, LedKind::single);
    add(c, {Role::shift, Role::play, Role::record, Role::navUp, Role::navDown, Role::navLeft, Role::navRight,
            Role::modeSession, Role::metronome, Role::tapTempo, Role::stopAll, Role::modeDevice});
    return c;
}

SurfaceCapabilities maschineMk3()
{
    SurfaceCapabilities c;
    c.name = "Native Instruments Maschine MK3";
    c.rows = c.columns = 4;
    c.aftertouch = Aftertouch::polyphonic;
    c.encoders = 8;
    c.encoderTouch = true;
    c.displayWidth = 960; // two 480 x 272 displays side by side
    c.displayHeight = 272;
    c.touchStrip = true;
    addNumbered(c, Role::displayUpper, 8, LedKind::single);
    addNumbered(c, Role::trackSelect, 8, LedKind::rgb); // its group buttons
    add(c, {Role::shift, Role::play, Role::record, Role::navUp, Role::navDown, Role::navLeft, Role::navRight,
            Role::modeSession, Role::modeNote, Role::modeDevice, Role::modeMix, Role::modeBrowse, Role::duplicate,
            Role::deleteClip, Role::undo, Role::quantize, Role::fixedLength, Role::mute, Role::solo, Role::metronome,
            Role::tapTempo, Role::octaveUp, Role::octaveDown, Role::scale});
    return c;
}

SurfaceCapabilities virtualPush(int rows, int columns, int encoders)
{
    SurfaceCapabilities c;
    c.name = "Virtual Push";
    c.rows = juce::jlimit(minVirtualSize, maxVirtualSize, rows);
    c.columns = juce::jlimit(minVirtualSize, maxVirtualSize, columns);
    c.encoders = encoders;
    c.encoderTouch = true;
    c.tempoEncoder = true;
    c.aftertouch = Aftertouch::polyphonic; // dragging up from a held pad
    c.touchStrip = true;
    addNumbered(c, Role::scene, c.rows, LedKind::rgb);
    addNumbered(c, Role::trackSelect, c.columns, LedKind::rgb);
    for (int r = 0; r < static_cast<int>(Role::count); ++r)
        if (const auto role = static_cast<Role>(r); !isNumbered(role))
            c.buttons.push_back({ButtonId{role, 0}, LedKind::rgb});
    return c;
}

SurfaceCapabilities gridOnly(int rows, int columns)
{
    SurfaceCapabilities c;
    c.name = "Grid only";
    c.rows = rows;
    c.columns = columns;
    return c;
}

std::vector<SurfaceCapabilities> allForTests()
{
    return {push2(),           push3(),           launchpadProMk3(),      apc64(),
            apc40Mk2(),        maschineMk3(),     virtualPush(8, 8, 8),   virtualPush(8, 12, 16),
            virtualPush(16, 16, 32), virtualPush(4, 4, 8), gridOnly(8, 8), gridOnly(4, 4)};
}

} // namespace lattice::control::presets
