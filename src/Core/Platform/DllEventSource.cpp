#include <CloverNT/API/Events/BuiltInEvents.hpp>
#include <CloverNT/API/Events/EventBus.hpp>
#include <CloverNT/Core/Platform/DllEventSource.hpp>
#include <CloverNT/Runtime/Kernel.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>

namespace CloverNT::Core::Platform {

struct DllEventSourceAccess {
    [[nodiscard]] static constexpr auto token() noexcept -> CloverNT::Event::KernelPublishToken {
        return CloverNT::Event::KernelPublishToken{};
    }
};

namespace {

    std::mutex                    gSubMutex;
    Runtime::Kernel::Subscription gSubscription;

    void CloverNT_KERNEL_CALL onDllLoad(const char* const   baseName,
                                        const char* const   fullPath,
                                        const CloverAddress base,
                                        const std::size_t   size,
                                        void*) {
        try {
            const CloverNT::Event::DllLoadInfo info{
                    .baseName    = baseName != nullptr ? baseName : "",
                    .fullPath    = fullPath != nullptr ? fullPath : "",
                    .baseAddress = static_cast<std::uint64_t>(base),
                    .imageSize   = static_cast<std::uint64_t>(size),
            };
            (void) CloverNT::Event::EventBus::emit<CloverNT::Event::Events::DllLoad>(info,
                                                                                     DllEventSourceAccess::token());
        } catch (...) {}
    }

} // namespace

void startDllEventSource() {
    std::lock_guard lock(gSubMutex);
    gSubscription = Runtime::Kernel::onDllLoad(&onDllLoad);
}

void stopDllEventSource() {
    Runtime::Kernel::Subscription old;
    {
        std::lock_guard lock(gSubMutex);
        old = std::move(gSubscription);
    }
    old.reset();
}

} // namespace CloverNT::Core::Platform
