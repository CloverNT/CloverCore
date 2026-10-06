#pragma once

#include <CloverNT/API/Expected.hpp>

#include <functional>
#include <memory>
#include <string_view>

namespace CloverNT::Plugin {
class JsPlugin;
} // namespace CloverNT::Plugin

namespace CloverNT::Core::Platform {
struct EnvironmentContext;
} // namespace CloverNT::Core::Platform

namespace CloverNT::Core::Modules {

class JsRuntime {
public:
    struct Callbacks {
        std::function<void()>                                                       onLoadStarting;
        std::function<void(std::string_view name, bool ok, std::string_view error)> onPluginResult;
        std::function<void()>                                                       onPluginsDone;
    };

    explicit JsRuntime(Callbacks callbacks);
    ~JsRuntime();

    JsRuntime(JsRuntime const&)            = delete;
    JsRuntime& operator=(JsRuntime const&) = delete;

    void attach(const Platform::EnvironmentContext& environment) const;
    void detach() const;

    [[nodiscard]] auto enqueue(std::shared_ptr<Plugin::JsPlugin> plugin) const -> Expected<void>;
    void               forget(std::string_view name) const;

    [[nodiscard]] auto loadPlugin(std::string_view name) const -> Expected<void>;
    [[nodiscard]] auto unloadPlugin(std::string_view name) const -> Expected<void>;
    [[nodiscard]] auto reloadPlugin(std::string_view name) const -> Expected<void>;

    struct State;

private:
    std::unique_ptr<State> mState;
};

} // namespace CloverNT::Core::Modules
