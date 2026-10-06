#include <CloverNT/API/Exception.hpp>
#include <CloverNT/API/Logger.hpp>
#include <CloverNT/API/Plugin/PluginManager.hpp>
#include <CloverNT/Core/Modules/EventRuntime.hpp>
#include <CloverNT/Core/Modules/Manager.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
#include <ranges>
#include <utility>

namespace CloverNT::Core::Modules {
namespace {

    thread_local std::map<std::string, std::size_t, std::less<>> tActiveListenerPlugins;

    Logger& logger() {
        static auto instance = LoggerRegistry::getInstance().getOrCreate("CloverCore");
        return *instance;
    }

    void appendError(std::string& errors, const std::string& message) {
        if (!errors.empty()) {
            errors += '\n';
        }
        errors += message;
    }

    class ThreadActivePluginScope {
    public:
        explicit ThreadActivePluginScope(std::string pluginName) : mPluginName(std::move(pluginName)) {
            if (!mPluginName.empty()) {
                ++tActiveListenerPlugins[mPluginName];
            }
        }

        ThreadActivePluginScope(ThreadActivePluginScope const&)            = delete;
        ThreadActivePluginScope& operator=(ThreadActivePluginScope const&) = delete;

        ~ThreadActivePluginScope() {
            if (mPluginName.empty()) {
                return;
            }
            if (const auto it = tActiveListenerPlugins.find(mPluginName); it != tActiveListenerPlugins.end()) {
                if (--it->second == 0) {
                    tActiveListenerPlugins.erase(it);
                }
            }
        }

    private:
        std::string mPluginName;
    };

} // namespace

EventRuntime::ListenerActivity::ListenerActivity(EventRuntime& bus, std::string pluginName) noexcept
    : mBus(&bus), mPluginName(std::move(pluginName)) {}

EventRuntime::ListenerActivity::ListenerActivity(ListenerActivity&& other) noexcept
    : mBus(std::exchange(other.mBus, nullptr)), mPluginName(std::move(other.mPluginName)) {}

auto EventRuntime::ListenerActivity::operator=(ListenerActivity&& other) noexcept -> ListenerActivity& {
    if (this != &other) {
        reset();
        mBus        = std::exchange(other.mBus, nullptr);
        mPluginName = std::move(other.mPluginName);
    }
    return *this;
}

EventRuntime::ListenerActivity::~ListenerActivity() {
    reset();
}

auto EventRuntime::ListenerActivity::pluginName() const noexcept -> std::string const& {
    return mPluginName;
}

void EventRuntime::ListenerActivity::reset() noexcept {
    auto* bus = std::exchange(mBus, nullptr);
    if (bus == nullptr) {
        return;
    }
    try {
        bus->finishListenerActivity(mPluginName);
    } catch (...) {}
}

bool EventRuntime::ListenerComparator::operator()(Event::ListenerPtr const& lhs, Event::ListenerPtr const& rhs) const {
    if (lhs->priority() != rhs->priority()) {
        return static_cast<int>(lhs->priority()) > static_cast<int>(rhs->priority());
    }
    return lhs->id() < rhs->id();
}

auto EventRuntime::onLoad() -> Expected<void> {
    return {};
}

auto EventRuntime::onUnload() -> Expected<void> {
    if (!tActiveListenerPlugins.empty()) {
        return unexpected(ErrorCategory::Event,
                          CommonErrorCode::InvalidState,
                          "Cannot unload EventRuntime from an active event listener callback");
    }

    std::unique_lock lock(mMutex);
    mPluginIdle.notify_all();
    using namespace std::chrono_literals;
    constexpr auto kDrainTimeout = 5s;
    if (!mPluginIdle.wait_for(lock, kDrainTimeout, [&] { return mActivePluginListeners.empty(); })) {
        std::string stuck;
        for (const auto& name: mActivePluginListeners | std::views::keys) {
            if (!stuck.empty()) {
                stuck += ", ";
            }
            stuck += name;
        }
        logger().warn(
                "EventRuntime unload: {} plugin(s) still have active listeners after {}ms; forcing teardown: [{}]",
                mActivePluginListeners.size(),
                std::chrono::duration_cast<std::chrono::milliseconds>(kDrainTimeout).count(),
                stuck);
    }
    mEvents.clear();
    mListeners.clear();
    mListenerOwners.clear();
    mPluginListeners.clear();
    mPluginEvents.clear();
    mPluginIdle.notify_all();
    return {};
}

auto EventRuntime::onEnable() -> Expected<void> {
    return {};
}

auto EventRuntime::onDisable() -> Expected<void> {
    std::lock_guard lock(mMutex);
    mPluginIdle.notify_all();
    return {};
}

auto EventRuntime::registerEvent(const std::string_view eventName, const std::weak_ptr<Plugin::Plugin>& owner)
        -> Expected<void> {
    if (eventName.empty()) {
        return unexpected(ErrorCategory::Event, CommonErrorCode::InvalidArgument, "Event name is empty");
    }

    auto plugin = owner.lock();
    if (!plugin) {
        plugin = Plugin::PluginManager::currentPlugin();
    }
    if (!plugin) {
        return unexpected(ErrorCategory::Event, CommonErrorCode::InvalidState, "No current plugin is available");
    }

    std::lock_guard lock(mMutex);
    if (!isEnabled()) {
        return unexpected(ErrorCategory::Event, CommonErrorCode::Inactive, "Event bus is inactive");
    }
    auto [it, inserted] = mEvents.try_emplace(std::string(eventName));
    if (!inserted) {
        return unexpected(ErrorCategory::Event,
                          CommonErrorCode::AlreadyExists,
                          "Event is already registered: " + std::string(eventName));
    }
    it->second.owner = std::string(plugin->name());
    mPluginEvents[it->second.owner].emplace(it->first);
    return {};
}

auto EventRuntime::addListener(Event::ListenerPtr const& listener, const std::string_view eventName) -> Expected<void> {
    if (!listener || eventName.empty()) {
        return unexpected(
                ErrorCategory::Event, CommonErrorCode::InvalidArgument, "Listener and event name are required");
    }

    auto owner = listener->owner().lock();
    if (!owner) {
        owner = Plugin::PluginManager::currentPlugin();
    }
    if (!owner) {
        return unexpected(ErrorCategory::Event, CommonErrorCode::InvalidState, "No current plugin is available");
    }

    std::lock_guard lock(mMutex);
    if (!isEnabled()) {
        return unexpected(ErrorCategory::Event, CommonErrorCode::Inactive, "Event bus is inactive");
    }
    const auto listenerId = listener->id();
    if (mListeners.contains(listenerId)) {
        return unexpected(ErrorCategory::Event, CommonErrorCode::AlreadyExists, "Listener id is already registered");
    }

    if (!mEvents.contains(eventName)) {
        if (auto [it, inserted] = mEvents.try_emplace(std::string(eventName)); inserted) {
            it->second.owner = std::string(owner->name());
            mPluginEvents[it->second.owner].emplace(it->first);
        }
    }

    auto& listeners = mEvents[std::string(eventName)].listeners;
    if (auto [_, inserted] = listeners.emplace(listener); !inserted) {
        return unexpected(ErrorCategory::Event, CommonErrorCode::AlreadyExists, "Listener is already registered");
    }

    if (auto [_, inserted] = mListeners.emplace(listenerId, listener); !inserted) {
        listeners.erase(listener);
        return unexpected(ErrorCategory::Event, CommonErrorCode::AlreadyExists, "Listener id is already registered");
    }
    const auto ownerName = std::string(owner->name());
    mListenerOwners.emplace(listenerId, ownerName);
    mPluginListeners[ownerName].emplace(listenerId);
    return {};
}

auto EventRuntime::removeListener(Event::ListenerPtr const& listener) -> Expected<void> {
    if (!listener) {
        return unexpected(ErrorCategory::Event, CommonErrorCode::InvalidArgument, "Listener is null");
    }
    return removeListener(listener->id());
}

auto EventRuntime::removeListener(const Event::ListenerId id) -> Expected<void> {
    std::lock_guard lock(mMutex);
    const auto      listenerIt = mListeners.find(id);
    if (listenerIt == mListeners.end()) {
        return unexpected(ErrorCategory::Event, CommonErrorCode::NotFound, "Listener was not found");
    }

    const auto listener = listenerIt->second;
    for (auto& [owner, listeners]: mEvents | std::views::values) {
        listeners.erase(listener);
    }
    for (auto& listenerIds: mPluginListeners | std::views::values) {
        listenerIds.erase(id);
    }
    mListeners.erase(listenerIt);
    mListenerOwners.erase(id);
    return {};
}

auto EventRuntime::getListener(const Event::ListenerId id) const -> Event::ListenerPtr {
    std::lock_guard lock(mMutex);
    const auto      it = mListeners.find(id);
    return it == mListeners.end() ? nullptr : it->second;
}

bool EventRuntime::hasListener(const Event::ListenerId id) const {
    std::lock_guard lock(mMutex);
    return mListeners.contains(id);
}

auto EventRuntime::getListenerCount(const std::string_view eventName) const -> std::size_t {
    std::lock_guard lock(mMutex);
    if (eventName.empty()) {
        return mListeners.size();
    }
    const auto it = mEvents.find(eventName);
    return it == mEvents.end() ? 0 : it->second.listeners.size();
}

auto EventRuntime::events() const -> std::vector<std::string> {
    std::lock_guard          lock(mMutex);
    std::vector<std::string> result;
    result.reserve(mEvents.size());
    for (const auto& name: mEvents | std::views::keys) {
        result.emplace_back(name);
    }
    return result;
}

void EventRuntime::finishListenerActivity(std::string const& pluginName) {
    if (pluginName.empty()) {
        return;
    }

    {
        std::lock_guard lock(mMutex);
        const auto      it = mActivePluginListeners.find(pluginName);
        if (it == mActivePluginListeners.end()) {
            return;
        }
        if (--it->second == 0) {
            mActivePluginListeners.erase(it);
        }
    }
    mPluginIdle.notify_all();
}

auto EventRuntime::beginListenerActivity(Event::ListenerPtr const& listener) -> std::optional<ListenerActivity> {
    std::lock_guard lock(mMutex);
    if (!isEnabled() || !listener) {
        return std::nullopt;
    }

    if (const auto listenerIt = mListeners.find(listener->id());
        listenerIt == mListeners.end() || listenerIt->second != listener) {
        return std::nullopt;
    }

    std::string pluginName;
    if (const auto ownerIt = mListenerOwners.find(listener->id()); ownerIt != mListenerOwners.end()) {
        pluginName = ownerIt->second;
    }
    std::optional<ListenerActivity> activity(std::in_place, *this, std::move(pluginName));
    if (!activity->pluginName().empty()) {
        ++mActivePluginListeners[activity->pluginName()];
    }
    return activity;
}

void EventRuntime::publish(Event::Event& event, const std::string_view eventName) {
    const auto* const cancellable = dynamic_cast<Event::Cancellable*>(&event);

    std::vector<Event::ListenerPtr>    listeners;
    std::map<std::string, std::size_t> publishHolds;
    {
        std::lock_guard lock(mMutex);
        if (!isEnabled()) {
            return;
        }
        const auto it = mEvents.find(eventName);
        if (it == mEvents.end()) {
            return;
        }
        listeners.reserve(it->second.listeners.size());
        for (auto const& listener: it->second.listeners) {
            listeners.emplace_back(listener);
            if (const auto ownerIt = mListenerOwners.find(listener->id());
                ownerIt != mListenerOwners.end() && !ownerIt->second.empty()) {
                ++mActivePluginListeners[ownerIt->second];
                ++publishHolds[ownerIt->second];
            }
        }
    }

    auto releaseHolds = [this, &listeners, &publishHolds]() noexcept {
        listeners.clear();
        if (publishHolds.empty()) {
            return;
        }
        {
            std::lock_guard lock(mMutex);
            for (auto const& [owner, count]: publishHolds) {
                if (const auto activeIt = mActivePluginListeners.find(owner);
                    activeIt != mActivePluginListeners.end()) {
                    if (activeIt->second <= count) {
                        mActivePluginListeners.erase(activeIt);
                    } else {
                        activeIt->second -= count;
                    }
                }
            }
        }
        mPluginIdle.notify_all();
    };
    struct HoldGuard {
        decltype(releaseHolds)& release;
        ~HoldGuard() {
            release();
        }
    } holdGuard{releaseHolds};

    for (auto const& listener: listeners) {
        std::optional<ListenerActivity> activity;
        try {
            activity = beginListenerActivity(listener);
        } catch (std::exception const& exception) {
            logger().error("Failed to begin event listener activity for {} while handling {}: {}",
                           listener ? listener->id() : 0,
                           std::string(eventName),
                           exception.what());
            continue;
        } catch (...) {
            logger().error("Failed to begin event listener activity for {} while handling {}: unknown exception",
                           listener ? listener->id() : 0,
                           std::string(eventName));
            continue;
        }

        if (!activity) {
            continue;
        }

        if (cancellable != nullptr && cancellable->isCancelled()) {
            break;
        }

        try {
            ThreadActivePluginScope activePlugin(activity->pluginName());
            listener->call(event);
        } catch (std::exception const& exception) {
            logger().error("Event listener {} failed while handling {}: {}",
                           listener ? listener->id() : 0,
                           std::string(eventName),
                           exception.what());
        } catch (...) {
            logger().error("Event listener {} failed while handling {}: unknown exception",
                           listener ? listener->id() : 0,
                           std::string(eventName));
        }
    }
}

auto EventRuntime::clearPlugin(const std::string_view pluginName) -> Expected<void> {
    std::lock_guard lock(mMutex);
    const auto      name = std::string(pluginName);
    std::string     errors;

    if (const auto listenerIt = mPluginListeners.find(name); listenerIt != mPluginListeners.end()) {
        for (const auto listenerIds = listenerIt->second; const auto id: listenerIds) {
            if (auto removed = removeListener(id); !removed) {
                appendError(errors, "Listener " + std::to_string(id) + ": " + removed.error().message);
            }
        }
        mPluginListeners.erase(name);
    }

    if (const auto eventIt = mPluginEvents.find(name); eventIt != mPluginEvents.end()) {
        for (auto const& eventName: eventIt->second) {
            if (auto existing = mEvents.find(eventName); existing != mEvents.end()) {
                for (auto const& listener: existing->second.listeners) {
                    const auto listenerId = listener->id();
                    if (mListeners.erase(listenerId) == 0) {
                        appendError(errors,
                                    "Listener " + std::to_string(listenerId) +
                                            " was missing from the global index while clearing event " + eventName);
                    }
                    mListenerOwners.erase(listenerId);
                    for (auto& listenerIds: mPluginListeners | std::views::values) {
                        listenerIds.erase(listenerId);
                    }
                }
                mEvents.erase(existing);
            }
        }
        mPluginEvents.erase(eventIt);
    }

    if (!errors.empty()) {
        return unexpected(ErrorCategory::Event,
                          CommonErrorCode::OperationFailed,
                          "Failed to clear one or more plugin listeners for " + name + ":\n" + errors);
    }
    return {};
}

auto EventRuntime::waitPluginIdle(const std::string_view pluginName) -> Expected<void> {
    if (pluginName.empty()) {
        return unexpected(ErrorCategory::Event, CommonErrorCode::InvalidArgument, "Plugin name is empty");
    }

    const auto name = std::string(pluginName);
    if (tActiveListenerPlugins.contains(name)) {
        return unexpected(ErrorCategory::Event,
                          CommonErrorCode::InvalidState,
                          "Cannot wait for plugin event listeners from one of its active listener callbacks: " + name);
    }

    std::unique_lock lock(mMutex);
    mPluginIdle.wait(lock, [&] { return !mActivePluginListeners.contains(name) || !isEnabled(); });
    if (mActivePluginListeners.contains(name)) {
        return unexpected(ErrorCategory::Event,
                          CommonErrorCode::InvalidState,
                          "Event bus became inactive while plugin listeners were still active: " + name);
    }
    return {};
}

} // namespace CloverNT::Core::Modules

namespace CloverNT::Event {
namespace {

    [[nodiscard]] auto eventBusModule() -> Expected<Core::Modules::ModuleRef<Core::Modules::EventRuntime>> {
        return Core::Modules::Manager::getInstance().requireModule<Core::Modules::EventRuntime>();
    }

} // namespace

EventBus::EventBus() = default;

EventBus::~EventBus() = default;

auto EventBus::getInstance() -> EventBus& {
    static EventBus instance;
    return instance;
}

auto EventBus::registerEvent(const std::string_view eventName, const std::weak_ptr<Plugin::Plugin>& owner)
        -> Expected<void> {
    auto module = eventBusModule();
    if (!module) {
        return unexpected(module.error());
    }
    return module->get().registerEvent(eventName, owner);
}

auto EventBus::addListener(const ListenerPtr& listener, const std::string_view eventName) -> Expected<void> {
    auto module = eventBusModule();
    if (!module) {
        return unexpected(module.error());
    }
    return module->get().addListener(listener, eventName);
}

auto EventBus::removeListener(const ListenerPtr& listener) -> Expected<void> {
    auto module = eventBusModule();
    if (!module) {
        return unexpected(module.error());
    }
    return module->get().removeListener(listener);
}

auto EventBus::removeListener(const ListenerId id) -> Expected<void> {
    auto module = eventBusModule();
    if (!module) {
        return unexpected(module.error());
    }
    return module->get().removeListener(id);
}

auto EventBus::getListener(const ListenerId id) -> ListenerPtr {
    const auto module = eventBusModule();
    return module ? module->get().getListener(id) : nullptr;
}

bool EventBus::hasListener(const ListenerId id) {
    const auto module = eventBusModule();
    return module && module->get().hasListener(id);
}

auto EventBus::getListenerCount(const std::string_view eventName) -> std::size_t {
    const auto module = eventBusModule();
    return module ? module->get().getListenerCount(eventName) : 0;
}

auto EventBus::events() -> std::vector<std::string> {
    const auto module = eventBusModule();
    return module ? module->get().events() : std::vector<std::string>{};
}

void EventBus::publish(Event& event, const std::string_view eventName) {
    if (const auto module = eventBusModule()) {
        module->get().publish(event, eventName);
    }
}

auto EventBus::clearPlugin(const std::string_view pluginName) -> Expected<void> {
    const auto module = eventBusModule();
    if (!module) {
        return unexpected(module.error());
    }
    return module->get().clearPlugin(pluginName);
}

auto EventBus::waitPluginIdle(const std::string_view pluginName) -> Expected<void> {
    const auto module = eventBusModule();
    if (!module) {
        return unexpected(module.error());
    }
    return module->get().waitPluginIdle(pluginName);
}

} // namespace CloverNT::Event
