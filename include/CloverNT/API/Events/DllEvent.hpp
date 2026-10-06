#pragma once

#include <CloverNT/API/Events/Event.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace CloverNT::Event {

class DllLoadEvent final : public Event {
public:
    static constexpr std::string_view kEventName = "CloverNT.DllLoadEvent";

    DllLoadEvent(std::string baseName, std::string fullPath, void* baseAddress, std::size_t imageSize) noexcept
        : mBaseName(std::move(baseName)),
          mFullPath(std::move(fullPath)),
          mBaseAddress(baseAddress),
          mImageSize(imageSize) {}

    [[nodiscard]] auto baseName() const noexcept -> std::string const& {
        return mBaseName;
    }
    [[nodiscard]] auto fullPath() const noexcept -> std::string const& {
        return mFullPath;
    }
    [[nodiscard]] auto baseAddress() const noexcept -> void* {
        return mBaseAddress;
    }
    [[nodiscard]] auto imageSize() const noexcept -> std::size_t {
        return mImageSize;
    }

private:
    std::string mBaseName;
    std::string mFullPath;
    void*       mBaseAddress{};
    std::size_t mImageSize{};
};

} // namespace CloverNT::Event
