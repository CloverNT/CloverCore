#include <CloverNT/API/Events/NamedEvent.hpp>
#include <CloverNT/API/Events/NamedEventApi.hpp>

#include <memory>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include <CloverNT/API/Events/EventBus.hpp>
#include <CloverNT/API/Plugin/PluginManager.hpp>

namespace CloverNT::Event {

NamedEvent::NamedEvent(std::string name, nlohmann::json payload)
    : mName(std::move(name)), mPayload(std::make_unique<nlohmann::json>(std::move(payload))) {}

NamedEvent::NamedEvent(std::string name) : mName(std::move(name)), mPayload(std::make_unique<nlohmann::json>()) {}

NamedEvent::~NamedEvent() = default;

auto NamedEvent::name() const noexcept -> std::string const& {
    return mName;
}
auto NamedEvent::payload() const noexcept -> nlohmann::json const& {
    return *mPayload;
}
auto NamedEvent::payload() noexcept -> nlohmann::json& {
    return *mPayload;
}
auto NamedEvent::bridgeToken() const noexcept -> void* {
    return mBridgeToken;
}
void NamedEvent::setBridgeToken(void* token) noexcept {
    mBridgeToken = token;
}

auto emitNamed(const std::string_view name, nlohmann::json payload) -> bool {
    NamedEvent event(std::string(name), std::move(payload));
    EventBus::publish(event, name);
    return event.isCancelled();
}

auto emitNamed(const std::string_view name) -> bool {
    return emitNamed(name, nlohmann::json());
}

auto onNamed(const std::string_view           name,
             std::function<void(NamedEvent&)> callback,
             const EventPriority              priority,
             std::weak_ptr<Plugin::Plugin>    owner) -> Expected<ListenerPtr> {
    if (name.empty()) {
        return unexpected(ErrorCategory::Event, CommonErrorCode::InvalidArgument, "Event name is empty");
    }

    (void) EventBus::registerEvent(name, Plugin::PluginManager::getPlugin("CloverCore"));

    auto listener = std::make_shared<Listener<NamedEvent>>(std::move(callback), priority, std::move(owner));
    if (auto result = EventBus::addListener(listener, name); !result) {
        return unexpected(result.error());
    }
    return listener;
}

} // namespace CloverNT::Event
