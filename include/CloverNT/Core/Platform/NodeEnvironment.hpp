#pragma once
#include <filesystem>

#include <QQNT/v8-cppgc.h>

#include <QQNT/node.h>

namespace CloverNT::Core::Platform {

struct EnvironmentContext {
    node::Environment*     env{};
    v8::Isolate*           isolate{};
    v8::Local<v8::Context> context;
    v8::Local<v8::Value>   require;
    std::filesystem::path  coreDir;
};

class NodeEnvironment {
public:
    static void publish(const EnvironmentContext& context);
    static void clear();

    [[nodiscard]] static bool isPresent();
    [[nodiscard]] static bool isRunnable();
    static void               markStopped();

    [[nodiscard]] static bool current(EnvironmentContext& out);
};

} // namespace CloverNT::Core::Platform
