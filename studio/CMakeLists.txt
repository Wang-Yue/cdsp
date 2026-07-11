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

# CDSP paths
get_filename_component(USER_HOME "~" REALPATH)
set(CDSP_DIR "${USER_HOME}/cdsp")

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
    ${CDSP_DIR}/Server
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
    "${CDSP_DIR}/libdsp.a"
    ${PLATFORM_LIBS}
)

