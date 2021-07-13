#include <vitasdk.h>
#include <taihen.h>
#include <string.h>
#include <stdio.h>
#include <vita2d.h>
#include "common.h"

#define StartY 20
#define TextColour RGBA8(0,255,0,255)
#define SelectColour RGBA8(0,247,255,255)
#define MaxSelections 2

void DebugScreen();
void TriggerSwap();
void WarningSreen();
void SelectScreen();
extern SceUID _vshKernelSearchModuleByName(const char *name, SceUInt64 *unk);

vita2d_pgf *pgf;

int moduleLoaded = 0;

char Selections[MaxSelections][400] = { "Debug Bluetooth", "Swap triggers and bumpers" };
void (*functions[MaxSelections])(void) = { &DebugScreen, &TriggerSwap };

void drawBuff()
{   
    int currentY = 20;
    if(!moduleLoaded)
    {
        vita2d_pgf_draw_text(pgf, (960/2) - 100, (544/2)-10, RGBA8(0,255,0,255), 1.0f, "Error X1Vita not found!");
        return;
    }
    else
    {
        vita2d_pgf_draw_text(pgf, 960 - 150, currentY, RGBA8(0,255,0,255), 1.0f, "X1Vita Found!");
    }
    int swapStatus = GetSwapStatus();
    char buff[0x12];
    GetBuff(0, buff);

    char outBuff[0x400];
    memset(outBuff, 0, 0x400);

    int currentX = 0;
	for (int i = 0; i < 0x12; i++)
    {
		sprintf(outBuff, "%02X", buff[i]);
        vita2d_pgf_draw_text(pgf, currentX, currentY, RGBA8(0,255,0,255), 1.0f, outBuff);
        currentX += 40;
    }
    vita2d_pgf_draw_text(pgf, currentX, currentY, RGBA8(0,255,0,255), 1.0f, outBuff);
    
    currentY += 25;
    currentX = 0;

    GetPortBuff(buff);
    for (int i = 0; i < CONTROLLER_COUNT; i++)
    {
        sprintf(outBuff, "%02X", buff[i]);
        vita2d_pgf_draw_text(pgf, currentX, currentY, RGBA8(0,255,0,255), 1.0f, outBuff);
        currentX += 40;
    }
    
    currentY += 25;
    int pid;
    int vid;
    GetPidVid(&pid, &vid);

    sprintf(outBuff, "Last connection attempt had PID: 0x%X VID: 0x%X\n", pid, vid);
    vita2d_pgf_draw_text(pgf, 0, currentY, SelectColour, 1.0f,outBuff);
    currentY += 25;
    if(swapStatus) vita2d_pgf_draw_text(pgf, 0, currentY, SelectColour, 1.0f, "Triggers and bumpers are swapped");
    else vita2d_pgf_draw_text(pgf, 0, currentY, SelectColour, 1.0f, "Triggers and bumpers are not swapped");
    currentY += 25;
    vita2d_pgf_draw_text(pgf, 0, currentY, TextColour, 1.0f, "Hold Select + Start + L + R to exit");
}

void TriggerSwap()
{
    vita2d_start_drawing();
    vita2d_clear_screen();

    if(GetSwapStatus())
    {
        vita2d_pgf_draw_text(pgf, 0, StartY, TextColour, 1.0f, "Triggers are no longer swapped");
        int swap = 0;
        SetSwapStatus(&swap);
    }
    else
    {
        vita2d_pgf_draw_text(pgf, 0, StartY, TextColour, 1.0f, "Triggers are now swapped");
        int swap = 1;
        SetSwapStatus(&swap);
    }

    vita2d_end_drawing();
    vita2d_swap_buffers();
    sceShellUtilUnlock(SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN_2);
    sceKernelDelayThread(3 * 1000000);
    sceShellUtilLock(SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN_2);
}

void DebugScreen()
{
    SceCtrlData buttons;
    do
    {
        sceCtrlPeekBufferPositive(0, &buttons, 1);
        vita2d_start_drawing();
        vita2d_clear_screen();
        drawBuff();
        vita2d_end_drawing();
        vita2d_swap_buffers();
    } while (((buttons.buttons & SCE_CTRL_START) && (buttons.buttons & SCE_CTRL_SELECT) && (buttons.buttons & SCE_CTRL_LTRIGGER) && (buttons.buttons & SCE_CTRL_RTRIGGER)) == 0);
    SelectScreen();
    sceKernelDelayThread(20000);
}

void PrintSelection(int selection)
{
    int printY = StartY;
    vita2d_pgf_draw_text(pgf, 0, printY, TextColour, 1.0f, "Thank you for using X1Vita! Press Start to exit");
    printY += StartY + 10;
    for (int i = 0; i < MaxSelections; i++)
    {
        if(selection == i) vita2d_pgf_draw_text(pgf, 0, printY, SelectColour, 1.0f, Selections[i]);
        else vita2d_pgf_draw_text(pgf, 0, printY, TextColour, 1.0f, Selections[i]);
        printY += StartY;
    }
    printY += StartY + 10;
    vita2d_pgf_draw_text(pgf, 0, printY, TextColour, 1.0f, "If you have any issues you can contact me via discord: M Ibrahim#0197");
}

void WarningScreen()
{
    sceShellUtilUnlock(SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN_2);
    while (1)
    {
        vita2d_start_drawing();
        vita2d_clear_screen();
        vita2d_pgf_draw_text(pgf, (960/2) - 100, (544/2)-10, RGBA8(0,255,0,255), 1.0f, "Error X1Vita not found!");
        vita2d_end_drawing();
        vita2d_swap_buffers();
    }
}

void SelectScreen()
{
    SceCtrlData pad;
    int CurrentSelection = 0;
    do
    {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        vita2d_start_drawing();
        vita2d_clear_screen();
        PrintSelection(CurrentSelection);
        vita2d_end_drawing();
        vita2d_swap_buffers();

        if(pad.buttons & SCE_CTRL_UP)
        {
            if(CurrentSelection > 0) 
            {
                CurrentSelection--;
                sceKernelDelayThread(200000);
            }
        }
        if(pad.buttons & SCE_CTRL_DOWN)
        {
            if(CurrentSelection < MaxSelections - 1) 
            {
                CurrentSelection++;
                sceKernelDelayThread(200000);
            }
        }
        if(pad.buttons & SCE_CTRL_CROSS)
        {
           functions[CurrentSelection]();
           sceKernelDelayThread(200000);
        }
    } while (!(pad.buttons & SCE_CTRL_START));
    
}

int main()
{
    sceShellUtilInitEvents(0);
    sceShellUtilLock(SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN_2);

    SceUInt64 searchBuff = 0;
    moduleLoaded = (_vshKernelSearchModuleByName("X1Vita", &searchBuff) >= 0);

    vita2d_init();
    vita2d_set_clear_color(RGBA8(0x40, 0x40, 0x40, 0xFF));
    
    pgf = vita2d_load_default_pgf();

    if(!moduleLoaded) WarningScreen();
    else SelectScreen();

    vita2d_fini();
    vita2d_free_pgf(pgf);
    sceShellUtilUnlock(SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN_2);
    return sceKernelExitProcess(0);
}
