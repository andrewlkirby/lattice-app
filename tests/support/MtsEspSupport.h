#pragma once

// MTS-ESP in the tests (docs/07-m6-plugins-external.md, 6c): the SDK's copy of libMTS, and taking
// turns with other test processes, since a machine has one master.

#include "tuning/external/MtsEsp.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>

namespace lattice::test
{

/** The SDK's libMTS, which the tests' master and the test plugin's client both load. */
inline juce::File mtsLibraryFile()
{
    return juce::File(LATTICE_TEST_MTS_LIBRARY);
}

/** For as long as it lives: this process's turn at MTS-ESP, and the test plugin told where libMTS
    is (it reads LATTICE_TEST_MTS_LIBRARY when it first loads). ours() is false, with a warning, if
    the library didn't load or something else is the master: Lattice running, another program, or
    a test process that crashed while it was the master. Tests then stop there (not SKIP, whose
    exception crashed while unwinding in some MSVC Release layouts). */
class MtsEspTurn
{
public:
    MtsEspTurn() : lock("LatticeMtsEspTests")
    {
        if (!lock.enter(120000))
        {
            WARN("Another test process kept MTS-ESP for two minutes");
            return;
        }
        locked = true;
        lib = mts::Library::at(mtsLibraryFile());
        if (lib == nullptr)
        {
            WARN("libMTS didn't load from " << mtsLibraryFile().getFullPathName());
            return;
        }
        if (lib->hasMaster())
        {
            WARN("Something else is the MTS-ESP master (Lattice, another program, or a test process that crashed): not tested");
            lib = nullptr;
            return;
        }
        setEnvironment(mtsLibraryFile().getFullPathName());
    }
    ~MtsEspTurn()
    {
        if (locked)
            lock.exit();
    }

    bool ours() const noexcept { return lib != nullptr; }
    mts::Library& library() const noexcept { return *lib; }

private:
    static void setEnvironment(const juce::String& path)
    {
#if JUCE_WINDOWS
        _putenv_s("LATTICE_TEST_MTS_LIBRARY", path.toRawUTF8());
#else
        setenv("LATTICE_TEST_MTS_LIBRARY", path.toRawUTF8(), 1);
#endif
    }

    juce::InterProcessLock lock;
    bool locked = false;
    mts::Library* lib = nullptr;
};

} // namespace lattice::test
