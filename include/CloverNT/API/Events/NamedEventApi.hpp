#pragma once

#include <functional>
#include <memory>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

#include <CloverNT/API/Events/Listener.hpp>
#include <CloverNT/API/Events/NamedEvent.hpp>
#include <CloverNT/API/Expected.hpp>
#include <CloverNT/API/Macros.hpp>
#include <CloverNT/API/Plugin/Plugin.hpp>

namespace CloverNT::Event {

[[nodiscard]] CloverNT_API auto emitNamed(std::string_view name, nlohmann::json payload) -> bool;
[[nodiscard]] CloverNT_API auto emitNamed(std::string_view name) -> bool;

[[nodiscard]] CloverNT_API auto onNamed(std::string_view                 name,
                                        std::function<void(NamedEvent&)> callback,
                                        EventPriority                    priority = EventPriority::Normal,
                                        std::weak_ptr<Plugin::Plugin>    owner    = {}) -> Expected<ListenerPtr>;

} // namespace CloverNT::Event
