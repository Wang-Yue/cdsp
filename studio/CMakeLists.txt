cmake_minimum_required(VERSION 3.16)
project(MonitorQt LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler optimization flags matching cdsp/Makefile
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    if(MSVC)
        add_compile_options(/O2 /fp:fast)
    else()
        add_compile_options(-O3 -ffp-contract=fast -fno-math-errno -funroll-loops)
        if(APPLE)
            add_compile_options(-mcpu=native)
        elseif(NOT CMAKE_CROSSCOMPILING)
            add_compile_options(-march=native)
        endif()
    endif()
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOMOC_PARALLEL 0)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Concurrent Multimedia OpenGL OpenGLWidgets)
find_package(Threads REQUIRED)

# Platform audio and acceleration options (aligned with cdsp)
option(ENABLE_FFTW "Enable FFTW3 acceleration (otherwise cdsp uses built-in FFT)" ON)
option(USE_LIBDISPATCH "Enable Apple Grand Central Dispatch (libdispatch) multi-threaded pipeline" ON)

# Define system libraries based on platform
if(APPLE)
    find_library(COREAUDIO_LIB CoreAudio REQUIRED)
    find_library(AUDIOTOOLBOX_LIB AudioToolbox REQUIRED)
    find_library(ACCELERATE_LIB Accelerate REQUIRED)
    find_library(COREFOUNDATION_LIB CoreFoundation REQUIRED)
    set(PLATFORM_LIBS ${COREAUDIO_LIB} ${AUDIOTOOLBOX_LIB} ${ACCELERATE_LIB} ${COREFOUNDATION_LIB})
    add_compile_definitions(ENABLE_COREAUDIO ENABLE_ACCELERATE)
elseif(WIN32)
    set(PLATFORM_LIBS ws2_32 winmm ole32 uuid ksuser ksguid avrt)
    add_compile_definitions(ENABLE_WASAPI ENABLE_ASIO _WIN32 COBJMACROS UNICODE _UNICODE _USE_MATH_DEFINES)

    if(ENABLE_FFTW)
        find_package(PkgConfig)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(FFTW3 fftw3)
            pkg_check_modules(FFTW3F fftw3f)
            if(FFTW3_FOUND AND FFTW3F_FOUND)
                list(APPEND PLATFORM_LIBS ${FFTW3_LIBRARIES} ${FFTW3F_LIBRARIES})
                include_directories(${FFTW3_INCLUDE_DIRS} ${FFTW3F_INCLUDE_DIRS})
                add_compile_definitions(ENABLE_FFTW)
            endif()
        endif()
    endif()

    if(USE_LIBDISPATCH)
        find_library(WIN_DISPATCH_LIB dispatch)
        find_library(WIN_BLOCKS_LIB BlocksRuntime)
        if(WIN_DISPATCH_LIB AND WIN_BLOCKS_LIB)
            list(APPEND PLATFORM_LIBS ${WIN_DISPATCH_LIB} ${WIN_BLOCKS_LIB})
            add_compile_definitions(USE_LIBDISPATCH HAVE_MODE_T)
        else()
            find_package(PkgConfig)
            if(PKG_CONFIG_FOUND)
                pkg_check_modules(DISPATCH libdispatch)
                if(DISPATCH_FOUND)
                    list(APPEND PLATFORM_LIBS ${DISPATCH_LIBRARIES})
                    include_directories(${DISPATCH_INCLUDE_DIRS})
                    add_compile_definitions(USE_LIBDISPATCH HAVE_MODE_T)
                endif()
            endif()
        endif()
    endif()
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DBUS dbus-1 REQUIRED)
    pkg_check_modules(PIPEWIRE libpipewire-0.3 REQUIRED)
    set(PLATFORM_LIBS m rt dbus-1 asound ${PIPEWIRE_LIBRARIES})
    include_directories(${PIPEWIRE_INCLUDE_DIRS})
    add_compile_definitions(ENABLE_ALSA ENABLE_PIPEWIRE _GNU_SOURCE _REENTRANT)

    if(ENABLE_FFTW)
        pkg_check_modules(FFTW3 fftw3 REQUIRED)
        pkg_check_modules(FFTW3F fftw3f REQUIRED)
        list(APPEND PLATFORM_LIBS ${FFTW3_LIBRARIES} ${FFTW3F_LIBRARIES})
        include_directories(${FFTW3_INCLUDE_DIRS} ${FFTW3F_INCLUDE_DIRS})
        add_compile_definitions(ENABLE_FFTW)
    endif()

    if(USE_LIBDISPATCH)
        find_library(DISPATCH_LIB dispatch HINTS "${CMAKE_SYSROOT}/usr/libexec/swift/lib/swift/linux" /usr/libexec/swift/lib/swift/linux)
        find_library(BLOCKS_LIB BlocksRuntime HINTS "${CMAKE_SYSROOT}/usr/libexec/swift/lib/swift/linux" /usr/libexec/swift/lib/swift/linux)
        if(DISPATCH_LIB AND BLOCKS_LIB)
            list(APPEND PLATFORM_LIBS ${DISPATCH_LIB} ${BLOCKS_LIB})
            include_directories(SYSTEM "${CMAKE_SYSROOT}/usr/libexec/swift/lib/swift" /usr/libexec/swift/lib/swift)
            add_compile_definitions(USE_LIBDISPATCH)
        endif()
    endif()
endif()

# CDSP FetchContent (fetched directly from GitHub: Wang-Yue/cdsp)
include(FetchContent)
message(STATUS "Fetching Wang-Yue/cdsp directly from GitHub...")
FetchContent_Declare(
    cdsp
    GIT_REPOSITORY https://github.com/Wang-Yue/cdsp.git
    GIT_TAG        main
)
FetchContent_MakeAvailable(cdsp)
set(CDSP_DIR "${cdsp_SOURCE_DIR}")

file(GLOB_RECURSE CDSP_C_SOURCES
    "${CDSP_DIR}/Engine/*.c"
    "${CDSP_DIR}/Audio/*.c"
    "${CDSP_DIR}/Backend/*.c"
    "${CDSP_DIR}/Config/*.c"
    "${CDSP_DIR}/FFT/*.c"
    "${CDSP_DIR}/Filters/*.c"
    "${CDSP_DIR}/Logging/*.c"
    "${CDSP_DIR}/Mixer/*.c"
    "${CDSP_DIR}/Pipeline/*.c"
    "${CDSP_DIR}/Processors/*.c"
    "${CDSP_DIR}/Resampler/*.c"
    "${CDSP_DIR}/DoP/*.c"
    "${CDSP_DIR}/Public/*.c"
    "${CDSP_DIR}/Utils/*.c"
)
add_library(cdsp_static STATIC ${CDSP_C_SOURCES})
target_include_directories(cdsp_static PUBLIC
    ${CDSP_DIR}
    ${CDSP_DIR}/Engine
    ${CDSP_DIR}/Audio
    ${CDSP_DIR}/Backend
    ${CDSP_DIR}/Config
    ${CDSP_DIR}/FFT
    ${CDSP_DIR}/Filters
    ${CDSP_DIR}/Logging
    ${CDSP_DIR}/Mixer
    ${CDSP_DIR}/Pipeline
    ${CDSP_DIR}/Processors
    ${CDSP_DIR}/Resampler
    ${CDSP_DIR}/DoP
    ${CDSP_DIR}/Public
    ${CDSP_DIR}/Utils
)
if(fftw3_SOURCE_DIR)
    target_include_directories(cdsp_static PUBLIC ${fftw3_SOURCE_DIR}/api)
endif()
set(CDSP_LIB cdsp_static)

include_directories(
    src
    ${fftw3_SOURCE_DIR}/api
    ${DBUS_INCLUDE_DIRS}
    ${CDSP_DIR}
    ${CDSP_DIR}/Engine
    ${CDSP_DIR}/Audio
    ${CDSP_DIR}/Backend
    ${CDSP_DIR}/Config
    ${CDSP_DIR}/FFT
    ${CDSP_DIR}/Filters
    ${CDSP_DIR}/Logging
    ${CDSP_DIR}/Mixer
    ${CDSP_DIR}/Pipeline
    ${CDSP_DIR}/Processors
    ${CDSP_DIR}/Resampler
    ${CDSP_DIR}/DoP
    ${CDSP_DIR}/Public
    ${CDSP_DIR}/Utils
)

# Collect source files
file(GLOB_RECURSE SOURCES "src/*.cpp" "src/*.h" "src/*.qrc")

if(APPLE)
    set(APP_ICON_MACOS "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/icons/app.icns")
    set_source_files_properties(${APP_ICON_MACOS} PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
    list(APPEND SOURCES ${APP_ICON_MACOS})
endif()

add_executable(MonitorQt WIN32 ${SOURCES})

if(APPLE)
    set_target_properties(MonitorQt PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "MonitorQt"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.wangyue.monitorqt"
        MACOSX_BUNDLE_ICON_FILE "app.icns"
    )
endif()

target_link_libraries(MonitorQt PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    Qt6::Concurrent
    Qt6::Multimedia
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    Threads::Threads
    ${CDSP_LIB}
    ${PLATFORM_LIBS}
)

# Dead code stripping & Link-Time Optimization (LTO)
if(APPLE)
    target_link_options(MonitorQt PRIVATE -Wl,-dead_strip)
elseif(WIN32)
    target_link_options(MonitorQt PRIVATE -static -static-libgcc -static-libstdc++)
    find_program(WINDEPLOYQT_BIN NAMES windeployqt.exe HINTS "${Qt6_DIR}/../../../bin")
    if(WINDEPLOYQT_BIN)
        add_custom_target(
            deploy-win
            COMMAND ${WINDEPLOYQT_BIN} $<TARGET_FILE:MonitorQt> --no-compiler-runtime
            DEPENDS MonitorQt
            COMMENT "Deploying Qt Windows DLLs alongside MonitorQt.exe..."
        )
    endif()
else()
    if(DISPATCH_LIB)
        target_link_options(MonitorQt PRIVATE -Wl,-rpath,/usr/libexec/swift/lib/swift/linux)
    endif()
endif()

# Option to enable LTO (Link-Time Optimization) - OFF by default for fast linking
option(ENABLE_LTO "Enable Link-Time Optimization (LTO/IPO)" OFF)
if(ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
    if(ipo_supported)
        set_property(TARGET MonitorQt PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()

if(APPLE)
    find_program(MACDEPLOYQT_BIN NAMES macdeployqt HINTS /opt/homebrew/bin)
    if(MACDEPLOYQT_BIN)
        add_custom_target(
            deploy
            COMMAND ${MACDEPLOYQT_BIN} $<TARGET_BUNDLE_DIR:MonitorQt> -always-overwrite
            COMMAND strip -x $<TARGET_BUNDLE_DIR:MonitorQt>/Contents/MacOS/MonitorQt
            DEPENDS MonitorQt
            COMMENT "Deploying Qt libraries into macOS .app bundle..."
        )
        add_custom_target(
            dmg
            COMMAND ${MACDEPLOYQT_BIN} $<TARGET_BUNDLE_DIR:MonitorQt> -always-overwrite
            COMMAND strip -x $<TARGET_BUNDLE_DIR:MonitorQt>/Contents/MacOS/MonitorQt
            COMMAND ${MACDEPLOYQT_BIN} $<TARGET_BUNDLE_DIR:MonitorQt> -dmg
            DEPENDS MonitorQt
            COMMENT "Deploying Qt libraries and creating DMG installer..."
        )
    endif()
endif()

# Automatic Code Formatting with clang-format
find_program(CLANG_FORMAT_BIN NAMES clang-format)
if(CLANG_FORMAT_BIN)
    file(GLOB_RECURSE ALL_PROJECT_SOURCES "src/*.cpp" "src/*.h")
    add_custom_target(
        format
        COMMAND ${CLANG_FORMAT_BIN} -i -style=file ${ALL_PROJECT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Running clang-format on all C++ source and header files..."
    )
    add_custom_target(
        format-check
        COMMAND ${CLANG_FORMAT_BIN} --dry-run -Werror -style=file ${ALL_PROJECT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Checking code formatting with clang-format..."
    )
endif()

