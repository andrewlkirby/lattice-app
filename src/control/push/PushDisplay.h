#pragma once

#include "core/LatencyStats.h"
#include <juce_core/juce_core.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace lattice::control::push
{

/** Why the display isn't showing, or that it is. */
enum class DisplayStatus : std::uint8_t
{
    starting,     // not tried yet
    connected,    // frames are going
    notFound,     // no Push on USB (or not yet)
    noDriver,     // Windows: no WinUSB driver on its display interface
    noPermission, // Linux: no udev rule giving this user the device
    busy,         // another program (Live) has it
    failed,       // anything else libusb says
    unsupported   // built without libusb
};
/** What to tell the user ("Push's display needs..."); empty for starting and connected. */
juce::String describe(DisplayStatus status);

/** The display's end of USB: bulk writes to its endpoint. */
class DisplayLink
{
public:
    virtual ~DisplayLink() = default;
    /** False if the write failed (unplugged): the link is then let go and opened again. */
    virtual bool write(const std::uint8_t* data, int size) = 0;
};

struct OpenedLink
{
    std::unique_ptr<DisplayLink> link;
    DisplayStatus status = DisplayStatus::failed;
};
using LinkOpener = std::function<OpenedLink()>;

/** Push's display through libusb: the device with Ableton's vendor ID and `productId`, interface 0
    claimed (Display Interface: USB Display Interface Access). */
OpenedLink openUsbDisplay(int productId);
/** The udev rule Linux needs (also resources/linux/udev/50-lattice-ableton-push.rules). */
juce::String udevRule();

/** Sends frames to the display on its own thread (docs/06-m5-controllers.md, "Display"): the
    newest frame posted, as soon as the last is out but no more than 60 a second, and the same frame
    again each second (the display goes black after 2 s without one). Opens the link when it can
    and again after it's lost (Push unplugged and plugged in again): a missing driver or rule shows
    in status(). The message thread posts; nothing else waits on USB. */
class DisplaySender
{
public:
    explicit DisplaySender(LinkOpener opener, double resendSeconds = 1.0, double retrySeconds = 2.0);
    ~DisplaySender();

    /** A frame's 327,680 bytes, after the header (encodeFrame()). Replaces one not sent yet. */
    void post(std::vector<std::uint8_t> frame);
    DisplayStatus status() const noexcept { return state.load(); }
    /** Frames sent so far (resends too). */
    std::uint64_t framesSent() const noexcept { return sent.load(); }
    /** From a frame posted to the display showing it (or a newer one), in ms: the wait for its
        slot (60 a second at most) and the USB writes (docs/06-m5-controllers.md, 5g). */
    const LatencyStats& delays() const noexcept { return shownAfter; }
    LatencyStats& delays() noexcept { return shownAfter; }

    static constexpr int chunkBytes = 16384; // pixel data in 16 KB writes, as the manual suggests
    static constexpr double minFrameSeconds = 1.0 / 60.0;

private:
    void run();
    bool sendFrame(DisplayLink& link, const std::vector<std::uint8_t>& frame);

    LinkOpener open;
    const double resend, retry;
    std::mutex lock;
    std::condition_variable wake;
    std::vector<std::uint8_t> next;    // posted, not sent yet
    bool hasNext = false;
    std::chrono::steady_clock::time_point postedAt; // the oldest post not shown yet
    LatencyStats shownAfter;
    bool quit = false;
    std::atomic<DisplayStatus> state{DisplayStatus::starting};
    std::atomic<std::uint64_t> sent{0};
    std::thread thread;
};

} // namespace lattice::control::push
