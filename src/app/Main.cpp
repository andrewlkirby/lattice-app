#include "app/MainWindow.h"
#include "app/SoakTest.h"
#include "core/Profiling.h"
#include "core/StartupTimeline.h"
#include "engine/AudioEngine.h"
#include "platform/Platform.h"

#include <juce_gui_extra/juce_gui_extra.h>

namespace lattice
{

class LatticeApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }

    // A second instance would fight over the (often exclusive) audio device.
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise(const juce::String& commandLine) override
    {
        LATTICE_ZONE("startup: initialise");
        timeline.mark("app init");
        quitWhenReady = commandLine.contains("--quit-when-ready");
        runSoak = SoakTest::parseCommandLine(commandLine, soakOptions);

        paths = platform::defaultPaths();
        juce::PropertiesFile::Options options;
        options.applicationName = getApplicationName();
        options.storageFormat = juce::PropertiesFile::storeAsXML;
        options.millisecondsBeforeSaving = 500;
        settings = std::make_unique<juce::PropertiesFile>(paths.settingsFile(), options);
        timeline.mark("settings loaded");

        engine = std::make_unique<AudioEngine>();
        timeline.mark("engine created");

        // Opening the device can take over a second in the driver (WASAPI exclusive mode on the
        // reference laptop), so it runs in the background while the window is built.
        engine->openDeviceAsync(settings->getXmlValue("audioDeviceState"),
                                [this](const juce::String& error) { audioOpened(error); });
        timeline.mark("audio open started");

        mainWindow = std::make_unique<MainWindow>(getApplicationName(), *engine, *settings,
                                                  [this] { windowShown(); },
                                                  [this](const char* stage) { timeline.mark(stage); });
        timeline.mark("window created");

        if (runSoak)
            mainWindow->content().setPersistDeviceState(false); // the soak's buffer size is temporary
    }

    void shutdown() override
    {
        soak = nullptr;
        mainWindow = nullptr; // saves the device state
        engine = nullptr;
        if (settings != nullptr)
            settings->saveIfNeeded();
        settings = nullptr;
    }

    void systemRequestedQuit() override { quit(); }

    void anotherInstanceStarted(const juce::String&) override
    {
        if (mainWindow != nullptr)
            mainWindow->toFront(true);
    }

private:
    void windowShown()
    {
        timeline.mark("window shown");
        windowIsShown = true;
        markReadyIfDone();
    }

    void audioOpened(const juce::String& error)
    {
        timeline.mark("audio started");
        audioIsOpen = true;
        if (mainWindow != nullptr)
            mainWindow->content().audioDeviceOpened(error);
        markReadyIfDone();
    }

    void markReadyIfDone()
    {
        if (!windowIsShown || !audioIsOpen)
            return;

        timeline.mark("ready");
        const bool logged = timeline.appendTo(paths.logs().getChildFile("startup.log"));
        juce::ignoreUnused(logged);
        jassert(logged); // the startup log could not be written
        DBG("Startup: " << timeline.toLogLine());

        if (runSoak)
            startSoak();
        else if (quitWhenReady)
            quit();
    }

    void startSoak()
    {
        soak = std::make_unique<SoakTest>(*engine, soakOptions, paths.logs().getChildFile("soak.log"),
                                          [this](bool passed) {
                                              setApplicationReturnValue(passed ? 0 : 1);
                                              quit();
                                          });
        soak->start();
        mainWindow->content().setToneToggle(true);
    }

    StartupTimeline timeline;
    platform::Paths paths;
    bool quitWhenReady = false;
    bool runSoak = false;
    SoakTest::Options soakOptions;
    std::unique_ptr<SoakTest> soak;
    bool windowIsShown = false;
    bool audioIsOpen = false;
    std::unique_ptr<juce::PropertiesFile> settings;
    std::unique_ptr<AudioEngine> engine;
    std::unique_ptr<MainWindow> mainWindow;
};

} // namespace lattice

START_JUCE_APPLICATION(lattice::LatticeApplication)
