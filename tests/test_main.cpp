#include "platform/Platform.h"

#include <catch2/catch_session.hpp>
#include <juce_events/juce_events.h>

int main(int argc, char* argv[])
{
    // Some JUCE classes (e.g. AudioDeviceManager) expect a MessageManager to exist.
    const juce::ScopedJuceInitialiser_GUI juceInit;
    // The window's library (presets, the browser's index) lives in a folder of the tests' own.
    const auto userData = juce::File::getSpecialLocation(juce::File::tempDirectory)
                              .getNonexistentChildFile("lattice-tests-userdata", "");
    lattice::platform::setUserDataFolderForTests(userData);
    const int result = Catch::Session().run(argc, argv);
    userData.deleteRecursively();
    return result;
}
