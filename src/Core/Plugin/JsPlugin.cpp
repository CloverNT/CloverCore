#include <CloverNT/API/Plugin/JsPlugin.hpp>

#include <utility>

namespace CloverNT::Plugin {

JsPlugin::JsPlugin(Manifest                             manifest,
                   std::filesystem::path                directory,
                   std::optional<std::filesystem::path> mainEntry,
                   std::optional<std::filesystem::path> preloadEntry,
                   std::optional<std::filesystem::path> rendererEntry)
    : Plugin(std::move(manifest), true),
      mDirectory(std::move(directory)),
      mMainEntry(std::move(mainEntry)),
      mPreloadEntry(std::move(preloadEntry)),
      mRendererEntry(std::move(rendererEntry)) {}

JsPlugin::~JsPlugin() = default;

auto JsPlugin::directory() const noexcept -> std::filesystem::path const& {
    return mDirectory;
}

auto JsPlugin::mainEntry() const noexcept -> std::optional<std::filesystem::path> const& {
    return mMainEntry;
}

auto JsPlugin::preloadEntry() const noexcept -> std::optional<std::filesystem::path> const& {
    return mPreloadEntry;
}

auto JsPlugin::rendererEntry() const noexcept -> std::optional<std::filesystem::path> const& {
    return mRendererEntry;
}

} // namespace CloverNT::Plugin
