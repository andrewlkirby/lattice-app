#pragma once

#include "control/surface/Surface.h"
#include "ui/feed/EngineStateFeed.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <map>
#include <memory>
#include <vector>

namespace lattice::control
{

/** Virtual Push (docs/06-m5-controllers.md, "Virtual Push"): an on-screen device with every role, the
    reference driver that the modes are built and checked on before any hardware.

    Its grid is rows x columns (4 to 16 each), with a scene button per row on the right, a track
    button per column underneath, and the other buttons in two columns on the left. Everything is
    sized from the space it has: nothing is a fixed pixel size.

    - Mouse: a pad's velocity comes from where it's clicked (top: loudest), or is fixed. Dragging
      up from a held pad presses it harder (pressure; a pen's own pressure if it has one). A
      right-click latches a pad or a button (pressed until right-clicked again), so Shift, Delete
      or a held pad work with one mouse. Each touch on a touchscreen is its own press.
    - The touch strip, left of the grid: dragging on it bends (springing back when let go), or
      what the Preferences set.
    - With encoders, a display above the grid (DisplayRenderer), its cells the encoders: pressing
      one touches it, dragging up or down turns it (a value's whole range in three pads' height;
      with Shift held, ten times finer), and the mouse wheel turns it. A button above and below
      each column of cells.
    - Pads show their labels (a note's index or name) and a status line sits above the grid.
    - It repaints only the lights that changed. Blinking and pulsing lights follow the beat
      (the feed's transport while playing; the tempo otherwise) on the display's refresh, and
      only while one is shown, so a still set costs nothing.

    Message thread. */
class VirtualPush final : public juce::Component, public SurfaceDriver
{
public:
    struct Settings
    {
        int rows = 8;
        int columns = 8;
        int fixedVelocity = 0; // 1..127; 0: from where the pad is clicked
        int encoders = 8;      // 0 (none), 8, 16 or 32: a display's cells

        bool operator==(const Settings&) const = default;
    };

    VirtualPush(ui::EngineStateFeed& feed, Settings settings);
    ~VirtualPush() override;

    const Settings& settings() const noexcept { return config; }
    /** The encoder count Virtual Push offers nearest to n: 0, 8, 16 or 32. */
    static int encoderCountFor(int n) noexcept;

    // SurfaceDriver
    const SurfaceCapabilities& capabilities() const override { return caps; }
    void showLeds(const LedFrame& frame, const LedFrame::Changes& changed) override;

    /** Where everything is, for the bounds it has (plain arithmetic, so tests check it). */
    struct Layout
    {
        struct Button
        {
            ButtonId id;
            juce::String label;
            juce::Rectangle<float> bounds;
        };
        std::vector<juce::Rectangle<float>> pads; // row by row, from the top
        std::vector<Button> buttons;              // in the order of the capabilities' buttons
        juce::Rectangle<float> strip;             // the touch strip, left of the grid (if it has one)
        juce::Rectangle<float> status;            // the status line, above the grid
        juce::Rectangle<float> display;           // above the status line (if it has encoders)
        std::vector<juce::Rectangle<float>> encoders; // the display's cells
        float unit = 0.0f;                        // a pad and its gap
        juce::Rectangle<float> used;              // all of it (the room left over is below)
    };
    static Layout layoutFor(const SurfaceCapabilities& caps, juce::Rectangle<float> bounds);
    const Layout& layout() const noexcept { return places; }

    /** What's at a point: a pad index, a button index or an encoder (-1 for none), or the strip. */
    struct Hit
    {
        int pad = -1;
        int button = -1;
        bool strip = false;
        int encoder = -1;
    };
    Hit hitTest(juce::Point<float> p) const;
    /** The velocity a click at p gives the pad (0..1]. */
    float velocityAt(int pad, juce::Point<float> p) const;
    /** The strip's position at a point: 0 at its bottom, 1 at its top. */
    float stripAt(juce::Point<float> p) const;
    /** A held pad's pressure, dragging from where it was pressed to p: a pen's own pressure, or
        how far up it's dragged (a pad's height is all of it). */
    float pressureFor(juce::Point<float> pressedAt, juce::Point<float> p, float penPressure) const;
    /** The encoder steps a drag of `pixels` upwards turns (fine: ten times fewer). */
    float stepsForDrag(float pixels, bool fine) const;

    // SurfaceDriver
    void showText(const SurfaceText& text) override;
    const SurfaceText& text() const noexcept { return words; }
    void showDisplay(const DisplayFrame& frame) override;
    const DisplayFrame& displayed() const noexcept { return screen; }

    bool isLatched(int padOrButton, bool isButton) const;
    /** True while an animated light is shown (the display refresh repaints them). */
    bool isAnimating() const noexcept { return animator != nullptr; }
    const LedFrame& leds() const noexcept { return frame; }

    void paint(juce::Graphics&) override;
    void resized() override;
    void lookAndFeelChanged() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void visibilityChanged() override { updateAnimator(); }
    void parentHierarchyChanged() override { updateAnimator(); }

    /** The short label on a button ("Shift", "Session", "Scene 3"). */
    static juce::String labelFor(ButtonId id);

private:
    struct Target
    {
        int pad = -1;
        int button = -1;
        bool strip = false;
        juce::Point<float> pressedAt{};
        int pressure = 0; // the last sent, 0..127
        int encoder = -1;
        float lastY = 0.0f;
        float steps = 0.0f; // dragged but not yet sent (less than a step)
    };
    void press(const Target& t, float velocity);
    void release(const Target& t);
    juce::Rectangle<int> areaOf(const Target& t) const;
    void updateAnimator();
    double beatNow() const;
    juce::Colour ledColour(const LedState& led, juce::Colour off, double beat) const;

    ui::EngineStateFeed& feed;
    Settings config;
    SurfaceCapabilities caps;
    Layout places;
    LedFrame frame;
    SurfaceText words;
    DisplayFrame screen;
    float stripShown = 0.5f; // where the strip's marker is
    std::map<int, Target> active; // by mouse source: what it's pressing
    std::vector<Target> latched;
    std::unique_ptr<juce::VBlankAttachment> animator;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VirtualPush)
};

} // namespace lattice::control
