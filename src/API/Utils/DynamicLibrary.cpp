#include <CloverNT/API/Utils/DynamicLibrary.hpp>

#include <utility>

namespace CloverNT::Utils {

DynamicLibrary::~DynamicLibrary() {
    try {
        (void) close();
    } catch (...) {}
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept : mHandle(std::exchange(other.mHandle, nullptr)) {}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
    if (this != &other) {
        try {
            (void) close();
        } catch (...) {}
        mHandle = std::exchange(other.mHandle, nullptr);
    }
    return *this;
}

void DynamicLibrary::releaseWithoutClose() noexcept {
    mHandle = nullptr;
}

void* DynamicLibrary::handle() const noexcept {
    return mHandle;
}

bool DynamicLibrary::isLoaded() const noexcept {
    return mHandle != nullptr;
}

} // namespace CloverNT::Utils
