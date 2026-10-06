#pragma once

#include <map>
#include <optional>
#include <string>

#include <CloverNT/API/Plugin/Version.hpp>

namespace CloverNT::Plugin {

struct Dependency {
    std::string            name;
    std::optional<Version> version;

    [[nodiscard]] bool operator==(Dependency const&) const = default;
};

using DependencyMap = std::map<std::string, std::optional<Version>, std::less<>>;

struct Manifest {
    std::string                                     name;
    std::optional<Version>                          version;
    std::string                                     type;
    std::string                                     entry;
    std::optional<std::string>                      preloadEntry;
    std::optional<std::string>                      rendererEntry;
    std::optional<std::string>                      platform;
    bool                                            passive{false};
    std::optional<std::string>                      author;
    std::optional<std::string>                      description;
    std::map<std::string, std::string, std::less<>> extraInfo;
    DependencyMap                                   dependencies;
    DependencyMap                                   optionalDependencies;
    DependencyMap                                   conflicts;
    DependencyMap                                   loadBefore;
};

} // namespace CloverNT::Plugin
