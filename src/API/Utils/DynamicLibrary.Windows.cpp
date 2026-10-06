#include <CloverNT/API/Utils/DynamicLibrary.hpp>

#include <mutex>

#include <Windows.h>

namespace CloverNT::Utils {
namespace {

    [[nodiscard]] auto windowsError(std::string message, const DWORD code) -> std::unexpected<Error> {
        return unexpected(ErrorCategory::DynamicLibrary,
                          std::move(message) + " failed with code " + std::to_string(code),
                          static_cast<std::int32_t>(code),
                          code);
    }

    [[nodiscard]] auto dllLoadMutex() -> std::mutex& {
        static std::mutex mutex;
        return mutex;
    }

} // namespace

auto DynamicLibrary::load(std::filesystem::path const& path, std::filesystem::path const& searchDirectory)
        -> Expected<void> {
    if (mHandle != nullptr) {
        return unexpected(
                ErrorCategory::DynamicLibrary, CommonErrorCode::AlreadyLoaded, "dynamic library is already loaded");
    }

    std::lock_guard lock(dllLoadMutex());

    DLL_DIRECTORY_COOKIE cookie{};
    if (!searchDirectory.empty()) {
        cookie = AddDllDirectory(searchDirectory.wstring().c_str());
        if (cookie == nullptr) {
            const auto code = GetLastError();
            return windowsError("AddDllDirectory", code);
        }
    }

    auto flags = static_cast<DWORD>(LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (cookie != nullptr) {
        flags |= LOAD_LIBRARY_SEARCH_USER_DIRS;
    }

    const auto handle   = LoadLibraryExW(path.wstring().c_str(), nullptr, flags);
    const auto loadCode = handle == nullptr ? GetLastError() : ERROR_SUCCESS;

    if (cookie != nullptr) {
        RemoveDllDirectory(cookie);
    }

    if (handle == nullptr) {
        return unexpected(ErrorCategory::DynamicLibrary,
                          "LoadLibraryExW failed with code " + std::to_string(loadCode),
                          static_cast<std::int32_t>(loadCode),
                          loadCode);
    }

    mHandle = handle;
    return {};
}

auto DynamicLibrary::close() -> Expected<void> {
    if (mHandle == nullptr) {
        return {};
    }
    if (FreeLibrary(static_cast<HMODULE>(mHandle)) == 0) {
        const auto code = GetLastError();
        return unexpected(ErrorCategory::DynamicLibrary,
                          "FreeLibrary failed with code " + std::to_string(code),
                          static_cast<std::int32_t>(code),
                          code);
    }
    mHandle = nullptr;
    return {};
}

auto DynamicLibrary::getSymbolAddress(const std::string_view name) const -> Expected<void*> {
    if (mHandle == nullptr) {
        return unexpected(ErrorCategory::DynamicLibrary, CommonErrorCode::NotLoaded, "dynamic library is not loaded");
    }
    auto* address = reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(mHandle), std::string(name).c_str()));
    if (address == nullptr) {
        const auto code = GetLastError();
        return unexpected(ErrorCategory::DynamicLibrary,
                          "symbol not found: " + std::string(name),
                          static_cast<std::int32_t>(code),
                          code);
    }
    return address;
}

} // namespace CloverNT::Utils
