#include <CloverNT/API/Events/EventBus.hpp>
#include <CloverNT/API/Plugin/JsPlugin.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/AlkaBindings.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/Binding.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/Lifecycle.hpp>
#include <CloverNT/Runtime/Kernel.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace CloverNT::Core::Modules::bridge {

using JsPluginRecord = JsRuntime::State::JsPluginRecord;
using Disposer       = JsRuntime::State::Disposer;
using PluginBinding  = JsRuntime::State::PluginBinding;

namespace {
    bool gShuttingDown = false;

    [[nodiscard]] auto functionProp(const v8::Local<v8::Context> context,
                                    const v8::Local<v8::Object>  object,
                                    const char*                  key) -> v8::Local<v8::Function> {
        if (const auto value = getProp(context, object, key); !value.IsEmpty() && value->IsFunction()) {
            return value.As<v8::Function>();
        }
        return {};
    }

    [[nodiscard]] auto resolvePluginExports(const v8::Local<v8::Context> context, const v8::Local<v8::Value> exports)
            -> v8::Local<v8::Value> {
        if (exports.IsEmpty() || exports->IsFunction() || !exports->IsObject()) {
            return exports;
        }
        const auto object = exports.As<v8::Object>();
        if (!functionProp(context, object, "onLoad").IsEmpty()) {
            return exports;
        }
        const auto fallback = getProp(context, object, "default");
        if (fallback.IsEmpty()) {
            return exports;
        }
        if (fallback->IsFunction()) {
            return fallback;
        }
        if (fallback->IsObject() && !functionProp(context, fallback.As<v8::Object>(), "onLoad").IsEmpty()) {
            return fallback;
        }
        return exports;
    }

    void warnIfAsync(const std::string& name, const v8::Local<v8::Value> result, const std::string_view what) {
        if (!result.IsEmpty() && result->IsPromise()) {
            JsRuntime::State::logger().warn("'{}': {} returned a Promise. The JS plugin lifecycle is synchronous, "
                                            "so the result is dropped and rejections are never reported.",
                                            name,
                                            what);
        }
    }
} // namespace

void CloverContext::setShuttingDown(const bool value) {
    gShuttingDown = value;
}

bool CloverContext::isShuttingDown() noexcept {
    return gShuttingDown;
}

namespace {

    struct HeadersChainState {
        v8::Global<v8::Object>                details;
        v8::Global<v8::Function>              finalCallback;
        v8::Global<v8::Object>                mergedResponse;
        std::vector<v8::Global<v8::Function>> listeners;
        std::size_t                           index{0};
    };

    void advanceHeadersChain(v8::Isolate*                              isolate,
                             v8::Local<v8::Context>                    context,
                             const std::shared_ptr<HeadersChainState>& chain);

    struct HeadersStepData {
        std::shared_ptr<HeadersChainState> chain;
        bool                               settled{false};
    };

    void onHeadersStepCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
        guardedCallback(args, [&] {
            auto* step = dataOf<HeadersStepData>(args);
            if (step == nullptr) {
                return;
            }
            if (step->settled) {
                return;
            }
            step->settled    = true;
            const auto chain = std::move(step->chain);
            delete step;
            if (!chain) {
                return;
            }

            v8::Isolate* isolate = args.GetIsolate();
            const auto   context = isolate->GetCurrentContext();

            if (args.Length() > 0 && args[0]->IsObject()) {
                const auto resp    = args[0].As<v8::Object>();
                const auto merged  = chain->mergedResponse.Get(isolate);
                const auto details = chain->details.Get(isolate);

                if (const auto cancelVal = getProp(context, resp, "cancel");
                    !cancelVal.IsEmpty() && cancelVal->BooleanValue(isolate)) {
                    setProp(context, merged, "cancel", v8::Boolean::New(isolate, true));
                    if (!chain->finalCallback.IsEmpty()) {
                        v8::Local<v8::Value> cbArgs[] = {merged};
                        (void) chain->finalCallback.Get(isolate)->Call(context, v8::Undefined(isolate), 1, cbArgs);
                    }
                    return;
                }
                if (const auto headersVal = getProp(context, resp, "responseHeaders");
                    !headersVal.IsEmpty() && headersVal->IsObject()) {
                    setProp(context, merged, "responseHeaders", headersVal);
                    setProp(context, details, "responseHeaders", headersVal);
                }
                if (const auto statusVal = getProp(context, resp, "statusLine");
                    !statusVal.IsEmpty() && statusVal->IsString()) {
                    setProp(context, merged, "statusLine", statusVal);
                }
            }

            advanceHeadersChain(isolate, context, chain);
        });
    }

    void advanceHeadersChain(v8::Isolate*                              isolate,
                             const v8::Local<v8::Context>              context,
                             const std::shared_ptr<HeadersChainState>& chain) {
        if (chain->index >= chain->listeners.size()) {
            if (!chain->finalCallback.IsEmpty()) {
                v8::Local<v8::Value> cbArgs[] = {chain->mergedResponse.Get(isolate)};
                (void) chain->finalCallback.Get(isolate)->Call(context, v8::Undefined(isolate), 1, cbArgs);
            }
            return;
        }

        const auto listener = chain->listeners[chain->index++].Get(isolate);
        auto*      step     = new HeadersStepData{.chain = chain, .settled = false};
        const auto stepFn   = makeFn(context, &onHeadersStepCallback, v8::External::New(isolate, step));

        v8::Local<v8::Value> callArgs[] = {chain->details.Get(isolate), stepFn};
        const v8::TryCatch   tryCatch(isolate);
        if (listener->Call(context, v8::Undefined(isolate), 2, callArgs).IsEmpty()) {
            JsRuntime::State::logger().error("onHeadersReceived listener threw: {}",
                                             describeException(isolate, context, tryCatch));
            if (!step->settled) {
                step->settled = true;
                step->chain.reset();
                delete step;
                advanceHeadersChain(isolate, context, chain);
            }
        }
    }

    void onHeadersReceivedMuxDispatch(const v8::FunctionCallbackInfo<v8::Value>& args) {
        guardedCallback(args, [&] {
            v8::Isolate* isolate = args.GetIsolate();
            const auto   context = isolate->GetCurrentContext();
            if (!args.Data()->IsArray() || args.Length() < 2 || !args[0]->IsObject() || !args[1]->IsFunction()) {
                return;
            }
            const auto           muxData = args.Data().As<v8::Array>();
            v8::Local<v8::Value> cellVal;
            v8::Local<v8::Value> webReqVal;
            if (!muxData->Get(context, 0).ToLocal(&cellVal) || !muxData->Get(context, 1).ToLocal(&webReqVal) ||
                !cellVal->IsExternal() || !webReqVal->IsObject()) {
                return;
            }
            auto*      cell    = static_cast<JsRuntime::State::SelfCell*>(cellVal.As<v8::External>()->Value());
            auto*      state   = cell != nullptr ? cell->state.load(std::memory_order_acquire) : nullptr;
            const auto finalCb = args[1].As<v8::Function>();

            if (state == nullptr) {
                const auto fallback = v8::Object::New(isolate);
                setProp(context, fallback, "cancel", v8::Boolean::New(isolate, false));
                v8::Local<v8::Value> cbArgs[] = {fallback};
                (void) finalCb->Call(context, v8::Undefined(isolate), 1, cbArgs);
                return;
            }

            const auto webRequest = webReqVal.As<v8::Object>();
            const auto chain      = std::make_shared<HeadersChainState>();
            chain->details.Reset(isolate, args[0].As<v8::Object>());
            chain->finalCallback.Reset(isolate, finalCb);

            const auto merged = v8::Object::New(isolate);
            setProp(context, merged, "cancel", v8::Boolean::New(isolate, false));
            chain->mergedResponse.Reset(isolate, merged);

            for (const auto& mux: state->mHeadersMuxes) {
                if (mux && !mux->webRequest.IsEmpty() && mux->webRequest.Get(isolate)->StrictEquals(webRequest)) {
                    chain->listeners.reserve(mux->entries.size());
                    for (const auto& entry: mux->entries) {
                        if (!entry.listener.IsEmpty()) {
                            chain->listeners.emplace_back(isolate, entry.listener.Get(isolate));
                        }
                    }
                    break;
                }
            }

            advanceHeadersChain(isolate, context, chain);
        });
    }

    void onTimeoutFired(const v8::FunctionCallbackInfo<v8::Value>& args) {
        guardedCallback(args, [&] {
            v8::Isolate* isolate = args.GetIsolate();
            const auto   context = isolate->GetCurrentContext();
            if (!args.Data()->IsArray()) {
                return;
            }
            const auto           wrapData = args.Data().As<v8::Array>();
            v8::Local<v8::Value> bindVal;
            v8::Local<v8::Value> tokenVal;
            v8::Local<v8::Value> fnVal;
            if (!wrapData->Get(context, 0).ToLocal(&bindVal) || !wrapData->Get(context, 1).ToLocal(&tokenVal) ||
                !wrapData->Get(context, 2).ToLocal(&fnVal) || !fnVal->IsFunction()) {
                return;
            }
            if (bindVal->IsExternal() && tokenVal->IsBigInt()) {
                auto*               binding = static_cast<PluginBinding*>(bindVal.As<v8::External>()->Value());
                bool                lossless{};
                const std::uint64_t token = tokenVal.As<v8::BigInt>()->Uint64Value(&lossless);
                if (binding != nullptr && binding->alive.load(std::memory_order_acquire)) {
                    if (auto* rec = binding->record.load(std::memory_order_acquire); rec != nullptr) {
                        std::erase_if(rec->disposers, [token](const Disposer& d) {
                            return d.kind == Disposer::Kind::ClearTimer && !d.interval && d.timerToken == token;
                        });
                    }
                }
            }

            std::vector<v8::Local<v8::Value>> callArgs;
            callArgs.reserve(static_cast<std::size_t>(args.Length()));
            for (int i = 0; i < args.Length(); ++i) {
                callArgs.push_back(args[i]);
            }
            (void) fnVal.As<v8::Function>()->Call(context,
                                                  args.This(),
                                                  static_cast<int>(callArgs.size()),
                                                  callArgs.empty() ? nullptr : callArgs.data());
        });
    }

} // namespace

void CloverContext::invalidateBindings(JsRuntime::State& state, const JsPluginRecord* record) {
    for (auto* binding: state.mBindings) {
        if (binding != nullptr && binding->record.load(std::memory_order_acquire) == record) {
            binding->alive.store(false, std::memory_order_release);
            binding->record.store(nullptr, std::memory_order_release);
            binding->state.store(nullptr, std::memory_order_release);
        }
    }
    if (record != nullptr) {
        (void) Event::EventBus::clearPlugin(record->name);
    }
}

auto CloverContext::liveBinding(const v8::FunctionCallbackInfo<v8::Value>& args) -> JsRuntime::State::PluginBinding* {
    auto* binding = dataOf<PluginBinding>(args);
    if (binding == nullptr || !binding->alive.load(std::memory_order_acquire) ||
        binding->record.load(std::memory_order_acquire) == nullptr ||
        binding->state.load(std::memory_order_acquire) == nullptr) {
        return nullptr;
    }
    return binding;
}

void CloverContext::pushListenerDisposer(const PluginBinding*          binding,
                                         v8::Isolate*                  isolate,
                                         const v8::Local<v8::Object>   target,
                                         const std::string&            key,
                                         const v8::Local<v8::Function> listener) {
    auto* rec = binding->record.load(std::memory_order_acquire);
    if (rec == nullptr) {
        return;
    }
    Disposer disposer;
    disposer.kind = Disposer::Kind::RemoveListener;
    disposer.target.Reset(isolate, target);
    disposer.key = key;
    disposer.listener.Reset(isolate, listener);
    rec->disposers.push_back(std::move(disposer));
}

void CloverContext::appOn(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args, [&] {
        const auto* binding = liveBinding(args);
        if (binding == nullptr) {
            return;
        }
        const auto* state = binding->state.load(std::memory_order_acquire);
        if (state == nullptr || args.Length() < 2 || !args[1]->IsFunction() || state->mApp.IsEmpty()) {
            return;
        }
        v8::Isolate* isolate = args.GetIsolate();
        const auto   context = isolate->GetCurrentContext();
        const auto   app     = state->mApp.Get(isolate);
        invokeMethod(context, app, "on", {args[0], args[1]});
        pushListenerDisposer(binding, isolate, app, toUtf8(isolate, args[0]), args[1].As<v8::Function>());
    });
}

void CloverContext::ipcOn(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args, [&] {
        const auto* binding = liveBinding(args);
        if (binding == nullptr) {
            return;
        }
        const auto* state = binding->state.load(std::memory_order_acquire);
        if (state == nullptr || args.Length() < 2 || !args[1]->IsFunction() || state->mIpcMain.IsEmpty()) {
            return;
        }
        v8::Isolate* isolate = args.GetIsolate();
        const auto   context = isolate->GetCurrentContext();
        const auto   ipc     = state->mIpcMain.Get(isolate);
        invokeMethod(context, ipc, "on", {args[0], args[1]});
        pushListenerDisposer(binding, isolate, ipc, toUtf8(isolate, args[0]), args[1].As<v8::Function>());
    });
}

void CloverContext::ipcHandle(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args, [&] {
        const auto* binding = liveBinding(args);
        if (binding == nullptr) {
            return;
        }
        const auto* state = binding->state.load(std::memory_order_acquire);
        auto*       rec   = binding->record.load(std::memory_order_acquire);
        if (state == nullptr || rec == nullptr || args.Length() < 2 || !args[1]->IsFunction() ||
            state->mIpcMain.IsEmpty()) {
            return;
        }
        v8::Isolate* isolate = args.GetIsolate();
        const auto   context = isolate->GetCurrentContext();
        const auto   ipc     = state->mIpcMain.Get(isolate);
        invokeMethod(context, ipc, "handle", {args[0], args[1]});
        Disposer disposer;
        disposer.kind = Disposer::Kind::RemoveHandler;
        disposer.target.Reset(isolate, ipc);
        disposer.key = toUtf8(isolate, args[0]);
        rec->disposers.push_back(std::move(disposer));
    });
}

void CloverContext::onHeadersReceived(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args, [&] {
        const auto* binding = liveBinding(args);
        if (binding == nullptr || args.Length() < 2 || !args[0]->IsObject() || !args[1]->IsFunction()) {
            return;
        }
        auto* state = binding->state.load(std::memory_order_acquire);
        auto* rec   = binding->record.load(std::memory_order_acquire);
        if (state == nullptr || rec == nullptr) {
            return;
        }
        v8::Isolate*               isolate   = args.GetIsolate();
        const auto                 context   = isolate->GetCurrentContext();
        const auto                 session   = args[0].As<v8::Object>();
        const v8::Local<v8::Value> webReqVal = getProp(context, session, "webRequest");
        if (webReqVal.IsEmpty() || !webReqVal->IsObject()) {
            return;
        }
        const auto webRequest = webReqVal.As<v8::Object>();

        JsRuntime::State::WebRequestHeadersMux* targetMux = nullptr;
        for (const auto& existing: state->mHeadersMuxes) {
            if (existing && !existing->webRequest.IsEmpty() &&
                existing->webRequest.Get(isolate)->StrictEquals(webRequest)) {
                targetMux = existing.get();
                break;
            }
        }
        if (targetMux == nullptr) {
            auto mux       = std::make_unique<JsRuntime::State::WebRequestHeadersMux>();
            mux->stateCell = state->selfCell();
            mux->webRequest.Reset(isolate, webRequest);
            targetMux = mux.get();
            state->mHeadersMuxes.push_back(std::move(mux));

            const auto muxData = v8::Array::New(isolate, 2);
            (void) muxData->Set(context, 0, v8::External::New(isolate, state->selfCell()));
            (void) muxData->Set(context, 1, webRequest);
            const auto muxFn = makeFn(context, &onHeadersReceivedMuxDispatch, muxData);
            invokeMethod(context, webRequest, "onHeadersReceived", {muxFn});
        }

        const std::uint64_t handlerId = state->mNextHeadersHandlerId++;
        auto& [id, listener]          = targetMux->entries.emplace_back();
        id                            = handlerId;
        listener.Reset(isolate, args[1].As<v8::Function>());

        Disposer disposer;
        disposer.kind             = Disposer::Kind::RemoveHeadersReceived;
        disposer.headersHandlerId = handlerId;
        disposer.target.Reset(isolate, webRequest);
        rec->disposers.push_back(std::move(disposer));
    });
}

void CloverContext::setTimer(const v8::FunctionCallbackInfo<v8::Value>& args, const bool interval) {
    guardedCallback(args, [&] {
        auto* binding = liveBinding(args);
        if (binding == nullptr) {
            return;
        }
        auto* state = binding->state.load(std::memory_order_acquire);
        auto* rec   = binding->record.load(std::memory_order_acquire);
        if (state == nullptr || rec == nullptr) {
            return;
        }
        v8::Isolate*               isolate = args.GetIsolate();
        const auto                 context = isolate->GetCurrentContext();
        const v8::Local<v8::Value> fnVal = getProp(context, context->Global(), interval ? "setInterval" : "setTimeout");
        if (fnVal.IsEmpty() || !fnVal->IsFunction()) {
            return;
        }
        const std::uint64_t               token = state->mNextTimerToken++;
        std::vector<v8::Local<v8::Value>> argv;
        argv.reserve(static_cast<std::size_t>(args.Length()));
        for (int i = 0; i < args.Length(); ++i) {
            if (i == 0 && !interval && args[0]->IsFunction()) {
                const auto wrapData = v8::Array::New(isolate, 3);
                (void) wrapData->Set(context, 0, v8::External::New(isolate, binding));
                (void) wrapData->Set(context, 1, v8::BigInt::NewFromUnsigned(isolate, token));
                (void) wrapData->Set(context, 2, args[0]);
                argv.emplace_back(makeFn(context, &onTimeoutFired, wrapData));
            } else {
                argv.push_back(args[i]);
            }
        }
        v8::Local<v8::Value> idValue;
        if (!fnVal.As<v8::Function>()
                     ->Call(context,
                            context->Global(),
                            static_cast<int>(argv.size()),
                            argv.empty() ? nullptr : argv.data())
                     .ToLocal(&idValue)) {
            return;
        }
        Disposer disposer;
        disposer.kind       = Disposer::Kind::ClearTimer;
        disposer.interval   = interval;
        disposer.timerToken = token;
        disposer.timerId.Reset(isolate, idValue);
        rec->disposers.push_back(std::move(disposer));
        args.GetReturnValue().Set(idValue);
    });
}

void CloverContext::setTimeout(const v8::FunctionCallbackInfo<v8::Value>& args) {
    setTimer(args, false);
}

void CloverContext::setInterval(const v8::FunctionCallbackInfo<v8::Value>& args) {
    setTimer(args, true);
}

void CloverContext::onUnload(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args, [&] {
        const auto* binding = liveBinding(args);
        if (binding == nullptr || args.Length() < 1 || !args[0]->IsFunction()) {
            return;
        }
        auto* rec = binding->record.load(std::memory_order_acquire);
        if (rec == nullptr) {
            return;
        }
        Disposer disposer;
        disposer.kind = Disposer::Kind::CallFn;
        disposer.fn.Reset(args.GetIsolate(), args[0].As<v8::Function>());
        rec->disposers.push_back(std::move(disposer));
    });
}

auto CloverContext::create(JsRuntime::State& state, const v8::Local<v8::Context> context, JsPluginRecord& record)
        -> v8::Local<v8::Object> {
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    const auto   ctxObj  = v8::Object::New(isolate);

    setProp(context, ctxObj, "name", v8Str(isolate, record.name));
    setProp(context, ctxObj, "version", v8Str(isolate, CLOVERNT_VERSION_FULL));
    setProp(context, ctxObj, "loaderVersion", v8Str(isolate, std::string{Runtime::Kernel::selfVersion()}));
    setProp(context, ctxObj, "qqVersion", v8Str(isolate, std::string{Runtime::Kernel::qqVersion()}));
    setProp(context, ctxObj, "coreDir", v8Str(isolate, state.mCoreDir.string()));
    setProp(context, ctxObj, "pluginsDir", v8Str(isolate, std::string{Runtime::Kernel::pluginsDir()}));
    (void) ctxObj->SetNativeDataProperty(
            context, v8Str(isolate, "plugins"), &pluginsGetter, nullptr, v8::External::New(isolate, state.selfCell()));
    setProp(context, ctxObj, "logger", Logger::create(state, context, record.name));
    if (!state.mElectron.IsEmpty()) {
        setProp(context, ctxObj, "electron", state.mElectron.Get(isolate));
    }

    auto*      bindingPtr = state.allocatePluginBinding(&record);
    const auto bext       = v8::External::New(isolate, bindingPtr);

    const auto app = v8::Object::New(isolate);
    setProp(context, app, "on", makeFn(context, &CloverContext::appOn, bext));
    setProp(context, ctxObj, "app", app);

    const auto ipcMain = v8::Object::New(isolate);
    setProp(context, ipcMain, "on", makeFn(context, &CloverContext::ipcOn, bext));
    setProp(context, ipcMain, "handle", makeFn(context, &CloverContext::ipcHandle, bext));
    setProp(context, ctxObj, "ipcMain", ipcMain);

    setProp(context, ctxObj, "onHeadersReceived", makeFn(context, &CloverContext::onHeadersReceived, bext));
    setProp(context, ctxObj, "setTimeout", makeFn(context, &CloverContext::setTimeout, bext));
    setProp(context, ctxObj, "setInterval", makeFn(context, &CloverContext::setInterval, bext));
    setProp(context, ctxObj, "onUnload", makeFn(context, &CloverContext::onUnload, bext));
    setProp(context, ctxObj, "events", mintEvents(context, state, record.name));
    setProp(context, ctxObj, "packet", mintPacket(context, state));
    return ctxObj;
}

void CloverContext::runDisposers(JsRuntime::State&            state,
                                 const v8::Local<v8::Context> context,
                                 JsPluginRecord&              record,
                                 std::vector<std::string>&    errors) {
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    for (auto disposers = std::exchange(record.disposers, {});
         auto& [kind, target, key, listener, timerId, timerToken, headersHandlerId, interval, fn]:
         std::views::reverse(disposers)) {
        (void) timerToken;
        if (gShuttingDown && kind != Disposer::Kind::CallFn) {
            continue;
        }
        const v8::TryCatch tryCatch(isolate);
        switch (kind) {
        case Disposer::Kind::RemoveListener:
            if (!target.IsEmpty() && !listener.IsEmpty()) {
                invokeMethod(
                        context, target.Get(isolate), "removeListener", {v8Str(isolate, key), listener.Get(isolate)});
            }
            break;
        case Disposer::Kind::RemoveHandler:
            if (!target.IsEmpty()) {
                invokeMethod(context, target.Get(isolate), "removeHandler", {v8Str(isolate, key)});
            }
            break;
        case Disposer::Kind::RemoveHeadersReceived:
            if (!target.IsEmpty()) {
                const auto webReq = target.Get(isolate);
                for (auto it = state.mHeadersMuxes.begin(); it != state.mHeadersMuxes.end(); ++it) {
                    const auto& mux = *it;
                    if (mux && !mux->webRequest.IsEmpty() && mux->webRequest.Get(isolate)->StrictEquals(webReq)) {
                        std::erase_if(mux->entries,
                                      [headersHandlerId](const auto& e) { return e.id == headersHandlerId; });
                        if (mux->entries.empty()) {
                            invokeMethod(context, webReq, "onHeadersReceived", {v8::Null(isolate)});
                            state.mHeadersMuxes.erase(it);
                        }
                        break;
                    }
                }
            }
            break;
        case Disposer::Kind::ClearTimer:
            if (!timerId.IsEmpty()) {
                invokeMethod(context,
                             context->Global(),
                             interval ? "clearInterval" : "clearTimeout",
                             {timerId.Get(isolate)});
            }
            break;
        case Disposer::Kind::CallFn:
            if (!fn.IsEmpty()) {
                (void) fn.Get(isolate)->Call(context, context->Global(), 0, nullptr);
            }
            break;
        }
        if (tryCatch.HasCaught()) {
            JsRuntime::State::logger().error(
                    "Plugin '{}' disposer threw: {}", record.name, describeException(isolate, context, tryCatch));
            errors.push_back(extractExceptionMessage(isolate, context, tryCatch));
        }
    }
}

void PluginLifecycle::purgeCache(JsRuntime::State&            state,
                                 const v8::Local<v8::Context> context,
                                 const JsPluginRecord&        record) {
    if (state.mModuleCache.IsEmpty()) {
        return;
    }
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    const auto   cache   = state.mModuleCache.Get(isolate);
    for (auto const& key: record.cacheKeys) {
        bool stillReferenced = false;
        {
            std::lock_guard lock(state.mMutex);
            for (const auto& otherRecord: state.mRecords | std::views::values) {
                if (otherRecord && std::ranges::find(otherRecord->cacheKeys, key) != otherRecord->cacheKeys.end()) {
                    stillReferenced = true;
                    break;
                }
            }
        }
        if (!stillReferenced) {
            (void) cache->Delete(context, v8Str(isolate, key));
        }
    }
}

auto PluginLifecycle::load(JsRuntime::State& state, const Plugin::JsPlugin& plugin) -> Expected<void> {
    v8::Isolate* isolate = state.mIsolate;
    if (isolate == nullptr) {
        return jsError("JS runtime is not attached", CommonErrorCode::Inactive);
    }
    const std::string name(plugin.name());
    {
        std::lock_guard lock(state.mMutex);
        if (state.mRecords.contains(name)) {
            return jsError(name + " is already loaded", CommonErrorCode::AlreadyExists);
        }
    }

    const v8::Isolate::Scope isolateScope(isolate);
    v8::HandleScope          handleScope(isolate);
    const auto               context = state.mContext.Get(isolate);
    v8::Context::Scope       contextScope(context);

    auto record          = std::make_unique<JsPluginRecord>();
    record->name         = name;
    record->mainPath     = plugin.mainEntry();
    record->preloadPath  = plugin.preloadEntry();
    record->rendererPath = plugin.rendererEntry();
    record->preloadId    = "clovernt:" + name;

    if (record->preloadPath) {
        for (auto& session: state.mSessions) {
            JsRuntime::State::registerPreloadOn(
                    context, session.Get(isolate), record->preloadId, record->preloadPath->string());
        }
    }

    if (record->mainPath) {
        std::vector<std::string> before;
        if (!state.mModuleCache.IsEmpty()) {
            before = objectOwnKeys(context, state.mModuleCache.Get(isolate));
        }
        const auto rollback = [&] {
            if (record->preloadPath) {
                for (auto& session: state.mSessions) {
                    JsRuntime::State::unregisterPreloadOn(context, session.Get(isolate), record->preloadId);
                }
            }
            if (!state.mModuleCache.IsEmpty()) {
                for (const auto  cache = state.mModuleCache.Get(isolate);
                     auto const& key: newKeys(objectOwnKeys(context, cache), before)) {
                    (void) cache->Delete(context, v8Str(isolate, key));
                }
            }
        };

        const v8::TryCatch   tryCatch(isolate);
        v8::Local<v8::Value> exports;
        if (!state.requireModule(context, record->mainPath->string()).ToLocal(&exports)) {
            if (tryCatch.HasCaught()) {
                JsRuntime::State::logger().error(
                        "Plugin '{}' failed to load: {}", name, describeException(isolate, context, tryCatch));
            }
            const std::string err = tryCatch.HasCaught() ? extractExceptionMessage(isolate, context, tryCatch)
                                                         : "module require failed";
            rollback();
            return jsError(err, CommonErrorCode::CallbackFailed);
        }
        if (!state.mModuleCache.IsEmpty()) {
            record->cacheKeys = newKeys(objectOwnKeys(context, state.mModuleCache.Get(isolate)), before);
        }

        const auto ctxObj = CloverContext::create(state, context, *record);
        if (const auto entry = resolvePluginExports(context, exports); entry->IsFunction()) {
            v8::Local<v8::Value> ctxArgs[] = {ctxObj};
            v8::Local<v8::Value> disposeVal;
            const bool           called =
                    entry.As<v8::Function>()->Call(context, context->Global(), 1, ctxArgs).ToLocal(&disposeVal);
            if (!called) {
                if (tryCatch.HasCaught()) {
                    JsRuntime::State::logger().error(
                            "Plugin '{}' entry threw: {}", name, describeException(isolate, context, tryCatch));
                }
                const std::string err = tryCatch.HasCaught() ? extractExceptionMessage(isolate, context, tryCatch)
                                                             : "plugin entry threw";
                CloverContext::invalidateBindings(state, record.get());
                std::vector<std::string> disposeErrors;
                CloverContext::runDisposers(state, context, *record, disposeErrors);
                rollback();
                return jsError(err, CommonErrorCode::CallbackFailed);
            }
            if (disposeVal->IsFunction()) {
                Disposer disposer;
                disposer.kind = Disposer::Kind::CallFn;
                disposer.fn.Reset(isolate, disposeVal.As<v8::Function>());
                record->disposers.push_back(std::move(disposer));
            } else {
                warnIfAsync(name, disposeVal, "the plugin entry");
            }
        } else if (entry->IsObject() && !functionProp(context, entry.As<v8::Object>(), "onLoad").IsEmpty()) {
            const auto           object    = entry.As<v8::Object>();
            v8::Local<v8::Value> ctxArgs[] = {ctxObj};
            v8::Local<v8::Value> loadResult;
            if (!functionProp(context, object, "onLoad")->Call(context, object, 1, ctxArgs).ToLocal(&loadResult)) {
                if (tryCatch.HasCaught()) {
                    JsRuntime::State::logger().error(
                            "Plugin '{}' onLoad threw: {}", name, describeException(isolate, context, tryCatch));
                }
                const std::string err = tryCatch.HasCaught() ? extractExceptionMessage(isolate, context, tryCatch)
                                                             : "plugin onLoad threw";
                CloverContext::invalidateBindings(state, record.get());
                std::vector<std::string> disposeErrors;
                CloverContext::runDisposers(state, context, *record, disposeErrors);
                rollback();
                return jsError(err, CommonErrorCode::CallbackFailed);
            }
            warnIfAsync(name, loadResult, "onLoad");
            if (const auto onUnload = functionProp(context, object, "onUnload"); !onUnload.IsEmpty()) {
                record->onUnload.Reset(isolate, onUnload);
                record->onUnloadThis.Reset(isolate, object);
            }
        } else {
            JsRuntime::State::logger().warn(
                    "'{}': the main entry exports neither a function nor an object with onLoad, so nothing was "
                    "called. Export the plugin as `export default` or `export function onLoad`.",
                    name);
        }
    }

    {
        std::lock_guard lock(state.mMutex);
        state.mRecords.emplace(name, std::move(record));
    }
    return {};
}

auto PluginLifecycle::unload(JsRuntime::State& state, const std::string_view name) -> Expected<void> {
    v8::Isolate* isolate = state.mIsolate;
    if (isolate == nullptr) {
        return jsError("JS runtime is not attached", CommonErrorCode::Inactive);
    }
    std::unique_ptr<JsPluginRecord> record;
    {
        std::lock_guard lock(state.mMutex);
        const auto      it = state.mRecords.find(name);
        if (it == state.mRecords.end()) {
            return jsError(std::string(name) + " is not loaded", CommonErrorCode::NotFound);
        }
        record = std::move(it->second);
        state.mRecords.erase(it);
    }

    const v8::Isolate::Scope isolateScope(isolate);
    v8::HandleScope          handleScope(isolate);
    const auto               context = state.mContext.Get(isolate);
    v8::Context::Scope       contextScope(context);

    CloverContext::invalidateBindings(state, record.get());

    std::vector<std::string> errors;
    if (!record->onUnload.IsEmpty()) {
        const v8::TryCatch         tryCatch(isolate);
        const v8::Local<v8::Value> receiver = record->onUnloadThis.IsEmpty() ? v8::Local<v8::Value>(context->Global())
                                                                             : record->onUnloadThis.Get(isolate);
        (void) record->onUnload.Get(isolate)->Call(context, receiver, 0, nullptr);
        if (tryCatch.HasCaught()) {
            JsRuntime::State::logger().error(
                    "Plugin '{}' onUnload threw: {}", name, describeException(isolate, context, tryCatch));
            errors.push_back("onUnload: " + extractExceptionMessage(isolate, context, tryCatch));
        }
    }

    CloverContext::runDisposers(state, context, *record, errors);

    if (record->preloadPath) {
        for (auto& session: state.mSessions) {
            JsRuntime::State::unregisterPreloadOn(context, session.Get(isolate), record->preloadId);
        }
    }

    purgeCache(state, context, *record);

    if (!errors.empty()) {
        std::string joined;
        for (std::size_t i = 0; i < errors.size(); ++i) {
            if (i != 0) {
                joined += '\n';
            }
            joined += errors[i];
        }
        return jsError(joined, CommonErrorCode::CallbackFailed);
    }
    return {};
}

auto PluginLifecycle::reload(JsRuntime::State& state, const std::string_view name) -> Expected<void> {
    std::shared_ptr<Plugin::JsPlugin> plugin;
    {
        std::lock_guard lock(state.mMutex);
        for (auto const& queued: state.mQueue) {
            if (queued && queued->name() == name) {
                plugin = queued;
                break;
            }
        }
    }
    if (!plugin) {
        return jsError("JS plugin is not queued: " + std::string(name), CommonErrorCode::NotFound);
    }
    const bool touchesRenderer = plugin->preloadEntry().has_value() || plugin->rendererEntry().has_value();

    auto unloaded = unload(state, name);
    auto loaded   = load(state, *plugin);
    if (touchesRenderer) {
        state.reloadAllWindows();
    }
    if (!loaded) {
        return loaded;
    }
    if (!unloaded) {
        JsRuntime::State::logger().error(
                "reload({}): unload phase reported errors: {}", name, unloaded.error().message);
    }
    return {};
}

} // namespace CloverNT::Core::Modules::bridge
