#include <CloverNT/API/Events/DllEvent.hpp>
#include <CloverNT/API/Events/Event.hpp>
#include <CloverNT/API/Events/PacketEvent.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/EventExports.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace CloverNT::Core::Modules::bridge {

namespace {

    auto toHex(const void* pointer) -> std::string {
        return std::format("0x{:x}", reinterpret_cast<std::uintptr_t>(pointer));
    }

    auto hexEncode(const std::span<const std::uint8_t> bytes) -> std::string {
        static constexpr char kDigits[] = "0123456789abcdef";
        std::string           out;
        out.reserve(bytes.size() * 2);
        for (const auto byte: bytes) {
            out.push_back(kDigits[(byte >> 4) & 0xF]);
            out.push_back(kDigits[byte & 0xF]);
        }
        return out;
    }

    auto encName(const Nt::Encryption enc) -> const char* {
        switch (enc) {
        case Nt::Encryption::None:
            return "none";
        case Nt::Encryption::D2Key:
            return "d2key";
        case Nt::Encryption::ZeroKey:
            return "zero";
        }
        return "?";
    }

    auto packetToJson(const Nt::Packet& packet) -> nlohmann::json {
        return nlohmann::json{
                {"seq", packet.seq},
                {"command", packet.command},
                {"uin", packet.uin},
                {"encryption", encName(packet.encryption)},
                {"body", hexEncode(packet.body)},
        };
    }

    auto o3ToJson(const Nt::O3Packet& packet) -> nlohmann::json {
        return nlohmann::json{
                {"command", packet.command},
                {"body", hexEncode(packet.body)},
        };
    }

    auto dllLoadToPayload(Event::Event& event) -> nlohmann::json {
        const auto& dllLoad = dynamic_cast<Event::DllLoadEvent&>(event);
        return nlohmann::json{
                {"baseName", dllLoad.baseName()},
                {"fullPath", dllLoad.fullPath()},
                {"baseAddress", toHex(dllLoad.baseAddress())},
                {"imageSize", dllLoad.imageSize()},
        };
    }

    auto packetSendToPayload(Event::Event& event) -> nlohmann::json {
        return packetToJson(dynamic_cast<Event::PacketSendEvent&>(event).packet());
    }

    auto packetRecvToPayload(Event::Event& event) -> nlohmann::json {
        return packetToJson(dynamic_cast<Event::PacketRecvEvent&>(event).packet());
    }

    auto o3SendToPayload(Event::Event& event) -> nlohmann::json {
        return o3ToJson(dynamic_cast<Event::O3SendEvent&>(event).packet());
    }

    auto o3RecvToPayload(Event::Event& event) -> nlohmann::json {
        return o3ToJson(dynamic_cast<Event::O3RecvEvent&>(event).packet());
    }

    auto credentialToPayload(Event::Event& event) -> nlohmann::json {
        const auto& [uin, a2, d2, d2Key] = dynamic_cast<Event::CredentialEvent&>(event).credential();
        return nlohmann::json{
                {"uin", uin},
                {"a2", hexEncode(a2)},
                {"d2", hexEncode(d2)},
                {"d2Key", hexEncode(d2Key)},
        };
    }

} // namespace

auto nativeEventExports() -> std::span<const NativeEventExport> {
    static constexpr std::array<NativeEventExport, 6> table{{
            NativeEventExport{.jsName    = "dll-load",
                              .busKey    = Event::getEventName<Event::DllLoadEvent>(),
                              .toPayload = &dllLoadToPayload},
            NativeEventExport{.jsName    = "packet-send",
                              .busKey    = Event::getEventName<Event::PacketSendEvent>(),
                              .toPayload = &packetSendToPayload},
            NativeEventExport{.jsName    = "packet-recv",
                              .busKey    = Event::getEventName<Event::PacketRecvEvent>(),
                              .toPayload = &packetRecvToPayload},
            NativeEventExport{.jsName    = "o3-send",
                              .busKey    = Event::getEventName<Event::O3SendEvent>(),
                              .toPayload = &o3SendToPayload},
            NativeEventExport{.jsName    = "o3-recv",
                              .busKey    = Event::getEventName<Event::O3RecvEvent>(),
                              .toPayload = &o3RecvToPayload},
            NativeEventExport{.jsName    = "credential",
                              .busKey    = Event::getEventName<Event::CredentialEvent>(),
                              .toPayload = &credentialToPayload},
    }};
    return table;
}

auto findNativeExport(const std::string_view jsName) -> const NativeEventExport* {
    for (const auto& export_: nativeEventExports()) {
        if (export_.jsName == jsName) {
            return &export_;
        }
    }
    return nullptr;
}

} // namespace CloverNT::Core::Modules::bridge
