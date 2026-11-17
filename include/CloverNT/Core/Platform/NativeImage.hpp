#pragma once

#include <CloverNT/API/Expected.hpp>
#include <CloverNT/API/Utils/DynamicLibrary.hpp>

#include <filesystem>
#include <string_view>

namespace CloverNT::Core::Platform {

[[nodiscard]] auto loadNativeImage(Utils::DynamicLibrary& library,
                                   std::string_view                 pluginName,
                                   std::filesystem::path const&     entryPath,
                                   std::filesystem::path const&     pluginDirectory) -> Expected<std::filesystem::path>;

void discardNativeImage(std::filesystem::path const& imageDirectory) noexcept;

} // namespace CloverNT::Core::Platform
