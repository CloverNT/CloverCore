#pragma once

#include <CloverNT/API/Events/Cancellable.hpp>
#include <CloverNT/API/Events/Event.hpp>
#include <CloverNT/API/Nt/Packet.hpp>

#include <string_view>

namespace CloverNT::Event {

class PacketSendEvent final : public Event, public Cancellable {
public:
    static constexpr std::string_view kEventName = "CloverNT.PacketSend";

    explicit PacketSendEvent(Nt::Packet& packet) noexcept : mPacket(packet) {}

    [[nodiscard]] auto packet() const noexcept -> Nt::Packet& {
        return mPacket;
    }

private:
    Nt::Packet& mPacket;
};

class PacketRecvEvent final : public Event, public Cancellable {
public:
    static constexpr std::string_view kEventName = "CloverNT.PacketRecv";

    explicit PacketRecvEvent(Nt::Packet& packet) noexcept : mPacket(packet) {}

    [[nodiscard]] auto packet() const noexcept -> Nt::Packet& {
        return mPacket;
    }

private:
    Nt::Packet& mPacket;
};

class O3SendEvent final : public Event, public Cancellable {
public:
    static constexpr std::string_view kEventName = "CloverNT.O3Send";

    explicit O3SendEvent(Nt::O3Packet& packet) noexcept : mPacket(packet) {}

    [[nodiscard]] auto packet() const noexcept -> Nt::O3Packet& {
        return mPacket;
    }

private:
    Nt::O3Packet& mPacket;
};

class O3RecvEvent final : public Event, public Cancellable {
public:
    static constexpr std::string_view kEventName = "CloverNT.O3Recv";

    explicit O3RecvEvent(Nt::O3Packet& packet) noexcept : mPacket(packet) {}

    [[nodiscard]] auto packet() const noexcept -> Nt::O3Packet& {
        return mPacket;
    }

private:
    Nt::O3Packet& mPacket;
};

class CredentialEvent final : public Event {
public:
    static constexpr std::string_view kEventName = "CloverNT.Credential";

    explicit CredentialEvent(const Nt::Credential& credential) noexcept : mCredential(credential) {}

    [[nodiscard]] auto credential() const noexcept -> const Nt::Credential& {
        return mCredential;
    }

private:
    const Nt::Credential& mCredential;
};

} // namespace CloverNT::Event
