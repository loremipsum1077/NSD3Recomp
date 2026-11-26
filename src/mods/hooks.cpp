// Hook points shared by more than one mod. Two mods cannot both replace the same line of a
// decomp file, so the places several mods need (cmake/DecompSources.cmake, "shared hooks")
// are patched once to call these, and each mod that is on is called in turn. CMake defines
// D3_HOOK_<MOD>=1 for each mod that uses them.
#include "X/Common/Definitions/GameBalanceDefinition.h"

#if D3_HOOK_VENDOR
void D3ModVendorExtendItems(GameBalanceDefinition * pdefGB, int32 & nCount, void *& pTable);  // src/mods/vendor.cpp
const char * D3ModVendorString(SNO snoStringList, const char * szLabel);
void D3ModVendorUpdate();
#endif
#if D3_HOOK_CORRUPT
void D3ModCorruptExtendTable(GameBalanceDefinition * pdefGB, int32 & nCount, void *& pTable);  // src/mods/corrupt.cpp
const char * D3ModCorruptString(SNO snoStringList, const char * szLabel);
void D3ModCorruptUpdate();
#endif
#if D3_HOOK_THEME
void D3ModThemeUpdate();  // src/mods/theme.cpp
#endif
#if D3_HOOK_EVENTS
void D3ModEventsUpdate();  // src/mods/events.cpp
#endif

// As each GameBalance file is loaded, before its rows are registered: rows may be added.
void D3ModExtendGameBalance(GameBalanceDefinition * pdefGB, int32 & nCount, void *& pTable)
{
#if D3_HOOK_VENDOR
    D3ModVendorExtendItems(pdefGB, nCount, pTable);
#endif
#if D3_HOOK_CORRUPT
    D3ModCorruptExtendTable(pdefGB, nCount, pTable);
#endif
}

// Every string-list lookup: a mod's own text, or NULL for the game's.
const char * D3ModString(SNO snoStringList, const char * szLabel)
{
    const char * szText = NULL;
#if D3_HOOK_VENDOR
    if (!szText)
    {
        szText = D3ModVendorString(snoStringList, szLabel);
    }
#endif
#if D3_HOOK_CORRUPT
    if (!szText)
    {
        szText = D3ModCorruptString(snoStringList, szLabel);
    }
#endif
    return szText;
}

// Once per server player update (about once a second), on the server's thread.
void D3ModServerUpdate()
{
#if D3_HOOK_VENDOR
    D3ModVendorUpdate();
#endif
#if D3_HOOK_CORRUPT
    D3ModCorruptUpdate();
#endif
#if D3_HOOK_THEME
    D3ModThemeUpdate();
#endif
#if D3_HOOK_EVENTS
    D3ModEventsUpdate();
#endif
}
