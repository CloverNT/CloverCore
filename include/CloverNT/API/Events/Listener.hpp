#pragma once
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

#include <CloverNT/API/Events/Event.hpp>
#include <CloverNT/API/Plugin/Plugin.hpp>

namespace CloverNT::Event {

using ListenerId = std::uint64_t;

enum class EventPriority {
    Lowest = 0,
    Low,
    Normal,
    High,
    Highest,
    Monitor,
};

class CloverNT_API ListenerBase {
public:
    explicit ListenerBase(EventPriority priority = EventPriority::Normal, std::weak_ptr<Plugin::Plugin> owner = {});
    virtual ~ListenerBase();

    ListenerBase(ListenerBase const&)            = delete;
    ListenerBase& operator=(ListenerBase const&) = delete;
    ListenerBase(ListenerBase&&)                 = delete;
    ListenerBase& operator=(ListenerBase&&)      = delete;

    [[nodiscard]] auto id() const noexcept -> ListenerId;
    [[nodiscard]] auto priority() const noexcept -> EventPriority;
    [[nodiscard]] auto owner() const noexcept -> std::weak_ptr<Plugin::Plugin>;

    virtual void call(Event& event) = 0;

private:
    ListenerId                    mId{};
    EventPriority                 mPriority{EventPriority::Normal};
    std::weak_ptr<Plugin::Plugin> mOwner;
};

using ListenerPtr = std::shared_ptr<ListenerBase>;

template <std::derived_from<Event> T>
class Listener final : public ListenerBase {
public:
    using Callback = std::function<void(T&)>;

    explicit Listener(Callback                      callback,
                      EventPriority                 priority = EventPriority::Normal,
                      std::weak_ptr<Plugin::Plugin> owner    = {})
        : ListenerBase(priority, std::move(owner)), mCallback(std::move(callback)) {}

    void call(Event& event) override {
        mCallback(static_cast<T&>(event));
    }

private:
    Callback mCallback;
};

} // namespace CloverNT::Event
