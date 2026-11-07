#include "hosting/clap/ClapEditor.h"

#include "hosting/clap/ClapInstance.h"

#include <clap/clap.h>

#include <cmath>
#include <cstring>

namespace lattice::hosting
{

namespace
{
bool usesPhysicalPixels(const char* api)
{
    return api != nullptr && std::strcmp(api, CLAP_WINDOW_API_WIN32) == 0;
}
} // namespace

ClapEditor::ClapEditor(ClapPluginInstance& o, const char* windowApi) : juce::AudioProcessorEditor(o), owner(o), api(windowApi)
{
    setOpaque(true);
    const auto* gui = owner.gui;
    const auto* plugin = owner.plugin;
    created = gui != nullptr && api != nullptr && gui->create(plugin, api, false);
    if (!created)
    {
        setSize(420, 80);
        return;
    }
    host = makeNativeHost(hostWindow);
    if (host != nullptr)
        addAndMakeVisible(*host);

    currentScale = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay() != nullptr
                       ? juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->scale
                       : 1.0;
    if (usesPhysicalPixels(api))
        gui->set_scale(plugin, currentScale);
    resizable = gui->can_resize(plugin);
    setResizable(resizable, false);
    std::uint32_t w = 0, h = 0;
    if (gui->get_size(plugin, &w, &h))
        setSizeFromPlugin(w, h);
    else
        setSize(640, 480);
}

ClapEditor::~ClapEditor()
{
    watchPeer(nullptr);
    if (created)
    {
        owner.gui->hide(owner.plugin);
        owner.gui->destroy(owner.plugin); // before its parent window goes (with `host`)
    }
}

double ClapEditor::scale() const
{
    return usesPhysicalPixels(api) ? currentScale : 1.0;
}

void ClapEditor::setSizeFromPlugin(std::uint32_t width, std::uint32_t height)
{
    const juce::ScopedValueSetter<bool> applying(applyingPluginSize, true);
    const double s = scale();
    setSize(juce::jmax(1, static_cast<int>(std::lround(width / s))), juce::jmax(1, static_cast<int>(std::lround(height / s))));
}

void ClapEditor::pluginAskedForSize(std::uint32_t width, std::uint32_t height)
{
    setSizeFromPlugin(width, height);
}

void ClapEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black);
    if (!created)
    {
        g.setColour(juce::Colours::white);
        g.drawFittedText(owner.getName() + "'s window couldn't be opened.", getLocalBounds().reduced(8),
                         juce::Justification::centred, 2);
    }
}

void ClapEditor::resized()
{
    if (host != nullptr)
        host->setBounds(getLocalBounds());
    if (!attached || !resizable || applyingPluginSize)
        return;
    const double s = scale();
    auto w = static_cast<std::uint32_t>(std::lround(getWidth() * s));
    auto h = static_cast<std::uint32_t>(std::lround(getHeight() * s));
    const auto* gui = owner.gui;
    gui->adjust_size(owner.plugin, &w, &h);
    if (gui->set_size(owner.plugin, w, h))
    {
        const int lw = static_cast<int>(std::lround(w / s)), lh = static_cast<int>(std::lround(h / s));
        if (lw != getWidth() || lh != getHeight())
            setSizeFromPlugin(w, h); // it only takes some sizes
    }
}

void ClapEditor::parentHierarchyChanged()
{
    attachIfReady();
}

void ClapEditor::visibilityChanged()
{
    attachIfReady();
}

void ClapEditor::attachIfReady()
{
    auto* peer = getPeer();
    watchPeer(peer);
    if (!created || attached || peer == nullptr || !isShowing() || hostWindow == nullptr)
        return;
    const auto* gui = owner.gui;
    const auto* plugin = owner.plugin;
    if (usesPhysicalPixels(api))
    {
        const double s = peer->getPlatformScaleFactor() * juce::Component::getApproximateScaleFactorForComponent(this);
        if (std::abs(s - currentScale) > 1e-6)
        {
            currentScale = s;
            gui->set_scale(plugin, s);
        }
    }
    clap_window_t window{};
    window.api = api;
#if JUCE_LINUX || JUCE_BSD
    window.x11 = static_cast<clap_xwnd>(reinterpret_cast<std::uintptr_t>(hostWindow));
#else
    window.ptr = hostWindow;
#endif
    attached = gui->set_parent(plugin, &window);
    if (!attached)
        return;
    std::uint32_t w = 0, h = 0;
    if (gui->get_size(plugin, &w, &h))
        setSizeFromPlugin(w, h);
    gui->show(plugin);
}

void ClapEditor::watchPeer(juce::ComponentPeer* peer)
{
    if (peer == watchedPeer)
        return;
    if (watchedPeer != nullptr)
        watchedPeer->removeScaleFactorListener(this);
    watchedPeer = peer;
    if (watchedPeer != nullptr)
        watchedPeer->addScaleFactorListener(this);
}

void ClapEditor::nativeScaleFactorChanged(double)
{
    // Moved to a monitor with another scale: the plugin draws for it, at the same logical size.
    if (!attached || !usesPhysicalPixels(api) || watchedPeer == nullptr)
        return;
    const double s = watchedPeer->getPlatformScaleFactor() * juce::Component::getApproximateScaleFactorForComponent(this);
    if (std::abs(s - currentScale) < 1e-6)
        return;
    currentScale = s;
    owner.gui->set_scale(owner.plugin, s);
    std::uint32_t w = 0, h = 0;
    if (owner.gui->get_size(owner.plugin, &w, &h))
        setSizeFromPlugin(w, h);
}

} // namespace lattice::hosting
