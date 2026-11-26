#pragma once
#include <CloverNT/API/Events/BuiltInEvents.hpp>
#include <CloverNT/API/Logger.hpp>
#include <CloverNT/API/Nt/Packet.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/JsRuntime.hpp>

#include <Alka/Binding/Bind.hpp>
#include <Alka/Binding/Params.hpp>
#include <Alka/Convert/Buffers.hpp>
#include <Alka/Convert/Containers.hpp>
#include <Alka/Convert/Converter.hpp>
#include <Alka/Convert/Primitives.hpp>
#include <Alka/Convert/Records.hpp>
#include <Alka/Js/Function.hpp>
#include <Alka/Js/Promise.hpp>
#include <Alka/Js/Value.hpp>
#include <Alka/Runtime/Trackable.hpp>
#include <Alka/Ts/TsTypes.hpp>

#include <QQNT/v8-cppgc.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace CloverNT::Event {
class EventInvocation;
} // namespace CloverNT::Event

namespace CloverNT::Core::Modules::bridge {

namespace Event = CloverNT::Event;
using CloverNT::LogLevel;

struct JsEventCallback : Alka::Value {
    using Value::Value;
};

class JsEventControl : public Alka::Trackable {
public:
    JsEventControl(std::string             topic,
                   std::string             sourcePlugin,
                   std::string             delivery,
                   const bool              canCancel,
                   Event::EventInvocation* invocation) noexcept
        : mTopic(std::move(topic)),
          mSourcePlugin(std::move(sourcePlugin)),
          mDelivery(std::move(delivery)),
          mCanCancel(canCancel),
          mInvocation(invocation) {}

    [[nodiscard]] auto topic() const noexcept -> const std::string& {
        return mTopic;
    }

    [[nodiscard]] auto sourcePlugin() const noexcept -> const std::string& {
        return mSourcePlugin;
    }

    [[nodiscard]] auto delivery() const noexcept -> const std::string& {
        return mDelivery;
    }

    [[nodiscard]] bool canCancel() const noexcept {
        return mCanCancel;
    }

    [[nodiscard]] bool cancelled() const noexcept;
    bool               cancel() noexcept;
    void               detach() noexcept;

private:
    std::string             mTopic;
    std::string             mSourcePlugin;
    std::string             mDelivery;
    bool                    mCanCancel{false};
    bool                    mCancelledSnapshot{false};
    Event::EventInvocation* mInvocation{nullptr};
};

class JsLogger : public Alka::Trackable {
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
    bool                       unloadable{};
    bool                       builtin{};
};

struct PluginActionResultDto {
    bool                       ok{};
    std::optional<std::string> error;
    double                     count{};
};

struct GuardSetResultDto {
    std::int32_t                                status{};
    std::variant<std::uint32_t, std::nullptr_t> lockedMask{nullptr};
};

class PluginManagerApi : public Alka::Trackable {
public:
    explicit PluginManagerApi(JsRuntime::State* state) noexcept : mState(state) {}

    [[nodiscard]] auto list() const -> std::vector<PluginListDto>;
    [[nodiscard]] auto scan() const -> std::vector<PluginScanDto>;
    [[nodiscard]] bool load(const std::string& name) const;
    [[nodiscard]] bool unload(const std::string& name) const;
    [[nodiscard]] bool reload(const std::string& name) const;
    [[nodiscard]] auto loadWithResult(const std::string& name) const -> PluginActionResultDto;
    [[nodiscard]] auto unloadWithResult(const std::string& name) const -> PluginActionResultDto;
    [[nodiscard]] auto reloadWithResult(const std::string& name) const -> PluginActionResultDto;
    [[nodiscard]] auto rescanWithResult() const -> PluginActionResultDto;
    [[nodiscard]] bool registerLoader(std::string id, const Alka::Object& config) const;
    [[nodiscard]] bool unregisterLoader(std::string id) const;

private:
    JsRuntime::State* mState;
};

class EventsApi : public Alka::Trackable {
public:
    EventsApi(JsRuntime::State* state, std::string ownerName) noexcept
        : mState(state), mOwnerName(std::move(ownerName)) {}

    [[nodiscard]] auto on(std::string          name,
                          JsEventCallback      callback,
                          Event::EventPriority priority = Event::EventPriority::Normal) -> double;
    [[nodiscard]] auto once(std::string          name,
                            JsEventCallback      callback,
                            Event::EventPriority priority = Event::EventPriority::Normal) -> double;
    [[nodiscard]] auto monitor(std::string name, JsEventCallback callback) -> double;
    [[nodiscard]] auto off(double id) -> bool;
    [[nodiscard]] auto emit(const Alka::CallInfo& call, std::string name, Alka::Rest<Alka::Value> args) -> bool;

private:
    [[nodiscard]] auto
    subscribe(const std::string& name, JsEventCallback callback, bool once, Event::EventPriority priority) -> double;

    JsRuntime::State* mState;
    std::string       mOwnerName;
};

class PacketApi : public Alka::Trackable {
public:
    explicit PacketApi(JsRuntime::State* state) noexcept : mState(state) {}

    [[nodiscard]] bool ready() const;
    [[nodiscard]] bool guardAvailable() const;
    [[nodiscard]] auto lockedFamilies() const -> std::uint32_t;
    [[nodiscard]] bool packetLocked() const;
    [[nodiscard]] bool credentialLocked() const;
    [[nodiscard]] auto patchGuardState() const -> std::uint32_t;
    [[nodiscard]] bool patchGuardActive() const;
    [[nodiscard]] bool patchGuardEnabledNext() const;
    [[nodiscard]] bool patchGuardRestartPending() const;
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
struct Alka::Converter<CloverNT::Core::Modules::bridge::JsEventCallback> {
    static auto fromJs(ConvertCtx& cx, const v8::Local<v8::Value> value)
            -> Result<CloverNT::Core::Modules::bridge::JsEventCallback> {
        if (value.IsEmpty() || !value->IsFunction()) {
            return std::unexpected(ConvertError::typeMismatch("function", describeJsValue(value)));
        }
        return CloverNT::Core::Modules::bridge::JsEventCallback(cx, value);
    }

    static auto toJs(ConvertCtx& cx, const CloverNT::Core::Modules::bridge::JsEventCallback& value)
            -> v8::Local<v8::Value> {
        return value.valid() && value.belongsTo(cx.mIsolate) ? value.local(cx.mIsolate)
                                                             : v8::Undefined(cx.mIsolate).As<v8::Value>();
    }
};

template <>
struct Alka::TsTypeName<CloverNT::Core::Modules::bridge::JsEventCallback> {
    static auto get(const TsNameRegistry&) -> TsType {
        return {.mText = "(...args: any[]) => unknown"};
    }
};

template <>
struct Alka::Bind<CloverNT::Core::Modules::bridge::CloverntModule> : Module<"clovernt"> {
    static constexpr auto doc     = "CloverNT plugin runtime API.";
    static constexpr auto members = Alka::members(Alka::enumType<CloverNT::LogLevel>(),
                                                  Alka::enumType<CloverNT::Nt::Encryption>(),
                                                  Alka::enumType<CloverNT::Event::EventPriority>(),
                                                  Alka::type<CloverNT::Core::Modules::bridge::JsLogger>,
                                                  Alka::type<CloverNT::Core::Modules::bridge::PluginManagerApi>,
                                                  Alka::type<CloverNT::Core::Modules::bridge::EventsApi>,
                                                  Alka::type<CloverNT::Core::Modules::bridge::PacketApi>,
                                                  Alka::type<CloverNT::Core::Modules::bridge::JsEventControl>,
                                                  Alka::type<CloverNT::Core::Modules::bridge::PluginListDto>,
                                                  Alka::type<CloverNT::Core::Modules::bridge::PluginScanDto>,
                                                  Alka::type<CloverNT::Core::Modules::bridge::PluginActionResultDto>,
                                                  Alka::type<CloverNT::Core::Modules::bridge::GuardSetResultDto>,
                                                  Alka::type<CloverNT::Nt::Packet>,
                                                  Alka::type<CloverNT::Nt::O3Packet>,
                                                  Alka::type<CloverNT::Nt::Credential>,
                                                  Alka::type<CloverNT::Event::DllLoadInfo>);
};

template <>
struct Alka::Bind<CloverNT::Core::Modules::bridge::JsEventControl>
    : Class<CloverNT::Core::Modules::bridge::JsEventControl, "EventControl"> {
    using C                        = CloverNT::Core::Modules::bridge::JsEventControl;
    static constexpr auto lifetime = Lifetime::Manual;
    static constexpr auto doc =
            "Scoped delivery metadata and cancellation handle passed as the trailing argument to event listeners.";
    static constexpr auto members =
            Alka::members(Alka::prop<"topic", &C::topic>(readonly),
                          Alka::prop<"sourcePlugin", &C::sourcePlugin>(readonly),
                          Alka::prop<"delivery", &C::delivery>(readonly, Alka::tsType<R"("sync" | "async")">),
                          Alka::prop<"canCancel", &C::canCancel>(readonly),
                          Alka::prop<"cancelled", &C::cancelled>(readonly),
                          Alka::method<&C::cancel>());
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
                                                  Alka::method<&P::loadWithResult>(arg("name")),
                                                  Alka::method<&P::unloadWithResult>(arg("name")),
                                                  Alka::method<&P::reloadWithResult>(arg("name")),
                                                  Alka::method<&P::rescanWithResult>(),
                                                  Alka::method<&P::registerLoader>(arg("id"), arg("config")),
                                                  Alka::method<&P::unregisterLoader>(arg("id")));
};

template <>
struct Alka::Bind<CloverNT::Core::Modules::bridge::EventsApi>
    : Class<CloverNT::Core::Modules::bridge::EventsApi, "Events"> {
    using E                       = CloverNT::Core::Modules::bridge::EventsApi;
    static constexpr auto doc     = "Unified C++<->JS event bus.";
    static constexpr auto members = Alka::members(
            Alka::method<&E::on>(
                    arg("name"), arg("callback"), arg("priority") = CloverNT::Event::EventPriority::Normal),
            Alka::method<&E::once>(
                    arg("name"), arg("callback"), arg("priority") = CloverNT::Event::EventPriority::Normal),
            Alka::method<&E::monitor>(arg("name"), arg("callback")),
            Alka::method<&E::off>(arg("id")),
            Alka::method<&E::emit>(arg("name"), arg("args")));
};

template <>
struct Alka::Bind<CloverNT::Core::Modules::bridge::PacketApi>
    : Class<CloverNT::Core::Modules::bridge::PacketApi, "PacketApi"> {
    using P                       = CloverNT::Core::Modules::bridge::PacketApi;
    static constexpr auto doc     = "QQNT's network packet access.";
    static constexpr auto members = Alka::members(Alka::method<&P::ready>(),
                                                  Alka::method<&P::guardAvailable>(),
                                                  Alka::method<&P::lockedFamilies>(),
                                                  Alka::method<&P::packetLocked>(),
                                                  Alka::method<&P::credentialLocked>(),
                                                  Alka::method<&P::patchGuardState>(),
                                                  Alka::method<&P::patchGuardActive>(),
                                                  Alka::method<&P::patchGuardEnabledNext>(),
                                                  Alka::method<&P::patchGuardRestartPending>(),
                                                  Alka::method<&P::credential>(),
                                                  Alka::method<&P::send>(arg("command"), arg("body")),
                                                  Alka::method<&P::call>(arg("command"), arg("body"), arg("timeoutMs")),
                                                  Alka::method<&P::receive>(arg("command"), arg("timeoutMs")),
                                                  Alka::method<&P::receiveSeq>(arg("seq"), arg("timeoutMs")));
};

template <>
struct Alka::Bind<CloverNT::Core::Modules::bridge::PluginListDto>
    : Record<CloverNT::Core::Modules::bridge::PluginListDto, "PluginListing"> {
    using T                       = CloverNT::Core::Modules::bridge::PluginListDto;
    static constexpr auto doc     = "A loaded plugin's entry points.";
    static constexpr auto members = Alka::members(Alka::prop<&T::name>(readonly),
                                                  Alka::prop<&T::main>(readonly),
                                                  Alka::prop<&T::preload>(readonly),
                                                  Alka::prop<&T::renderer>(readonly));
};

template <>
struct Alka::Bind<CloverNT::Core::Modules::bridge::PluginScanDto>
    : Record<CloverNT::Core::Modules::bridge::PluginScanDto, "PluginScan"> {
    using T                       = CloverNT::Core::Modules::bridge::PluginScanDto;
    static constexpr auto doc     = "A discovered plugin (whether or not loaded).";
    static constexpr auto members = Alka::members(Alka::prop<&T::name>(readonly),
                                                  Alka::prop<&T::type>(readonly),
                                                  Alka::prop<&T::version>(readonly),
                                                  Alka::prop<&T::author>(readonly),
                                                  Alka::prop<&T::description>(readonly),
                                                  Alka::prop<&T::passive>(readonly),
                                                  Alka::prop<&T::loaded>(readonly),
                                                  Alka::prop<&T::unloadable>(readonly),
                                                  Alka::prop<&T::builtin>(readonly));
};

template <>
struct Alka::Bind<CloverNT::Core::Modules::bridge::PluginActionResultDto>
    : Record<CloverNT::Core::Modules::bridge::PluginActionResultDto, "PluginActionResult"> {
    using T                       = CloverNT::Core::Modules::bridge::PluginActionResultDto;
    static constexpr auto doc     = "Result of a plugin lifecycle or rescan operation.";
    static constexpr auto members = Alka::members(Alka::prop<&T::ok>(readonly),
                                                  Alka::prop<&T::error>(readonly),
                                                  Alka::prop<&T::count>(readonly));
};

template <>
struct Alka::Bind<CloverNT::Core::Modules::bridge::GuardSetResultDto>
    : Record<CloverNT::Core::Modules::bridge::GuardSetResultDto, "GuardSetResult"> {
    using T                   = CloverNT::Core::Modules::bridge::GuardSetResultDto;
    static constexpr auto doc = "Result of toggling an ApiGuard / PatchGuard family mask.";
    static constexpr auto members =
            Alka::members(Alka::prop<&T::status>(readonly), Alka::prop<&T::lockedMask>(readonly));
};

template <>
struct Alka::Converter<std::array<std::uint8_t, 16>> {
    static auto toJs(ConvertCtx& cx, const std::array<std::uint8_t, 16>& value) -> v8::Local<v8::Value> {
        return Converter<std::span<const std::uint8_t>>::toJs(cx, std::span<const std::uint8_t>{value});
    }

    static auto fromJs(ConvertCtx& cx, const v8::Local<v8::Value> value) -> Result<std::array<std::uint8_t, 16>> {
        auto bytes = Converter<std::vector<std::uint8_t>>::fromJs(cx, value);
        if (!bytes) {
            return std::unexpected(std::move(bytes).error());
        }
        if (bytes->size() != 16) {
            return std::unexpected(ConvertError{"expected a 16-byte Uint8Array"});
        }
        std::array<std::uint8_t, 16> out{};
        std::copy_n(bytes->begin(), out.size(), out.begin());
        return out;
    }
};

template <>
struct Alka::TsTypeName<std::array<std::uint8_t, 16>> {
    static auto get(const TsNameRegistry&) -> TsType {
        return {"Uint8Array"};
    }
};

template <>
struct Alka::Bind<CloverNT::Nt::Packet> : Record<CloverNT::Nt::Packet, "Packet"> {
    using T                       = CloverNT::Nt::Packet;
    static constexpr auto doc     = "A decoded SSO packet.";
    static constexpr auto members = Alka::members(Alka::prop<&T::seq>(),
                                                  Alka::prop<&T::command>(),
                                                  Alka::prop<&T::uin>(),
                                                  Alka::prop<&T::uid>(),
                                                  Alka::prop<&T::encryption>(),
                                                  Alka::prop<&T::body>(),
                                                  Alka::prop<&T::injected>());
};

template <>
struct Alka::Bind<CloverNT::Nt::O3Packet> : Record<CloverNT::Nt::O3Packet, "O3Packet"> {
    using T                       = CloverNT::Nt::O3Packet;
    static constexpr auto doc     = "A plaintext o3 secure-channel packet.";
    static constexpr auto members = Alka::members(Alka::prop<&T::command>(), Alka::prop<&T::body>());
};

template <>
struct Alka::Bind<CloverNT::Nt::Credential> : Record<CloverNT::Nt::Credential, "Credential"> {
    using T                       = CloverNT::Nt::Credential;
    static constexpr auto doc     = "Captured session credentials.";
    static constexpr auto members = Alka::members(Alka::prop<&T::uin>(readonly),
                                                  Alka::prop<&T::uid>(readonly),
                                                  Alka::prop<&T::a2>(readonly),
                                                  Alka::prop<&T::d2>(readonly),
                                                  Alka::prop<&T::d2Key>(readonly));
};

template <>
struct Alka::Bind<CloverNT::Event::DllLoadInfo> : Record<CloverNT::Event::DllLoadInfo, "DllLoadInfo"> {
    using T                   = CloverNT::Event::DllLoadInfo;
    static constexpr auto doc = "Metadata of a native module loaded into the host process.";
    static constexpr auto members =
            Alka::members(Alka::prop<&T::baseName>(readonly),
                          Alka::prop<&T::fullPath>(readonly),
                          Alka::prop<&T::baseAddress>(readonly, Alka::tsType<"number | bigint">),
                          Alka::prop<&T::imageSize>(readonly, Alka::tsType<"number | bigint">));
};
