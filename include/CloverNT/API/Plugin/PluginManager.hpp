#pragma once
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <CloverNT/API/Expected.hpp>
#include <CloverNT/API/Plugin/Plugin.hpp>
#include <CloverNT/API/Plugin/Version.hpp>

namespace CloverNT::Plugin {

struct PluginListing {
    std::string            name;
    std::string            type;
    std::optional<Version> version;
    std::string            author;
    std::string            description;
    bool                   passive{};
    bool                   loaded{};
};

class PluginLoader;

class CloverNT_API PluginManager {
public:
    PluginManager(PluginManager const&)            = delete;
    PluginManager& operator=(PluginManager const&) = delete;

    static auto getInstance() -> PluginManager&;

    [[nodiscard]] static auto getPlugin(std::string_view name) -> std::shared_ptr<Plugin>;
    [[nodiscard]] static auto currentPlugin() -> std::shared_ptr<Plugin>;
    [[nodiscard]] static auto plugins() -> std::vector<std::shared_ptr<Plugin>>;
    [[nodiscard]] static auto scan() -> std::vector<PluginListing>;

    [[nodiscard]] static auto unloadPlugin(std::string_view name) -> Expected<void>;
    [[nodiscard]] static auto loadPlugin(std::string_view name) -> Expected<void>;
    [[nodiscard]] static auto reloadPlugin(std::string_view name) -> Expected<void>;

    [[nodiscard]] static auto unloadAllPlugins() -> Expected<void>;

    [[nodiscard]] static auto rescan() -> Expected<std::size_t>;

    [[nodiscard]] static auto registerLoader(std::shared_ptr<PluginLoader> loader) -> Expected<void>;
    [[nodiscard]] static auto unregisterLoader(std::string_view id) -> Expected<void>;

private:
    PluginManager();
    ~PluginManager();
};

} // namespace CloverNT::Plugin
