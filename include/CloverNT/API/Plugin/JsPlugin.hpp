#pragma once

#include <filesystem>
#include <memory>
#include <optional>

#include <CloverNT/API/Macros.hpp>
#include <CloverNT/API/Plugin/Plugin.hpp>

namespace CloverNT::Core::Modules {
class PluginRuntime;
class JsRuntime;
} // namespace CloverNT::Core::Modules

namespace CloverNT::Plugin {

class CloverNT_API JsPlugin : public Plugin {
public:
    JsPlugin(Manifest                             manifest,
             std::filesystem::path                directory,
             std::optional<std::filesystem::path> mainEntry,
             std::optional<std::filesystem::path> preloadEntry,
             std::optional<std::filesystem::path> rendererEntry);
    ~JsPlugin() override;

    [[nodiscard]] auto directory() const noexcept -> std::filesystem::path const&;
    [[nodiscard]] auto mainEntry() const noexcept -> std::optional<std::filesystem::path> const&;
    [[nodiscard]] auto preloadEntry() const noexcept -> std::optional<std::filesystem::path> const&;
    [[nodiscard]] auto rendererEntry() const noexcept -> std::optional<std::filesystem::path> const&;

private:
    std::filesystem::path                mDirectory;
    std::optional<std::filesystem::path> mMainEntry;
    std::optional<std::filesystem::path> mPreloadEntry;
    std::optional<std::filesystem::path> mRendererEntry;

    friend class PluginManager;
    friend class Core::Modules::PluginRuntime;
    friend class Core::Modules::JsRuntime;
};

using JsPluginPtr = std::shared_ptr<JsPlugin>;

} // namespace CloverNT::Plugin
