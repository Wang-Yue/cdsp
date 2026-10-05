cmake_minimum_required(VERSION 3.20)
project(cdsp VERSION 5.0.0 LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJC OBJCXX)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Output all built binaries and libraries in top-level build/bin and build/lib
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# Symlink compile_commands.json to source root for seamless LSP support (clangd / VS Code)
if(CMAKE_EXPORT_COMPILE_COMMANDS AND NOT CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    file(CREATE_LINK
        "${CMAKE_BINARY_DIR}/compile_commands.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json"
        RESULT _compile_commands_link_result
        SYMBOLIC
        COPY_ON_ERROR
    )
endif()

# -----------------------------------------------------------------------------
# Options & Platform Defaults
# -----------------------------------------------------------------------------
if(EMSCRIPTEN)
    set(DEFAULT_ENABLE_COREAUDIO   OFF)
    set(DEFAULT_ENABLE_ACCELERATE  OFF)
    set(DEFAULT_ENABLE_LIBDISPATCH OFF)
    set(DEFAULT_ENABLE_ALSA        OFF)
    set(DEFAULT_ENABLE_PIPEWIRE    OFF)
    set(DEFAULT_ENABLE_WASAPI      OFF)
    set(DEFAULT_ENABLE_ASIO        OFF)
    set(DEFAULT_ENABLE_OPENMP      OFF)
    set(DEFAULT_ENABLE_DBUS        OFF)
    set(DEFAULT_ENABLE_WEBSOCKET   OFF)
    set(DEFAULT_ENABLE_STUDIO      ON)
    set(DEFAULT_BUILD_BENCHMARKS   OFF)
    set(DEFAULT_BUILD_RUST_HARNESS OFF)
    set(DEFAULT_ENABLE_WEBAUDIO    ON)
elseif(APPLE)
    set(DEFAULT_ENABLE_COREAUDIO   ON)
    set(DEFAULT_ENABLE_ACCELERATE  ON)
    set(DEFAULT_ENABLE_LIBDISPATCH ON)
    set(DEFAULT_ENABLE_ALSA        OFF)
    set(DEFAULT_ENABLE_PIPEWIRE    OFF)
    set(DEFAULT_ENABLE_WASAPI      OFF)
    set(DEFAULT_ENABLE_ASIO        OFF)
    set(DEFAULT_ENABLE_OPENMP      OFF)
    set(DEFAULT_ENABLE_DBUS        OFF)
    set(DEFAULT_ENABLE_WEBSOCKET   ON)
    set(DEFAULT_ENABLE_STUDIO      ON)
    set(DEFAULT_BUILD_BENCHMARKS   ON)
    set(DEFAULT_BUILD_RUST_HARNESS ON)
    set(DEFAULT_ENABLE_WEBAUDIO    OFF)
elseif(WIN32)
    set(DEFAULT_ENABLE_COREAUDIO   OFF)
    set(DEFAULT_ENABLE_ACCELERATE  OFF)
    set(DEFAULT_ENABLE_LIBDISPATCH OFF)
    set(DEFAULT_ENABLE_ALSA        OFF)
    set(DEFAULT_ENABLE_PIPEWIRE    OFF)
    set(DEFAULT_ENABLE_WASAPI      ON)
    set(DEFAULT_ENABLE_ASIO        ON)
    set(DEFAULT_ENABLE_OPENMP      ON)
    set(DEFAULT_ENABLE_DBUS        OFF)
    set(DEFAULT_ENABLE_WEBSOCKET   ON)
    set(DEFAULT_ENABLE_STUDIO      ON)
    set(DEFAULT_BUILD_BENCHMARKS   ON)
    set(DEFAULT_BUILD_RUST_HARNESS ON)
    set(DEFAULT_ENABLE_WEBAUDIO    OFF)
else()
    # Linux (Default)
    set(DEFAULT_ENABLE_COREAUDIO   OFF)
    set(DEFAULT_ENABLE_ACCELERATE  OFF)
    set(DEFAULT_ENABLE_LIBDISPATCH OFF)
    set(DEFAULT_ENABLE_ALSA        ON)
    set(DEFAULT_ENABLE_PIPEWIRE    ON)
    set(DEFAULT_ENABLE_WASAPI      OFF)
    set(DEFAULT_ENABLE_ASIO        OFF)
    set(DEFAULT_ENABLE_OPENMP      ON)
    set(DEFAULT_ENABLE_DBUS        ON)
    set(DEFAULT_ENABLE_WEBSOCKET   ON)
    set(DEFAULT_ENABLE_STUDIO      ON)
    set(DEFAULT_BUILD_BENCHMARKS   ON)
    set(DEFAULT_BUILD_RUST_HARNESS ON)
    set(DEFAULT_ENABLE_WEBAUDIO    OFF)
endif()

option(ENABLE_COREAUDIO   "Enable macOS CoreAudio backend"      ${DEFAULT_ENABLE_COREAUDIO})
option(ENABLE_ACCELERATE  "Enable Apple Accelerate (vDSP/vForce)" ${DEFAULT_ENABLE_ACCELERATE})
option(ENABLE_ALSA        "Enable Linux ALSA audio backend"     ${DEFAULT_ENABLE_ALSA})
option(ENABLE_PIPEWIRE    "Enable Linux PipeWire backend"       ${DEFAULT_ENABLE_PIPEWIRE})
option(ENABLE_WASAPI      "Enable Windows WASAPI audio backend" ${DEFAULT_ENABLE_WASAPI})
option(ENABLE_ASIO        "Enable Windows ASIO audio backend"   ${DEFAULT_ENABLE_ASIO})
option(ENABLE_OPENMP      "Enable OpenMP parallelization"       ${DEFAULT_ENABLE_OPENMP})
option(ENABLE_WEBSOCKET   "Enable WebSocket control server"     ${DEFAULT_ENABLE_WEBSOCKET})
option(ENABLE_DBUS        "Enable Linux DBus device monitoring" ${DEFAULT_ENABLE_DBUS})
option(ENABLE_LIBDISPATCH "Enable Apple libdispatch (GCD)"      ${DEFAULT_ENABLE_LIBDISPATCH})

option(ENABLE_LTO         "Enable Link Time Optimization"       ON)
option(ENABLE_NATIVE_ARCH "Enable native CPU optimization"      ON)
option(BUILD_TESTING      "Build test runner and unit tests"    ON)
option(BUILD_RUST_HARNESS "Build Rust test harness binaries"    ${DEFAULT_BUILD_RUST_HARNESS})
option(BUILD_BENCHMARKS   "Build benchmark targets"             ${DEFAULT_BUILD_BENCHMARKS})
option(ENABLE_STUDIO       "Build CDSP Studio GUI application"   ${DEFAULT_ENABLE_STUDIO})
option(ENABLE_WEBAUDIO        "Enable WebAudio backend"             ${DEFAULT_ENABLE_WEBAUDIO})
if(EMSCRIPTEN)
    set(ENABLE_WEBAUDIO ON CACHE BOOL "Enable WebAudio backend" FORCE)
endif()
option(ENABLE_STATIC_WINDOWS "Build static standalone binaries on Windows (MinGW)" ON)

set(ENABLE_SANITIZER "none" CACHE STRING "Sanitizer to enable (none, address, thread, undefined, memory, leak, intsan, cfi)")

# -----------------------------------------------------------------------------
# Global Optimization & Build Settings
# -----------------------------------------------------------------------------
if(ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
    if(ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON)
    else()
        message(STATUS "IPO/LTO not supported: ${ipo_error}")
    endif()
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(
        -Wall
        -Wextra
        -Wswitch
        -Werror=switch
        -fvisibility=hidden
    )

    # Optimization flags applied only to Release and RelWithDebInfo builds
    add_compile_options(
        "$<$<CONFIG:Release,RelWithDebInfo>:-O3;-ffp-contract=fast;-fno-math-errno;-funroll-loops>"
    )

    add_compile_options(
        "$<$<CONFIG:Release,RelWithDebInfo>:-fassociative-math;-fno-signed-zeros;-fno-trapping-math>"
    )

    if(ENABLE_NATIVE_ARCH AND NOT CMAKE_CROSSCOMPILING)
        if(APPLE)
            add_compile_options(-mcpu=native)
        else()
            add_compile_options(-march=native)
        endif()
    endif()

    if(EMSCRIPTEN)
        add_compile_options(-pthread)
        add_link_options(-pthread)
    endif()
endif()

if(WIN32 AND ENABLE_STATIC_WINDOWS AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a" ".lib")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc -static-libstdc++ -Wl,--gc-sections")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -static -static-libgcc -static-libstdc++ -Wl,--gc-sections")
endif()

# Sanitizers
if(NOT ENABLE_SANITIZER STREQUAL "none")
    if(ENABLE_SANITIZER STREQUAL "address")
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=address,undefined)
    elseif(ENABLE_SANITIZER STREQUAL "thread")
        add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=thread)
        if(ENABLE_OPENMP AND CMAKE_C_COMPILER_ID MATCHES "Clang")
            execute_process(
                COMMAND ${CMAKE_C_COMPILER} -print-file-name=libarcher.so
                OUTPUT_VARIABLE _libarcher_path
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
            if(EXISTS "${_libarcher_path}")
                set(LIBARCHER_PATH "${_libarcher_path}")
                message(STATUS "ThreadSanitizer: Found Archer for OpenMP at ${LIBARCHER_PATH}")
            endif()
        endif()
    elseif(ENABLE_SANITIZER STREQUAL "undefined")
        add_compile_options(-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=undefined)
    elseif(ENABLE_SANITIZER STREQUAL "intsan")
        add_compile_options(-fsanitize=signed-integer-overflow,shift,integer-divide-by-zero -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=signed-integer-overflow,shift,integer-divide-by-zero)
    elseif(ENABLE_SANITIZER STREQUAL "memory")
        if(APPLE)
            message(FATAL_ERROR "MemorySanitizer is not supported on macOS")
        endif()
        set(ENABLE_ALSA OFF CACHE BOOL "Disabled under MemorySanitizer" FORCE)
        set(ENABLE_PIPEWIRE OFF CACHE BOOL "Disabled under MemorySanitizer" FORCE)
        set(ENABLE_DBUS OFF CACHE BOOL "Disabled under MemorySanitizer" FORCE)
        set(ENABLE_WEBSOCKET OFF CACHE BOOL "Disabled under MemorySanitizer" FORCE)
        add_compile_options(-fsanitize=memory -fsanitize-memory-track-origins=2 -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=memory)
    elseif(ENABLE_SANITIZER STREQUAL "leak")
        if(APPLE)
            add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
            add_link_options(-fsanitize=address)
        else()
            add_compile_options(-fsanitize=leak -fno-omit-frame-pointer -g)
            add_link_options(-fsanitize=leak)
        endif()
    elseif(ENABLE_SANITIZER STREQUAL "cfi")
        add_compile_options(-flto -fvisibility=hidden -fsanitize=cfi -fno-omit-frame-pointer -g)
        add_link_options(-flto -fsanitize=cfi)
    else()
        message(FATAL_ERROR "Unknown sanitizer: ${ENABLE_SANITIZER}")
    endif()
endif()

# -----------------------------------------------------------------------------
# Optional Target: Re-generate Configuration Parser & Schemas
# -----------------------------------------------------------------------------
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_FOUND)
    file(GLOB_RECURSE CODEGEN_SCRIPTS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/codegen/*.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_config.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/config/schema/*.py"
    )
    add_custom_target(generate_config
        COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1 "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_config.py" --out-dir "${CMAKE_CURRENT_SOURCE_DIR}/src/config"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        DEPENDS ${CODEGEN_SCRIPTS}
        COMMENT "Re-generating configuration parser in src/config"
        VERBATIM
    )
endif()

# -----------------------------------------------------------------------------
# Source Files Selection
# -----------------------------------------------------------------------------
file(GLOB_RECURSE DSP_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
if(APPLE)
    file(GLOB_RECURSE DSP_OBJC_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.m")
    list(APPEND DSP_SRCS ${DSP_OBJC_SRCS})
    if(DSP_OBJC_SRCS)
        set_source_files_properties(${DSP_OBJC_SRCS} PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    endif()
endif()

if(ENABLE_WEBSOCKET)
    file(GLOB SERVER_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/app/server/*.c")
else()
    set(SERVER_SRCS "")
endif()

# -----------------------------------------------------------------------------
# Core Library: dsp
# -----------------------------------------------------------------------------
add_library(dsp STATIC ${DSP_SRCS})
add_library(cdsp::dsp ALIAS dsp)

target_include_directories(dsp PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/cdsp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/cdsp>
)

target_compile_definitions(dsp PUBLIC CDSP_BUILD_SHARED)

# -----------------------------------------------------------------------------
# Base Platform Dependencies
# -----------------------------------------------------------------------------
find_package(Threads REQUIRED)
target_link_libraries(dsp PUBLIC Threads::Threads)

find_package(PkgConfig QUIET)

if(WIN32)
    target_compile_definitions(dsp PUBLIC
        COBJMACROS
        _WIN32
        UNICODE
        _UNICODE
        _USE_MATH_DEFINES
        YAML_DECLARE_STATIC
        CJSON_HIDE_SYMBOLS
    )
    target_link_libraries(dsp PUBLIC ws2_32 winmm ole32 uuid ksuser ksguid avrt synchronization)
elseif(APPLE)
    target_link_libraries(dsp PUBLIC "-framework CoreFoundation" m)
elseif(EMSCRIPTEN)
    target_link_libraries(dsp PUBLIC m)
else()
    target_compile_definitions(dsp PUBLIC _GNU_SOURCE)
    target_link_libraries(dsp PUBLIC m rt)
endif()

# -----------------------------------------------------------------------------
# Audio Backends
# -----------------------------------------------------------------------------
if(ENABLE_COREAUDIO)
    target_compile_definitions(dsp PUBLIC ENABLE_COREAUDIO)
    target_link_libraries(dsp PUBLIC "-framework CoreAudio" "-framework AudioToolbox" "-framework Foundation")
endif()

if(ENABLE_ACCELERATE)
    target_compile_definitions(dsp PUBLIC ENABLE_ACCELERATE)
    target_link_libraries(dsp PUBLIC "-framework Accelerate")
endif()

if(ENABLE_WASAPI)
    target_compile_definitions(dsp PUBLIC ENABLE_WASAPI)
endif()

if(ENABLE_ASIO)
    target_compile_definitions(dsp PUBLIC ENABLE_ASIO)
endif()

if(ENABLE_WEBAUDIO)
    target_compile_definitions(dsp PUBLIC ENABLE_WEBAUDIO)
endif()

if(ENABLE_ALSA)
    find_package(ALSA QUIET)
    if(TARGET ALSA::ALSA)
        target_link_libraries(dsp PUBLIC ALSA::ALSA)
        target_compile_definitions(dsp PUBLIC ENABLE_ALSA)
    elseif(PKG_CONFIG_FOUND)
        pkg_check_modules(ALSA QUIET IMPORTED_TARGET alsa)
        if(TARGET PkgConfig::ALSA)
            target_link_libraries(dsp PUBLIC PkgConfig::ALSA)
            target_compile_definitions(dsp PUBLIC ENABLE_ALSA)
        endif()
    endif()
endif()

if(ENABLE_PIPEWIRE AND PKG_CONFIG_FOUND)
    pkg_check_modules(PIPEWIRE QUIET IMPORTED_TARGET libpipewire-0.3)
    if(TARGET PkgConfig::PIPEWIRE)
        target_link_libraries(dsp PUBLIC PkgConfig::PIPEWIRE)
        target_compile_definitions(dsp PUBLIC ENABLE_PIPEWIRE)
    endif()
endif()

if(ENABLE_DBUS AND PKG_CONFIG_FOUND)
    pkg_check_modules(DBUS QUIET IMPORTED_TARGET dbus-1)
    if(TARGET PkgConfig::DBUS)
        target_link_libraries(dsp PUBLIC PkgConfig::DBUS)
        target_compile_definitions(dsp PUBLIC ENABLE_DBUS)
    else()
        target_compile_definitions(dsp PUBLIC NO_DBUS)
    endif()
else()
    target_compile_definitions(dsp PUBLIC NO_DBUS)
endif()

if(ENABLE_WEBSOCKET)
    target_compile_definitions(dsp PUBLIC ENABLE_WEBSOCKET)

    # OpenSSL (needed by libwebsockets headers and linking)
    find_package(OpenSSL QUIET)
    if(TARGET OpenSSL::SSL)
        target_link_libraries(dsp PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    endif()

    set(WEBSOCKETS_FOUND_FLAG FALSE)
    find_path(WEBSOCKETS_INCLUDE_DIR NAMES libwebsockets.h PATHS /opt/homebrew/include /usr/local/include /usr/include)
    find_library(WEBSOCKETS_LIB NAMES websockets libwebsockets libwebsockets_static PATHS /opt/homebrew/lib /usr/local/lib /usr/lib)

    if(WEBSOCKETS_INCLUDE_DIR AND WEBSOCKETS_LIB)
        target_include_directories(dsp PUBLIC ${WEBSOCKETS_INCLUDE_DIR})
        target_link_libraries(dsp PUBLIC ${WEBSOCKETS_LIB})
        set(WEBSOCKETS_FOUND_FLAG TRUE)
    elseif(PKG_CONFIG_FOUND)
        pkg_check_modules(WEBSOCKETS QUIET IMPORTED_TARGET libwebsockets)
        if(TARGET PkgConfig::WEBSOCKETS)
            target_link_libraries(dsp PUBLIC PkgConfig::WEBSOCKETS)
            set(WEBSOCKETS_FOUND_FLAG TRUE)
        endif()
    endif()

    if(NOT WEBSOCKETS_FOUND_FLAG)
        find_package(Libwebsockets CONFIG QUIET)
        if(TARGET websockets OR TARGET websockets_shared)
            if(TARGET websockets)
                target_link_libraries(dsp PUBLIC websockets)
            else()
                target_link_libraries(dsp PUBLIC websockets_shared)
            endif()
            set(WEBSOCKETS_FOUND_FLAG TRUE)
        endif()
    endif()

    if(NOT WEBSOCKETS_FOUND_FLAG)
        message(FATAL_ERROR "libwebsockets is required when ENABLE_WEBSOCKET=ON.")
    endif()

    if(WIN32)
        find_library(LIBUV_LIB NAMES uv libuv)
        if(LIBUV_LIB)
            target_link_libraries(dsp PUBLIC ${LIBUV_LIB})
        endif()
        find_library(SSL_LIB NAMES ssl libssl)
        find_library(CRYPTO_LIB NAMES crypto libcrypto)
        if(SSL_LIB AND CRYPTO_LIB)
            target_link_libraries(dsp PUBLIC ${SSL_LIB} ${CRYPTO_LIB})
        endif()
        target_link_libraries(dsp PUBLIC crypt32 dbghelp userenv iphlpapi synchronization)
    endif()
endif()


# -----------------------------------------------------------------------------
# Math, FFT & Acceleration Libraries (FFTW3 Required)
# -----------------------------------------------------------------------------
set(FFTW_FOUND_FLAG FALSE)

if(EMSCRIPTEN)
    include(FetchContent)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    FetchContent_Declare(
        fftw_tarball
        URL https://www.fftw.org/fftw-3.3.10.tar.gz
    )
    FetchContent_GetProperties(fftw_tarball)
    if(NOT fftw_tarball_POPULATED)
        FetchContent_Populate(fftw_tarball)
        set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        set(ENABLE_THREADS OFF CACHE BOOL "" FORCE)
        set(ENABLE_OPENMP OFF CACHE BOOL "" FORCE)

        set(ENABLE_FLOAT OFF CACHE BOOL "" FORCE)
        add_subdirectory(${fftw_tarball_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/fftw3_double EXCLUDE_FROM_ALL)

        set(ENABLE_FLOAT ON CACHE BOOL "" FORCE)
        add_subdirectory(${fftw_tarball_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/fftw3_float EXCLUDE_FROM_ALL)
    endif()
    target_include_directories(dsp PUBLIC ${fftw_tarball_SOURCE_DIR}/api)
    target_link_libraries(dsp PUBLIC fftw3 fftw3f)
    target_compile_definitions(dsp PUBLIC ENABLE_FFTW)
    set(FFTW_FOUND_FLAG TRUE)
else()
    # 1. Search for headers and full absolute library paths first (prevents MSYS2 FFTW3Config.cmake relative path bugs)
    find_path(FFTW3_INCLUDE_DIR NAMES fftw3.h PATHS /opt/homebrew/include /usr/local/include /usr/include)
    find_library(FFTW3_LIB NAMES fftw3 libfftw3-3 libfftw3 PATHS /opt/homebrew/lib /usr/local/lib /usr/lib)
    find_library(FFTW3F_LIB NAMES fftw3f libfftw3f-3 libfftw3f PATHS /opt/homebrew/lib /usr/local/lib /usr/lib)

    if(FFTW3_INCLUDE_DIR AND FFTW3_LIB AND FFTW3F_LIB)
        target_include_directories(dsp PUBLIC ${FFTW3_INCLUDE_DIR})
        target_link_libraries(dsp PUBLIC ${FFTW3_LIB} ${FFTW3F_LIB})
        target_compile_definitions(dsp PUBLIC ENABLE_FFTW)
        set(FFTW_FOUND_FLAG TRUE)
    elseif(PKG_CONFIG_FOUND)
        pkg_check_modules(FFTW3 QUIET IMPORTED_TARGET fftw3 fftw3f)
        if(TARGET PkgConfig::FFTW3)
            target_link_libraries(dsp PUBLIC PkgConfig::FFTW3)
            target_compile_definitions(dsp PUBLIC ENABLE_FFTW)
            set(FFTW_FOUND_FLAG TRUE)
        endif()
    endif()

    if(NOT FFTW_FOUND_FLAG)
        find_package(FFTW3 CONFIG QUIET)
        find_package(FFTW3f CONFIG QUIET)
        if(TARGET FFTW3::fftw3 AND TARGET FFTW3::fftw3f)
            target_link_libraries(dsp PUBLIC FFTW3::fftw3 FFTW3::fftw3f)
            target_compile_definitions(dsp PUBLIC ENABLE_FFTW)
            set(FFTW_FOUND_FLAG TRUE)
        endif()
    endif()
endif()

if(NOT FFTW_FOUND_FLAG)
    message(FATAL_ERROR "FFTW3 (both double-precision 'fftw3' and single-precision 'fftw3f') is required to build cdsp.")
endif()

# -----------------------------------------------------------------------------
# YAML Parser & Emitter (libyaml Required)
# -----------------------------------------------------------------------------
set(YAML_FOUND_FLAG FALSE)

if(ENABLE_SANITIZER STREQUAL "memory" OR EMSCRIPTEN)
    include(FetchContent)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    FetchContent_Declare(
        yaml_src
        GIT_REPOSITORY https://github.com/yaml/libyaml.git
        GIT_TAG 0.2.5
    )
    FetchContent_GetProperties(yaml_src)
    if(NOT yaml_src_POPULATED)
        FetchContent_Populate(yaml_src)
        set(SAVED_BUILD_TESTING ${BUILD_TESTING})
        set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
        set(YAML_STATIC_LIB ON CACHE BOOL "" FORCE)
        add_subdirectory(${yaml_src_SOURCE_DIR} ${yaml_src_BINARY_DIR} EXCLUDE_FROM_ALL)
        set(BUILD_TESTING ${SAVED_BUILD_TESTING} CACHE BOOL "Build test runner and unit tests" FORCE)
    endif()
    if(TARGET yaml)
        target_include_directories(dsp PUBLIC ${yaml_src_SOURCE_DIR}/include)
        target_link_libraries(dsp PUBLIC yaml)
        set(YAML_FOUND_FLAG TRUE)
    endif()
else()
    find_path(YAML_INCLUDE_DIR NAMES yaml.h PATHS /opt/homebrew/include /usr/local/include /usr/include)
    find_library(YAML_LIB NAMES yaml libyaml PATHS /opt/homebrew/lib /usr/local/lib /usr/lib)

    if(YAML_INCLUDE_DIR AND YAML_LIB)
        target_include_directories(dsp PUBLIC ${YAML_INCLUDE_DIR})
        target_link_libraries(dsp PUBLIC ${YAML_LIB})
        set(YAML_FOUND_FLAG TRUE)
    elseif(PKG_CONFIG_FOUND)
        pkg_check_modules(YAML QUIET IMPORTED_TARGET yaml-0.1)
        if(TARGET PkgConfig::YAML)
            target_link_libraries(dsp PUBLIC PkgConfig::YAML)
            set(YAML_FOUND_FLAG TRUE)
        endif()
    endif()

    if(NOT YAML_FOUND_FLAG)
        find_package(yaml CONFIG QUIET)
        if(TARGET yaml)
            target_link_libraries(dsp PUBLIC yaml)
            set(YAML_FOUND_FLAG TRUE)
        endif()
    endif()
endif()

if(NOT YAML_FOUND_FLAG)
    message(FATAL_ERROR "libyaml is required to build cdsp.")
endif()

# -----------------------------------------------------------------------------
# JSON Parser & Generator (cJSON Required)
# -----------------------------------------------------------------------------
set(CJSON_FOUND_FLAG FALSE)

if(ENABLE_SANITIZER STREQUAL "memory" OR EMSCRIPTEN)
    include(FetchContent)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    FetchContent_Declare(
        cjson_src
        GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
        GIT_TAG v1.7.18
    )
    set(ENABLE_CJSON_TEST OFF CACHE BOOL "Build cJSON tests" FORCE)
    set(ENABLE_CUSTOM_COMPILER_FLAGS OFF CACHE BOOL "Custom compiler flags" FORCE)
    set(BUILD_SHARED_AND_STATIC_LIBS OFF CACHE BOOL "Build both" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static" FORCE)
    FetchContent_MakeAvailable(cjson_src)
    if(TARGET cjson)
        file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/cjson_include/cjson)
        file(COPY ${cjson_src_SOURCE_DIR}/cJSON.h ${cjson_src_SOURCE_DIR}/cJSON_Utils.h DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/cjson_include/cjson)
        target_include_directories(dsp PUBLIC ${cjson_src_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/cjson_include)
        target_link_libraries(dsp PUBLIC cjson)
        set(CJSON_FOUND_FLAG TRUE)
    endif()
else()
    find_path(CJSON_INCLUDE_DIR NAMES cjson/cJSON.h cJSON.h PATHS /opt/homebrew/include /usr/local/include /usr/include)
    find_library(CJSON_LIB NAMES cjson libcjson PATHS /opt/homebrew/lib /usr/local/lib /usr/lib)

    if(CJSON_INCLUDE_DIR AND CJSON_LIB)
        target_include_directories(dsp PUBLIC ${CJSON_INCLUDE_DIR})
        if(EXISTS "${CJSON_INCLUDE_DIR}/cjson")
            target_include_directories(dsp PUBLIC "${CJSON_INCLUDE_DIR}/cjson")
        endif()
        target_link_libraries(dsp PUBLIC ${CJSON_LIB})
        set(CJSON_FOUND_FLAG TRUE)
    elseif(PKG_CONFIG_FOUND)
        pkg_check_modules(CJSON QUIET IMPORTED_TARGET libcjson cjson)
        if(TARGET PkgConfig::CJSON)
            target_link_libraries(dsp PUBLIC PkgConfig::CJSON)
            set(CJSON_FOUND_FLAG TRUE)
        endif()
    endif()

    if(NOT CJSON_FOUND_FLAG)
        find_package(cJSON CONFIG QUIET)
        if(TARGET cjson)
            target_link_libraries(dsp PUBLIC cjson)
            set(CJSON_FOUND_FLAG TRUE)
        elseif(TARGET cJSON::cjson)
            target_link_libraries(dsp PUBLIC cJSON::cjson)
            set(CJSON_FOUND_FLAG TRUE)
        endif()
    endif()
endif()

if(NOT CJSON_FOUND_FLAG)
    message(FATAL_ERROR "cJSON (libcjson) is required to build cdsp.")
endif()



# -----------------------------------------------------------------------------
# Parallelization: OpenMP & libdispatch (GCD)
# -----------------------------------------------------------------------------
if(ENABLE_OPENMP)
    find_package(OpenMP QUIET)
    if(TARGET OpenMP::OpenMP_C)
        target_link_libraries(dsp PUBLIC OpenMP::OpenMP_C)
        target_compile_definitions(dsp PUBLIC ENABLE_OPENMP)
    endif()
endif()

if(ENABLE_LIBDISPATCH)
    set(DISPATCH_FOUND_FLAG FALSE)
    if(WIN32)
        target_compile_definitions(dsp PUBLIC HAVE_MODE_T ENABLE_LIBDISPATCH)
        target_link_libraries(dsp PUBLIC dispatch BlocksRuntime)
        set(DISPATCH_FOUND_FLAG TRUE)
    elseif(APPLE)
        # Built-in in Darwin libSystem
        target_compile_definitions(dsp PUBLIC ENABLE_LIBDISPATCH)
        set(DISPATCH_FOUND_FLAG TRUE)
    else()
        # 1. Discover Swift toolchain paths if swift is installed
        find_program(SWIFT_EXECUTABLE swift)
        set(SWIFT_SEARCH_PATHS "")
        if(SWIFT_EXECUTABLE)
            get_filename_component(SWIFT_REAL_PATH "${SWIFT_EXECUTABLE}" REALPATH)
            get_filename_component(SWIFT_BIN_DIR "${SWIFT_REAL_PATH}" DIRECTORY)
            get_filename_component(SWIFT_TOOLCHAIN_DIR "${SWIFT_BIN_DIR}" DIRECTORY)
            list(APPEND SWIFT_SEARCH_PATHS
                "${SWIFT_TOOLCHAIN_DIR}/lib/swift"
                "${SWIFT_TOOLCHAIN_DIR}/lib/swift/linux"
                "${SWIFT_TOOLCHAIN_DIR}/lib/swift/linux/${CMAKE_SYSTEM_PROCESSOR}"
                "${SWIFT_TOOLCHAIN_DIR}/lib/swift_static"
                "${SWIFT_TOOLCHAIN_DIR}/lib/swift_static/linux"
            )
        endif()

        # 2. Try pkg-config if available
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(DISPATCH QUIET IMPORTED_TARGET libdispatch dispatch)
            if(TARGET PkgConfig::DISPATCH)
                target_link_libraries(dsp PUBLIC PkgConfig::DISPATCH)
                target_compile_definitions(dsp PUBLIC ENABLE_LIBDISPATCH)
                set(DISPATCH_FOUND_FLAG TRUE)
            endif()
        endif()

        # 3. Fallback to searching header & libraries in Swift toolchain and system paths
        if(NOT DISPATCH_FOUND_FLAG)
            find_path(DISPATCH_INCLUDE_DIR NAMES dispatch/dispatch.h
                HINTS ${SWIFT_SEARCH_PATHS}
                PATH_SUFFIXES
                    dispatch
                    swift
                    swift/linux/${CMAKE_SYSTEM_PROCESSOR}
                    swift/linux
                    lib/swift
                    lib/swift/dispatch
                    usr/lib/swift
                    usr/libexec/swift/lib/swift
            )
            find_library(DISPATCH_LIB NAMES dispatch libdispatch
                HINTS ${SWIFT_SEARCH_PATHS}
                PATH_SUFFIXES
                    swift/linux/${CMAKE_SYSTEM_PROCESSOR}
                    swift/linux
                    lib/swift/linux
                    usr/lib/swift/linux
                    usr/libexec/swift/lib/swift/linux
            )
            find_library(BLOCKSRUNTIME_LIB NAMES BlocksRuntime libBlocksRuntime
                HINTS ${SWIFT_SEARCH_PATHS}
                PATH_SUFFIXES
                    swift/linux/${CMAKE_SYSTEM_PROCESSOR}
                    swift/linux
                    lib/swift/linux
                    usr/lib/swift/linux
                    usr/libexec/swift/lib/swift/linux
            )

            if(DISPATCH_INCLUDE_DIR AND DISPATCH_LIB)
                target_include_directories(dsp PUBLIC ${DISPATCH_INCLUDE_DIR})
                target_link_libraries(dsp PUBLIC ${DISPATCH_LIB})
                if(BLOCKSRUNTIME_LIB)
                    target_link_libraries(dsp PUBLIC ${BLOCKSRUNTIME_LIB})
                endif()
                get_filename_component(DISPATCH_LIB_DIR "${DISPATCH_LIB}" DIRECTORY)
                target_link_directories(dsp PUBLIC "${DISPATCH_LIB_DIR}")
                target_link_options(dsp PUBLIC "-Wl,-rpath,${DISPATCH_LIB_DIR}")
                target_compile_definitions(dsp PUBLIC ENABLE_LIBDISPATCH)
                set(DISPATCH_FOUND_FLAG TRUE)
            endif()
        endif()

        if(DISPATCH_FOUND_FLAG)
            message(STATUS "libdispatch (GCD) found and enabled: ${DISPATCH_LIB}")
        else()
            message(WARNING "ENABLE_LIBDISPATCH was requested but libdispatch headers or libraries were not found.")
        endif()
    endif()
endif()

# -----------------------------------------------------------------------------
# CLI Executable: cdsp
# -----------------------------------------------------------------------------
if(NOT EMSCRIPTEN)
    add_executable(cdsp app/main.c ${SERVER_SRCS})
    target_link_libraries(cdsp PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,dsp>")
    target_include_directories(cdsp PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/app
        ${CMAKE_CURRENT_SOURCE_DIR}/app/server
    )
endif()

# -----------------------------------------------------------------------------
# Testing: libdsp_test & CTest Test Infrastructure
# -----------------------------------------------------------------------------
if(BUILD_TESTING AND NOT EMSCRIPTEN)
    include(CTest)

    # Optional Rust Harness Build (if cargo is installed and BUILD_RUST_HARNESS is ON)
    if(BUILD_RUST_HARNESS)
        find_program(CARGO_EXECUTABLE cargo)
        if(CARGO_EXECUTABLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/rust_harnesses/Cargo.toml")
            set(RUST_HARNESS_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/rust_target")
            add_custom_target(test-rust-build
                COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${RUST_HARNESS_TARGET_DIR} ${CARGO_EXECUTABLE} build --release
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests/rust_harnesses
                COMMENT "Building Rust harness binaries in ${RUST_HARNESS_TARGET_DIR}..."
                VERBATIM
            )
        endif()
    endif()

    add_library(dsp_test STATIC ${DSP_SRCS})
    target_link_libraries(dsp_test PUBLIC dsp)
    target_compile_definitions(dsp_test PUBLIC
        CDSP_TEST
        CDSP_COMBINED_TEST_SUITE
    )

    file(GLOB TEST_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.c")
    list(FILTER TEST_SRCS EXCLUDE REGEX ".*/test_runner_main\\.c$")
    list(FILTER TEST_SRCS EXCLUDE REGEX ".*/test_filter_benchmark\\.c$")
    list(FILTER TEST_SRCS EXCLUDE REGEX ".*/test_dsd_benchmark\\.c$")
    list(FILTER TEST_SRCS EXCLUDE REGEX ".*/test_pipeline_benchmark\\.c$")
    list(FILTER TEST_SRCS EXCLUDE REGEX ".*/test_resampler_matrix\\.c$")
    list(FILTER TEST_SRCS EXCLUDE REGEX ".*/test_simd_benchmark\\.c$")
    list(FILTER TEST_SRCS EXCLUDE REGEX ".*/test_convolution_backends_benchmark\\.c$")
    list(FILTER TEST_SRCS EXCLUDE REGEX ".*/test_wasm\\.c$")

    if(NOT ENABLE_WEBSOCKET)
        list(FILTER TEST_SRCS EXCLUDE REGEX ".*/test_websocket_server\\.c$")
    endif()

    if(NOT ENABLE_WEBAUDIO)
        list(FILTER TEST_SRCS EXCLUDE REGEX ".*/test_webaudio_backend\\.c$")
    endif()

    add_executable(test_runner
        tests/test_runner_main.c
        ${TEST_SRCS}
        ${SERVER_SRCS}
    )
    target_link_libraries(test_runner PRIVATE dsp_test)
    target_include_directories(test_runner PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CMAKE_CURRENT_SOURCE_DIR}/app
        ${CMAKE_CURRENT_SOURCE_DIR}/app/server
    )
    target_compile_definitions(test_runner PRIVATE
        CDSP_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
        CDSP_BINARY_DIR="${CMAKE_BINARY_DIR}"
    )

    if(TARGET test-rust-build)
        add_dependencies(test_runner test-rust-build)
    endif()

    if(WIN32)
        target_link_options(test_runner PRIVATE
            -Wl,--wrap,malloc -Wl,--wrap,calloc -Wl,--wrap,realloc -Wl,--wrap,free
            -Wl,--wrap,fopen
        )
    endif()

    # CTest Test Discovery: Register each unit test case as an individual CTest test
    set(CDSP_DISCOVERED_TESTS_FILE "${CMAKE_CURRENT_BINARY_DIR}/cdsp_tests.cmake")
    if(NOT EXISTS "${CDSP_DISCOVERED_TESTS_FILE}")
        file(WRITE "${CDSP_DISCOVERED_TESTS_FILE}" "# Automatically populated by test_runner discovery\nadd_test(NAME test_runner_suite COMMAND test_runner)\nset_tests_properties(test_runner_suite PROPERTIES LABELS \"unit\")\n")
    endif()
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${CDSP_DISCOVERED_TESTS_FILE}")

    add_custom_command(
        TARGET test_runner POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DTEST_EXECUTABLE=$<TARGET_FILE:test_runner>
            -DCTEST_FILE=${CDSP_DISCOVERED_TESTS_FILE}
            -DLIBARCHER_PATH=${LIBARCHER_PATH}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/CdspDiscoverTests.cmake
        COMMENT "Discovering CTest test cases from test_runner..."
        VERBATIM
    )

    # Convenience target
    add_custom_target(check
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -j
        DEPENDS test_runner
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running unit test suite in parallel via CTest..."
    )
endif()

# -----------------------------------------------------------------------------
# Benchmarks
# -----------------------------------------------------------------------------
if(BUILD_BENCHMARKS)
    set(BENCHMARK_NAMES
        test_filter_benchmark
        test_dsd_benchmark
        test_pipeline_benchmark
        test_resampler_matrix
        test_simd_benchmark
        test_convolution_backends_benchmark
    )

    set(BENCH_TARGETS "")
    foreach(bench_name ${BENCHMARK_NAMES})
        add_executable(${bench_name} tests/${bench_name}.c)
        target_link_libraries(${bench_name} PRIVATE dsp)
        target_include_directories(${bench_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        target_compile_definitions(${bench_name} PRIVATE
            CDSP_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
            CDSP_BINARY_DIR="${CMAKE_BINARY_DIR}"
        )
        list(APPEND BENCH_TARGETS ${bench_name})
    endforeach()

    if(TARGET test_convolution_backends_benchmark)
        if(APPLE)
            target_compile_definitions(test_convolution_backends_benchmark PRIVATE ENABLE_ACCELERATE)
            target_link_libraries(test_convolution_backends_benchmark PRIVATE "-framework Accelerate")
        endif()
        if(FFTW_FOUND_FLAG)
            target_compile_definitions(test_convolution_backends_benchmark PRIVATE HAS_FFTW)
        endif()
    endif()

    if(TARGET test_simd_benchmark)
        if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(test_simd_benchmark PRIVATE -UENABLE_ACCELERATE)
        elseif(MSVC)
            target_compile_options(test_simd_benchmark PRIVATE /UENABLE_ACCELERATE)
        endif()

        if(APPLE)
            target_compile_definitions(test_simd_benchmark PRIVATE ACCELERATE_NEW_LAPACK)
            target_link_libraries(test_simd_benchmark PRIVATE "-framework Accelerate")
        else()
            if(PKG_CONFIG_FOUND)
                pkg_check_modules(OPENBLAS QUIET IMPORTED_TARGET openblas cblas)
            endif()
            if(TARGET PkgConfig::OPENBLAS)
                target_compile_definitions(test_simd_benchmark PRIVATE HAS_CBLAS)
                target_link_libraries(test_simd_benchmark PRIVATE PkgConfig::OPENBLAS)
            else()
                find_path(CBLAS_INCLUDE_DIR NAMES cblas.h
                    PATH_SUFFIXES openblas openblas-pthread openblas-openmp openblas-serial cblas
                )
                find_library(CBLAS_LIB NAMES openblas cblas blas libopenblas)
                if(CBLAS_INCLUDE_DIR)
                    target_include_directories(test_simd_benchmark PRIVATE ${CBLAS_INCLUDE_DIR})
                endif()
                if(CBLAS_LIB)
                    target_compile_definitions(test_simd_benchmark PRIVATE HAS_CBLAS)
                    target_link_libraries(test_simd_benchmark PRIVATE ${CBLAS_LIB})
                else()
                    find_package(BLAS QUIET)
                    if(BLAS_FOUND)
                        target_compile_definitions(test_simd_benchmark PRIVATE HAS_CBLAS)
                        target_link_libraries(test_simd_benchmark PRIVATE ${BLAS_LIBRARIES})
                    endif()
                endif()
            endif()
        endif()
    endif()

    add_custom_target(bench
        COMMAND $<TARGET_FILE:test_filter_benchmark>
        COMMAND $<TARGET_FILE:test_dsd_benchmark>
        COMMAND $<TARGET_FILE:test_pipeline_benchmark>
        COMMAND $<TARGET_FILE:test_resampler_matrix>
        COMMAND $<TARGET_FILE:test_simd_benchmark>
        COMMAND $<TARGET_FILE:test_convolution_backends_benchmark>
        DEPENDS ${BENCH_TARGETS}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running C benchmarks..."
    )
endif()

# -----------------------------------------------------------------------------
# Custom Targets: Call Graph Audit, Clang-Format
# -----------------------------------------------------------------------------
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_FOUND)
    add_custom_target(callgraph-audit
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_callgraph.py
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Running Static Call Graph Audit..."
    )
endif()

find_program(CLANG_FORMAT_BIN clang-format)
if(CLANG_FORMAT_BIN)
    file(GLOB_RECURSE ALL_C_AND_H_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
    )
    list(FILTER ALL_C_AND_H_FILES EXCLUDE REGEX ".*/build.*")
    list(FILTER ALL_C_AND_H_FILES EXCLUDE REGEX ".*/\\.build.*")
    list(FILTER ALL_C_AND_H_FILES EXCLUDE REGEX ".*/tests/rust_harnesses/.*")
    list(FILTER ALL_C_AND_H_FILES EXCLUDE REGEX ".*/config_gen\\.[ch]$")

    add_custom_target(format
        COMMAND ${CLANG_FORMAT_BIN} -i ${ALL_C_AND_H_FILES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Formatting all source and header files with clang-format..."
    )
    add_custom_target(format-check
        COMMAND ${CLANG_FORMAT_BIN} --dry-run -Werror ${ALL_C_AND_H_FILES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Checking code formatting with clang-format..."
    )
endif()

# Include-What-You-Use (IWYU) targets
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/run-iwyu.sh")
    add_custom_target(iwyu
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/run-iwyu.sh" --check
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Running Include-What-You-Use analysis..."
        USES_TERMINAL
    )
    add_custom_target(iwyu-fix
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/run-iwyu.sh" --fix
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Applying Include-What-You-Use fixes..."
        USES_TERMINAL
    )
endif()

# -----------------------------------------------------------------------------
# Installation Rules
# -----------------------------------------------------------------------------
include(GNUInstallDirs)

if(TARGET cdsp)
    install(TARGETS cdsp dsp
        EXPORT cdspTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
else()
    install(TARGETS dsp
        EXPORT cdspTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)

if(UNIX AND NOT APPLE)
    # Post-install: Grant cap_sys_nice=ep capability for Real-Time priority scheduling (SCHED_FIFO)
    install(CODE "
        find_program(SETCAP_BIN setcap)
        if(SETCAP_BIN)
            set(TARGET_BIN \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/cdsp\")
            if(EXISTS \"\${TARGET_BIN}\")
                message(STATUS \"Applying cap_sys_nice=ep capability to \${TARGET_BIN}...\")
                execute_process(
                    COMMAND \${SETCAP_BIN} cap_sys_nice=ep \${TARGET_BIN}
                    RESULT_VARIABLE SETCAP_RES
                )
                if(NOT SETCAP_RES EQUAL 0)
                    message(WARNING \"Failed to set capability (requires root/sudo). Run manually: sudo setcap cap_sys_nice=ep \${TARGET_BIN}\")
                endif()
            endif()
        else()
            message(WARNING \"'setcap' tool not found. Real-time scheduling capability was not applied.\")
        endif()
    ")
endif()

# -----------------------------------------------------------------------------
# CDSP Studio (Qt GUI Application)
# -----------------------------------------------------------------------------
if(ENABLE_STUDIO)
    add_subdirectory(studio)
endif()

# -----------------------------------------------------------------------------
# WebAssembly & Chrome Extension
# -----------------------------------------------------------------------------
if(EMSCRIPTEN)
    add_subdirectory(extension)
endif()

# -----------------------------------------------------------------------------
# Plugins (ALSA Rate Notify Plugin for Linux)
# -----------------------------------------------------------------------------
if(UNIX AND NOT APPLE AND ENABLE_ALSA)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(ALSA QUIET alsa)
        if(ALSA_FOUND)
            add_subdirectory(plugins)
        endif()
    endif()
endif()

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dsd/dsd_encoder.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dsd/sigma_delta_modulator.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dsd.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_resampler_matrix.c"
    PROPERTIES COMPILE_OPTIONS "-fno-associative-math"
)
