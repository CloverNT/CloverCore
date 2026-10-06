#pragma once

#include <filesystem>
#include <string_view>

#include <CloverNT/API/Macros.hpp>

namespace CloverNT {

class Logger;

namespace Plugin {

    class NativePlugin;
    struct Manifest;

    // Base class a native plugin author inherits. Inheriting it binds the plugin instance to its
    // framework-owned NativePlugin and exposes convenient, bound accessors (logger, name, paths, ...),
    // so the lifecycle methods can be written argument-free:
    //
    //   class MyPlugin : public CloverNT::Plugin::PluginBase {
    //   public:
    //       bool load()   { logger().info("loaded from {}", directory().string()); return true; }
    //       bool unload() { return true; }   // optional: defining it makes the plugin unloadable
    //   };
    //   CloverNT_REGISTER_PLUGIN(MyPlugin)
    //
    // The lifecycle (load / optional unload) is detected at compile time via concepts in
    // RegisterHelper.hpp, not through virtual functions.
    class CloverNT_API PluginBase {
    public:
        PluginBase() = default;

        PluginBase(PluginBase const&)            = delete;
        PluginBase& operator=(PluginBase const&) = delete;
        PluginBase(PluginBase&&)                 = delete;
        PluginBase& operator=(PluginBase&&)      = delete;

        // Wired by the registration macro: bindSelf on load, clearSelf on (successful) unload.
        void bindSelf(NativePlugin& plugin) noexcept;
        void clearSelf() noexcept;

    protected:
        ~PluginBase() = default; // not meant to be deleted through a PluginBase pointer

        [[nodiscard]] auto self() const -> NativePlugin&; // throws if used before bindSelf

        [[nodiscard]] auto logger() const -> Logger&;
        [[nodiscard]] auto name() const -> std::string_view;
        [[nodiscard]] auto type() const -> std::string_view;
        [[nodiscard]] auto manifest() const -> Manifest const&;
        [[nodiscard]] auto directory() const -> std::filesystem::path const&;
        [[nodiscard]] auto dataDir() const -> std::filesystem::path;   // directory() / "data"
        [[nodiscard]] auto configDir() const -> std::filesystem::path; // directory() / "config"

    private:
        NativePlugin* mSelf{};
    };

} // namespace Plugin
} // namespace CloverNT
