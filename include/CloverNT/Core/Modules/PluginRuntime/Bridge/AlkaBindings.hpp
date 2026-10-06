#pragma once
#include <CloverNT/API/Logger.hpp>
#include <CloverNT/API/Nt/Packet.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/JsRuntime.hpp>

#include <Alka/Bind.hpp>
#include <Alka/Converter.hpp>
#include <Alka/Converters/Buffers.hpp>
#include <Alka/Function.hpp>
#include <Alka/Ts/TsTypes.hpp>

#include <QQNT/v8-cppgc.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace CloverNT::Core::Modules::bridge {

using CloverNT::LogLevel;

class JsLogger {
public:
    explicit JsLogger(std::shared_ptr<Logger> logger) noexcept : mLogger(std::move(logger)) {}

    void debug(const std::string& message) const;
    void info(const std::string& message) const;
    void warn(const std::string& message) const;
    void error(const std::string& message) const;
    void fatal(const std::string& message) const;
    void log(LogLevel level, std::string message) const;

    void setTitle(const std::string& title) const;
    void setMinLevel(LogLevel level) const;
    void setOutputConsole(bool enabled) const;
    void setOutputColor(bool enabled) const;
    void setLogFile(const std::string& filePath) const;

    [[nodiscard]] auto title() const -> std::string;
    [[nodiscard]] auto minLevel() const -> LogLevel;
    [[nodiscard]] bool shouldLog(LogLevel level) const;
    void               flush() const;

private:
    std::shared_ptr<Logger> mLogger;
};

struct PluginListDto {
    std::string                name;
    std::optional<std::string> main;
    std::optional<std::string> preload;
    std::optional<std::string> renderer;
};
struct PluginScanDto {
    std::string                name;
    std::string                type;
    std::optional<std::string> version;
    std::optional<std::string> author;
    std::optional<std::string> description;
    bool                       passive{};
    bool                       loaded{};
};

class PluginManagerApi {
public:
    explicit PluginManagerApi(JsRuntime::State* state) noexcept : mState(state) {}

    [[nodiscard]] auto list() const -> std::vector<PluginListDto>;
    [[nodiscard]] auto scan() const -> std::vector<PluginScanDto>;
    bool               load(const std::string& name) const;
    bool               unload(const std::string& name) const;
    bool               reload(const std::string& name) const;
    bool               registerLoader(std::string id, const Alka::Object& config) const;
    bool               unregisterLoader(std::string id) const;

private:
    JsRuntime::State* mState;
};

class EventsApi {
public:
    EventsApi(JsRuntime::State* state, std::string ownerName) noexcept
        : mState(state), mOwnerName(std::move(ownerName)) {}

    [[nodiscard]] auto on(std::string name, Alka::Function<void(Alka::Value)> callback) -> double;
    [[nodiscard]] auto once(std::string name, Alka::Function<void(Alka::Value)> callback) -> double;
    [[nodiscard]] auto off(double id) -> bool;
    [[nodiscard]] auto emit(std::string name, const Alka::Value& payload) -> bool;

private:
    [[nodiscard]] auto subscribe(std::string name, Alka::Function<void(Alka::Value)> callback, bool once) -> double;

    JsRuntime::State* mState;
    std::string       mOwnerName;
};

/// Exposes the SSO Packet / o3 API to JS as `clovernt.packet`. Byte payloads
/// cross as `Uint8Array`; results are the bound `Packet` / `Credential` types
/// (or `undefined` on miss/timeout).
class PacketApi {
public:
    explicit PacketApi(JsRuntime::State* state) noexcept : mState(state) {}

    [[nodiscard]] bool ready() const;
    [[nodiscard]] auto credential() const -> std::optional<Nt::Credential>;
    [[nodiscard]] auto send(std::string command, std::vector<std::uint8_t> body) const -> std::optional<std::uint32_t>;
    [[nodiscard]] auto call(std::string command, std::vector<std::uint8_t> body, double timeoutMs) const
            -> std::optional<Nt::Packet>;
    [[nodiscard]] auto receive(std::string command, double timeoutMs) const -> std::optional<Nt::Packet>;
    [[nodiscard]] auto receiveSeq(double seq, double timeoutMs) const -> std::optional<Nt::Packet>;

private:
    [[maybe_unused]] JsRuntime::State* mState;
};

void ensureAlkaClasses(v8::Isolate* isolate);

[[nodiscard]] auto mintPluginManager(v8::Local<v8::Context> context, JsRuntime::State& state) -> v8::Local<v8::Value>;
[[nodiscard]] auto mintLogger(v8::Local<v8::Context> context, JsRuntime::State& state, const std::string& scope)
        -> v8::Local<v8::Value>;
[[nodiscard]] auto mintEvents(v8::Local<v8::Context> context, JsRuntime::State& state, const std::string& ownerName)
        -> v8::Local<v8::Value>;
[[nodiscard]] auto mintPacket(v8::Local<v8::Context> context, JsRuntime::State& state) -> v8::Local<v8::Value>;

void clearBindings(JsRuntime::State& state);

struct CloverntModule {};

} // namespace CloverNT::Core::Modules::bridge

template <>
struct Alka::Bind<CloverNT::Core::Modules::bridge::CloverntModule> : Module<"clovernt"> {
    static constexpr auto doc     = "CloverNT plugin runtime API (auto-generated from the native bindings).";
    static constexpr auto members = Alka::members(Alka::enumType<CloverNT::LogLevel>(),
                                                  Alka::enumType<CloverNT::Nt::Encryption>(),
                                                  Alka::type<CloverNT::Core::Modules::bridge::JsLogger>,
                                                  Alka::type<CloverNT::Core::Modules::bridge::PluginManagerApi>,
                                                  Alka::type<CloverNT::Core::Modules::bridge::EventsApi>,
                                                  Alka::type<CloverNT::Core::Modules::bridge::PacketApi>,
                                                  Alka::type<CloverNT::Core::Modules::bridge::PluginListDto>,
                                                  Alka::type<CloverNT::Core::Modules::bridge::PluginScanDto>,
                                                  Alka::type<CloverNT::Nt::Packet>,
                                                  Alka::type<CloverNT::Nt::Credential>);
};

template <>
struct Alka::Bind<CloverNT::Core::Modules::bridge::JsLogger>
    : Class<CloverNT::Core::Modules::bridge::JsLogger, "Logger"> {
    using L                       = CloverNT::Core::Modules::bridge::JsLogger;
    static constexpr auto doc     = "A scoped logger.";
    static constexpr auto members = Alka::members(Alka::method<&L::debug>(arg("message")),
                                                  Alka::method<&L::info>(arg("message")),
                                                  Alka::method<&L::warn>(arg("message")),
                                                  Alka::method<&L::error>(arg("message")),
                                                  Alka::method<&L::fatal>(arg("message")),
                                                  Alka::method<&L::log>(arg("level"), arg("message")),
                                                  Alka::method<&L::setTitle>(arg("title")),
                                                  Alka::method<&L::setMinLevel>(arg("level")),
                                                  Alka::method<&L::setOutputConsole>(arg("enabled")),
                                                  Alka::method<&L::setOutputColor>(arg("enabled")),
                                                  Alka::method<&L::setLogFile>(arg("filePath")),
                                                  Alka::method<&L::title>(),
                                                  Alka::method<&L::minLevel>(),
                                                  Alka::method<&L::shouldLog>(arg("level")),
                                                  Alka::method<&L::flush>());
};

template <>
struct Alka::Bind<CloverNT::Core::Modules::bridge::PluginManagerApi>
    : Class<CloverNT::Core::Modules::bridge::PluginManagerApi, "PluginManager"> {
    using P                       = CloverNT::Core::Modules::bridge::PluginManagerApi;
    static constexpr auto doc     = "Manages CloverNT plugins.";
    static constexpr auto members = Alka::members(Alka::method<&P::list>(),
                                                  Alka::method<&P::scan>(),
                                                  Alka::method<&P::load>(arg("name")),
                                                  Alka::method<&P::unload>(arg("name")),
                                                  Alka::method<&P::reload>(arg("name")),
                                                  Alka::method<&P::registerLoader>(arg("id"), arg("config")),
                                                  Alka::method<&P::unregisterLoader>(arg("id")));
};

template <>
struct Alka::Bind<CloverNT::Core::Modules::bridge::EventsApi>
    : Class<CloverNT::Core::Modules::bridge::EventsApi, "Events"> {
    using E                       = CloverNT::Core::Modules::bridge::EventsApi;
    static constexpr auto doc     = "Bidirectional C++<->JS event bus.";
    static constexpr auto members = Alka::members(Alka::method<&E::on>(arg("name"), arg("callback")),
                                                  Alka::method<&E::once>(arg("name"), arg("callback")),
                                                  Alka::method<&E::off>(arg("id")),
                                                  Alka::method<&E::emit>(arg("name"), arg("payload")));
};

template <>
struct Alka::Bind<CloverNT::Core::Modules::bridge::PacketApi>
    : Class<CloverNT::Core::Modules::bridge::PacketApi, "PacketApi"> {
    using P                       = CloverNT::Core::Modules::bridge::PacketApi;
    static constexpr auto doc     = "Low-level QQNT SSO packet / o3 channel access. Byte payloads are Uint8Array.";
    static constexpr auto members = Alka::members(Alka::method<&P::ready>(),
                                                  Alka::method<&P::credential>(),
                                                  Alka::method<&P::send>(arg("command"), arg("body")),
                                                  Alka::method<&P::call>(arg("command"), arg("body"), arg("timeoutMs")),
                                                  Alka::method<&P::receive>(arg("command"), arg("timeoutMs")),
                                                  Alka::method<&P::receiveSeq>(arg("seq"), arg("timeoutMs")));
};

template <>
struct Alka::Bind<CloverNT::Core::Modules::bridge::PluginListDto>
    : Class<CloverNT::Core::Modules::bridge::PluginListDto, "PluginListing"> {
    using T                       = CloverNT::Core::Modules::bridge::PluginListDto;
    static constexpr auto doc     = "A loaded plugin's entry points.";
    static constexpr auto members = Alka::members(Alka::prop<&T::name>(readonly),
                                                  Alka::prop<&T::main>(readonly),
                                                  Alka::prop<&T::preload>(readonly),
                                                  Alka::prop<&T::renderer>(readonly));
};

template <>
struct Alka::Bind<CloverNT::Core::Modules::bridge::PluginScanDto>
    : Class<CloverNT::Core::Modules::bridge::PluginScanDto, "PluginScan"> {
    using T                       = CloverNT::Core::Modules::bridge::PluginScanDto;
    static constexpr auto doc     = "A discovered plugin (whether or not loaded).";
    static constexpr auto members = Alka::members(Alka::prop<&T::name>(readonly),
                                                  Alka::prop<&T::type>(readonly),
                                                  Alka::prop<&T::version>(readonly),
                                                  Alka::prop<&T::author>(readonly),
                                                  Alka::prop<&T::description>(readonly),
                                                  Alka::prop<&T::passive>(readonly),
                                                  Alka::prop<&T::loaded>(readonly));
};

// A byte array (e.g. the 16-byte d2 key) crosses as a Uint8Array.
template <>
struct Alka::Converter<std::array<std::uint8_t, 16>> {
    static auto toJs(ConvertCtx& cx, const std::array<std::uint8_t, 16>& value) -> v8::Local<v8::Value> {
        return Alka::Converter<std::span<const std::uint8_t>>::toJs(cx, std::span<const std::uint8_t>{value});
    }
    static auto fromJs(ConvertCtx& cx, v8::Local<v8::Value> value) -> Result<std::array<std::uint8_t, 16>> {
        auto bytes = Alka::Converter<std::vector<std::uint8_t>>::fromJs(cx, value);
        if (!bytes) {
            return std::unexpected(std::move(bytes).error());
        }
        std::array<std::uint8_t, 16> out{};
        std::copy_n(bytes->begin(), std::min<std::size_t>(bytes->size(), out.size()), out.begin());
        return out;
    }
};
template <>
struct Alka::TsTypeName<std::array<std::uint8_t, 16>> {
    static auto get(const TsNameRegistry&) -> TsType {
        return {"Uint8Array"};
    }
};

// The public SSO value types, bound declaratively so JS gets named `Packet` /
// `Credential` objects (byte fields as Uint8Array) with auto-generated .d.ts.
template <>
struct Alka::Bind<CloverNT::Nt::Packet> : Class<CloverNT::Nt::Packet, "Packet"> {
    using T                       = CloverNT::Nt::Packet;
    static constexpr auto doc     = "A decoded SSO packet (body: decrypted payload for recv / plaintext for send).";
    static constexpr auto members = Alka::members(Alka::prop<&T::seq>(readonly),
                                                  Alka::prop<&T::command>(readonly),
                                                  Alka::prop<&T::uin>(readonly),
                                                  Alka::prop<&T::encryption>(readonly),
                                                  Alka::prop<&T::body>(readonly));
};

template <>
struct Alka::Bind<CloverNT::Nt::Credential> : Class<CloverNT::Nt::Credential, "Credential"> {
    using T                       = CloverNT::Nt::Credential;
    static constexpr auto doc     = "Captured session credentials.";
    static constexpr auto members = Alka::members(Alka::prop<&T::uin>(readonly),
                                                  Alka::prop<&T::a2>(readonly),
                                                  Alka::prop<&T::d2>(readonly),
                                                  Alka::prop<&T::d2Key>(readonly));
};
