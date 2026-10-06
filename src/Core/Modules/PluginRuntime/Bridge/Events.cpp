#include <CloverNT/API/Events/EventBus.hpp>
#include <CloverNT/API/Events/NamedEvent.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/Events.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/Support.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace CloverNT::Core::Modules::bridge {

auto jsonFromV8(v8::Isolate* isolate, const v8::Local<v8::Context> context, const v8::Local<v8::Value> value)
        -> nlohmann::json {
    if (value.IsEmpty() || value->IsUndefined() || value->IsNull()) {
        return nullptr;
    }
    if (value->IsBoolean()) {
        return value->BooleanValue(isolate);
    }
    if (value->IsInt32()) {
        return value.As<v8::Int32>()->Value();
    }
    if (value->IsUint32()) {
        return value.As<v8::Uint32>()->Value();
    }
    if (value->IsNumber()) {
        return value.As<v8::Number>()->Value();
    }
    if (value->IsString()) {
        return toUtf8(isolate, value);
    }
    if (value->IsArray()) {
        const auto arr = value.As<v8::Array>();
        auto       out = nlohmann::json::array();
        for (std::uint32_t i = 0; i < arr->Length(); ++i) {
            v8::Local<v8::Value> element;
            out.push_back(arr->Get(context, i).ToLocal(&element) ? jsonFromV8(isolate, context, element)
                                                                 : nlohmann::json(nullptr));
        }
        return out;
    }
    if (value->IsObject() && !value->IsFunction()) {
        const auto           obj = value.As<v8::Object>();
        auto                 out = nlohmann::json::object();
        v8::Local<v8::Array> names;
        if (obj->GetOwnPropertyNames(context).ToLocal(&names)) {
            for (std::uint32_t i = 0; i < names->Length(); ++i) {
                v8::Local<v8::Value> key;
                if (!names->Get(context, i).ToLocal(&key)) {
                    continue;
                }
                v8::Local<v8::Value> propertyValue;
                if (obj->Get(context, key).ToLocal(&propertyValue)) {
                    out[toUtf8(isolate, key)] = jsonFromV8(isolate, context, propertyValue);
                }
            }
        }
        return out;
    }
    // Functions, symbols, BigInt, etc. cannot cross to a portable payload.
    return nullptr;
}

auto v8FromJson(v8::Isolate* isolate, const v8::Local<v8::Context> context, const nlohmann::json& json)
        -> v8::Local<v8::Value> {
    switch (json.type()) {
    case nlohmann::json::value_t::boolean:
        return v8::Boolean::New(isolate, json.get<bool>());
    case nlohmann::json::value_t::number_integer:
        return v8::Number::New(isolate, static_cast<double>(json.get<std::int64_t>()));
    case nlohmann::json::value_t::number_unsigned:
        return v8::Number::New(isolate, static_cast<double>(json.get<std::uint64_t>()));
    case nlohmann::json::value_t::number_float:
        return v8::Number::New(isolate, json.get<double>());
    case nlohmann::json::value_t::string:
        return v8Str(isolate, json.get_ref<const std::string&>());
    case nlohmann::json::value_t::array: {
        const auto    arr   = v8::Array::New(isolate, static_cast<int>(json.size()));
        std::uint32_t index = 0;
        for (const auto& element: json) {
            (void) arr->Set(context, index++, v8FromJson(isolate, context, element));
        }
        return arr;
    }
    case nlohmann::json::value_t::object: {
        const auto obj = v8::Object::New(isolate);
        for (auto it = json.begin(); it != json.end(); ++it) {
            (void) obj->Set(context, v8Str(isolate, it.key()), v8FromJson(isolate, context, it.value()));
        }
        return obj;
    }
    case nlohmann::json::value_t::null:
    case nlohmann::json::value_t::discarded:
        return v8::Null(isolate);
    default: // binary and anything else is unsupported
        return v8::Undefined(isolate);
    }
}

void JsListener::call(Event::Event& event) {
    const auto channel = mChannel.lock();
    if (!channel || !channel->alive.load(std::memory_order_acquire)) {
        return;
    }

    nlohmann::json     payload;
    void*              liveToken = nullptr;
    Event::NamedEvent* named     = nullptr;
    if (mExport != nullptr) {
        payload = mExport->toPayload(event);
    } else {
        named     = static_cast<Event::NamedEvent*>(&event); // script events publish NamedEvent under a string key
        payload   = named->payload();
        liveToken = named->bridgeToken();
    }

    const uv_thread_t self = uv_thread_self();
    if (uv_thread_equal(&self, &channel->mainThread) != 0) {
        v8::Isolate* const         isolate = channel->isolate;
        const v8::HandleScope      handleScope(isolate);
        const auto                 context = channel->state->mContext.Get(isolate);
        const v8::Context::Scope   contextScope(context);
        const v8::Local<v8::Value> payloadValue = liveToken != nullptr ? static_cast<LivePayload*>(liveToken)->value
                                                                       : v8FromJson(isolate, context, payload);
        channel->state->invokeEventCallback(id(), payloadValue, named);
    } else {
        const std::lock_guard lock(channel->mutex);
        if (!channel->alive.load(std::memory_order_acquire) || channel->async == nullptr) {
            return;
        }
        channel->queue.push_back(PendingEvent{id(), std::move(payload)});
        uv_async_send(channel->async);
    }
}

} // namespace CloverNT::Core::Modules::bridge

namespace CloverNT::Core::Modules {

namespace {

    void controlCancel(const v8::FunctionCallbackInfo<v8::Value>& args) {
        guardedCallback(args, [&] {
            if (auto* event = dataOf<Event::NamedEvent>(args); event != nullptr) {
                event->setCancelled(true);
            }
        });
    }

} // namespace

void JsRuntime::State::invokeEventCallback(const std::uint64_t        subId,
                                           const v8::Local<v8::Value> payload,
                                           Event::NamedEvent*         cancellable) {
    v8::Isolate* isolate = mIsolate;
    if (isolate == nullptr) {
        return;
    }
    const auto it = mEventSubs.find(subId);
    if (it == mEventSubs.end() || it->second.jsFn.IsEmpty()) {
        return;
    }
    const v8::Local<v8::Function> fn   = it->second.jsFn.Get(isolate);
    const bool                    once = it->second.once;

    const auto context = mContext.Get(isolate);

    const v8::Local<v8::Value> controlData = cancellable != nullptr
                                                     ? v8::External::New(isolate, cancellable).As<v8::Value>()
                                                     : v8::Undefined(isolate).As<v8::Value>();
    const auto                 control     = v8::Object::New(isolate);
    setProp(context, control, "cancel", makeFn(context, &controlCancel, controlData));

    v8::Local<v8::Value> argv[2] = {payload.IsEmpty() ? v8::Undefined(isolate).As<v8::Value>() : payload, control};
    const v8::TryCatch   tryCatch(isolate);
    (void) fn->Call(context, v8::Undefined(isolate), 2, argv);
    if (tryCatch.HasCaught()) {
        logger().error("clovernt.events listener threw: {}", describeException(isolate, context, tryCatch));
    }

    if (once) {
        removeEventSubscription(subId);
    }
}

void JsRuntime::State::dispatchEventToJs(const bridge::PendingEvent& pending) {
    v8::Isolate* isolate = mIsolate;
    if (isolate == nullptr) {
        return;
    }
    const v8::HandleScope    handleScope(isolate);
    const auto               context = mContext.Get(isolate);
    const v8::Context::Scope contextScope(context);
    const auto               payload = bridge::v8FromJson(isolate, context, pending.payload);
    invokeEventCallback(pending.subId, payload, nullptr);
}

void JsRuntime::State::onEventAsync(uv_async_t* handle) {
    auto* self = static_cast<State*>(handle->data);
    if (self == nullptr) {
        return;
    }
    const auto channel = self->mEventChannel;
    if (!channel) {
        return;
    }
    std::vector<bridge::PendingEvent> events;
    {
        const std::lock_guard lock(channel->mutex);
        events.swap(channel->queue);
    }
    if (self->mIsolate == nullptr) {
        return;
    }
    const v8::Isolate::Scope isolateScope(self->mIsolate);
    for (const auto& pending: events) {
        try {
            self->dispatchEventToJs(pending);
        } catch (const std::exception& exception) {
            logger().error("clovernt.events async dispatch faulted: {}", exception.what());
        } catch (...) {
            logger().error("clovernt.events async dispatch faulted with an unknown exception");
        }
    }
}

bool JsRuntime::State::removeEventSubscription(const std::uint64_t subId) {
    const auto it = mEventSubs.find(subId);
    if (it == mEventSubs.end()) {
        return false;
    }
    (void) Event::EventBus::removeListener(subId);
    it->second.jsFn.Reset();
    mEventSubs.erase(it);
    return true;
}

void JsRuntime::State::clearEventSubscriptionsForOwner(const std::string_view ownerName) {
    for (auto it = mEventSubs.begin(); it != mEventSubs.end();) {
        if (it->second.ownerName == ownerName) {
            (void) Event::EventBus::removeListener(it->first);
            it->second.jsFn.Reset();
            it = mEventSubs.erase(it);
        } else {
            ++it;
        }
    }
}

void JsRuntime::State::clearEventSubscriptions() {
    for (auto& [id, subscription]: mEventSubs) {
        (void) Event::EventBus::removeListener(id);
        subscription.jsFn.Reset();
    }
    mEventSubs.clear();
}

} // namespace CloverNT::Core::Modules
