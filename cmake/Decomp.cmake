# Locates the decompiled game source (the NSD3Decomp repository).
# Order: -DD3_DECOMP_DIR=..., $D3_DECOMP_DIR, extern/NSD3Decomp, ../NSD3Decomp.
set(D3_DECOMP_DIR "" CACHE PATH "Path to an NSD3Decomp checkout")
set(_candidates "${D3_DECOMP_DIR}" "$ENV{D3_DECOMP_DIR}"
    "${PROJECT_SOURCE_DIR}/extern/NSD3Decomp" "${PROJECT_SOURCE_DIR}/../NSD3Decomp")
set(D3_DECOMP_ROOT "")
foreach(c IN LISTS _candidates)
    if(c AND IS_DIRECTORY "${c}/src/X")
        get_filename_component(D3_DECOMP_ROOT "${c}" ABSOLUTE)
        break()
    endif()
endforeach()
if(NOT D3_DECOMP_ROOT)
    message(FATAL_ERROR "NSD3Decomp not found. Pass -DD3_DECOMP_DIR=<path> (see README.md).")
endif()
message(STATUS "Decomp source: ${D3_DECOMP_ROOT}")
