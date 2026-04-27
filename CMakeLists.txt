cmake_minimum_required(VERSION 3.27)

project(DonCraft VERSION 0.1.0 LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not supported. Use one of the configured presets so generated files stay under build/.")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(Don_Craft_ENABLE_TESTS "Build the self-check executable and enable CTest." ON)
option(Don_Craft_CREATE_LOCAL_STEAM_APPID "Copy steam_appid.txt=480 beside local developer builds." ON)
option(Don_Craft_BUILD_CLIENT "Build the SDL3/Vulkan/Steam client target." ON)
option(Don_Craft_BUILD_SERVER "Build the headless Steam dedicated server target." ON)
option(Don_Craft_BUILD_LAUNCHER "Build the Windows auto-update launcher target." ON)
set(Don_Craft_SDL3_SOURCE_DIR "C:/SDL3-3.4.4" CACHE PATH "Path to an SDL3 source tree to build as a subproject. If empty or invalid, a packaged SDL3 install will be searched.")
set(Don_Craft_UPDATE_MANIFEST_URL "https://raw.githubusercontent.com/Ronnie-Reagan/DonCraft/main/dist/update/alpha/manifest.json" CACHE STRING "Default GitHub raw manifest URL used by DonCraftLauncher.")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

function(df_configure_target target_name)
    target_compile_features(${target_name} PUBLIC cxx_std_20)

    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
        target_compile_definitions(${target_name} PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
    else()
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

if(WIN32 AND Don_Craft_BUILD_LAUNCHER)
    add_executable(DonCraftLauncher WIN32
        src/launcher/launcher_main.cpp
    )
    target_compile_definitions(DonCraftLauncher PRIVATE
        DON_CRAFT_DEFAULT_UPDATE_MANIFEST_URL="${Don_Craft_UPDATE_MANIFEST_URL}"
    )
    target_link_libraries(DonCraftLauncher PRIVATE winhttp bcrypt shell32)
    set_target_properties(DonCraftLauncher PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "$(TargetDir)"
    )
    df_configure_target(DonCraftLauncher)
endif()

add_library(df_core STATIC
    src/core/assert.hpp
    src/core/bit_packer.hpp
    src/core/config.hpp
    src/core/fixed_step_clock.hpp
    src/core/job_system.cpp
    src/core/job_system.hpp
    src/core/log.cpp
    src/core/log.hpp
    src/core/math.hpp
    src/core/profiler.cpp
    src/core/profiler.hpp
    src/core/timer.hpp
)
target_include_directories(df_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
df_configure_target(df_core)

add_library(df_world STATIC
    src/world/demo_world.cpp
    src/world/demo_world.hpp
    src/world/material.hpp
    src/world/material_field.cpp
    src/world/material_field.hpp
    src/world/material_properties.hpp
)
target_link_libraries(df_world PUBLIC df_core)
df_configure_target(df_world)

add_library(df_net STATIC
    src/net/chunk_delta.cpp
    src/net/chunk_delta.hpp
    src/net/session_client.cpp
    src/net/session_client.hpp
    src/net/session_host.cpp
    src/net/session_host.hpp
    src/net/session_protocol.cpp
    src/net/session_protocol.hpp
    src/net/transport.hpp
)
add_library(df_game STATIC
    src/game/construction.cpp
    src/game/construction.hpp
    src/game/control_state.hpp
    src/game/debug_hud.cpp
    src/game/debug_hud.hpp
    src/game/model_primitives.cpp
    src/game/model_primitives.hpp
    src/game/player_controller.cpp
    src/game/player_controller.hpp
    src/game/session_runtime.cpp
    src/game/session_runtime.hpp
    src/game/session_types.hpp
    src/game/truck_controller.cpp
    src/game/truck_controller.hpp
)
target_link_libraries(df_game PUBLIC df_core df_world)
df_configure_target(df_game)

target_link_libraries(df_net PUBLIC df_core df_world df_game)
df_configure_target(df_net)

add_library(df_sim STATIC
    src/sim/reference_mpm.cpp
    src/sim/reference_mpm.hpp
    src/sim/vulkan_mpm_2d.cpp
    src/sim/vulkan_mpm_2d.hpp
)
target_link_libraries(df_sim PUBLIC df_core df_world)
df_configure_target(df_sim)

file(GLOB_RECURSE Don_Craft_INVALID_SHADER_SOURCES
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
    CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/*.hpp"
)
if(Don_Craft_INVALID_SHADER_SOURCES)
    list(JOIN Don_Craft_INVALID_SHADER_SOURCES ", " Don_Craft_INVALID_SHADER_LIST)
    message(FATAL_ERROR "assets/shaders may only contain runtime shader assets. Remove the following C++ duplicates: ${Don_Craft_INVALID_SHADER_LIST}")
endif()

if(Don_Craft_BUILD_CLIENT OR Don_Craft_BUILD_SERVER)
    find_package(Steamworks REQUIRED)

    if(Don_Craft_CREATE_LOCAL_STEAM_APPID)
        file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/steam_appid.txt" CONTENT "480\n")
    endif()

    add_library(df_steam STATIC
        src/steam/steam_client.cpp
        src/steam/steam_client.hpp
        src/steam/steam_server.cpp
        src/steam/steam_server.hpp
        src/steam/steam_transport.cpp
        src/steam/steam_transport.hpp
    )
    target_link_libraries(df_steam
        PUBLIC df_core df_net Steamworks::Steamworks
    )
    if(WIN32)
        target_link_libraries(df_steam PUBLIC ws2_32)
    endif()
    df_configure_target(df_steam)
endif()

if(Don_Craft_BUILD_CLIENT)
    find_package(Vulkan REQUIRED)

    set(Don_Craft_SDL_TARGET "")
    if(EXISTS "${Don_Craft_SDL3_SOURCE_DIR}/CMakeLists.txt")
        message(STATUS "Using SDL3 source tree from ${Don_Craft_SDL3_SOURCE_DIR}")

        set(SDL_INSTALL OFF CACHE BOOL "Disable SDL3 install rules when used as a DonCraft subproject." FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "Disable SDL3_test when building SDL3 as a DonCraft subproject." FORCE)
        set(SDL_TESTS OFF CACHE BOOL "Disable SDL3 test targets when building SDL3 as a DonCraft subproject." FORCE)

        add_subdirectory(
            "${Don_Craft_SDL3_SOURCE_DIR}"
            "${CMAKE_CURRENT_BINARY_DIR}/third_party/SDL3"
            EXCLUDE_FROM_ALL
        )
    else()
        message(STATUS "SDL3 source tree was not found at '${Don_Craft_SDL3_SOURCE_DIR}'. Falling back to find_package(SDL3 CONFIG).")
        find_package(SDL3 CONFIG REQUIRED)
    endif()

    if(TARGET SDL3::SDL3)
        set(Don_Craft_SDL_TARGET SDL3::SDL3)
    elseif(TARGET SDL3::SDL3-shared)
        set(Don_Craft_SDL_TARGET SDL3::SDL3-shared)
    elseif(TARGET SDL3::SDL3-static)
        set(Don_Craft_SDL_TARGET SDL3::SDL3-static)
    else()
        message(FATAL_ERROR "SDL3 was found, but no supported imported target was exported. Expected SDL3::SDL3, SDL3::SDL3-shared, or SDL3::SDL3-static.")
    endif()

    if(DEFINED Vulkan_GLSLC_EXECUTABLE AND EXISTS "${Vulkan_GLSLC_EXECUTABLE}")
        set(Don_Craft_GLSLC_EXECUTABLE "${Vulkan_GLSLC_EXECUTABLE}")
    else()
        find_program(Don_Craft_GLSLC_EXECUTABLE
            NAMES glslc
            HINTS
                "$ENV{VULKAN_SDK}/Bin"
                "$ENV{VULKAN_SDK}/Bin32"
        )
    endif()

    if(NOT Don_Craft_GLSLC_EXECUTABLE)
        message(FATAL_ERROR "glslc was not found. Install the Vulkan SDK or add glslc to PATH so shaders can be compiled.")
    endif()

    function(df_compile_shaders out_var)
        set(shader_outputs "")
        foreach(shader_path IN LISTS ARGN)
            get_filename_component(shader_name "${shader_path}" NAME)
            set(shader_output "${CMAKE_CURRENT_BINARY_DIR}/generated/shaders/${shader_name}.spv")

            add_custom_command(
                OUTPUT "${shader_output}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated/shaders"
                COMMAND "${Don_Craft_GLSLC_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/${shader_path}" -o "${shader_output}"
                DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${shader_path}"
                VERBATIM
            )

            list(APPEND shader_outputs "${shader_output}")
        endforeach()

        set(${out_var} "${shader_outputs}" PARENT_SCOPE)
    endfunction()

    set(Don_Craft_SHADER_SOURCES
        assets/shaders/mpm_clear_nodes.comp
        assets/shaders/mpm_advect_particles.comp
        assets/shaders/mpm_scatter_nodes.comp
        assets/shaders/mpm_update_nodes.comp
        assets/shaders/shadow_depth.vert
        assets/shaders/world_3d.vert
        assets/shaders/world_3d.frag
        assets/shaders/world_unshadowed.frag
        assets/shaders/oit_weighted_shadowed.frag
        assets/shaders/oit_weighted_unshadowed.frag
        assets/shaders/oit_composite.vert
        assets/shaders/oit_composite.frag
        assets/shaders/overlay_2d.vert
        assets/shaders/overlay_2d.frag
    )
    df_compile_shaders(Don_Craft_SHADER_OUTPUTS ${Don_Craft_SHADER_SOURCES})
    add_custom_target(df_shaders DEPENDS ${Don_Craft_SHADER_OUTPUTS})

    add_library(df_platform STATIC
        src/platform/audio_device.cpp
        src/platform/audio_device.hpp
        src/platform/filesystem.cpp
        src/platform/filesystem.hpp
        src/platform/input_state.hpp
        src/platform/sdl_platform.cpp
        src/platform/sdl_platform.hpp
    )
    target_link_libraries(df_platform
        PUBLIC df_core ${Don_Craft_SDL_TARGET}
    )
    df_configure_target(df_platform)

    add_library(df_render STATIC
        src/render/frame_data.hpp
        src/render/vulkan_renderer.cpp
        src/render/vulkan_renderer.hpp
    )
    target_link_libraries(df_render
        PUBLIC df_core ${Don_Craft_SDL_TARGET} Vulkan::Vulkan
    )
    df_configure_target(df_render)

    add_library(df_session_view STATIC
        src/game/session_render.cpp
        src/game/session_render.hpp
    )
    target_link_libraries(df_session_view
        PUBLIC df_game df_net
    )
    df_configure_target(df_session_view)

    target_link_libraries(df_sim PUBLIC Vulkan::Vulkan)
    target_compile_definitions(df_sim PUBLIC DF_ENABLE_VULKAN_MPM=1)

    add_library(df_app STATIC
        src/app/application.cpp
        src/app/application.hpp
    )
    target_link_libraries(df_app
        PUBLIC df_core df_render df_steam
        PRIVATE df_platform df_session_view df_sim
    )
    df_configure_target(df_app)

    add_executable(Don_Craft_client
        src/app/main.cpp
    )
    target_link_libraries(Don_Craft_client PRIVATE df_app)
    df_configure_target(Don_Craft_client)
    add_dependencies(Don_Craft_client df_shaders)
    set_target_properties(Don_Craft_client PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "$(TargetDir)"
    )

    add_custom_command(TARGET Don_Craft_client POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${Steamworks_DLL}"
            "$<TARGET_FILE_DIR:Don_Craft_client>/steam_api64.dll"
    )

    add_custom_command(TARGET Don_Craft_client POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:Don_Craft_client>/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${Don_Craft_SHADER_OUTPUTS}
            "$<TARGET_FILE_DIR:Don_Craft_client>/shaders"
    )

    add_custom_command(TARGET Don_Craft_client POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/DonCraft.ini"
            "$<TARGET_FILE_DIR:Don_Craft_client>/DonCraft.ini"
    )

    if(Don_Craft_CREATE_LOCAL_STEAM_APPID)
        add_custom_command(TARGET Don_Craft_client POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_BINARY_DIR}/generated/steam_appid.txt"
                "$<TARGET_FILE_DIR:Don_Craft_client>/steam_appid.txt"
        )
    endif()
endif()

if(Don_Craft_BUILD_SERVER)
    add_library(df_server_app STATIC
        src/app/server_application.cpp
        src/app/server_application.hpp
    )
    target_link_libraries(df_server_app
        PUBLIC df_core df_steam
        PRIVATE df_game df_net df_world df_sim
    )
    df_configure_target(df_server_app)

    add_executable(Don_Craft_server
        src/app/server_main.cpp
    )
    target_link_libraries(Don_Craft_server PRIVATE df_server_app)
    df_configure_target(Don_Craft_server)
    set_target_properties(Don_Craft_server PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "$(TargetDir)"
    )

    add_custom_command(TARGET Don_Craft_server POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${Steamworks_DLL}"
            "$<TARGET_FILE_DIR:Don_Craft_server>/steam_api64.dll"
    )

    if(Don_Craft_CREATE_LOCAL_STEAM_APPID)
        add_custom_command(TARGET Don_Craft_server POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_BINARY_DIR}/generated/steam_appid.txt"
                "$<TARGET_FILE_DIR:Don_Craft_server>/steam_appid.txt"
        )
    endif()
endif()

if(Don_Craft_ENABLE_TESTS)
    include(CTest)

    add_executable(Don_Craft_selfcheck
        tests/selfcheck_main.cpp
    )
    target_link_libraries(Don_Craft_selfcheck PRIVATE df_core df_world df_game df_net df_sim)
    df_configure_target(Don_Craft_selfcheck)

    if(TARGET df_shaders)
        add_dependencies(Don_Craft_selfcheck df_shaders)
        add_custom_command(TARGET Don_Craft_selfcheck POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:Don_Craft_selfcheck>/shaders"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${Don_Craft_SHADER_OUTPUTS}
                "$<TARGET_FILE_DIR:Don_Craft_selfcheck>/shaders"
        )
    endif()

    add_test(NAME Don_Craft_selfcheck COMMAND Don_Craft_selfcheck)
endif()
