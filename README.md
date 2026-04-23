# DonCraft

DonCraft is a Windows-first C++20 deformable-material FPS project built around SDL3, Vulkan 1.3, and Steamworks. The runtime now supports four session modes through one shared gameplay/world core:

- `Offline`
- `ListenHost`
- `Client`
- `DedicatedServer`

That means the same authoritative world simulation can run inside the graphical client for offline or self-hosted play, or inside a headless dedicated server executable for persistent Steam-listed worlds.

## What is implemented

- Steam client startup MUST happens before SDL startup.
- A graphical client executable now has a real session front end:
  - selectable world slots with saved/empty status
  - `Continue Offline Slot N`
  - `New Offline World`
  - `Host Self-Hosted World` with load-saved/create-new setup
  - `Join World`
  - `Quit`
- Offline, listen-host, client, and dedicated server all run through a shared authoritative `SessionRuntime`.
- Steam listen-host discovery uses Steam lobbies.
- Headless dedicated discovery uses Steam GameServer APIs plus SteamNetworkingSockets.
- The world browser shows both listen-host lobbies and dedicated servers through one UI model.
- The in-session `Esc` menu now behaves correctly by mode:
  - offline pauses the simulation
  - online opens a local overlay without freezing the authoritative session
- The online session overlay includes resume, invite/browser, save, restart, and disconnect actions.
- Listen hosts can host and play in the same world.
- Dedicated servers run as a separate executable with no SDL, audio, or Vulkan dependency.
- World state replication covers:
  - handshake/welcome
  - command frames
  - full world snapshots
  - chunk/material deltas
  - actor snapshots
  - disconnect handling
- Client rendering now uses a 4-frame snapshot history:
  - remote actors and the truck render from a two-tick buffered snapshot path
  - the local predicted player replays up to 64 pending commands after authoritative correction
  - grenades, bullets, and beams still use the latest authoritative snapshot
- The server remains authoritative for terrain edits, actors, projectiles, and truck state.
- The Vulkan renderer now includes a single 2048x2048 directional shadow map with 3x3 PCF for terrain, players, truck geometry, and held-item meshes.
- Debug lines, UI/overlay elements, beams, and tracer effects remain unshadowed on purpose.
- Listen-host and dedicated sessions both persist world saves and support resume/autosave.
- The build now produces both:
  - `Don_Craft_client`
  - `Don_Craft_server`
- `build-server-debug` now builds both `Don_Craft_server` and `Don_Craft_selfcheck`, so `ctest --preset test-server-debug` is usable after the matching build preset.
- `assets/shaders/` is now limited to runtime shader assets, and CMake fails configure-time if `*.cpp` or `*.hpp` files drift back into that directory.
- Self-check coverage now includes protocol, browser entry, world snapshot, chunk-delta, save/resume, and in-memory host/client session validation.
- The loopback self-check now also covers remote interpolation, delayed authoritative correction with local command replay, and disconnect/reconnect recovery.

## Repository layout

- `cmake/`: custom dependency discovery for Steamworks.
- `src/core/`: low-level utilities used across the runtime.
- `src/platform/`: SDL bootstrap, window, input, and audio handling.
- `src/render/`: Vulkan renderer bootstrap and frame submission.
- `src/world/`: sparse chunked material field, terrain extraction, simulation, and persistence.
- `src/game/`: player/truck controllers, shared session runtime, render-model helpers, and HUD drawing.
- `src/net/`: protocol, chunk deltas, session host/client logic, and transport interfaces.
- `src/steam/`: Steam client context, dedicated server context, and SteamNetworkingSockets transport.
- `src/app/`: graphical client app plus headless dedicated server app.
- `tests/`: self-check executable source.

## Prerequisites

- Windows x64
- Visual Studio 2022 with the C++ toolchain
- CMake 3.27+
- Vulkan SDK installed and `VULKAN_SDK` set for client builds
- SDL3 source tree available at `C:\SDL3-3.4.4`, or an installed SDL3 package discoverable through CMake, for client builds
- Steamworks SDK installed at `C:\Steamworks\steamworks_sdk_164\sdk`, or pass `-DSTEAMWORKS_SDK_ROOT=...`
- Steam running on the same machine for client or dedicated Steam-backed sessions

## Configure and build

Default debug and release presets build both the graphical client and the dedicated server:

```powershell
cmake --preset vs2022-debug
cmake --build --preset build-debug
ctest --preset test-debug
```

```powershell
cmake --preset vs2022-release
cmake --build --preset build-release
```

Dedicated-only presets are also available:

```powershell
cmake --preset vs2022-server-debug
cmake --build --preset build-server-debug
ctest --preset test-server-debug
```

```powershell
cmake --preset vs2022-server-release
cmake --build --preset build-server-release
```

For serialization/world/net/sim validation without client or server targets:

```powershell
cmake --preset vs2022-selfcheck
cmake --build --preset build-selfcheck
ctest --preset test-selfcheck
```

### Python build helper

The helper now builds and stages both runtime executables by default when both are enabled:

```powershell
python build_DonCraft.py
```

Useful variants:

```powershell
python build_DonCraft.py --configure-preset vs2022-debug --ctest
python build_DonCraft.py --configure-preset vs2022-server-debug
python build_DonCraft.py --configure-preset vs2022-debug --no-build-server
python build_DonCraft.py --configure-preset vs2022-server-debug --no-build-client
python build_DonCraft.py --configure-preset vs2022-release --fresh --clean-first
python build_DonCraft.py --target Don_Craft_server --no-package
```

By default, staged runtime packages are written under:

- `build/package/<config>/Don_Craft_client/`
- `build/package/<config>/Don_Craft_server/`

The build output keeps `steam_api64.dll` and a local development `steam_appid.txt` beside both executables for local runs. Staged packages include `steam_appid.txt` when CMake created it, and otherwise only include runtime files for that target.

### Launcher and GitHub updates

Release builds also produce `DonCraftLauncher.exe`. The Python build helper stages the tracked distribution tree by default:

- `dist/launcher/windows-x64/DonCraftLauncher.exe`
- `dist/update/alpha/manifest.json`
- `dist/update/alpha/files/`

The launcher downloads `dist/update/alpha/manifest.json` from the GitHub `main` branch, validates every runtime file by SHA-256, updates `DonCraftRuntime/` beside the launcher, and then starts `Don_Craft_client.exe`. This keeps the launcher compatible with Steam: set the Steam launch executable to `DonCraftLauncher.exe`; the launcher waits for the game process and forwards normal launch arguments.

For an alpha update, run a release build, commit the rewritten `dist/` tree, and push `main`:

```powershell
python build_DonCraft.py --configure-preset vs2022-release --fresh --clean-first
git add dist
git commit -m "Publish alpha build"
git push origin main
```

Use `--no-dist` for local builds that should not rewrite the tracked launcher/update payloads. The generated manifest uses repository-relative file paths only; it does not include local build directories or Windows user paths.

## Running the client

Launch `Don_Craft_client.exe` from the build output. The client opens into the main menu and supports:

- offline resume/play
- self-hosted Steam lobby creation
- world browsing and joining
- online in-session overlay menus

### Controls

- `WASD`: move
- `Shift`: sprint
- `Space`: jump
- Mouse: look
- `1`: rifle
- `2`: grenade tool
- `3`: dig tool
- Left mouse: use current tool
- `G`: quick-throw grenade
- `E`: enter or exit the truck
- `F2`: toggle active chunk visualization
- `F3`: toggle terrain wireframe
- `C`: toggle third-person rendering
- `Esc`: open or close the current menu/overlay
- `Up` and `Down`: change menu selection
- `Left` and `Right`: adjust host-world setup values
- `Enter`: confirm the highlighted action
- `R`: refresh the world browser when the browser is open

## Running the dedicated server

Launch `Don_Craft_server.exe` from the build output. The server is headless and uses Steam GameServer registration plus SteamNetworkingSockets.

Example:

```powershell
Don_Craft_server.exe ^
  --name "DonCraft Dedicated" ^
  --save ".\server_world.bin" ^
  --game-port 8080 ^
  --query-port 8081 ^
  --max-players 8 ^
  --autosave 20 ^
  --visibility public
```

Useful options:

- `--name <server name>`
- `--save <world save path>`
- `--max-players <count>`
- `--autosave <seconds>`
- `--game-port <port>`
- `--query-port <port>`
- `--tick-rate <hz>`
- `--visibility <public|private>`
- `--world-width <cells>`
- `--world-height <cells>`
- `--world-depth <cells>`
- `--active-chunk-size <cells>`
- `--cell-size <meters>`
- `--seed <uint32>`
- `--terrain-relief <scalar>`
- `--water-level <0..1>`
- `--region <steam region>`
- `--tags <comma separated tags>`
- `--map-name <server browser map name>`
- `--new-world`

## Current limits

This repo now implements the multiplayer and dedicated-hosting plan at the runtime/build level, but a few intentional limits remain:

- No host migration. Listen sessions end when the host leaves.
- Dedicated administration is CLI/log based only.
- No RCON, voice chat, or anti-cheat work is included.
- Client reconciliation only predicts the local on-foot player; truck prediction is still authoritative-only for this pass.
- Grenades, bullets, and beams still render from the latest authoritative snapshot instead of the buffered interpolation path.
- Directional shadows use a single non-cascaded 2048x2048 map, and debug-only shader hot reload is not implemented in this pass.
- Terrain replication is authoritative via snapshots and chunk deltas rather than raw particle replication.
