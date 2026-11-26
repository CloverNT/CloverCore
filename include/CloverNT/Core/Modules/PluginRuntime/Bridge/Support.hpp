#pragma once
#include <CloverNT/API/Expected.hpp>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <ios>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QQNT/v8-cppgc.h>

#include <QQNT/node.h>

namespace CloverNT::Core::Modules {

[[nodiscard]] inline auto jsError(std::string message, const CommonErrorCode code = CommonErrorCode::OperationFailed)
        -> std::unexpected<Error> {
    return unexpected(makeError(ErrorCategory::Plugin, code, std::move(message)));
}

[[nodiscard]] inline auto toUtf8(v8::Isolate* isolate, const v8::Local<v8::Value> value) -> std::string {
    if (value.IsEmpty()) {
        return {};
    }
    const v8::String::Utf8Value utf8(isolate, value);
    return *utf8 != nullptr ? std::string(*utf8, utf8.length()) : std::string{};
}

[[nodiscard]] inline auto extractValueErrorMessage(v8::Isolate*                 isolate,
                                                   const v8::Local<v8::Context> context,
                                                   const v8::Local<v8::Value>   value) -> std::string {
    if (value.IsEmpty() || value->IsUndefined() || value->IsNull()) {
        return {};
    }
    if (value->IsObject()) {
        const v8::TryCatch   innerCatch(isolate);
        const auto           msgKey = v8::String::NewFromUtf8Literal(isolate, "message");
        v8::Local<v8::Value> msgVal;
        if (value.As<v8::Object>()->Get(context, msgKey).ToLocal(&msgVal) && !msgVal.IsEmpty() && msgVal->IsString()) {
            if (auto normalized = normalizeErrorMessage(toUtf8(isolate, msgVal)); !normalized.empty()) {
                return normalized;
            }
        }
    }
    return normalizeErrorMessage(toUtf8(isolate, value));
}

[[nodiscard]] inline auto extractExceptionMessage(v8::Isolate*                 isolate,
                                                  const v8::Local<v8::Context> context,
                                                  const v8::TryCatch&          tryCatch) -> std::string {
    if (!tryCatch.HasCaught()) {
        return "unknown error";
    }
    if (auto msg = extractValueErrorMessage(isolate, context, tryCatch.Exception()); !msg.empty()) {
        return msg;
    }
    return "unknown error";
}

[[nodiscard]] inline auto describeException(v8::Isolate*                 isolate,
                                            const v8::Local<v8::Context> context,
                                            const v8::TryCatch&          tryCatch) -> std::string {
    if (!tryCatch.HasCaught()) {
        return "(no exception)";
    }
    v8::Local<v8::Value> stack;
    if (tryCatch.StackTrace(context).ToLocal(&stack) && !stack.IsEmpty() && !stack->IsUndefined()) {
        if (auto text = toUtf8(isolate, stack); !text.empty()) {
            const auto newline = text.find('\n');
            const auto head    = std::string_view(text).substr(0, newline);
            const auto clean   = normalizeErrorMessage(head);
            if (!clean.empty() && head != clean) {
                if (newline == std::string::npos) {
                    return clean;
                }
                return clean + text.substr(newline);
            }
            return text;
        }
    }
    return extractExceptionMessage(isolate, context, tryCatch);
}

[[nodiscard]] inline auto v8Str(v8::Isolate* isolate, const std::string_view text) -> v8::Local<v8::String> {
    return v8::String::NewFromUtf8(isolate, text.data(), v8::NewStringType::kNormal, static_cast<int>(text.size()))
            .FromMaybe(v8::String::Empty(isolate));
}

inline void setProp(const v8::Local<v8::Context> context,
                    const v8::Local<v8::Object>  object,
                    const char*                  key,
                    const v8::Local<v8::Value>   value) {
    if (!value.IsEmpty()) {
        (void) object->Set(context, v8Str(v8::Isolate::GetCurrent(), key), value);
    }
}

[[nodiscard]] inline auto getProp(const v8::Local<v8::Context> context,
                                  const v8::Local<v8::Object>  object,
                                  const char*                  key) -> v8::Local<v8::Value> {
    v8::Local<v8::Value> value;
    if (object->Get(context, v8Str(v8::Isolate::GetCurrent(), key)).ToLocal(&value)) {
        return value;
    }
    return {};
}

[[nodiscard]] inline auto makeFn(const v8::Local<v8::Context> context,
                                 const v8::FunctionCallback   callback,
                                 const v8::Local<v8::Value>   data) -> v8::Local<v8::Function> {
    v8::Local<v8::Function> fn;
    (void) v8::Function::New(context, callback, data).ToLocal(&fn);
    return fn;
}

inline void invokeMethod(const v8::Local<v8::Context>                      context,
                         const v8::Local<v8::Object>                       object,
                         const char*                                       method,
                         const std::initializer_list<v8::Local<v8::Value>> argv) {
    const v8::Local<v8::Value> fnVal = getProp(context, object, method);
    if (fnVal.IsEmpty() || !fnVal->IsFunction()) {
        return;
    }
    std::vector args(argv);
    (void) fnVal.As<v8::Function>()->Call(
            context, object, static_cast<int>(args.size()), args.empty() ? nullptr : args.data());
}

[[nodiscard]] inline auto objectOwnKeys(const v8::Local<v8::Context> context, const v8::Local<v8::Object> object)
        -> std::vector<std::string> {
    std::vector<std::string> keys;
    v8::Local<v8::Array>     names;
    if (!object->GetOwnPropertyNames(context).ToLocal(&names)) {
        return keys;
    }
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    keys.reserve(names->Length());
    for (std::uint32_t i = 0; i < names->Length(); ++i) {
        v8::Local<v8::Value> key;
        if (names->Get(context, i).ToLocal(&key)) {
            keys.push_back(toUtf8(isolate, key));
        }
    }
    return keys;
}

[[nodiscard]] inline auto newKeys(std::vector<std::string> const& after, std::vector<std::string> const& before)
        -> std::vector<std::string> {
    std::vector<std::string> result;
    for (auto const& key: after) {
        if (std::ranges::find(before, key) == before.end()) {
            result.push_back(key);
        }
    }
    return result;
}

[[nodiscard]] inline auto readFileUtf8(const std::filesystem::path& path) -> std::string {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

template <typename T>
[[nodiscard]] auto holderOf(const v8::FunctionCallbackInfo<v8::Value>& args) -> T* {
    const auto self = args.This();
    if (self.IsEmpty() || self->InternalFieldCount() < 1) {
        return nullptr;
    }
    const auto field = self->GetInternalField(0).As<v8::Value>();
    return field->IsExternal() ? static_cast<T*>(field.As<v8::External>()->Value()) : nullptr;
}

template <typename T>
[[nodiscard]] auto dataOf(const v8::FunctionCallbackInfo<v8::Value>& args) -> T* {
    const auto data = args.Data();
    return data->IsExternal() ? static_cast<T*>(data.As<v8::External>()->Value()) : nullptr;
}

template <typename Info, typename Fn>
auto guardedCallback(const Info& info, Fn&& body) -> void {
    v8::Isolate* isolate = info.GetIsolate();
    try {
        std::forward<Fn>(body)();
    } catch (const std::exception& exception) {
        const auto msg = normalizeErrorMessage(exception.what());
        isolate->ThrowException(v8::Exception::Error(v8Str(isolate, msg.empty() ? "unknown error" : msg)));
    } catch (...) {
        isolate->ThrowException(v8::Exception::Error(v8Str(isolate, "unknown error")));
    }
}

} // namespace CloverNT::Core::Modules
