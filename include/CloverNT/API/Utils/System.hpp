#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include <CloverNT/API/Macros.hpp>

namespace CloverNT::Utils::System {

enum class Platform {
    Windows,
    Linux,
};

struct TimeWithMs {
    std::uint16_t hour{};
    std::uint16_t minute{};
    std::uint16_t second{};
    std::uint16_t millisecond{};
};

[[nodiscard]] CloverNT_API auto GetCurrentPlatform() noexcept -> Platform;

[[nodiscard]] CloverNT_API auto GetCurrentPlatformName() noexcept -> std::string_view;

[[nodiscard]] CloverNT_API auto GetCurrentTimeWithMs() noexcept -> TimeWithMs;

[[nodiscard]] CloverNT_API auto GetModuleFilePath(ModuleHandle module) -> std::filesystem::path;

[[nodiscard]] CloverNT_API auto GetModuleDirectory(ModuleHandle module) -> std::filesystem::path;

[[nodiscard]] CloverNT_API auto IsPathInside(std::filesystem::path const& child, std::filesystem::path const& parent)
        -> bool;

} // namespace CloverNT::Utils::System
