#include <CloverNT/API/Utils/System.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/PluginResolver.hpp>

#include <format>
#include <fstream>
#include <queue>
#include <ranges>
#include <utility>

#include <nlohmann/json.hpp>

namespace CloverNT::Core::Modules::Resolver {
namespace {

    auto manifestError(std::string message, const ErrorCategory category = ErrorCategory::Plugin)
            -> std::unexpected<Error> {
        return unexpected(makeError(category, CommonErrorCode::ParseFailed, std::move(message)));
    }

    Expected<std::optional<Plugin::Version>> parseVersionValue(nlohmann::json const& value) {
        if (value.is_null()) {
            return std::nullopt;
        }
        if (!value.is_string()) {
            return manifestError("version value must be a string");
        }
        const auto text = value.get<std::string>();
        if (text.empty() || text == "*") {
            return std::nullopt;
        }
        auto parsed = Plugin::Version::parse(text);
        if (!parsed) {
            return manifestError("invalid version: " + text);
        }
        return parsed;
    }

    Expected<void>
    readDependencyMap(nlohmann::json const& json, const std::string_view key, Plugin::DependencyMap& out) {
        const auto keyName = std::string(key);
        if (!json.contains(keyName)) {
            return {};
        }
        auto const& value = json.at(keyName);
        if (!value.is_object()) {
            return manifestError(keyName + " must be an object");
        }
        for (auto const& [name, versionJson]: value.items()) {
            auto version = parseVersionValue(versionJson);
            if (!version) {
                return manifestError(std::format("{}.{}: {}", keyName, name, version.error().message));
            }
            out.emplace(name, version.value());
        }
        return {};
    }

    Expected<void>
    readOptionalString(nlohmann::json const& json, const std::string_view key, std::optional<std::string>& out) {
        const auto keyName = std::string(key);
        if (!json.contains(keyName)) {
            return {};
        }
        if (!json.at(keyName).is_string()) {
            return manifestError(keyName + " must be a string");
        }
        out = json.at(keyName).get<std::string>();
        return {};
    }

    Expected<void> readRequiredString(nlohmann::json const& json, const std::string_view key, std::string& out) {
        const auto keyName = std::string(key);
        if (!json.contains(keyName) || !json.at(keyName).is_string() || json.at(keyName).get<std::string>().empty()) {
            return manifestError(keyName + " must be a non-empty string");
        }
        out = json.at(keyName).get<std::string>();
        return {};
    }

    void addDependencyClosure(std::string const& name, ManifestMap const& manifests, NameSet& selected) {
        if (!selected.emplace(name).second) {
            return;
        }
        const auto manifestIt = manifests.find(name);
        if (manifestIt == manifests.end()) {
            return;
        }

        for (const auto& dependency: manifestIt->second.manifest.dependencies | std::views::keys) {
            if (manifests.contains(dependency)) {
                addDependencyClosure(dependency, manifests, selected);
            }
        }
        for (auto const& [dependency, version]: manifestIt->second.manifest.optionalDependencies) {
            if (auto dependencyIt = manifests.find(dependency);
                dependencyIt != manifests.end() && versionMatches(dependencyIt->second.manifest, version)) {
                addDependencyClosure(dependency, manifests, selected);
            }
        }
    }

} // namespace

bool versionMatches(Plugin::Manifest const& real, std::optional<Plugin::Version> const& required) {
    if (!required) {
        return true; // no constraint → matches anything
    }
    if (!real.version) {
        return false; // a version was required but the provider is unversioned → mismatch
    }
    return real.version->major == required->major && *real.version >= *required;
}

bool supportsPlatform(Plugin::Manifest const& manifest) {
    if (!manifest.platform || manifest.platform->empty() || *manifest.platform == "any") {
        return true;
    }
    return *manifest.platform == Utils::System::GetCurrentPlatformName();
}

bool manifestDependsOn(Plugin::Manifest const& manifest, const std::string_view dependencyName) {
    return manifest.dependencies.contains(dependencyName) || manifest.optionalDependencies.contains(dependencyName);
}

auto loadManifest(std::filesystem::path const& directory) -> Expected<Plugin::Manifest> {
    Plugin::Manifest manifest;
    auto             manifestPath = directory / "clover.json";
    std::ifstream    file(manifestPath);
    if (!file) {
        return manifestError("missing clover.json", ErrorCategory::Filesystem);
    }

    nlohmann::json json;
    try {
        file >> json;
    } catch (std::exception const& e) {
        return manifestError(std::string("invalid JSON: ") + e.what(), ErrorCategory::Json);
    }

    if (!json.is_object()) {
        return manifestError("manifest root must be an object", ErrorCategory::Json);
    }

    if (auto result = readRequiredString(json, "name", manifest.name); !result) {
        return unexpected(result.error());
    }
    if (auto result = readRequiredString(json, "type", manifest.type); !result) {
        return unexpected(result.error());
    }
    if (manifest.type == "native") {
        if (auto result = readRequiredString(json, "entry", manifest.entry); !result) {
            return unexpected(result.error());
        }
    } else if (manifest.type == "js") {
        if (json.contains("entry")) {
            if (auto result = readRequiredString(json, "entry", manifest.entry); !result) {
                return unexpected(result.error());
            }
        }
        if (auto result = readOptionalString(json, "preloadEntry", manifest.preloadEntry); !result) {
            return unexpected(result.error());
        }
        if (auto result = readOptionalString(json, "rendererEntry", manifest.rendererEntry); !result) {
            return unexpected(result.error());
        }
        if (manifest.entry.empty() && !manifest.preloadEntry && !manifest.rendererEntry) {
            return manifestError("js plugin must declare at least one of entry/preloadEntry/rendererEntry");
        }
    } else {
        if (json.contains("entry")) {
            if (auto result = readRequiredString(json, "entry", manifest.entry); !result) {
                return unexpected(result.error());
            }
        }
    }
    if (auto result = readOptionalString(json, "platform", manifest.platform); !result) {
        return unexpected(result.error());
    }
    if (auto result = readOptionalString(json, "author", manifest.author); !result) {
        return unexpected(result.error());
    }
    if (auto result = readOptionalString(json, "description", manifest.description); !result) {
        return unexpected(result.error());
    }
    if (auto result = readDependencyMap(json, "dependencies", manifest.dependencies); !result) {
        return unexpected(result.error());
    }
    if (auto result = readDependencyMap(json, "optionalDependencies", manifest.optionalDependencies); !result) {
        return unexpected(result.error());
    }
    if (auto result = readDependencyMap(json, "conflicts", manifest.conflicts); !result) {
        return unexpected(result.error());
    }
    if (auto result = readDependencyMap(json, "loadBefore", manifest.loadBefore); !result) {
        return unexpected(result.error());
    }

    if (json.contains("version")) {
        auto version = parseVersionValue(json.at("version"));
        if (!version) {
            return unexpected(version.error());
        }
        manifest.version = version.value();
    }
    if (json.contains("passive")) {
        if (!json.at("passive").is_boolean()) {
            return manifestError("passive must be a boolean");
        }
        manifest.passive = json.at("passive").get<bool>();
    }
    if (json.contains("extraInfo")) {
        if (!json.at("extraInfo").is_object()) {
            return manifestError("extraInfo must be an object");
        }
        for (auto const& [name, value]: json.at("extraInfo").items()) {
            if (!value.is_string()) {
                return manifestError("extraInfo." + name + " must be a string");
            }
            manifest.extraInfo.emplace(name, value.get<std::string>());
        }
    }

    if (manifest.name != directory.filename().string()) {
        return manifestError("plugin name must match directory name");
    }

    return manifest;
}

auto makeCoreManifest() -> Plugin::Manifest {
    Plugin::Manifest manifest;
    manifest.name     = std::string(CorePluginName);
    manifest.version  = Plugin::Version{0, 0, 1};
    manifest.type     = "core";
    manifest.entry    = "CloverCore";
    manifest.platform = std::string(Utils::System::GetCurrentPlatformName());
    return manifest;
}

auto resolveLoadSet(ManifestMap const& manifests) -> LoadSet {
    NameSet     selected;
    std::string errorLog;
    selected.emplace(CorePluginName);

    for (auto const& [name, entry]: manifests) {
        if (name == CorePluginName || entry.manifest.passive) {
            continue;
        }
        addDependencyClosure(name, manifests, selected);
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (auto it = selected.begin(); it != selected.end();) {
            auto manifestIt = manifests.find(*it);
            if (manifestIt == manifests.end()) {
                it      = selected.erase(it);
                changed = true;
                continue;
            }

            auto const& manifest = manifestIt->second.manifest;
            bool        remove   = !supportsPlatform(manifest);
            if (remove) {
                errorLog += manifest.name + " skipped because platform is not compatible\n";
            }

            for (auto const& [dependency, version]: manifest.dependencies) {
                if (auto dependencyIt = manifests.find(dependency);
                    dependencyIt == manifests.end() || !selected.contains(dependency) ||
                    !versionMatches(dependencyIt->second.manifest, version)) {
                    remove = true;
                    errorLog += manifest.name + " skipped because dependency " + dependency + " is missing\n";
                }
            }

            for (auto const& [conflict, version]: manifest.conflicts) {
                auto       conflictIt   = manifests.find(conflict);
                const bool conflictHits = conflictIt != manifests.end() && selected.contains(conflict) &&
                                          (!version || !conflictIt->second.manifest.version ||
                                           versionMatches(conflictIt->second.manifest, version));
                if (conflictHits) {
                    remove = true;
                    errorLog += manifest.name + " skipped because it conflicts with " + conflict + "\n";
                }
            }

            if (remove && *it != CorePluginName) {
                it      = selected.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
    }
    return LoadSet{std::move(selected), std::move(errorLog)};
}

auto sortLoadOrder(ManifestMap const& manifests, NameSet const& selected) -> Expected<std::vector<std::string>> {
    std::map<std::string, NameSet, std::less<>>     edges;
    std::map<std::string, std::size_t, std::less<>> indegree;
    for (auto const& name: selected) {
        edges.emplace(name, NameSet{});
        indegree.emplace(name, 0);
    }

    auto addEdge = [&](std::string const& before, std::string const& after) {
        if (!selected.contains(before) || !selected.contains(after) || before == after) {
            return;
        }
        if (edges[before].emplace(after).second) {
            ++indegree[after];
        }
    };

    for (auto const& name: selected) {
        auto const& manifest = manifests.at(name).manifest;
        for (const auto& dependency: manifest.dependencies | std::views::keys) {
            addEdge(dependency, name);
        }
        for (auto const& [dependency, version]: manifest.optionalDependencies) {
            if (auto dependencyIt = manifests.find(dependency);
                dependencyIt != manifests.end() && versionMatches(dependencyIt->second.manifest, version)) {
                addEdge(dependency, name);
            }
        }
        for (auto const& [target, version]: manifest.loadBefore) {
            if (auto targetIt = manifests.find(target);
                targetIt != manifests.end() && versionMatches(targetIt->second.manifest, version)) {
                addEdge(name, target);
            }
        }
    }

    std::priority_queue<std::string, std::vector<std::string>, std::greater<>> queue;
    for (auto const& [name, degree]: indegree) {
        if (degree == 0) {
            queue.push(name);
        }
    }

    std::vector<std::string> order;
    while (!queue.empty()) {
        auto name = queue.top();
        queue.pop();
        order.emplace_back(name);
        for (auto const& next: edges[name]) {
            if (--indegree[next] == 0) {
                queue.push(next);
            }
        }
    }

    if (order.size() != selected.size()) {
        return unexpected(
                makeError(ErrorCategory::Plugin, CommonErrorCode::InvalidState, "plugin dependencies contain a cycle"));
    }
    return order;
}

} // namespace CloverNT::Core::Modules::Resolver
