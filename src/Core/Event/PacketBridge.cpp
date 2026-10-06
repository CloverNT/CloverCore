#include <CloverNT/API/Events/EventBus.hpp>
#include <CloverNT/API/Events/PacketEvent.hpp>
#include <CloverNT/API/Logger.hpp>
#include <CloverNT/API/Nt/Packet.hpp>
#include <CloverNT/API/Plugin/PluginManager.hpp>
#include <CloverNT/Core/Event/PacketBridge.hpp>
#include <CloverNT/Runtime/Kernel.h>

#include <cstddef>
#include <mutex>
#include <span>
#include <vector>

namespace CloverNT::Core::Event {
namespace {

    using namespace CloverNT;

    std::mutex                      gMutex;
    std::vector<CloverSubscription> gSubs;

    auto logger() -> Logger& {
        static auto instance = LoggerRegistry::getInstance().getOrCreate("PacketBridge");
        return *instance;
    }

    template <typename PacketT>
    void applyVerdict(CloverEvent* const ev, const PacketT& packet, const bool cancelled) {
        if (cancelled) {
            ev->cancelled = 1;
            return;
        }
        thread_local Nt::Bytes buffer;
        buffer.assign(packet.body.begin(), packet.body.end());
        if (buffer.empty()) {
            buffer.reserve(1);
        }
        ev->replacement       = buffer.data();
        ev->replacementLength = buffer.size();
    }

    void CloverNT_KERNEL_CALL onSsoSend(CloverEvent* const ev, void*) {
        auto packet = Nt::Codec::decodePacket(std::span{ev->data, ev->length});
        if (!packet) {
            return;
        }
        CloverNT::Event::PacketSendEvent event{*packet};
        CloverNT::Event::EventBus::getInstance().publish(event);
        applyVerdict(ev, *packet, event.isCancelled());
    }

    void CloverNT_KERNEL_CALL onSsoRecv(CloverEvent* const ev, void*) {
        auto packet = Nt::Codec::decodePacket(std::span{ev->data, ev->length});
        if (!packet) {
            return;
        }
        CloverNT::Event::PacketRecvEvent event{*packet};
        CloverNT::Event::EventBus::getInstance().publish(event);
        applyVerdict(ev, *packet, event.isCancelled());
    }

    void CloverNT_KERNEL_CALL onO3Send(CloverEvent* const ev, void*) {
        auto packet = Nt::Codec::decodeO3(std::span{ev->data, ev->length});
        if (!packet) {
            return;
        }
        CloverNT::Event::O3SendEvent event{*packet};
        CloverNT::Event::EventBus::getInstance().publish(event);
        applyVerdict(ev, *packet, event.isCancelled());
    }

    void CloverNT_KERNEL_CALL onO3Recv(CloverEvent* const ev, void*) {
        auto packet = Nt::Codec::decodeO3(std::span{ev->data, ev->length});
        if (!packet) {
            return;
        }
        CloverNT::Event::O3RecvEvent event{*packet};
        CloverNT::Event::EventBus::getInstance().publish(event);
        applyVerdict(ev, *packet, event.isCancelled());
    }

    void CloverNT_KERNEL_CALL onCredential(CloverEvent* const ev, void*) {
        const auto credential = Nt::Codec::decodeCredential(std::span{ev->data, ev->length});
        if (!credential) {
            return;
        }
        CloverNT::Event::CredentialEvent event{*credential};
        CloverNT::Event::EventBus::getInstance().publish(event); // not cancellable
    }

} // namespace

void startPacketBridge() {
    std::scoped_lock lock(gMutex);
    if (!gSubs.empty()) {
        return;
    }

    const auto owner = Plugin::PluginManager::getPlugin("CloverCore");
    (void) CloverNT::Event::EventBus::registerEvent<CloverNT::Event::PacketSendEvent>(owner);
    (void) CloverNT::Event::EventBus::registerEvent<CloverNT::Event::PacketRecvEvent>(owner);
    (void) CloverNT::Event::EventBus::registerEvent<CloverNT::Event::O3SendEvent>(owner);
    (void) CloverNT::Event::EventBus::registerEvent<CloverNT::Event::O3RecvEvent>(owner);
    (void) CloverNT::Event::EventBus::registerEvent<CloverNT::Event::CredentialEvent>(owner);

    gSubs.push_back(CloverEventSubscribe(Nt::Events::kSsoSend.data(), &onSsoSend, nullptr));
    gSubs.push_back(CloverEventSubscribe(Nt::Events::kSsoRecv.data(), &onSsoRecv, nullptr));
    gSubs.push_back(CloverEventSubscribe(Nt::Events::kO3Send.data(), &onO3Send, nullptr));
    gSubs.push_back(CloverEventSubscribe(Nt::Events::kO3Recv.data(), &onO3Recv, nullptr));
    gSubs.push_back(CloverEventSubscribe(Nt::Events::kCredential.data(), &onCredential, nullptr));
    logger().info("packet bridge active: {} kernel event subscriptions.", gSubs.size());
}

void stopPacketBridge() {
    std::scoped_lock lock(gMutex);
    for (const auto sub: gSubs) {
        CloverEventUnsubscribe(sub);
    }
    gSubs.clear();
}

} // namespace CloverNT::Core::Event
