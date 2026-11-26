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
# This branch builds game version 2.7.7 (the decomp's dev branch); the 2.6.2 branches of
# both repositories build 2.6.2. 2.7.7 ships no debug file, so symbol names and addresses
# come from the decomp's data/2.7.7 folder and data sizes from the 2.6.2 debug file.
# Where the game's data is read from by default. Never inside version control.
set(D3_GAME_DIR "${PROJECT_SOURCE_DIR}/game/2.7.7" CACHE PATH "Folder holding your own dump of the game, updated to 2.7.7")
if(NOT EXISTS "${D3_DECOMP_ROOT}/data/2.7.7/functions.csv")
    message(FATAL_ERROR "The decomp at ${D3_DECOMP_ROOT} has no data/2.7.7: this branch builds 2.7.7, "
        "so check out the decomp's dev branch there (for 2.6.2, use the 2.6.2 branch of both repositories).")
endif()
# Target d3_decomp: the decomp's sources (game and middleware).
include(cmake/DecompSources.cmake)

# The host program is defined at the end of this file, once every part exists.

# The platform layer: everything that replaces the Switch underneath the game.
file(GLOB_RECURSE D3_PLATFORM_SOURCES CONFIGURE_DEPENDS
    src/nn/*.c src/nn/*.cpp src/gfx/*.c src/gfx/*.cpp
    src/libc/*.c src/replace/*.c src/host/unimplemented.c src/host/crash.c src/host/window.cpp src/host/audio.cpp
    src/gfx/*.mm)
# Default entry points of the do-nothing graphics driver, from the decomp's NVN header.
set(D3_NVN_NULL "${CMAKE_BINARY_DIR}/generated/nvn_null_defaults.c")
# NSDiablo3Decomp's bootstrap writes the entry-point names to this file instead of the header.
set(_nvn_entries "${D3_DECOMP_ROOT}/src/X/Common/generated-game-data/nvn_entry_points.inc")
if(NOT EXISTS "${_nvn_entries}")
    set(_nvn_entries "")
endif()
add_custom_command(OUTPUT "${D3_NVN_NULL}"
    COMMAND ${Python3_EXECUTABLE} "${PROJECT_SOURCE_DIR}/tools/gen_nvn_null.py" "${D3_DECOMP_ROOT}" "${D3_NVN_NULL}"
    DEPENDS tools/gen_nvn_null.py "${D3_DECOMP_ROOT}/src/NintendoSDK/Include/nvn/nvn_CppFuncPtrImpl.h"
            "${D3_DECOMP_ROOT}/src/NintendoSDK/Include/nvn/nvn_Cpp.h" ${_nvn_entries}
    COMMENT "Generating the do-nothing graphics driver's entry points")
add_library(d3_platform OBJECT ${D3_PLATFORM_SOURCES} "${D3_NVN_NULL}")
# Window and input. SDL2 must be installed (on macOS: Homebrew's sdl2).
find_package(SDL2 REQUIRED)
target_include_directories(d3_platform PRIVATE src)
target_link_libraries(d3_platform PRIVATE SDL2::SDL2)
set_source_files_properties(src/gfx/metal/nvn_metal.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
# Where saves go until milestone 7 settles it. Ignored by git.
set(D3_SAVE_DIR "${PROJECT_SOURCE_DIR}/save/2.7.7" CACHE PATH "Folder for save data")
# Translated shaders, made from the player's own game data. Ignored by git.
set(D3_SHADER_CACHE "${PROJECT_SOURCE_DIR}/cache/shaders-2.7.7" CACHE PATH "Folder for translated shaders")
target_compile_definitions(d3_platform PRIVATE D3_DEFAULT_GAME_DIR="${D3_GAME_DIR}" D3_DEFAULT_SAVE_DIR="${D3_SAVE_DIR}"
    D3_DEFAULT_SHADER_CACHE="${D3_SHADER_CACHE}")
set_target_properties(d3_platform PROPERTIES CXX_STANDARD 17)

# Data that exists in the original program but that the decomp does not define yet,
# written out as C from the player's own dump. The output is game content and stays
# in the build directory.
set(D3_EXTRACTED "${CMAKE_BINARY_DIR}/generated/extracted_data.c")
# 2.7.7 has no debug file: symbol names and addresses from the decomp's data/2.7.7
# folder, and data sizes from the 2.6.2 debug file (your cartridge dump in game/) where known.
set(D3_SYMBOLS_CSV "${D3_DECOMP_ROOT}/data/2.7.7" CACHE PATH "Decomp data/<version> folder to use instead of the dump's debug file")
set(D3_SIZE_HINTS "${PROJECT_SOURCE_DIR}/game/romfs/DiabloIIINX64r.nss" CACHE FILEPATH "The 2.6.2 DiabloIIINX64r.nss, for data sizes")
set(_extract_args)
if(D3_SYMBOLS_CSV)
    list(APPEND _extract_args --symbols-csv "${D3_SYMBOLS_CSV}")
    if(D3_SIZE_HINTS)
        list(APPEND _extract_args --size-hints "${D3_SIZE_HINTS}")
    endif()
endif()
if(EXISTS "${D3_GAME_DIR}/exefs/main" AND (D3_SYMBOLS_CSV OR EXISTS "${D3_GAME_DIR}/romfs/DiabloIIINX64r.nss"))
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_command(OUTPUT "${D3_EXTRACTED}"
        COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/extract_data.py"
                --game "${D3_GAME_DIR}" --out "${D3_EXTRACTED}" ${_extract_args}
                --skip-defined-in "${CMAKE_BINARY_DIR}/decomp_objects.txt"
        DEPENDS tools/extract_data.py config/extract_symbols.txt "${D3_GAME_DIR}/exefs/main"
                $<TARGET_OBJECTS:d3_decomp>
        COMMENT "Extracting data definitions from the game dump")
    # The objects of this build exactly: the object directory can also hold leftovers
    # from source files the decomp has since removed.
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/decomp_objects.txt" CONTENT "$<JOIN:$<TARGET_OBJECTS:d3_decomp>,\n>\n")
    # The Vorbis setup packets FMOD keeps in the executable, which the game's sample
    # banks need to be decoded (src/audio/fmod_host.cpp).
    set(D3_VORBIS_SETUPS "${CMAKE_BINARY_DIR}/generated/vorbis_setups.c")
    add_custom_command(OUTPUT "${D3_VORBIS_SETUPS}"
        COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/extract_vorbis_setups.py"
                "${D3_GAME_DIR}/exefs/main" "${D3_VORBIS_SETUPS}"
        DEPENDS tools/extract_vorbis_setups.py tools/extract_data.py "${D3_GAME_DIR}/exefs/main"
        COMMENT "Extracting Vorbis setup packets from the game dump")
    add_library(d3_extracted OBJECT "${D3_EXTRACTED}" "${D3_VORBIS_SETUPS}")
    target_compile_options(d3_extracted PRIVATE -w)
else()
    message(STATUS "No game dump in ${D3_GAME_DIR}: extracted data definitions are not built")
endif()

# Gap-fillers written against the decomp's own headers (src/replace/README.md).
file(GLOB D3_REPLACE_SOURCES CONFIGURE_DEPENDS src/replace/*.cc src/replace/*.cpp)
add_library(d3_replace OBJECT ${D3_REPLACE_SOURCES})
target_link_libraries(d3_replace PRIVATE d3_decomp_flags)
set_source_files_properties(src/replace/protobuf_field_skipper.cc PROPERTIES COMPILE_OPTIONS -funsigned-char)   # protobuf's setting

# Sound: the FMOD functions (src/audio/fmod_host.cpp) with a private copy of the
# decomp's Ogg Vorbis decoder, whose memory comes from the host and not from the
# game's heap (src/audio/ogg_alloc.c). The pieces are joined into one object in
# which everything but the FMOD functions is local, so the copy does not collide
# with the one the game itself links for its videos.
set(_d3_ogg "${D3_DECOMP_ROOT}/src/Contrib/Contrib/OggVorbis")
file(GLOB D3_SOUND_VORBIS CONFIGURE_DEPENDS "${_d3_ogg}/libogg/src/*.c" "${_d3_ogg}/libvorbis/lib/*.c")
add_library(d3_sound OBJECT src/audio/fmod_host.cpp src/audio/ogg_alloc.c ${D3_SOUND_VORBIS})
target_link_libraries(d3_sound PRIVATE d3_decomp_flags)
target_compile_options(d3_sound PRIVATE -fvisibility=hidden -w)
set_source_files_properties(${D3_SOUND_VORBIS} PROPERTIES COMPILE_OPTIONS "-std=gnu89;-funsigned-char;-ffast-math")   # as in cmake/DecompSources.cmake
target_include_directories(d3_sound PRIVATE "${_d3_ogg}/libogg/include" "${_d3_ogg}/libvorbis/include" "${_d3_ogg}/libvorbis/lib")
set(D3_SOUND_OBJECT "${CMAKE_BINARY_DIR}/generated/sound.o")
add_custom_command(OUTPUT "${D3_SOUND_OBJECT}"
    COMMAND "${CMAKE_C_COMPILER}" -r -nostdlib -o "${D3_SOUND_OBJECT}" $<TARGET_OBJECTS:d3_sound>
    DEPENDS d3_sound $<TARGET_OBJECTS:d3_sound>
    COMMAND_EXPAND_LISTS
    COMMENT "Joining the sound code and its Vorbis decoder")
set_source_files_properties("${D3_SOUND_OBJECT}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

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
        $<TARGET_OBJECTS:d3_replace> $<TARGET_OBJECTS:d3_extracted> "${D3_SOUND_OBJECT}")
    target_compile_definitions(d3 PRIVATE D3_DEFAULT_GAME_DIR="${D3_GAME_DIR}" D3_WITH_GAME)
    # Link the private C++ library and export nothing but main, so that no C++ symbol
    # of this program is shared with the system's libraries (see D3_LIBCXX_ROOT above).
    target_link_options(d3 PRIVATE -nostdlib++ "-Wl,-exported_symbol,_main")
    target_link_libraries(d3 PRIVATE "${D3_LIBCXX_ROOT}/lib/c++/libc++.a" "${D3_LIBCXX_ROOT}/lib/c++/libc++abi.a")
    target_link_libraries(d3 PRIVATE SDL2::SDL2 "-framework Metal" "-framework QuartzCore" "-framework Foundation"
        "-framework ImageIO" "-framework CoreGraphics")
    set_target_properties(d3_decomp PROPERTIES EXCLUDE_FROM_ALL OFF)
endif()
