#pragma once

#include "game/session_runtime.hpp"
#include "net/session_client.hpp"
#include "render/frame_data.hpp"

namespace df::game
{
struct SessionRenderOptions
{
    bool showWireframe = false;
    bool showActiveChunks = true;
    bool thirdPerson = false;
    bool aimDownSights = false;
    float zoomMagnification = 3.5f;
    float terrainDrawDistanceMeters = 500.0f;
    ToolType localTool = ToolType::Rifle;
    float localWeaponCycle = 0.0f;
};

[[nodiscard]] auto BuildRuntimeRenderData(
    SessionRuntime& runtime,
    PlayerId localPlayerId,
    const PlayerController* predictedLocalPlayer,
    const SessionRenderOptions& options,
    int viewportWidth,
    int viewportHeight) -> render::FrameRenderData;

[[nodiscard]] auto BuildClientRenderData(
    net::SessionClient& client,
    const SessionRenderOptions& options,
    int viewportWidth,
    int viewportHeight,
    float interpolationAlpha) -> render::FrameRenderData;
}
