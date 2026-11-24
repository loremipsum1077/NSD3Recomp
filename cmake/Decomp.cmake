# Locates the decompiled game source (the NSDiablo3Decomp repository).
# Order: -DD3_DECOMP_DIR=..., $D3_DECOMP_DIR, extern/NSDiablo3Decomp, ../NSDiablo3Decomp.
set(D3_DECOMP_DIR "" CACHE PATH "Path to an NSDiablo3Decomp checkout")
set(_candidates "${D3_DECOMP_DIR}" "$ENV{D3_DECOMP_DIR}"
    "${PROJECT_SOURCE_DIR}/extern/NSDiablo3Decomp" "${PROJECT_SOURCE_DIR}/../NSDiablo3Decomp")
set(D3_DECOMP_ROOT "")
foreach(c IN LISTS _candidates)
    if(c AND IS_DIRECTORY "${c}/src/X")
        get_filename_component(D3_DECOMP_ROOT "${c}" ABSOLUTE)
        break()
    endif()
endforeach()
if(NOT D3_DECOMP_ROOT)
    message(FATAL_ERROR "NSDiablo3Decomp not found. Pass -DD3_DECOMP_DIR=<path> (see README.md).")
endif()
message(STATUS "Decomp source: ${D3_DECOMP_ROOT}")

# The decomp does not ship its game-specific Protobuf bindings or data tables: its
# bootstrap generates them from the user's own game dump. Stop early if that has not run.
file(GLOB _pb "${D3_DECOMP_ROOT}/src/X/Common/Protobuf/*.pb.cc")
if(NOT _pb OR NOT IS_DIRECTORY "${D3_DECOMP_ROOT}/src/X/Common/generated-game-data")
    message(FATAL_ERROR "The decomp at ${D3_DECOMP_ROOT} has no generated game bindings. Run, in that checkout:\n"
        "  python3 tools/bootstrap_game_data.py <your game dump> --build-protoc\n"
        "(the dump folder holds exefs/main and romfs/DiabloIIINX64r.nss; this repository's game/ works).")
endif()
