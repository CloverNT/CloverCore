#include <CloverNT/Core/Platform/NativeImage.hpp>

#include <filesystem>
#include <string_view>

namespace CloverNT::Core::Platform {

auto loadNativeImage(Utils::DynamicLibrary& library,
                     std::string_view,
                     std::filesystem::path const& entryPath,
                     std::filesystem::path const& pluginDirectory) -> Expected<std::filesystem::path> {
    if (auto loaded = library.load(entryPath, pluginDirectory); !loaded) {
        return unexpected(loaded.error());
    }
    return std::filesystem::path{};
}

void discardNativeImage(std::filesystem::path const&) noexcept {}

} // namespace CloverNT::Core::Platform
