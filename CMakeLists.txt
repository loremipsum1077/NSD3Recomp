cmake_minimum_required(VERSION 3.24)
project(NSD3Recomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/Decomp.cmake)
# Targets d3_game and d3_middleware: the decomp's sources. Not part of the default
# build until they all compile (milestone 1): build them with
#   cmake --build build/macos --target d3_game d3_middleware -- -k 0
include(cmake/DecompSources.cmake)

# Where the game's data is read from by default. Never inside version control.
set(D3_GAME_DIR "${PROJECT_SOURCE_DIR}/game" CACHE PATH "Folder holding the user's own game dump")

# The host program. Today it only locates the game folder; the decomp's sources
# and the platform layer are added to it milestone by milestone (docs/ROADMAP.md).
add_executable(d3 src/host/main.cpp)
target_compile_definitions(d3 PRIVATE D3_DEFAULT_GAME_DIR="${D3_GAME_DIR}")
