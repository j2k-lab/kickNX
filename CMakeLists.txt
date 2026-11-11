cmake_minimum_required(VERSION 3.18)

# build options
set(BOREALIS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/library/borealis)
set(BOREALIS_LIBRARY ${BOREALIS_DIR}/library)

# Include Platform common options
# example: cmake -B build -DPLATFORM_DESKTOP=ON
include(${BOREALIS_LIBRARY}/cmake/commonOption.cmake)

option(USE_SYSTEM_CURL "use system installed libcurl" ON)

# NintendoSwitch
cmake_dependent_option(BUILTIN_NSP "Built in NSP forwarder" OFF "PLATFORM_SWITCH" OFF)
cmake_dependent_option(USE_LIBUSBHSFS "Use libusbhsfs for ums access" OFF "PLATFORM_SWITCH" OFF)

# mpv related
# If your system does not support OpenGL(ES), you can use software rendering, but it will affect performance.
option(MPV_SW_RENDER "Using CPU to draw videos" OFF)
if (MPV_SW_RENDER)
    list(APPEND APP_PLATFORM_OPTION -DMPV_SW_RENDER)
endif ()

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build, options are: Debug Release RelWithDebInfo MinSizeRel." FORCE)
elseif (CMAKE_BUILD_TYPE STREQUAL Debug)
    list(APPEND APP_PLATFORM_OPTION -D_DEBUG -D_GLIBCXX_ASSERTIONS)
endif()

# Add git info when building from a clone. Source archives intentionally do not
# contain .git metadata, so use stable fallback values without printing errors.
find_package(Git QUIET)
if (NOT DEFINED GIT_TAG_SHORT)
    if (GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            TIMEOUT 5
            RESULT_VARIABLE GIT_TAG_RESULT
            OUTPUT_VARIABLE GIT_TAG_SHORT
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE)
    endif ()
    if (NOT GIT_TAG_SHORT)
        set(GIT_TAG_SHORT "source")
    endif ()
endif ()

if (NOT DEFINED VERSION_BUILD)
    if (GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-list --count --all
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            TIMEOUT 5
            RESULT_VARIABLE VERSION_BUILD_RESULT
            OUTPUT_VARIABLE VERSION_BUILD
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE)
    endif ()
    if (NOT VERSION_BUILD)
        set(VERSION_BUILD "0")
    endif ()
endif ()
message(STATUS "git commit ${GIT_TAG_SHORT} build ${VERSION_BUILD}")

# toolchain
include(${BOREALIS_LIBRARY}/cmake/toolchain.cmake)

# project info
project(KickNX)
set(VERSION_MAJOR "0")
set(VERSION_MINOR "5")
set(VERSION_ALTER "2")
set(PROJECT_TITLEID "010ff000ffff0006")
set(PROJECT_AUTHOR "j2k")
set(PROJECT_DISPLAY_NAME "kickNX")
set(PACKAGE_NAME "app.kicknx")
set(PROJECT_ICON ${CMAKE_CURRENT_SOURCE_DIR}/resources/icon/icon.jpg)
set(PROJECT_RESOURCES ${CMAKE_CURRENT_SOURCE_DIR}/resources)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake")

if (CUSTOM_RESOURCES_DIR)
    set(BRLS_RESOURCES_DIR ${CUSTOM_RESOURCES_DIR})
endif ()

# setting src and include
file(GLOB_RECURSE MAIN_SRC CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/app/src/*.cpp")
if (PLATFORM_SWITCH)
    # System service initialization functions required for libnx
    list(APPEND MAIN_SRC ${BOREALIS_LIBRARY}/lib/platforms/switch/switch_wrapper.c)
endif ()

if (USE_LIBROMFS)
    message(STATUS "USE LIBROMFS")
    add_definitions(-DUSE_LIBROMFS)
    set(LIBROMFS_PROJECT_NAME ${PROJECT_NAME})
    set(LIBROMFS_RESOURCE_LOCATION "${PROJECT_RESOURCES}")
endif ()

# build borealis and other third party libraries
add_subdirectory(library)

if (USE_SYSTEM_CURL)
    find_package(CURL REQUIRED)
else ()
    include(FetchContent)
    FetchContent_Declare(curl
        URL https://curl.se/download/curl-8.16.0.tar.xz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    if (WIN32)
        set(CURL_USE_SCHANNEL ON)
        set(USE_WIN32_IDN ON)
        set(ZLIB_USE_STATIC_LIBS ON)
    elseif (PLATFORM_ANDROID)
        set(CURL_USE_MBEDTLS ON)
    else ()
        find_package(OpenSSL REQUIRED)
        set(CURL_USE_OPENSSL ON)
    endif()
    set(HTTP_ONLY ON)
    set(BUILD_CURL_EXE OFF)
    set(BUILD_TESTING OFF)
    set(BUILD_EXAMPLES OFF)
    set(BUILD_LIBCURL_DOCS OFF)
    set(BUILD_MISC_DOCS OFF)
    set(ENABLE_CURL_MANUAL OFF)
    set(USE_NGHTTP2 OFF)
    set(USE_LIBIDN2 OFF)
    set(CURL_BROTLI OFF)
    set(CURL_ZSTD OFF)
    set(CURL_USE_LIBSSH2 OFF)
    set(CURL_USE_LIBPSL OFF)
    set(CURL_DISABLE_PROGRESS_METER ON)
    FetchContent_MakeAvailable(curl)
endif ()
list(APPEND APP_PLATFORM_LIB CURL::libcurl)

find_package(Threads REQUIRED)
list(APPEND APP_PLATFORM_LIB ${CMAKE_THREAD_LIBS_INIT})

if (USE_BOOST_FILESYSTEM)
    find_package(Boost 1.44 REQUIRED COMPONENTS filesystem)
    if(Boost_FOUND)
        list(APPEND APP_PLATFORM_LIB Boost::filesystem)
        list(APPEND APP_PLATFORM_OPTION -DUSE_BOOST_FILESYSTEM -DBOOST_FILESYSTEM_VERSION=4)
    endif()
endif ()

if (PLATFORM_DESKTOP)
    find_package(MPV REQUIRED)
    list(APPEND APP_PLATFORM_INCLUDE ${MPV_INCLUDE_DIR})
    list(APPEND APP_PLATFORM_LIB ${MPV_LIBRARY})
    if (NOT WIN32)
        find_library(AVFORMAT_LIB NAMES avformat REQUIRED)
        list(APPEND APP_PLATFORM_LIB ${AVFORMAT_LIB})
    endif ()

    find_package(WebP)
    if (WebP_FOUND)
        list(APPEND APP_PLATFORM_OPTION -DUSE_WEBP)
        list(APPEND APP_PLATFORM_INCLUDE ${WebP_INCLUDE_DIRS})
        find_library(WEBPDEMUX_LIB NAMES webpdemux REQUIRED)
        list(APPEND APP_PLATFORM_LIB ${WEBPDEMUX_LIB} ${WebP_LIBRARY})
    endif ()

    if (MINGW)
        set(CMAKE_EXE_LINKER_FLAGS "-static-libgcc -static-libstdc++ -static")
    endif ()
else ()
    # Used for Cross Compile such as NintendoSwitch
    # If dependent files cannot be found, environment variables can be set before executing the cmake command:
    # PKG_CONFIG_LIBDIR=/some/path/arm64/lib/pkgconfig
    find_package(PkgConfig REQUIRED)

    pkg_search_module(MPV REQUIRED mpv)
    if (PLATFORM_SWITCH)
        set(KICKNX_FFMPEG_ROOT "" CACHE PATH
            "Optional FFmpeg root containing include/ and lib/ for the kickNX Switch build")
        set(KICKNX_LOCAL_FFMPEG_ROOT
            "${CMAKE_SOURCE_DIR}/.kicknx-build/ffmpeg-local-root/opt/devkitpro/portlibs/switch")

        if (NOT KICKNX_FFMPEG_ROOT AND EXISTS "${KICKNX_LOCAL_FFMPEG_ROOT}/lib/libavformat.a")
            set(KICKNX_FFMPEG_ROOT "${KICKNX_LOCAL_FFMPEG_ROOT}")
        endif ()

        if (KICKNX_FFMPEG_ROOT)
            set(KICKNX_FFMPEG_LIB_DIR "${KICKNX_FFMPEG_ROOT}/lib")
            foreach(KICKNX_FFMPEG_NAME avfilter avformat avcodec avutil swresample swscale)
                set(KICKNX_FFMPEG_LIBRARY
                    "${KICKNX_FFMPEG_LIB_DIR}/lib${KICKNX_FFMPEG_NAME}.a")
                if (NOT EXISTS "${KICKNX_FFMPEG_LIBRARY}")
                    message(FATAL_ERROR "Missing custom FFmpeg library: ${KICKNX_FFMPEG_LIBRARY}")
                endif ()

                list(FIND MPV_LIBRARIES "${KICKNX_FFMPEG_NAME}" KICKNX_FFMPEG_INDEX)
                if (NOT KICKNX_FFMPEG_INDEX EQUAL -1)
                    list(REMOVE_AT MPV_LIBRARIES ${KICKNX_FFMPEG_INDEX})
                    list(INSERT MPV_LIBRARIES ${KICKNX_FFMPEG_INDEX} "${KICKNX_FFMPEG_LIBRARY}")
                endif ()
            endforeach()
            message(STATUS "Using kickNX FFmpeg override: ${KICKNX_FFMPEG_ROOT}")
        else ()
            message(STATUS "Using FFmpeg libraries reported by mpv pkg-config")
        endif ()
    endif ()
    list(APPEND APP_PLATFORM_INCLUDE ${MPV_INCLUDE_DIRS})
    list(APPEND APP_PLATFORM_LIB ${MPV_LIBRARIES})
    link_directories(${MPV_LIBRARY_DIRS})

    pkg_search_module(LIBWEBP QUIET libwebp)
    if (LIBWEBP_FOUND)
        message(STATUS "Found libwebp: ${LIBWEBP_INCLUDE_DIRS} ${LIBWEBP_LIBRARIES}")
        list(APPEND APP_PLATFORM_INCLUDE ${LIBWEBP_INCLUDE_DIRS})
        list(APPEND APP_PLATFORM_LIB ${LIBWEBP_LIBRARIES})
        list(APPEND APP_PLATFORM_OPTION -DUSE_WEBP)
        link_directories(${LIBWEBP_LIBRARY_DIRS})
        # Animated WebP emotes need the demux decoder; it links before libwebp.
        pkg_search_module(LIBWEBPDEMUX REQUIRED libwebpdemux)
        list(INSERT APP_PLATFORM_LIB 0 ${LIBWEBPDEMUX_LIBRARIES})
    endif ()
endif()

if (BUILTIN_NSP)
    find_library(NSP_LIB NAMES nsp REQUIRED)
    list(APPEND APP_PLATFORM_LIB nsp)
    list(APPEND APP_PLATFORM_OPTION -DBUILTIN_NSP)
    add_subdirectory(scripts/forwarder)
endif ()

if (USE_LIBUSBHSFS)
    find_library(USBHSFS_LIB NAMES usbhsfs REQUIRED)
    list(APPEND APP_PLATFORM_LIB lwext4 ntfs-3g usbhsfs)
    list(APPEND APP_PLATFORM_OPTION -DUSE_LIBUSBHSFS)
endif ()

# building target
program_target(${PROJECT_NAME} "${MAIN_SRC}")
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 17)
target_include_directories(${PROJECT_NAME} PRIVATE app/include ${APP_PLATFORM_INCLUDE})
target_compile_options(${PROJECT_NAME} PRIVATE -ffunction-sections -fdata-sections -Wunused-variable ${APP_PLATFORM_OPTION})
if (PLATFORM_SWITCH AND KICKNX_FFMPEG_ROOT)
    target_link_directories(${PROJECT_NAME} BEFORE PRIVATE "${KICKNX_FFMPEG_ROOT}/lib")
endif ()
target_link_libraries(${PROJECT_NAME} PRIVATE borealis lunasvg ${APP_PLATFORM_LIB})
target_compile_definitions(${PROJECT_NAME} PRIVATE
    APP_VERSION=${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_ALTER}
    APP_BUILD=${VERSION_BUILD}
    BUILD_TAG_SHORT=${GIT_TAG_SHORT}
    BUILD_PACKAGE_NAME=${PROJECT_NAME}
)

# building release file
if (PLATFORM_DESKTOP)
    if (NOT USE_LIBROMFS)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_directory ${PROJECT_RESOURCES} ${PROJECT_BINARY_DIR}/resources
        )
    endif()
elseif (PLATFORM_SWITCH)
    if (BUILTIN_NSP)
        add_dependencies(${PROJECT_NAME} forwarder.nsp)
    endif ()
    if (USE_DEKO3D)
        gen_dksh("${PROJECT_RESOURCES}/shaders")
    endif ()
    # The release file is kickNX.nro. PROJECT_NAME stays KickNX because it
    # also names the save folder (sdmc:/switch/KickNX) and the user agent.
    # The old file is removed first: on a case-insensitive drive, writing
    # over KickNX.nro would keep its old capitalisation.
    # The romfs copy is cleared first so files deleted from resources/ are
    # not packed into the NRO from an earlier build.
    set(PROJECT_NRO_NAME kickNX)
    add_custom_target(${PROJECT_NRO_NAME}.nro DEPENDS ${PROJECT_NAME}
        COMMAND ${NX_NACPTOOL_EXE} --create "${PROJECT_DISPLAY_NAME}" "${PROJECT_AUTHOR}" "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_ALTER}" ${PROJECT_NAME}.nacp
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/resources
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_RESOURCES} ${CMAKE_BINARY_DIR}/resources
        COMMAND ${CMAKE_COMMAND} -E remove ${CMAKE_BINARY_DIR}/resources/font/keymap*.ttf ${CMAKE_BINARY_DIR}/resources/font/*_fallback.ttf ${CMAKE_BINARY_DIR}/resources/font/switch_font.ttf
        COMMAND ${CMAKE_COMMAND} -E remove ${PROJECT_NAME}.nro ${PROJECT_NRO_NAME}.nro
        COMMAND ${NX_ELF2NRO_EXE} ${PROJECT_NAME}.elf ${PROJECT_NRO_NAME}.nro --icon=${PROJECT_ICON} --nacp=${PROJECT_NAME}.nacp --romfsdir=${CMAKE_BINARY_DIR}/resources
    )
endif ()
