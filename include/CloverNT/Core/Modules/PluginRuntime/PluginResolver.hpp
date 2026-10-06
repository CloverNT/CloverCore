#pragma once

#include <CloverNT/API/Expected.hpp>
#include <CloverNT/API/Plugin/Manifest.hpp>
#include <CloverNT/Core/Modules/PluginRuntime.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace CloverNT::Core::Modules::Resolver {

inline constexpr std::string_view CorePluginName = "CloverCore";

using ManifestMap = std::map<std::string, PluginRuntime::ManifestEntry, std::less<>>;
using NameSet     = std::set<std::string, std::less<>>;

struct LoadSet {
    NameSet     selected;
    std::string log;
};

[[nodiscard]] bool versionMatches(Plugin::Manifest const& real, std::optional<Plugin::Version> const& required);
[[nodiscard]] bool supportsPlatform(Plugin::Manifest const& manifest);
[[nodiscard]] bool manifestDependsOn(Plugin::Manifest const& manifest, std::string_view dependencyName);

[[nodiscard]] auto loadManifest(std::filesystem::path const& directory) -> Expected<Plugin::Manifest>;
[[nodiscard]] auto makeCoreManifest() -> Plugin::Manifest;

[[nodiscard]] auto resolveLoadSet(ManifestMap const& manifests) -> LoadSet;
[[nodiscard]] auto sortLoadOrder(ManifestMap const& manifests, NameSet const& selected)
        -> Expected<std::vector<std::string>>;

} // namespace CloverNT::Core::Modules::Resolver
