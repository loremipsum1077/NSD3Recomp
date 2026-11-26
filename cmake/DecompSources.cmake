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
# ---- mods: deliberate changes to the game, off by default ----
# The decomp itself is never edited. A mod compiles a patched copy of one source
# file, written to the build directory by tools/patch_source.py, in place of the
# original (same position in the link order).
function(d3_patch_source relative)
    set(_original "${D3_DECOMP_ROOT}/src/${relative}")
    set(_patched "${CMAKE_BINARY_DIR}/generated/mods/${relative}")
    list(FIND D3_DECOMP_SOURCES "${_original}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "mod: ${relative} is not among the decomp's sources")
    endif()
    set(_replacements)
    foreach(_text IN LISTS ARGN)
        list(APPEND _replacements "${_text}")
    endforeach()
    add_custom_command(OUTPUT "${_patched}"
        COMMAND ${Python3_EXECUTABLE} "${PROJECT_SOURCE_DIR}/tools/patch_source.py" "${_original}" "${_patched}" ${_replacements}
        DEPENDS "${_original}" "${PROJECT_SOURCE_DIR}/tools/patch_source.py"
        COMMENT "Mod: patching a copy of ${relative}" VERBATIM)
    list(REMOVE_AT D3_DECOMP_SOURCES ${_at})
    list(INSERT D3_DECOMP_SOURCES ${_at} "${_patched}")
    set(D3_DECOMP_SOURCES "${D3_DECOMP_SOURCES}" PARENT_SCOPE)
    # The copy still finds the headers that the original includes by relative path.
    get_filename_component(_dir "${_original}" DIRECTORY)
    set_property(GLOBAL APPEND PROPERTY D3_PATCHED_SOURCES "${_patched}|${_dir}")
endfunction()

# D3_MOD_RUN_SPEED=2 makes heroes walk, run and sprint at twice the game's speed.
set(D3_MOD_RUN_SPEED "" CACHE STRING "Mod: multiply the heroes' movement speed by this number (empty: unchanged)")
if(D3_MOD_RUN_SPEED)
    message(STATUS "Mod: hero movement speed x${D3_MOD_RUN_SPEED}")
    d3_patch_source("X/Server/SHero.cpp"
        --replace "pHero->flAttributesWalkSpeed);" "pHero->flAttributesWalkSpeed * ${D3_MOD_RUN_SPEED}f);"
        --replace "pHero->flAttributesRunSpeed);" "pHero->flAttributesRunSpeed * ${D3_MOD_RUN_SPEED}f);"
        --replace "pHero->flAttributesSprintSpeed);" "pHero->flAttributesSprintSpeed * ${D3_MOD_RUN_SPEED}f);")
endif()

# D3_MOD_RENDER_SIZE=2560x1440 draws the game at that size instead of the Switch's
# docked 1600x900. The window shows whatever size is drawn, scaled to fit.
set(D3_MOD_RENDER_SIZE "" CACHE STRING "Mod: the size the game draws at, as WIDTHxHEIGHT (empty: 1600x900)")
if(D3_MOD_RENDER_SIZE)
    if(NOT D3_MOD_RENDER_SIZE MATCHES "^([0-9]+)x([0-9]+)$")
        message(FATAL_ERROR "D3_MOD_RENDER_SIZE must look like 2560x1440")
    endif()
    message(STATUS "Mod: render size ${CMAKE_MATCH_1} by ${CMAKE_MATCH_2}")
    d3_patch_source("X/Client/Gfx/NX64/NX64NVNCore.cpp"
        --replace "globals.dwDeviceWidth = 1600;" "globals.dwDeviceWidth = ${CMAKE_MATCH_1};"
        --replace "globals.dwDeviceHeight = 900;" "globals.dwDeviceHeight = ${CMAKE_MATCH_2};")
    # Text is drawn from ready-made font sets, one per display height. The Switch's
    # data holds the 900-line set (and a 720-line one) but none for 1080 or 2160
    # lines, so above 1080 lines no text would appear: keep the 900-line set.
    d3_patch_source("X/Client/Definitions/FontDefinition.cpp"
        --replace "dwResolutionFlags = 1 << FCR_2160P_BIT;" "dwResolutionFlags = 1 << FCR_900P_BIT;"
        --replace "dwResolutionFlags = 1 << FCR_1080P_BIT;" "dwResolutionFlags = 1 << FCR_900P_BIT;")
    # The game gives its screen-sized buffers a fixed budget, sized for 1600x900. It
    # is raised in proportion to the number of pixels (never lowered, and kept under
    # 2 GB, which is as far as the heap's 32-bit sizes are known to be safe).
    math(EXPR _budget "390070272 * ${CMAKE_MATCH_1} * ${CMAKE_MATCH_2} / 1440000")
    if(_budget LESS 390070272)
        set(_budget 390070272)
    elseif(_budget GREATER 2013265920)
        set(_budget 2013265920)
    endif()
    math(EXPR _budget "${_budget} / 1048576 * 1048576")
    d3_patch_source("X/Sigma/Memory/SigmaMemory.cpp"
        --replace "GRAPHICS_PERSISTENT_MEMORY_SIZE = 0x17400000;" "GRAPHICS_PERSISTENT_MEMORY_SIZE = ${_budget};")
endif()

# D3_MOD_CAMERA_DISTANCE=1.5 pulls the in-game camera back to that multiple of its
# normal distance from the hero, along the same line of sight. Only the view is
# moved: the camera position the game keeps for its own follow logic is unchanged.
set(D3_MOD_CAMERA_DISTANCE "" CACHE STRING "Mod: multiply the camera's distance from the hero by this number (empty: unchanged)")
if(D3_MOD_CAMERA_DISTANCE)
    message(STATUS "Mod: camera distance x${D3_MOD_CAMERA_DISTANCE}")
    set(_k "${D3_MOD_CAMERA_DISTANCE}f")
    # $<SEMICOLON> because a plain semicolon would split the text into separate arguments.
    d3_patch_source("X/Client/Camera/ConsoleViewController.cpp"
        --replace "csThis.transform.Set(csThis.wvLookFrom, csThis.wvLookAt, cgwvZAxis);"
                  "Vector3D wvModFrom = csThis.wvLookFrom$<SEMICOLON> wvModFrom.x = csThis.wvLookAt.x + (csThis.wvLookFrom.x - csThis.wvLookAt.x) * ${_k}$<SEMICOLON> wvModFrom.y = csThis.wvLookAt.y + (csThis.wvLookFrom.y - csThis.wvLookAt.y) * ${_k}$<SEMICOLON> wvModFrom.z = csThis.wvLookAt.z + (csThis.wvLookFrom.z - csThis.wvLookAt.z) * ${_k}$<SEMICOLON> csThis.transform.Set(wvModFrom, csThis.wvLookAt, cgwvZAxis)$<SEMICOLON>")
endif()

# D3_MOD_SEASON=1 makes a season active without the online service. Normally the
# season's number and state arrive from the servers; here they start out as "season
# <number>, active", the game is told the season data has arrived, and the check made
# with the server when a seasonal hero is created answers yes.
# Seasonal content repeats every 12 seasons in this version's data (the season cycle
# length in the achievements file), so 1 to 12 cover everything.
set(D3_MOD_SEASON "" CACHE STRING "Mod: make this season number active offline (empty: no season)")
if(D3_MOD_SEASON)
    if(NOT D3_MOD_SEASON MATCHES "^[0-9]+$")
        message(FATAL_ERROR "D3_MOD_SEASON must be a season number")
    endif()
    message(STATUS "Mod: season ${D3_MOD_SEASON} active offline")
    d3_patch_source("X/Common/OnlineService/OnlineService.cpp"
        --replace "DEFAULT_SEASON_NUM = 1$<SEMICOLON>" "DEFAULT_SEASON_NUM = ${D3_MOD_SEASON}$<SEMICOLON>"
        --replace "DEFAULT_SEASON_STATE = 0$<SEMICOLON>" "DEFAULT_SEASON_STATE = 1$<SEMICOLON>")
    d3_patch_source("X/Client/Console/LobbyService.cpp"
        --replace "blz::atomic_bool sbSeasonsInitialized(false)$<SEMICOLON>" "blz::atomic_bool sbSeasonsInitialized(true)$<SEMICOLON>"
        --replace "::execute>(pfnCallback, FALSE))$<SEMICOLON>" "::execute>(pfnCallback, TRUE))$<SEMICOLON>")
endif()

# D3_MOD_CHEATS=1 adds testing cheats: F1 raises the hero to the level cap, F2 adds 100
# paragon levels, and the Mods page can do either or give any item while the game runs
# (src/mods/cheats.cpp). The server's update calls it once per update, right after the
# game's own Lua scripts, so it runs on the server's thread like a quest script.
set(D3_MOD_CHEATS "" CACHE STRING "Mod: testing cheats (level, paragon, items); 1 to turn on, empty for off")
if(D3_MOD_CHEATS)
    message(STATUS "Mod: cheats (F1 level cap, F2 +100 paragon, Mods page for items)")
    # The call's own block declares it, so the patch adds no line before the original's.
    d3_patch_source("X/Server/SGame.cpp"
        --replace "        LuaScriptUpdate()$<SEMICOLON>"
                  "        LuaScriptUpdate()$<SEMICOLON> { void D3ModCheatsUpdate()$<SEMICOLON> D3ModCheatsUpdate()$<SEMICOLON> }")
    list(APPEND D3_DECOMP_SOURCES "${PROJECT_SOURCE_DIR}/src/mods/cheats.cpp")
endif()

# D3_MOD_MOUSE=1 lets the mouse play alongside the controller (src/mods/mouse.cpp): it
# selects and presses menu entries and dialog buttons, and walks the hero toward the
# pointer. Each input update first hands the window's mouse events to the mod.
set(D3_MOD_MOUSE "" CACHE STRING "Mod: mouse input alongside the controller; 1 to turn on, empty for off")
if(D3_MOD_MOUSE)
    message(STATUS "Mod: mouse")
    d3_patch_source("X/Client/Input/InputNX64.cpp"
        --replace "    if ( sCSAIsActive() )"
                  "    { void D3ModMouseUpdate()$<SEMICOLON> D3ModMouseUpdate()$<SEMICOLON> } if ( sCSAIsActive() )")
    list(APPEND D3_DECOMP_SOURCES "${PROJECT_SOURCE_DIR}/src/mods/mouse.cpp")
endif()

# Mods that use the shared hooks (src/mods/hooks.cpp) add their name to this list.
set(D3_SHARED_HOOKS)

# D3_MOD_VENDOR=1 puts Deckard Cain back in New Tristram as a vendor of custom legendary
# gems, each with an extra power that fires every so many kills (src/mods/vendor.cpp).
# Heroes holding those gems need the mod on to load.
set(D3_MOD_VENDOR "" CACHE STRING "Mod: Deckard Cain sells custom gems in New Tristram; 1 to turn on, empty for off")
if(D3_MOD_VENDOR)
    message(STATUS "Mod: Cain's custom gems")
    list(APPEND D3_SHARED_HOOKS VENDOR)  # update, item table, strings (see "shared hooks")
    d3_patch_source("X/Common/NPC.cpp"
        --replace "    return ActorDefinitionGetTagMapValueInt(TAG_ACTOR_NPC_TRADE, tACD.snoActor)$<SEMICOLON>"
                  "    { BOOL D3ModVendorIsVendor(const ActorCommonData &)$<SEMICOLON> if (D3ModVendorIsVendor(tACD)) return TRUE$<SEMICOLON> } return ActorDefinitionGetTagMapValueInt(TAG_ACTOR_NPC_TRADE, tACD.snoActor)$<SEMICOLON>")
    d3_patch_source("X/Server/LuaScript/ScriptEvent.cpp"
        --replace "    ScriptEventDispatchEvent(EVENT_ACTOR_KILLED, &tEventDefParams, NULL, NULL, 0.0f, ACDID_INVALID)$<SEMICOLON>"
                  "    { void D3ModVendorOnKill(ACDID, ACDID)$<SEMICOLON> D3ModVendorOnKill(idACDActor, ACDGetControllingActor(idACDKiller))$<SEMICOLON> } ScriptEventDispatchEvent(EVENT_ACTOR_KILLED, &tEventDefParams, NULL, NULL, 0.0f, ACDID_INVALID)$<SEMICOLON>")
    d3_patch_source("X/Common/ActorInventory.cpp"
        --replace "    if (bNPC && bMerchant)"
                  "    BOOL D3ModVendorIsMerchantActor(const ActorCommonData &)$<SEMICOLON> if ((bNPC && bMerchant) || D3ModVendorIsMerchantActor(tACD))")
    list(APPEND D3_DECOMP_SOURCES "${PROJECT_SOURCE_DIR}/src/mods/vendor.cpp")
    # 2.7.7's jewel upgrade takes the item's owner as well; 2.6.2's does not.
    file(READ "${_src}/X/Server/SItems.cpp" _sitems)
    string(FIND "${_sitems}" "SItemUpgradeJewel(ActorCommonData & tACDItem, int32 nUpgradeRanks, ActorCommonData & tACDOwner)" _owner)
    if(_owner GREATER -1)
        set_source_files_properties("${PROJECT_SOURCE_DIR}/src/mods/vendor.cpp" PROPERTIES COMPILE_DEFINITIONS D3_VENDOR_UPGRADE_HAS_OWNER=1)
    endif()
endif()

# D3_MOD_CORRUPT=1 adds a "Corrupt" recipe to Kanai's Cube (src/mods/corrupt.cpp): a
# legendary item comes out carrying a random boss's skill, which it casts now and then
# when the hero hits something. The corruption is saved as an affix, so heroes holding
# corrupted items need the mod on to load.
set(D3_MOD_CORRUPT "" CACHE STRING "Mod: Kanai's Cube corrupts items with boss skills; 1 to turn on, empty for off")
if(D3_MOD_CORRUPT)
    message(STATUS "Mod: Kanai's Cube corruption")
    list(APPEND D3_SHARED_HOOKS CORRUPT)  # update, affix and recipe tables, strings
    # The new recipe's action (40; the game's run to 18) is handled before the switch.
    d3_patch_source("X/Server/Crafting/SItemCrafting.cpp"
        --replace "    eFailureError = GAMEERROR_NONE$<SEMICOLON>"
                  "    eFailureError = GAMEERROR_NONE$<SEMICOLON> if (eAction == 40) { BOOL D3ModCorruptItem(Player &, const ACDID *, int32, ACDID &, ACDID &, GameError &)$<SEMICOLON> ACDID idKeep = ACDID_INVALID, idNew = ACDID_INVALID$<SEMICOLON> if (!D3ModCorruptItem(tPlayer, aridACDItems.data(), nItemsCount, idKeep, idNew, eFailureError)) return FALSE$<SEMICOLON> tResultsMessage.annItem = ANNFromACD(idNew)$<SEMICOLON> sTransmuteConsumeItems(tPlayer, tTransmuteInventoryFinal, idKeep)$<SEMICOLON> return TRUE$<SEMICOLON> }")
    # Every hit that can proc (the same place the game rolls its own on-hit effects).
    d3_patch_source("X/Server/Powers/PowerAPI.cpp"
        --replace "                sPowerAPI_CheckForOnHitProcs(*ptPower, *ptACDMainActor, tACDTarget, tPowerDamage.eKnockBack, tPayload.flProcScalar, flDamageDone, flHitPointDamage, &tPayload, NULL, tPowerDamage.bCriticalHit)$<SEMICOLON>"
                  "                sPowerAPI_CheckForOnHitProcs(*ptPower, *ptACDMainActor, tACDTarget, tPowerDamage.eKnockBack, tPayload.flProcScalar, flDamageDone, flHitPointDamage, &tPayload, NULL, tPowerDamage.bCriticalHit)$<SEMICOLON> { void D3ModCorruptOnHit(ActorCommonData &, ActorCommonData &, float, SNO)$<SEMICOLON> D3ModCorruptOnHit(*ptACDMainActor, tACDTarget, tPayload.flProcScalar, ptPower->snoPower)$<SEMICOLON> }")
    # Item tooltips (console): a line for the corruption, after the item's properties.
    # (Patch text is one line: CMake drops newlines in a custom command's arguments.)
    d3_patch_source("X/Client/UserInterface/Console/UIConsoleInventoryViewUtilities.cpp"
        --replace "        sTextSections[nCurrentSection] += sAffixes$<SEMICOLON>"
                  "        sTextSections[nCurrentSection] += sAffixes$<SEMICOLON> { void D3ModCorruptTooltip(const ActorCommonData &, CRefString &)$<SEMICOLON> D3ModCorruptTooltip(*ptACDItem, sTextSections[nCurrentSection])$<SEMICOLON> }")
    list(APPEND D3_DECOMP_SOURCES "${PROJECT_SOURCE_DIR}/src/mods/corrupt.cpp")
endif()

# Shared hooks: decomp files that more than one mod needs are patched once here, to
# call src/mods/hooks.cpp, which calls each mod in D3_SHARED_HOOKS.
if(D3_SHARED_HOOKS)
    d3_patch_source("X/Server/SPlayer.cpp"
        --replace "    ServerUpdateWasCalled(ptServerPlayerGlobals->tPlayerUpdateData)$<SEMICOLON>"
                  "    ServerUpdateWasCalled(ptServerPlayerGlobals->tPlayerUpdateData)$<SEMICOLON> { void D3ModServerUpdate()$<SEMICOLON> D3ModServerUpdate()$<SEMICOLON> }")
    d3_patch_source("X/Common/Definitions/GameBalanceDefinition.cpp"
        --replace "        GBHeader * ptHeader = (GBHeader *)((uint8 *)pTable + i * nSize)$<SEMICOLON>"
                  "        if (i == 0) { void D3ModExtendGameBalance(GameBalanceDefinition *, int32 &, void *&)$<SEMICOLON> D3ModExtendGameBalance(pdefGB, nCount, pTable)$<SEMICOLON> } GBHeader * ptHeader = (GBHeader *)((uint8 *)pTable + i * nSize)$<SEMICOLON>")
    d3_patch_source("X/Common/Definitions/StringListDefinition.cpp"
        --replace "static LPCSTR sStringListFetchInternal(const SNO snoStringList, const CHAR * szLabel, BOOL & fFound) {"
                  "LPCSTR D3ModString(SNO, LPCSTR)$<SEMICOLON> static LPCSTR sStringListFetchInternal(const SNO snoStringList, const CHAR * szLabel, BOOL & fFound) { if (LPCSTR szMod = D3ModString(snoStringList, szLabel)) { fFound = TRUE$<SEMICOLON> return szMod$<SEMICOLON> }")
    set(_hook_defs)
    foreach(_mod IN LISTS D3_SHARED_HOOKS)
        list(APPEND _hook_defs "D3_HOOK_${_mod}=1")
    endforeach()
    list(APPEND D3_DECOMP_SOURCES "${PROJECT_SOURCE_DIR}/src/mods/hooks.cpp")
    set_source_files_properties("${PROJECT_SOURCE_DIR}/src/mods/hooks.cpp" PROPERTIES COMPILE_DEFINITIONS "${_hook_defs}")
endif()

add_library(d3_decomp OBJECT EXCLUDE_FROM_ALL ${D3_DECOMP_SOURCES})
get_property(_patched_sources GLOBAL PROPERTY D3_PATCHED_SOURCES)
foreach(_entry IN LISTS _patched_sources)
    string(REPLACE "|" ";" _pair "${_entry}")
    list(GET _pair 0 _file)
    list(GET _pair 1 _dir)
    set_source_files_properties("${_file}" TARGET_DIRECTORY d3_decomp PROPERTIES INCLUDE_DIRECTORIES "${_dir}")
endforeach()
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
