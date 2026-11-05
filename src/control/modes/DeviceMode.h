#pragma once

#include "control/surface/Mode.h"

#include <map>
#include <vector>

namespace lattice::control
{

/** Device mode (docs/06-m5-controllers.md, "Device mode"): the encoders control the selected
    device's parameters, a page at a time, paged for the device's encoder count (ParameterPager;
    a rack's macros in use, any count).

    - The device is the shared selection's (the Device view selects the same one); a track with
      none selected shows its first device.
    - The upper display buttons choose a device in its chain. Pressing the selected rack again
      shows the rack's chains on them; choosing a chain goes into it. Inside a rack the first
      button goes back up ("< Rack"). More devices than buttons: the buttons show the group the
      selected one is in.
    - The lower display buttons: the first turns the device on and off, the others choose pages.
    - The display shows each encoder's parameter, its value and a knob, with the devices above
      and the pages below; a touched encoder is shown large with its range.
    - A device without encoders gets its grid's columns as faders for the page's first
      parameters, the scene buttons choosing pages and the arrows moving along the chain (down
      into a rack, up out of it). */
class DeviceMode final : public Mode
{
public:
    explicit DeviceMode(ModeHost& host);

    ModeKind kind() const override { return ModeKind::device; }
    bool handle(const SurfaceInput& input) override;
    void render(LedFrame& frame) override;
    void describe(SurfaceText& text) override;
    void encoderTargets(std::vector<juce::String>& targets) override;
    void display(DisplayFrame& frame) override;
    std::uint32_t aspects() const override { return ui::EngineStateFeed::transportAspect; } // the Play button

    /** The TRACK, RETURN or MASTER node whose devices it shows (the shared selection's). */
    juce::ValueTree owner() const;
    /** The device it controls (invalid: none). */
    juce::ValueTree device() const;
    void selectDevice(const juce::ValueTree& device);

    /** The device's pages for this surface: as many parameters to a page as it has encoders
        (or grid columns, without encoders). */
    std::vector<ParameterGroup> pages() const;
    int page() const;
    void setPage(int page);
    /** How many controls a page has. */
    int controls() const;

    /** The rack whose chains the upper buttons show (invalid: they show devices). */
    const juce::ValueTree& chainsShown() const noexcept { return chainsOf; }
    /** Into the selected rack (its chains on the upper buttons), or out of the rack the device is in. */
    void enterRack();
    void leaveRack();

    /** What the upper buttons show, all of them (the buttons show a group of them). */
    struct Item
    {
        enum class Kind
        {
            device,
            chain,
            back
        };
        Kind kind = Kind::device;
        juce::ValueTree node;
        juce::String label;
        bool selected = false;
    };
    std::vector<Item> items() const;
    /** The first item on the upper buttons now. */
    int firstItem() const;

private:
    bool padsOwned() const { return caps().encoders <= 0; }
    juce::ValueTree level() const; // the DEVICES node the device is in
    juce::String parameterId(const juce::ValueTree& device, int index) const;
    juce::String nameOf(const juce::ValueTree& device) const;
    void pressItem(int index);
    void pressLower(int index);
    int firstPage() const;     // the first page on the lower buttons
    int pageButtons() const;   // how many lower buttons choose pages
    void step(int direction);  // along the chain

    juce::ValueTree chainsOf;  // a rack whose chains are shown
    juce::ValueTree emptyChain; // the DEVICES of an empty chain gone into
    std::map<std::int64_t, int> pageOf; // device ID -> its page
};

} // namespace lattice::control
