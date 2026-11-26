#include <CloverNT/API/Events/EventBus.hpp>
#include <CloverNT/API/Nt/Packet.hpp>
#include <CloverNT/API/Plugin/JsPlugin.hpp>
#include <CloverNT/API/Plugin/NativePlugin.hpp>
#include <CloverNT/API/Plugin/PluginManager.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/AlkaBindings.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/Binding.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/Events.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/ReflectBridge.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/JsRuntimeState.hpp>

#include <Alka/Alka.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace CloverNT::Core::Modules::bridge {

void JsLogger::debug(const std::string& message) const {
    if (mLogger) {
        mLogger->debug(message);
    }
}
void JsLogger::info(const std::string& message) const {
    if (mLogger) {
        mLogger->info(message);
    }
}
void JsLogger::warn(const std::string& message) const {
    if (mLogger) {
        mLogger->warn(message);
    }
}
void JsLogger::error(const std::string& message) const {
    if (mLogger) {
        mLogger->error(message);
    }
}
void JsLogger::fatal(const std::string& message) const {
    if (mLogger) {
        mLogger->fatal(message);
    }
}
void JsLogger::log(const LogLevel level, std::string message) const {
    if (mLogger) {
        mLogger->logString(level, std::move(message));
    }
}
void JsLogger::setTitle(const std::string& title) const {
    if (mLogger) {
        mLogger->setTitle(title);
    }
}
void JsLogger::setMinLevel(const LogLevel level) const {
    if (mLogger) {
        mLogger->setMinLevel(level);
    }
}
void JsLogger::setOutputConsole(const bool enabled) const {
    if (mLogger) {
        mLogger->setOutputConsole(enabled);
    }
}
void JsLogger::setOutputColor(const bool enabled) const {
    if (mLogger) {
        mLogger->setOutputColor(enabled);
    }
}
void JsLogger::setLogFile(const std::string& filePath) const {
    if (!mLogger) {
        return;
    }
    if (auto result = mLogger->setLogFile(filePath); !result) {
        throw std::runtime_error(result.error().message);
    }
}
auto JsLogger::title() const -> std::string {
    return mLogger ? mLogger->title() : std::string{};
}
auto JsLogger::minLevel() const -> LogLevel {
    return mLogger ? mLogger->minLevel() : LogLevel::Info;
}
bool JsLogger::shouldLog(const LogLevel level) const {
    return mLogger && mLogger->shouldLog(level);
}
void JsLogger::flush() const {
    if (mLogger) {
        mLogger->flush();
    }
}

auto PluginManagerApi::list() const -> std::vector<PluginListDto> {
    std::vector<PluginListDto> out;
    if (mState == nullptr) {
        return out;
    }
    std::vector<std::shared_ptr<Plugin::JsPlugin>> snapshot;
    {
        std::lock_guard lock(mState->mMutex);
        snapshot = mState->mQueue;
    }
    for (const auto& plugin: snapshot) {
        if (!plugin) {
            continue;
        }
        PluginListDto dto;
        dto.name = std::string(plugin->name());
        if (plugin->mainEntry()) {
            dto.main = plugin->mainEntry()->string();
        }
        if (plugin->preloadEntry()) {
            dto.preload = plugin->preloadEntry()->string();
        }
        if (plugin->rendererEntry()) {
            dto.renderer = plugin->rendererEntry()->string();
        }
        out.push_back(std::move(dto));
    }
    return out;
}

auto PluginManagerApi::scan() const -> std::vector<PluginScanDto> {
    std::vector<PluginScanDto> out;
    for (const auto& [name, type, version, author, description, passive, loaded]: Plugin::PluginManager::scan()) {
        PluginScanDto dto;
        dto.name = name;
        dto.type = type;
        if (version) {
            dto.version = version->toString();
        }
        if (!author.empty()) {
            dto.author = author;
        }
        if (!description.empty()) {
            dto.description = description;
        }
        dto.passive = passive;
        dto.loaded  = loaded;
        if (loaded) {
            const auto colon = name.find(':');
            const auto bare =
                    colon == std::string::npos ? std::string_view{name} : std::string_view{name}.substr(colon + 1);
            if (const auto plugin = Plugin::PluginManager::getPlugin(bare)) {
                const auto native = std::dynamic_pointer_cast<Plugin::NativePlugin>(plugin);
                dto.unloadable    = native ? native->hasUnloadHandler() : plugin->isUnloadable();
                dto.builtin       = plugin->isBuiltin();
            }
        }
        out.push_back(std::move(dto));
    }
    return out;
}

bool PluginManagerApi::load(const std::string& name) const {
    return Plugin::PluginManager::loadPlugin(name).has_value();
}
bool PluginManagerApi::unload(const std::string& name) const {
    return Plugin::PluginManager::unloadPlugin(name).has_value();
}
bool PluginManagerApi::reload(const std::string& name) const {
    return Plugin::PluginManager::reloadPlugin(name).has_value();
}

auto PluginManagerApi::loadWithResult(const std::string& name) const -> PluginActionResultDto {
    if (const auto result = Plugin::PluginManager::loadPlugin(name); !result) {
        return {.ok = false, .error = result.error().message};
    }
    return {.ok = true};
}

auto PluginManagerApi::unloadWithResult(const std::string& name) const -> PluginActionResultDto {
    if (const auto result = Plugin::PluginManager::unloadPlugin(name); !result) {
        return {.ok = false, .error = result.error().message};
    }
    return {.ok = true};
}

auto PluginManagerApi::reloadWithResult(const std::string& name) const -> PluginActionResultDto {
    if (const auto result = Plugin::PluginManager::reloadPlugin(name); !result) {
        return {.ok = false, .error = result.error().message};
    }
    return {.ok = true};
}

auto PluginManagerApi::rescanWithResult() const -> PluginActionResultDto {
    const auto result = Plugin::PluginManager::rescan();
    if (!result) {
        return {.ok = false, .error = result.error().message};
    }
    return {.ok = true, .count = static_cast<double>(result.value())};
}

bool PluginManagerApi::registerLoader(std::string id, const Alka::Object& config) const {
    if (mState == nullptr || !config.valid()) {
        return false;
    }
    if (const auto result =
                Plugin::PluginManager::registerLoader(std::make_shared<JsPluginLoader>(mState->selfCell(), id));
        !result) {
        return false;
    }
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    mState->registerLoaderCallbacks(std::move(id), config.local(isolate).As<v8::Object>());
    return true;
}

bool PluginManagerApi::unregisterLoader(std::string id) const {
    if (mState == nullptr) {
        return false;
    }
    const auto result = Plugin::PluginManager::unregisterLoader(id);
    if (result) {
        mState->unregisterLoaderCallbacks(id);
    }
    return result.has_value();
}

auto EventsApi::on(std::string name, JsEventCallback callback, const Event::EventPriority priority) -> double {
    return subscribe(name, std::move(callback), false, priority);
}

auto EventsApi::once(std::string name, JsEventCallback callback, const Event::EventPriority priority) -> double {
    return subscribe(name, std::move(callback), true, priority);
}

auto EventsApi::monitor(std::string name, JsEventCallback callback) -> double {
    return subscribe(name, std::move(callback), false, Event::EventPriority::Monitor);
}

auto EventsApi::off(const double id) -> bool {
    if (mState == nullptr || !(id > 0.0)) {
        return false;
    }
    const auto subId   = static_cast<Event::SubscriptionId>(id);
    const bool removed = Event::EventBus::off(subId);
    mState->untrackJsSubscription(subId);
    return removed;
}

auto EventsApi::emit(const Alka::CallInfo& call, std::string name, Alka::Rest<Alka::Value> args) -> bool {
    if (mState == nullptr) {
        return false;
    }
    const auto topic = Event::EventBus::normalizeEmitTopic(name, mOwnerName, /*normalizeBare=*/true);
    if (!topic) {
        throw Alka::JsException(topic.error().message);
    }

    v8::Isolate* const    isolate = call.mIsolate != nullptr ? call.mIsolate : v8::Isolate::GetCurrent();
    const v8::HandleScope handleScope(isolate);
    const auto            context = !call.mContext.IsEmpty() ? call.mContext : isolate->GetCurrentContext();

    std::vector<v8::Local<v8::Value>> rawJsArgs;
    rawJsArgs.reserve(args.size());
    for (const auto& val: args) {
        rawJsArgs.push_back(val.valid() && val.belongsTo(isolate) ? val.local(isolate)
                                                                  : v8::Undefined(isolate).As<v8::Value>());
    }

    auto materialize = [&](const Reflect::MarshalContext& mctx) -> Expected<std::vector<Reflect::BridgeValue>> {
        std::vector<Reflect::BridgeValue> out;
        out.reserve(rawJsArgs.size());
        for (std::size_t i = 0; i < rawJsArgs.size(); ++i) {
            auto converted = bridgeValueFromV8(isolate, context, rawJsArgs[i], mctx.activeLease);
            if (!converted) {
                return unexpected(
                        Reflect::prependErrorPath(std::move(converted).error(), "arg[" + std::to_string(i) + "]"));
            }
            out.push_back(std::move(*converted));
        }
        return out;
    };

    auto writeBack = [&](const std::span<const Reflect::BridgeValue> updated,
                         Reflect::MarshalContext&) -> Expected<void> {
        const std::size_t limit = std::min(rawJsArgs.size(), updated.size());
        for (std::size_t i = 0; i < limit; ++i) {
            const auto orig = rawJsArgs[i];
            if (orig.IsEmpty() || !orig->IsObject() || orig->IsFunction()) {
                continue;
            }
            if (orig->IsArray() && updated[i].isArray()) {
                const auto  arr   = orig.As<v8::Array>();
                const auto& items = updated[i].asArray();
                (void) arr->Set(context,
                                v8Str(isolate, "length"),
                                v8::Integer::NewFromUnsigned(isolate, static_cast<std::uint32_t>(items.size())));
                for (std::size_t k = 0; k < items.size(); ++k) {
                    auto elemJs = bridgeValueToV8(isolate, context, items[k]);
                    if (!elemJs) {
                        return unexpected(elemJs.error());
                    }
                    (void) arr->Set(context, static_cast<std::uint32_t>(k), *elemJs);
                }
            } else if (!orig->IsArray() && !orig->IsArrayBufferView() && !orig->IsArrayBuffer() &&
                       updated[i].isObject()) {
                if (auto res = writeBridgeFieldsToV8Object(isolate, context, orig.As<v8::Object>(), updated[i]); !res) {
                    return res;
                }
            }
        }
        return {};
    };

    const v8::Local<v8::Value> dummyArg{};
    const void* const          fastPtr =
            rawJsArgs.empty() ? static_cast<const void*>(&dummyArg) : static_cast<const void*>(rawJsArgs.data());

    const Reflect::ScopedOwnerPlugin ownerGuard(mOwnerName);
    Event::EventInvocation           inv(*topic,
                                         mOwnerName,
                                         /*cancellable=*/true,
                                         /*mutablePayload=*/!rawJsArgs.empty(),
                                         /*nativeArgs=*/{},
                                         std::move(materialize),
                                         std::move(writeBack),
                                         isolate,
                                         fastPtr,
                                         rawJsArgs.size());

    const auto dispatched = Event::EventBus::dispatchPluginInvocation(inv);
    if (!dispatched) {
        throw Alka::JsException(dispatched.error().message);
    }
    return *dispatched;
}

auto EventsApi::subscribe(const std::string&         name,
                          JsEventCallback            callback,
                          const bool                 once,
                          const Event::EventPriority priority) -> double {
    if (mState == nullptr || !callback.valid()) {
        return -1.0;
    }
    const auto topic = Event::EventBus::normalizeSubscribeTopic(name, mOwnerName, /*normalizeBare=*/true);
    if (!topic) {
        throw Alka::JsException(topic.error().message);
    }

    const auto subId    = Event::EventBus::allocateSubscriptionId();
    auto       activity = Reflect::acquirePluginActivity(mOwnerName);
    auto       slot     = std::make_shared<JsListenerSlot>(subId,
                                                           priority,
                                                           once,
                                                           mOwnerName,
                                                           std::move(activity),
                                                           mState->selfCell(),
                                                           std::move(callback),
                                                           mState->mAlkaDispatcher);

    const auto added = Event::EventBus::addSlot(*topic, std::move(slot));
    if (!added) {
        throw Alka::JsException(added.error().message);
    }
    mState->trackJsSubscription(*added);
    return static_cast<double>(*added);
}

namespace {

    auto clampTimeout(const double timeoutMs) -> std::chrono::milliseconds {
        if (!(timeoutMs > 0.0)) {
            return Nt::kDefaultTimeout;
        }
        return std::chrono::milliseconds{static_cast<std::int64_t>(timeoutMs)};
    }

} // namespace

bool PacketApi::ready() const {
    return Nt::ready();
}

bool PacketApi::guardAvailable() const {
    return Nt::guardAvailable();
}

auto PacketApi::lockedFamilies() const -> std::uint32_t {
    return Nt::lockedFamilies();
}

bool PacketApi::packetLocked() const {
    return Nt::packetLocked();
}

bool PacketApi::credentialLocked() const {
    return Nt::credentialLocked();
}

auto PacketApi::patchGuardState() const -> std::uint32_t {
    return Nt::patchGuardState();
}

bool PacketApi::patchGuardActive() const {
    return Nt::patchGuardActive();
}

bool PacketApi::patchGuardEnabledNext() const {
    return Nt::patchGuardEnabledNext();
}

bool PacketApi::patchGuardRestartPending() const {
    return Nt::patchGuardRestartPending();
}

auto PacketApi::credential() const -> std::optional<Nt::Credential> {
    return Nt::credential();
}

auto PacketApi::send(std::string command, std::vector<std::uint8_t> body) const -> std::optional<std::uint32_t> {
    return Nt::send(command, body);
}

auto PacketApi::call(std::string command, std::vector<std::uint8_t> body, const double timeoutMs) const
        -> std::optional<Nt::Packet> {
    return Nt::call(command, body, clampTimeout(timeoutMs));
}

auto PacketApi::receive(std::string command, const double timeoutMs) const -> std::optional<Nt::Packet> {
    return Nt::receive(command, clampTimeout(timeoutMs));
}

auto PacketApi::receiveSeq(const double seq, const double timeoutMs) const -> std::optional<Nt::Packet> {
    return Nt::receive(static_cast<std::uint32_t>(seq), clampTimeout(timeoutMs));
}

namespace {

    struct StateBindings {
        std::unique_ptr<PluginManagerApi>                           pluginManager;
        std::unique_ptr<PacketApi>                                  packet;
        std::unordered_map<std::string, std::unique_ptr<JsLogger>>  loggers;
        std::unordered_map<std::string, std::unique_ptr<EventsApi>> events; // keyed by owner name
    };

    std::mutex gMutex;

    auto registry() -> std::unordered_map<JsRuntime::State*, StateBindings>& {
        static std::unordered_map<JsRuntime::State*, StateBindings> instances;
        return instances;
    }

    template <class Api>
    auto toJsValue(const v8::Local<v8::Context> context, Api* api) -> v8::Local<v8::Value> {
        Alka::ConvertCtx cx{v8::Isolate::GetCurrent(), context};
        return Alka::toJs(cx, api);
    }

    template <class Api, class Factory>
    auto mintSingleton(std::unique_ptr<Api> StateBindings::* slot,
                       const v8::Local<v8::Context>          context,
                       JsRuntime::State&                     state,
                       Factory&&                             make) -> v8::Local<v8::Value> {
        Api* api = nullptr;
        {
            const std::lock_guard lock(gMutex);
            auto&                 slotRef = registry()[&state].*slot;
            if (!slotRef) {
                slotRef = make();
            }
            api = slotRef.get();
        }
        return toJsValue(context, api);
    }

    template <class Api, class Factory>
    auto mintKeyed(std::unordered_map<std::string, std::unique_ptr<Api>> StateBindings::* map,
                   const v8::Local<v8::Context>                                           context,
                   JsRuntime::State&                                                      state,
                   const std::string&                                                     key,
                   Factory&& make) -> v8::Local<v8::Value> {
        Api* api = nullptr;
        {
            const std::lock_guard lock(gMutex);
            auto&                 slot = registry()[&state].*map;
            auto                  it   = slot.find(key);
            if (it == slot.end()) {
                it = slot.emplace(key, make()).first;
            }
            api = it->second.get();
        }
        return toJsValue(context, api);
    }

} // namespace

void ensureAlkaClasses(v8::Isolate* isolate) {
    const v8::HandleScope scope(isolate);
    Alka::registerClass<JsLogger, PluginManagerApi, EventsApi, PacketApi, JsEventControl>(isolate);
}

auto mintPluginManager(const v8::Local<v8::Context> context, JsRuntime::State& state) -> v8::Local<v8::Value> {
    return mintSingleton(
            &StateBindings::pluginManager, context, state, [&] { return std::make_unique<PluginManagerApi>(&state); });
}

auto mintPacket(const v8::Local<v8::Context> context, JsRuntime::State& state) -> v8::Local<v8::Value> {
    return mintSingleton(&StateBindings::packet, context, state, [&] { return std::make_unique<PacketApi>(&state); });
}

auto mintLogger(const v8::Local<v8::Context> context, JsRuntime::State& state, const std::string& scope)
        -> v8::Local<v8::Value> {
    return mintKeyed(&StateBindings::loggers, context, state, scope, [&] {
        return std::make_unique<JsLogger>(state.resolveLogger(scope));
    });
}

auto mintEvents(const v8::Local<v8::Context> context, JsRuntime::State& state, const std::string& ownerName)
        -> v8::Local<v8::Value> {
    return mintKeyed(&StateBindings::events, context, state, ownerName, [&] {
        return std::make_unique<EventsApi>(&state, ownerName);
    });
}

void clearBindings(JsRuntime::State& state) {
    const std::lock_guard lock(gMutex);
    registry().erase(&state);
}

} // namespace CloverNT::Core::Modules::bridge
