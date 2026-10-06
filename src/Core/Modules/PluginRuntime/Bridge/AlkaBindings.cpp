#include <CloverNT/API/Events/EventBus.hpp>
#include <CloverNT/API/Events/NamedEvent.hpp>
#include <CloverNT/API/Nt/Packet.hpp>
#include <CloverNT/API/Plugin/JsPlugin.hpp>
#include <CloverNT/API/Plugin/PluginManager.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/AlkaBindings.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/Binding.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/EventExports.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/Events.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/JsRuntimeState.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <Alka/Alka.hpp>
#include <nlohmann/json.hpp>


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

bool PluginManagerApi::registerLoader(std::string id, const Alka::Object& config) const {
    if (mState == nullptr || !config.valid()) {
        return false;
    }
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    mState->registerLoaderCallbacks(id, config.local(isolate).As<v8::Object>());
    if (const auto result = Plugin::PluginManager::registerLoader(std::make_shared<JsPluginLoader>(mState, id));
        !result) {
        mState->unregisterLoaderCallbacks(id);
        return false;
    }
    return true;
}

bool PluginManagerApi::unregisterLoader(std::string id) const {
    if (mState == nullptr) {
        return false;
    }
    const auto result = Plugin::PluginManager::unregisterLoader(id);
    mState->unregisterLoaderCallbacks(id);
    return result.has_value();
}

auto EventsApi::on(std::string name, Alka::Function<void(Alka::Value)> callback) -> double {
    return subscribe(std::move(name), std::move(callback), false);
}

auto EventsApi::once(std::string name, Alka::Function<void(Alka::Value)> callback) -> double {
    return subscribe(std::move(name), std::move(callback), true);
}

auto EventsApi::off(const double id) -> bool {
    return mState != nullptr && mState->removeEventSubscription(static_cast<std::uint64_t>(id));
}

auto EventsApi::emit(std::string name, const Alka::Value& payload) -> bool {
    if (mState == nullptr) {
        return false;
    }
    v8::Isolate*               isolate = v8::Isolate::GetCurrent();
    const v8::HandleScope      handleScope(isolate);
    const auto                 context = isolate->GetCurrentContext();
    const v8::Local<v8::Value> live = payload.valid() ? payload.local(isolate) : v8::Undefined(isolate).As<v8::Value>();

    nlohmann::json    json = jsonFromV8(isolate, context, live);
    LivePayload       token{live};
    Event::NamedEvent event(std::move(name), std::move(json));
    event.setBridgeToken(&token);
    Event::EventBus::publish(event, event.name());
    return event.isCancelled();
}

auto EventsApi::subscribe(std::string name, Alka::Function<void(Alka::Value)> callback, const bool once) -> double {
    if (mState == nullptr || !callback.valid()) {
        return -1;
    }
    v8::Isolate*                  isolate = v8::Isolate::GetCurrent();
    const v8::Local<v8::Function> fn      = callback.local(isolate).As<v8::Function>();

    auto owner = Plugin::PluginManager::getPlugin(mOwnerName);
    if (!owner) {
        owner = Plugin::PluginManager::getPlugin("CloverCore");
    }

    const NativeEventExport* nativeExport = findNativeExport(name);
    const std::string        key          = nativeExport != nullptr ? std::string(nativeExport->busKey) : name;
    (void) Event::EventBus::registerEvent(key, Plugin::PluginManager::getPlugin("CloverCore"));

    auto listener =
            std::make_shared<JsListener>(mState->mEventChannel, nativeExport, Event::EventPriority::Normal, owner);
    const std::uint64_t subId = listener->id();
    if (auto result = Event::EventBus::addListener(listener, key); !result) {
        return -1;
    }

    JsRuntime::State::JsSubscription subscription;
    subscription.jsFn.Reset(isolate, fn);
    subscription.jsName    = std::move(name);
    subscription.once      = once;
    subscription.ownerName = mOwnerName;
    mState->mEventSubs.emplace(subId, std::move(subscription));
    return static_cast<double>(subId);
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
    Alka::registerClass<JsLogger,
                        PluginManagerApi,
                        EventsApi,
                        PacketApi,
                        PluginListDto,
                        PluginScanDto,
                        Nt::Packet,
                        Nt::Credential>(isolate);
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
