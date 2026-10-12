#include "platform/Platform.h"

namespace lattice::platform::detail
{

XdgDirs resolveXdgDirs(const EnvLookup& env, const std::string& home)
{
    // The spec says empty or relative values are invalid and the default must be used instead.
    const auto dir = [&](const char* variable, const char* fallbackUnderHome) -> std::string {
        if (const auto value = env(variable); value && !value->empty() && value->front() == '/')
            return *value;
        auto base = home;
        if (!base.empty() && base.back() == '/')
            base.pop_back();
        return base + "/" + fallbackUnderHome;
    };

    return {dir("XDG_DATA_HOME", ".local/share") + "/Lattice", dir("XDG_CONFIG_HOME", ".config") + "/Lattice"};
}

} // namespace lattice::platform::detail
