#include <CloverNT/API/Events/Listener.hpp>

namespace CloverNT::Event {

static std::atomic<ListenerId> sNextListenerId{1};

ListenerBase::ListenerBase(const EventPriority priority, std::weak_ptr<Plugin::Plugin> owner)
    : mId(sNextListenerId.fetch_add(1, std::memory_order_relaxed)), mPriority(priority), mOwner(std::move(owner)) {}

ListenerBase::~ListenerBase() = default;

auto ListenerBase::id() const noexcept -> ListenerId {
    return mId;
}

auto ListenerBase::priority() const noexcept -> EventPriority {
    return mPriority;
}

auto ListenerBase::owner() const noexcept -> std::weak_ptr<Plugin::Plugin> {
    return mOwner;
}

} // namespace CloverNT::Event
