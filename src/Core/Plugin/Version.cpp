#include <CloverNT/API/Plugin/Version.hpp>

#include <charconv>
#include <string>

namespace CloverNT::Plugin {

static bool parsePart(const std::string_view text, int& out) {
    if (text.empty()) {
        return false;
    }
    int        value{};
    const auto first = text.data();
    const auto last  = text.data() + text.size();
    if (auto [ptr, ec] = std::from_chars(first, last, value); ec != std::errc{} || ptr != last || value < 0) {
        return false;
    }
    out = value;
    return true;
}

auto Version::parse(std::string_view text) -> std::optional<Version> {
    if (text.empty() || text == "*") {
        return std::nullopt;
    }

    Version    version{};
    const auto firstDot = text.find('.');
    if (firstDot == std::string_view::npos) {
        return std::nullopt;
    }
    const auto secondDot = text.find('.', firstDot + 1);
    if (secondDot == std::string_view::npos || text.find('.', secondDot + 1) != std::string_view::npos) {
        return std::nullopt;
    }

    if (!parsePart(text.substr(0, firstDot), version.major) ||
        !parsePart(text.substr(firstDot + 1, secondDot - firstDot - 1), version.minor) ||
        !parsePart(text.substr(secondDot + 1), version.patch)) {
        return std::nullopt;
    }
    return version;
}

auto Version::toString() const -> std::string {
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

} // namespace CloverNT::Plugin
