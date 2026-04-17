set(_df_default_steamworks_root "C:/Steamworks/steamworks_sdk_164/sdk")

if(NOT DEFINED STEAMWORKS_SDK_ROOT)
    if(DEFINED ENV{STEAMWORKS_SDK_ROOT})
        set(STEAMWORKS_SDK_ROOT "$ENV{STEAMWORKS_SDK_ROOT}" CACHE PATH "Path to the Steamworks SDK root directory.")
    else()
        set(STEAMWORKS_SDK_ROOT "${_df_default_steamworks_root}" CACHE PATH "Path to the Steamworks SDK root directory.")
    endif()
endif()

find_path(Steamworks_INCLUDE_DIR
    NAMES steam/steam_api.h
    PATHS
        "${STEAMWORKS_SDK_ROOT}/public"
)

if(WIN32)
    find_library(Steamworks_LIBRARY
        NAMES steam_api64
        PATHS
            "${STEAMWORKS_SDK_ROOT}/redistributable_bin/win64"
    )

    find_file(Steamworks_DLL
        NAMES steam_api64.dll
        PATHS
            "${STEAMWORKS_SDK_ROOT}/redistributable_bin/win64"
    )
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Steamworks
    REQUIRED_VARS
        STEAMWORKS_SDK_ROOT
        Steamworks_INCLUDE_DIR
        Steamworks_LIBRARY
        Steamworks_DLL
    FAIL_MESSAGE
        "Steamworks SDK was not found. Install the SDK and set STEAMWORKS_SDK_ROOT to the SDK root, for example C:/Steamworks/steamworks_sdk_164/sdk."
)

if(Steamworks_FOUND AND NOT TARGET Steamworks::Steamworks)
    add_library(Steamworks::Steamworks SHARED IMPORTED)
    set_target_properties(Steamworks::Steamworks PROPERTIES
        IMPORTED_IMPLIB "${Steamworks_LIBRARY}"
        IMPORTED_LOCATION "${Steamworks_DLL}"
        INTERFACE_INCLUDE_DIRECTORIES "${Steamworks_INCLUDE_DIR}"
    )
endif()

mark_as_advanced(
    Steamworks_INCLUDE_DIR
    Steamworks_LIBRARY
    Steamworks_DLL
)
