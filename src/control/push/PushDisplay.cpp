#include "control/push/PushDisplay.h"

#include "control/push/PushProtocol.h"

#include <chrono>

namespace lattice::control::push
{

juce::String describe(DisplayStatus status)
{
    switch (status)
    {
    case DisplayStatus::starting:
    case DisplayStatus::connected: return {};
    case DisplayStatus::notFound: return "Push's display isn't on USB (still starting, or a USB hub that only passes its MIDI).";
    case DisplayStatus::noDriver:
        return "Push's display needs the WinUSB driver. Installing Ableton Live (or Push's driver from Ableton's website) installs it; "
               "otherwise Zadig can install WinUSB for \"Ableton Push 2 (Interface 0)\". Then unplug Push and plug it in again.";
    case DisplayStatus::noPermission:
        return "Linux doesn't let Lattice open Push's display: copy resources/linux/udev/50-lattice-ableton-push.rules to "
               "/etc/udev/rules.d/ (or write this line there: " +
               udevRule() + "), run \"sudo udevadm control --reload\", then unplug Push and plug it in again.";
    case DisplayStatus::busy: return "Another program (Ableton Live?) is using Push's display. Quit it, or turn Push off and on again.";
    case DisplayStatus::failed: return "Push's display couldn't be opened over USB.";
    case DisplayStatus::unsupported: return "This build of Lattice has no USB support, so Push's display stays dark.";
    }
    return {};
}

juce::String udevRule()
{
    const auto id = [](int v) { return juce::String::toHexString(v).paddedLeft('0', 4); };
    juce::String rule;
    for (const int product : {push2ProductId, push3ProductId})
        rule << "SUBSYSTEM==\"usb\", ATTR{idVendor}==\"" << id(vendorId) << "\", ATTR{idProduct}==\"" << id(product)
             << "\", TAG+=\"uaccess\"\n";
    return rule.trimEnd();
}

DisplaySender::DisplaySender(LinkOpener opener, double resendSeconds, double retrySeconds)
    : open(std::move(opener)), resend(resendSeconds), retry(retrySeconds)
{
    thread = std::thread([this] { run(); });
}

DisplaySender::~DisplaySender()
{
    {
        const std::scoped_lock l(lock);
        quit = true;
    }
    wake.notify_all();
    thread.join();
}

void DisplaySender::post(std::vector<std::uint8_t> frame)
{
    {
        const std::scoped_lock l(lock);
        next = std::move(frame);
        hasNext = true;
    }
    wake.notify_all();
}

bool DisplaySender::sendFrame(DisplayLink& link, const std::vector<std::uint8_t>& frame)
{
    if (!link.write(frameHeader.data(), static_cast<int>(frameHeader.size())))
        return false;
    for (std::size_t at = 0; at < frame.size(); at += chunkBytes)
        if (!link.write(frame.data() + at, static_cast<int>(std::min<std::size_t>(chunkBytes, frame.size() - at))))
            return false;
    ++sent;
    return true;
}

void DisplaySender::run()
{
    juce::Thread::setCurrentThreadName("Push display");
    using Clock = std::chrono::steady_clock;
    const auto seconds = [](double s) { return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(s)); };
    std::unique_ptr<DisplayLink> link;
    std::vector<std::uint8_t> shown; // the frame on the display (resent while nothing new comes)
    auto lastSent = Clock::now() - seconds(60.0);
    auto lastTry = Clock::now() - seconds(60.0);

    std::unique_lock l(lock);
    while (!quit)
    {
        const auto now = Clock::now();
        if (link == nullptr && now - lastTry >= seconds(retry))
        {
            l.unlock();
            auto opened = open ? open() : OpenedLink{nullptr, DisplayStatus::unsupported};
            l.lock();
            lastTry = Clock::now();
            link = std::move(opened.link);
            state = link != nullptr ? DisplayStatus::connected : opened.status;
            if (link != nullptr)
                lastSent = Clock::now() - seconds(60.0); // show the frame at once
            continue;
        }
        if (link != nullptr)
        {
            const bool fresh = hasNext && now - lastSent >= seconds(minFrameSeconds);
            const bool stale = !fresh && !hasNext && !shown.empty() && now - lastSent >= seconds(resend);
            if (fresh || stale || (hasNext && shown.empty()))
            {
                if (hasNext)
                {
                    shown.swap(next);
                    hasNext = false;
                }
                l.unlock();
                const bool ok = sendFrame(*link, shown);
                l.lock();
                lastSent = Clock::now();
                if (!ok)
                {
                    link.reset(); // gone: open it again (the frame goes again then)
                    state = DisplayStatus::notFound;
                    lastTry = Clock::now();
                }
                continue;
            }
        }
        // Sleep until something is due: the next frame's slot, a resend, or another try.
        auto until = now + seconds(link == nullptr ? retry : resend);
        if (link == nullptr)
            until = lastTry + seconds(retry);
        else if (hasNext)
            until = lastSent + seconds(minFrameSeconds);
        else if (!shown.empty())
            until = lastSent + seconds(resend);
        wake.wait_until(l, until);
    }
    l.unlock();
    link.reset();
}

} // namespace lattice::control::push
