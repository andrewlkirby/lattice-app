#include "platform/Platform.h"

#include <catch2/catch_test_macros.hpp>

#include <map>

using namespace lattice::platform;

namespace
{
detail::EnvLookup envFrom(std::map<std::string, std::string> values)
{
    return [values = std::move(values)](const char* name) -> std::optional<std::string> {
        if (const auto it = values.find(name); it != values.end())
            return it->second;
        return std::nullopt;
    };
}
} // namespace

TEST_CASE("XDG folders default to ~/.local/share and ~/.config", "[platform][paths]")
{
    const auto dirs = detail::resolveXdgDirs(envFrom({}), "/home/ada");
    CHECK(dirs.userData == "/home/ada/.local/share/Lattice");
    CHECK(dirs.settings == "/home/ada/.config/Lattice");
}

TEST_CASE("XDG_DATA_HOME and XDG_CONFIG_HOME are followed", "[platform][paths]")
{
    const auto dirs = detail::resolveXdgDirs(
        envFrom({{"XDG_DATA_HOME", "/data/ada"}, {"XDG_CONFIG_HOME", "/cfg/ada"}}), "/home/ada/");
    CHECK(dirs.userData == "/data/ada/Lattice");
    CHECK(dirs.settings == "/cfg/ada/Lattice");
}

TEST_CASE("Empty or relative XDG values fall back to the defaults", "[platform][paths]")
{
    const auto dirs = detail::resolveXdgDirs(
        envFrom({{"XDG_DATA_HOME", ""}, {"XDG_CONFIG_HOME", "relative/cfg"}}), "/home/ada");
    CHECK(dirs.userData == "/home/ada/.local/share/Lattice");
    CHECK(dirs.settings == "/home/ada/.config/Lattice");
}

TEST_CASE("Default paths end in Lattice on this OS", "[platform][paths]")
{
    const auto paths = defaultPaths();
    CHECK(paths.userData.getFileName() == "Lattice");
    CHECK(paths.settings.getFileName() == "Lattice");
    CHECK(paths.logs().getParentDirectory() == paths.userData);
    CHECK(paths.settingsFile().getParentDirectory() == paths.settings);
#if JUCE_WINDOWS
    CHECK(paths.userData.getFullPathName().containsIgnoreCase("AppData"));
#endif
}

TEST_CASE("Preferred audio device types are listed", "[platform]")
{
    CHECK_FALSE(preferredAudioDeviceTypes().isEmpty());
}

TEST_CASE("Process start time is in the past", "[platform]")
{
    const double t = secondsSinceProcessStart();
    CHECK(t > 0.0);
    CHECK(t < 3600.0);
}
