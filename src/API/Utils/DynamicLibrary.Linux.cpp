#include <CloverNT/API/Utils/DynamicLibrary.hpp>

#include <dlfcn.h>

namespace CloverNT::Utils {

auto DynamicLibrary::load(std::filesystem::path const& path, std::filesystem::path const& searchDirectory)
        -> Expected<void> {
    // dlopen has no per-call search dir; plugin-local deps must use RUNPATH/RPATH ($ORIGIN).
    (void) searchDirectory;
    if (mHandle != nullptr) {
        return unexpected(
                ErrorCategory::DynamicLibrary, CommonErrorCode::AlreadyLoaded, "dynamic library is already loaded");
    }

    auto handle = ::dlopen(path.c_str(), RTLD_NOW);
    if (handle == nullptr) {
        if (auto const* message = ::dlerror(); message != nullptr) {
            return unexpected(ErrorCategory::DynamicLibrary, CommonErrorCode::OperationFailed, message);
        }
        return unexpected(ErrorCategory::DynamicLibrary, CommonErrorCode::OperationFailed, "dlopen failed");
    }
    mHandle = handle;
    return {};
}

auto DynamicLibrary::close() -> Expected<void> {
    if (mHandle == nullptr) {
        return {};
    }
    if (::dlclose(mHandle) != 0) {
        if (auto const* message = ::dlerror(); message != nullptr) {
            return unexpected(ErrorCategory::DynamicLibrary, CommonErrorCode::OperationFailed, message);
        }
        return unexpected(ErrorCategory::DynamicLibrary, CommonErrorCode::OperationFailed, "dlclose failed");
    }
    mHandle = nullptr;
    return {};
}

auto DynamicLibrary::getSymbolAddress(std::string_view name) const -> Expected<void*> {
    if (mHandle == nullptr) {
        return unexpected(ErrorCategory::DynamicLibrary, CommonErrorCode::NotLoaded, "dynamic library is not loaded");
    }
    ::dlerror();
    auto* address = ::dlsym(mHandle, std::string(name).c_str());
    if (auto const* message = ::dlerror(); message != nullptr) {
        return unexpected(ErrorCategory::DynamicLibrary, CommonErrorCode::NotFound, message);
    }
    return address;
}

} // namespace CloverNT::Utils
