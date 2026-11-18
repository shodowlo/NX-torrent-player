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
set(VERSION_MINOR "6")
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

# The .nro's file name, which is also the release asset the updater downloads:
# a release carries one .nro per console, and each build must pick its own --
# a Switch 1 binary does not run on Switch 2, nor the other way round.
if (PLATFORM_SWITCH2)
    set(NRO_NAME "NX2-torrent-player")
else ()
    set(NRO_NAME "NX-torrent-player")
endif ()
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

# Switch 2 (-DPLATFORM_SWITCH2=ON, ouncebrew devkit): the portlibs above are the
# Switch 1 ones, and their .pc files carry two Switch 1 leftovers to undo:
# - CMake resolves every -l against the .pc's own libdir (portlibs/switch), so
#   mpv would drag in switch-mesa's libEGL/libglapi/libdrm_nouveau (Maxwell
#   nouveau) next to OunceVK's. Any library portlibs/switch2 also ships is
#   swapped for its Switch 2 copy -- the order switch2vars.sh/switch2_rules use.
# - mpv's Cflags hard-code -march=armv8-a -mtune=cortex-a57, which would
#   override the toolchain's -mcpu=cortex-a78c.
if (PLATFORM_SWITCH2)
    foreach (pkg MPV CURL WEBP)
        get_target_property(_libs PkgConfig::${pkg} INTERFACE_LINK_LIBRARIES)
        set(_fixed "")
        foreach (_lib IN LISTS _libs)
            get_filename_component(_name "${_lib}" NAME)
            if (IS_ABSOLUTE "${_lib}" AND EXISTS "${DEVKITPRO}/portlibs/switch2/lib/${_name}")
                set(_lib "${DEVKITPRO}/portlibs/switch2/lib/${_name}")
            endif ()
            list(APPEND _fixed "${_lib}")
        endforeach ()
        set_target_properties(PkgConfig::${pkg} PROPERTIES INTERFACE_LINK_LIBRARIES "${_fixed}")
        get_target_property(_opts PkgConfig::${pkg} INTERFACE_COMPILE_OPTIONS)
        if (_opts)
            list(FILTER _opts EXCLUDE REGEX "^-m(arch|tune|cpu)=")
            set_target_properties(PkgConfig::${pkg} PROPERTIES INTERFACE_COMPILE_OPTIONS "${_opts}")
        endif ()
    endforeach ()
endif ()

# UI (app/, C++) + our torrent engine (engine/, C).
file(GLOB_RECURSE MAIN_SRC ${CMAKE_CURRENT_SOURCE_DIR}/app/*.cpp)
set(ENGINE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/engine)
# torrentfs.c is the streaming engine: 3 threads, leech-only, one piece per
# session with parked-claim adoption and strength-based peer steering.
set(ENGINE_SRC
    ${ENGINE_DIR}/bencode.c ${ENGINE_DIR}/torrent.c ${ENGINE_DIR}/torrentfs.c
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
    UPDATE_API_URL="${UPDATE_API_URL}"
    UPDATE_ASSET="${NRO_NAME}.nro")

if (PLATFORM_SWITCH)
    # Bundle borealis' resources into the nro's romfs (borealis reads them from
    # "romfs:/"). The font is dropped — borealis uses the Switch shared system
    # font on device. This mirrors borealis/demo's Switch packaging.
    add_custom_target(${PROJECT_NAME}.nro DEPENDS ${PROJECT_NAME}
        COMMAND ${NX_NACPTOOL_EXE} --create "${APP_TITLE}" "${PROJECT_AUTHOR}" "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_ALTER}" ${PROJECT_NAME}.nacp --titleid=${PROJECT_TITLEID}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_RESOURCES} ${CMAKE_BINARY_DIR}/resources
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/resources/font
        # Drop borealis' demo/sample resources -- our app ships its own UI in code
        # and its own assets, so none of these are read on device (~1.7 MB, mostly
        # the pokemon demo images). Kept: img/sys (header battery/wifi), material
        # (icon font), and ALL of i18n -- borealis' button hints (OK/Back/Exit)
        # live in those json files, so pruning them blanked the hint labels.
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/resources/img/pokemon
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/resources/xml
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/resources/icon
        COMMAND ${CMAKE_COMMAND} -E remove -f
            ${CMAKE_BINARY_DIR}/resources/img/tiles.png
            ${CMAKE_BINARY_DIR}/resources/img/demo_icon.jpg
            ${CMAKE_BINARY_DIR}/resources/img/borealis_256.png
            ${CMAKE_BINARY_DIR}/resources/img/borealis_96.png
        # Bundle our loading-screen logo, list icon and the Stremio mark at romfs:/.
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/assets/NX-torrent-player-bg-rounded.png ${CMAKE_BINARY_DIR}/resources/NX-torrent-player-bg-rounded.png
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/assets/video-icon.png ${CMAKE_BINARY_DIR}/resources/video-icon.png
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/assets/magnet-icon.png ${CMAKE_BINARY_DIR}/resources/magnet-icon.png
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/assets/stremio-icon.png ${CMAKE_BINARY_DIR}/resources/stremio-icon.png
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/assets/local-tab-icon.png ${CMAKE_BINARY_DIR}/resources/local-icon.png
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/assets/SpaceGrotesk-Medium.ttf ${CMAKE_BINARY_DIR}/resources/SpaceGrotesk-Medium.ttf
        # The CA store curl checks certificates against (Mozilla's roots, from
        # curl.se/ca). The console mounts none, so it has to ride in the romfs --
        # refresh it from there when roots rotate. See app/http.cpp.
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/assets/cacert.pem ${CMAKE_BINARY_DIR}/resources/cacert.pem
        COMMAND ${NX_ELF2NRO_EXE} ${PROJECT_NAME}.elf ${NRO_NAME}.nro --icon=${PROJECT_ICON} --nacp=${PROJECT_NAME}.nacp --romfsdir=${CMAKE_BINARY_DIR}/resources
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
# Drop unreferenced functions/data (paired with -ffunction/-fdata-sections above)
# so the linker only keeps the mpv/ffmpeg code actually reached, shrinking the nro.
target_link_options(${PROJECT_NAME} PRIVATE -Wl,--gc-sections)
target_link_libraries(${PROJECT_NAME} PRIVATE
    borealis
    PkgConfig::MPV
    PkgConfig::CURL
    PkgConfig::WEBP
    mbedtls mbedx509 mbedcrypto z
    ${APP_PLATFORM_LIB})
