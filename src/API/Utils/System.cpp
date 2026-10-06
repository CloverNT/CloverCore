#include <CloverNT/API/Utils/System.hpp>

namespace CloverNT::Utils::System {

bool IsPathInside(std::filesystem::path const& child, std::filesystem::path const& parent) {
    const auto normalizedChild  = std::filesystem::absolute(child).lexically_normal();
    const auto normalizedParent = std::filesystem::absolute(parent).lexically_normal();
    if (normalizedChild == normalizedParent) {
        return true;
    }
    auto relative = normalizedChild.lexically_relative(normalizedParent);
    if (relative.empty()) {
        return true;
    }
    const auto first = *relative.begin();
    return first != ".." && first != ".";
}

} // namespace CloverNT::Utils::System
