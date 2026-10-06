#pragma once
#include <CloverNT/API/Logger.hpp>
#include <CloverNT/API/Plugin/PluginManager.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/JsRuntime.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <QQNT/v8-cppgc.h>

#include <QQNT/node.h>
#include <QQNT/uv.h>

namespace CloverNT::Plugin {
class JsPlugin;
} // namespace CloverNT::Plugin

namespace CloverNT::Event {
class NamedEvent;
} // namespace CloverNT::Event

namespace CloverNT::Core::Modules::bridge {
struct JsEventChannel;
struct PendingEvent;
} // namespace CloverNT::Core::Modules::bridge

namespace CloverNT::Core::Modules {

struct LoggerHandle {
    std::shared_ptr<Logger> logger;
};

struct JsRuntime::State {
    enum class LifecycleOp { Load, Unload, Reload };

    explicit State(Callbacks callbacks);
    ~State();

    State(State const&)            = delete;
    State& operator=(State const&) = delete;

    [[nodiscard]] bool isAttached() const;
    void               attach(node::Environment*     env,
                              v8::Isolate*           isolate,
                              v8::Local<v8::Context> context,
                              v8::Local<v8::Value>   require,
                              std::filesystem::path  coreDir);
    void               detach();

    [[nodiscard]] auto enqueue(std::shared_ptr<Plugin::JsPlugin> plugin) -> Expected<void>;
    void               forget(std::string_view name);
    [[nodiscard]] auto loadPlugin(std::string_view name) -> Expected<void>;
    [[nodiscard]] auto unloadPlugin(std::string_view name) -> Expected<void>;
    [[nodiscard]] auto reloadPlugin(std::string_view name) -> Expected<void>;

    void               detachState();
    [[nodiscard]] auto callHandler(const v8::Global<v8::Function>& handler, std::string_view name) const
            -> Expected<void>;
    [[nodiscard]] auto dispatchLifecycle(LifecycleOp op, std::string_view name) -> Expected<void>;
    [[nodiscard]] auto runLifecycle(LifecycleOp op, std::string_view name) -> Expected<void>;
    static void        onLifecycleAsync(uv_async_t* handle);
    void               drainLifecycleQueue(const Expected<void>& reason);

    static void onEventAsync(uv_async_t* handle);
    void        dispatchEventToJs(const bridge::PendingEvent& pending);
    void        invokeEventCallback(std::uint64_t subId, v8::Local<v8::Value> payload, Event::NamedEvent* cancellable);
    void        clearEventSubscriptions();
    bool        removeEventSubscription(std::uint64_t subId);
    void        clearEventSubscriptionsForOwner(std::string_view ownerName);

    void               registerLoaderCallbacks(std::string id, v8::Local<v8::Object> config);
    void               unregisterLoaderCallbacks(std::string_view id);
    [[nodiscard]] auto loaderLifecycle(std::string_view id, LifecycleOp op, std::string_view name) const
            -> Expected<void>;

    void               runEventLoopOnce() const;
    [[nodiscard]] auto requireModule(v8::Local<v8::Context> context, const std::string& path) const
            -> v8::MaybeLocal<v8::Value>;
    static auto        logger() -> Logger&;
    [[nodiscard]] auto loggerHandle(const std::string& scope) -> LoggerHandle&;
    [[nodiscard]] auto resolveLogger(const std::string& scope) -> std::shared_ptr<Logger>;

    [[nodiscard]] auto buildPluginInfoArray(v8::Local<v8::Context> context) const -> v8::Local<v8::Value>;
    void               tickCallback();
    static void        registerPreloadOn(v8::Local<v8::Context> context,
                                         v8::Local<v8::Object>  session,
                                         const std::string&     id,
                                         const std::string&     filePath);
    static void
         unregisterPreloadOn(v8::Local<v8::Context> context, v8::Local<v8::Object> session, const std::string& id);
    void reloadAllWindows() const;

    Callbacks                                      mCallbacks;
    mutable std::recursive_mutex                   mMutex;
    node::Environment*                             mEnv{};
    v8::Isolate*                                   mIsolate{};
    v8::Global<v8::Context>                        mContext;
    v8::Global<v8::Function>                       mRequire;
    std::filesystem::path                          mCoreDir;
    std::vector<std::shared_ptr<Plugin::JsPlugin>> mQueue;
    bool                                           mAttached{};
    bool                                           mDetaching{};
    mutable bool                                   mShutdownHookRegistered{};
    bool                                           mShutdownUnloadDone{};

    v8::Global<v8::Object>              mElectron;
    v8::Global<v8::Object>              mApp;
    v8::Global<v8::Object>              mIpcMain;
    v8::Global<v8::Object>              mModuleBuiltin; // require('module')
    v8::Global<v8::Object>              mModuleCache;   // Module._cache
    v8::Global<v8::Function>            mOriginalModuleLoad;
    v8::Global<v8::Function>            mProcessTickCallback;
    bool                                mElectronReady{};
    std::vector<v8::Global<v8::Object>> mSessions;

    struct Disposer {
        enum class Kind { RemoveListener, RemoveHandler, HeadersReceivedNull, ClearTimer, CallFn };
        Kind                     kind{};
        v8::Global<v8::Object>   target;   // app / ipcMain / session.webRequest
        std::string              key;      // event name or channel
        v8::Global<v8::Function> listener; // RemoveListener
        v8::Global<v8::Value>    timerId;  // ClearTimer
        bool                     interval{};
        v8::Global<v8::Function> fn; // CallFn (onUnload / returned dispose)
    };
    struct JsPluginRecord {
        std::string                          name;
        std::optional<std::filesystem::path> mainPath;
        std::optional<std::filesystem::path> preloadPath;
        std::optional<std::filesystem::path> rendererPath;
        std::string                          preloadId; // "clovernt:<name>"
        std::vector<std::string>             cacheKeys;
        std::vector<Disposer>                disposers;    // run LIFO on unload
        v8::Global<v8::Function>             onUnload;     // object-form {onLoad,onUnload}
        v8::Global<v8::Value>                onUnloadThis; // receiver for onUnload
    };
    std::map<std::string, std::unique_ptr<JsPluginRecord>, std::less<>> mRecords;

    struct PluginBinding {
        State*            state{};
        JsPluginRecord*   record{};
        std::atomic<bool> alive{true};
    };
    std::deque<std::unique_ptr<PluginBinding>> mBindings;

    struct LoaderCallbacks {
        v8::Global<v8::Function> load;
        v8::Global<v8::Function> unload;
        v8::Global<v8::Function> reload;
    };
    std::map<std::string, LoaderCallbacks, std::less<>> mLoaders;

    uv_thread_t mMainThread{};
    uv_async_t* mLifecycleAsync{};
    bool        mAsyncInited{};

    struct PendingOp {
        LifecycleOp                  kind;
        std::string                  name;
        std::promise<Expected<void>> promise;
    };
    std::mutex             mLifecycleQueueMutex;
    std::vector<PendingOp> mLifecycleQueue;

    uv_async_t*                             mEventAsync{};
    bool                                    mEventAsyncInited{};
    std::shared_ptr<bridge::JsEventChannel> mEventChannel;

    struct JsSubscription {
        v8::Global<v8::Function> jsFn;
        std::string              jsName;
        bool                     once{};
        std::string              ownerName;
    };
    std::map<std::uint64_t, JsSubscription> mEventSubs; // keyed by the listener's Event::ListenerId

    std::mutex                                    mLoggerMutex;
    std::unordered_map<std::string, LoggerHandle> mLoggers;
};

} // namespace CloverNT::Core::Modules
