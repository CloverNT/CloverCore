#include <CloverNT/API/Events/DllEvent.hpp>
#include <CloverNT/API/Events/EventBus.hpp>
#include <CloverNT/API/Plugin/PluginManager.hpp>
#include <CloverNT/Core/Platform/DllEventSource.hpp>
#include <CloverNT/Runtime/Kernel.hpp>

#include <cstddef>

namespace CloverNT::Core::Platform {
namespace {

    Runtime::Kernel::Subscription gSubscription;

    void CloverNT_KERNEL_CALL onDllLoad(const char* const   baseName,
                                        const char* const   fullPath,
                                        const CloverAddress base,
                                        const std::size_t   size,
                                        void*) {
        Event::DllLoadEvent event(baseName != nullptr ? baseName : "",
                                  fullPath != nullptr ? fullPath : "",
                                  reinterpret_cast<void*>(base),
                                  size);
        Event::EventBus::getInstance().publish(event);
    }

} // namespace

void startDllEventSource() {
    (void) Event::EventBus::registerEvent<Event::DllLoadEvent>(Plugin::PluginManager::getPlugin("CloverCore"));
    gSubscription = Runtime::Kernel::onDllLoad(&onDllLoad);
}

void stopDllEventSource() {
    gSubscription.reset();
}

} // namespace CloverNT::Core::Platform
