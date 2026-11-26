#pragma once

#include <CloverNT/API/Events/EventBus.hpp>
#include <CloverNT/Core/Modules/ModuleBase.hpp>

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <shared_mutex>
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

    [[nodiscard]] auto addSlot(std::string_view topic, CloverNT::Event::ListenerSlotPtr slot)
            -> Expected<CloverNT::Event::SubscriptionId>;
    bool               removeSlot(CloverNT::Event::SubscriptionId id) noexcept;
    [[nodiscard]] bool hasSubscription(CloverNT::Event::SubscriptionId id) const noexcept;
    [[nodiscard]] auto listenerCount(std::string_view topic = {}) const noexcept -> std::size_t;
    [[nodiscard]] auto topics() const -> std::vector<std::string>;

    [[nodiscard]] auto dispatch(CloverNT::Event::EventInvocation& inv) -> Expected<bool>;

    [[nodiscard]] auto clearPlugin(std::string_view pluginName) -> Expected<void>;
    [[nodiscard]] auto waitPluginIdle(std::string_view pluginName) -> Expected<void>;

private:
    using SlotList          = std::vector<CloverNT::Event::ListenerSlotPtr>;
    using ImmutableSlotList = std::shared_ptr<const SlotList>;

    struct EventChannel {
        ImmutableSlotList snapshot;
    };

    void compactConsumedOnceSlots(std::string_view topic) noexcept;

    mutable std::shared_mutex                                        mMutex;
    std::map<std::string, EventChannel, std::less<>>                 mChannels;
    std::unordered_map<CloverNT::Event::SubscriptionId, std::string> mSlotTopics;
};

} // namespace CloverNT::Core::Modules
