#include <CloverNT/API/Plugin/JsPlugin.hpp>
#include <CloverNT/Core/Modules/Manager.hpp>
#include <CloverNT/Core/Modules/PluginRuntime.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/Binding.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/Electron.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/Lifecycle.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace CloverNT::Core::Modules::bridge {

namespace {

/** The origin of QQ's own pages; framework/src/shared/trust.ts holds the same value. */
constexpr std::string_view kTrustedOrigin = "app://.";

constexpr std::size_t kMaxLogScopeBytes   = 64;
constexpr std::size_t kMaxLogMessageBytes = 8 * 1024;

/** Whether an IPC event comes from one of QQ's own pages; a frame that navigated away or closed does not. */
bool isTrustedSender(const v8::Local<v8::Context> context, const v8::Local<v8::Value> event) {
    if (event.IsEmpty() || !event->IsObject()) {
        return false;
    }
    const auto frame = getProp(context, event.As<v8::Object>(), "senderFrame");
    if (frame.IsEmpty() || !frame->IsObject()) {
        return false;
    }
    const auto origin = getProp(context, frame.As<v8::Object>(), "origin");
    return !origin.IsEmpty() && origin->IsString() && toUtf8(v8::Isolate::GetCurrent(), origin) == kTrustedOrigin;
}

/** Text from a page for the log: no control characters, which could forge the log framing or lines, and at most `limit` bytes. */
std::string loggable(std::string text, const std::size_t limit) {
    std::erase_if(text, [](const unsigned char c) { return c < 0x20 || c == 0x7f; });
    if (text.size() <= limit) {
        return text;
    }
    text.resize(limit);
    // Drop a UTF-8 sequence that the cut left incomplete.
    std::size_t lead = text.size();
    while (lead > 0 && (static_cast<unsigned char>(text[lead - 1]) & 0xC0) == 0x80) {
        --lead;
    }
    if (lead > 0) {
        const auto        byte   = static_cast<unsigned char>(text[lead - 1]);
        const std::size_t length = byte < 0x80 ? 1 : byte >= 0xF0 ? 4 : byte >= 0xE0 ? 3 : 2;
        if (text.size() - (lead - 1) < length) {
            text.resize(lead - 1);
        }
    }
    return text;
}

} // namespace

void ElectronRuntime::installDetection(JsRuntime::State& state) {
    v8::Isolate* isolate = state.mIsolate;
    if (isolate == nullptr || state.mRequire.IsEmpty()) {
        return;
    }
    v8::HandleScope    handleScope(isolate);
    const auto         context = state.mContext.Get(isolate);
    v8::Context::Scope contextScope(context);

    // module = require('module')
    v8::Local<v8::Value> moduleArgs[] = {v8::String::NewFromUtf8Literal(isolate, "module")};
    v8::Local<v8::Value> moduleValue;
    if (!state.mRequire.Get(isolate)->Call(context, context->Global(), 1, moduleArgs).ToLocal(&moduleValue) ||
        !moduleValue->IsObject()) {
        JsRuntime::State::logger().error("could not resolve 'module' builtin; electron detection not installed");
        return;
    }
    const auto module = moduleValue.As<v8::Object>();
    state.mModuleBuiltin.Reset(isolate, module);

    if (const auto cacheVal = getProp(context, module, "_cache"); !cacheVal.IsEmpty() && cacheVal->IsObject()) {
        state.mModuleCache.Reset(isolate, cacheVal.As<v8::Object>());
    }

    const auto loadVal = getProp(context, module, "_load");
    if (loadVal.IsEmpty() || !loadVal->IsFunction()) {
        JsRuntime::State::logger().error("module._load unavailable; electron detection not installed");
        return;
    }
    state.mOriginalModuleLoad.Reset(isolate, loadVal.As<v8::Function>());

    const auto patched = makeFn(context, &ElectronRuntime::onModuleLoad, v8::External::New(isolate, &state));
    (void) module->Set(context, v8Str(isolate, "_load"), patched);
}

void ElectronRuntime::onModuleLoad(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args, [&] {
        auto* self = dataOf<JsRuntime::State>(args);
        if (self == nullptr || self->mOriginalModuleLoad.IsEmpty() || self->mModuleBuiltin.IsEmpty()) {
            return;
        }
        v8::Isolate* isolate = args.GetIsolate();
        const auto   context = isolate->GetCurrentContext();

        std::vector<v8::Local<v8::Value>> argv;
        argv.reserve(static_cast<std::size_t>(args.Length()));
        for (int i = 0; i < args.Length(); ++i) {
            argv.push_back(args[i]);
        }

        const auto           original      = self->mOriginalModuleLoad.Get(isolate);
        const auto           moduleBuiltin = self->mModuleBuiltin.Get(isolate);
        v8::Local<v8::Value> exported;
        if (!original->Call(context, moduleBuiltin, static_cast<int>(argv.size()), argv.empty() ? nullptr : argv.data())
                     .ToLocal(&exported)) {
            return; // original _load threw: leave the exception pending so it propagates unchanged
        }
        args.GetReturnValue().Set(exported);

        if (self->mElectronReady || args.Length() < 1 || !args[0]->IsString() || !exported->IsObject()) {
            return;
        }
        if (toUtf8(isolate, args[0]) != "electron") {
            return;
        }
        const auto electron = exported.As<v8::Object>();
        if (const auto appVal = getProp(context, electron, "app"); appVal.IsEmpty() || !appVal->IsObject()) {
            return; // electron not fully initialised yet; wait for a later require('electron')
        }
        (void) moduleBuiltin->Set(context, v8Str(isolate, "_load"), original); // restore the original loader
        onElectronReady(*self, electron);
    });
}

void ElectronRuntime::onElectronReady(JsRuntime::State&           state,
                                                         const v8::Local<v8::Object> electron) {
    v8::Isolate* isolate = state.mIsolate;
    if (isolate == nullptr || state.mElectronReady) {
        return;
    }
    state.mElectronReady = true;

    const auto context = state.mContext.Get(isolate);
    state.mElectron.Reset(isolate, electron);
    if (const auto appVal = getProp(context, electron, "app"); !appVal.IsEmpty() && appVal->IsObject()) {
        state.mApp.Reset(isolate, appVal.As<v8::Object>());
    }
    if (const auto ipcVal = getProp(context, electron, "ipcMain"); !ipcVal.IsEmpty() && ipcVal->IsObject()) {
        state.mIpcMain.Reset(isolate, ipcVal.As<v8::Object>());
    }

    const auto ext = v8::External::New(isolate, &state);

    if (!state.mIpcMain.IsEmpty()) {
        const auto ipc = state.mIpcMain.Get(isolate);
        invokeMethod(context,
                     ipc,
                     "on",
                     {v8Str(isolate, "clovernt:log"), makeFn(context, &ElectronRuntime::rendererLog, ext)});
        invokeMethod(
                context,
                ipc,
                "handle",
                {v8Str(isolate, "clovernt:renderer-entries"), makeFn(context, &ElectronRuntime::rendererEntries, ext)});
    }

    if (!state.mApp.IsEmpty()) {
        const auto app = state.mApp.Get(isolate);
        invokeMethod(context,
                     app,
                     "on",
                     {v8Str(isolate, "session-created"), makeFn(context, &ElectronRuntime::sessionCreated, ext)});
        // app.whenReady().then(whenReady) -> register defaultSession preloads
        if (const auto whenReadyVal = getProp(context, app, "whenReady");
            !whenReadyVal.IsEmpty() && whenReadyVal->IsFunction()) {
            v8::Local<v8::Value> promiseVal;
            if (whenReadyVal.As<v8::Function>()->Call(context, app, 0, nullptr).ToLocal(&promiseVal) &&
                promiseVal->IsPromise()) {
                invokeMethod(context,
                             promiseVal.As<v8::Object>(),
                             "then",
                             {makeFn(context, &ElectronRuntime::whenReady, ext)});
            }
        }
    }

    registerShutdownHook(state);

    if (std::error_code ec;
        std::filesystem::exists(state.mCoreDir / "framework" / "clovernt-main.js", ec) && !ec) {
        const auto         mainScript = state.mCoreDir / "framework" / "clovernt-main.js";
        const v8::TryCatch tryCatch(isolate);
        if (state.requireModule(context, mainScript.string()).IsEmpty()) {
            JsRuntime::State::logger().error("clovernt-main.js failed to load: {}",
                                             describeException(isolate, context, tryCatch));
        }
    }

    driveJsLoad(state);
}

void ElectronRuntime::rendererEntries(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args, [&] {
        auto*        state   = dataOf<JsRuntime::State>(args);
        v8::Isolate* isolate = args.GetIsolate();
        const auto   context = isolate->GetCurrentContext();
        const auto   array   = v8::Array::New(isolate);
        // Pages other than QQ's own get no renderer code, only the fonts the preload applies.
        if (state != nullptr && args.Length() > 0 && isTrustedSender(context, args[0])) {
            std::vector<std::pair<std::string, std::filesystem::path>> entries;
            {
                std::lock_guard lock(state->mMutex);
                for (auto const& [name, record]: state->mRecords) {
                    if (record && record->rendererPath) {
                        entries.emplace_back(name, *record->rendererPath);
                    }
                }
            }
            std::uint32_t index = 0;
            if (std::error_code ec;
                std::filesystem::exists(state->mCoreDir / "framework" / "clovernt-renderer.js", ec) && !ec) {
                const auto uiScript = state->mCoreDir / "framework" / "clovernt-renderer.js";
                const auto entry    = v8::Object::New(isolate);
                setProp(context, entry, "name", v8Str(isolate, "clovernt-ui"));
                setProp(context, entry, "source", v8Str(isolate, readFileUtf8(uiScript)));
                (void) array->Set(context, index++, entry);
            }
            for (auto const& [name, path]: entries) {
                const auto entry = v8::Object::New(isolate);
                setProp(context, entry, "name", v8Str(isolate, name));
                setProp(context, entry, "source", v8Str(isolate, readFileUtf8(path)));
                (void) array->Set(context, index++, entry);
            }
        }
        args.GetReturnValue().Set(array);
    });
}

void ElectronRuntime::rendererLog(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args, [&] {
        auto*        state   = dataOf<JsRuntime::State>(args);
        v8::Isolate* isolate = args.GetIsolate();
        const auto   context = isolate->GetCurrentContext();
        // An ipcMain.on listener gets (event, level, scope, message).
        if (state == nullptr || args.Length() < 4 || !isTrustedSender(context, args[0])) {
            return;
        }
        const int level = std::clamp(args[1]->Int32Value(context).FromMaybe(static_cast<int>(LogLevel::Info)),
                                     static_cast<int>(LogLevel::Debug),
                                     static_cast<int>(LogLevel::Fatal));
        const auto scope   = loggable(toUtf8(isolate, args[2]), kMaxLogScopeBytes);
        const auto message = loggable(toUtf8(isolate, args[3]), kMaxLogMessageBytes);
        state->resolveRendererLogger(scope.empty() ? "renderer" : scope)->log(static_cast<LogLevel>(level), message);
    });
}

void ElectronRuntime::sessionCreated(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args, [&] {
        if (auto* state = dataOf<JsRuntime::State>(args);
            state != nullptr && args.Length() >= 1 && args[0]->IsObject()) {
            onSessionCreated(*state, args[0].As<v8::Object>());
        }
    });
}

void ElectronRuntime::whenReady(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args, [&] {
        auto* state = dataOf<JsRuntime::State>(args);
        if (state == nullptr || state->mElectron.IsEmpty()) {
            return;
        }
        v8::Isolate* isolate    = args.GetIsolate();
        const auto   context    = isolate->GetCurrentContext();
        const auto   sessionVal = getProp(context, state->mElectron.Get(isolate), "session");
        if (sessionVal.IsEmpty() || !sessionVal->IsObject()) {
            return;
        }
        if (const auto defaultSession = getProp(context, sessionVal.As<v8::Object>(), "defaultSession");
            !defaultSession.IsEmpty() && defaultSession->IsObject()) {
            onSessionCreated(*state, defaultSession.As<v8::Object>());
        }
    });
}

void ElectronRuntime::onSessionCreated(JsRuntime::State&           state,
                                                          const v8::Local<v8::Object> session) {
    v8::Isolate* isolate = state.mIsolate;
    if (isolate == nullptr) {
        return;
    }
    const auto context = state.mContext.Get(isolate);
    for (auto& existing: state.mSessions) {
        if (existing.Get(isolate)->StrictEquals(session)) {
            return;
        }
    }
    state.mSessions.emplace_back();
    state.mSessions.back().Reset(isolate, session);

    JsRuntime::State::registerPreloadOn(
            context, session, "clovernt-framework", (state.mCoreDir / "framework" / "clovernt-preload.js").string());

    std::vector<std::pair<std::string, std::string>> preloads;
    {
        std::lock_guard lock(state.mMutex);
        for (const auto& record: state.mRecords | std::views::values) {
            if (record && record->preloadPath) {
                preloads.emplace_back(record->preloadId, record->preloadPath->string());
            }
        }
    }
    for (auto const& [id, path]: preloads) {
        JsRuntime::State::registerPreloadOn(context, session, id, path);
    }
}

void ElectronRuntime::onAppQuitEvent(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args, [&] {
        auto* self = dataOf<JsRuntime::State>(args);
        if (self == nullptr) {
            return;
        }
        {
            std::lock_guard lock(self->mMutex);
            if (self->mShutdownUnloadDone) {
                return;
            }
            self->mShutdownUnloadDone = true;
        }
        CloverContext::setShuttingDown(true);
        JsRuntime::State::logger().info("Unloading plugins...");
        auto runtime = Manager::getInstance().requireModule<PluginRuntime>();
        if (!runtime) {
            JsRuntime::State::logger().error("shutdown plugin runtime unavailable: {}", runtime.error().message);
            return;
        }
        if (auto result = runtime->get().unloadForProcessExit(); !result) {
            JsRuntime::State::logger().error("shutdown plugin unload reported errors: {}", result.error().message);
        }
    });
}

void ElectronRuntime::registerShutdownHook(JsRuntime::State& state) {
    v8::Isolate* isolate = nullptr;
    {
        std::lock_guard lock(state.mMutex);
        if (state.mShutdownHookRegistered || state.mIsolate == nullptr || state.mApp.IsEmpty()) {
            return;
        }
        isolate = state.mIsolate;
    }

    v8::HandleScope    handleScope(isolate);
    const auto         context = state.mContext.Get(isolate);
    v8::Context::Scope contextScope(context);
    const v8::TryCatch tryCatch(isolate);

    const auto           app = state.mApp.Get(isolate);
    v8::Local<v8::Value> onValue;
    if (!app->Get(context, v8::String::NewFromUtf8Literal(isolate, "on")).ToLocal(&onValue) || !onValue->IsFunction()) {
        JsRuntime::State::logger().error("electron.app.on unavailable, shutdown hook not installed");
        return;
    }

    v8::Local<v8::Function> listener;
    if (!v8::Function::New(context, &ElectronRuntime::onAppQuitEvent, v8::External::New(isolate, &state))
                 .ToLocal(&listener)) {
        return;
    }
    // The earlier before-quit and will-quit events can both be cancelled.
    v8::Local<v8::Value> onArgs[] = {v8::String::NewFromUtf8Literal(isolate, "quit"), listener};
    if (onValue.As<v8::Function>()->Call(context, app, 2, onArgs).IsEmpty()) {
        JsRuntime::State::logger().error("shutdown hook registration failed: {}",
                                         describeException(isolate, context, tryCatch));
        return;
    }

    {
        std::lock_guard lock(state.mMutex);
        state.mShutdownHookRegistered = true;
    }
}

void ElectronRuntime::driveJsLoad(JsRuntime::State& state) {
    std::vector<std::shared_ptr<Plugin::JsPlugin>> snapshot;
    {
        std::lock_guard lock(state.mMutex);
        snapshot = state.mQueue;
    }
    if (!snapshot.empty() && state.mCallbacks.onLoadStarting) {
        state.mCallbacks.onLoadStarting();
    }
    for (auto const& plugin: snapshot) {
        if (!plugin) {
            continue;
        }
        auto result = PluginLifecycle::load(state, *plugin);
        if (result.has_value() && (plugin->preloadEntry() || plugin->rendererEntry())) {
            state.reloadAllWindows();
        }
        if (state.mCallbacks.onPluginResult) {
            state.mCallbacks.onPluginResult(plugin->name(),
                                            result.has_value(),
                                            result ? std::string_view{} : std::string_view{result.error().message});
        }
    }
    if (state.mCallbacks.onPluginsDone) {
        state.mCallbacks.onPluginsDone();
    }
}

} // namespace CloverNT::Core::Modules::bridge
