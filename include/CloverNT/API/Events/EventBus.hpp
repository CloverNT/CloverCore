#pragma once

#include <concepts>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <CloverNT/API/Events/Cancellable.hpp>
#include <CloverNT/API/Events/Listener.hpp>
#include <CloverNT/API/Expected.hpp>
#include <CloverNT/API/Plugin/PluginManager.hpp>

namespace CloverNT::Event {

class CloverNT_API EventBus {
public:
    EventBus(EventBus const&)            = delete;
    EventBus& operator=(EventBus const&) = delete;

    static auto getInstance() -> EventBus&;

    [[nodiscard]] static auto registerEvent(std::string_view eventName, const std::weak_ptr<Plugin::Plugin>& owner = {})
            -> Expected<void>;

    [[nodiscard]] static auto addListener(ListenerPtr const& listener, std::string_view eventName) -> Expected<void>;
    [[nodiscard]] static auto removeListener(ListenerPtr const& listener) -> Expected<void>;
    [[nodiscard]] static auto removeListener(ListenerId id) -> Expected<void>;
    [[nodiscard]] static auto getListener(ListenerId id) -> ListenerPtr;
    [[nodiscard]] static bool hasListener(ListenerId id);
    [[nodiscard]] static auto getListenerCount(std::string_view eventName = {}) -> std::size_t;
    [[nodiscard]] static auto events() -> std::vector<std::string>;

    static void               publish(Event& event, std::string_view eventName);
    [[nodiscard]] static auto clearPlugin(std::string_view pluginName) -> Expected<void>;
    [[nodiscard]] static auto waitPluginIdle(std::string_view pluginName) -> Expected<void>;

    template <std::derived_from<Event> T>
    [[nodiscard]] static auto
    registerEvent(const std::weak_ptr<Plugin::Plugin>& owner = Plugin::PluginManager::currentPlugin())
            -> Expected<void> {
        return registerEvent(getEventName<T>(), owner);
    }

    template <std::derived_from<Event> T>
    [[nodiscard]] auto addListener(std::shared_ptr<Listener<T>> const& listener) -> Expected<void> {
        return addListener(listener, getEventName<T>());
    }

    template <std::derived_from<Event> T, class Callback>
    [[nodiscard]] auto emplaceListener(Callback&&                    callback,
                                       EventPriority                 priority = EventPriority::Normal,
                                       std::weak_ptr<Plugin::Plugin> owner    = Plugin::PluginManager::currentPlugin())
            -> Expected<std::shared_ptr<Listener<T>>> {
        auto listener = std::make_shared<Listener<T>>(std::forward<Callback>(callback), priority, std::move(owner));
        if (auto result = addListener<T>(listener); !result) {
            return unexpected(result.error());
        }
        return listener;
    }

    template <std::derived_from<Event> T>
    void publish(T& event) {
        publish(event, getEventName<T>());
    }

private:
    EventBus();
    ~EventBus();
};

} // namespace CloverNT::Event
