#include <CloverNT/API/Reflect/Class.hpp>
#include <CloverNT/API/Reflect/Marshal.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/ReflectBridge.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/Support.hpp>

#include <Alka/Js/Value.hpp>
#include <Alka/Runtime/Dispatcher.hpp>

#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace CloverNT::Core::Modules::bridge {

namespace {

    constexpr std::int64_t  kMaxSafeJsInt   = 9007199254740991LL;  // 2^53 - 1
    constexpr std::int64_t  kMinSafeJsInt   = -9007199254740991LL; // -(2^53 - 1)
    constexpr std::uint64_t kMaxSafeJsUint  = 9007199254740991ULL;
    constexpr std::size_t   kMaxBridgeDepth = 64;

    constexpr std::uint64_t kJsCallableTag = 0x434C5652'4A53464EULL; // "CLVRJSFN"
    constexpr std::uint64_t kJsRemoteTag   = 0x434C5652'4A534F42ULL; // "CLVRJSOB"

    [[nodiscard]] auto wrapperPrivateKey(v8::Isolate* isolate) -> v8::Local<v8::Private> {
        return v8::Private::ForApi(isolate, v8Str(isolate, "clovernt::reflect::native_wrapper"));
    }

    [[nodiscard]] auto callablePrivateKey(v8::Isolate* isolate) -> v8::Local<v8::Private> {
        return v8::Private::ForApi(isolate, v8Str(isolate, "clovernt::reflect::cpp_callable"));
    }

    struct IdentityCacheKey {
        const Reflect::ClassDescriptor* descriptor{};
        const void*                     identity{};
        bool                            isConst{};

        bool operator==(const IdentityCacheKey&) const noexcept = default;
    };

    struct IdentityCacheKeyHash {
        auto operator()(const IdentityCacheKey& k) const noexcept -> std::size_t {
            const auto h1 = std::hash<const void*>{}(k.descriptor);
            const auto h2 = std::hash<const void*>{}(k.identity);
            return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2)) ^ (k.isConst ? 1ULL : 0ULL);
        }
    };

    struct IsolateReflectState;

    struct NativeWrapperRecord {
        v8::Isolate*                                  isolate{};
        std::shared_ptr<Reflect::RevocableObjectCell> sharedCell;
        std::optional<Reflect::BorrowedObjectHandle>  borrowed;
        IdentityCacheKey                              cacheKey{};
        bool                                          inCache{false};
        v8::Global<v8::Object>                        weakSelf;
    };

    struct CppCallableRecord {
        v8::Isolate*             isolate{};
        Reflect::BridgeCallable  callable;
        v8::Global<v8::Function> weakFn;
    };

    struct PromiseReactionData {
        Reflect::BridgeAsync state;
        bool                 isReject{false};
    };

    struct IsolateReflectState {
        v8::Global<v8::Object>                                                                internalCtorSentinel;
        std::unordered_map<const Reflect::ClassDescriptor*, v8::Global<v8::FunctionTemplate>> templates;
        std::unordered_map<IdentityCacheKey, NativeWrapperRecord*, IdentityCacheKeyHash>      identityCache;
        std::vector<std::unique_ptr<NativeWrapperRecord>>                                     wrappers;
        std::vector<std::unique_ptr<CppCallableRecord>>                                       callables;
    };

    std::mutex                                                             gIsolateMapMutex;
    std::unordered_map<v8::Isolate*, std::unique_ptr<IsolateReflectState>> gIsolateStates;

    [[nodiscard]] auto stateForIsolate(v8::Isolate* isolate) -> IsolateReflectState& {
        std::lock_guard lock(gIsolateMapMutex);
        auto&           ptr = gIsolateStates[isolate];
        if (!ptr) {
            ptr = std::make_unique<IsolateReflectState>();
            const v8::HandleScope scope(isolate);
            ptr->internalCtorSentinel.Reset(isolate, v8::Object::New(isolate));
        }
        return *ptr;
    }

    void detachWrapperRecord(v8::Isolate* isolate, NativeWrapperRecord* target) noexcept {
        std::unique_ptr<NativeWrapperRecord> owned;
        {
            std::lock_guard lock(gIsolateMapMutex);
            const auto      it = gIsolateStates.find(isolate);
            if (it == gIsolateStates.end() || !it->second) {
                return;
            }
            auto& st = *it->second;
            if (target->inCache) {
                if (const auto cacheIt = st.identityCache.find(target->cacheKey);
                    cacheIt != st.identityCache.end() && cacheIt->second == target) {
                    st.identityCache.erase(cacheIt);
                }
                target->inCache = false;
            }
            for (auto vecIt = st.wrappers.begin(); vecIt != st.wrappers.end(); ++vecIt) {
                if (vecIt->get() == target) {
                    owned = std::move(*vecIt);
                    st.wrappers.erase(vecIt);
                    break;
                }
            }
        }
        if (owned) {
            owned->weakSelf.Reset();
        }
    }

    void detachCallableRecord(v8::Isolate* isolate, const CppCallableRecord* target) noexcept {
        std::unique_ptr<CppCallableRecord> owned;
        {
            std::lock_guard lock(gIsolateMapMutex);
            const auto      it = gIsolateStates.find(isolate);
            if (it == gIsolateStates.end() || !it->second) {
                return;
            }
            auto& st = *it->second;
            for (auto vecIt = st.callables.begin(); vecIt != st.callables.end(); ++vecIt) {
                if (vecIt->get() == target) {
                    owned = std::move(*vecIt);
                    st.callables.erase(vecIt);
                    break;
                }
            }
        }
        if (owned) {
            owned->weakFn.Reset();
        }
    }

    void onWrapperWeakCallback(const v8::WeakCallbackInfo<NativeWrapperRecord>& data) {
        if (auto* rec = data.GetParameter(); rec != nullptr) {
            detachWrapperRecord(data.GetIsolate(), rec);
        }
    }

    void onCallableWeakCallback(const v8::WeakCallbackInfo<CppCallableRecord>& data) {
        if (auto* rec = data.GetParameter(); rec != nullptr) {
            detachCallableRecord(data.GetIsolate(), rec);
        }
    }

    [[nodiscard]] auto unwrapNativeRecord(v8::Isolate*                 isolate,
                                          const v8::Local<v8::Context> context,
                                          const v8::Local<v8::Object>  obj) -> NativeWrapperRecord* {
        const auto privKey = wrapperPrivateKey(isolate);
        bool       hasPriv = false;
        if (!obj->HasPrivate(context, privKey).To(&hasPriv) || !hasPriv) {
            return nullptr;
        }
        v8::Local<v8::Value> extVal;
        if (!obj->GetPrivate(context, privKey).ToLocal(&extVal) || !extVal->IsExternal()) {
            return nullptr;
        }
        return static_cast<NativeWrapperRecord*>(extVal.As<v8::External>()->Value());
    }

    struct PinnedNativeTarget {
        Reflect::PluginActivity::Guard  guard;
        std::shared_ptr<void>           keepAlive;
        void*                           rawPtr{};
        const Reflect::ClassDescriptor* descriptor{};
        bool                            isConst{false};
    };

    [[nodiscard]] auto pinWrapperRecord(const NativeWrapperRecord* rec) -> Expected<PinnedNativeTarget> {
        if (rec == nullptr) {
            return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::TypeMismatch,
                                             "invalid object receiver ('this')");
        }
        if (rec->sharedCell) {
            auto pinned = rec->sharedCell->pin();
            if (!pinned) {
                return unexpected(std::move(pinned).error());
            }
            const auto* desc = rec->sharedCell->descriptor();
            if (desc == nullptr) {
                return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::ObjectRevoked,
                                                 "object '" + std::string(rec->sharedCell->typeName()) +
                                                         "' is no longer available");
            }
            return PinnedNativeTarget{
                    .guard      = std::move(pinned->guard),
                    .keepAlive  = std::move(pinned->instance),
                    .rawPtr     = pinned->rawPtr,
                    .descriptor = desc,
                    .isConst    = rec->sharedCell->isConst(),
            };
        }
        if (rec->borrowed.has_value()) {
            if (!rec->borrowed->alive() || rec->borrowed->descriptor == nullptr) {
                return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::BorrowExpired,
                                                 "borrowed object has expired");
            }
            return PinnedNativeTarget{
                    .guard      = {},
                    .keepAlive  = {},
                    .rawPtr     = rec->borrowed->rawPtr,
                    .descriptor = rec->borrowed->descriptor,
                    .isConst    = rec->borrowed->isConst,
            };
        }
        return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::ObjectRevoked, "object is no longer available");
    }

    [[nodiscard]] auto findMethodAlongHierarchy(const Reflect::ClassDescriptor* desc,
                                                const std::string_view          methodName,
                                                void*& inOutPtr) -> const Reflect::MethodDescriptor* {
        const auto* cur = desc;
        while (cur != nullptr) {
            for (const auto& m: cur->methods) {
                if (m.name == methodName) {
                    inOutPtr = desc->castTo(inOutPtr, cur->typeId);
                    return inOutPtr != nullptr ? &m : nullptr;
                }
            }
            cur = cur->base;
        }
        return nullptr;
    }

    [[nodiscard]] auto findPropertyAlongHierarchy(const Reflect::ClassDescriptor* desc,
                                                  const std::string_view          propName,
                                                  void*& inOutPtr) -> const Reflect::PropertyDescriptor* {
        const auto* cur = desc;
        while (cur != nullptr) {
            for (const auto& p: cur->properties) {
                if (p.name == propName) {
                    inOutPtr = desc->castTo(inOutPtr, cur->typeId);
                    return inOutPtr != nullptr ? &p : nullptr;
                }
            }
            cur = cur->base;
        }
        return nullptr;
    }

    auto fromV8Impl(v8::Isolate*                        isolate,
                    v8::Local<v8::Context>              context,
                    v8::Local<v8::Value>                value,
                    Reflect::BorrowLease*               activeLease,
                    std::vector<v8::Local<v8::Object>>& seen,
                    std::size_t                         depth) -> Expected<Reflect::BridgeValue>;

    void throwBridgeError(v8::Isolate* isolate, const Error& err) {
        const auto msg  = normalizeErrorMessage(err.message);
        const auto text = msg.empty() ? std::string_view{"unknown error"} : std::string_view{msg};
        if (err.code == static_cast<std::int32_t>(Reflect::BridgeErrorCode::TypeMismatch) ||
            err.code == static_cast<std::int32_t>(Reflect::BridgeErrorCode::ReadonlyViolation)) {
            isolate->ThrowException(v8::Exception::TypeError(v8Str(isolate, text)));
            return;
        }
        if (err.code == static_cast<std::int32_t>(Reflect::BridgeErrorCode::IntegerOverflow) ||
            err.code == static_cast<std::int32_t>(Reflect::BridgeErrorCode::EnumOutOfRange)) {
            isolate->ThrowException(v8::Exception::RangeError(v8Str(isolate, text)));
            return;
        }
        isolate->ThrowException(v8::Exception::Error(v8Str(isolate, text)));
    }

    class ScopedJsValue final : public Alka::Value {
    public:
        ScopedJsValue() = default;

        ScopedJsValue(Alka::ConvertCtx& cx, const v8::Local<v8::Value> val) : Alka::Value(cx, val) {}

        [[nodiscard]] static auto make(v8::Isolate*                 isolate,
                                       const v8::Local<v8::Context> context,
                                       const v8::Local<v8::Value>   val) -> ScopedJsValue {
            Alka::ConvertCtx cx{isolate, context};
            return ScopedJsValue{cx, val};
        }

        using Value::enter;
    };

    class JsBridgeCallable final : public Reflect::BridgeCallableImpl {
    public:
        JsBridgeCallable(v8::Isolate* isolate, const v8::Local<v8::Context> context, const v8::Local<v8::Function> fn)
            : mFn(ScopedJsValue::make(isolate, context, fn)),
              mDispatcher(isolate),
              mOwnerPlugin(Reflect::currentOwnerPluginName()) {}

        [[nodiscard]] auto ownerPlugin() const noexcept -> std::string_view override {
            return mOwnerPlugin;
        }

        [[nodiscard]] bool alive() const noexcept override {
            return mFn.valid() && mDispatcher.alive();
        }

        [[nodiscard]] bool canInvokeSyncHere() const noexcept override {
            return mDispatcher.link() && mDispatcher.link()->onIsolateThread();
        }

        [[nodiscard]] auto nativeTag() const noexcept -> std::uint64_t override {
            return kJsCallableTag;
        }

        [[nodiscard]] bool belongsTo(v8::Isolate* isolate) const noexcept {
            return mFn.valid() && mFn.belongsTo(isolate);
        }

        [[nodiscard]] auto localFunction(v8::Isolate* isolate) const -> v8::Local<v8::Function> {
            return mFn.local(isolate).As<v8::Function>();
        }

        [[nodiscard]] auto invoke(const std::span<const Reflect::BridgeValue> args)
                -> Expected<Reflect::BridgeValue> override {
            if (!alive()) {
                return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::ObjectRevoked,
                                                 "callback is no longer available");
            }
            if (!canInvokeSyncHere()) {
                return Reflect::bridgeUnexpected(
                        Reflect::BridgeErrorCode::CrossThreadSyncCallForbidden,
                        "cannot synchronously call a JavaScript function from a background thread");
            }
            auto res = mFn.enter<Expected<Reflect::BridgeValue>>(
                    [&](const Alka::ConvertCtx&    cx,
                        const v8::Local<v8::Value> fnVal) -> Alka::Result<Expected<Reflect::BridgeValue>> {
                        std::vector<v8::Local<v8::Value>> argv;
                        argv.reserve(args.size());
                        for (std::size_t i = 0; i < args.size(); ++i) {
                            auto jsArg = bridgeValueToV8(cx.mIsolate, cx.mContext, args[i]);
                            if (!jsArg) {
                                return Expected<Reflect::BridgeValue>{unexpected(Reflect::prependErrorPath(
                                        std::move(jsArg).error(), "arg[" + std::to_string(i) + "]"))};
                            }
                            argv.push_back(*jsArg);
                        }
                        const v8::TryCatch   tryCatch(cx.mIsolate);
                        v8::Local<v8::Value> jsRet;
                        if (!fnVal.As<v8::Function>()
                                     ->Call(cx.mContext,
                                            v8::Undefined(cx.mIsolate),
                                            static_cast<int>(argv.size()),
                                            argv.empty() ? nullptr : argv.data())
                                     .ToLocal(&jsRet)) {
                            return Expected<Reflect::BridgeValue>{Reflect::bridgeUnexpected(
                                    Reflect::BridgeErrorCode::ExecutionFailed,
                                    extractExceptionMessage(cx.mIsolate, cx.mContext, tryCatch))};
                        }
                        return bridgeValueFromV8(cx.mIsolate, cx.mContext, jsRet);
                    });
            if (!res) {
                return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::ExecutionFailed, res.error().mMessage);
            }
            return std::move(*res);
        }

        [[nodiscard]] auto post(std::vector<Reflect::BridgeValue> args) -> Expected<void> override {
            if (!alive()) {
                return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::ObjectRevoked,
                                                 "JavaScript runtime is no longer available");
            }
            if (canInvokeSyncHere()) {
                auto syncRes = invoke(args);
                if (!syncRes) {
                    return unexpected(std::move(syncRes).error());
                }
                return {};
            }
            const ScopedJsValue fnCopy = mFn;
            const bool          posted = mDispatcher.post([fnCopy, capturedArgs = std::move(args)]() mutable {
                (void) fnCopy.enter<void>(
                        [&](const Alka::ConvertCtx& cx, const v8::Local<v8::Value> fnVal) -> Alka::Result<void> {
                            std::vector<v8::Local<v8::Value>> argv;
                            argv.reserve(capturedArgs.size());
                            for (const auto& a: capturedArgs) {
                                auto jsArg = bridgeValueToV8(cx.mIsolate, cx.mContext, a);
                                if (!jsArg) {
                                    return {};
                                }
                                argv.push_back(*jsArg);
                            }
                            const v8::TryCatch tryCatch(cx.mIsolate);
                            (void) fnVal.As<v8::Function>()->Call(cx.mContext,
                                                                  v8::Undefined(cx.mIsolate),
                                                                  static_cast<int>(argv.size()),
                                                                  argv.empty() ? nullptr : argv.data());
                            return {};
                        });
            });
            if (!posted) {
                return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::ObjectRevoked,
                                                 "failed to schedule callback on JavaScript thread");
            }
            return {};
        }

        [[nodiscard]] auto invokeAsync(std::vector<Reflect::BridgeValue> args) -> Reflect::BridgeAsync override {
            auto asyncOut = std::make_shared<Reflect::BridgeAsyncState>();
            if (!alive()) {
                asyncOut->reject(Reflect::bridgeError(Reflect::BridgeErrorCode::ObjectRevoked,
                                                      "callback is no longer available"));
                return asyncOut;
            }
            auto runOnJs = [fnCopy = mFn, capturedArgs = std::move(args), asyncOut]() mutable {
                auto entered = fnCopy.enter<void>([&](Alka::ConvertCtx&          cx,
                                                      const v8::Local<v8::Value> fnVal) -> Alka::Result<void> {
                    std::vector<v8::Local<v8::Value>> argv;
                    argv.reserve(capturedArgs.size());
                    for (std::size_t i = 0; i < capturedArgs.size(); ++i) {
                        auto jsArg = bridgeValueToV8(cx.mIsolate, cx.mContext, capturedArgs[i]);
                        if (!jsArg) {
                            asyncOut->reject(Reflect::prependErrorPath(std::move(jsArg).error(),
                                                                       "arg[" + std::to_string(i) + "]"));
                            return {};
                        }
                        argv.push_back(*jsArg);
                    }
                    const v8::TryCatch   tryCatch(cx.mIsolate);
                    v8::Local<v8::Value> jsRet;
                    if (!fnVal.As<v8::Function>()
                                 ->Call(cx.mContext,
                                        v8::Undefined(cx.mIsolate),
                                        static_cast<int>(argv.size()),
                                        argv.empty() ? nullptr : argv.data())
                                 .ToLocal(&jsRet)) {
                        asyncOut->reject(
                                Reflect::bridgeError(Reflect::BridgeErrorCode::ExecutionFailed,
                                                     extractExceptionMessage(cx.mIsolate, cx.mContext, tryCatch)));
                        return {};
                    }
                    auto converted = bridgeValueFromV8(cx.mIsolate, cx.mContext, jsRet);
                    if (!converted) {
                        asyncOut->reject(std::move(converted).error());
                        return {};
                    }
                    if (auto* nestedPromise = std::get_if<Reflect::BridgeAsync>(&converted->data);
                        nestedPromise != nullptr && *nestedPromise) {
                        (*nestedPromise)->then([asyncOut](const Expected<Reflect::BridgeValue>& res) {
                            asyncOut->settle(res);
                        });
                    } else {
                        asyncOut->resolve(std::move(*converted));
                    }
                    return {};
                });
                if (!entered) {
                    asyncOut->reject(
                            Reflect::bridgeError(Reflect::BridgeErrorCode::ExecutionFailed, entered.error().mMessage));
                }
            };

            if (canInvokeSyncHere()) {
                runOnJs();
            } else if (!mDispatcher.post(std::move(runOnJs))) {
                asyncOut->reject(Reflect::bridgeError(Reflect::BridgeErrorCode::ObjectRevoked,
                                                      "JavaScript runtime is no longer available"));
            }
            return asyncOut;
        }

    private:
        ScopedJsValue    mFn;
        Alka::Dispatcher mDispatcher;
        std::string      mOwnerPlugin;
    };

    class JsRemoteObject final : public Reflect::RemoteObjectImpl {
    public:
        JsRemoteObject(v8::Isolate* isolate, const v8::Local<v8::Context> context, const v8::Local<v8::Object> obj)
            : mObj(ScopedJsValue::make(isolate, context, obj)), mDispatcher(isolate) {}

        [[nodiscard]] bool alive() const noexcept override {
            return mObj.valid() && mDispatcher.alive();
        }

        [[nodiscard]] bool canAccessSyncHere() const noexcept override {
            return mDispatcher.link() && mDispatcher.link()->onIsolateThread();
        }

        [[nodiscard]] auto nativeTag() const noexcept -> std::uint64_t override {
            return kJsRemoteTag;
        }

        [[nodiscard]] bool belongsTo(v8::Isolate* isolate) const noexcept {
            return mObj.valid() && mObj.belongsTo(isolate);
        }

        [[nodiscard]] auto localObject(v8::Isolate* isolate) const -> v8::Local<v8::Object> {
            return mObj.local(isolate).As<v8::Object>();
        }

        [[nodiscard]] auto hasProperty(const std::string_view key) -> Expected<bool> override {
            if (!alive()) {
                return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::ObjectRevoked,
                                                 "object is no longer available");
            }
            if (!canAccessSyncHere()) {
                return Reflect::bridgeUnexpected(
                        Reflect::BridgeErrorCode::CrossThreadSyncCallForbidden,
                        "cannot synchronously inspect JavaScript object from a background thread");
            }
            auto res = mObj.enter<Expected<bool>>([&](const Alka::ConvertCtx&    cx,
                                                      const v8::Local<v8::Value> val) -> Alka::Result<Expected<bool>> {
                const v8::TryCatch tryCatch(cx.mIsolate);
                bool               found = false;
                if (!val.As<v8::Object>()->Has(cx.mContext, v8Str(cx.mIsolate, key)).To(&found)) {
                    return Expected<bool>{
                            Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::ExecutionFailed,
                                                      extractExceptionMessage(cx.mIsolate, cx.mContext, tryCatch))};
                }
                return Expected<bool>{found};
            });
            if (!res) {
                return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::ExecutionFailed, res.error().mMessage);
            }
            return std::move(*res);
        }

        [[nodiscard]] auto getProperty(const std::string_view key) -> Expected<Reflect::BridgeValue> override {
            if (!alive()) {
                return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::ObjectRevoked,
                                                 "object is no longer available");
            }
            if (!canAccessSyncHere()) {
                return Reflect::bridgeUnexpected(
                        Reflect::BridgeErrorCode::CrossThreadSyncCallForbidden,
                        "cannot synchronously read JavaScript property from a background thread");
            }
            auto res = mObj.enter<Expected<Reflect::BridgeValue>>(
                    [&](const Alka::ConvertCtx&    cx,
                        const v8::Local<v8::Value> val) -> Alka::Result<Expected<Reflect::BridgeValue>> {
                        const v8::TryCatch   tryCatch(cx.mIsolate);
                        v8::Local<v8::Value> propVal;
                        if (!val.As<v8::Object>()->Get(cx.mContext, v8Str(cx.mIsolate, key)).ToLocal(&propVal)) {
                            return Expected<Reflect::BridgeValue>{Reflect::bridgeUnexpected(
                                    Reflect::BridgeErrorCode::ExecutionFailed,
                                    extractExceptionMessage(cx.mIsolate, cx.mContext, tryCatch))};
                        }
                        return bridgeValueFromV8(cx.mIsolate, cx.mContext, propVal);
                    });
            if (!res) {
                return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::ExecutionFailed, res.error().mMessage);
            }
            return std::move(*res);
        }

        [[nodiscard]] auto setProperty(const std::string_view key, Reflect::BridgeValue value)
                -> Expected<void> override {
            if (!alive()) {
                return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::ObjectRevoked,
                                                 "object is no longer available");
            }
            if (!canAccessSyncHere()) {
                return Reflect::bridgeUnexpected(
                        Reflect::BridgeErrorCode::CrossThreadSyncCallForbidden,
                        "cannot synchronously modify JavaScript property from a background thread");
            }
            auto res = mObj.enter<Expected<void>>(
                    [&](const Alka::ConvertCtx& cx, const v8::Local<v8::Value> val) -> Alka::Result<Expected<void>> {
                        auto jsVal = bridgeValueToV8(cx.mIsolate, cx.mContext, value);
                        if (!jsVal) {
                            return Expected<void>{unexpected(std::move(jsVal).error())};
                        }
                        const v8::TryCatch tryCatch(cx.mIsolate);
                        bool               ok = false;
                        if (!val.As<v8::Object>()->Set(cx.mContext, v8Str(cx.mIsolate, key), *jsVal).To(&ok) || !ok) {
                            return Expected<void>{Reflect::bridgeUnexpected(
                                    Reflect::BridgeErrorCode::ExecutionFailed,
                                    tryCatch.HasCaught() ? extractExceptionMessage(cx.mIsolate, cx.mContext, tryCatch)
                                                         : "failed to set property '" + std::string(key) + "'")};
                        }
                        return Expected<void>{};
                    });
            if (!res) {
                return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::ExecutionFailed, res.error().mMessage);
            }
            return std::move(*res);
        }

        [[nodiscard]] auto propertyNames() -> Expected<std::vector<std::string>> override {
            if (!alive()) {
                return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::ObjectRevoked,
                                                 "object is no longer available");
            }
            if (!canAccessSyncHere()) {
                return Reflect::bridgeUnexpected(
                        Reflect::BridgeErrorCode::CrossThreadSyncCallForbidden,
                        "cannot synchronously enumerate JavaScript properties from a background thread");
            }
            auto res = mObj.enter<Expected<std::vector<std::string>>>(
                    [&](const Alka::ConvertCtx&    cx,
                        const v8::Local<v8::Value> val) -> Alka::Result<Expected<std::vector<std::string>>> {
                        return Expected<std::vector<std::string>>{objectOwnKeys(cx.mContext, val.As<v8::Object>())};
                    });
            if (!res) {
                return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::ExecutionFailed, res.error().mMessage);
            }
            return std::move(*res);
        }

        [[nodiscard]] auto invokeMethod(const std::string_view method, const std::span<const Reflect::BridgeValue> args)
                -> Expected<Reflect::BridgeValue> override {
            if (!alive()) {
                return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::ObjectRevoked,
                                                 "object is no longer available");
            }
            if (!canAccessSyncHere()) {
                return Reflect::bridgeUnexpected(
                        Reflect::BridgeErrorCode::CrossThreadSyncCallForbidden,
                        "cannot synchronously call JavaScript method from a background thread");
            }
            auto res = mObj.enter<Expected<Reflect::BridgeValue>>(
                    [&](const Alka::ConvertCtx&    cx,
                        const v8::Local<v8::Value> val) -> Alka::Result<Expected<Reflect::BridgeValue>> {
                        const auto           obj = val.As<v8::Object>();
                        const v8::TryCatch   tryCatch(cx.mIsolate);
                        v8::Local<v8::Value> fnVal;
                        if (!obj->Get(cx.mContext, v8Str(cx.mIsolate, method)).ToLocal(&fnVal) ||
                            !fnVal->IsFunction()) {
                            return Expected<Reflect::BridgeValue>{Reflect::bridgeUnexpected(
                                    Reflect::BridgeErrorCode::MemberNotFound,
                                    "method '" + std::string(method) + "' not found on object")};
                        }
                        std::vector<v8::Local<v8::Value>> argv;
                        argv.reserve(args.size());
                        for (std::size_t i = 0; i < args.size(); ++i) {
                            auto jsArg = bridgeValueToV8(cx.mIsolate, cx.mContext, args[i]);
                            if (!jsArg) {
                                return Expected<Reflect::BridgeValue>{unexpected(Reflect::prependErrorPath(
                                        std::move(jsArg).error(), "arg[" + std::to_string(i) + "]"))};
                            }
                            argv.push_back(*jsArg);
                        }
                        v8::Local<v8::Value> jsRet;
                        if (!fnVal.As<v8::Function>()
                                     ->Call(cx.mContext,
                                            obj,
                                            static_cast<int>(argv.size()),
                                            argv.empty() ? nullptr : argv.data())
                                     .ToLocal(&jsRet)) {
                            return Expected<Reflect::BridgeValue>{Reflect::bridgeUnexpected(
                                    Reflect::BridgeErrorCode::ExecutionFailed,
                                    extractExceptionMessage(cx.mIsolate, cx.mContext, tryCatch))};
                        }
                        return bridgeValueFromV8(cx.mIsolate, cx.mContext, jsRet);
                    });
            if (!res) {
                return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::ExecutionFailed, res.error().mMessage);
            }
            return std::move(*res);
        }

        [[nodiscard]] auto postMethod(const std::string_view method, std::vector<Reflect::BridgeValue> args)
                -> Expected<void> override {
            if (canAccessSyncHere()) {
                if (auto syncRes = invokeMethod(method, args); !syncRes) {
                    return unexpected(std::move(syncRes).error());
                }
                return {};
            }
            if (!mDispatcher.alive()) {
                return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::ObjectRevoked,
                                                 "JavaScript runtime is no longer available");
            }
            std::string         methodStr{method};
            const ScopedJsValue objCopy = mObj;
            const bool          posted  = mDispatcher.post(
                    [objCopy, methodName = std::move(methodStr), capturedArgs = std::move(args)]() mutable {
                        (void) objCopy.enter<void>(
                                [&](const Alka::ConvertCtx& cx, const v8::Local<v8::Value> val) -> Alka::Result<void> {
                                    const auto           obj = val.As<v8::Object>();
                                    const v8::TryCatch   tryCatch(cx.mIsolate);
                                    v8::Local<v8::Value> fnVal;
                                    if (!obj->Get(cx.mContext, v8Str(cx.mIsolate, methodName)).ToLocal(&fnVal) ||
                                        !fnVal->IsFunction()) {
                                        return {};
                                    }
                                    std::vector<v8::Local<v8::Value>> argv;
                                    argv.reserve(capturedArgs.size());
                                    for (const auto& a: capturedArgs) {
                                        auto jsArg = bridgeValueToV8(cx.mIsolate, cx.mContext, a);
                                        if (!jsArg) {
                                            return {};
                                        }
                                        argv.push_back(*jsArg);
                                    }
                                    (void) fnVal.As<v8::Function>()->Call(cx.mContext,
                                                                          obj,
                                                                          static_cast<int>(argv.size()),
                                                                          argv.empty() ? nullptr : argv.data());
                                    return {};
                                });
                    });
            if (!posted) {
                return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::ObjectRevoked,
                                                 "failed to schedule method call on JavaScript thread");
            }
            return {};
        }

        [[nodiscard]] auto invokeMethodAsync(const std::string_view method, std::vector<Reflect::BridgeValue> args)
                -> Reflect::BridgeAsync override {
            auto        asyncOut = std::make_shared<Reflect::BridgeAsyncState>();
            std::string methodStr{method};
            auto        runOnJs = [objCopy      = mObj,
                                   methodName   = std::move(methodStr),
                                   capturedArgs = std::move(args),
                                   asyncOut]() mutable {
                auto entered = objCopy.enter<void>([&](Alka::ConvertCtx&          cx,
                                                       const v8::Local<v8::Value> val) -> Alka::Result<void> {
                    const auto           obj = val.As<v8::Object>();
                    const v8::TryCatch   tryCatch(cx.mIsolate);
                    v8::Local<v8::Value> fnVal;
                    if (!obj->Get(cx.mContext, v8Str(cx.mIsolate, methodName)).ToLocal(&fnVal) ||
                        !fnVal->IsFunction()) {
                        asyncOut->reject(Reflect::bridgeError(Reflect::BridgeErrorCode::MemberNotFound,
                                                              "method '" + methodName + "' not found on object"));
                        return {};
                    }
                    std::vector<v8::Local<v8::Value>> argv;
                    argv.reserve(capturedArgs.size());
                    for (std::size_t i = 0; i < capturedArgs.size(); ++i) {
                        auto jsArg = bridgeValueToV8(cx.mIsolate, cx.mContext, capturedArgs[i]);
                        if (!jsArg) {
                            asyncOut->reject(Reflect::prependErrorPath(std::move(jsArg).error(),
                                                                       "arg[" + std::to_string(i) + "]"));
                            return {};
                        }
                        argv.push_back(*jsArg);
                    }
                    v8::Local<v8::Value> jsRet;
                    if (!fnVal.As<v8::Function>()
                                 ->Call(cx.mContext,
                                        obj,
                                        static_cast<int>(argv.size()),
                                        argv.empty() ? nullptr : argv.data())
                                 .ToLocal(&jsRet)) {
                        asyncOut->reject(
                                Reflect::bridgeError(Reflect::BridgeErrorCode::ExecutionFailed,
                                                     extractExceptionMessage(cx.mIsolate, cx.mContext, tryCatch)));
                        return {};
                    }
                    auto converted = bridgeValueFromV8(cx.mIsolate, cx.mContext, jsRet);
                    if (!converted) {
                        asyncOut->reject(std::move(converted).error());
                        return {};
                    }
                    if (auto* nestedPromise = std::get_if<Reflect::BridgeAsync>(&converted->data);
                        nestedPromise != nullptr && *nestedPromise) {
                        (*nestedPromise)->then([asyncOut](const Expected<Reflect::BridgeValue>& res) {
                            asyncOut->settle(res);
                        });
                    } else {
                        asyncOut->resolve(std::move(*converted));
                    }
                    return {};
                });
                if (!entered) {
                    asyncOut->reject(
                            Reflect::bridgeError(Reflect::BridgeErrorCode::ExecutionFailed, entered.error().mMessage));
                }
            };

            if (canAccessSyncHere()) {
                runOnJs();
            } else if (!mDispatcher.post(std::move(runOnJs))) {
                asyncOut->reject(Reflect::bridgeError(Reflect::BridgeErrorCode::ObjectRevoked,
                                                      "JavaScript runtime is no longer available"));
            }
            return asyncOut;
        }

        [[nodiscard]] auto getPropertyAsync(const std::string_view key) -> Reflect::BridgeAsync override {
            auto        asyncOut = std::make_shared<Reflect::BridgeAsyncState>();
            std::string keyStr{key};
            auto        runOnJs = [objCopy = mObj, propKey = std::move(keyStr), asyncOut]() mutable {
                auto entered = objCopy.enter<void>([&](const Alka::ConvertCtx&    cx,
                                                       const v8::Local<v8::Value> val) -> Alka::Result<void> {
                    const v8::TryCatch   tryCatch(cx.mIsolate);
                    v8::Local<v8::Value> propVal;
                    if (!val.As<v8::Object>()->Get(cx.mContext, v8Str(cx.mIsolate, propKey)).ToLocal(&propVal)) {
                        asyncOut->reject(
                                Reflect::bridgeError(Reflect::BridgeErrorCode::ExecutionFailed,
                                                     extractExceptionMessage(cx.mIsolate, cx.mContext, tryCatch)));
                        return {};
                    }
                    auto converted = bridgeValueFromV8(cx.mIsolate, cx.mContext, propVal);
                    if (!converted) {
                        asyncOut->reject(std::move(converted).error());
                    } else {
                        asyncOut->resolve(std::move(*converted));
                    }
                    return {};
                });
                if (!entered) {
                    asyncOut->reject(
                            Reflect::bridgeError(Reflect::BridgeErrorCode::ExecutionFailed, entered.error().mMessage));
                }
            };

            if (canAccessSyncHere()) {
                runOnJs();
            } else if (!mDispatcher.post(std::move(runOnJs))) {
                asyncOut->reject(Reflect::bridgeError(Reflect::BridgeErrorCode::ObjectRevoked,
                                                      "JavaScript runtime is no longer available"));
            }
            return asyncOut;
        }

    private:
        ScopedJsValue    mObj;
        Alka::Dispatcher mDispatcher;
    };

    void dynamicCtorCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
        guardedCallback(args, [&] {
            v8::Isolate* isolate = args.GetIsolate();
            const auto   context = isolate->GetCurrentContext();
            if (!args.IsConstructCall()) {
                isolate->ThrowException(
                        v8::Exception::TypeError(v8Str(isolate, "Class constructor cannot be invoked without 'new'")));
                return;
            }
            auto& st = stateForIsolate(isolate);
            if (args.Length() == 1 && args[0]->StrictEquals(st.internalCtorSentinel.Get(isolate))) {
                return;
            }

            const auto* desc = dataOf<Reflect::ClassDescriptor>(args);
            if (desc == nullptr || desc->ctors.empty()) {
                const std::string name = desc != nullptr ? std::string(desc->name) : std::string("NativeObject");
                isolate->ThrowException(v8::Exception::TypeError(
                        v8Str(isolate, "'" + name + "' cannot be constructed directly from JavaScript")));
                return;
            }

            std::vector<Reflect::BridgeValue> callArgs;
            callArgs.reserve(static_cast<std::size_t>(args.Length()));
            for (int i = 0; i < args.Length(); ++i) {
                Reflect::BorrowScopeGuard borrowScope;
                auto converted = bridgeValueFromV8(isolate, context, args[i], borrowScope.lease().get());
                if (!converted) {
                    throwBridgeError(
                            isolate,
                            Reflect::prependErrorPath(std::move(converted).error(), "arg[" + std::to_string(i) + "]"));
                    return;
                }
                callArgs.push_back(std::move(*converted));
            }

            std::shared_ptr<Reflect::RevocableObjectCell> cell;
            std::optional<Error>                          lastErr;
            for (const auto& ctor: desc->ctors) {
                if (ctor.construct == nullptr || callArgs.size() < ctor.minArity || callArgs.size() > ctor.maxArity) {
                    continue;
                }
                auto res = ctor.construct(callArgs);
                if (res) {
                    cell = std::move(*res);
                    break;
                }
                lastErr = std::move(res).error();
            }
            if (!cell) {
                if (lastErr) {
                    throwBridgeError(isolate, *lastErr);
                } else {
                    throwBridgeError(isolate,
                                     Reflect::bridgeError(Reflect::BridgeErrorCode::ArityMismatch,
                                                          "no constructor of '" + std::string(desc->name) +
                                                                  "' accepts " + std::to_string(callArgs.size()) +
                                                                  " argument(s)"));
                }
                return;
            }

            auto rec        = std::make_unique<NativeWrapperRecord>();
            rec->isolate    = isolate;
            rec->sharedCell = std::move(cell);
            rec->cacheKey =
                    IdentityCacheKey{.descriptor = desc, .identity = rec->sharedCell->identityKey(), .isConst = false};
            rec->inCache      = true;
            auto*      rawRec = rec.get();
            const auto self   = args.This();
            (void) self->SetPrivate(context, wrapperPrivateKey(isolate), v8::External::New(isolate, rawRec));
            rawRec->weakSelf.Reset(isolate, self);
            rawRec->weakSelf.SetWeak(rawRec, &onWrapperWeakCallback, v8::WeakCallbackType::kParameter);

            std::lock_guard lock(gIsolateMapMutex);
            st.identityCache[rawRec->cacheKey] = rawRec;
            st.wrappers.push_back(std::move(rec));
        });
    }

    void dynamicMethodCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
        guardedCallback(args, [&] {
            v8::Isolate* isolate = args.GetIsolate();
            const auto   context = isolate->GetCurrentContext();
            const auto*  rec     = unwrapNativeRecord(isolate, context, args.This());
            auto         pinned  = pinWrapperRecord(rec);
            if (!pinned) {
                throwBridgeError(isolate, pinned.error());
                return;
            }

            const std::string methodName = toUtf8(isolate, args.Data());
            void*             targetPtr  = pinned->rawPtr;
            const auto*       methodDesc = findMethodAlongHierarchy(pinned->descriptor, methodName, targetPtr);
            if (methodDesc == nullptr || methodDesc->invoke == nullptr) {
                throwBridgeError(isolate,
                                 Reflect::bridgeError(Reflect::BridgeErrorCode::MemberNotFound,
                                                      "method '" + methodName + "' not found on '" +
                                                              std::string(pinned->descriptor->name) + "'"));
                return;
            }

            Reflect::BorrowScopeGuard         borrowScope;
            std::vector<Reflect::BridgeValue> callArgs;
            callArgs.reserve(static_cast<std::size_t>(args.Length()));
            for (int i = 0; i < args.Length(); ++i) {
                auto converted = bridgeValueFromV8(isolate, context, args[i], borrowScope.lease().get());
                if (!converted) {
                    throwBridgeError(
                            isolate,
                            Reflect::prependErrorPath(std::move(converted).error(), "arg[" + std::to_string(i) + "]"));
                    return;
                }
                callArgs.push_back(std::move(*converted));
            }

            auto ret = methodDesc->invoke(targetPtr, pinned->isConst, callArgs, borrowScope.lease().get());
            if (!ret) {
                throwBridgeError(isolate, ret.error());
                return;
            }
            if (ret->isNull()) {
                return;
            }
            auto jsRet = bridgeValueToV8(isolate, context, *ret);
            if (!jsRet) {
                throwBridgeError(isolate, jsRet.error());
                return;
            }
            args.GetReturnValue().Set(*jsRet);
        });
    }

    void dynamicGetterCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
        guardedCallback(args, [&] {
            v8::Isolate* isolate = args.GetIsolate();
            const auto   context = isolate->GetCurrentContext();
            const auto*  rec     = unwrapNativeRecord(isolate, context, args.This());
            auto         pinned  = pinWrapperRecord(rec);
            if (!pinned) {
                throwBridgeError(isolate, pinned.error());
                return;
            }

            const std::string propName  = toUtf8(isolate, args.Data());
            void*             targetPtr = pinned->rawPtr;
            const auto*       propDesc  = findPropertyAlongHierarchy(pinned->descriptor, propName, targetPtr);
            if (propDesc == nullptr || propDesc->get == nullptr) {
                return;
            }

            Reflect::BorrowScopeGuard borrowScope;
            auto                      ret = propDesc->get(targetPtr, borrowScope.lease().get());
            if (!ret) {
                throwBridgeError(isolate, ret.error());
                return;
            }
            auto jsVal = bridgeValueToV8(isolate, context, *ret);
            if (!jsVal) {
                throwBridgeError(isolate, jsVal.error());
                return;
            }
            args.GetReturnValue().Set(*jsVal);
        });
    }

    void dynamicSetterCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
        guardedCallback(args, [&] {
            v8::Isolate* isolate = args.GetIsolate();
            const auto   context = isolate->GetCurrentContext();
            const auto*  rec     = unwrapNativeRecord(isolate, context, args.This());
            auto         pinned  = pinWrapperRecord(rec);
            if (!pinned) {
                throwBridgeError(isolate, pinned.error());
                return;
            }

            const std::string propName = toUtf8(isolate, args.Data());
            if (pinned->isConst) {
                throwBridgeError(isolate,
                                 Reflect::bridgeError(Reflect::BridgeErrorCode::ReadonlyViolation,
                                                      "cannot assign property '" + propName + "' on read-only '" +
                                                              std::string(pinned->descriptor->name) + "'"));
                return;
            }

            void*       targetPtr = pinned->rawPtr;
            const auto* propDesc  = findPropertyAlongHierarchy(pinned->descriptor, propName, targetPtr);
            if (propDesc == nullptr || propDesc->readonly || propDesc->set == nullptr) {
                throwBridgeError(isolate,
                                 Reflect::bridgeError(Reflect::BridgeErrorCode::ReadonlyViolation,
                                                      "property '" + propName + "' on '" +
                                                              std::string(pinned->descriptor->name) +
                                                              "' is read-only"));
                return;
            }

            Reflect::BorrowScopeGuard  borrowScope;
            const v8::Local<v8::Value> rhs       = args.Length() > 0 ? args[0] : v8::Undefined(isolate).As<v8::Value>();
            auto                       converted = bridgeValueFromV8(isolate, context, rhs, borrowScope.lease().get());
            if (!converted) {
                throwBridgeError(isolate, converted.error());
                return;
            }
            if (auto res = propDesc->set(targetPtr, *converted); !res) {
                throwBridgeError(isolate, res.error());
            }
        });
    }

    [[nodiscard]] auto getOrBuildClassTemplate(v8::Isolate* isolate, const Reflect::ClassDescriptor* desc)
            -> v8::Local<v8::FunctionTemplate> {
        {
            std::lock_guard lock(gIsolateMapMutex);
            auto&           st = gIsolateStates[isolate];
            if (st) {
                if (const auto it = st->templates.find(desc); it != st->templates.end() && !it->second.IsEmpty()) {
                    return it->second.Get(isolate);
                }
            }
        }
        (void) stateForIsolate(isolate);

        v8::Local<v8::FunctionTemplate> baseTmpl;
        if (desc->base != nullptr) {
            baseTmpl = getOrBuildClassTemplate(isolate, desc->base);
        }

        const auto tmpl = v8::FunctionTemplate::New(
                isolate, &dynamicCtorCallback, v8::External::New(isolate, const_cast<Reflect::ClassDescriptor*>(desc)));
        tmpl->SetClassName(v8Str(isolate, desc->name));
        tmpl->InstanceTemplate()->SetInternalFieldCount(1);

        if (!baseTmpl.IsEmpty()) {
            tmpl->Inherit(baseTmpl);
        }

        const auto proto = tmpl->PrototypeTemplate();
        proto->Set(v8::Symbol::GetToStringTag(isolate),
                   v8Str(isolate, desc->name),
                   static_cast<v8::PropertyAttribute>(v8::ReadOnly | v8::DontEnum));

        for (const auto& method: desc->methods) {
            const auto nameStr = v8Str(isolate, method.name);
            proto->Set(nameStr, v8::FunctionTemplate::New(isolate, &dynamicMethodCallback, nameStr));
        }

        for (const auto& prop: desc->properties) {
            const auto nameStr    = v8Str(isolate, prop.name);
            const auto getterTmpl = v8::FunctionTemplate::New(isolate, &dynamicGetterCallback, nameStr);
            v8::Local<v8::FunctionTemplate> setterTmpl;
            v8::PropertyAttribute           attrs = v8::None;
            if (!prop.readonly && prop.set != nullptr) {
                setterTmpl = v8::FunctionTemplate::New(isolate, &dynamicSetterCallback, nameStr);
            } else {
                attrs = v8::ReadOnly;
            }
            proto->SetAccessorProperty(nameStr, getterTmpl, setterTmpl, attrs);
        }

        {
            std::lock_guard lock(gIsolateMapMutex);
            gIsolateStates[isolate]->templates[desc].Reset(isolate, tmpl);
        }
        return tmpl;
    }

    [[nodiscard]] auto instantiateNativeWrapper(v8::Isolate*                         isolate,
                                                const v8::Local<v8::Context>         context,
                                                const Reflect::ClassDescriptor*      desc,
                                                std::unique_ptr<NativeWrapperRecord> rec)
            -> Expected<v8::Local<v8::Value>> {
        const auto              tmpl = getOrBuildClassTemplate(isolate, desc);
        v8::Local<v8::Function> ctor;
        if (!tmpl->GetFunction(context).ToLocal(&ctor)) {
            return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::ExecutionFailed,
                                             "failed to initialize wrapper for '" + std::string(desc->name) + "'");
        }
        auto&                 st     = stateForIsolate(isolate);
        v8::Local<v8::Value>  argv[] = {st.internalCtorSentinel.Get(isolate)};
        v8::Local<v8::Object> instance;
        const v8::TryCatch    tryCatch(isolate);
        if (!ctor->NewInstance(context, 1, argv).ToLocal(&instance)) {
            return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::ExecutionFailed,
                                             extractExceptionMessage(isolate, context, tryCatch));
        }

        auto* rawRec = rec.get();
        (void) instance->SetPrivate(context, wrapperPrivateKey(isolate), v8::External::New(isolate, rawRec));
        rawRec->weakSelf.Reset(isolate, instance);
        rawRec->weakSelf.SetWeak(rawRec, &onWrapperWeakCallback, v8::WeakCallbackType::kParameter);

        {
            std::lock_guard lock(gIsolateMapMutex);
            if (rawRec->inCache) {
                st.identityCache[rawRec->cacheKey] = rawRec;
            }
            st.wrappers.push_back(std::move(rec));
        }
        return instance.As<v8::Value>();
    }

    void cppCallableV8Trampoline(const v8::FunctionCallbackInfo<v8::Value>& args) {
        guardedCallback(args, [&] {
            v8::Isolate* isolate = args.GetIsolate();
            const auto   context = isolate->GetCurrentContext();
            const auto*  rec     = dataOf<CppCallableRecord>(args);
            if (rec == nullptr || !rec->callable) {
                isolate->ThrowException(v8::Exception::Error(v8Str(isolate, "callback is no longer available")));
                return;
            }

            std::vector<Reflect::BridgeValue> callArgs;
            callArgs.reserve(static_cast<std::size_t>(args.Length()));
            for (int i = 0; i < args.Length(); ++i) {
                Reflect::BorrowScopeGuard borrowScope;
                auto converted = bridgeValueFromV8(isolate, context, args[i], borrowScope.lease().get());
                if (!converted) {
                    throwBridgeError(
                            isolate,
                            Reflect::prependErrorPath(std::move(converted).error(), "arg[" + std::to_string(i) + "]"));
                    return;
                }
                callArgs.push_back(std::move(*converted));
            }

            auto ret = rec->callable->invoke(callArgs);
            if (!ret) {
                throwBridgeError(isolate, ret.error());
                return;
            }
            if (ret->isNull()) {
                return;
            }
            auto jsRet = bridgeValueToV8(isolate, context, *ret);
            if (!jsRet) {
                throwBridgeError(isolate, jsRet.error());
                return;
            }
            args.GetReturnValue().Set(*jsRet);
        });
    }

    void onJsPromiseSettled(const v8::FunctionCallbackInfo<v8::Value>& args) {
        v8::Isolate*                               isolate = args.GetIsolate();
        const auto                                 context = isolate->GetCurrentContext();
        const std::unique_ptr<PromiseReactionData> reaction(dataOf<PromiseReactionData>(args));
        if (!reaction || !reaction->state) {
            return;
        }
        const v8::Local<v8::Value> arg = args.Length() > 0 ? args[0] : v8::Undefined(isolate).As<v8::Value>();
        if (reaction->isReject) {
            auto msg = extractValueErrorMessage(isolate, context, arg);
            reaction->state->reject(Reflect::bridgeError(Reflect::BridgeErrorCode::ExecutionFailed,
                                                         msg.empty() ? "promise rejected" : std::move(msg)));
            return;
        }
        auto converted = bridgeValueFromV8(isolate, context, arg);
        if (!converted) {
            reaction->state->reject(std::move(converted).error());
        } else {
            reaction->state->resolve(std::move(*converted));
        }
    }

    auto fromV8Impl(v8::Isolate*                        isolate,
                    const v8::Local<v8::Context>        context,
                    const v8::Local<v8::Value>          value,
                    Reflect::BorrowLease*               activeLease,
                    std::vector<v8::Local<v8::Object>>& seen,
                    const std::size_t                   depth) -> Expected<Reflect::BridgeValue> {
        if (depth > kMaxBridgeDepth) {
            return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::DepthExceeded,
                                             "maximum object nesting depth (64) exceeded");
        }
        if (value.IsEmpty() || value->IsUndefined() || value->IsNull()) {
            return Reflect::BridgeValue{nullptr};
        }
        if (value->IsBoolean()) {
            return Reflect::BridgeValue{value->BooleanValue(isolate)};
        }
        if (value->IsInt32()) {
            return Reflect::BridgeValue{static_cast<std::int64_t>(value.As<v8::Int32>()->Value())};
        }
        if (value->IsUint32()) {
            return Reflect::BridgeValue{static_cast<std::uint64_t>(value.As<v8::Uint32>()->Value())};
        }
        if (value->IsNumber()) {
            const double d = value.As<v8::Number>()->Value();
            if (std::isfinite(d) && std::trunc(d) == d && d >= static_cast<double>(kMinSafeJsInt) &&
                d <= static_cast<double>(kMaxSafeJsInt)) {
                return Reflect::BridgeValue{static_cast<std::int64_t>(d)};
            }
            return Reflect::BridgeValue{d};
        }
        if (value->IsBigInt()) {
            const auto bg       = value.As<v8::BigInt>();
            bool       lossless = false;
            const auto signedV  = bg->Int64Value(&lossless);
            if (lossless) {
                return Reflect::BridgeValue{signedV};
            }
            const auto unsignedV = bg->Uint64Value(&lossless);
            if (lossless) {
                return Reflect::BridgeValue{unsignedV};
            }
            return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::IntegerOverflow,
                                             "BigInt value exceeds 64-bit integer range");
        }
        if (value->IsString()) {
            return Reflect::BridgeValue{toUtf8(isolate, value)};
        }

        if (value->IsArrayBufferView()) {
            const auto                  view = value.As<v8::ArrayBufferView>();
            Reflect::BridgeValue::Bytes bytes(view->ByteLength());
            if (!bytes.empty()) {
                (void) view->CopyContents(bytes.data(), bytes.size());
            }
            return Reflect::BridgeValue{std::move(bytes)};
        }
        if (value->IsArrayBuffer()) {
            const auto                  ab    = value.As<v8::ArrayBuffer>();
            const auto                  store = ab->GetBackingStore();
            Reflect::BridgeValue::Bytes bytes(ab->ByteLength());
            if (!bytes.empty() && store && store->Data() != nullptr) {
                std::memcpy(bytes.data(), store->Data(), bytes.size());
            }
            return Reflect::BridgeValue{std::move(bytes)};
        }

        if (value->IsPromise()) {
            const auto promise  = value.As<v8::Promise>();
            auto       asyncOut = std::make_shared<Reflect::BridgeAsyncState>();
            if (promise->State() == v8::Promise::kFulfilled) {
                auto inner = fromV8Impl(isolate, context, promise->Result(), activeLease, seen, depth + 1);
                if (!inner) {
                    asyncOut->reject(std::move(inner).error());
                } else {
                    asyncOut->resolve(std::move(*inner));
                }
                return Reflect::BridgeValue{std::move(asyncOut)};
            }
            if (promise->State() == v8::Promise::kRejected) {
                auto msg = extractValueErrorMessage(isolate, context, promise->Result());
                asyncOut->reject(Reflect::bridgeError(Reflect::BridgeErrorCode::ExecutionFailed,
                                                      msg.empty() ? "promise rejected" : std::move(msg)));
                return Reflect::BridgeValue{std::move(asyncOut)};
            }

            auto okData  = std::make_unique<PromiseReactionData>(PromiseReactionData{asyncOut, false});
            auto errData = std::make_unique<PromiseReactionData>(PromiseReactionData{asyncOut, true});
            auto onOkFn  = makeFn(context, &onJsPromiseSettled, v8::External::New(isolate, okData.get()));
            auto onErrFn = makeFn(context, &onJsPromiseSettled, v8::External::New(isolate, errData.get()));
            if (!onOkFn.IsEmpty() && !onErrFn.IsEmpty()) {
                v8::Local<v8::Promise> chained;
                if (promise->Then(context, onOkFn, onErrFn).ToLocal(&chained)) {
                    (void) okData.release();
                    (void) errData.release();
                }
            }
            return Reflect::BridgeValue{std::move(asyncOut)};
        }

        if (value->IsFunction()) {
            const auto fn      = value.As<v8::Function>();
            const auto privKey = callablePrivateKey(isolate);
            bool       hasPriv = false;
            if (fn->HasPrivate(context, privKey).To(&hasPriv) && hasPriv) {
                v8::Local<v8::Value> extVal;
                if (fn->GetPrivate(context, privKey).ToLocal(&extVal) && extVal->IsExternal()) {
                    if (const auto* rec = static_cast<CppCallableRecord*>(extVal.As<v8::External>()->Value());
                        rec != nullptr && rec->callable) {
                        return Reflect::BridgeValue{rec->callable};
                    }
                }
            }
            return Reflect::BridgeValue{std::static_pointer_cast<Reflect::BridgeCallableImpl>(
                    std::make_shared<JsBridgeCallable>(isolate, context, fn))};
        }

        if (value->IsArray()) {
            const auto arr = value.As<v8::Array>();
            for (const auto& prev: seen) {
                if (prev->StrictEquals(arr)) {
                    return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::CircularReference,
                                                     "circular array reference");
                }
            }
            seen.emplace_back(arr);
            Reflect::BridgeValue::Array out;
            out.reserve(arr->Length());
            for (std::uint32_t i = 0; i < arr->Length(); ++i) {
                v8::Local<v8::Value> elem;
                if (!arr->Get(context, i).ToLocal(&elem)) {
                    seen.pop_back();
                    return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::ExecutionFailed,
                                                     "failed reading array element [" + std::to_string(i) + "]");
                }
                auto converted = fromV8Impl(isolate, context, elem, activeLease, seen, depth + 1);
                if (!converted) {
                    seen.pop_back();
                    return unexpected(
                            Reflect::prependErrorPath(std::move(converted).error(), "[" + std::to_string(i) + "]"));
                }
                out.push_back(std::move(*converted));
            }
            seen.pop_back();
            return Reflect::BridgeValue{std::move(out)};
        }

        if (value->IsObject()) {
            const auto obj = value.As<v8::Object>();
            if (const auto* rec = unwrapNativeRecord(isolate, context, obj); rec != nullptr) {
                if (rec->sharedCell) {
                    return Reflect::BridgeValue{rec->sharedCell};
                }
                if (rec->borrowed.has_value()) {
                    return Reflect::BridgeValue{*rec->borrowed};
                }
                return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::ObjectRevoked,
                                                 "object is no longer available");
            }

            return Reflect::BridgeValue{std::static_pointer_cast<Reflect::RemoteObjectImpl>(
                    std::make_shared<JsRemoteObject>(isolate, context, obj))};
        }

        return Reflect::BridgeValue{nullptr};
    }

} // namespace

auto bridgeValueToV8(v8::Isolate*                 isolate,
                     const v8::Local<v8::Context> context,
                     const Reflect::BridgeValue&  value,
                     const std::size_t            depth) -> Expected<v8::Local<v8::Value>> {
    if (depth > kMaxBridgeDepth) {
        return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::DepthExceeded,
                                         "maximum object nesting depth (64) exceeded");
    }

    return std::visit(
            [&]<class Alt>(const Alt& alt) -> Expected<v8::Local<v8::Value>> {
                if constexpr (std::same_as<Alt, std::nullptr_t>) {
                    return v8::Local<v8::Value>{v8::Null(isolate)};
                } else if constexpr (std::same_as<Alt, bool>) {
                    return v8::Local<v8::Value>{v8::Boolean::New(isolate, alt)};
                } else if constexpr (std::same_as<Alt, std::int64_t>) {
                    if (alt >= kMinSafeJsInt && alt <= kMaxSafeJsInt) {
                        return v8::Local<v8::Value>{v8::Number::New(isolate, static_cast<double>(alt))};
                    }
                    return v8::Local<v8::Value>{v8::BigInt::New(isolate, alt)};
                } else if constexpr (std::same_as<Alt, std::uint64_t>) {
                    if (alt <= kMaxSafeJsUint) {
                        return v8::Local<v8::Value>{v8::Number::New(isolate, static_cast<double>(alt))};
                    }
                    return v8::Local<v8::Value>{v8::BigInt::NewFromUnsigned(isolate, alt)};
                } else if constexpr (std::same_as<Alt, double>) {
                    return v8::Local<v8::Value>{v8::Number::New(isolate, alt)};
                } else if constexpr (std::same_as<Alt, std::string>) {
                    return v8::Local<v8::Value>{v8Str(isolate, alt)};
                } else if constexpr (std::same_as<Alt, Reflect::BridgeValue::Bytes>) {
                    const auto ab = v8::ArrayBuffer::New(isolate, alt.size());
                    if (!alt.empty()) {
                        if (const auto store = ab->GetBackingStore(); store && store->Data() != nullptr) {
                            std::memcpy(store->Data(), alt.data(), alt.size());
                        }
                    }
                    return v8::Local<v8::Value>{v8::Uint8Array::New(ab, 0, alt.size())};
                } else if constexpr (std::same_as<Alt, Reflect::BridgeValue::Array>) {
                    const auto arr = v8::Array::New(isolate, static_cast<int>(alt.size()));
                    for (std::size_t i = 0; i < alt.size(); ++i) {
                        auto elem = bridgeValueToV8(isolate, context, alt[i], depth + 1);
                        if (!elem) {
                            return unexpected(
                                    Reflect::prependErrorPath(std::move(elem).error(), "[" + std::to_string(i) + "]"));
                        }
                        (void) arr->Set(context, static_cast<std::uint32_t>(i), *elem);
                    }
                    return v8::Local<v8::Value>{arr};
                } else if constexpr (std::same_as<Alt, Reflect::BridgeValue::Object>) {
                    const auto obj = v8::Object::New(isolate);
                    for (const auto& [k, v]: alt) {
                        auto prop = bridgeValueToV8(isolate, context, v, depth + 1);
                        if (!prop) {
                            return unexpected(Reflect::prependErrorPath(std::move(prop).error(), "." + k));
                        }
                        (void) obj->Set(context, v8Str(isolate, k), *prop);
                    }
                    return v8::Local<v8::Value>{obj};
                } else if constexpr (std::same_as<Alt, std::shared_ptr<Reflect::RevocableObjectCell>>) {
                    if (!alt) {
                        return v8::Local<v8::Value>{v8::Null(isolate)};
                    }
                    const auto* desc = alt->descriptor();
                    if (!alt->alive() || desc == nullptr) {
                        return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::ObjectRevoked,
                                                         "object '" + std::string(alt->typeName()) +
                                                                 "' is no longer available");
                    }
                    const IdentityCacheKey key{desc, alt->identityKey(), alt->isConst()};
                    {
                        std::lock_guard lock(gIsolateMapMutex);
                        if (const auto stIt = gIsolateStates.find(isolate);
                            stIt != gIsolateStates.end() && stIt->second) {
                            if (const auto cacheIt = stIt->second->identityCache.find(key);
                                cacheIt != stIt->second->identityCache.end() && cacheIt->second != nullptr &&
                                !cacheIt->second->weakSelf.IsEmpty()) {
                                return v8::Local<v8::Value>{cacheIt->second->weakSelf.Get(isolate)};
                            }
                        }
                    }
                    auto rec        = std::make_unique<NativeWrapperRecord>();
                    rec->isolate    = isolate;
                    rec->sharedCell = alt;
                    rec->cacheKey   = key;
                    rec->inCache    = true;
                    return instantiateNativeWrapper(isolate, context, desc, std::move(rec));
                } else if constexpr (std::same_as<Alt, Reflect::BorrowedObjectHandle>) {
                    if (alt.rawPtr == nullptr) {
                        return v8::Local<v8::Value>{v8::Null(isolate)};
                    }
                    if (!alt.alive() || alt.descriptor == nullptr) {
                        return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::BorrowExpired,
                                                         "borrowed object has expired");
                    }
                    auto rec      = std::make_unique<NativeWrapperRecord>();
                    rec->isolate  = isolate;
                    rec->borrowed = alt;
                    rec->inCache  = false;
                    return instantiateNativeWrapper(isolate, context, alt.descriptor, std::move(rec));
                } else if constexpr (std::same_as<Alt, Reflect::RemoteObject>) {
                    if (!alt) {
                        return v8::Local<v8::Value>{v8::Null(isolate)};
                    }
                    if (alt->nativeTag() == kJsRemoteTag) {
                        const auto* jsRemote = static_cast<const JsRemoteObject*>(alt.get());
                        if (jsRemote->belongsTo(isolate)) {
                            return v8::Local<v8::Value>{jsRemote->localObject(isolate)};
                        }
                    }
                    auto keys = alt->propertyNames();
                    if (!keys) {
                        return unexpected(std::move(keys).error());
                    }
                    const auto obj = v8::Object::New(isolate);
                    for (const auto& k: *keys) {
                        auto v = alt->getProperty(k);
                        if (!v) {
                            return unexpected(Reflect::prependErrorPath(std::move(v).error(), "." + k));
                        }
                        auto jsV = bridgeValueToV8(isolate, context, *v, depth + 1);
                        if (!jsV) {
                            return unexpected(Reflect::prependErrorPath(std::move(jsV).error(), "." + k));
                        }
                        (void) obj->Set(context, v8Str(isolate, k), *jsV);
                    }
                    return v8::Local<v8::Value>{obj};
                } else if constexpr (std::same_as<Alt, Reflect::BridgeCallable>) {
                    if (!alt) {
                        return v8::Local<v8::Value>{v8::Null(isolate)};
                    }
                    if (alt->nativeTag() == kJsCallableTag) {
                        const auto* jsCall = static_cast<const JsBridgeCallable*>(alt.get());
                        if (jsCall->belongsTo(isolate)) {
                            return v8::Local<v8::Value>{jsCall->localFunction(isolate)};
                        }
                    }
                    auto rec          = std::make_unique<CppCallableRecord>();
                    rec->isolate      = isolate;
                    rec->callable     = alt;
                    auto*      rawRec = rec.get();
                    const auto fn     = makeFn(context, &cppCallableV8Trampoline, v8::External::New(isolate, rawRec));
                    if (fn.IsEmpty()) {
                        return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::ExecutionFailed,
                                                         "failed to wrap callback");
                    }
                    (void) fn->SetPrivate(context, callablePrivateKey(isolate), v8::External::New(isolate, rawRec));
                    rawRec->weakFn.Reset(isolate, fn);
                    rawRec->weakFn.SetWeak(rawRec, &onCallableWeakCallback, v8::WeakCallbackType::kParameter);
                    {
                        std::lock_guard lock(gIsolateMapMutex);
                        stateForIsolate(isolate).callables.push_back(std::move(rec));
                    }
                    return v8::Local<v8::Value>{fn};
                } else {
                    v8::Local<v8::Promise::Resolver> resolver;
                    if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
                        return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::ExecutionFailed,
                                                         "failed to create Promise");
                    }
                    const auto promise = resolver->GetPromise();
                    if (!alt) {
                        (void) resolver->Resolve(context, v8::Undefined(isolate));
                        return v8::Local<v8::Value>{promise};
                    }

                    ScopedJsValue    resolverHandle = ScopedJsValue::make(isolate, context, resolver);
                    Alka::Dispatcher dispatcher{isolate};
                    dispatcher.retain();

                    alt->then([resolverHandle = std::move(resolverHandle),
                               dispatcher](const Expected<Reflect::BridgeValue>& outcome) mutable {
                        auto settleOnJs = [resolverHandle, dispatcher, outcome]() mutable {
                            (void) resolverHandle.enter<void>([&](const Alka::ConvertCtx&    cx,
                                                                  const v8::Local<v8::Value> resVal)
                                                                      -> Alka::Result<void> {
                                const auto res = resVal.As<v8::Promise::Resolver>();
                                if (!outcome) {
                                    const auto msg = normalizeErrorMessage(outcome.error().message);
                                    (void) res->Reject(cx.mContext,
                                                       v8::Exception::Error(v8Str(
                                                               cx.mIsolate, msg.empty() ? "promise rejected" : msg)));
                                    return {};
                                }
                                if (outcome->isNull()) {
                                    (void) res->Resolve(cx.mContext, v8::Undefined(cx.mIsolate));
                                    return {};
                                }
                                if (auto jsVal = bridgeValueToV8(cx.mIsolate, cx.mContext, *outcome); !jsVal) {
                                    const auto msg = normalizeErrorMessage(jsVal.error().message);
                                    (void) res->Reject(cx.mContext,
                                                       v8::Exception::Error(v8Str(
                                                               cx.mIsolate, msg.empty() ? "promise rejected" : msg)));
                                } else {
                                    (void) res->Resolve(cx.mContext, *jsVal);
                                }
                                return {};
                            });
                            dispatcher.release();
                        };

                        if (dispatcher.link() && dispatcher.link()->onIsolateThread()) {
                            settleOnJs();
                        } else if (!dispatcher.post(std::move(settleOnJs))) {
                            dispatcher.release();
                        }
                    });

                    return v8::Local<v8::Value>{promise};
                }
            },
            value.data);
}

auto bridgeValueFromV8(v8::Isolate*                 isolate,
                       const v8::Local<v8::Context> context,
                       const v8::Local<v8::Value>   value,
                       Reflect::BorrowLease*        activeLease) -> Expected<Reflect::BridgeValue> {
    std::vector<v8::Local<v8::Object>> seen;
    return fromV8Impl(isolate, context, value, activeLease, seen, 0);
}

auto writeBridgeFieldsToV8Object(v8::Isolate*                 isolate,
                                 const v8::Local<v8::Context> context,
                                 const v8::Local<v8::Object>  target,
                                 const Reflect::BridgeValue&  updated) -> Expected<void> {
    const auto* fields = std::get_if<Reflect::BridgeValue::Object>(&updated.data);
    if (fields == nullptr) {
        return {};
    }
    const v8::TryCatch tryCatch(isolate);
    for (const auto& [k, v]: *fields) {
        auto jsVal = bridgeValueToV8(isolate, context, v);
        if (!jsVal) {
            return unexpected(Reflect::prependErrorPath(std::move(jsVal).error(), "." + k));
        }
        bool ok = false;
        if (!target->Set(context, v8Str(isolate, k), *jsVal).To(&ok) || !ok) {
            return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::ExecutionFailed,
                                             tryCatch.HasCaught() ? extractExceptionMessage(isolate, context, tryCatch)
                                                                  : "failed to update field '" + k + "'");
        }
    }
    return {};
}

void disposeReflectIsolate(v8::Isolate* isolate) noexcept {
    if (isolate == nullptr) {
        return;
    }
    std::unique_ptr<IsolateReflectState> doomed;
    {
        std::lock_guard lock(gIsolateMapMutex);
        if (const auto it = gIsolateStates.find(isolate); it != gIsolateStates.end()) {
            doomed = std::move(it->second);
            gIsolateStates.erase(it);
        }
    }
    if (!doomed) {
        return;
    }
    for (auto& w: doomed->wrappers) {
        if (w) {
            w->weakSelf.ClearWeak();
            w->weakSelf.Reset();
        }
    }
    for (auto& c: doomed->callables) {
        if (c) {
            c->weakFn.ClearWeak();
            c->weakFn.Reset();
        }
    }
    for (auto& t: doomed->templates | std::views::values) {
        t.Reset();
    }
    doomed->internalCtorSentinel.Reset();
}

} // namespace CloverNT::Core::Modules::bridge
