#include <CloverNT/API/Plugin/NativePlugin.hpp>

#include <utility>

namespace CloverNT::Plugin {

NativePlugin::NativePlugin(Manifest manifest, std::filesystem::path directory, std::filesystem::path entryPath)
    : Plugin(std::move(manifest), false), mDirectory(std::move(directory)), mEntryPath(std::move(entryPath)) {}

NativePlugin::~NativePlugin() = default;

auto NativePlugin::directory() const noexcept -> std::filesystem::path const& {
    return mDirectory;
}

auto NativePlugin::entryPath() const noexcept -> std::filesystem::path const& {
    return mEntryPath;
}

void* NativePlugin::nativeHandle() const noexcept {
    return mLibrary.handle();
}

} // namespace CloverNT::Plugin
