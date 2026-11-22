# Build targets for the decomp's sources, compiled for the host.
#
# This mirrors the per-library settings of the decomp's own CMakeLists.txt, keeping
# what changes behaviour (whether `char` is signed, language level, feature macros,
# include paths) and dropping what only exists to reproduce the original bytes
# (optimisation level per file, the Switch target, compiler plugins).
#
# The decomp's top-level include/ directory is deliberately not on the include
# path: its stdio.h, errno.h and setjmp.h rebind names to Switch libc symbols.

set(_src "${D3_DECOMP_ROOT}/src")
set(_contrib "${_src}/Contrib/Contrib")

file(GLOB_RECURSE D3_GAME_SOURCES CONFIGURE_DEPENDS "${_src}/X/*.cpp" "${_src}/X/*.cc")
file(GLOB_RECURSE D3_MIDDLEWARE_SOURCES CONFIGURE_DEPENDS "${_src}/*.cpp" "${_src}/*.cc" "${_src}/*.c")
list(FILTER D3_MIDDLEWARE_SOURCES EXCLUDE REGEX "/src/X/")
# Fragments that aes.c and sha256.c #include; not compile units.
list(FILTER D3_MIDDLEWARE_SOURCES EXCLUDE REGEX "/external/libtomcrypt/src/(ciphers/aes/aes_tab|hashes/sha2/sha224)\\.c$")

add_library(d3_decomp_flags INTERFACE)
target_include_directories(d3_decomp_flags INTERFACE "${PROJECT_SOURCE_DIR}/src/compat" "${_src}"
    "${_contrib}/OggVorbis/libogg/include")
# _FORTIFY_SOURCE=0: macOS otherwise turns memcpy and friends into macros, and
# middleware uses those names in declarations.
target_compile_definitions(d3_decomp_flags INTERFACE NDEBUG NON_MATCHING _FORTIFY_SOURCE=0)
# `char` is signed in the game's own code. Libraries that differ are set below.
target_compile_options(d3_decomp_flags INTERFACE -fsigned-char -fno-strict-aliasing
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++11 -fno-exceptions -fno-rtti>
    $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
    -include "${PROJECT_SOURCE_DIR}/src/compat/d3_host_prefix.h"
    -Wno-everything)

# One target for game and middleware, with the sources in the original link order:
# the game's static initialisers depend on it (tools/link_order.py).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_all ${D3_GAME_SOURCES} ${D3_MIDDLEWARE_SOURCES})
file(WRITE "${CMAKE_BINARY_DIR}/decomp_sources.txt" "${_all}")
execute_process(COMMAND ${Python3_EXECUTABLE} "${PROJECT_SOURCE_DIR}/tools/link_order.py" "${D3_DECOMP_ROOT}"
    INPUT_FILE "${CMAKE_BINARY_DIR}/decomp_sources.txt"
    OUTPUT_VARIABLE D3_DECOMP_SOURCES RESULT_VARIABLE _status)
if(NOT _status EQUAL 0)
    message(FATAL_ERROR "tools/link_order.py failed")
endif()
add_library(d3_decomp OBJECT EXCLUDE_FROM_ALL ${D3_DECOMP_SOURCES})
target_link_libraries(d3_decomp PRIVATE d3_decomp_flags)

# Libraries the original built with `char` unsigned (recorded in its debug info).
file(GLOB_RECURSE _unsigned_char CONFIGURE_DEPENDS
    "${_contrib}/BlizzardProtoBuf/*.cc" "${_contrib}/DemonWare/*.cpp" "${_contrib}/lua/src/*.c"
    "${_src}/Externals/curl/*.c")
set_property(SOURCE ${_unsigned_char} TARGET_DIRECTORY d3_decomp APPEND PROPERTY COMPILE_OPTIONS -funsigned-char)

file(GLOB _pcre CONFIGURE_DEPENDS "${_contrib}/PCRE/*.c")
set_source_files_properties(${_pcre} TARGET_DIRECTORY d3_decomp PROPERTIES
    COMPILE_DEFINITIONS HAVE_CONFIG_H
    INCLUDE_DIRECTORIES "${_contrib}/PCRE")

set(_ogg "${_contrib}/OggVorbis")
file(GLOB_RECURSE _ogg_sources CONFIGURE_DEPENDS "${_ogg}/*.c")
set_source_files_properties(${_ogg_sources} TARGET_DIRECTORY d3_decomp PROPERTIES
    COMPILE_OPTIONS "-std=gnu89;-funsigned-char;-ffast-math"
    INCLUDE_DIRECTORIES "${_ogg}/libogg/include;${_ogg}/libvorbis/include;${_ogg}/libvorbis/lib;${_ogg}/libtheora/include")

# LibTomCrypt 1.10 / LibTomMath 0.38, configured as in the decomp.
set(_tom "${_contrib}/DemonWare/external")
file(GLOB_RECURSE _tom_sources CONFIGURE_DEPENDS "${_tom}/libtomcrypt/src/*.c" "${_tom}/libtommath/*.c")
set_source_files_properties(${_tom_sources} TARGET_DIRECTORY d3_decomp PROPERTIES
    COMPILE_DEFINITIONS "LTC_SOURCE;ARGTYPE=3;LTC_NO_TEST;LTM_DESC;LTC_NO_CIPHERS;DES;RIJNDAEL;LTC_NO_HASHES;SHA512;SHA384;SHA256;SHA224;SHA1;MD5;TIGER;LTC_NO_PRNGS;YARROW;FORTUNA;YARROW_AES=3;FORTUNA_POOLS=32;FORTUNA_WD=10;XMALLOC=libTomCryptMalloc;XREALLOC=libTomCryptRealloc;XCALLOC=libTomCryptCalloc;XFREE=libTomCryptFree;XQSORT=libTomCryptQsort"
    INCLUDE_DIRECTORIES "${_tom}/libtomcrypt/upstream-headers;${_tom}/libtommath")

# Sources that embed the Switch libc's 32-byte pthread mutex (see the header).
file(GLOB_RECURSE _nnmusl_mutex CONFIGURE_DEPENDS "${_contrib}/Domino/*.cpp" "${_contrib}/blz/src/linux/mutex_linux.cpp")
set_property(SOURCE ${_nnmusl_mutex} TARGET_DIRECTORY d3_decomp APPEND PROPERTY
    COMPILE_OPTIONS "--include=${PROJECT_SOURCE_DIR}/src/compat/d3_nnmusl_mutex_rename.h")
set_property(SOURCE "${_contrib}/BlizzardProtoBuf/2.6.0/src/src/google/protobuf/stubs/common.cc"
    TARGET_DIRECTORY d3_decomp APPEND PROPERTY
    COMPILE_OPTIONS "--include=${PROJECT_SOURCE_DIR}/src/compat/d3_nnmusl_mutex_protobuf.h")
