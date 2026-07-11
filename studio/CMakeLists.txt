cmake_minimum_required(VERSION 3.16)
project(MonitorQt LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Concurrent)
find_package(Threads REQUIRED)

# Define system libraries based on platform
if(APPLE)
    find_library(COREAUDIO_LIB CoreAudio REQUIRED)
    find_library(AUDIOTOOLBOX_LIB AudioToolbox REQUIRED)
    find_library(ACCELERATE_LIB Accelerate REQUIRED)
    find_library(COREFOUNDATION_LIB CoreFoundation REQUIRED)
    set(PLATFORM_LIBS ${COREAUDIO_LIB} ${AUDIOTOOLBOX_LIB} ${ACCELERATE_LIB} ${COREFOUNDATION_LIB})
    add_compile_definitions(ENABLE_COREAUDIO ENABLE_ACCELERATE)
elseif(WIN32)
    set(PLATFORM_LIBS ws2_32 winmm ole32 uuid ksuser ksguid)
    add_compile_definitions(ENABLE_WASAPI ENABLE_ASIO _WIN32 COBJMACROS UNICODE _UNICODE _USE_MATH_DEFINES)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ALSA alsa)
    pkg_check_modules(PULSE libpulse libpulse-simple)
    set(PLATFORM_LIBS m rt ${ALSA_LIBRARIES} ${PULSE_LIBRARIES})
    add_compile_definitions(ENABLE_ALSA ENABLE_PULSE _GNU_SOURCE)
endif()

# CDSP FetchContent / Local Fallback (excluding Server directory)
include(FetchContent)
get_filename_component(USER_HOME "~" REALPATH)
set(LOCAL_CDSP_DIR "${USER_HOME}/cdsp")

if(EXISTS "${LOCAL_CDSP_DIR}/libdsp.a")
    message(STATUS "Using local CDSP library at ${LOCAL_CDSP_DIR}")
    set(CDSP_DIR "${LOCAL_CDSP_DIR}")
    set(CDSP_LIB "${LOCAL_CDSP_DIR}/libdsp.a")
else()
    message(STATUS "Local CDSP not found. Fetching Wang-Yue/cdsp from GitHub...")
    FetchContent_Declare(
        cdsp
        GIT_REPOSITORY https://github.com/Wang-Yue/cdsp.git
        GIT_TAG        main
    )
    FetchContent_MakeAvailable(cdsp)
    set(CDSP_DIR "${cdsp_SOURCE_DIR}")
    if(EXISTS "${CDSP_DIR}/libdsp.a")
        set(CDSP_LIB "${CDSP_DIR}/libdsp.a")
    else()
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
        )
        set(CDSP_LIB cdsp_static)
    endif()
endif()

include_directories(
    src
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
)

# Collect source files
file(GLOB_RECURSE SOURCES "src/*.cpp" "src/*.h")

add_executable(MonitorQt ${SOURCES})

target_link_libraries(MonitorQt PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    Qt6::Concurrent
    Threads::Threads
    ${CDSP_LIB}
    ${PLATFORM_LIBS}
)

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

