cmake_minimum_required(VERSION 3.16)

# Detect if built standalone or as a subdirectory of the parent cdsp project
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(simd_bench C)
    set(STANDALONE_BUILD TRUE)
    set(CDSP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

    # Options matching parent CMakeLists.txt
    option(ENABLE_NATIVE_ARCH "Enable native CPU optimization" ON)

    # Compiler optimization flags matching parent CMakeLists.txt
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        add_compile_options(
            -Wall
            -Wextra
            -O3
            -fassociative-math
            -fno-signed-zeros
            -fno-trapping-math
            -ffp-contract=fast
            -fno-math-errno
            -funroll-loops
            -fvisibility=hidden
        )

        if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
            add_compile_options(
                -fvariable-expansion-in-unroller
                --param=max-variable-expansions-in-unroller=4
            )
        endif()

        if(ENABLE_NATIVE_ARCH AND NOT CMAKE_CROSSCOMPILING)
            if(APPLE)
                add_compile_options(-mcpu=native)
            else()
                add_compile_options(-march=native)
            endif()
        endif()
    endif()
else()
    set(STANDALONE_BUILD FALSE)
    set(CDSP_ROOT "${CMAKE_SOURCE_DIR}")
endif()

# -----------------------------------------------------------------------------
# simd_bench executable (Unified vDSP + BLAS + Pure C Benchmark)
# -----------------------------------------------------------------------------
add_executable(simd_bench simd_bench.c)

target_include_directories(simd_bench PRIVATE
    ${CDSP_ROOT}
)

if(APPLE)
    target_compile_definitions(simd_bench PRIVATE ACCELERATE_NEW_LAPACK)
    target_link_libraries(simd_bench PRIVATE "-framework Accelerate")
else()
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(OPENBLAS QUIET IMPORTED_TARGET openblas cblas)
    endif()

    if(TARGET PkgConfig::OPENBLAS)
        target_compile_definitions(simd_bench PRIVATE HAS_CBLAS)
        target_link_libraries(simd_bench PRIVATE PkgConfig::OPENBLAS)
    else()
        find_path(CBLAS_INCLUDE_DIR NAMES cblas.h
            PATH_SUFFIXES openblas openblas-pthread openblas-openmp openblas-serial cblas
        )
        find_library(CBLAS_LIB NAMES openblas cblas blas libopenblas)
        if(CBLAS_INCLUDE_DIR)
            target_include_directories(simd_bench PRIVATE ${CBLAS_INCLUDE_DIR})
        endif()
        if(CBLAS_LIB)
            target_compile_definitions(simd_bench PRIVATE HAS_CBLAS)
            target_link_libraries(simd_bench PRIVATE ${CBLAS_LIB})
        else()
            find_package(BLAS QUIET)
            if(BLAS_FOUND)
                target_compile_definitions(simd_bench PRIVATE HAS_CBLAS)
                target_link_libraries(simd_bench PRIVATE ${BLAS_LIBRARIES})
            endif()
        endif()
    endif()
endif()

# Math and threading libraries
find_package(Threads REQUIRED)
target_link_libraries(simd_bench PRIVATE m Threads::Threads)

# Convenience target to build and run benchmark
add_custom_target(run
    COMMAND $<TARGET_FILE:simd_bench>
    DEPENDS simd_bench
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running unified SIMD benchmark..."
)
