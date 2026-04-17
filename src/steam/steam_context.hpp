#pragma once

#include <cstdint>
#include <string>

namespace df::steam
{
class SteamContext
{
public:
    SteamContext() = default;
    ~SteamContext();

    SteamContext(const SteamContext&) = delete;
    SteamContext& operator=(const SteamContext&) = delete;

    [[nodiscard]] bool Initialize();
    void Shutdown();
    void PumpCallbacks();

    [[nodiscard]] bool IsInitialized() const
    {
        return initialized_;
    }

    [[nodiscard]] std::uint64_t LocalSteamId() const;
    [[nodiscard]] std::string PersonaName() const;
    [[nodiscard]] const std::string& FailureMessage() const
    {
        return failureMessage_;
    }

private:
    bool initialized_ = false;
    std::string failureMessage_;
};
}
