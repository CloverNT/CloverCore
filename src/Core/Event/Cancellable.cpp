#include <CloverNT/API/Events/Cancellable.hpp>
#include <CloverNT/API/Events/Event.hpp>

namespace CloverNT::Event {

Event::~Event() = default;

bool Cancellable::isCancelled() const noexcept {
    return mCancelled.load(std::memory_order_acquire);
}

void Cancellable::setCancelled(const bool cancelled) noexcept {
    mCancelled.store(cancelled, std::memory_order_release);
}

} // namespace CloverNT::Event
