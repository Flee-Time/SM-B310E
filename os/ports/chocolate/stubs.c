/* Chocolate Doom, Heretic and Hexen on B310E OS: what the phone does not
 * have. No network (a single player), no CD audio, no ENDOOM, no
 * directory listings (autoload folders, music packs), no text-mode or
 * 640 x 480 start-up screens (the games fall back to their plain ones),
 * and the zone as big as the game's block allows (upstream's I_ZoneBase
 * asks for whole MiB from 16 down; the link wraps it).
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "i_cdmus.h"
#include "i_endoom.h"
#include "i_glob.h"
#include "i_system.h"
#include "m_argv.h"
#include "net_client.h"
#include "net_dedicated.h"
#include "net_gui.h"
#include "net_io.h"
#include "net_loop.h"
#include "net_query.h"
#include "net_server.h"

#include "kit.h"

/* ---- the zone: the largest block the game's malloc can get, less room for
 * what the game mallocs outside it (the WAD's directory, the frame, the
 * sound's copies) ---------------------------------------------------------------- */
#define OUTSIDE_ZONE (448 * 1024)

byte *__wrap_I_ZoneBase(int *size)
{
    size_t have = kit_heap_largest();
    int p = M_CheckParmWithArgs("-mb", 1);
    byte *zone;

    *size = have > OUTSIDE_ZONE + 1024 * 1024 ? (int)(have - OUTSIDE_ZONE) : (int)(have * 2 / 3);
    if (p > 0)
        *size = atoi(myargv[p + 1]) * 1024 * 1024;
    *size &= ~4095;
    while (!(zone = malloc(*size)) && *size > 256 * 1024)
        *size -= 64 * 1024;
    if (!zone)
        I_Error("No memory for the zone");
    printf("zone memory: %p, %d KB allocated for zone\n", (void *)zone, *size / 1024);
    return zone;
}

/* ---- no network: a game is a local one. Upstream's network core would
 * cost 0.5 MB of buffers (net_server.c's clients); its entry points are
 * these, as if no server and no peers were ever found ------------------------------- */
boolean drone = false;
boolean net_client_connected = false;
net_waitdata_t net_client_wait_data;
char *net_client_reject_reason = NULL;
net_module_t net_loop_client_module, net_loop_server_module, net_sdl_module;

void NET_Init(void) {}
void NET_BindVariables(void) {}
void NET_SV_Init(void) {}
void NET_SV_Run(void) {}
void NET_SV_Shutdown(void) {}
void NET_SV_AddModule(net_module_t *module) { (void)module; }
void NET_SV_RegisterWithMaster(void) {}
void NET_CL_Run(void) {}
boolean NET_CL_Connect(net_addr_t *addr, net_connect_data_t *data) { (void)addr; (void)data; return false; }
void NET_CL_Disconnect(void) {}
boolean NET_CL_GetSettings(net_gamesettings_t *settings) { (void)settings; return false; }
void NET_CL_StartGame(net_gamesettings_t *settings) { (void)settings; }
void NET_CL_SendTiccmd(ticcmd_t *ticcmd, int maketic) { (void)ticcmd; (void)maketic; }
void NET_ReferenceAddress(net_addr_t *addr) { (void)addr; }
void NET_ReleaseAddress(net_addr_t *addr) { (void)addr; }
char *NET_AddrToString(net_addr_t *addr) { (void)addr; return "none"; }
void NET_LANQuery(void) {}
void NET_MasterQuery(void) {}
void NET_QueryAddress(const char *addr) { (void)addr; }
net_addr_t *NET_FindLANServer(void) { return NULL; }
void NET_DedicatedServer(void) {}
void NET_WaitForLaunch(void) {}

/* ---- no CD audio (Heretic, Hexen) ---------------------------------------------------- */
int cd_Error = 1;
int I_CDMusInit(void) { return -1; }
int I_CDMusPlay(int track) { (void)track; return -1; }
int I_CDMusStop(void) { return -1; }
int I_CDMusResume(void) { return -1; }
int I_CDMusSetVolume(int volume) { (void)volume; return -1; }
int I_CDMusFirstTrack(void) { return -1; }
int I_CDMusLastTrack(void) { return -1; }
int I_CDMusTrackLength(int track) { (void)track; return -1; }
void I_CDMusPrintStartup(void) {}

/* ---- no ENDOOM, no directory listings ------------------------------------------------ */
void I_Endoom(byte *data) { (void)data; }

glob_t *I_StartGlob(const char *dir, const char *glob, int flags)
{
    (void)dir; (void)glob; (void)flags;
    return NULL;
}

glob_t *I_StartMultiGlob(const char *dir, int flags, const char *glob, ...)
{
    (void)dir; (void)flags; (void)glob;
    return NULL;
}

void I_EndGlob(glob_t *glob) { (void)glob; }
const char *I_NextGlob(glob_t *glob) { (void)glob; return NULL; }

/* ---- no text-mode start-up (Heretic): TXT_Init fails, so it is not used --------------- */
int TXT_Init(void) { return 0; }
void TXT_Shutdown(void) {}
unsigned char *TXT_GetScreenData(void) { static unsigned char screen[80 * 25 * 2]; return screen; }
void TXT_UpdateScreen(void) {}
void TXT_GotoXY(int x, int y) { (void)x; (void)y; }
void TXT_FGColor(int color) { (void)color; }
void TXT_BGColor(int color, int blinking) { (void)color; (void)blinking; }
void TXT_PutChar(int c) { (void)c; }
void TXT_Puts(const char *s) { (void)s; }
int TXT_GetChar(void) { return -1; }
void TXT_SetWindowTitle(const char *title) { (void)title; }

/* ---- no 640 x 480 start-up (Hexen): the mode does not set, so it is not used ---------- */
boolean I_SetVideoModeHR(void) { return false; }
void I_UnsetVideoModeHR(void) {}
void I_SetWindowTitleHR(const char *title) { (void)title; }
void I_ClearScreenHR(void) {}
void I_SlamBlockHR(int x, int y, int w, int h, const byte *src) { (void)x; (void)y; (void)w; (void)h; (void)src; }
void I_SlamHR(const byte *buffer) { (void)buffer; }
void I_InitPaletteHR(void) {}
void I_SetPaletteHR(const byte *palette) { (void)palette; }
void I_FadeToPaletteHR(const byte *palette) { (void)palette; }
void I_BlackPaletteHR(void) {}
boolean I_CheckAbortHR(void) { return false; }
