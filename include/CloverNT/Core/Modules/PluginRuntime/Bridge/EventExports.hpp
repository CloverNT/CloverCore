#pragma once

#include <span>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

#include <CloverNT/API/Events/Event.hpp>

namespace CloverNT::Core::Modules::bridge {

struct NativeEventExport {
    std::string_view jsName; // e.g. "dll-load"
    std::string_view busKey; // getEventName<T>() — the stable EventBus key the event is published under
    nlohmann::json (*toPayload)(Event::Event& event);
};

[[nodiscard]] auto nativeEventExports() -> std::span<const NativeEventExport>;

[[nodiscard]] auto findNativeExport(std::string_view jsName) -> const NativeEventExport*;

} // namespace CloverNT::Core::Modules::bridge
