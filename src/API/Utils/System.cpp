#include <CloverNT/API/Utils/System.hpp>

#include <filesystem>
#include <system_error>

namespace CloverNT::Utils::System {

namespace {

    auto normalizePath(std::filesystem::path const& path) -> std::filesystem::path {
        std::error_code ec;
        const auto      canonical = std::filesystem::weakly_canonical(path, ec);
        if (!ec) {
            return canonical.lexically_normal();
        }
        ec.clear();
        auto absolute = std::filesystem::absolute(path, ec);
        return (ec ? path : absolute).lexically_normal();
    }

} // namespace

bool IsPathInside(std::filesystem::path const& child, std::filesystem::path const& parent) {
    auto normalizedChild  = normalizePath(child);
    auto normalizedParent = normalizePath(parent);

    if (normalizedChild.has_relative_path() && normalizedChild.filename().empty()) {
        normalizedChild = normalizedChild.parent_path();
    }
    if (normalizedParent.has_relative_path() && normalizedParent.filename().empty()) {
        normalizedParent = normalizedParent.parent_path();
    }

    if (normalizedChild == normalizedParent) {
        return true;
    }
    const auto relative = normalizedChild.lexically_relative(normalizedParent);
    if (relative.empty()) {
        return false;
    }
    const auto first = *relative.begin();
    return first != ".." && first != ".";
}

} // namespace CloverNT::Utils::System
