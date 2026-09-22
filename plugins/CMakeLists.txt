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
    C_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF
)

target_compile_options(asound_module_pcm_rate_notify PRIVATE
    -fvisibility=default
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

# Determine target user's home directory (resolves real user even when run under sudo)
if(DEFINED ENV{SUDO_USER} AND NOT "$ENV{SUDO_USER}" STREQUAL "" AND NOT "$ENV{SUDO_USER}" STREQUAL "root")
    execute_process(
        COMMAND sh -c "getent passwd $SUDO_USER | cut -d: -f6"
        OUTPUT_VARIABLE TARGET_HOME
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif()
if(NOT TARGET_HOME)
    set(TARGET_HOME "$ENV{HOME}")
endif()

set(ALSA_PLUGIN_DIR "${TARGET_HOME}/.local/lib/alsa-lib")
set(ALSA_CONF_DIR "${TARGET_HOME}/.config/alsa/conf.d")
set(PIPEWIRE_CONF_DIR "${TARGET_HOME}/.config/pipewire/pipewire.conf.d")
set(PIPEWIRE_PULSE_CONF_DIR "${TARGET_HOME}/.config/pipewire/pipewire-pulse.conf.d")
set(WIREPLUMBER_CONF_DIR "${TARGET_HOME}/.config/wireplumber/wireplumber.conf.d")

install(TARGETS asound_module_pcm_rate_notify
    LIBRARY DESTINATION ${ALSA_PLUGIN_DIR}
)

install(FILES asoundrc
    DESTINATION ${TARGET_HOME}
    RENAME .asoundrc
)

install(FILES 50-cdsp.conf
    DESTINATION ${ALSA_CONF_DIR}
)

install(FILES
    pipewire/50-bitperfect-dac.conf
    pipewire/60-cdsp-sink.conf
    DESTINATION ${PIPEWIRE_CONF_DIR}
)

install(FILES pipewire-pulse/99-bitperfect.conf
    DESTINATION ${PIPEWIRE_PULSE_CONF_DIR}
)

install(FILES
    wireplumber/50-bitperfect-dac.conf
    wireplumber/50-loopback.conf
    DESTINATION ${WIREPLUMBER_CONF_DIR}
)

# Automatically fix ownership and restart PipeWire/WirePlumber user services
install(CODE "
    find_program(_SYSTEMCTL systemctl)
    if(DEFINED ENV{SUDO_USER} AND NOT \"\$ENV{SUDO_USER}\" STREQUAL \"\" AND NOT \"\$ENV{SUDO_USER}\" STREQUAL \"root\")
        set(_USER \"\$ENV{SUDO_USER}\")
        execute_process(COMMAND id -gn \${_USER} OUTPUT_VARIABLE _GID OUTPUT_STRIP_TRAILING_WHITESPACE)

        message(STATUS \"Setting user ownership of installed configs to \${_USER}:\${_GID}...\")
        execute_process(
            COMMAND chown -R \${_USER}:\${_GID}
                \"${ALSA_PLUGIN_DIR}\"
                \"${ALSA_CONF_DIR}\"
                \"${PIPEWIRE_CONF_DIR}\"
                \"${PIPEWIRE_PULSE_CONF_DIR}\"
                \"${WIREPLUMBER_CONF_DIR}\"
                \"${TARGET_HOME}/.asoundrc\"
        )

        if(_SYSTEMCTL)
            message(STATUS \"Restarting PipeWire and WirePlumber user services for \${_USER}...\")
            execute_process(
                COMMAND \${_SYSTEMCTL} --machine=\${_USER}@.host --user restart pipewire pipewire-pulse wireplumber
                RESULT_VARIABLE _res
                OUTPUT_QUIET
                ERROR_QUIET
            )
            if(_res EQUAL 0)
                message(STATUS \"Successfully restarted PipeWire and WirePlumber user services for \${_USER}\")
            endif()
        endif()
    else()
        if(_SYSTEMCTL)
            execute_process(
                COMMAND \${_SYSTEMCTL} --user restart pipewire pipewire-pulse wireplumber
                RESULT_VARIABLE _res
                OUTPUT_QUIET
                ERROR_QUIET
            )
            if(_res EQUAL 0)
                message(STATUS \"Successfully restarted PipeWire and WirePlumber user services\")
            endif()
        endif()
    endif()
")



