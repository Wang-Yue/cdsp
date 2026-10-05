# ==============================================================================
# extension/CMakeLists.txt - WebAssembly AudioWorklet & Chrome Extension Target
# ==============================================================================

cmake_minimum_required(VERSION 3.20)

# -----------------------------------------------------------------------------
# 1. WebAssembly Compilation Target
# -----------------------------------------------------------------------------
if(EMSCRIPTEN)
    set(CHROME_EXT_DIR "${CMAKE_BINARY_DIR}/extension_dist")
    set(EXT_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json")

    add_executable(cdsp_wasm
        wasm/cdsp_wasm.c
    )
    target_include_directories(cdsp_wasm PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/wasm
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/config
    )
    target_link_libraries(cdsp_wasm PRIVATE dsp)

    set_target_properties(cdsp_wasm PROPERTIES
        OUTPUT_NAME "cdsp_wasm"
        RUNTIME_OUTPUT_DIRECTORY "${CHROME_EXT_DIR}/wasm"
        SUFFIX ".js"
    )

    target_compile_options(cdsp_wasm PRIVATE -O3 -msimd128)
    target_link_options(cdsp_wasm PRIVATE
        -O3
        -msimd128
        -sWASM=1
        -sALLOW_MEMORY_GROWTH=1
        -sMODULARIZE=1
        -sEXPORT_NAME="loadCdspWasm"
        -sENVIRONMENT="web,worker"
        -sFILESYSTEM=0
        "-sEXPORTED_FUNCTIONS=['_malloc','_free','_cdsp_wasm_create','_cdsp_wasm_destroy','_cdsp_wasm_get_input_buffer_ptrs','_cdsp_wasm_get_output_buffer_ptrs','_cdsp_wasm_process','_cdsp_wasm_set_fader_volume','_cdsp_wasm_set_fader_mute','_cdsp_wasm_set_config_json','_cdsp_wasm_get_vu_levels','_cdsp_wasm_get_spectrum']"
        "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','stringToUTF8','UTF8ToString','getValue','setValue','allocateUTF8']"
        "--pre-js" "${CMAKE_CURRENT_SOURCE_DIR}/wasm/pre.js"
        "--extern-post-js" "${CMAKE_CURRENT_SOURCE_DIR}/wasm/post.js"
    )

    # -------------------------------------------------------------------------
    # 2. WebAssembly Test Target (runs under node when EMSCRIPTEN is active)
    # -------------------------------------------------------------------------
    if(BUILD_TESTING)
        add_executable(test_wasm
            ${CMAKE_SOURCE_DIR}/tests/test_wasm.c
            wasm/cdsp_wasm.c
        )
        target_include_directories(test_wasm PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/wasm
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/src/config
            ${CMAKE_SOURCE_DIR}/tests
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
        )
        add_test(NAME WebAudio_Wasm_Bit_Correctness COMMAND node $<TARGET_FILE:test_wasm>)
        set_tests_properties(WebAudio_Wasm_Bit_Correctness PROPERTIES LABELS "unit;wasm")
    endif()

    # -------------------------------------------------------------------------
    # 3. Chrome Extension Package Target (Built purely in build directory)
    # -------------------------------------------------------------------------
    add_custom_target(chrome_extension ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CHROME_EXT_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/background" "${CHROME_EXT_DIR}/background"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/offscreen" "${CHROME_EXT_DIR}/offscreen"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/worklet" "${CHROME_EXT_DIR}/worklet"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/ui" "${CHROME_EXT_DIR}/ui"
        COMMAND ${CMAKE_COMMAND} -E copy "${EXT_MANIFEST}" "${CHROME_EXT_DIR}/manifest.json"
        COMMAND ${CMAKE_COMMAND} -E tar "cfv" "${CMAKE_BINARY_DIR}/cdsp_extension.zip" "--format=zip" "."
        WORKING_DIRECTORY "${CHROME_EXT_DIR}"
        DEPENDS cdsp_wasm
        COMMENT "Packaging Chrome Extension into ${CHROME_EXT_DIR} and ${CMAKE_BINARY_DIR}/cdsp_extension.zip..."
        VERBATIM
    )
endif()
