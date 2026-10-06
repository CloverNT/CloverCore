#pragma once

#include <CloverNT/API/Events/EventBus.hpp>
#include <CloverNT/Core/Modules/ModuleBase.hpp>

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace CloverNT::Core::Modules {

class EventRuntime final : public ModuleBase {
public:
    [[nodiscard]] auto onLoad() -> Expected<void> override;
    [[nodiscard]] auto onUnload() -> Expected<void> override;
    [[nodiscard]] auto onEnable() -> Expected<void> override;
    [[nodiscard]] auto onDisable() -> Expected<void> override;

    [[nodiscard]] auto registerEvent(std::string_view eventName, const std::weak_ptr<Plugin::Plugin>& owner = {})
            -> Expected<void>;

    [[nodiscard]] auto addListener(CloverNT::Event::ListenerPtr const& listener, std::string_view eventName) -> Expected<void>;
    [[nodiscard]] auto removeListener(CloverNT::Event::ListenerPtr const& listener) -> Expected<void>;
    [[nodiscard]] auto removeListener(CloverNT::Event::ListenerId id) -> Expected<void>;
    [[nodiscard]] auto getListener(CloverNT::Event::ListenerId id) const -> CloverNT::Event::ListenerPtr;
    [[nodiscard]] bool hasListener(CloverNT::Event::ListenerId id) const;
    [[nodiscard]] auto getListenerCount(std::string_view eventName = {}) const -> std::size_t;
    [[nodiscard]] auto events() const -> std::vector<std::string>;

    void               publish(CloverNT::Event::Event& event, std::string_view eventName);
    [[nodiscard]] auto clearPlugin(std::string_view pluginName) -> Expected<void>;
    [[nodiscard]] auto waitPluginIdle(std::string_view pluginName) -> Expected<void>;

private:
    class ListenerActivity {
    public:
        ListenerActivity(EventRuntime& bus, std::string pluginName) noexcept;
        ListenerActivity(ListenerActivity const&)            = delete;
        ListenerActivity& operator=(ListenerActivity const&) = delete;
        ListenerActivity(ListenerActivity&& other) noexcept;
        ListenerActivity& operator=(ListenerActivity&& other) noexcept;
        ~ListenerActivity();

        [[nodiscard]] auto pluginName() const noexcept -> std::string const&;

    private:
        void reset() noexcept;

        EventRuntime* mBus{};
        std::string mPluginName;
    };

    struct ListenerComparator {
        bool operator()(CloverNT::Event::ListenerPtr const& lhs, CloverNT::Event::ListenerPtr const& rhs) const;
    };

    struct EventInfo {
        std::string                                      owner;
        std::set<CloverNT::Event::ListenerPtr, ListenerComparator> listeners;
    };

    void               finishListenerActivity(std::string const& pluginName);
    [[nodiscard]] auto beginListenerActivity(CloverNT::Event::ListenerPtr const& listener) -> std::optional<ListenerActivity>;

    mutable std::recursive_mutex                                    mMutex;
    std::condition_variable_any                                     mPluginIdle;
    std::map<std::string, EventInfo, std::less<>>                   mEvents;
    std::unordered_map<CloverNT::Event::ListenerId, CloverNT::Event::ListenerPtr>       mListeners;
    std::unordered_map<CloverNT::Event::ListenerId, std::string>              mListenerOwners;
    std::map<std::string, std::set<CloverNT::Event::ListenerId>, std::less<>> mPluginListeners;
    std::map<std::string, std::set<std::string>, std::less<>>       mPluginEvents;
    std::map<std::string, std::size_t, std::less<>>                 mActivePluginListeners;
};

} // namespace CloverNT::Core::Modules
