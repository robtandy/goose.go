cmake_minimum_required(VERSION 3.27)
project(goose C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(GOOSE_UPSTREAM "${CMAKE_CURRENT_SOURCE_DIR}/vendor/goose")
if(NOT EXISTS "${GOOSE_UPSTREAM}/src/ast.h")
    message(STATUS "Initializing vendor/goose submodule...")
    execute_process(
        COMMAND git -c url.https://github.com/.insteadOf=git@github.com: submodule update --init --depth 1 vendor/goose
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE submod_res
    )
    if(NOT submod_res EQUAL 0)
        execute_process(
            COMMAND git submodule update --init vendor/goose
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        )
    endif()
endif()

# One C runtime for the compiler, TinyCC, SDL and the gfx library, and the
# one `cl game.c` links by default, so a program can link the same archives.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_executable(goose
    src/main.cpp
    src/process.cpp
    "${GOOSE_UPSTREAM}/src/version.cpp"
    src/process.h
    "${GOOSE_UPSTREAM}/src/progress.cpp"
    "${GOOSE_UPSTREAM}/src/progress.h"
    src/build.h
    src/codegen_stacks.h
    src/codegen_go.h
    src/codegen_go_runtime.h
    src/codegen_go_values.h
    src/codegen_go_types.h
    src/codegen_go_places.h
    src/codegen_go_aggr.h
    src/codegen_go_wide.h
    src/codegen_go_select.h
    src/codegen_go_stmts.h
    src/codegen_go_calls.h
    src/codegen_go_rz.h
    src/codegen_go_bytes.h
    src/codegen_go_export.h
    src/codegen_go_format.h
    src/codegen_go_threads.h
    src/codegen_go_emit.h
)

# Put our src/ before vendor/goose/src so our modified headers (build.h,
# process.h, codegen_stacks.h) and codegen_go*.h take precedence over upstream.
target_include_directories(goose PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${GOOSE_UPSTREAM}/src"
    "${GOOSE_UPSTREAM}/third_party/subprocess"
)

# The Go runtime (runtime/go), embedded in the compiler as it builds
# (cmake/go_runtime.cmake, src/codegen_go_runtime.h): the files a
# --target=go file takes its runtime from, in the order it writes them.
# goose_package.go and goose_extra.go are the package's alone.
set(GOOSE_GO_RUNTIME_FILES
    goose_runtime.go goose_bytes.go goose_format.go goose_spans.go goose_os.go goose_threads.go)
set(go_runtime_h "${CMAKE_CURRENT_BINARY_DIR}/go_runtime/goose_go_runtime.h")
list(TRANSFORM GOOSE_GO_RUNTIME_FILES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/runtime/go/"
     OUTPUT_VARIABLE go_runtime_srcs)
list(JOIN GOOSE_GO_RUNTIME_FILES "," go_runtime_names)
add_custom_command(OUTPUT "${go_runtime_h}"
    COMMAND "${CMAKE_COMMAND}" "-DDIR=${CMAKE_CURRENT_SOURCE_DIR}/runtime/go" "-DFILES=${go_runtime_names}"
            "-DOUT=${go_runtime_h}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/go_runtime.cmake"
    DEPENDS ${go_runtime_srcs} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/go_runtime.cmake"
    COMMENT "Embedding the Go runtime (runtime/go)"
    VERBATIM)
target_sources(goose PRIVATE "${go_runtime_h}")
target_include_directories(goose PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/go_runtime")

if(MSVC)
    target_sources(goose PRIVATE "${GOOSE_UPSTREAM}/src/goose.manifest")
elseif(WIN32)
    enable_language(RC)
    target_sources(goose PRIVATE "${GOOSE_UPSTREAM}/src/goose.rc")
    set_property(SOURCE "${GOOSE_UPSTREAM}/src/goose.rc" APPEND PROPERTY OBJECT_DEPENDS
                 "${GOOSE_UPSTREAM}/src/goose.manifest")
endif()

set(GOOSE_VERSION "" CACHE STRING "The compiler's version; empty for git describe")
set(goose_version_h "${CMAKE_CURRENT_BINARY_DIR}/version/goose_version.h")
add_custom_target(goose_version
    COMMAND "${CMAKE_COMMAND}" "-DSRC=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUT=${goose_version_h}"
            "-DVERSION=${GOOSE_VERSION}" -P "${GOOSE_UPSTREAM}/cmake/version.cmake"
    BYPRODUCTS "${goose_version_h}")
add_dependencies(goose goose_version)
target_include_directories(goose PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/version")

find_package(Threads REQUIRED)
target_link_libraries(goose PRIVATE Threads::Threads)

set(goose_main_stack 67108864)
if(MSVC)
    target_link_options(goose PRIVATE "/STACK:${goose_main_stack}")
elseif(MINGW)
    target_link_options(goose PRIVATE "LINKER:--stack,${goose_main_stack}")
elseif(APPLE)
    math(EXPR goose_main_stack_hex "${goose_main_stack}" OUTPUT_FORMAT HEXADECIMAL)
    target_link_options(goose PRIVATE "LINKER:-stack_size,${goose_main_stack_hex}")
endif()

add_library(goose_shaderc STATIC
    "${GOOSE_UPSTREAM}/src/shaderc.c"
    "${GOOSE_UPSTREAM}/src/shaderc.h"
    "${GOOSE_UPSTREAM}/src/gfx_blob.h"
)
target_include_directories(goose_shaderc PRIVATE "${GOOSE_UPSTREAM}/third_party/cute_spirv")
if(MSVC)
    target_compile_options(goose_shaderc PRIVATE /W0 /std:clatest /experimental:c11atomics)
else()
    set_target_properties(goose_shaderc PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
    target_compile_options(goose_shaderc PRIVATE -w)
endif()
target_link_libraries(goose PRIVATE goose_shaderc)

# Upstream cmake/libtcc.cmake and cmake/package.cmake expect CMAKE_CURRENT_SOURCE_DIR
# and CMAKE_SOURCE_DIR to point at the upstream root.
set(_saved_cur_src "${CMAKE_CURRENT_SOURCE_DIR}")
set(_saved_src "${CMAKE_SOURCE_DIR}")
set(CMAKE_CURRENT_SOURCE_DIR "${GOOSE_UPSTREAM}")
set(CMAKE_SOURCE_DIR "${GOOSE_UPSTREAM}")
include("${GOOSE_UPSTREAM}/cmake/libtcc.cmake")
set(GOOSE_BUILTIN_LIBS "" CACHE STRING
    "Blessed packages to compile into the compiler: all, or a list of their names")
include("${GOOSE_UPSTREAM}/cmake/package.cmake")
goose_builtin_packages("${GOOSE_BUILTIN_LIBS}")
set(CMAKE_CURRENT_SOURCE_DIR "${_saved_cur_src}")
set(CMAKE_SOURCE_DIR "${_saved_src}")

target_link_libraries(goose PRIVATE libtcc)
add_dependencies(goose tcc_runtime)
set(GOOSE_TCC_LIB_PATH "${TCC_STAGE}" CACHE PATH "TinyCC's runtime directory")
target_compile_definitions(goose PRIVATE GOOSE_TCC_LIB_PATH="${GOOSE_TCC_LIB_PATH}")

if(MSVC AND TARGET goose_gfx)
    foreach(dll IN ITEMS user32 gdi32 winmm imm32 ole32 oleaut32 version setupapi shell32)
        target_link_options(goose PRIVATE "/DELAYLOAD:${dll}.dll")
    endforeach()
    target_link_libraries(goose PRIVATE delayimp)
endif()

if(MSVC)
    target_compile_options(goose PRIVATE /W4 /permissive- /utf-8 /EHsc /bigobj)
    target_compile_definitions(goose PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(goose PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
endif()
