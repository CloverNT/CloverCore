#pragma once

#include <concepts>

#include <CloverNT/API/Macros.hpp>
#include <CloverNT/API/Plugin/NativePlugin.hpp>
#include <CloverNT/API/Plugin/PluginBase.hpp>

namespace CloverNT::Plugin::Detail {

template <class T>
concept LoadablePlugin = requires(T value) {
    { value.load() } -> std::same_as<bool>;
};

template <class T>
concept UnloadablePlugin = requires(T value) {
    { value.unload() } -> std::same_as<bool>;
};

template <class T>
void bindPlugin(T& plugin, NativePlugin& self) {
    if constexpr (std::derived_from<T, PluginBase>) {
        plugin.bindSelf(self);
    } else {
        (void) self;
    }
}

template <class T>
void unbindPlugin(T& plugin) {
    if constexpr (std::derived_from<T, PluginBase>) {
        plugin.clearSelf();
    } else {
        (void) plugin;
    }
}

template <class T>
bool invokeUnload(T& plugin) {
    if constexpr (UnloadablePlugin<T>) {
        return plugin.unload();
    } else {
        (void) plugin;
        return true;
    }
}

} // namespace CloverNT::Plugin::Detail

#define CloverNT_REGISTER_PLUGIN(TYPE)                                                                                 \
    namespace {                                                                                                        \
        TYPE& clovernt_plugin_instance() {                                                                             \
            static TYPE instance{};                                                                                    \
            return instance;                                                                                           \
        }                                                                                                              \
        [[maybe_unused]] bool clovernt_plugin_unload_thunk() {                                                         \
            auto&      instance = clovernt_plugin_instance();                                                          \
            const bool ok       = ::CloverNT::Plugin::Detail::invokeUnload(instance);                                  \
            ::CloverNT::Plugin::Detail::unbindPlugin(instance);                                                        \
            return ok;                                                                                                 \
        }                                                                                                              \
    }                                                                                                                  \
    extern "C" CloverNT_EXPORT bool clovernt_plugin_load(::CloverNT::Plugin::NativePlugin& self) {                     \
        static_assert(::CloverNT::Plugin::Detail::LoadablePlugin<TYPE>,                                                \
                      "CloverNT_REGISTER_PLUGIN requires the plugin type to define `bool load()`");                    \
        auto& instance = clovernt_plugin_instance();                                                                   \
        ::CloverNT::Plugin::Detail::bindPlugin(instance, self);                                                        \
        bool clovernt_plugin_loaded = false;                                                                           \
        try {                                                                                                          \
            clovernt_plugin_loaded = instance.load();                                                                  \
        } catch (...) {                                                                                                \
            ::CloverNT::Plugin::Detail::unbindPlugin(instance);                                                        \
            throw;                                                                                                     \
        }                                                                                                              \
        if (!clovernt_plugin_loaded) {                                                                                 \
            ::CloverNT::Plugin::Detail::unbindPlugin(instance);                                                        \
            return false;                                                                                              \
        }                                                                                                              \
        if constexpr (::CloverNT::Plugin::Detail::UnloadablePlugin<TYPE>) {                                            \
            ::CloverNT::Plugin::setUnloadHandler(self, &clovernt_plugin_unload_thunk);                                 \
        }                                                                                                              \
        return true;                                                                                                   \
    }
