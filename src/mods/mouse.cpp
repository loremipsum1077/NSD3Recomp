// The Mouse mod (D3_MOD_MOUSE): a mouse alongside the controller.
//
// The Switch build keeps some of the PC engine's mouse code, but neither its menus nor its
// click-to-move use it: the console UI sends every input to the control with controller
// focus and never hit-tests the pointer, and a click in the world resolves its target
// from the controller's picking. So the pointer here works the console's own controls:
//
//   in menus    hovering a list row selects it (and gives its list the focus), a left
//               click on it presses A; a left click on a button presses the pad button
//               it answers to (a dialog's "{icon:button1} Accept" is A, button2 is B)
//   in game     holding the left button walks toward the pointer (the left stick is
//               tilted from the hero toward it); the right button holds A, the primary
//               skill, which also talks, opens and picks up
//
// The decomp's InputPlatformUpdate is patched to call D3ModMouseUpdate() once per input
// update, on the thread that runs the UI. D3_MOUSE_TRACE=1 prints what each click hit.
#include "X/Client/CGame.h"
#include "X/Client/Camera/CameraManager.h"
#include "X/Client/Input/Input.h"
#include "X/Client/UserInterface/Controls/UIButtonControl.h"
#include "X/Client/UserInterface/Controls/UIConsoleListboxControl.h"
#include "X/Client/UserInterface/Controls/UIControlControl.h"
#include "X/Client/UserInterface/Controls/UIWindowControl.h"
#include "X/Client/UserInterface/Interface/UIGame.h"
#include "X/Client/UserInterface/UI.h"
#include "X/Client/UserInterface/UIAttributes.h"
#include "X/Client/UserInterface/UIMessage.h"
#include "X/Client/UserInterface/UITypes.h"
#include "X/Client/Definitions/ControllerSchemeDefinition.h"
#include "X/Common/ActorCommonData.h"
#include "X/Common/Player.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

UI & UIGet();                          // X/Client/UserInterface/UI.cpp
const UIID & UI_GetControllerFocus();  // X/Client/UserInterface/UI.cpp
BOOL UI_SendMessage(const UIID & idRecipient, const UIMsg & msg, const uintptr dwParam);  // X/Client/UserInterface/UI.cpp

extern "C" {
// src/host/window.h
enum { D3_MOUSE_MOVE, D3_MOUSE_DOWN, D3_MOUSE_UP, D3_MOUSE_WHEEL };
enum { D3_MOUSE_LEFT, D3_MOUSE_MIDDLE, D3_MOUSE_RIGHT };
enum { D3_PAD_A = 0, D3_PAD_B, D3_PAD_X, D3_PAD_Y };
struct d3_mouse_event {
    int type;
    int button;
    int wheel;
    float x, y;
};
int d3_mouse_take(struct d3_mouse_event * out, int max);
void d3_pad_tap(int button, int milliseconds);
void d3_mouse_stick(int x, int y);
// src/gfx/metal/nvn_metal.mm; absent from builds without the Metal renderer.
__attribute__((weak)) void d3_present_size(int * width, int * height);
}

namespace {

const int kTapMilliseconds = 100;
const float kWalkDeadZonePixels = 24;  // closer to the hero than this, stand still

IVector2D gPoint(0, 0);
bool gWalking;    // left button held on the world
bool gAttacking;  // right button held on the world

bool Trace()
{
    static int trace = getenv("D3_MOUSE_TRACE") != NULL;
    return trace;
}

bool UIReady()
{
    return CGameIsValid() && CGameGet().ptUIGlobals != NULL && UIGet().pmapElements != NULL;
}

float Area(const Rect2D & rect)
{
    return (rect.right - rect.left) * (rect.bottom - rect.top);
}

// Most console menus are list boxes: the list has the focus and a selected row. Finds the
// visible row under the point, in the smallest list that has one there.
UIConsoleListBox * ListRowAt(const IVector2D & point, int32 & nRow)
{
    UIConsoleListBox * ptBest = NULL;
    POS pos = UIGet().pmapElements->GetStartAssoc();
    while (pos)
    {
        uint64 nID;
        UIControl * pControl;
        UIGet().pmapElements->GetNextAssoc(pos, nID, pControl);
        if (pControl == NULL || !pControl->IsType(UIT_CONSOLELISTBOX) || !pControl->IsActive())
        {
            continue;
        }
        UIConsoleListBox & tList = static_cast<UIConsoleListBox &>(*pControl);
        if (!tList.IsInside(point) || tList.GetFirstVisibleItemIndex() < 0 ||
            (ptBest && Area(tList.GetWindowRect()) >= Area(ptBest->GetWindowRect())))
        {
            continue;
        }
        for (int32 i = tList.GetFirstVisibleItemIndex(); i <= tList.GetLastVisibleItemIndex(); i++)
        {
            const UIID & idItem = tList.GetItemUIID(i);
            if (idItem == GetUIIDNone() || !UI_TestID(idItem))
            {
                continue;
            }
            UIControl & tItem = UI_GetControlFromID(idItem);
            if (tItem.IsType(UIT_WINDOW) && static_cast<UIWindow &>(tItem).IsInside(point))
            {
                ptBest = &tList;
                nRow = i;
                break;
            }
        }
    }
    return ptBest;
}

// Any other control that can take controller focus (the smallest one under the point).
UIWindow * FocusableAt(const IVector2D & point)
{
    UIWindow * ptBest = NULL;
    POS pos = UIGet().pmapElements->GetStartAssoc();
    while (pos)
    {
        uint64 nID;
        UIControl * pControl;
        UIGet().pmapElements->GetNextAssoc(pos, nID, pControl);
        if (pControl == NULL || !pControl->IsType(UIT_WINDOW) || pControl->IsType(UIT_CONSOLELISTBOX) ||
            !pControl->CanAcceptControllerFocus() || !pControl->IsActive())
        {
            continue;
        }
        UIWindow & tWindow = static_cast<UIWindow &>(*pControl);
        if (tWindow.IsInside(point) && (ptBest == NULL || Area(tWindow.GetWindowRect()) < Area(ptBest->GetWindowRect())))
        {
            ptBest = &tWindow;
        }
    }
    return ptBest;
}

UIButton * ButtonAt(const IVector2D & point)
{
    UIButton * ptBest = NULL;
    POS pos = UIGet().pmapElements->GetStartAssoc();
    while (pos)
    {
        uint64 nID;
        UIControl * pControl;
        UIGet().pmapElements->GetNextAssoc(pos, nID, pControl);
        if (pControl == NULL || !pControl->IsType(UIT_BUTTON) || !pControl->IsActive())
        {
            continue;
        }
        UIButton & tButton = static_cast<UIButton &>(*pControl);
        if (tButton.IsInside(point) && (ptBest == NULL || Area(tButton.GetWindowRect()) < Area(ptBest->GetWindowRect())))
        {
            ptBest = &tButton;
        }
    }
    return ptBest;
}

// UIButton keeps its click function protected; the console UI runs it only for a
// controller press.
struct ButtonClick : UIButton {
    static void (UIButton::*Function())(UIAttributeType, const BOOL) { return &ButtonClick::sRunOnClickFunction; }
};

// Presses a button the way the controller would. Returns whether it was a button.
bool PressButtonAt(const IVector2D & point)
{
    UIButton * ptButton = ButtonAt(point);
    if (ptButton == NULL)
    {
        return false;
    }
    if (Trace())
    {
        fprintf(stderr, "mouse: button \"%s\" action %d\n", ptButton->GetText().GetText(), ptButton->GetUIAction());
    }
    if (ptButton->GetUIAction() == -1)
    {
        // No controller action of its own. A dialog's buttons read "{icon:button1} Accept",
        // "{icon:button2} Cancel": the dialog listens for that pad button (1 to 4 are A, B,
        // X, Y), so press it. Otherwise run its click function, as a touch would.
        LPCSTR szText = ptButton->GetText().GetText();
        LPCSTR szIcon = szText ? strstr(szText, "{icon:button") : NULL;
        if (szIcon && szIcon[12] >= '1' && szIcon[12] <= '4' && (szIcon[13] < '0' || szIcon[13] > '9'))
        {
            d3_pad_tap(D3_PAD_A + (szIcon[12] - '1'), kTapMilliseconds);
        }
        else if (ptButton->IsEnabled() && (ptButton->m_pfnClickPredicate == NULL || ptButton->m_pfnClickPredicate(ptButton->GetID())))
        {
            (ptButton->*ButtonClick::Function())(ATT_ONCLICK, FALSE);
        }
        return true;
    }
    BufferedInput tPress;
    tPress.eInputMsg = INPUTMSG_CONTROLLERBUTTONDOWN;
    tPress.tBufControllerData.nControllerIndex = 0;
    for (InputButton eButton = 0; eButton < 18; eButton++)
    {
        tPress.tBufControllerData.eInputButton = eButton;
        if (Controller::ActionButtonMatch(ptButton->GetUIAction(), tPress))
        {
            UI_SendMessage(ptButton->GetID(), UI_INPUT, (uintptr)&tPress);
            break;
        }
    }
    return true;
}

// Moves the selection or focus to what is under the pointer. Returns whether there was
// something to select there.
bool Hover()
{
    int32 nRow = -1;
    if (UIConsoleListBox * ptList = ListRowAt(gPoint, nRow))
    {
        if (ptList->CanAcceptControllerFocus() && ptList->GetID() != UI_GetControllerFocus())
        {
            UI_SetControllerFocus(ptList->GetID());
        }
        ptList->SetSelectedIndex(nRow);
        return true;
    }
    if (UIWindow * ptControl = FocusableAt(gPoint))
    {
        if (ptControl->GetID() != UI_GetControllerFocus())
        {
            UI_SetControllerFocus(ptControl->GetID());
        }
        return true;
    }
    return false;
}

// A left click on the UI. Returns whether the UI took it.
bool ClickUI()
{
    if (PressButtonAt(gPoint))
    {
        return true;
    }
    if (Hover())
    {
        d3_pad_tap(D3_PAD_A, kTapMilliseconds);
        return true;
    }
    return false;
}

bool InWorld()
{
    return PlayerGetFirstInGame() != NULL && !UIGame::MenuIsActive() && !UIGame::ModalIsActive();
}

// Tilts the left stick from the hero toward the pointer, full when it is far.
void Walk()
{
    Player * ptPlayer = PlayerGetFirstInGame();
    ActorCommonData * ptACD = ptPlayer ? ACDTryToGet(ptPlayer->idPlayerACD) : NULL;
    Vector2D vecHero;
    if (!ptACD || !WorldPointToScreenPoint(ptACD->GetPlace().wp, vecHero, -1))
    {
        d3_mouse_stick(0, 0);
        return;
    }
    float flX = gPoint.x - vecHero.x;
    float flY = vecHero.y - gPoint.y;  // the stick's Y points up
    float flLength = sqrtf(flX * flX + flY * flY);
    if (flLength < kWalkDeadZonePixels)
    {
        d3_mouse_stick(0, 0);
        return;
    }
    d3_mouse_stick((int)(flX / flLength * 32767), (int)(flY / flLength * 32767));
}

}  // namespace

void D3ModMouseUpdate()
{
    int width = 0, height = 0;
    if (d3_present_size)
    {
        d3_present_size(&width, &height);
    }
    if (width <= 0 || height <= 0)
    {
        width = 1600;
        height = 900;
    }

    d3_mouse_event events[64];
    int count;
    while ((count = d3_mouse_take(events, 64)) > 0)
    {
        for (int i = 0; i < count; i++)
        {
            const d3_mouse_event & e = events[i];
            IVector2D point((int32)floorf(e.x * width), (int32)floorf(e.y * height));
            bool moved = point.x != gPoint.x || point.y != gPoint.y;
            gPoint = point;
            if (!UIReady())
            {
                continue;
            }
            switch (e.type)
            {
            case D3_MOUSE_MOVE:
                if (moved && !gWalking && !gAttacking)
                {
                    Hover();
                }
                break;
            case D3_MOUSE_DOWN:
                if (e.button == D3_MOUSE_LEFT)
                {
                    if (!ClickUI() && InWorld())
                    {
                        gWalking = true;
                    }
                }
                else if (e.button == D3_MOUSE_RIGHT && InWorld() && ButtonAt(gPoint) == NULL)
                {
                    gAttacking = true;
                }
                if (Trace())
                {
                    fprintf(stderr, "mouse: button %d down at %d,%d walking %d attacking %d\n", e.button, gPoint.x, gPoint.y, gWalking, gAttacking);
                }
                break;
            case D3_MOUSE_UP:
                if (e.button == D3_MOUSE_LEFT && gWalking)
                {
                    gWalking = false;
                    d3_mouse_stick(0, 0);
                }
                else if (e.button == D3_MOUSE_RIGHT)
                {
                    gAttacking = false;
                }
                break;
            }
        }
    }

    if (gWalking || gAttacking)
    {
        if (!UIReady() || !InWorld())
        {
            gWalking = gAttacking = false;
            d3_mouse_stick(0, 0);
            return;
        }
        if (gWalking)
        {
            Walk();
        }
        if (gAttacking)
        {
            d3_pad_tap(D3_PAD_A, kTapMilliseconds);  // renewed every update while held
        }
    }
}
