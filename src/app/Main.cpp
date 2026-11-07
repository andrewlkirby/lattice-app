#include "app/MainWindow.h"
#include "app/SoakTest.h"
#include "app/UiStress.h"
#include "core/Profiling.h"
#include "core/StartupTimeline.h"
#include "devices/dsp/Wavetable.h"
#include "engine/AudioEngine.h"
#include "engine/SessionController.h"
#include "platform/Platform.h"
#include "tuning/TuningRegistry.h"
#include "ui/prefs/AppSettings.h"

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
        // The synth's band-limited tables build in the background while the rest starts up.
        dsp::WaveformLibrary::preloadInBackground();
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
        // Parallel track processing: a worker per physical core, unless switched off in the settings.
        engine->setWorkerThreads(ui::prefs::workerThreads(*settings));
        ui::prefs::applyZoom(ui::prefs::zoom(*settings));
        ui::prefs::applyNewTrackDefaults(*settings); // new tracks' tuning and high notes
        timeline.mark("engine created");

        tunings = std::make_unique<TuningRegistry>();
        tunings->loadFolder(paths.userData.getChildFile("Tunings"));
        // What the tuning editor saved (docs/05-m4-ui.md, 4f): in the User Library, where the browser shows it.
        tunings->loadFolder(paths.userData.getChildFile("Library").getChildFile("Tunings"));
        ui::prefs::loadRememberedTunings(*tunings, *settings); // tuning files loaded from elsewhere
        timeline.mark("tunings loaded");

        // A new set, its tracks in the Preferences' tuning (a tuning file no longer there: 12-TET).
        if (tunings->findById(LiveSet::newTrackDefaults().tuningId) == nullptr)
            LiveSet::newTrackDefaults().tuningId = {};
        session = std::make_unique<SessionController>(*engine, *tunings);
        timeline.mark("session created");

        // Opening the device can take over a second in the driver (WASAPI exclusive mode on the
        // reference laptop), so it runs in the background while the window is built.
        engine->openDeviceAsync(settings->getXmlValue("audioDeviceState"),
                                [this](const juce::String& error) { audioOpened(error); });
        timeline.mark("audio open started");

        mainWindow = std::make_unique<MainWindow>(getApplicationName(), *engine, *settings, *tunings, *session,
                                                  [this] { windowShown(); },
                                                  [this](const char* stage) { timeline.mark(stage); });
        timeline.mark("window created");

        if (runSoak)
            mainWindow->content().setPersistDeviceState(false); // the soak's buffer size is temporary
    }

    void shutdown() override
    {
        uiStress = nullptr;
        soak = nullptr;
        mainWindow = nullptr; // saves the device state
        session = nullptr;    // the engine owns everything the audio thread reads, so it can keep running
        engine = nullptr;
        tunings = nullptr;
        if (settings != nullptr)
            settings->saveIfNeeded();
        settings = nullptr;
    }

    void systemRequestedQuit() override
    {
        if (mainWindow == nullptr || runSoak || quitWhenReady)
        {
            quit();
            return;
        }
        mainWindow->content().shell().files().confirmDiscardingChanges([this](bool proceed) {
            if (!proceed)
                return;
            mainWindow->content().shell().files().quitting(); // a normal quit: no recovery copy
            quit();
        });
    }

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
        else
        {
            mainWindow->content().shell().files().offerRecovery(); // after a crash
            // New and changed plugins, in the background, once everything else has started.
            mainWindow->content().shell().startPluginScan();
        }
    }

    void startSoak()
    {
        if (soakOptions.uiStress)
        {
            // The UI stress test: the set plays while the window is worked (UiStress).
            const auto result = session->loadSet(juce::File::getCurrentWorkingDirectory().getChildFile(soakOptions.setFile));
            if (soakOptions.setFile.isEmpty() || result.failed())
            {
                paths.logs().getChildFile("soak.log").appendText(juce::Time::getCurrentTime().toISO8601(true) +
                                                                 " | ui stress | could not open the set '" + soakOptions.setFile +
                                                                 "': " + result.getErrorMessage() + " | FAIL\n");
                setApplicationReturnValue(1);
                quit();
                return;
            }
            // Quiet, as the soak's tone is: the master fader at its level (the set renders the same).
            session->set().setMasterVolumeDb(soakOptions.levelDb);
            uiStress = std::make_unique<UiStress>(mainWindow->content().shell());
            uiStress->start();
        }
        soak = std::make_unique<SoakTest>(*engine, soakOptions, paths.logs().getChildFile("soak.log"),
                                          [this](bool passed) {
                                              setApplicationReturnValue(passed ? 0 : 1);
                                              quit();
                                          });
        if (uiStress != nullptr)
        {
            soak->setContext([this] { return uiStress->describe(); });
            soak->setReport([this] { return uiStress->report(); });
            soak->onMeasuring = [this] { uiStress->resetMeasures(); };
        }
        soak->start();
    }

    StartupTimeline timeline;
    platform::Paths paths;
    bool quitWhenReady = false;
    bool runSoak = false;
    SoakTest::Options soakOptions;
    std::unique_ptr<SoakTest> soak;
    std::unique_ptr<UiStress> uiStress;
    bool windowIsShown = false;
    bool audioIsOpen = false;
    std::unique_ptr<juce::PropertiesFile> settings;
    std::unique_ptr<AudioEngine> engine;
    std::unique_ptr<TuningRegistry> tunings;
    std::unique_ptr<SessionController> session;
    std::unique_ptr<MainWindow> mainWindow;
};

} // namespace lattice

START_JUCE_APPLICATION(lattice::LatticeApplication)
