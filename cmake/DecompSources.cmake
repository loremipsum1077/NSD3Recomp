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
# original (same position in the link order). Several mods may patch the same file:
# d3_patch_source collects each file's replacements, applied in the order the mods
# ask for them, and d3_apply_patches makes one patched copy per file.
function(d3_patch_source relative)
    set(_original "${D3_DECOMP_ROOT}/src/${relative}")
    list(FIND D3_DECOMP_SOURCES "${_original}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "mod: ${relative} is not among the decomp's sources")
    endif()
    string(MAKE_C_IDENTIFIER "${relative}" _key)
    get_property(_files GLOBAL PROPERTY D3_PATCH_FILES)
    if(NOT relative IN_LIST _files)
        set_property(GLOBAL APPEND PROPERTY D3_PATCH_FILES "${relative}")
    endif()
    foreach(_text IN LISTS ARGN)
        set_property(GLOBAL APPEND PROPERTY "D3_PATCH_${_key}" "${_text}")
    endforeach()
endfunction()

function(d3_apply_patches)
    get_property(_files GLOBAL PROPERTY D3_PATCH_FILES)
    foreach(relative IN LISTS _files)
        set(_original "${D3_DECOMP_ROOT}/src/${relative}")
        set(_patched "${CMAKE_BINARY_DIR}/generated/mods/${relative}")
        string(MAKE_C_IDENTIFIER "${relative}" _key)
        get_property(_replacements GLOBAL PROPERTY "D3_PATCH_${_key}")
        add_custom_command(OUTPUT "${_patched}"
            COMMAND ${Python3_EXECUTABLE} "${PROJECT_SOURCE_DIR}/tools/patch_source.py" "${_original}" "${_patched}" ${_replacements}
            DEPENDS "${_original}" "${PROJECT_SOURCE_DIR}/tools/patch_source.py"
            COMMENT "Mod: patching a copy of ${relative}" VERBATIM)
        list(FIND D3_DECOMP_SOURCES "${_original}" _at)
        list(REMOVE_AT D3_DECOMP_SOURCES ${_at})
        list(INSERT D3_DECOMP_SOURCES ${_at} "${_patched}")
        # The copy still finds the headers that the original includes by relative path.
        get_filename_component(_dir "${_original}" DIRECTORY)
        set_property(GLOBAL APPEND PROPERTY D3_PATCHED_SOURCES "${_patched}|${_dir}")
    endforeach()
    set(D3_DECOMP_SOURCES "${D3_DECOMP_SOURCES}" PARENT_SCOPE)
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
# length in the achievements file); 14 to 37 also pick that season's theme (D3_MOD_THEME=Season).
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
    # Starting a seasonal game warns that players not signed in online miss the
    # leaderboards; offline there are none, so the check is skipped.
    d3_patch_source("X/Client/UserInterface/Console/UIConsoleMainMenu.cpp"
        --replace "                !sVerifyAllLocalPlayersSignedInOnline(nProfileUserIdx))"
                  "                false)")
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

# Developer starts (src/mods/dev.cpp), which the release still has the code for:
# D3_MOD_LOAD_WORLD=<world name> skips the menus and starts a local game in that world with a
# developer account, as Blizzard's developer builds could; D3_MOD_BLIZZCON=1 starts the
# BlizzCon demo (level-70 demo heroes, straight into a Greater Rift), as -blizzcon does. The
# demo's title screen loads fewer UI files than 2.7.7's game screens need, so it is given
# the usual set.
set(D3_MOD_LOAD_WORLD "" CACHE STRING "Mod: start straight into this world (an SNO name); empty for the menus")
set(D3_MOD_BLIZZCON "" CACHE STRING "Mod: the BlizzCon demo; 1 to turn on, empty for off")
# D3_MOD_DEV_TOOLS=1: F5 the screenshot camera, F6 the target's internal name, F7 the
# controller view, developer views the game still draws once switched on; F8 the designer
# menu (src/mods/designer_menu.inc).
set(D3_MOD_DEV_TOOLS "" CACHE STRING "Mod: developer views on F5-F7 and the designer menu on F8; 1 to turn on, empty for off")
if(D3_MOD_LOAD_WORLD OR D3_MOD_BLIZZCON OR D3_MOD_DEV_TOOLS)
    set(_dev_defs "D3_MOD_LOAD_WORLD=\"${D3_MOD_LOAD_WORLD}\"")
    if(D3_MOD_LOAD_WORLD)
        message(STATUS "Mod: start in world ${D3_MOD_LOAD_WORLD}")
    endif()
    if(D3_MOD_BLIZZCON)
        message(STATUS "Mod: BlizzCon demo")
        list(APPEND _dev_defs "D3_MOD_BLIZZCON=1")
        d3_patch_source("X/Client/UserInterface/UI.cpp"
            --replace "        if (CmdLineGetParams()->tBitArray.Test(CMDLINE_BLIZZCON_BIT))"
                      "        if (false)")
    endif()
    if(D3_MOD_DEV_TOOLS)
        message(STATUS "Mod: developer views on F5-F7, designer menu on F8")
        list(APPEND _dev_defs "D3_MOD_DEV_TOOLS=1")
        d3_patch_source("X/Client/Input/InputNX64.cpp"
            --replace "    if ( sgtNX64Globals.bWasControllerAppShown )"
                      "    { void D3ModDevToolsUpdate()$<SEMICOLON> D3ModDevToolsUpdate()$<SEMICOLON> } if ( sgtNX64Globals.bWasControllerAppShown )")
        # F8 the designer menu: the release dropped its setup and list code, and runs none of
        # its entries' commands; src/mods/designer_menu.inc supplies both.
        d3_patch_source("X/Client/UserInterface/Console/UIConsoleDesignerMenu.cpp"
            --replace "DesignerMenuGlobals * sgptDesignerMenuGlobals$<SEMICOLON>"
                      "DesignerMenuGlobals * sgptDesignerMenuGlobals$<SEMICOLON> static void D3ModDesignerCommand(LPCSTR)$<SEMICOLON>"
            --replace "        puszCurrentCommand += strlen(puszCurrentCommand)$<SEMICOLON>"
                      "        D3ModDesignerCommand(puszCurrentCommand)$<SEMICOLON> puszCurrentCommand += strlen(puszCurrentCommand)$<SEMICOLON>"
            --append "#include \"${PROJECT_SOURCE_DIR}/src/mods/designer_menu.inc\"")
    endif()
    d3_patch_source("X/Common/Shell/MainNX64.cpp"
        --replace "    if (gtCmdLineParams.eTestType != TESTTYPE_NONE)"
                  "    { void D3ModDevStartup(CmdLineParams &)$<SEMICOLON> D3ModDevStartup(gtCmdLineParams)$<SEMICOLON> } if (gtCmdLineParams.eTestType != TESTTYPE_NONE)")
    list(APPEND D3_DECOMP_SOURCES "${PROJECT_SOURCE_DIR}/src/mods/dev.cpp")
    set_source_files_properties("${PROJECT_SOURCE_DIR}/src/mods/dev.cpp" PROPERTIES COMPILE_DEFINITIONS "${_dev_defs}")
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

# D3_MOD_EVENTS turns on events Blizzard's servers ran, comma-separated (src/mods/events.cpp):
# Anniversary (the Darkening of Tristram and the anniversary buff), CowLevel, and the
# DoubleBloodShards and DoubleRiftKeystones community buffs, and WorldBoss, our own event
# (src/mods/worldboss.cpp): every D3_MOD_WORLD_BOSS_MINUTES a rift guardian is announced and
# its spot in the hero's area marked on the minimap; it appears when the hero gets there. The first two change the
# defaults of the cvars the lobby would have set; the buffs go to the season theme mod.
set(D3_MOD_EVENTS "" CACHE STRING "Mod: events (Anniversary, CowLevel, DoubleBloodShards, DoubleRiftKeystones, WorldBoss; comma-separated); empty for off")
set(D3_MOD_WORLD_BOSS_MINUTES "10" CACHE STRING "Mod: minutes between world bosses (with D3_MOD_EVENTS=WorldBoss)")
set(_theme_keys "${D3_MOD_THEME}")
if(D3_MOD_EVENTS)
    string(REPLACE "," ";" _events "${D3_MOD_EVENTS}")
    foreach(_event IN LISTS _events)
        if(_event STREQUAL "Anniversary")
            d3_patch_source("X/Common/OnlineService/OnlineService.cpp"
                --replace "DEFAULT_ANNIVERSARY_EVENT_STATUS = -1$<SEMICOLON>" "DEFAULT_ANNIVERSARY_EVENT_STATUS = 1$<SEMICOLON>")
            d3_patch_source("X/Common/CommunityBuffs.cpp"
                --replace "\"AnniversaryEnabled\", false)" "\"AnniversaryEnabled\", true)")
        elseif(_event STREQUAL "CowLevel")
            d3_patch_source("X/Common/CommunityBuffs.cpp"
                --replace "\"EasterEggWorldEnabled\", false)" "\"EasterEggWorldEnabled\", true)")
        elseif(_event MATCHES "^(DoubleBloodShards|DoubleRiftKeystones)$")
            if(_theme_keys)
                string(APPEND _theme_keys ",${_event}")
            else()
                set(_theme_keys "${_event}")
            endif()
        elseif(_event STREQUAL "WorldBoss")
            if(NOT D3_MOD_WORLD_BOSS_MINUTES MATCHES "^[1-9][0-9]*$")
                message(FATAL_ERROR "D3_MOD_WORLD_BOSS_MINUTES must be a whole number of minutes, got '${D3_MOD_WORLD_BOSS_MINUTES}'")
            endif()
            list(APPEND D3_SHARED_HOOKS WORLDBOSS)  # strings, update
            list(APPEND D3_DECOMP_SOURCES "${PROJECT_SOURCE_DIR}/src/mods/worldboss.cpp")
            set_source_files_properties("${PROJECT_SOURCE_DIR}/src/mods/worldboss.cpp" PROPERTIES COMPILE_DEFINITIONS
                "D3_MOD_WORLD_BOSS_MINUTES=${D3_MOD_WORLD_BOSS_MINUTES}")
        else()
            message(FATAL_ERROR "D3_MOD_EVENTS: unknown event '${_event}' (Anniversary, CowLevel, DoubleBloodShards, DoubleRiftKeystones, WorldBoss)")
        endif()
    endforeach()
    message(STATUS "Mod: events ${D3_MOD_EVENTS}")
    list(APPEND D3_SHARED_HOOKS EVENTS)  # update
    list(APPEND D3_DECOMP_SOURCES "${PROJECT_SOURCE_DIR}/src/mods/events.cpp")
endif()

# D3_MOD_THEME turns on a season theme offline (src/mods/theme.cpp). 2.7.7's themes are
# community buffs that Blizzard's lobby switched on from its Config.txt; the value is the
# list of buff names it would have sent (CommunityBuff<Name> "1"), comma-separated, e.g.
# "SoulShards" or "ShadowClones,FourthKanaisCubeSlot". "Season" stands for the theme of the
# season D3_MOD_SEASON makes active (a table in theme.cpp). The buffs apply to every hero.
set(D3_MOD_THEME "" CACHE STRING "Mod: season theme (community buff names, comma-separated, or Season); empty for off")
if(_theme_keys)
    if(NOT _theme_keys MATCHES "^[A-Za-z]+(,[A-Za-z]+)*$")
        message(FATAL_ERROR "D3_MOD_THEME must be buff names separated by commas, got '${D3_MOD_THEME}'")
    endif()
    set(_theme_season 0)
    if(D3_MOD_SEASON)
        set(_theme_season ${D3_MOD_SEASON})
    endif()
    message(STATUS "Mod: season theme ${_theme_keys}")
    list(APPEND D3_SHARED_HOOKS THEME)  # update
    list(APPEND D3_DECOMP_SOURCES "${PROJECT_SOURCE_DIR}/src/mods/theme.cpp")
    set_source_files_properties("${PROJECT_SOURCE_DIR}/src/mods/theme.cpp" PROPERTIES COMPILE_DEFINITIONS
        "D3_MOD_THEME_KEYS=\"${_theme_keys}\";D3_MOD_THEME_SEASON=${_theme_season}")
endif()

# D3_MOD_CHALLENGE turns on challenge rifts offline (src/mods/challenge.cpp). The game
# fetched each week's challenge from Blizzard's storage as two files; this reads them from
# a folder instead: $D3_CHALLENGE_DIR, else <game folder>/rift_data. The value is "rotate"
# (each challenge in the folder in turn, moving on when one is completed) or the number of
# one challengerift_NN.dat to always play. Rewards earned offline are tagged with the
# season, as they are online, so seasonal heroes find the reward chest.
set(D3_MOD_CHALLENGE "" CACHE STRING "Mod: challenge rifts offline from files: rotate, or a file number; empty for off")
if(D3_MOD_CHALLENGE)
    if(NOT D3_MOD_CHALLENGE MATCHES "^(rotate|[0-9]+)$")
        message(FATAL_ERROR "D3_MOD_CHALLENGE must be rotate or a file number, got '${D3_MOD_CHALLENGE}'")
    endif()
    message(STATUS "Mod: challenge rifts offline (${D3_MOD_CHALLENGE})")
    d3_patch_source("X/Client/Console/LobbyService.cpp"
        --replace "    GetChallengeRiftConfig(nUserIndex,"
                  "    { void D3ModChallengeGetData(int32, OnlineService::Callback<void (StorageResult, const D3::ChallengeRifts::ChallengeData &, const D3::Leaderboard::WeeklyChallengeData &)>)$<SEMICOLON> D3ModChallengeGetData(nUserIndex, pfnCallback)$<SEMICOLON> return$<SEMICOLON> } GetChallengeRiftConfig(nUserIndex,")
    d3_patch_source("X/Client/CHero.cpp"
        --replace "if (Console::Online::LobbyIsConnected(nUserIndex) &&"
                  "if ((Console::Online::LobbyIsConnected(nUserIndex) || true) &&")
    list(APPEND D3_DECOMP_SOURCES "${PROJECT_SOURCE_DIR}/src/mods/challenge.cpp")
    set_source_files_properties("${PROJECT_SOURCE_DIR}/src/mods/challenge.cpp" PROPERTIES COMPILE_DEFINITIONS
        "D3_MOD_CHALLENGE_DIR=\"${D3_GAME_DIR}/rift_data\";D3_MOD_CHALLENGE_PICK=\"${D3_MOD_CHALLENGE}\"")
endif()

# D3_MOD_QOL: quality-of-life tweaks for offline play, comma-separated:
#   InstantPortal  Town Portal skips its 3-second channel;
#   InstantIdentify  identifying an item, and the Book of Cain, take no time;
#   InstantCraft   the blacksmith, the Mystic and Kanai's Cube skip their wait;
#   FreeCube       Kanai's Cube recipes keep their materials and gold (you still need
#                  them to start; the item being changed is still used up);
#   GemUpgrade     legendary gem upgrades at the Urshi always succeed;
#   GemMax         one Urshi upgrade takes a legendary gem straight to rank 150;
#   AnySocket      any gem fits any socket, legendary gems included;
#   AnySlot        armour and jewellery go in any armour or jewellery slot;
#   NoUniqueEquip  wear two of a unique-equipped legendary, such as a ring;
#   AutoPickup     walking over a legendary or set item picks it up.
set(D3_MOD_QOL "" CACHE STRING "Mod: quality-of-life tweaks (InstantPortal, InstantIdentify, InstantCraft, FreeCube, GemUpgrade, GemMax, AnySocket, AnySlot, NoUniqueEquip, AutoPickup; comma-separated); empty for off")
if(D3_MOD_QOL)
    set(_ui "X/Client/UserInterface/Console")
    set(_crafting "X/Server/Crafting/SItemCrafting.cpp")
    string(REPLACE "," ";" _options "${D3_MOD_QOL}")
    foreach(_option IN LISTS _options)
        if(_option STREQUAL "InstantPortal")
            # The channel is a timer on the Hearth power; 0 ticks fires on the next update.
            d3_patch_source("X/Server/Powers/SpellEffect.cpp"
                --replace "    int32 uTicks = Math::FloatRoundToInt(flTime * 60.0f)$<SEMICOLON>"
                          "    int32 uTicks = 0$<SEMICOLON> (void)flTime$<SEMICOLON>")
        elseif(_option STREQUAL "InstantIdentify")
            # Single items: the client's progress bar (1, not 0: it divides by it). The
            # Book of Cain: identify at once, then stop the power on the next update.
            d3_patch_source("${_ui}/UIConsoleInventoryView.cpp"
                --replace "const uint32 UIInventoryView::IDENTIFYING_DURATION = 150$<SEMICOLON>"
                          "const uint32 UIInventoryView::IDENTIFYING_DURATION = 1$<SEMICOLON>")
            d3_patch_source("${_ui}/UIConsoleJewelUpgrade.cpp"
                --replace "static const uint32 IDENTIFYING_DURATION = 150$<SEMICOLON>"
                          "static const uint32 IDENTIFYING_DURATION = 1$<SEMICOLON>")
            d3_patch_source("X/Server/Powers/SpellEffect.cpp"
                --replace "    PowerSetStoredInt(ptPower, \"idRopeLeft\", -1)$<SEMICOLON>"
                          "    PowerSetStoredInt(ptPower, \"idRopeLeft\", -1)$<SEMICOLON> sSpellEffect_IdentifyAll(idPower, NULL)$<SEMICOLON> PowerTriggerAddTimerTrigger(idPower, 0, [](const PowerID id, void *) { PowerForceStop(id, TRUE)$<SEMICOLON> }, NULL)$<SEMICOLON>")
        elseif(_option STREQUAL "InstantCraft")
            # All client-side waits; the server crafts as soon as it is asked.
            d3_patch_source("${_ui}/UIConsoleBlacksmith.cpp"
                --replace "const uint32 PlayerBlacksmithScreen::CRAFTING_DURATION = 150$<SEMICOLON>"
                          "const uint32 PlayerBlacksmithScreen::CRAFTING_DURATION = 0$<SEMICOLON>")
            d3_patch_source("${_ui}/UIConsoleEnchantView.cpp"
                --replace "static const uint32 REPLACEMENT_LIST_REVEAL_DURATION = 120$<SEMICOLON>"
                          "static const uint32 REPLACEMENT_LIST_REVEAL_DURATION = 1$<SEMICOLON>")
            d3_patch_source("${_ui}/UIConsoleTransmute.cpp"
                --replace "static const uint32 FX_DURATION = 120$<SEMICOLON>" "static const uint32 FX_DURATION = 2$<SEMICOLON>"
                --replace "static const uint32 FX_UBER_DURATION = 240$<SEMICOLON>" "static const uint32 FX_UBER_DURATION = 2$<SEMICOLON>"
                --replace "static const uint32 FX_COVERAGE_TIME = 60$<SEMICOLON>" "static const uint32 FX_COVERAGE_TIME = 1$<SEMICOLON>"
                --replace "static const uint32 FX_UBER_COVERAGE_TIME = 180$<SEMICOLON>" "static const uint32 FX_UBER_COVERAGE_TIME = 1$<SEMICOLON>")
        elseif(_option STREQUAL "FreeCube")
            # 2.7.7's materials (and gold) are currencies; items given to a recipe are
            # still removed, or the cube would copy the item it changes.
            d3_patch_source("${_crafting}"
                --replace "                ptACDPlayer->ModifyCurrencyAmount(-nDeleteCount, eCurrency, GMR_CUBE_RECIPE, FALSE)$<SEMICOLON>"
                          "                (void)nDeleteCount$<SEMICOLON>")
        elseif(_option STREQUAL "GemUpgrade")
            if(NOT D3_MOD_QOL MATCHES "GemMax")
                d3_patch_source("${_crafting}"
                    --replace "            nUpgradeRanks = (flRandChance <= flUpgradeChance) ? 1 : 0$<SEMICOLON>"
                              "            nUpgradeRanks = 1$<SEMICOLON> (void)flRandChance$<SEMICOLON>")
            endif()
            # The Urshi shows 100%, from the same function the server uses.
            d3_patch_source("X/Common/Crafting/ItemCrafting.cpp"
                --replace "    if (nRiftLevel < 0 || nJewelRank >= 150)"
                          "    if (nRiftLevel >= 0 && nJewelRank < 150) { return 1.0f$<SEMICOLON> } else")
        elseif(_option STREQUAL "GemMax")
            # SItemUpgradeJewel adds each rank's stats and unlocks rank 25's effect.
            d3_patch_source("${_crafting}"
                --replace "            nUpgradeRanks = (flRandChance <= flUpgradeChance) ? 1 : 0$<SEMICOLON>"
                          "            nUpgradeRanks = Math::Max(150 - tACDItem.AttributesGetInt(ATTRIBUTE_JEWEL_RANK), 0)$<SEMICOLON> (void)flRandChance$<SEMICOLON>")
        elseif(_option STREQUAL "AnySocket")
            # Any gem in any socket. A normal gem whose effects table has no row for the
            # item gives no bonus there, instead of stopping the game.
            d3_patch_source("X/Common/Items.cpp"
                --replace "    if (ItemIsJewel(tACDItemToSocket))"
                        "    fFound = TRUE$<SEMICOLON> if (ItemIsJewel(tACDItemToSocket))"
                --replace "BOOL ItemRemoveSocketedItemAttributes(ActorCommonData & tACDItemToSocket, const ActorCommonData & tACDItemSocketedInto)"
                        "static BOOL sD3ModGemHasEffect(GBID gbidGem, GBHandle hgbInto) { GBHandleList lstEffects(GBGetHandlePool())$<SEMICOLON> GBEnumerate(GB_SOCKETED_EFFECTS, lstEffects)$<SEMICOLON> BOOL bFound = FALSE$<SEMICOLON> POS pos = lstEffects.GetHeadPosition()$<SEMICOLON> while (pos && !bFound) { const GBHandle & hGB = lstEffects.GetNext(pos)$<SEMICOLON> const GameBalanceDefinition * pdefGB$<SEMICOLON> SocketedEffectsTableEntry * ptEffectDef = (SocketedEffectsTableEntry *)GBLockR(hGB, pdefGB, TRUE)$<SEMICOLON> bFound = ptEffectDef->gbidItemBeingSocketed == gbidGem && (ptEffectDef->gbidItemTypeSocketedInto == StringToGBID(\"All\") || ItemIsOfType(hgbInto, ptEffectDef->gbidItemTypeSocketedInto))$<SEMICOLON> GBUnlockR(pdefGB)$<SEMICOLON> } return bFound$<SEMICOLON> } static BOOL sD3ModRemoveSocketed(ActorCommonData &, const ActorCommonData &)$<SEMICOLON> BOOL ItemRemoveSocketedItemAttributes(ActorCommonData & a, const ActorCommonData & b) { if (ItemIsGem(a.hGB.gbid) && !ItemIsJewel(a.hGB.gbid) && !sD3ModGemHasEffect(a.hGB.gbid, b.hGB)) { return TRUE$<SEMICOLON> } return sD3ModRemoveSocketed(a, b)$<SEMICOLON> } static BOOL sD3ModRemoveSocketed(ActorCommonData & tACDItemToSocket, const ActorCommonData & tACDItemSocketedInto)"
                --replace "BOOL ItemAddSocketedItemAttributes(ActorCommonData & tACDItemToSocket, const ActorCommonData & tACDItemSocketedInto)"
                        "static BOOL sD3ModAddSocketed(ActorCommonData &, const ActorCommonData &)$<SEMICOLON> BOOL ItemAddSocketedItemAttributes(ActorCommonData & a, const ActorCommonData & b) { if (ItemIsGem(a.hGB.gbid) && !ItemIsJewel(a.hGB.gbid) && !sD3ModGemHasEffect(a.hGB.gbid, b.hGB)) { return TRUE$<SEMICOLON> } return sD3ModAddSocketed(a, b)$<SEMICOLON> } static BOOL sD3ModAddSocketed(ActorCommonData & tACDItemToSocket, const ActorCommonData & tACDItemSocketedInto)")
        elseif(_option STREQUAL "AnySlot")
            # Armour and jewellery in any armour or jewellery slot (not the hands).
            d3_patch_source("X/Common/ActorInventory.cpp"
                --replace "    if (!ItemIsAllowedInSlot(tInvLoc.eSlot, pItemTypeDef))"
                        "    if (!ItemIsAllowedInSlot(tInvLoc.eSlot, pItemTypeDef) && !(tInvLoc.eSlot >= SLOT_PLAYER_BODYSLOT_START && tInvLoc.eSlot < SLOT_PLAYER_BODYSLOT_END && tInvLoc.eSlot != SLOT_PLAYER_RIGHT_HAND && tInvLoc.eSlot != SLOT_PLAYER_LEFT_HAND && pItemTypeDef->eBodySlot1 >= SLOT_PLAYER_BODYSLOT_START && pItemTypeDef->eBodySlot1 < SLOT_PLAYER_BODYSLOT_END && pItemTypeDef->eBodySlot1 != SLOT_PLAYER_RIGHT_HAND && pItemTypeDef->eBodySlot1 != SLOT_PLAYER_LEFT_HAND))")
            d3_patch_source("${_ui}/UIConsoleInventoryFilters.cpp"
                --replace "    if (!ACDInventoryItemAllowedInThisSlotType(ptACDItem->hGB.gbid, eSlot)) return FALSE$<SEMICOLON>"
                        "    if (!ACDInventoryItemAllowedInThisSlotType(ptACDItem->hGB.gbid, eSlot) && !(eSlot >= SLOT_PLAYER_BODYSLOT_START && eSlot < SLOT_PLAYER_BODYSLOT_END && eSlot != SLOT_PLAYER_RIGHT_HAND && eSlot != SLOT_PLAYER_LEFT_HAND && [&]() { for (int32 s = SLOT_PLAYER_BODYSLOT_START$<SEMICOLON> s < SLOT_PLAYER_BODYSLOT_END$<SEMICOLON> s++) { if (s != SLOT_PLAYER_RIGHT_HAND && s != SLOT_PLAYER_LEFT_HAND && ACDInventoryItemAllowedInThisSlotType(ptACDItem->hGB.gbid, (InventorySlotType)s)) { return true$<SEMICOLON> } } return false$<SEMICOLON> }())) return FALSE$<SEMICOLON>")
        elseif(_option STREQUAL "NoUniqueEquip")
            # Wear more than one of a unique-equipped legendary, such as two of a ring.
            d3_patch_source("X/Common/ActorInventory.cpp"
                --replace "    if (ItemIsUniqueEquipped(tACDItem))"
                        "    if (FALSE && ItemIsUniqueEquipped(tACDItem))"
                --replace "        if (ItemIsUniqueEquipped(gbidItem))"
                        "        if (FALSE && ItemIsUniqueEquipped(gbidItem))"
                --replace "                (gbidLinkedPowerOutter != GBID_NONE || gbidLinkedPowerInner != GBID_NONE))"
                        "                FALSE)")
        elseif(_option STREQUAL "AutoPickup")
            # Walking over legendary and set items picks them up, like gold. Items the hero
            # drops are marked so they stay on the ground.
            d3_patch_source("X/Common/ActorCommonData.cpp"
                --replace "            ItemIsAutoPickup(*ptACDItem) &&"
                        "            (ItemIsAutoPickup(*ptACDItem) || (!ptACDItem->AttributesGetInt(ATTRIBUTE_NO_AUTO_PICKUP) && (ItemIsLegendary(ptACDItem->hGB.gbid) || ItemIsSetItem(ptACDItem->hGB.gbid)))) &&")
            d3_patch_source("X/Common/ActorInventory.cpp"
                --replace "        if (ItemIsAutoPickup(tACDItem))"
                        "        if (ItemIsAutoPickup(tACDItem) || ItemIsLegendary(tACDItem.hGB.gbid) || ItemIsSetItem(tACDItem.hGB.gbid))")
        else()
            message(FATAL_ERROR "D3_MOD_QOL: unknown option '${_option}' (InstantPortal, InstantIdentify, InstantCraft, FreeCube, GemUpgrade, GemMax, AnySocket, AnySlot, NoUniqueEquip, AutoPickup)")
        endif()
    endforeach()
    message(STATUS "Mod: quality of life ${D3_MOD_QOL}")
endif()

# D3_MOD_ANCIENTS changes how legendary and set items roll ancient. All of the game's
# rolls (drops, Kadala, crafting, the cube) go through LootRollForAncientLegendary:
#   NoPrimals  ancients as normal, never primal ancient;
#   None       no ancients (so no primals either);
#   Ancient    every level-70 legendary and set item is ancient;
#   Primal     every level-70 legendary and set item is primal ancient.
set(D3_MOD_ANCIENTS "" CACHE STRING "Mod: ancient rolls (NoPrimals, None, Ancient, Primal); empty: the game's own")
if(D3_MOD_ANCIENTS)
    set(_loot "X/Server/Loot/Loot.cpp")
    if(D3_MOD_ANCIENTS STREQUAL "NoPrimals")
        d3_patch_source("${_loot}"
            --replace "        if (idACDLooter != ACDID_INVALID && !tSpecifier.tLooteeParams.bDisablePrimalAncientDrops)"
                      "        if (false && idACDLooter != ACDID_INVALID && !tSpecifier.tLooteeParams.bDisablePrimalAncientDrops)"
            --replace "    if (GameIsSeasonal() && tSpecifier.tLooteeParams.nTieredLootRunLevel > 0 && NephalemRiftManager::RiftPlayerCount() == 1)"
                      "    if (false && GameIsSeasonal() && tSpecifier.tLooteeParams.nTieredLootRunLevel > 0 && NephalemRiftManager::RiftPlayerCount() == 1)")
    elseif(D3_MOD_ANCIENTS STREQUAL "None")
        d3_patch_source("${_loot}"
            --replace "    if (tSpecifier.tLooteeParams.bDisableAncientDrops)"
                      "    if (true || tSpecifier.tLooteeParams.bDisableAncientDrops)")
    elseif(D3_MOD_ANCIENTS MATCHES "^(Ancient|Primal)$")
        if(D3_MOD_ANCIENTS STREQUAL "Ancient")
            set(_rank ANCIENT_RANK_ANCIENT)
        else()
            set(_rank ANCIENT_RANK_PRIMAL_ANCIENT)
        endif()
        # After the level-70 and legendary checks, before the ethereal and chance rolls.
        d3_patch_source("${_loot}"
            --replace "    if (sub_710050e910(tSpecifier.hGBItem.gbid))"
                      "    if (ItemIsEquippableByPlayerOrHireling(tSpecifier.hGBItem.gbid) || sub_710050e910(tSpecifier.hGBItem.gbid)) { tSpecifier.eAncientRank = ${_rank}$<SEMICOLON> return$<SEMICOLON> } if (sub_710050e910(tSpecifier.hGBItem.gbid))")
    else()
        message(FATAL_ERROR "D3_MOD_ANCIENTS must be NoPrimals, None, Ancient or Primal, got '${D3_MOD_ANCIENTS}'")
    endif()
    message(STATUS "Mod: ancients ${D3_MOD_ANCIENTS}")
endif()

# D3_MOD_ITEM_LEVEL=70 makes monsters, chests and other world drops roll as if the area
# were that level (1 to 70): which items can drop, their stats and their level. Kadala,
# crafting and the cube keep their own levels.
set(D3_MOD_ITEM_LEVEL "" CACHE STRING "Mod: the level world drops roll at, 1 to 70 (empty: the area's level)")
if(D3_MOD_ITEM_LEVEL)
    if(NOT D3_MOD_ITEM_LEVEL MATCHES "^[0-9]+$" OR D3_MOD_ITEM_LEVEL LESS 1 OR D3_MOD_ITEM_LEVEL GREATER 70)
        message(FATAL_ERROR "D3_MOD_ITEM_LEVEL must be a level from 1 to 70, got '${D3_MOD_ITEM_LEVEL}'")
    endif()
    message(STATUS "Mod: world drops roll at level ${D3_MOD_ITEM_LEVEL}")
    d3_patch_source("X/Server/Loot/Loot.cpp"
        --replace "    if (tLooteeParams.nLevel <= 0)"
                  "    tLooteeParams.nLevel = ${D3_MOD_ITEM_LEVEL}$<SEMICOLON> if (tLooteeParams.nLevel <= 0)"
        # The game raises legendaries rolled at 61 to 63 to 64; a chosen level stays.
        --replace "            if (tLootee.tLooteeParams.nLevel >= 61 && tLootee.tLooteeParams.nLevel <= 63)"
                  "            if (false && tLootee.tLooteeParams.nLevel >= 61 && tLootee.tLooteeParams.nLevel <= 63)"
        --replace "        if (tLooteeParams.nLevel >= 61 && tLooteeParams.nLevel <= 63)"
                  "        if (false && tLooteeParams.nLevel >= 61 && tLooteeParams.nLevel <= 63)")
endif()

# D3_MOD_LOOT loosens the drop rules, comma-separated:
#   NoTormentGate      items marked Torment-only drop on every difficulty;
#   NoRestrictions     items kept for later acts, rifts, hardcore heroes or season unlocks
#                      drop anywhere;
#   KadalaLegendaries  every Kadala purchase is a legendary or set item.
set(D3_MOD_LOOT "" CACHE STRING "Mod: drop rules (NoTormentGate, NoRestrictions, KadalaLegendaries; comma-separated); empty for off")
if(D3_MOD_LOOT)
    set(_loot "X/Server/Loot/Loot.cpp")
    string(REPLACE "," ";" _options "${D3_MOD_LOOT}")
    foreach(_option IN LISTS _options)
        if(_option STREQUAL "NoTormentGate")
            d3_patch_source("${_loot}"
                --replace "    if (!bDisableTormentCheck && (tItem.dwFlags & (1 << ITEM_TORMENT_ONLY_BIT)))"
                          "    if (false && !bDisableTormentCheck && (tItem.dwFlags & (1 << ITEM_TORMENT_ONLY_BIT)))")
        elseif(_option STREQUAL "NoRestrictions")
            d3_patch_source("${_loot}"
                --replace "    if (tItem.eMinAct != ACT_INVALID && eCurrentAct != ACT_INVALID && tItem.eMinAct > eCurrentAct)"
                          "    if (false && tItem.eMinAct != ACT_INVALID && eCurrentAct != ACT_INVALID && tItem.eMinAct > eCurrentAct)"
                --replace "    if (tItem.dwFlags & (1 << ITEM_TIERED_LOOT_RUN_ONLY_BIT))"
                          "    if (false && (tItem.dwFlags & (1 << ITEM_TIERED_LOOT_RUN_ONLY_BIT)))"
                --replace "    if (!bIgnoreSeason && !sub_7100897970(tItem.gbid, ACDTryToGet(idACDLooter)))"
                          "    if (false && !bIgnoreSeason && !sub_7100897970(tItem.gbid, ACDTryToGet(idACDLooter)))"
                --replace "    BOOL bHardcore = FALSE$<SEMICOLON>" "    BOOL bHardcore = TRUE$<SEMICOLON>")
        elseif(_option STREQUAL "KadalaLegendaries")
            # Only Kadala's purchases set bIgnoreClassLegendaryDropWeight. Her treasure
            # classes may pick one entry or roll each; both rolls are covered.
            d3_patch_source("${_loot}"
                --replace "                            float flChance = flNaturalChance$<SEMICOLON>"
                          "                            float flChance = tLootee.tLooteeParams.bIgnoreClassLegendaryDropWeight ? 1.0f : flNaturalChance$<SEMICOLON>"
                --replace "    BOOL bIsLegendary = sLootModifierIsLegendary(tLootDropModifier)$<SEMICOLON>"
                          "    BOOL bIsLegendary = sLootModifierIsLegendary(tLootDropModifier)$<SEMICOLON> if (bIsLegendary && tLootee.tLooteeParams.bIgnoreClassLegendaryDropWeight) { flChance = 1.0f$<SEMICOLON> }")
        else()
            message(FATAL_ERROR "D3_MOD_LOOT: unknown option '${_option}' (NoTormentGate, NoRestrictions, KadalaLegendaries)")
        endif()
    endforeach()
    message(STATUS "Mod: loot ${D3_MOD_LOOT}")
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

d3_apply_patches()
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
