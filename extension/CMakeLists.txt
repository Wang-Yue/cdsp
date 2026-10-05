# ==============================================================================
# extension/CMakeLists.txt - WebAssembly Test & Chrome Extension Package
# ==============================================================================

cmake_minimum_required(VERSION 3.20)

if(EMSCRIPTEN)
    set(CHROME_EXT_DIR "${CMAKE_BINARY_DIR}/extension_dist")
    set(EXT_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json")

    # -------------------------------------------------------------------------
    # 1. WebAssembly Test Target (runs under node when EMSCRIPTEN is active)
    #
    # Runs the real multithreaded engine with WebAudio capture/playback backends
    # and drives the WebAudio device callback from the test, as the AudioWorklet
    # does in the browser. PROXY_TO_PTHREAD runs main() on a pthread so it can
    # block while the engine's threads start.
    # -------------------------------------------------------------------------
    if(BUILD_TESTING)
        add_executable(test_wasm
            ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_wasm.c
        )
        target_include_directories(test_wasm PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../include
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/config
            ${CMAKE_CURRENT_SOURCE_DIR}/../tests
        )
        target_link_libraries(test_wasm PRIVATE dsp)
        set_target_properties(test_wasm PROPERTIES
            OUTPUT_NAME "test_wasm"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
            SUFFIX ".js"
        )
        target_compile_options(test_wasm PRIVATE -O3 -msimd128)
        target_link_options(test_wasm PRIVATE
            -O3
            -msimd128
            -sWASM=1
            -sALLOW_MEMORY_GROWTH=1
            -sNODERAWFS=1
            -sPROXY_TO_PTHREAD=1
            -sEXIT_RUNTIME=1
            # dsp_engine_create() references the AudioWorklet device (a no-op under Node).
            -sAUDIO_WORKLET=1
            -sWASM_WORKERS=1
        )
        add_test(NAME WebAudio_Wasm_Engine COMMAND node $<TARGET_FILE:test_wasm>)
        set_tests_properties(WebAudio_Wasm_Engine PROPERTIES LABELS "unit;wasm" TIMEOUT 120)
    endif()

    # -------------------------------------------------------------------------
    # 2. Chrome Extension Package Target (Built purely in build directory)
    # -------------------------------------------------------------------------
    set(STUDIO_PKG_COMMANDS "")
    set(STUDIO_PKG_DEPENDS "")
    if(TARGET cdsp-studio)
        list(APPEND STUDIO_PKG_DEPENDS cdsp-studio)
        set(STUDIO_PKG_COMMANDS
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CHROME_EXT_DIR}/ui/studio"
            # Hand-written shell files are copied from source (not the cdsp-studio output dir) so
            # edits reach the package even when cdsp-studio is not relinked.
            COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/studio/resources/cdsp-studio.html" "${CHROME_EXT_DIR}/ui/studio/cdsp-studio.html"
            COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE_DIR:cdsp-studio>/cdsp-studio.js" "${CHROME_EXT_DIR}/ui/studio/cdsp-studio.js"
            COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE_DIR:cdsp-studio>/cdsp-studio.wasm" "${CHROME_EXT_DIR}/ui/studio/cdsp-studio.wasm"
            COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE_DIR:cdsp-studio>/cdsp-studio.worker.js" "${CHROME_EXT_DIR}/ui/studio/cdsp-studio.worker.js"
            # AudioWorklet bootstrap for the WebAudio device (-sAUDIO_WORKLET).
            COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE_DIR:cdsp-studio>/cdsp-studio.aw.js" "${CHROME_EXT_DIR}/ui/studio/cdsp-studio.aw.js"
            COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/studio/resources/studio-bootstrap.js" "${CHROME_EXT_DIR}/ui/studio/studio-bootstrap.js"
            COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE_DIR:cdsp-studio>/qtloader.js" "${CHROME_EXT_DIR}/ui/studio/qtloader.js"
            COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE_DIR:cdsp-studio>/qtlogo.svg" "${CHROME_EXT_DIR}/ui/studio/qtlogo.svg"
        )
    endif()

    add_custom_target(chrome_extension ALL
        # Start clean so files removed from the source tree never linger in the package.
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${CHROME_EXT_DIR}" "${CMAKE_BINARY_DIR}/cdsp_extension.zip"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CHROME_EXT_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/background" "${CHROME_EXT_DIR}/background"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/ui" "${CHROME_EXT_DIR}/ui"
        ${STUDIO_PKG_COMMANDS}
        COMMAND ${CMAKE_COMMAND} -E copy "${EXT_MANIFEST}" "${CHROME_EXT_DIR}/manifest.json"
        COMMAND ${CMAKE_COMMAND} -E chdir "${CHROME_EXT_DIR}" ${CMAKE_COMMAND} -E tar "cfv" "${CMAKE_BINARY_DIR}/cdsp_extension.zip" "--format=zip" "."
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        DEPENDS ${STUDIO_PKG_DEPENDS}
        COMMENT "Packaging Chrome Extension into ${CHROME_EXT_DIR} and ${CMAKE_BINARY_DIR}/cdsp_extension.zip..."
        VERBATIM
    )
endif()
