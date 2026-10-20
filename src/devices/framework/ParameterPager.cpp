#include "devices/framework/ParameterPager.h"

namespace lattice
{

std::vector<ParameterGroup> ParameterPager::pages(const std::vector<ParameterGroup>& groups, int encoders)
{
    encoders = std::max(encoders, 1);
    const auto pageSize = static_cast<std::size_t>(encoders);
    std::vector<ParameterGroup> result;
    ParameterGroup open; // a page that small groups are still being added to
    juce::StringArray openNames;

    const auto closeOpen = [&] {
        if (!open.parameters.empty())
        {
            open.name = openNames.joinIntoString(" / ");
            result.push_back(std::move(open));
        }
        open = {};
        openNames.clear();
    };

    for (const auto& group : groups)
    {
        if (group.parameters.empty())
            continue;

        if (group.parameters.size() > pageSize)
        {
            closeOpen();
            const auto numPages = (group.parameters.size() + pageSize - 1) / pageSize;
            for (std::size_t p = 0; p < numPages; ++p)
            {
                ParameterGroup page;
                page.name = group.name + " " + juce::String(static_cast<int>(p + 1));
                const auto from = p * pageSize;
                const auto to = std::min(from + pageSize, group.parameters.size());
                page.parameters.assign(group.parameters.begin() + static_cast<std::ptrdiff_t>(from),
                                       group.parameters.begin() + static_cast<std::ptrdiff_t>(to));
                result.push_back(std::move(page));
            }
            continue;
        }

        if (open.parameters.size() + group.parameters.size() > pageSize)
            closeOpen();
        open.parameters.insert(open.parameters.end(), group.parameters.begin(), group.parameters.end());
        openNames.add(group.name);
    }
    closeOpen();
    return result;
}

std::vector<ParameterGroup> ParameterPager::pagesFor(const StudioDevice& device, int encoders)
{
    auto custom = device.customPages(encoders);
    return custom.empty() ? pages(device.parameterGroups(), encoders) : custom;
}

} // namespace lattice
