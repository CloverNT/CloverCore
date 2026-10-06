#pragma once

#include <memory>
#include <string>

#include <nlohmann/json_fwd.hpp>

#include <CloverNT/API/Events/Cancellable.hpp>
#include <CloverNT/API/Events/Event.hpp>
#include <CloverNT/API/Macros.hpp>

namespace CloverNT::Event {

class CloverNT_API NamedEvent final : public Event, public Cancellable {
public:
    NamedEvent(std::string name, nlohmann::json payload);
    explicit NamedEvent(std::string name);
    ~NamedEvent() override;

    NamedEvent(NamedEvent const&)            = delete;
    NamedEvent& operator=(NamedEvent const&) = delete;

    [[nodiscard]] auto name() const noexcept -> std::string const&;
    [[nodiscard]] auto payload() const noexcept -> nlohmann::json const&;
    [[nodiscard]] auto payload() noexcept -> nlohmann::json&;
    [[nodiscard]] auto bridgeToken() const noexcept -> void*;
    void               setBridgeToken(void* token) noexcept;

private:
    std::string                     mName;
    std::unique_ptr<nlohmann::json> mPayload;
    void*                           mBridgeToken{};
};

} // namespace CloverNT::Event
