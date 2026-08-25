cmake_minimum_required(VERSION 3.20)
project(asound_module_pcm_rate_notify LANGUAGES C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA REQUIRED alsa)

add_library(asound_module_pcm_rate_notify MODULE
    pcm_rate_notify.c
)

set_target_properties(asound_module_pcm_rate_notify PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "asound_module_pcm_rate_notify"
)

target_compile_definitions(asound_module_pcm_rate_notify PRIVATE
    PIC
    _GNU_SOURCE
)

target_include_directories(asound_module_pcm_rate_notify PRIVATE
    ${ALSA_INCLUDE_DIRS}
)

target_link_libraries(asound_module_pcm_rate_notify PRIVATE
    ${ALSA_LIBRARIES}
)

include(GNUInstallDirs)

# Detect system ALSA plugin directory dynamically across all CPU architectures and Linux distros
pkg_get_variable(ALSA_PLUGIN_DIR alsa plugindir)
if(NOT ALSA_PLUGIN_DIR)
    pkg_get_variable(ALSA_LIB_DIR alsa libdir)
    if(ALSA_LIB_DIR)
        set(ALSA_PLUGIN_DIR "${ALSA_LIB_DIR}/alsa-lib")
    else()
        set(ALSA_PLUGIN_DIR "${CMAKE_INSTALL_FULL_LIBDIR}/alsa-lib")
    endif()
endif()

install(TARGETS asound_module_pcm_rate_notify
    LIBRARY DESTINATION ${ALSA_PLUGIN_DIR}
)

