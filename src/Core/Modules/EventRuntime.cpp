#include <CloverNT/API/Events/EventBus.hpp>
#include <CloverNT/API/Exception.hpp>
#include <CloverNT/API/Logger.hpp>
#include <CloverNT/API/Plugin/PluginManager.hpp>
#include <CloverNT/API/Reflect/BridgeValue.hpp>
#include <CloverNT/API/Reflect/Class.hpp>
#include <CloverNT/Core/Modules/EventRuntime.hpp>
#include <CloverNT/Core/Modules/Manager.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

thread_local std::string_view                                tScopedOwnerPlugin{};
thread_local std::map<std::string, std::size_t, std::less<>> tActiveDispatchPlugins;

auto coreLogger() -> CloverNT::Logger& {
    static auto instance = CloverNT::LoggerRegistry::getInstance().getOrCreate("CloverCore");
    return *instance;
}

class ThreadDispatchPluginScope final {
public:
    explicit ThreadDispatchPluginScope(std::string pluginName) : mPluginName(std::move(pluginName)) {
        if (!mPluginName.empty()) {
            ++tActiveDispatchPlugins[mPluginName];
        }
    }

    ThreadDispatchPluginScope(const ThreadDispatchPluginScope&)            = delete;
    ThreadDispatchPluginScope& operator=(const ThreadDispatchPluginScope&) = delete;

    ~ThreadDispatchPluginScope() {
        if (mPluginName.empty()) {
            return;
        }
        if (const auto it = tActiveDispatchPlugins.find(mPluginName); it != tActiveDispatchPlugins.end()) {
            if (--it->second == 0) {
                tActiveDispatchPlugins.erase(it);
            }
        }
    }

private:
    std::string mPluginName;
};

[[nodiscard]] constexpr auto priorityRank(const CloverNT::Event::EventPriority priority) noexcept -> int {
    return priority == CloverNT::Event::EventPriority::Monitor ? -1 : static_cast<int>(priority);
}

[[nodiscard]] bool slotPrecedes(const CloverNT::Event::ListenerSlotPtr& lhs,
                                const CloverNT::Event::ListenerSlotPtr& rhs) noexcept {
    const int lhsRank = priorityRank(lhs->priority());
    const int rhsRank = priorityRank(rhs->priority());
    if (lhsRank != rhsRank) {
        return lhsRank > rhsRank;
    }
    return lhs->id() < rhs->id();
}

class ReflectPluginRegistry final {
public:
    static auto instance() -> ReflectPluginRegistry& {
        static auto* const reg = new ReflectPluginRegistry();
        return *reg;
    }

    auto acquireActivity(const std::string_view pluginName) -> std::shared_ptr<CloverNT::Reflect::PluginActivity> {
        const std::string key = pluginName.empty() ? "CloverCore" : std::string(pluginName);
        std::lock_guard   lock(mMutex);
        auto& [activity, cells] = mEntries[key];
        if (!activity) {
            activity = std::make_shared<CloverNT::Reflect::PluginActivity>(key);
        }
        return activity;
    }

    void registerCell(const std::string_view pluginName, std::weak_ptr<CloverNT::Reflect::RevocableObjectCell> cell) {
        const std::string key = pluginName.empty() ? "CloverCore" : std::string(pluginName);
        std::lock_guard   lock(mMutex);
        auto& [activity, cells] = mEntries[key];
        if (!activity) {
            activity = std::make_shared<CloverNT::Reflect::PluginActivity>(key);
        }
        if (cells.size() >= 64) {
            std::erase_if(cells, [](const auto& w) { return w.expired(); });
        }
        cells.push_back(std::move(cell));
    }

    void beginClose(const std::string_view pluginName) {
        std::shared_ptr<CloverNT::Reflect::PluginActivity> activity;
        {
            std::lock_guard lock(mMutex);
            if (const auto it = mEntries.find(pluginName); it != mEntries.end()) {
                activity = it->second.activity;
            }
        }
        if (activity) {
            activity->beginClose();
        }
    }

    [[nodiscard]] bool waitIdleAndRevoke(const std::string_view pluginName, const std::uint32_t timeoutMs) {
        std::shared_ptr<CloverNT::Reflect::PluginActivity>                 activity;
        std::vector<std::weak_ptr<CloverNT::Reflect::RevocableObjectCell>> cells;
        {
            std::lock_guard lock(mMutex);
            const auto      it = mEntries.find(pluginName);
            if (it == mEntries.end()) {
                return true;
            }
            activity = it->second.activity;
        }
        if (activity) {
            activity->beginClose();
            if (!activity->waitIdleFor(timeoutMs)) {
                return false;
            }
        }
        {
            std::lock_guard lock(mMutex);
            if (const auto it = mEntries.find(pluginName); it != mEntries.end()) {
                cells.swap(it->second.cells);
            }
        }
        for (const auto& weakCell: cells) {
            if (const auto cell = weakCell.lock()) {
                cell->revoke();
            }
        }
        if (activity) {
            activity->reopen();
        }
        return true;
    }

    [[nodiscard]] bool closeAndDrainAll(const std::uint32_t timeoutMs, std::string& stuckOut) {
        std::vector<std::pair<std::string, std::shared_ptr<CloverNT::Reflect::PluginActivity>>> activities;
        std::vector<std::weak_ptr<CloverNT::Reflect::RevocableObjectCell>>                      allCells;
        {
            std::lock_guard lock(mMutex);
            for (auto& [name, entry]: mEntries) {
                if (entry.activity) {
                    entry.activity->beginClose();
                    activities.emplace_back(name, entry.activity);
                }
            }
        }
        bool allIdle = true;
        for (const auto& [name, act]: activities) {
            if (!act->waitIdleFor(timeoutMs)) {
                allIdle = false;
                if (!stuckOut.empty()) {
                    stuckOut += ", ";
                }
                stuckOut += name;
            }
        }
        if (allIdle) {
            {
                std::lock_guard lock(mMutex);
                for (auto& [activity, cells]: mEntries | std::views::values) {
                    for (auto& c: cells) {
                        allCells.push_back(std::move(c));
                    }
                    cells.clear();
                }
            }
            for (const auto& weakCell: allCells) {
                if (auto cell = weakCell.lock()) {
                    cell->revoke();
                }
            }
            for (const auto& act: activities | std::views::values) {
                act->reopen();
            }
        }
        return allIdle;
    }

private:
    struct Entry {
        std::shared_ptr<CloverNT::Reflect::PluginActivity>                 activity;
        std::vector<std::weak_ptr<CloverNT::Reflect::RevocableObjectCell>> cells;
    };

    std::mutex                                mMutex;
    std::map<std::string, Entry, std::less<>> mEntries;
};

} // namespace

namespace CloverNT::Reflect {

void PluginActivity::leave() noexcept {
    if (const auto prev = mInFlight.fetch_sub(1, std::memory_order_seq_cst);
        prev == 1 && mWaiters.load(std::memory_order_seq_cst) > 0) {
        std::lock_guard lock(mWaitMutex);
        mWaitCv.notify_all();
    }
}

bool PluginActivity::waitIdleFor(const std::uint32_t timeoutMs) {
    if (mInFlight.load(std::memory_order_acquire) == 0) {
        return true;
    }
    mWaiters.fetch_add(1, std::memory_order_seq_cst);
    bool idle = false;
    {
        std::unique_lock lock(mWaitMutex);
        idle = mWaitCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
            return mInFlight.load(std::memory_order_acquire) == 0;
        });
    }
    mWaiters.fetch_sub(1, std::memory_order_seq_cst);
    return idle;
}

void PluginActivity::waitIdle() {
    if (mInFlight.load(std::memory_order_acquire) == 0) {
        return;
    }
    mWaiters.fetch_add(1, std::memory_order_seq_cst);
    {
        std::unique_lock lock(mWaitMutex);
        mWaitCv.wait(lock, [&] { return mInFlight.load(std::memory_order_acquire) == 0; });
    }
    mWaiters.fetch_sub(1, std::memory_order_seq_cst);
}

RevocableObjectCell::RevocableObjectCell(std::shared_ptr<void>           instance,
                                         void*                           rawPtr,
                                         const ClassDescriptor*          descriptor,
                                         const bool                      isConst,
                                         std::string                     ownerPlugin,
                                         std::shared_ptr<PluginActivity> activity,
                                         std::string                     typeName)
    : mDescriptor(descriptor),
      mTypeName(!typeName.empty() ? std::move(typeName)
                                  : (descriptor != nullptr ? std::string(descriptor->name) : std::string("Object"))),
      mIdentityKey(rawPtr),
      mIsConst(isConst),
      mOwnerPlugin(std::move(ownerPlugin)),
      mActivity(std::move(activity)),
      mInstance(std::move(instance)),
      mRawPtr(rawPtr) {}

auto RevocableObjectCell::pin() const -> Expected<PinnedAccess> {
    PluginActivity::Guard guard;
    if (mActivity) {
        guard = mActivity->tryEnter();
        if (!guard) {
            return bridgeUnexpected(BridgeErrorCode::ObjectRevoked,
                                    "object '" + mTypeName + "' from plugin '" + mOwnerPlugin +
                                            "' is no longer available");
        }
    }
    std::shared_lock lock(mMutex);
    if (mRevoked.load(std::memory_order_acquire) || !mInstance || mRawPtr == nullptr) {
        return bridgeUnexpected(BridgeErrorCode::ObjectRevoked,
                                "object '" + mTypeName + "' from plugin '" + mOwnerPlugin + "' is no longer available");
    }
    return PinnedAccess{
            .guard    = std::move(guard),
            .instance = mInstance,
            .rawPtr   = mRawPtr,
    };
}

auto RevocableObjectCell::shareRaw() const -> std::pair<std::shared_ptr<void>, void*> {
    if (!alive()) {
        return {nullptr, nullptr};
    }
    std::shared_lock lock(mMutex);
    if (mRevoked.load(std::memory_order_acquire) || !mInstance || mRawPtr == nullptr) {
        return {nullptr, nullptr};
    }
    return {mInstance, mRawPtr};
}

void RevocableObjectCell::revoke() noexcept {
    if (mRevoked.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    mDescriptor.store(nullptr, std::memory_order_release);
    std::shared_ptr<void> doomed;
    {
        std::unique_lock lock(mMutex);
        doomed  = std::move(mInstance);
        mRawPtr = nullptr;
    }
    doomed.reset();
}

void BridgeAsyncState::resolve(BridgeValue value) {
    settle(Expected<BridgeValue>{std::move(value)});
}

void BridgeAsyncState::reject(Error error) {
    settle(Expected<BridgeValue>{unexpected(error)});
}

void BridgeAsyncState::settle(Expected<BridgeValue> outcome) {
    std::vector<Continuation> continuations;
    {
        std::lock_guard lock(mMutex);
        if (mOutcome.has_value()) {
            return;
        }
        mOutcome.emplace(std::move(outcome));
        continuations.swap(mContinuations);
    }
    mCv.notify_all();
    for (auto& cont: continuations) {
        if (cont) {
            try {
                cont(*mOutcome);
            } catch (...) {}
        }
    }
}

void BridgeAsyncState::then(Continuation continuation) {
    if (!continuation) {
        return;
    }
    std::optional<Expected<BridgeValue>> immediate;
    {
        std::lock_guard lock(mMutex);
        if (mOutcome.has_value()) {
            immediate = mOutcome;
        } else {
            mContinuations.push_back(std::move(continuation));
            return;
        }
    }
    try {
        continuation(*immediate);
    } catch (...) {}
}

bool BridgeAsyncState::ready() const {
    std::lock_guard lock(mMutex);
    return mOutcome.has_value();
}

auto BridgeAsyncState::wait() -> const Expected<BridgeValue>& {
    std::unique_lock lock(mMutex);
    mCv.wait(lock, [&] { return mOutcome.has_value(); });
    return *mOutcome;
}

CoreBridgeCallable::CoreBridgeCallable(std::shared_ptr<RevocableObjectCell> cell, const InvokeFn invokeFn) noexcept
    : mCell(std::move(cell)), mInvokeFn(invokeFn) {}

auto CoreBridgeCallable::ownerPlugin() const noexcept -> std::string_view {
    return mCell ? mCell->ownerPlugin() : std::string_view{"CloverCore"};
}

bool CoreBridgeCallable::alive() const noexcept {
    return mCell && mCell->alive() && mInvokeFn != nullptr;
}

bool CoreBridgeCallable::canInvokeSyncHere() const noexcept {
    return mInvokeFn != nullptr;
}

auto CoreBridgeCallable::invoke(const std::span<const BridgeValue> args) -> Expected<BridgeValue> {
    if (!mCell || mInvokeFn == nullptr) {
        return bridgeUnexpected(BridgeErrorCode::ObjectRevoked, "callback is no longer available");
    }
    auto pinned = mCell->pin();
    if (!pinned) {
        return unexpected(std::move(pinned).error());
    }
    ScopedOwnerPlugin         ownerScope(mCell->ownerPlugin());
    ThreadDispatchPluginScope dispatchScope(std::string(mCell->ownerPlugin()));
    return mInvokeFn(pinned->rawPtr, args);
}

auto CoreBridgeCallable::post(std::vector<BridgeValue> args) -> Expected<void> {
    auto res = invoke(args);
    if (!res) {
        return unexpected(std::move(res).error());
    }
    return {};
}

auto CoreBridgeCallable::invokeAsync(std::vector<BridgeValue> args) -> BridgeAsync {
    auto state = std::make_shared<BridgeAsyncState>();
    auto res   = invoke(args);
    if (!res) {
        state->reject(std::move(res).error());
    } else if (auto* nested = std::get_if<BridgeAsync>(&res->data); nested != nullptr && *nested) {
        return *nested;
    } else {
        state->resolve(std::move(*res));
    }
    return state;
}

auto currentOwnerPluginName() -> std::string {
    if (!tScopedOwnerPlugin.empty()) {
        return std::string(tScopedOwnerPlugin);
    }
    if (const auto current = Plugin::PluginManager::currentPlugin(); current && !current->name().empty()) {
        return std::string(current->name());
    }
    return "CloverCore";
}

auto acquirePluginActivity(const std::string_view pluginName) -> std::shared_ptr<PluginActivity> {
    return ReflectPluginRegistry::instance().acquireActivity(pluginName);
}

void registerPluginObjectCell(const std::string_view pluginName, std::weak_ptr<RevocableObjectCell> cell) {
    ReflectPluginRegistry::instance().registerCell(pluginName, std::move(cell));
}

ScopedOwnerPlugin::ScopedOwnerPlugin(const std::string_view pluginName) noexcept
    : mPrevious(std::exchange(tScopedOwnerPlugin, pluginName)) {}

ScopedOwnerPlugin::~ScopedOwnerPlugin() {
    tScopedOwnerPlugin = mPrevious;
}

} // namespace CloverNT::Reflect

namespace CloverNT::Core::Modules {

namespace Event = CloverNT::Event;

auto EventRuntime::onLoad() -> Expected<void> {
    return {};
}

auto EventRuntime::onUnload() -> Expected<void> {
    if (!tActiveDispatchPlugins.empty()) {
        return unexpected(ErrorCategory::Event,
                          CommonErrorCode::InvalidState,
                          "cannot unload event system while an event listener is running");
    }
    {
        std::unique_lock lock(mMutex);
        mChannels.clear();
        mSlotTopics.clear();
    }
    std::string stuck;
    if (!ReflectPluginRegistry::instance().closeAndDrainAll(5000, stuck)) {
        return unexpected(ErrorCategory::Event,
                          CommonErrorCode::OperationFailed,
                          "timed out waiting for active plugin callbacks to finish: " + stuck);
    }
    return {};
}

auto EventRuntime::onEnable() -> Expected<void> {
    return {};
}

auto EventRuntime::onDisable() -> Expected<void> {
    return {};
}

auto EventRuntime::addSlot(const std::string_view topic, Event::ListenerSlotPtr slot)
        -> Expected<Event::SubscriptionId> {
    if (topic.empty()) {
        return unexpected(ErrorCategory::Event, CommonErrorCode::InvalidArgument, "event topic cannot be empty");
    }
    if (!slot || slot->id() == Event::kInvalidSubscriptionId) {
        return unexpected(ErrorCategory::Event, CommonErrorCode::InvalidArgument, "event listener is invalid");
    }

    std::unique_lock lock(mMutex);
    if (!isEnabled()) {
        return unexpected(ErrorCategory::Event, CommonErrorCode::Inactive, "event system is not active");
    }
    if (mSlotTopics.contains(slot->id())) {
        return unexpected(ErrorCategory::Event, CommonErrorCode::AlreadyExists, "duplicate subscription id");
    }

    const auto id       = slot->id();
    auto&      channel  = mChannels[std::string(topic)];
    auto       nextList = std::make_shared<SlotList>();
    if (channel.snapshot) {
        nextList->reserve(channel.snapshot->size() + 1);
        for (const auto& existing: *channel.snapshot) {
            if (!existing->isConsumed()) {
                nextList->push_back(existing);
            }
        }
    }
    const auto insertPos = std::ranges::upper_bound(*nextList, slot, slotPrecedes);
    nextList->insert(insertPos, std::move(slot));
    channel.snapshot = std::move(nextList);
    mSlotTopics.emplace(id, std::string(topic));
    return id;
}

bool EventRuntime::removeSlot(const Event::SubscriptionId id) noexcept {
    if (id == Event::kInvalidSubscriptionId) {
        return false;
    }
    Event::ListenerSlotPtr removedSlot;
    {
        std::unique_lock lock(mMutex);
        const auto       topicIt = mSlotTopics.find(id);
        if (topicIt == mSlotTopics.end()) {
            return false;
        }
        const std::string topic = topicIt->second;
        mSlotTopics.erase(topicIt);

        if (const auto chanIt = mChannels.find(topic); chanIt != mChannels.end() && chanIt->second.snapshot) {
            auto nextList = std::make_shared<SlotList>();
            nextList->reserve(chanIt->second.snapshot->size());
            for (const auto& slot: *chanIt->second.snapshot) {
                if (slot->id() == id) {
                    removedSlot = slot;
                } else if (!slot->isConsumed()) {
                    nextList->push_back(slot);
                }
            }
            if (nextList->empty()) {
                mChannels.erase(chanIt);
            } else {
                chanIt->second.snapshot = std::move(nextList);
            }
        }
    }
    removedSlot.reset();
    return true;
}

bool EventRuntime::hasSubscription(const Event::SubscriptionId id) const noexcept {
    if (id == Event::kInvalidSubscriptionId) {
        return false;
    }
    std::shared_lock lock(mMutex);
    const auto       topicIt = mSlotTopics.find(id);
    if (topicIt == mSlotTopics.end()) {
        return false;
    }
    if (const auto chanIt = mChannels.find(topicIt->second); chanIt != mChannels.end() && chanIt->second.snapshot) {
        for (const auto& slot: *chanIt->second.snapshot) {
            if (slot->id() == id) {
                return !slot->isConsumed();
            }
        }
    }
    return false;
}

auto EventRuntime::listenerCount(const std::string_view topic) const noexcept -> std::size_t {
    std::shared_lock lock(mMutex);
    if (topic.empty()) {
        std::size_t total = 0;
        for (const auto& [snapshot]: mChannels | std::views::values) {
            if (snapshot) {
                for (const auto& slot: *snapshot) {
                    if (!slot->isConsumed()) {
                        ++total;
                    }
                }
            }
        }
        return total;
    }
    if (const auto it = mChannels.find(topic); it != mChannels.end() && it->second.snapshot) {
        std::size_t count = 0;
        for (const auto& slot: *it->second.snapshot) {
            if (!slot->isConsumed()) {
                ++count;
            }
        }
        return count;
    }
    return 0;
}

auto EventRuntime::topics() const -> std::vector<std::string> {
    std::shared_lock         lock(mMutex);
    std::vector<std::string> names;
    names.reserve(mChannels.size());
    for (const auto& [name, chan]: mChannels) {
        if (chan.snapshot && !chan.snapshot->empty()) {
            names.push_back(name);
        }
    }
    return names;
}

void EventRuntime::compactConsumedOnceSlots(const std::string_view topic) noexcept {
    std::vector<Event::ListenerSlotPtr> doomed;
    {
        std::unique_lock lock(mMutex);
        const auto       chanIt = mChannels.find(topic);
        if (chanIt == mChannels.end() || !chanIt->second.snapshot) {
            return;
        }
        bool anyConsumed = false;
        for (const auto& slot: *chanIt->second.snapshot) {
            if (slot->isConsumed()) {
                anyConsumed = true;
                break;
            }
        }
        if (!anyConsumed) {
            return;
        }
        auto nextList = std::make_shared<SlotList>();
        nextList->reserve(chanIt->second.snapshot->size());
        for (const auto& slot: *chanIt->second.snapshot) {
            if (slot->isConsumed()) {
                mSlotTopics.erase(slot->id());
                doomed.push_back(slot);
            } else {
                nextList->push_back(slot);
            }
        }
        if (nextList->empty()) {
            mChannels.erase(chanIt);
        } else {
            chanIt->second.snapshot = std::move(nextList);
        }
    }
    doomed.clear();
}

auto EventRuntime::dispatch(Event::EventInvocation& inv) -> Expected<bool> {
    ImmutableSlotList                           snapshot;
    std::vector<Reflect::PluginActivity::Guard> pinnedGuards;
    {
        std::shared_lock lock(mMutex);
        if (!isEnabled()) {
            return false;
        }
        const auto it = mChannels.find(inv.topic());
        if (it == mChannels.end() || !it->second.snapshot || it->second.snapshot->empty()) {
            return false;
        }
        snapshot = it->second.snapshot;
        pinnedGuards.reserve(snapshot->size());
        for (const auto& slot: *snapshot) {
            if (slot->activity()) {
                pinnedGuards.push_back(slot->activity()->tryEnter());
            } else {
                pinnedGuards.emplace_back();
            }
        }
    }

    struct SnapshotHoldCleanup {
        ImmutableSlotList&                           snap;
        std::vector<Reflect::PluginActivity::Guard>& guards;
        ~SnapshotHoldCleanup() {
            snap.reset();
            guards.clear();
        }
    } holdCleanup{snapshot, pinnedGuards};

    bool anyOnceConsumed = false;

    for (std::size_t i = 0; i < snapshot->size(); ++i) {
        const auto& slot = (*snapshot)[i];
        if (slot->activity() && (!pinnedGuards[i] || slot->activity()->isClosing())) {
            continue;
        }
        if (inv.isCancelled() && slot->priority() != Event::EventPriority::Monitor) {
            continue;
        }
        if (!slot->tryConsumeOnce()) {
            continue;
        }
        if (slot->isOnce()) {
            anyOnceConsumed = true;
        }

        try {
            Reflect::ScopedOwnerPlugin ownerScope(slot->ownerPlugin());
            ThreadDispatchPluginScope  dispatchScope(std::string(slot->ownerPlugin()));
            slot->dispatch(inv);
        } catch (const Exception& ex) {
            coreLogger().error("Event listener {} (owner '{}') faulted on '{}': {}",
                               slot->id(),
                               slot->ownerPlugin(),
                               inv.topic(),
                               ex.describe());
        } catch (const std::exception& ex) {
            coreLogger().error("Event listener {} (owner '{}') threw on '{}': {}",
                               slot->id(),
                               slot->ownerPlugin(),
                               inv.topic(),
                               normalizeErrorMessage(ex.what()));
        } catch (...) {
            coreLogger().error("Event listener {} (owner '{}') threw an unknown exception on '{}'",
                               slot->id(),
                               slot->ownerPlugin(),
                               inv.topic());
        }
    }

    if (anyOnceConsumed) {
        compactConsumedOnceSlots(inv.topic());
    }
    return inv.isCancelled();
}

auto EventRuntime::clearPlugin(const std::string_view pluginName) -> Expected<void> {
    if (pluginName.empty()) {
        return {};
    }
    ReflectPluginRegistry::instance().beginClose(pluginName);

    std::vector<Event::ListenerSlotPtr> removedSlots;
    {
        std::unique_lock lock(mMutex);
        for (auto chanIt = mChannels.begin(); chanIt != mChannels.end();) {
            const auto& snap = chanIt->second.snapshot;
            if (!snap || snap->empty()) {
                chanIt = mChannels.erase(chanIt);
                continue;
            }
            bool matches = false;
            for (const auto& slot: *snap) {
                if (slot->ownerPlugin() == pluginName || slot->isConsumed()) {
                    matches = true;
                    break;
                }
            }
            if (!matches) {
                ++chanIt;
                continue;
            }
            auto nextList = std::make_shared<SlotList>();
            nextList->reserve(snap->size());
            for (const auto& slot: *snap) {
                if (slot->ownerPlugin() == pluginName || slot->isConsumed()) {
                    mSlotTopics.erase(slot->id());
                    removedSlots.push_back(slot);
                } else {
                    nextList->push_back(slot);
                }
            }
            if (nextList->empty()) {
                chanIt = mChannels.erase(chanIt);
            } else {
                chanIt->second.snapshot = std::move(nextList);
                ++chanIt;
            }
        }
    }
    removedSlots.clear();
    return {};
}

auto EventRuntime::waitPluginIdle(const std::string_view pluginName) -> Expected<void> {
    if (pluginName.empty()) {
        return {};
    }
    if (tActiveDispatchPlugins.contains(pluginName)) {
        return unexpected(ErrorCategory::Event,
                          CommonErrorCode::InvalidState,
                          "cannot unload plugin '" + std::string(pluginName) + "' from inside its own active callback");
    }
    if (!ReflectPluginRegistry::instance().waitIdleAndRevoke(pluginName, 5000)) {
        return unexpected(ErrorCategory::Event,
                          CommonErrorCode::OperationFailed,
                          "timed out waiting for active callbacks of plugin '" + std::string(pluginName) +
                                  "' to finish");
    }
    return {};
}

} // namespace CloverNT::Core::Modules

namespace CloverNT::Event {

namespace {

    [[nodiscard]] auto requireEventRuntime() -> Expected<Core::Modules::ModuleRef<Core::Modules::EventRuntime>> {
        return Core::Modules::Manager::getInstance().requireModule<Core::Modules::EventRuntime>();
    }

    [[nodiscard]] auto validateTopicSyntax(const std::string_view topic) -> Expected<std::size_t> {
        if (topic.empty()) {
            return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::PermissionDenied, "event topic cannot be empty");
        }
        const auto colon = topic.find(':');
        if (colon == 0 || colon == topic.size() - 1) {
            return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::PermissionDenied,
                                             "invalid event topic '" + std::string(topic) +
                                                     "': expected '<namespace>:<topic>'");
        }
        return colon;
    }

} // namespace

namespace Detail {

    void reportListenerDispatchError(const std::string_view topic,
                                     const std::string_view ownerPlugin,
                                     const Error&           error) noexcept {
        try {
            coreLogger().error("Event listener (owner '{}') on '{}': {}", ownerPlugin, topic, error.message);
        } catch (...) {}
    }

} // namespace Detail

bool ScopedSubscription::unsubscribe() noexcept {
    const auto id = std::exchange(mId, kInvalidSubscriptionId);
    if (id == kInvalidSubscriptionId) {
        return false;
    }
    return EventBus::off(id);
}

auto EventBus::allocateSubscriptionId() noexcept -> SubscriptionId {
    static std::atomic<SubscriptionId> sNextId{1};
    return sNextId.fetch_add(1, std::memory_order_relaxed);
}

auto EventBus::normalizeSubscribeTopic(const std::string_view rawTopic,
                                       const std::string_view ownerPlugin,
                                       const bool             normalizeBare) -> Expected<std::string> {
    const auto colon = validateTopicSyntax(rawTopic);
    if (!colon) {
        return unexpected(colon.error());
    }
    if (*colon == std::string_view::npos) {
        if (!normalizeBare) {
            return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::PermissionDenied,
                                             "event topic '" + std::string(rawTopic) +
                                                     "' must include a '<namespace>:' prefix");
        }
        const std::string_view ns = ownerPlugin.empty() ? "CloverCore" : ownerPlugin;
        return std::string(ns) + ":" + std::string(rawTopic);
    }
    return std::string(rawTopic);
}

auto EventBus::normalizeEmitTopic(const std::string_view rawTopic,
                                  const std::string_view ownerPlugin,
                                  const bool             normalizeBare) -> Expected<std::string> {
    const auto colon = validateTopicSyntax(rawTopic);
    if (!colon) {
        return unexpected(colon.error());
    }
    const std::string_view effectiveOwner = ownerPlugin.empty() ? "CloverCore" : ownerPlugin;
    if (*colon == std::string_view::npos) {
        if (!normalizeBare) {
            return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::PermissionDenied,
                                             "event topic '" + std::string(rawTopic) +
                                                     "' must include a '<namespace>:' prefix");
        }
        if (effectiveOwner == "clovernt") {
            return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::PermissionDenied,
                                             "namespace 'clovernt' is reserved for built-in events");
        }
        return std::string(effectiveOwner) + ":" + std::string(rawTopic);
    }

    const std::string_view ns = rawTopic.substr(0, *colon);
    if (ns == "clovernt") {
        return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::PermissionDenied,
                                         "namespace 'clovernt' is reserved for built-in events");
    }
    if (effectiveOwner != "CloverCore" && ns != effectiveOwner) {
        return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::PermissionDenied,
                                         "plugin '" + std::string(effectiveOwner) +
                                                 "' cannot emit events in namespace '" + std::string(ns) + "'");
    }
    return std::string(rawTopic);
}

auto EventBus::addSlot(const std::string_view topic, ListenerSlotPtr slot) -> Expected<SubscriptionId> {
    auto runtime = requireEventRuntime();
    if (!runtime) {
        return unexpected(runtime.error());
    }
    return runtime->get().addSlot(topic, std::move(slot));
}

bool EventBus::off(const SubscriptionId id) noexcept {
    const auto runtime = requireEventRuntime();
    return runtime && runtime->get().removeSlot(id);
}

bool EventBus::hasSubscription(const SubscriptionId id) noexcept {
    const auto runtime = requireEventRuntime();
    return runtime && runtime->get().hasSubscription(id);
}

auto EventBus::listenerCount(const std::string_view topic) noexcept -> std::size_t {
    const auto runtime = requireEventRuntime();
    return runtime ? runtime->get().listenerCount(topic) : 0;
}

auto EventBus::topics() -> std::vector<std::string> {
    const auto runtime = requireEventRuntime();
    return runtime ? runtime->get().topics() : std::vector<std::string>{};
}

auto EventBus::dispatchPluginInvocation(EventInvocation& inv) -> Expected<bool> {
    auto runtime = requireEventRuntime();
    if (!runtime) {
        return unexpected(runtime.error());
    }
    return runtime->get().dispatch(inv);
}

auto EventBus::dispatchKernelInvocation(EventInvocation& inv, KernelPublishToken) -> Expected<bool> {
    auto runtime = requireEventRuntime();
    if (!runtime) {
        return unexpected(runtime.error());
    }
    return runtime->get().dispatch(inv);
}

auto EventBus::clearPlugin(const std::string_view pluginName) -> Expected<void> {
    auto runtime = requireEventRuntime();
    if (!runtime) {
        return unexpected(runtime.error());
    }
    return runtime->get().clearPlugin(pluginName);
}

auto EventBus::waitPluginIdle(const std::string_view pluginName) -> Expected<void> {
    auto runtime = requireEventRuntime();
    if (!runtime) {
        return unexpected(runtime.error());
    }
    return runtime->get().waitPluginIdle(pluginName);
}

} // namespace CloverNT::Event
