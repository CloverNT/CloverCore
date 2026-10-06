#pragma once

#include <compare>
#include <optional>
#include <string>
#include <string_view>

#include <CloverNT/API/Macros.hpp>

namespace CloverNT::Plugin {

struct CloverNT_API Version {
    int major{0};
    int minor{0};
    int patch{0};

    [[nodiscard]] static auto parse(std::string_view text) -> std::optional<Version>;
    [[nodiscard]] auto        toString() const -> std::string;

    [[nodiscard]] auto operator<=>(Version const&) const = default;
};

} // namespace CloverNT::Plugin
