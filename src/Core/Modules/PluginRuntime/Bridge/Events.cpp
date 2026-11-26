#include <CloverNT/API/Events/EventBus.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/AlkaBindings.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/Events.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/ReflectBridge.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/Support.hpp>

#include <Alka/Alka.hpp>

#include <atomic>
#include <cstddef>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace CloverNT::Core::Modules::bridge {

namespace {

    struct ScopedControlInvalidator {
        v8::Isolate*    isolate{};
        JsEventControl* ctrl{};

        ~ScopedControlInvalidator() noexcept {
            if (ctrl != nullptr) {
                ctrl->detach();
                if (isolate != nullptr) {
                    Alka::invalidate(isolate, ctrl);
                }
            }
        }
    };

    [[nodiscard]] bool containsScopedBorrow(const Reflect::BridgeValue& val) noexcept {
        if (val.isBorrowedObject()) {
            return true;
        }
        if (val.isArray()) {
            for (const auto& elem: val.asArray()) {
                if (containsScopedBorrow(elem)) {
                    return true;
                }
            }
            return false;
        }
        if (val.isObject()) {
            for (const auto& fieldVal: val.asObject() | std::views::values) {
                if (containsScopedBorrow(fieldVal)) {
                    return true;
                }
            }
            return false;
        }
        return false;
    }

} // namespace

bool JsEventControl::cancelled() const noexcept {
    return mInvocation != nullptr ? mInvocation->isCancelled() : mCancelledSnapshot;
}

bool JsEventControl::cancel() noexcept {
    if (!mCanCancel || mInvocation == nullptr) {
        return false;
    }
    const bool ok = mInvocation->cancel();
    if (ok) {
        mCancelledSnapshot = true;
    }
    return ok;
}

void JsEventControl::detach() noexcept {
    if (mInvocation != nullptr) {
        mCancelledSnapshot = mInvocation->isCancelled();
        mInvocation        = nullptr;
    }
}

void JsListenerSlot::dispatch(Event::EventInvocation& inv) {
    auto* const state = mStateCell != nullptr ? mStateCell->state.load(std::memory_order_acquire) : nullptr;
    if (state == nullptr || !state->isAttached()) {
        return;
    }

    if (state->isOnMainThread()) {
        v8::Isolate* const isolate = state->mIsolate;
        if (isolate == nullptr) {
            return;
        }
        const v8::Isolate::Scope isolateScope(isolate);
        const v8::HandleScope    handleScope(isolate);
        const auto               context = state->mContext.Get(isolate);
        const v8::Context::Scope contextScope(context);

        if (!mFn.valid() || !mFn.belongsTo(isolate)) {
            return;
        }
        const auto fnVal = mFn.local(isolate);
        if (fnVal.IsEmpty() || !fnVal->IsFunction()) {
            return;
        }
        const auto fn = fnVal.As<v8::Function>();

        const bool isJsFast = (inv.jsIsolate() == isolate && inv.jsFastArgs() != nullptr);

        std::vector<v8::Local<v8::Value>> argv;
        if (isJsFast) {
            const auto* rawArgs = static_cast<const v8::Local<v8::Value>*>(inv.jsFastArgs());
            const auto  count   = inv.jsFastArgCount();
            argv.reserve(count + 1);
            for (std::size_t i = 0; i < count; ++i) {
                argv.push_back(rawArgs[i]);
            }
        } else {
            const auto bridgeSpan = inv.bridgeArgs();
            if (!bridgeSpan) {
                JsRuntime::State::logger().error("Event listener (owner '{}') on '{}': {}",
                                                 ownerPlugin(),
                                                 inv.topic(),
                                                 bridgeSpan.error().message);
                return;
            }
            argv.reserve(bridgeSpan->size() + 1);
            for (std::size_t i = 0; i < bridgeSpan->size(); ++i) {
                auto jsArg = bridgeValueToV8(isolate, context, (*bridgeSpan)[i]);
                if (!jsArg) {
                    JsRuntime::State::logger().error("Event listener (owner '{}') on '{}': arg[{}]: {}",
                                                     ownerPlugin(),
                                                     inv.topic(),
                                                     i,
                                                     jsArg.error().message);
                    return;
                }
                argv.push_back(*jsArg);
            }
        }

        const std::size_t payloadCount = argv.size();
        const bool        allowCancel  = inv.isCancellable() && priority() != Event::EventPriority::Monitor;

        JsEventControl   ctrl(std::string(inv.topic()), std::string(inv.sourcePlugin()), "sync", allowCancel, &inv);
        Alka::ConvertCtx cx{isolate, context};
        argv.push_back(Alka::toJs(cx, &ctrl));
        const ScopedControlInvalidator ctrlGuard{isolate, &ctrl};

        const Reflect::ScopedOwnerPlugin ownerGuard(ownerPlugin());
        const v8::TryCatch               tryCatch(isolate);
        (void) fn->Call(context, v8::Undefined(isolate), static_cast<int>(argv.size()), argv.data());
        if (tryCatch.HasCaught()) {
            JsRuntime::State::logger().error("Event listener (owner '{}') threw on '{}': {}",
                                             ownerPlugin(),
                                             inv.topic(),
                                             describeException(isolate, context, tryCatch));
        } else if (!isJsFast && inv.isMutablePayload() && priority() != Event::EventPriority::Monitor &&
                   payloadCount > 0) {
            std::vector<Reflect::BridgeValue> updated;
            updated.reserve(payloadCount);
            bool conversionOk = true;
            for (std::size_t i = 0; i < payloadCount; ++i) {
                auto back = bridgeValueFromV8(isolate, context, argv[i], inv.borrowLease().get());
                if (!back) {
                    JsRuntime::State::logger().error("Event listener (owner '{}') on '{}': mutated arg[{}]: {}",
                                                     ownerPlugin(),
                                                     inv.topic(),
                                                     i,
                                                     back.error().message);
                    conversionOk = false;
                    break;
                }
                updated.push_back(std::move(*back));
            }
            if (conversionOk) {
                if (const auto committed = inv.commitBridgeMutation(updated); !committed) {
                    JsRuntime::State::logger().error("Event listener (owner '{}') on '{}': {}",
                                                     ownerPlugin(),
                                                     inv.topic(),
                                                     committed.error().message);
                }
            }
        }

        if (isOnce()) {
            state->untrackJsSubscription(id());
        }
        return;
    }

    const auto bridgeSpan = inv.bridgeArgs();
    if (!bridgeSpan) {
        JsRuntime::State::logger().error(
                "Event listener (owner '{}') on '{}': {}", ownerPlugin(), inv.topic(), bridgeSpan.error().message);
        return;
    }
    for (const auto& elem: *bridgeSpan) {
        if (containsScopedBorrow(elem)) {
            JsRuntime::State::logger().error(
                    "Event listener (owner '{}') on '{}': cannot deliver borrowed object across threads",
                    ownerPlugin(),
                    inv.topic());
            return;
        }
    }

    std::vector capturedArgs(bridgeSpan->begin(), bridgeSpan->end());
    std::string topicStr(inv.topic());
    std::string sourceStr(inv.sourcePlugin());
    std::string ownerStr(ownerPlugin());
    auto* const stateCell = mStateCell;
    Alka::Value fnCopy    = mFn;
    const auto  subId     = id();
    const bool  once      = isOnce();
    auto        act       = activity();

    (void) mDispatcher.post([stateCell,
                             fnCopy       = std::move(fnCopy),
                             capturedArgs = std::move(capturedArgs),
                             topicStr     = std::move(topicStr),
                             sourceStr    = std::move(sourceStr),
                             ownerStr     = std::move(ownerStr),
                             subId,
                             once,
                             act = std::move(act)]() mutable {
        Reflect::PluginActivity::Guard pin;
        if (act) {
            pin = act->tryEnter();
            if (!pin) {
                return;
            }
        }
        auto* const liveState = stateCell != nullptr ? stateCell->state.load(std::memory_order_acquire) : nullptr;
        if (liveState == nullptr || !liveState->isAttached() || liveState->mIsolate == nullptr) {
            return;
        }
        v8::Isolate* const       isolate = liveState->mIsolate;
        const v8::Isolate::Scope isolateScope(isolate);
        const v8::HandleScope    handleScope(isolate);
        const auto               context = liveState->mContext.Get(isolate);
        const v8::Context::Scope contextScope(context);

        if (!fnCopy.valid() || !fnCopy.belongsTo(isolate)) {
                return;
            }
            const auto fnVal = fnCopy.local(isolate);
            if (fnVal.IsEmpty() || !fnVal->IsFunction()) {
                return;
            }
            const auto fn = fnVal.As<v8::Function>();

            std::vector<v8::Local<v8::Value>> argv;
            argv.reserve(capturedArgs.size() + 1);
            for (std::size_t i = 0; i < capturedArgs.size(); ++i) {
                auto jsArg = bridgeValueToV8(isolate, context, capturedArgs[i]);
                if (!jsArg) {
                    JsRuntime::State::logger().error(
                        "Event listener (owner '{}') on '{}': arg[{}]: {}",
                        ownerStr,
                        topicStr,
                        i,
                        jsArg.error().message);
                    return;
                }
                argv.push_back(*jsArg);
            }

            JsEventControl   ctrl(topicStr, sourceStr, "async", /*canCancel=*/false, nullptr);
            Alka::ConvertCtx cx{isolate, context};
            argv.push_back(Alka::toJs(cx, &ctrl));
            const ScopedControlInvalidator ctrlGuard{isolate, &ctrl};

            const Reflect::ScopedOwnerPlugin ownerGuard(ownerStr);
            const v8::TryCatch               tryCatch(isolate);
            (void) fn->Call(context, v8::Undefined(isolate), static_cast<int>(argv.size()), argv.data());
            if (tryCatch.HasCaught()) {
                JsRuntime::State::logger().error("Event listener (owner '{}') threw on '{}': {}",
                                                 ownerStr,
                                                 topicStr,
                                                 describeException(isolate, context, tryCatch));
            }

        if (once) {
            liveState->untrackJsSubscription(subId);
        }
    });
}

} // namespace CloverNT::Core::Modules::bridge
