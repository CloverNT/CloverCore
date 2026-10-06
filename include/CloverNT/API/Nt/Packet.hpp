#pragma once

#include <CloverNT/Runtime/Nt.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace CloverNT::Nt {

/// Raw byte payload.
using Bytes = std::vector<std::uint8_t>;

/// Encryption applied to an SSO packet body.
enum class Encryption : std::uint8_t {
    None    = 0, ///< Plaintext body.
    D2Key   = 1, ///< Encrypted with the session d2 key.
    ZeroKey = 2, ///< Encrypted with the zero key (login handshake).
};

/// A single SSO packet.
///
/// For received packets `body` is the decrypted payload; for outgoing packets it
/// is the plaintext payload to send.
struct Packet {
    std::uint32_t seq{};
    std::string   command;
    std::string   uin;
    Encryption    encryption{Encryption::None};
    Bytes         body;
};

/// A plaintext o3 secure-channel packet.
struct O3Packet {
    std::string command;
    Bytes       body;
};

/// The captured session credentials.
struct Credential {
    std::string                  uin;
    Bytes                        a2;
    Bytes                        d2;
    std::array<std::uint8_t, 16> d2Key{};
};

namespace Events {
    inline constexpr std::string_view kSsoSend    = "packet::sso::send";
    inline constexpr std::string_view kSsoRecv    = "packet::sso::recv";
    inline constexpr std::string_view kO3Send     = "packet::o3::send";
    inline constexpr std::string_view kO3Recv     = "packet::o3::recv";
    inline constexpr std::string_view kCredential = "packet::credential";
} // namespace Events

/// Default timeout for the blocking wait helpers.
inline constexpr std::chrono::milliseconds kDefaultTimeout{5000};

namespace Codec {

    /// Upper bound on any single length field, so a corrupt prefix can't request a
    /// huge allocation. 64 MiB comfortably exceeds any real SSO/o3 packet.
    inline constexpr std::uint32_t kMaxField = 64u * 1024u * 1024u;

    namespace Detail {

        inline void putU32(Bytes& out, const std::uint32_t value) {
            out.push_back(static_cast<std::uint8_t>(value & 0xFF));
            out.push_back(static_cast<std::uint8_t>(value >> 8 & 0xFF));
            out.push_back(static_cast<std::uint8_t>(value >> 16 & 0xFF));
            out.push_back(static_cast<std::uint8_t>(value >> 24 & 0xFF));
        }

        inline void putBytes(Bytes& out, const std::span<const std::uint8_t> bytes) {
            putU32(out, static_cast<std::uint32_t>(bytes.size()));
            out.insert(out.end(), bytes.begin(), bytes.end());
        }

        inline void putString(Bytes& out, const std::string_view str) {
            putBytes(out, std::span{reinterpret_cast<const std::uint8_t*>(str.data()), str.size()});
        }

        /// Bounds-checked forward cursor over a borrowed buffer.
        class Reader {
        public:
            explicit Reader(const std::span<const std::uint8_t> data) noexcept : mData(data) {}

            [[nodiscard]] bool readU32(std::uint32_t& out) noexcept {
                if (mData.size() - mPos < 4) {
                    return false;
                }
                out = static_cast<std::uint32_t>(mData[mPos]) | static_cast<std::uint32_t>(mData[mPos + 1]) << 8 |
                      static_cast<std::uint32_t>(mData[mPos + 2]) << 16 |
                      static_cast<std::uint32_t>(mData[mPos + 3]) << 24;
                mPos += 4;
                return true;
            }

            [[nodiscard]] bool readU8(std::uint8_t& out) noexcept {
                if (mData.size() - mPos < 1) {
                    return false;
                }
                out = mData[mPos++];
                return true;
            }

            [[nodiscard]] bool readBytes(Bytes& out) {
                std::uint32_t length = 0;
                if (!readU32(length) || length > kMaxField || mData.size() - mPos < length) {
                    return false;
                }
                out.assign(mData.begin() + static_cast<std::ptrdiff_t>(mPos),
                           mData.begin() + static_cast<std::ptrdiff_t>(mPos + length));
                mPos += length;
                return true;
            }

            [[nodiscard]] bool readString(std::string& out) {
                std::uint32_t length = 0;
                if (!readU32(length) || length > kMaxField || mData.size() - mPos < length) {
                    return false;
                }
                out.assign(reinterpret_cast<const char*>(mData.data()) + mPos, length);
                mPos += length;
                return true;
            }

            [[nodiscard]] bool readRaw(std::uint8_t* dest, const std::size_t count) noexcept {
                if (mData.size() - mPos < count) {
                    return false;
                }
                for (std::size_t i = 0; i < count; ++i) {
                    dest[i] = mData[mPos + i];
                }
                mPos += count;
                return true;
            }

        private:
            std::span<const std::uint8_t> mData;
            std::size_t                   mPos{0};
        };

    } // namespace Detail

    [[nodiscard]] inline Bytes encodePacket(const Packet& packet) {
        Bytes out;
        Detail::putU32(out, packet.seq);
        out.push_back(static_cast<std::uint8_t>(packet.encryption));
        Detail::putString(out, packet.command);
        Detail::putString(out, packet.uin);
        Detail::putBytes(out, packet.body);
        return out;
    }

    [[nodiscard]] inline std::optional<Packet> decodePacket(const std::span<const std::uint8_t> data) {
        Detail::Reader reader(data);
        Packet         packet;
        std::uint8_t   enc = 0;
        if (!reader.readU32(packet.seq) || !reader.readU8(enc) || !reader.readString(packet.command) ||
            !reader.readString(packet.uin) || !reader.readBytes(packet.body)) {
            return std::nullopt;
        }
        packet.encryption = static_cast<Encryption>(enc);
        return packet;
    }

    [[nodiscard]] inline Bytes encodeO3(const O3Packet& packet) {
        Bytes out;
        Detail::putString(out, packet.command);
        Detail::putBytes(out, packet.body);
        return out;
    }

    [[nodiscard]] inline std::optional<O3Packet> decodeO3(const std::span<const std::uint8_t> data) {
        Detail::Reader reader(data);
        O3Packet       packet;
        if (!reader.readString(packet.command) || !reader.readBytes(packet.body)) {
            return std::nullopt;
        }
        return packet;
    }

    [[nodiscard]] inline Bytes encodeCredential(const Credential& credential) {
        Bytes out;
        Detail::putString(out, credential.uin);
        Detail::putBytes(out, credential.a2);
        Detail::putBytes(out, credential.d2);
        out.insert(out.end(), credential.d2Key.begin(), credential.d2Key.end());
        return out;
    }

    [[nodiscard]] inline std::optional<Credential> decodeCredential(const std::span<const std::uint8_t> data) {
        Detail::Reader reader(data);
        Credential     credential;
        if (!reader.readString(credential.uin) || !reader.readBytes(credential.a2) ||
            !reader.readBytes(credential.d2) || !reader.readRaw(credential.d2Key.data(), credential.d2Key.size())) {
            return std::nullopt;
        }
        return credential;
    }

} // namespace Codec

namespace Detail {

    inline void CloverNT_KERNEL_CALL packetSink(const std::uint8_t* const data,
                                                const std::size_t         len,
                                                void* const               user) {
        *static_cast<std::optional<Packet>*>(user) = Codec::decodePacket(std::span{data, len});
    }

    inline void CloverNT_KERNEL_CALL credentialSink(const std::uint8_t* const data,
                                                    const std::size_t         len,
                                                    void* const               user) {
        *static_cast<std::optional<Credential>*>(user) = Codec::decodeCredential(std::span{data, len});
    }

} // namespace Detail

/// Whether the low-level direct send path has been discovered from wrapper.node.
[[nodiscard]] inline auto ready() -> bool {
    return CloverPacketReady() != 0;
}

/// The most recently captured session credentials, if any.
[[nodiscard]] inline auto credential() -> std::optional<Credential> {
    std::optional<Credential> out;
    (void) CloverPacketCredential(&Detail::credentialSink, &out);
    return out;
}

/// Send a packet (fire-and-forget). Returns the captured sequence id when the
/// interceptor was able to observe it; std::nullopt if unavailable or uncaptured.
[[nodiscard]] inline auto send(const std::string_view command, const std::span<const std::uint8_t> body)
        -> std::optional<std::uint32_t> {
    std::uint32_t seq = 0;
    if (CloverPacketSend(command.data(), command.size(), body.data(), body.size(), &seq) == 0) {
        return std::nullopt;
    }
    return seq != 0 ? std::optional{seq} : std::nullopt;
}

/// Wait for the next received packet with the given service command.
[[nodiscard]] inline auto receive(const std::string_view          command,
                                  const std::chrono::milliseconds timeout = kDefaultTimeout) -> std::optional<Packet> {
    std::optional<Packet> out;
    (void) CloverPacketReceiveByCommand(
            command.data(), command.size(), static_cast<std::uint32_t>(timeout.count()), &Detail::packetSink, &out);
    return out;
}

/// Wait for the next received packet with the given sequence id.
[[nodiscard]] inline auto receive(const std::uint32_t seq, const std::chrono::milliseconds timeout = kDefaultTimeout)
        -> std::optional<Packet> {
    std::optional<Packet> out;
    (void) CloverPacketReceiveBySequence(seq, static_cast<std::uint32_t>(timeout.count()), &Detail::packetSink, &out);
    return out;
}

/// Send a packet and wait for its response.
[[nodiscard]] inline auto call(const std::string_view              command,
                               const std::span<const std::uint8_t> body,
                               const std::chrono::milliseconds     timeout = kDefaultTimeout) -> std::optional<Packet> {
    std::uint32_t seq = 0;
    if (CloverPacketSend(command.data(), command.size(), body.data(), body.size(), &seq) == 0) {
        return std::nullopt;
    }
    return seq != 0 ? receive(seq, timeout) : receive(command, timeout);
}

} // namespace CloverNT::Nt
