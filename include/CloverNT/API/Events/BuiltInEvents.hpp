#pragma once

#include <CloverNT/API/Events/EventTopic.hpp>
#include <CloverNT/API/Nt/Packet.hpp>

#include <cstdint>
#include <string>
#include <tuple>

namespace CloverNT::Event {

struct DllLoadInfo {
    std::string   baseName;
    std::string   fullPath;
    std::uint64_t baseAddress{};
    std::uint64_t imageSize{};

    [[nodiscard]] auto moduleHandle() const noexcept -> void* {
        return reinterpret_cast<void*>(static_cast<std::uintptr_t>(baseAddress));
    }
};

namespace Events {

    using DllLoad = EventTopic<"clovernt:dll-load",
                               DllLoadInfo,
                               /*Mutable=*/false,
                               /*Cancellable=*/false,
                               /*KernelOnly=*/true>;

    using PacketSend = EventTopic<"clovernt:packet-send",
                                  Nt::Packet,
                                  /*Mutable=*/true,
                                  /*Cancellable=*/true,
                                  /*KernelOnly=*/true>;

    using PacketRecv = EventTopic<"clovernt:packet-recv",
                                  Nt::Packet,
                                  /*Mutable=*/true,
                                  /*Cancellable=*/true,
                                  /*KernelOnly=*/true>;

    using O3Send = EventTopic<"clovernt:o3-send",
                              Nt::O3Packet,
                              /*Mutable=*/true,
                              /*Cancellable=*/true,
                              /*KernelOnly=*/true>;

    using O3Recv = EventTopic<"clovernt:o3-recv",
                              Nt::O3Packet,
                              /*Mutable=*/true,
                              /*Cancellable=*/true,
                              /*KernelOnly=*/true>;

    using Credential = EventTopic<"clovernt:credential",
                                  Nt::Credential,
                                  /*Mutable=*/false,
                                  /*Cancellable=*/false,
                                  /*KernelOnly=*/true>;

    using All = std::tuple<DllLoad, PacketSend, PacketRecv, O3Send, O3Recv, Credential>;

} // namespace Events

} // namespace CloverNT::Event
