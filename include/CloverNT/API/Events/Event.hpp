#pragma once

#include <concepts>
#include <string_view>

#include <CloverNT/API/Macros.hpp>

namespace CloverNT::Event {

class CloverNT_API Event {
public:
    virtual ~Event();
};

template <class T>
concept NamedEventType = requires {
    { T::kEventName } -> std::convertible_to<std::string_view>;
};

template <NamedEventType T>
[[nodiscard]] constexpr auto getEventName() noexcept -> std::string_view {
    return T::kEventName;
}

} // namespace CloverNT::Event
