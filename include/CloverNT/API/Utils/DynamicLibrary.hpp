#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include <CloverNT/API/Expected.hpp>
#include <CloverNT/API/Macros.hpp>

namespace CloverNT::Core::Modules {
class PluginRuntime;
}

namespace CloverNT::Utils {

class CloverNT_API DynamicLibrary {
public:
    DynamicLibrary() = default;
    ~DynamicLibrary();

    DynamicLibrary(DynamicLibrary const&)            = delete;
    DynamicLibrary& operator=(DynamicLibrary const&) = delete;

    DynamicLibrary(DynamicLibrary&& other) noexcept;
    DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;

    [[nodiscard]] auto load(std::filesystem::path const& path, std::filesystem::path const& searchDirectory)
            -> Expected<void>;

    [[nodiscard]] auto close() -> Expected<void>;

    [[nodiscard]] void* handle() const noexcept;
    [[nodiscard]] bool  isLoaded() const noexcept;
    [[nodiscard]] auto  getSymbolAddress(std::string_view name) const -> Expected<void*>;

    template <class T>
    [[nodiscard]] auto getSymbol(const std::string_view name) const -> Expected<T> {
        auto address = getSymbolAddress(name);
        if (!address) {
            return unexpected(address.error());
        }
        return reinterpret_cast<T>(address.value());
    }

private:
    friend class Core::Modules::PluginRuntime;

    // Dangerous final-shutdown escape hatch: drops ownership without closing the library.
    void releaseWithoutClose() noexcept;

    void* mHandle{};
};

} // namespace CloverNT::Utils
