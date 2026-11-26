#pragma once

#include <CloverNT/API/Expected.hpp>
#include <CloverNT/API/Reflect/BridgeValue.hpp>

#include <QQNT/v8-cppgc.h>

#include <cstddef>

namespace CloverNT::Core::Modules::bridge {

[[nodiscard]] auto bridgeValueToV8(v8::Isolate*                isolate,
                                   v8::Local<v8::Context>      context,
                                   const Reflect::BridgeValue& value,
                                   std::size_t                 depth = 0) -> Expected<v8::Local<v8::Value>>;

[[nodiscard]] auto bridgeValueFromV8(v8::Isolate*           isolate,
                                     v8::Local<v8::Context> context,
                                     v8::Local<v8::Value>   value,
                                     Reflect::BorrowLease*  activeLease = nullptr) -> Expected<Reflect::BridgeValue>;

[[nodiscard]] auto writeBridgeFieldsToV8Object(v8::Isolate*                isolate,
                                               v8::Local<v8::Context>      context,
                                               v8::Local<v8::Object>       target,
                                               const Reflect::BridgeValue& updated) -> Expected<void>;

void disposeReflectIsolate(v8::Isolate* isolate) noexcept;

} // namespace CloverNT::Core::Modules::bridge
