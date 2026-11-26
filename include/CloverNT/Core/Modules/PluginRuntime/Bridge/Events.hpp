#pragma once

#include <CloverNT/API/Events/Listener.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/JsRuntimeState.hpp>

#include <Alka/Js/Value.hpp>
#include <Alka/Runtime/Dispatcher.hpp>

#include <memory>
#include <string>
#include <utility>

namespace CloverNT::Core::Modules::bridge {

namespace Event = ::CloverNT::Event;

class JsListenerSlot final : public Event::ListenerSlot {
public:
    JsListenerSlot(const Event::SubscriptionId              id,
                   const Event::EventPriority               priority,
                   const bool                               once,
                   std::string                              ownerPlugin,
                   std::shared_ptr<Reflect::PluginActivity> activity,
                   JsRuntime::State::SelfCell*              stateCell,
                   Alka::Value                              fn,
                   Alka::Dispatcher                         dispatcher) noexcept
        : ListenerSlot(id, priority, once, std::move(ownerPlugin), std::move(activity)),
          mStateCell(stateCell),
          mFn(std::move(fn)),
          mDispatcher(std::move(dispatcher)) {}

    void dispatch(Event::EventInvocation& inv) override;

private:
    JsRuntime::State::SelfCell* mStateCell{};
    Alka::Value                 mFn;
    Alka::Dispatcher            mDispatcher;
};

} // namespace CloverNT::Core::Modules::bridge
