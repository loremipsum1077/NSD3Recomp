// The developer "designer menu" (D3_MOD_DEV_TOOLS, F8), included at the end of a patched copy
// of X/Client/UserInterface/Console/UIConsoleDesignerMenu.cpp, so it sees the file's own classes.
//
// The release keeps the menu's layout (ConsoleDesignerMenu), its screen set and the code that
// opens it and handles the buttons, but the linker dropped what nothing called: the globals'
// constructor, the list data sources' item code and the lists of entries, and every command
// the entries ran (ProcessCompoundCommands splits its text and does nothing with it). This
// file supplies those. The entry lists are ours: the original ones are gone. The Quest and
// Music pages fill themselves from game data, as the release's code does.
//
// A selected entry's command goes to D3ModDesignerCommand: "cheat <line>" runs a cheats-page
// command (when D3_MOD_CHEATS is built), "theme <request>" switches the season theme (when
// D3_MOD_THEME is built), "dev <view>" toggles a developer view, anything else
// (the Quest and Music pages' original console commands) is only logged.

#include "X/Client/CPlayer.h"
#include "X/Common/ActorCommonData.h"
#include <stdio.h>
#include <stdlib.h>
// The cheats and theme mods' entry points (src/mods/cheats.cpp, theme.cpp). The Mac linker
// will not leave a weak reference undefined, so these weak definitions stand in when a mod
// is off, and D3ModCheatsOn/D3ModThemeOn say whether it is on.
__attribute__((weak)) bool D3ModCheatsOn() { return false; }
__attribute__((weak)) bool D3ModThemeOn() { return false; }
__attribute__((weak)) void D3ModCheatsQueue(const char *) {}
__attribute__((weak)) void D3ModThemeRequest(const char *) {}
extern UIID UIDLG_CONSOLEDESIGNERMENU_CHARACTER;  // X/Client/UserInterface/UIGlobals.cpp
extern UIID UIDLG_CONSOLEDESIGNERMENU_GAME;
extern UIID UIDLG_CONSOLEDESIGNERMENU_CONSOLE;
extern UIID UIDLG_CONSOLEDESIGNERMENU_QUICKOPTIONS;
void CinematographyCutToStaticView(const SNO snoObserver, const Vector3D & wpCenter);  // X/Client/Camera/Cinematography.cpp
void CinematographyFixedView(RActorID idTargetRActor);
void CinematographyResetCameras();
namespace Console {
void ToggleTargetName();            // X/Client/Debug/TargetingDebugView.cpp
void ToggleControllerDebugView();   // X/Client/Debug/ControllerDebugView.cpp
}
namespace UIConsoleDesignerMenu {

static void D3ModDesignerCommand(LPCSTR szCommand)
{
    while (*szCommand == ' ')
    {
        szCommand++;
    }
    if (!*szCommand)
    {
        return;
    }
    if (strncmp(szCommand, "cheat ", 6) == 0)
    {
        if (D3ModCheatsOn())
        {
            D3ModCheatsQueue(szCommand + 6);
            fprintf(stderr, "designer: %s\n", szCommand + 6);
        }
        else
        {
            fprintf(stderr, "designer: %s (build with D3_MOD_CHEATS to run it)\n", szCommand + 6);
        }
        return;
    }
    if (strncmp(szCommand, "theme ", 6) == 0)
    {
        if (D3ModThemeOn())
        {
            D3ModThemeRequest(szCommand + 6);
            fprintf(stderr, "designer: %s\n", szCommand);
        }
        else
        {
            fprintf(stderr, "designer: %s (build with D3_MOD_THEME to switch themes)\n", szCommand);
        }
        return;
    }
    if (strcmp(szCommand, "dev targetname") == 0)
    {
        ::Console::ToggleTargetName();
    }
    else if (strcmp(szCommand, "dev stickview") == 0)
    {
        ::Console::ToggleControllerDebugView();
    }
    else if (strcmp(szCommand, "dev camera") == 0 && CGameIsValid() && ACDTryToGet(LocalPlayerACDGet()))
    {
        static bool bFixed = false;
        if (!bFixed)
        {
            CinematographyCutToStaticView(GlobalSNOGet(GLOBALSNO_OBSERVER_SCREENSHOT_FRONT),
                                          ACDTryToGet(LocalPlayerACDGet())->GetWorldLocation().transform.wp);
        }
        else
        {
            CinematographyFixedView(LocalPlayerRActorGet());
            CinematographyResetCameras();
        }
        bFixed = !bFixed;
    }
    else
    {
        fprintf(stderr, "designer: \"%s\" was a developer console command; this build has no console\n", szCommand);
        return;
    }
    fprintf(stderr, "designer: %s\n", szCommand);
}

// Rows: each list has its own row template in the layout, with one text control.
static UIControlSourceKey sRowKey(LPCSTR szTemplate)
{
    CRefString sKey;
    sKey.Format("ConsoleDesignerMenu:ConsoleDesignerMenu_main.LayoutRoot.%s", szTemplate);
    return UIControlSourceKey::Parse(sKey, -1, NULL, NULL);
}

static void sSetRow(const UIConsoleListBox & tListBox, const UIID & idItemUI, const int32 nIndex, LPCSTR szTextChild, LPCSTR szText, BOOL bDim)
{
    UIID idText = UIIDCreateFromParent(idItemUI, szTextChild);
    if (!UI_TestID(idText))
    {
        return;
    }
    UIText & tText = UI_GetTextFromID(idText);
    tText.SetText(szText, FALSE);
    tText.SetTextColor(tListBox.GetSelectedIndex() == nIndex ? crgbaWhite : bDim ? crgbaGray : crgbaGold);
}

static void sSetCommandRow(const UIConsoleListBox & tListBox, const UIID & idItemUI, const int32 nIndex, LPCSTR szTextChild,
                           const ConsoleCommandOption & tOption, BOOL bToggle)
{
    CRefString sText;
    if (bToggle)
    {
        sText.Format("%s  [%s]", tOption.m_displayText.str(), tOption.bToggledOn ? "on" : "off");
    }
    else
    {
        sText = tOption.m_displayText;
    }
    sSetRow(tListBox, idItemUI, nIndex, szTextChild, sText, tOption.bHasBeenSelected);
}

static void sAdd(blz::vector<ConsoleCommandOption, blz::allocator<ConsoleCommandOption> > & arList, LPCSTR szText, LPCSTR szCommand)
{
    ConsoleCommandOption tOption;
    tOption.m_displayText = szText;
    tOption.m_command = szCommand;
    arList.push_back(tOption);
}

// Every list draws its rows the same way.
#define D3_DESIGNER_ROWS(Class, Items, Toggle, Template, TextChild)                                                \
    UIControlSourceKey Class::GetSourceKeyForItem(const int32) const { return sRowKey(Template); }               \
    void Class::InitializeItemUI(const UIConsoleListBox &, const UIID &, const int32) {}                          \
    void Class::UpdateItemUI(const UIConsoleListBox & tListBox, const UIID & idItemUI, const int32 nIndex)       \
    {                                                                                                             \
        sSetCommandRow(tListBox, idItemUI, nIndex, TextChild, Items[nIndex], Toggle);                                          \
    }

MenuOptionItem::MenuOptionItem()
    : m_eMenu(MENU_CHARACTER)
{
}

MultipleTypeOption::MultipleTypeOption()
    : m_bCloseOnSelect(FALSE), bToggledOn(FALSE), bHasBeenSelected(FALSE), eType(TEXT), eID(OTHER)
{
}

MainMenuListDataSource::MainMenuListDataSource() {}
int32 MainMenuListDataSource::GetNumItems() const { return NUM_MENUS; }
UIControlSourceKey MainMenuListDataSource::GetSourceKeyForItem(const int32) const { return sRowKey("MenuDialog.MenuOptionsList.MenuOptionsItem"); }
void MainMenuListDataSource::InitializeItemUI(const UIConsoleListBox &, const UIID &, const int32) {}
void MainMenuListDataSource::UpdateItemUI(const UIConsoleListBox & tListBox, const UIID & idItemUI, const int32 nIndex)
{
    sSetRow(tListBox, idItemUI, nIndex, ".MenuOptionName", m_menuOptionItems[nIndex].m_displayText, FALSE);
}
void MainMenuListDataSource::Initialize()
{
    // In MainMenus order, which OnButton1's switch follows.
    const UIID * arIDs[NUM_MENUS] = {&UIDLG_CONSOLEDESIGNERMENU_CHARACTER, &UIDLG_CONSOLEDESIGNERMENU_QUEST,
                                     &UIDLG_CONSOLEDESIGNERMENU_GAME, &UIDLG_CONSOLEDESIGNERMENU_CONSOLE,
                                     &UIDLG_CONSOLEDESIGNERMENU_QUICKOPTIONS, &UIDLG_CONSOLEDESIGNERMENU_GAME};  // Music uses the Game page's list
    static LPCSTR const arNames[NUM_MENUS] = {"Character", "Quest", "Game", "Console", "Quick Options", "Music"};
    for (int32 i = 0; i < NUM_MENUS; i++)
    {
        m_menuOptionItems[i].m_menuID = *arIDs[i];
        m_menuOptionItems[i].m_eMenu = (MainMenus)i;
        m_menuOptionItems[i].m_displayText = arNames[i];
    }
}

CharacterResetHeroListDataSource::CharacterResetHeroListDataSource() {}
D3_DESIGNER_ROWS(CharacterResetHeroListDataSource, m_consoleCommandOptionItems, FALSE, "Character.CharacterResetList.CharacterResetListItem", ".CharacterResetName")
void CharacterResetHeroListDataSource::Initialize()
{
    sAdd(m_consoleCommandOptionItems, "Level 70", "cheat level 70");
    sAdd(m_consoleCommandOptionItems, "Unlock Kanai's Cube", "cheat cube");
}

CharacterFollowerListDataSource::CharacterFollowerListDataSource() {}
D3_DESIGNER_ROWS(CharacterFollowerListDataSource, m_consoleCommandOptionItems, FALSE, "Character.CharacterFollowerList.CharacterFollowerListItem", ".CharacterFollowerName")
void CharacterFollowerListDataSource::Initialize()
{
    sAdd(m_consoleCommandOptionItems, "Templar", "follower templar");
    sAdd(m_consoleCommandOptionItems, "Scoundrel", "follower scoundrel");
    sAdd(m_consoleCommandOptionItems, "Enchantress", "follower enchantress");
}

CharacterBoostOptionsListDataSource::CharacterBoostOptionsListDataSource() {}
D3_DESIGNER_ROWS(CharacterBoostOptionsListDataSource, m_consoleCommandOptionItems, FALSE, "Character.CharacterBoostList.CharacterBoostListItem", ".CharacterBoostName")
void CharacterBoostOptionsListDataSource::Initialize()
{
    sAdd(m_consoleCommandOptionItems, "+100 Paragon", "cheat paragon 100");
    sAdd(m_consoleCommandOptionItems, "+10,000,000 Gold", "cheat gold 10000000");
    sAdd(m_consoleCommandOptionItems, "Greater Rift 150 unlocked", "cheat rifts 150");
    sAdd(m_consoleCommandOptionItems, "5 Act I Horadric Caches", "cheat cache 1 5");
}

int32 QuestListDataSource::GetNumItems() const { return m_menuOptions.size(); }
UIControlSourceKey QuestListDataSource::GetSourceKeyForItem(const int32) const { return sRowKey("Quest.QuestList.QuestListItem"); }
void QuestListDataSource::InitializeItemUI(const UIConsoleListBox &, const UIID &, const int32) {}
void QuestListDataSource::UpdateItemUI(const UIConsoleListBox & tListBox, const UIID & idItemUI, const int32 nIndex)
{
    sSetRow(tListBox, idItemUI, nIndex, ".QuestName", m_menuOptions[nIndex].m_displayName, FALSE);
}

// The console page's selector rows (voice chat and the like) are not used.
int32 SelectorDataSource::GetNumItems() const { return 0; }
UIControlSourceKey SelectorDataSource::GetSourceKeyForItem(const int32) const { return sRowKey("Console.ConsoleList.SelectorItem.SelectorTemplate"); }
void SelectorDataSource::InitializeItemUI(const UIConsoleSelector &, const UIID &, const int32) {}
void SelectorDataSource::UpdateItemUI(const UIConsoleSelector &, const UIID &, const int32) {}

UIControlSourceKey ConsoleListDataSource::GetSourceKeyForItem(const int32) const { return sRowKey("Console.ConsoleList.ConsoleItem"); }
void ConsoleListDataSource::InitializeItemUI(const UIConsoleListBox &, const UIID &, const int32) {}
void ConsoleListDataSource::UpdateItemUI(const UIConsoleListBox & tListBox, const UIID & idItemUI, const int32 nIndex)
{
    sSetRow(tListBox, idItemUI, nIndex, ".ConsoleName", m_MultipleTypeItems[nIndex].m_displayText, m_MultipleTypeItems[nIndex].bHasBeenSelected);
}

GameListDataSource::GameListDataSource() {}
D3_DESIGNER_ROWS(GameListDataSource, m_consoleCommandOptionItems, FALSE, "Game.GameList.GameItem", ".GameName")
void GameListDataSource::Initialize()
{
    sAdd(m_consoleCommandOptionItems, "Spawn Rime (ice Morlu)", "cheat spawn X1_LR_Boss_MorluSpellcaster_Ice boss");
    sAdd(m_consoleCommandOptionItems, "Spawn a champion pack", "cheat spawn X1_LR_Boss_MorluSpellcaster_Ice champion");
    sAdd(m_consoleCommandOptionItems, "Spawn a rare", "cheat spawn X1_LR_Boss_MorluSpellcaster_Ice rare");
    // Season themes (src/mods/theme.cpp): the first season of each, and off.
    if (D3ModThemeOn())
    {
        static LPCSTR const arThemes[][2] = {
            {"Theme: off", "theme off"},
            {"Theme: build's default", "theme default"},
            {"Theme 14: double goblins", "theme 14"},
            {"Theme 15: double bounty caches", "theme 15"},
            {"Theme 16: Royal Grandeur", "theme 16"},
            {"Theme 17: Legacy of Nightmares", "theme 17"},
            {"Theme 18: Triune's Will", "theme 18"},
            {"Theme 19: Pandemonium", "theme 19"},
            {"Theme 20: Kanai's powers", "theme 20"},
            {"Theme 21: Trials of the Tempests", "theme 21"},
            {"Theme 22: shadow clones, 4th cube slot", "theme 22"},
            {"Theme 24: ethereal items", "theme 24"},
            {"Theme 25: soul shards", "theme 25"},
            {"Theme 26: swarm rifts", "theme 26"},
            {"Theme 27: sanctified items", "theme 27"},
            {"Theme 28: Altar of Rites", "theme 28"},
            {"Theme 29: nesting portals", "theme 29"},
        };
        for (size_t i = 0; i < sizeof(arThemes) / sizeof(arThemes[0]); i++)
        {
            sAdd(m_consoleCommandOptionItems, arThemes[i][0], arThemes[i][1]);
        }
    }
}

QuickToggleListDataSource::QuickToggleListDataSource() {}
int32 QuickToggleListDataSource::GetNumItems() const { return m_consoleCommandOptionItems.size(); }
D3_DESIGNER_ROWS(QuickToggleListDataSource, m_consoleCommandOptionItems, TRUE, "QuickOptions.QuickOptionsToggleList.QuickOptionsToggleListItem", ".QuickOptionsToggleName")
void QuickToggleListDataSource::Initialize()
{
    sAdd(m_consoleCommandOptionItems, "Screenshot camera", "dev camera");
    sAdd(m_consoleCommandOptionItems, "Target names", "dev targetname");
    sAdd(m_consoleCommandOptionItems, "Controller view", "dev stickview");
}

QuickActionsListDataSource::QuickActionsListDataSource() {}
D3_DESIGNER_ROWS(QuickActionsListDataSource, m_consoleCommandOptionItems, FALSE, "QuickOptions.QuickOptionsActionsList.QuickOptionsActionsListItem", ".QuickOptionsActionsName")
void QuickActionsListDataSource::Initialize()
{
    sAdd(m_consoleCommandOptionItems, "Level 70", "cheat level 70");
    sAdd(m_consoleCommandOptionItems, "+100 Paragon", "cheat paragon 100");
    sAdd(m_consoleCommandOptionItems, "Unlock Kanai's Cube", "cheat cube");
}

MusicListDataSource::MusicListDataSource() {}
int32 MusicListDataSource::GetNumItems() const { return m_musicOptionItems.size(); }
D3_DESIGNER_ROWS(MusicListDataSource, m_musicOptionItems, FALSE, "Game.GameList.GameItem", ".GameName")
void MusicListDataSource::Initialize() {}

DesignerMenuGlobals::DesignerMenuGlobals()
    : m_eCurrentMenu(MENU_CHARACTER), m_consoleMenuSelectionId(0), m_gameMenuSelectionId(0), m_quickToggleMenuSelectionId(0),
      m_quickActionsMenuSelectionId(0), m_characterResetHeroMenuSelectionId(0), m_characterFollowersMenuSelectionId(0),
      m_characterBoostOptionsMenuSelectionId(0), m_musicMenuSelectionId(0), eCurrentQuestMenu(QuestMenus_Main), questDifficulty(0),
      questAct(0), bMusicChoosingCategory(FALSE), tStickFilter(::Console::GetDefaultStickInputFilter(::Console::FILTER_DIRECTIONAL)),
      bReseed(FALSE)
{
    listDataMainMenu.Initialize();
    listDataGameMenu.Initialize();
    listDataQuickToggleMenu.Initialize();
    listDataQuickActionsMenu.Initialize();
    listDataCharacterResetHeroMenu.Initialize();
    listDataCharacterFollowerMenu.Initialize();
    listDataCharacterBoostOptionsMenu.Initialize();
    listDataMusicMenu.Initialize();
    static LPCSTR const arConsole[][2] = {
        {"Screenshot camera", "dev camera"}, {"Target names", "dev targetname"}, {"Controller view", "dev stickview"},
        {"Gamma Lower", ""}, {"Gamma Higher", ""},
    };
    for (size_t i = 0; i < sizeof(arConsole) / sizeof(arConsole[0]); i++)
    {
        MultipleTypeOption tOption;
        tOption.m_displayText = arConsole[i][0];
        tOption.m_command = arConsole[i][1];
        listDataConsoleMenu.m_MultipleTypeItems.push_back(tOption);
    }
    m_selectedMenu = UIDLG_CONSOLEDESIGNERMENU_CHARACTER;
    m_currentMenu = UIDLG_CONSOLEDESIGNERMENU_MAINMENU;
}

DesignerMenuGlobals::~DesignerMenuGlobals() {}

// F8 (src/mods/dev.cpp): open the menu, or close it.
void D3ModToggle(int32 nControllerIndex)
{
    if (!sgptDesignerMenuGlobals)
    {
        sgptDesignerMenuGlobals = new DesignerMenuGlobals;
    }
    if (UI_GetWindowFromID(UIDLG_CONSOLEDESIGNERMENU).IsActive())
    {
        ::Console::Screens::CloseScreenSet(TRUE, FALSE);
        fprintf(stderr, "designer: closed\n");
    }
    else
    {
        ::Console::Screens::TryOpenToScreen(::Console::Screens::DESIGNER_MENU_SCREEN_SET, 0, nControllerIndex, FALSE);
        fprintf(stderr, "designer: opened\n");
    }
}

}  // namespace UIConsoleDesignerMenu
