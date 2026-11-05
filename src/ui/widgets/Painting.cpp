#include "ui/widgets/Painting.h"

#if JUCE_WINDOWS
// Rather than all of windows.h: the one call used.
extern "C" __declspec(dllimport) int __stdcall UpdateWindow(void* window);
#endif

namespace lattice::ui::painting
{

bool cachedImagesPay(const juce::Component& c)
{
    auto* peer = c.getPeer();
    if (peer == nullptr)
        return true;
    const auto engines = peer->getAvailableRenderingEngines();
    const int current = peer->getCurrentRenderingEngine();
    return !juce::isPositiveAndBelow(current, engines.size()) || engines[current] != "Direct2D";
}

void bufferWhereItPays(juce::Component& c)
{
    const bool pays = cachedImagesPay(c);
    if (pays != (c.getCachedComponentImage() != nullptr))
        c.setBufferedToImage(pays);
}

void paintAtNextRefresh(juce::Component& c)
{
#if JUCE_WINDOWS
    if (auto* peer = c.getPeer())
        UpdateWindow(peer->getNativeHandle()); // WM_PAINT now, if anything is to be painted
#else
    juce::ignoreUnused(c);
#endif
}

} // namespace lattice::ui::painting
