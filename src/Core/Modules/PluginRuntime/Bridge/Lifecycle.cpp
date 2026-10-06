#include <CloverNT/API/Plugin/JsPlugin.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/AlkaBindings.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/Binding.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/Lifecycle.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <ranges>
#include <utility>
#include <vector>

namespace CloverNT::Core::Modules::bridge {

using JsPluginRecord = JsRuntime::State::JsPluginRecord;
using Disposer       = JsRuntime::State::Disposer;
using PluginBinding  = JsRuntime::State::PluginBinding;

namespace {
    bool gShuttingDown = false;
} // namespace

void CloverContext::setShuttingDown(const bool value) {
    gShuttingDown = value;
}

void CloverContext::invalidateBindings(JsRuntime::State& state, const JsPluginRecord* record) {
    for (const auto& binding: state.mBindings) {
        if (binding && binding->record == record) {
            binding->alive.store(false);
            binding->record = nullptr;
        }
    }
    if (record != nullptr) {
        state.clearEventSubscriptionsForOwner(record->name);
    }
}

auto CloverContext::liveBinding(const v8::FunctionCallbackInfo<v8::Value>& args) -> JsRuntime::State::PluginBinding* {
    auto* binding = dataOf<PluginBinding>(args);
    if (binding == nullptr || !binding->alive.load() || binding->record == nullptr || binding->state == nullptr) {
        return nullptr;
    }
    return binding;
}

void CloverContext::pushListenerDisposer(const PluginBinding*          binding,
                                         v8::Isolate*                  isolate,
                                         const v8::Local<v8::Object>   target,
                                         const std::string&            key,
                                         const v8::Local<v8::Function> listener) {
    Disposer disposer;
    disposer.kind = Disposer::Kind::RemoveListener;
    disposer.target.Reset(isolate, target);
    disposer.key = key;
    disposer.listener.Reset(isolate, listener);
    binding->record->disposers.push_back(std::move(disposer));
}

void CloverContext::appOn(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args, [&] {
        const auto* binding = liveBinding(args);
        if (binding == nullptr || args.Length() < 2 || !args[1]->IsFunction() || binding->state->mApp.IsEmpty()) {
            return;
        }
        v8::Isolate* isolate = args.GetIsolate();
        const auto   context = isolate->GetCurrentContext();
        const auto   app     = binding->state->mApp.Get(isolate);
        invokeMethod(context, app, "on", {args[0], args[1]});
        pushListenerDisposer(binding, isolate, app, toUtf8(isolate, args[0]), args[1].As<v8::Function>());
    });
}

void CloverContext::ipcOn(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args, [&] {
        const auto* binding = liveBinding(args);
        if (binding == nullptr || args.Length() < 2 || !args[1]->IsFunction() || binding->state->mIpcMain.IsEmpty()) {
            return;
        }
        v8::Isolate* isolate = args.GetIsolate();
        const auto   context = isolate->GetCurrentContext();
        const auto   ipc     = binding->state->mIpcMain.Get(isolate);
        invokeMethod(context, ipc, "on", {args[0], args[1]});
        pushListenerDisposer(binding, isolate, ipc, toUtf8(isolate, args[0]), args[1].As<v8::Function>());
    });
}

void CloverContext::ipcHandle(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args, [&] {
        const auto* binding = liveBinding(args);
        if (binding == nullptr || args.Length() < 2 || !args[1]->IsFunction() || binding->state->mIpcMain.IsEmpty()) {
            return;
        }
        v8::Isolate* isolate = args.GetIsolate();
        const auto   context = isolate->GetCurrentContext();
        const auto   ipc     = binding->state->mIpcMain.Get(isolate);
        invokeMethod(context, ipc, "handle", {args[0], args[1]});
        Disposer disposer;
        disposer.kind = Disposer::Kind::RemoveHandler;
        disposer.target.Reset(isolate, ipc);
        disposer.key = toUtf8(isolate, args[0]);
        binding->record->disposers.push_back(std::move(disposer));
    });
}

void CloverContext::onHeadersReceived(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args, [&] {
        const auto* binding = liveBinding(args);
        if (binding == nullptr || args.Length() < 2 || !args[0]->IsObject() || !args[1]->IsFunction()) {
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
        invokeMethod(context, webRequest, "onHeadersReceived", {args[1]});
        Disposer disposer;
        disposer.kind = Disposer::Kind::HeadersReceivedNull;
        disposer.target.Reset(isolate, webRequest);
        binding->record->disposers.push_back(std::move(disposer));
    });
}

void CloverContext::setTimer(const v8::FunctionCallbackInfo<v8::Value>& args, const bool interval) {
    guardedCallback(args, [&] {
        const auto* binding = liveBinding(args);
        if (binding == nullptr) {
            return;
        }
        v8::Isolate*               isolate = args.GetIsolate();
        const auto                 context = isolate->GetCurrentContext();
        const v8::Local<v8::Value> fnVal = getProp(context, context->Global(), interval ? "setInterval" : "setTimeout");
        if (fnVal.IsEmpty() || !fnVal->IsFunction()) {
            return;
        }
        std::vector<v8::Local<v8::Value>> argv;
        argv.reserve(static_cast<std::size_t>(args.Length()));
        for (int i = 0; i < args.Length(); ++i) {
            argv.push_back(args[i]);
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
        disposer.kind     = Disposer::Kind::ClearTimer;
        disposer.interval = interval;
        disposer.timerId.Reset(isolate, idValue);
        binding->record->disposers.push_back(std::move(disposer));
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
        Disposer disposer;
        disposer.kind = Disposer::Kind::CallFn;
        disposer.fn.Reset(args.GetIsolate(), args[0].As<v8::Function>());
        binding->record->disposers.push_back(std::move(disposer));
    });
}

auto CloverContext::create(JsRuntime::State& state, const v8::Local<v8::Context> context, JsPluginRecord& record)
        -> v8::Local<v8::Object> {
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    const auto   ctxObj  = v8::Object::New(isolate);

    setProp(context, ctxObj, "name", v8Str(isolate, record.name));
    setProp(context, ctxObj, "version", v8Str(isolate, "0.0.1"));
    setProp(context, ctxObj, "coreDir", v8Str(isolate, state.mCoreDir.string()));
    (void) ctxObj->SetNativeDataProperty(
            context, v8Str(isolate, "plugins"), &pluginsGetter, nullptr, v8::External::New(isolate, &state));
    setProp(context, ctxObj, "logger", Logger::create(state, context, record.name));
    if (!state.mElectron.IsEmpty()) {
        setProp(context, ctxObj, "electron", state.mElectron.Get(isolate));
    }

    auto binding    = std::make_unique<PluginBinding>();
    binding->state  = &state;
    binding->record = &record;
    binding->alive.store(true);
    auto* bindingPtr = binding.get();
    state.mBindings.push_back(std::move(binding));
    const auto bext = v8::External::New(isolate, bindingPtr);

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

void CloverContext::runDisposers(const v8::Local<v8::Context> context,
                                 JsPluginRecord&              record,
                                 std::vector<std::string>&    errors) {
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    for (auto& [kind, target, key, listener, timerId, interval, fn]: std::views::reverse(record.disposers)) {
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
        case Disposer::Kind::HeadersReceivedNull:
            if (!target.IsEmpty()) {
                invokeMethod(context, target.Get(isolate), "onHeadersReceived", {v8::Null(isolate)});
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
            errors.push_back(describeException(isolate, context, tryCatch));
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

    v8::HandleScope    handleScope(isolate);
    const auto         context = state.mContext.Get(isolate);
    v8::Context::Scope contextScope(context);

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
            const std::string err =
                    tryCatch.HasCaught() ? describeException(isolate, context, tryCatch) : "module require failed";
            rollback();
            return jsError("failed to load '" + name + "': " + err, CommonErrorCode::CallbackFailed);
        }
        if (!state.mModuleCache.IsEmpty()) {
            record->cacheKeys = newKeys(objectOwnKeys(context, state.mModuleCache.Get(isolate)), before);
        }

        const auto ctxObj = CloverContext::create(state, context, *record);
        if (exports->IsFunction()) {
            v8::Local<v8::Value> ctxArgs[] = {ctxObj};
            v8::Local<v8::Value> disposeVal;
            const bool           called =
                    exports.As<v8::Function>()->Call(context, context->Global(), 1, ctxArgs).ToLocal(&disposeVal);
            if (!called) {
                const std::string err =
                        tryCatch.HasCaught() ? describeException(isolate, context, tryCatch) : "plugin entry threw";
                std::vector<std::string> disposeErrors;
                CloverContext::runDisposers(context, *record, disposeErrors);
                CloverContext::invalidateBindings(state, record.get());
                rollback();
                return jsError("failed to load '" + name + "': " + err, CommonErrorCode::CallbackFailed);
            }
            if (disposeVal->IsFunction()) {
                Disposer disposer;
                disposer.kind = Disposer::Kind::CallFn;
                disposer.fn.Reset(isolate, disposeVal.As<v8::Function>());
                record->disposers.push_back(std::move(disposer));
            }
        } else if (exports->IsObject()) {
            const auto object = exports.As<v8::Object>();
            if (const auto onLoad = getProp(context, object, "onLoad"); !onLoad.IsEmpty() && onLoad->IsFunction()) {
                v8::Local<v8::Value> ctxArgs[] = {ctxObj};
                if (onLoad.As<v8::Function>()->Call(context, object, 1, ctxArgs).IsEmpty()) {
                    const std::string        err = tryCatch.HasCaught() ? describeException(isolate, context, tryCatch)
                                                                        : "plugin onLoad threw";
                    std::vector<std::string> disposeErrors;
                    CloverContext::runDisposers(context, *record, disposeErrors);
                    CloverContext::invalidateBindings(state, record.get());
                    rollback();
                    return jsError("failed to load '" + name + "': " + err, CommonErrorCode::CallbackFailed);
                }
                if (const auto onUnload = getProp(context, object, "onUnload");
                    !onUnload.IsEmpty() && onUnload->IsFunction()) {
                    record->onUnload.Reset(isolate, onUnload.As<v8::Function>());
                    record->onUnloadThis.Reset(isolate, object);
                }
            }
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

    v8::HandleScope    handleScope(isolate);
    const auto         context = state.mContext.Get(isolate);
    v8::Context::Scope contextScope(context);

    std::vector<std::string> errors;
    if (!record->onUnload.IsEmpty()) {
        const v8::TryCatch         tryCatch(isolate);
        const v8::Local<v8::Value> receiver = record->onUnloadThis.IsEmpty() ? v8::Local<v8::Value>(context->Global())
                                                                             : record->onUnloadThis.Get(isolate);
        (void) record->onUnload.Get(isolate)->Call(context, receiver, 0, nullptr);
        if (tryCatch.HasCaught()) {
            errors.push_back("onUnload: " + describeException(isolate, context, tryCatch));
        }
    }

    CloverContext::runDisposers(context, *record, errors);

    if (record->preloadPath) {
        for (auto& session: state.mSessions) {
            JsRuntime::State::unregisterPreloadOn(context, session.Get(isolate), record->preloadId);
        }
    }

    CloverContext::invalidateBindings(state, record.get());
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
