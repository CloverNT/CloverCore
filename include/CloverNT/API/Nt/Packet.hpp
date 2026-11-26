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

#if defined(CloverNT_SYSTEM_WINDOWS) || defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #ifdef GetObject
        #undef GetObject
    #endif
#else
    #include <dlfcn.h>
#endif

#ifndef CLOVER_FAMILY_PATCHGUARD
    #define CLOVER_FAMILY_PATCHGUARD            0x00000004u
    #undef CLOVER_FAMILY_ALL
    #define CLOVER_FAMILY_ALL                   0x00000007u
    #define CLOVER_PATCHGUARD_NEXT              0x00000008u
    #define CLOVER_PATCHGUARD_ACTIVE            0x00000001u
    #define CLOVER_PATCHGUARD_ENABLED           0x00000002u
    #define CLOVER_PATCHGUARD_STATE(lockedMask) (((std::uint32_t) (lockedMask) >> 2) & 0x3u)
#endif

namespace CloverNT::Nt {

using Bytes = std::vector<std::uint8_t>;

enum class Encryption : std::uint8_t {
    None    = 0,
    D2Key   = 1,
    ZeroKey = 2,
};

struct Packet {
    std::uint32_t seq{};
    std::string   command;
    std::string   uin;
    std::string   uid;
    Encryption    encryption{Encryption::None};
    Bytes         body;

    bool injected{false};
};

struct O3Packet {
    std::string command;
    Bytes       body;
};

struct Credential {
    std::string                  uin;
    std::string                  uid;
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

inline constexpr std::chrono::milliseconds kDefaultTimeout{5000};

namespace Codec {

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
        out.push_back(packet.injected ? std::uint8_t{1} : std::uint8_t{0});
        Detail::putString(out, packet.uid);
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

        if (std::uint8_t flags = 0; reader.readU8(flags)) {
            packet.injected = (flags & 1U) != 0;
            (void) reader.readString(packet.uid);
        }
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
        Detail::putString(out, credential.uid);
        return out;
    }

    [[nodiscard]] inline std::optional<Credential> decodeCredential(const std::span<const std::uint8_t> data) {
        Detail::Reader reader(data);
        Credential     credential;
        if (!reader.readString(credential.uin) || !reader.readBytes(credential.a2) ||
            !reader.readBytes(credential.d2) || !reader.readRaw(credential.d2Key.data(), credential.d2Key.size())) {
            return std::nullopt;
        }
        (void) reader.readString(credential.uid);
        return credential;
    }

} // namespace Codec

namespace Detail {

    using GetLockedFn  = std::uint32_t(CloverNT_KERNEL_CALL*)();
    using PacketCallFn = CloverBool(CloverNT_KERNEL_CALL*)(const char*,
                                                           std::size_t,
                                                           const std::uint8_t*,
                                                           std::size_t,
                                                           std::uint32_t,
                                                           CloverPacketSink,
                                                           void*);

    [[nodiscard]] inline auto resolveLoaderSymbol(const char* const name) noexcept -> void* {
        if (name == nullptr || name[0] == '\0') {
            return nullptr;
        }
#if defined(CloverNT_SYSTEM_WINDOWS) || defined(_WIN32)
        HMODULE loaderModule = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&CloverPluginsDirectory),
                               &loaderModule) == 0 ||
            loaderModule == nullptr) {
            loaderModule = GetModuleHandleW(L"CloverLoader.dll");
        }
        return loaderModule != nullptr ? reinterpret_cast<void*>(GetProcAddress(loaderModule, name)) : nullptr;
#else
        if (void* const fn = ::dlsym(RTLD_DEFAULT, name); fn != nullptr) {
            return fn;
        }
        Dl_info info{};
        if (::dladdr(reinterpret_cast<void*>(&CloverPluginsDirectory), &info) != 0 && info.dli_fname != nullptr) {
            if (void* const handle = ::dlopen(info.dli_fname, RTLD_NOLOAD | RTLD_NOW); handle != nullptr) {
                void* const fn = ::dlsym(handle, name);
                ::dlclose(handle);
                return fn;
            }
        }
        return nullptr;
#endif
    }

    [[nodiscard]] inline auto getLockedFn() noexcept -> GetLockedFn {
        static const auto fn = reinterpret_cast<GetLockedFn>(resolveLoaderSymbol("CloverGetLocked"));
        return fn;
    }

    [[nodiscard]] inline auto packetCallFn() noexcept -> PacketCallFn {
        static const auto fn = reinterpret_cast<PacketCallFn>(resolveLoaderSymbol("CloverPacketCall"));
        return fn;
    }

    inline void CloverNT_KERNEL_CALL packetSink(const std::uint8_t* const data,
                                                const std::size_t         len,
                                                void* const               user) {
        try {
            if (user != nullptr) {
                *static_cast<std::optional<Packet>*>(user) = Codec::decodePacket(std::span{data, len});
            }
        } catch (...) {}
    }

    inline void CloverNT_KERNEL_CALL credentialSink(const std::uint8_t* const data,
                                                    const std::size_t         len,
                                                    void* const               user) {
        try {
            if (user != nullptr) {
                *static_cast<std::optional<Credential>*>(user) = Codec::decodeCredential(std::span{data, len});
            }
        } catch (...) {}
    }

    [[nodiscard]] constexpr auto clampTimeoutMs(const std::chrono::milliseconds timeout) noexcept -> std::uint32_t {
        const auto count = timeout.count();
        if (count <= 0) {
            return 0u;
        }
        if (count >= static_cast<std::chrono::milliseconds::rep>(UINT32_MAX)) {
            return UINT32_MAX;
        }
        return static_cast<std::uint32_t>(count);
    }

} // namespace Detail

[[nodiscard]] inline auto ready() -> bool {
    return CloverPacketReady() != 0;
}

[[nodiscard]] inline auto guardAvailable() noexcept -> bool {
    return Detail::getLockedFn() != nullptr;
}

[[nodiscard]] inline auto lockedFamilies() -> std::uint32_t {
    const auto fn = Detail::getLockedFn();
    return fn != nullptr ? fn() : 0u;
}

[[nodiscard]] inline auto packetLocked() -> bool {
    return (lockedFamilies() & CLOVER_FAMILY_PACKET) != 0;
}

[[nodiscard]] inline auto credentialLocked() -> bool {
    return (lockedFamilies() & CLOVER_FAMILY_CREDENTIAL) != 0;
}

[[nodiscard]] inline auto patchGuardState() -> std::uint32_t {
    return CLOVER_PATCHGUARD_STATE(lockedFamilies());
}

[[nodiscard]] inline auto patchGuardActive() -> bool {
    return (patchGuardState() & CLOVER_PATCHGUARD_ACTIVE) != 0;
}

[[nodiscard]] inline auto patchGuardEnabledNext() -> bool {
    return (patchGuardState() & CLOVER_PATCHGUARD_ENABLED) != 0;
}

[[nodiscard]] inline auto patchGuardRestartPending() -> bool {
    const auto state = patchGuardState();
    return ((state & CLOVER_PATCHGUARD_ACTIVE) != 0) != ((state & CLOVER_PATCHGUARD_ENABLED) != 0);
}

[[nodiscard]] inline auto credential() -> std::optional<Credential> {
    std::optional<Credential> out;
    (void) CloverPacketCredential(&Detail::credentialSink, &out);
    return out;
}

[[nodiscard]] inline auto send(const std::string_view command, const std::span<const std::uint8_t> body)
        -> std::optional<std::uint32_t> {
    std::uint32_t seq = 0;
    if (CloverPacketSend(command.data(), command.size(), body.data(), body.size(), &seq) == 0) {
        return std::nullopt;
    }
    return seq;
}

[[nodiscard]] inline auto receive(const std::string_view          command,
                                  const std::chrono::milliseconds timeout = kDefaultTimeout) -> std::optional<Packet> {
    std::optional<Packet> out;
    (void) CloverPacketReceiveByCommand(
            command.data(), command.size(), Detail::clampTimeoutMs(timeout), &Detail::packetSink, &out);
    return out;
}

[[nodiscard]] inline auto receive(const std::uint32_t seq, const std::chrono::milliseconds timeout = kDefaultTimeout)
        -> std::optional<Packet> {
    std::optional<Packet> out;
    (void) CloverPacketReceiveBySequence(seq, Detail::clampTimeoutMs(timeout), &Detail::packetSink, &out);
    return out;
}

[[nodiscard]] inline auto call(const std::string_view              command,
                               const std::span<const std::uint8_t> body,
                               const std::chrono::milliseconds     timeout = kDefaultTimeout) -> std::optional<Packet> {
    if (const auto fn = Detail::packetCallFn(); fn != nullptr) {
        std::optional<Packet> out;
        (void) fn(command.data(),
                  command.size(),
                  body.data(),
                  body.size(),
                  Detail::clampTimeoutMs(timeout),
                  &Detail::packetSink,
                  &out);
        return out;
    }
    std::uint32_t seq = 0;
    if (CloverPacketSend(command.data(), command.size(), body.data(), body.size(), &seq) == 0) {
        return std::nullopt;
    }
    return seq != 0 ? receive(seq, timeout) : receive(command, timeout);
}

} // namespace CloverNT::Nt
