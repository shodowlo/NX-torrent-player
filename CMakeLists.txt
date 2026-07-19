cmake_minimum_required(VERSION 3.15)

# ---------------------------------------------------------------------------
# NX Torrent Player — borealis (official Horizon-style UI) front-end.
# Modeled on borealis/demo/CMakeLists.txt, Switch target only.
# ---------------------------------------------------------------------------

set(BOREALIS_LIBRARY ${CMAKE_CURRENT_SOURCE_DIR}/library/borealis/library)

include(${BOREALIS_LIBRARY}/cmake/commonOption.cmake)

option(USE_SHARED_LIB "" OFF)
cmake_dependent_option(USE_SYSTEM_FMT "" OFF "NOT USE_SHARED_LIB" ON)
cmake_dependent_option(USE_SYSTEM_TINYXML2 "" OFF "NOT USE_SHARED_LIB" ON)
cmake_dependent_option(USE_SYSTEM_TWEENY "" OFF "NOT USE_SHARED_LIB" ON)

include(${BOREALIS_LIBRARY}/cmake/toolchain.cmake)

project(NX-torrent-player)
set(VERSION_MAJOR "0")
set(VERSION_MINOR "1")
set(VERSION_ALTER "0")
set(PROJECT_AUTHOR "shodowlo")
# On-device app name shown in the Homebrew Menu.
set(APP_TITLE "NX Torrent Player")
set(PROJECT_TITLEID "0x010000000000A123")

# Where the in-app updater looks for a newer release. UPDATE_REPO is the GitHub
# "owner/name"; override UPDATE_API_URL to point the updater somewhere else
# entirely -- a local HTTP server serving a fake release JSON is how the update
# flow gets tested without publishing one:
#   cmake . -DUPDATE_API_URL=http://192.168.1.20:8000/latest.json
set(UPDATE_REPO "shodowlo/NX-torrent-player" CACHE STRING "GitHub owner/name for updates")
set(UPDATE_API_URL "https://api.github.com/repos/${UPDATE_REPO}/releases/latest"
    CACHE STRING "Release API endpoint the updater polls")
set(PROJECT_ICON ${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.jpg)
set(PROJECT_RESOURCES ${CMAKE_CURRENT_SOURCE_DIR}/library/borealis/resources)

add_subdirectory(${BOREALIS_LIBRARY} borealis)

find_package(Threads REQUIRED)
list(APPEND APP_PLATFORM_LIB ${CMAKE_THREAD_LIBS_INIT})

find_package(PkgConfig REQUIRED)
set(PKG_CONFIG_USE_STATIC_LIBS ON)  # Switch links everything statically
pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
# Metahub serves some Stremio posters as WebP, which nanovg's stb_image cannot
# decode; libwebp lets us convert them (see app/stremio.cpp).
pkg_check_modules(WEBP REQUIRED IMPORTED_TARGET libwebp)

# UI (app/, C++) + our torrent engine (engine/, C).
file(GLOB_RECURSE MAIN_SRC ${CMAKE_CURRENT_SOURCE_DIR}/app/*.cpp)
set(ENGINE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/engine)
# torrentfs3.c is the v3 engine (lighter: 3 threads, leech-only, one piece
# per session with parked-claim adoption). torrentfs2.c (v2) and torrentfs.c
# (v1) are kept in the tree for reference but not built (all three define the
# same torrentfs.h API -- swap the file here to switch engines).
set(ENGINE_SRC
    ${ENGINE_DIR}/bencode.c ${ENGINE_DIR}/torrent.c ${ENGINE_DIR}/torrentfs3.c
    ${ENGINE_DIR}/peer.c ${ENGINE_DIR}/swarm.c ${ENGINE_DIR}/magnet.c
    ${ENGINE_DIR}/udp_tracker.c ${ENGINE_DIR}/dht.c ${ENGINE_DIR}/dhtclient.c
    ${ENGINE_DIR}/udpdiag.c ${ENGINE_DIR}/stream.c
    ${ENGINE_DIR}/utp.cpp ${ENGINE_DIR}/utp_utils.cpp ${ENGINE_DIR}/utpbridge.cpp)
# libutp needs -DPOSIX for its socket types (SOCKADDR_STORAGE etc).
set_source_files_properties(${ENGINE_SRC} PROPERTIES COMPILE_DEFINITIONS "POSIX")
list(APPEND MAIN_SRC ${ENGINE_SRC})

if (PLATFORM_SWITCH)
    list(APPEND MAIN_SRC ${BOREALIS_LIBRARY}/lib/platforms/switch/switch_wrapper.c)
endif ()

program_target(${PROJECT_NAME} "${MAIN_SRC}")
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 17)

# A printf/snprintf whose arguments don't match its format string is always a
# bug, and on Switch it is a console-crashing one: a shifted %s dereferences an
# int as a pointer and data aborts. These were silently compiling before, so
# make them hard errors rather than warnings nobody reads. Scoped to our target
# so vendored borealis keeps building as-is.
target_compile_options(${PROJECT_NAME} PRIVATE -Wformat -Werror=format)

# One source of truth for the version: it names the release archive, is shown in
# Options, and is what the updater compares against the latest tag.
target_compile_definitions(${PROJECT_NAME} PRIVATE
    APP_VERSION="${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_ALTER}"
    UPDATE_API_URL="${UPDATE_API_URL}")

if (PLATFORM_SWITCH)
    # Bundle borealis' resources into the nro's romfs (borealis reads them from
    # "romfs:/"). The font is dropped — borealis uses the Switch shared system
    # font on device. This mirrors borealis/demo's Switch packaging.
    add_custom_target(${PROJECT_NAME}.nro DEPENDS ${PROJECT_NAME}
        COMMAND ${NX_NACPTOOL_EXE} --create "${APP_TITLE}" "${PROJECT_AUTHOR}" "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_ALTER}" ${PROJECT_NAME}.nacp --titleid=${PROJECT_TITLEID}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_RESOURCES} ${CMAKE_BINARY_DIR}/resources
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/resources/font
        # Bundle our loading-screen logo, list icon and the Stremio mark at romfs:/.
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/assets/logo.png ${CMAKE_BINARY_DIR}/resources/logo.png
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/assets/video-icon.png ${CMAKE_BINARY_DIR}/resources/video-icon.png
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/assets/stremio-icon.png ${CMAKE_BINARY_DIR}/resources/stremio-icon.png
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/assets/local-tab-icon.png ${CMAKE_BINARY_DIR}/resources/local-icon.png
        COMMAND ${NX_ELF2NRO_EXE} ${PROJECT_NAME}.elf ${PROJECT_NAME}.nro --icon=${PROJECT_ICON} --nacp=${PROJECT_NAME}.nacp --romfsdir=${CMAKE_BINARY_DIR}/resources
    )

endif ()

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/app
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
    ${CMAKE_CURRENT_SOURCE_DIR}/include  # libutp
    ${BOREALIS_LIBRARY}/include/borealis/extern       # glad/glad.h
    ${BOREALIS_LIBRARY}/lib/extern/glfw/include        # GLFW/glfw3.h
    ${BOREALIS_LIBRARY}/lib/extern/glfw/deps)          # stb_image_write.h
target_compile_options(${PROJECT_NAME} PRIVATE -ffunction-sections -fdata-sections ${APP_PLATFORM_OPTION})
target_link_libraries(${PROJECT_NAME} PRIVATE
    borealis
    PkgConfig::MPV
    PkgConfig::CURL
    PkgConfig::WEBP
    mbedtls mbedx509 mbedcrypto z
    ${APP_PLATFORM_LIB})
