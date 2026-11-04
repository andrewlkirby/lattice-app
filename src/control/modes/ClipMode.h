#pragma once

#include "control/surface/Mode.h"

#include <vector>

namespace lattice::control
{

/** Clip mode (docs/06-m5-controllers.md, "Clip mode"): the encoders edit the selected clip (the
    Clip view's: the selected slot's, or the one its track plays) through the same bindings as the
    Clip view's panel, so a turn is the same undo step a drag there makes.

    - Its values come in groups: Loop (on, start, position, length, end), Launch (mode,
      quantization, legato, follow actions), Signature, and Quantize (the grid and the amount
      Quantize uses, and Fixed Length for recording). The groups are packed into pages as wide as
      the encoders; the upper display buttons choose the page.
    - The lower display buttons act on the clip: Double Loop (its notes copied after it), Double
      and Halve (the loop alone), Quantize, and Duplicate (into the next empty slot below).
    - A device without encoders gets its grid's columns as faders for the page's values above a
      row of the actions, the scene buttons choosing the page. */
class ClipMode final : public Mode
{
public:
    explicit ClipMode(ModeHost& host);

    ModeKind kind() const override { return ModeKind::clip; }
    bool handle(const SurfaceInput& input) override;
    void render(LedFrame& frame) override;
    void describe(SurfaceText& text) override;
    void encoderTargets(std::vector<juce::String>& targets) override;
    void display(DisplayFrame& frame) override;
    std::uint32_t aspects() const override { return ui::EngineStateFeed::transportAspect | ui::EngineStateFeed::slotsAspect; }

    /** The track and the clip it edits (-1 / invalid: none). */
    int track() const;
    juce::ValueTree clip() const;

    struct Page
    {
        juce::String name;
        std::vector<juce::String> values; // clipValues IDs, or "set/<property>"
    };
    /** The pages for this surface: as many values to a page as it has encoders (or grid columns). */
    std::vector<Page> pages() const;
    int page() const;
    void setPage(int page);
    /** The address of one of a page's values for the clip ("" for a clip value with no clip). */
    juce::String targetFor(const juce::String& value) const;

    /** What the lower buttons (or, without them, the grid's bottom row) do. */
    enum class Action
    {
        duplicateLoop,
        doubleLoop,
        halveLoop,
        quantize,
        duplicate
    };
    static const std::vector<std::pair<Action, juce::String>>& actions();
    void perform(Action action);

private:
    bool padsOwned() const { return caps().encoders <= 0; }
    int controls() const;
    int faderRows() const { return std::max(1, caps().rows - 1); }

    int shownPage = 0;
};

} // namespace lattice::control
