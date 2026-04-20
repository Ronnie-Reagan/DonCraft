#pragma once

#include "core/math.hpp"
#include "game/session_types.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace df::game
{
enum class CartridgeType : std::uint8_t
{
    None = 0,
    Rifle762 = 1,
    Pistol9mm = 2,
};

struct CartridgeDefinition
{
    CartridgeType type = CartridgeType::None;
    std::string_view name = "N/A";
    float muzzleVelocity = 0.0f;
    float recoilPitchDegrees = 0.0f;
    float recoilYawDegrees = 0.0f;
    Vec4 tracerColor{1.0f, 1.0f, 1.0f, 1.0f};
};

struct WeaponDefinition
{
    ToolType tool = ToolType::Rifle;
    std::string_view name = "Rifle";
    CartridgeType cartridge = CartridgeType::None;
    bool automatic = false;
    bool supportsAds = false;
    bool usesScope = false;
    bool usesMagazine = false;
    int magazineCapacity = 0;
    int startingReserveAmmo = 0;
    float fireCooldownSeconds = 0.0f;
    float reloadDurationSeconds = 0.0f;
    float weaponCycleDecayRate = 6.5f;
    float viewModelKickDistance = 0.0f;
    float viewModelKickRise = 0.0f;
    float recoilPitchMultiplier = 0.0f;
    float recoilYawMultiplier = 0.0f;
    float defaultZoomMagnification = 1.0f;
    float minimumZoomMagnification = 1.0f;
    float maximumZoomMagnification = 1.0f;
};

struct WeaponAdsTuning
{
    Vec3 sightLocalPoint{};
    float eyeReliefMeters = 0.0f;
};

struct ScopeScreenTuning
{
    Vec3 localCenterPoint{};
    Vec2 localHalfSize{};
    Vec2 pixelOffset{};
    float paddingPixels = 0.0f;
    float fallbackScreenFraction = 0.0f;
    int fallbackMinSizePixels = 0;
    int fallbackMaxSizePixels = 0;
};

struct ScopeCameraTuning
{
    Vec3 localPosition{};
    Vec3 localFocalPoint{};
    float minimumTargetDistanceMeters = 0.0f;
    float farPlanePaddingMeters = 0.0f;
};

struct WeaponScopeTuning
{
    bool enabled = false;
    ScopeScreenTuning screen{};
    ScopeCameraTuning camera{};
};

struct WeaponViewTuning
{
    WeaponAdsTuning ads{};
    Vec3 muzzleLocalPoint{};
    WeaponScopeTuning scope{};
};

inline constexpr std::size_t kToolTypeCount = 4u;

[[nodiscard]] inline auto ToToolIndex(const ToolType tool) -> std::size_t
{
    switch (tool)
    {
    case ToolType::Rifle:
        return 0u;
    case ToolType::Grenade:
        return 1u;
    case ToolType::Dig:
        return 2u;
    case ToolType::Smg:
        return 3u;
    default:
        return 0u;
    }
}

[[nodiscard]] inline auto GetCartridgeDefinition(const CartridgeType type) -> CartridgeDefinition
{
    switch (type)
    {
    case CartridgeType::Rifle762:
        return {
            .type = CartridgeType::Rifle762,
            .name = "7.62 NATO",
            .muzzleVelocity = 128.0f,
            .recoilPitchDegrees = 0.50f,
            .recoilYawDegrees = 0.70f,
            .tracerColor = Vec4{1.0f, 0.84f, 0.44f, 1.0f},
        };
    case CartridgeType::Pistol9mm:
        return {
            .type = CartridgeType::Pistol9mm,
            .name = "9x19mm",
            .muzzleVelocity = 96.0f,
            .recoilPitchDegrees = 0.22f,
            .recoilYawDegrees = 0.45f,
            .tracerColor = Vec4{0.82f, 0.96f, 0.48f, 1.0f},
        };
    case CartridgeType::None:
    default:
        return {};
    }
}

[[nodiscard]] inline auto GetWeaponDefinition(const ToolType tool) -> WeaponDefinition
{
    switch (tool)
    {
    case ToolType::Rifle:
        return {
            .tool = ToolType::Rifle,
            .name = "RIFLE",
            .cartridge = CartridgeType::Rifle762,
            .automatic = false,
            .supportsAds = true,
            .usesScope = true,
            .usesMagazine = true,
            .magazineCapacity = 5,
            .startingReserveAmmo = 25,
            .fireCooldownSeconds = 0.34f,
            .reloadDurationSeconds = 2.35f,
            .weaponCycleDecayRate = 6.5f,
            .viewModelKickDistance = 0.14f,
            .viewModelKickRise = 0.028f,
            .recoilPitchMultiplier = 1.0f,
            .recoilYawMultiplier = 1.0f,
            .defaultZoomMagnification = 1.0f,
            .minimumZoomMagnification = 1.0f,
            .maximumZoomMagnification = 6.0f,
        };
    case ToolType::Smg:
        return {
            .tool = ToolType::Smg,
            .name = "SMG",
            .cartridge = CartridgeType::Pistol9mm,
            .automatic = true,
            .supportsAds = true,
            .usesScope = false,
            .usesMagazine = true,
            .magazineCapacity = 30,
            .startingReserveAmmo = 120,
            .fireCooldownSeconds = 0.09f,
            .reloadDurationSeconds = 1.80f,
            .weaponCycleDecayRate = 11.0f,
            .viewModelKickDistance = 0.075f,
            .viewModelKickRise = 0.016f,
            .recoilPitchMultiplier = 0.90f,
            .recoilYawMultiplier = 0.90f,
            .defaultZoomMagnification = 1.0f,
            .minimumZoomMagnification = 1.0f,
            .maximumZoomMagnification = 1.0f,
        };
    case ToolType::Grenade:
        return {
            .tool = ToolType::Grenade,
            .name = "GRENADE",
            .cartridge = CartridgeType::None,
            .automatic = false,
            .supportsAds = false,
            .usesScope = false,
            .usesMagazine = false,
            .weaponCycleDecayRate = 6.5f,
        };
    case ToolType::Dig:
    default:
        return {
            .tool = ToolType::Dig,
            .name = "DIG TOOL",
            .cartridge = CartridgeType::None,
            .automatic = false,
            .supportsAds = false,
            .usesScope = false,
            .usesMagazine = false,
            .weaponCycleDecayRate = 2.1f,
        };
    }
}

[[nodiscard]] inline auto GetWeaponViewTuning(const ToolType tool) -> WeaponViewTuning
{
    switch (tool)
    {
    case ToolType::Rifle:
        return {
            .ads =
                {
                    .sightLocalPoint = Vec3{0.0f, 0.064f, 0.150f},
                    .eyeReliefMeters = 0.11f,
                },
            .muzzleLocalPoint = Vec3{0.0f, -0.01f, 0.96f},
            .scope =
                {
                    .enabled = true,
                    .screen =
                        {
                            .localCenterPoint = Vec3{0.0f, 0.064f, -0.354f},
                            .localHalfSize = Vec2{0.128f, 0.104f},
                            .pixelOffset = Vec2{0.0f, 0.0f},
                            .paddingPixels = 3.0f,
                            .fallbackScreenFraction = 0.54f,
                            .fallbackMinSizePixels = 280,
                            .fallbackMaxSizePixels = 620,
                        },
                    .camera =
                        {
                            .localPosition = Vec3{0.0f, 0.064f, 0.354f},
                            .localFocalPoint = Vec3{0.0f, 0.064f, 1.354f},
                            .minimumTargetDistanceMeters = 48.0f,
                            .farPlanePaddingMeters = 64.0f,
                        },
                },
        };
    case ToolType::Smg:
        return {
            .ads =
                {
                    .sightLocalPoint = Vec3{0.0f, 0.095f, 0.150f},
                    .eyeReliefMeters = 0.18f,
                },
            .muzzleLocalPoint = Vec3{0.0f, -0.012f, 0.78f},
        };
    case ToolType::Grenade:
        return {
            .muzzleLocalPoint = Vec3{0.0f, 0.0f, 0.0f},
        };
    case ToolType::Dig:
    default:
        return {
            .muzzleLocalPoint = Vec3{0.0f, 0.0f, 0.0f},
        };
    }
}

[[nodiscard]] inline bool IsFirearmTool(const ToolType tool)
{
    return tool == ToolType::Rifle || tool == ToolType::Smg;
}
}
