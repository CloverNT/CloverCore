#include <CloverNT/API/Events/BuiltInEvents.hpp>
#include <CloverNT/API/Events/EventBus.hpp>
#include <CloverNT/API/Logger.hpp>
#include <CloverNT/API/Nt/Packet.hpp>
#include <CloverNT/Core/Event/PacketBridge.hpp>
#include <CloverNT/Runtime/Kernel.h>

#include <array>
#include <cstddef>
#include <mutex>
#include <span>
#include <vector>

namespace CloverNT::Core::Event {

struct PacketBridgeAccess {
    [[nodiscard]] static constexpr auto token() noexcept -> CloverNT::Event::KernelPublishToken {
        return CloverNT::Event::KernelPublishToken{};
    }
};

namespace {

    using namespace CloverNT;

    std::mutex                      gMutex;
    std::vector<CloverSubscription> gSubs;

    auto logger() -> Logger& {
        static auto instance = LoggerRegistry::getInstance().getOrCreate("PacketBridge");
        return *instance;
    }

    template <typename PacketT>
    void
    applyVerdict(CloverEvent* const ev, const PacketT& packet, const Nt::Bytes& originalBody, const bool cancelled) {
        if (cancelled) {
            ev->cancelled = 1;
            return;
        }
        if (packet.body == originalBody) {
            return;
        }
        thread_local std::array<Nt::Bytes, 8> ring;
        thread_local std::size_t              cursor = 0;
        auto&                                 buffer = ring[cursor++ & (ring.size() - 1)];
        buffer.assign(packet.body.begin(), packet.body.end());
        if (buffer.empty()) {
            buffer.reserve(1);
        }
        ev->replacement       = buffer.data();
        ev->replacementLength = buffer.size();
    }

    void CloverNT_KERNEL_CALL onSsoSend(CloverEvent* const ev, void*) {
        try {
            if (ev == nullptr) {
                return;
            }
            auto packet = Nt::Codec::decodePacket(std::span{ev->data, ev->length});
            if (!packet) {
                return;
            }
            const auto originalBody = packet->body;
            const auto cancelled    = CloverNT::Event::EventBus::emit<CloverNT::Event::Events::PacketSend>(
                    *packet, PacketBridgeAccess::token());
            applyVerdict(ev, *packet, originalBody, cancelled && *cancelled);
        } catch (...) {}
    }

    void CloverNT_KERNEL_CALL onSsoRecv(CloverEvent* const ev, void*) {
        try {
            if (ev == nullptr) {
                return;
            }
            auto packet = Nt::Codec::decodePacket(std::span{ev->data, ev->length});
            if (!packet) {
                return;
            }
            const auto originalBody = packet->body;
            const auto cancelled    = CloverNT::Event::EventBus::emit<CloverNT::Event::Events::PacketRecv>(
                    *packet, PacketBridgeAccess::token());
            applyVerdict(ev, *packet, originalBody, cancelled && *cancelled);
        } catch (...) {}
    }

    void CloverNT_KERNEL_CALL onO3Send(CloverEvent* const ev, void*) {
        try {
            if (ev == nullptr) {
                return;
            }
            auto packet = Nt::Codec::decodeO3(std::span{ev->data, ev->length});
            if (!packet) {
                return;
            }
            const auto originalBody = packet->body;
            const auto cancelled    = CloverNT::Event::EventBus::emit<CloverNT::Event::Events::O3Send>(
                    *packet, PacketBridgeAccess::token());
            applyVerdict(ev, *packet, originalBody, cancelled && *cancelled);
        } catch (...) {}
    }

    void CloverNT_KERNEL_CALL onO3Recv(CloverEvent* const ev, void*) {
        try {
            if (ev == nullptr) {
                return;
            }
            auto packet = Nt::Codec::decodeO3(std::span{ev->data, ev->length});
            if (!packet) {
                return;
            }
            const auto originalBody = packet->body;
            const auto cancelled    = CloverNT::Event::EventBus::emit<CloverNT::Event::Events::O3Recv>(
                    *packet, PacketBridgeAccess::token());
            applyVerdict(ev, *packet, originalBody, cancelled && *cancelled);
        } catch (...) {}
    }

    void CloverNT_KERNEL_CALL onCredential(CloverEvent* const ev, void*) {
        try {
            if (ev == nullptr) {
                return;
            }
            const auto credential = Nt::Codec::decodeCredential(std::span{ev->data, ev->length});
            if (!credential) {
                return;
            }
            (void) CloverNT::Event::EventBus::emit<CloverNT::Event::Events::Credential>(*credential,
                                                                                        PacketBridgeAccess::token());
        } catch (...) {}
    }

} // namespace

void startPacketBridge() {
    std::scoped_lock lock(gMutex);
    if (!gSubs.empty()) {
        return;
    }

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
