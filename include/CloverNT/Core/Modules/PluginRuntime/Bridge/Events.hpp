#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <QQNT/uv.h>
#include <QQNT/v8-cppgc.h>

#include <CloverNT/API/Events/Event.hpp>
#include <CloverNT/API/Events/Listener.hpp>
#include <CloverNT/API/Events/NamedEvent.hpp>
#include <CloverNT/API/Plugin/Plugin.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/EventExports.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/JsRuntimeState.hpp>

namespace CloverNT::Core::Modules::bridge {

struct PendingEvent {
    std::uint64_t  subId; // == Event::ListenerId of the JsListener
    nlohmann::json payload;
};

struct JsEventChannel {
    std::atomic<bool>         alive{true};
    uv_async_t*               async{};
    v8::Isolate*              isolate{};
    uv_thread_t               mainThread{};
    JsRuntime::State*         state{};
    std::mutex                mutex; // guards `queue`
    std::vector<PendingEvent> queue;
};

struct LivePayload {
    v8::Local<v8::Value> value;
};

class JsListener final : public Event::ListenerBase {
public:
    JsListener(std::weak_ptr<JsEventChannel> channel,
               const NativeEventExport*      nativeExport,
               Event::EventPriority          priority,
               std::weak_ptr<Plugin::Plugin> owner)
        : ListenerBase(priority, std::move(owner)), mChannel(std::move(channel)), mExport(nativeExport) {}

    void call(Event::Event& event) override;

private:
    std::weak_ptr<JsEventChannel> mChannel;
    const NativeEventExport*      mExport{};
};

[[nodiscard]] auto jsonFromV8(v8::Isolate* isolate, v8::Local<v8::Context> context, v8::Local<v8::Value> value)
        -> nlohmann::json;
[[nodiscard]] auto v8FromJson(v8::Isolate* isolate, v8::Local<v8::Context> context, const nlohmann::json& json)
        -> v8::Local<v8::Value>;

} // namespace CloverNT::Core::Modules::bridge
