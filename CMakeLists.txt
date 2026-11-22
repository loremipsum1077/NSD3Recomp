cmake_minimum_required(VERSION 3.24)
project(NSD3Recomp LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# A private copy of the C++ standard library.
#
# The game defines the global operator new and delete on its own heap, as it does on
# the Switch, where the C++ library is linked into the program. With the system's
# shared C++ library that cannot work: either the system's libraries end up
# allocating from the game's heap, or memory allocated on one side is freed on the
# other. So this program gets its own static C++ library, built against the same
# headers, and shares no C++ symbols with the system. It comes from an LLVM
# installation (on macOS: `brew install llvm`).
set(D3_LIBCXX_ROOT "/opt/homebrew/opt/llvm" CACHE PATH "LLVM installation providing a static libc++")
if(NOT EXISTS "${D3_LIBCXX_ROOT}/lib/c++/libc++.a")
    message(FATAL_ERROR "No static libc++ in ${D3_LIBCXX_ROOT}. Install LLVM (brew install llvm) or set D3_LIBCXX_ROOT.")
endif()
add_compile_options(
    "$<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-nostdinc++>"
    "$<$<COMPILE_LANGUAGE:CXX,OBJCXX>:SHELL:-isystem ${D3_LIBCXX_ROOT}/include/c++/v1>")

include(cmake/Decomp.cmake)
# Target d3_decomp: the decomp's sources (game and middleware).
include(cmake/DecompSources.cmake)

# Where the game's data is read from by default. Never inside version control.
set(D3_GAME_DIR "${PROJECT_SOURCE_DIR}/game" CACHE PATH "Folder holding the user's own game dump")

# The host program is defined at the end of this file, once every part exists.

# The platform layer: everything that replaces the Switch underneath the game.
file(GLOB_RECURSE D3_PLATFORM_SOURCES CONFIGURE_DEPENDS
    src/nn/*.c src/nn/*.cpp src/gfx/*.c src/gfx/*.cpp src/audio/*.c src/audio/*.cpp
    src/libc/*.c src/replace/*.c src/host/unimplemented.c src/host/crash.c src/host/window.cpp
    src/gfx/*.mm)
# Default entry points of the do-nothing graphics driver, from the decomp's NVN header.
set(D3_NVN_NULL "${CMAKE_BINARY_DIR}/generated/nvn_null_defaults.c")
add_custom_command(OUTPUT "${D3_NVN_NULL}"
    COMMAND ${Python3_EXECUTABLE} "${PROJECT_SOURCE_DIR}/tools/gen_nvn_null.py" "${D3_DECOMP_ROOT}" "${D3_NVN_NULL}"
    DEPENDS tools/gen_nvn_null.py "${D3_DECOMP_ROOT}/src/NintendoSDK/Include/nvn/nvn_CppFuncPtrImpl.h"
            "${D3_DECOMP_ROOT}/src/NintendoSDK/Include/nvn/nvn_Cpp.h"
    COMMENT "Generating the do-nothing graphics driver's entry points")
add_library(d3_platform OBJECT ${D3_PLATFORM_SOURCES} "${D3_NVN_NULL}")
# Window and input. SDL2 must be installed (on macOS: Homebrew's sdl2).
find_package(SDL2 REQUIRED)
target_include_directories(d3_platform PRIVATE src)
target_link_libraries(d3_platform PRIVATE SDL2::SDL2)
set_source_files_properties(src/gfx/metal/nvn_metal.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
# Where saves go until milestone 7 settles it. Ignored by git.
set(D3_SAVE_DIR "${PROJECT_SOURCE_DIR}/save" CACHE PATH "Folder for save data")
# Translated shaders, made from the player's own game data. Ignored by git.
set(D3_SHADER_CACHE "${PROJECT_SOURCE_DIR}/cache/shaders" CACHE PATH "Folder for translated shaders")
target_compile_definitions(d3_platform PRIVATE D3_DEFAULT_GAME_DIR="${D3_GAME_DIR}" D3_DEFAULT_SAVE_DIR="${D3_SAVE_DIR}"
    D3_DEFAULT_SHADER_CACHE="${D3_SHADER_CACHE}")
set_target_properties(d3_platform PROPERTIES CXX_STANDARD 17)

# Data that exists in the original program but that the decomp does not define yet,
# written out as C from the player's own dump. The output is game content and stays
# in the build directory.
set(D3_EXTRACTED "${CMAKE_BINARY_DIR}/generated/extracted_data.c")
if(EXISTS "${D3_GAME_DIR}/exefs/main" AND EXISTS "${D3_GAME_DIR}/romfs/DiabloIIINX64r.nss")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_command(OUTPUT "${D3_EXTRACTED}"
        COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/extract_data.py"
                --game "${D3_GAME_DIR}" --out "${D3_EXTRACTED}"
                --skip-defined-in "${CMAKE_BINARY_DIR}/decomp_objects.txt"
        DEPENDS tools/extract_data.py config/extract_symbols.txt "${D3_GAME_DIR}/exefs/main"
                $<TARGET_OBJECTS:d3_decomp>
        COMMENT "Extracting data definitions from the game dump")
    # The objects of this build exactly: the object directory can also hold leftovers
    # from source files the decomp has since removed.
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/decomp_objects.txt" CONTENT "$<JOIN:$<TARGET_OBJECTS:d3_decomp>,\n>\n")
    add_library(d3_extracted OBJECT "${D3_EXTRACTED}")
    target_compile_options(d3_extracted PRIVATE -w)
else()
    message(STATUS "No game dump in ${D3_GAME_DIR}: extracted data definitions are not built")
endif()

# Gap-fillers written against the decomp's own headers (src/replace/README.md).
file(GLOB D3_REPLACE_SOURCES CONFIGURE_DEPENDS src/replace/*.cc src/replace/*.cpp)
add_library(d3_replace OBJECT ${D3_REPLACE_SOURCES})
target_link_libraries(d3_replace PRIVATE d3_decomp_flags)
target_compile_options(d3_replace PRIVATE -funsigned-char)   # protobuf's setting

# The program: host entry point + game + middleware + platform layer + gap-fillers
# + data extracted from the dump. Without a dump it cannot link, so it is only
# defined when one is present; d3_check then still reports what is missing.
add_executable(d3_check src/host/main.cpp)
target_compile_definitions(d3_check PRIVATE D3_DEFAULT_GAME_DIR="${D3_GAME_DIR}")
if(TARGET d3_extracted)
    # The platform layer is linked first so that its start-up code (the crash
    # reporter) runs before the game's static initialisers.
    add_executable(d3 $<TARGET_OBJECTS:d3_platform> src/host/main.cpp
        $<TARGET_OBJECTS:d3_decomp>
        $<TARGET_OBJECTS:d3_replace> $<TARGET_OBJECTS:d3_extracted>)
    target_compile_definitions(d3 PRIVATE D3_DEFAULT_GAME_DIR="${D3_GAME_DIR}" D3_WITH_GAME)
    # Link the private C++ library and export nothing but main, so that no C++ symbol
    # of this program is shared with the system's libraries (see D3_LIBCXX_ROOT above).
    target_link_options(d3 PRIVATE -nostdlib++ "-Wl,-exported_symbol,_main")
    target_link_libraries(d3 PRIVATE "${D3_LIBCXX_ROOT}/lib/c++/libc++.a" "${D3_LIBCXX_ROOT}/lib/c++/libc++abi.a")
    target_link_libraries(d3 PRIVATE SDL2::SDL2 "-framework Metal" "-framework QuartzCore" "-framework Foundation"
        "-framework ImageIO" "-framework CoreGraphics")
    set_target_properties(d3_decomp PROPERTIES EXCLUDE_FROM_ALL OFF)
endif()
