#include <catch2/catch_session.hpp>
#include <juce_events/juce_events.h>

int main(int argc, char* argv[])
{
    // Some JUCE classes (e.g. AudioDeviceManager) expect a MessageManager to exist.
    const juce::ScopedJuceInitialiser_GUI juceInit;
    return Catch::Session().run(argc, argv);
}
