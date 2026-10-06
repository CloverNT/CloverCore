#pragma once

#include <string_view>

#include <CloverNT/API/Expected.hpp>

namespace CloverNT::Plugin {

class PluginLoader {
public:
    virtual ~PluginLoader() = default;

    [[nodiscard]] virtual auto id() const -> std::string_view = 0;

    [[nodiscard]] virtual auto load(std::string_view name) -> Expected<void>   = 0;
    [[nodiscard]] virtual auto unload(std::string_view name) -> Expected<void> = 0;
    [[nodiscard]] virtual auto reload(std::string_view name) -> Expected<void> = 0;
};

} // namespace CloverNT::Plugin
