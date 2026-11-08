/* Chocolate Doom, Heretic and Hexen on B310E OS: the game's video, keys,
 * timer and (no) joystick on the game kit (os/ports/kit): each 320 x 200
 * frame to the LCD as the app's view shows it (sideways at first,
 * stretched to 160 x 128: Doom's pixels were never square), the phone's
 * keys as the keyboard keys the game is bound to.
 *
 * Keys, with the phone sideways (the keypad to the right; the kit turns
 * the D-pad with the view, so Right points ahead):
 *   D-pad      move and turn, the menus
 *   Center     fire (in the menus: select, yes)
 *   LSoft, 5   use, open doors (in the menus: select, yes)
 *              In the menus these are a gamepad's fire button: the games
 *              then name an empty save slot themselves (the map's name)
 *              and take it for yes, as they do for a gamepad.
 *   RSoft      the menu (in the menus: back, no)
 *   Dial       run on/off
 *   1, 3       strafe left, right
 *   4, 6       previous, next weapon
 *   7, 9       quick save, quick load (LSoft says yes)
 *   0          the map
 *   *, #       Doom: zoom the map, the view's size;
 *              Heretic, Hexen: the inventory left, right
 *   2          Heretic, Hexen: use the artifact
 *   END        quit (asks first)
 *   * and # together: the OS's quick panel (volume ...), not the game's
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "d_event.h"
#include "deh_str.h"
#include "doomkeys.h"
#include "doomtype.h"
#include "i_input.h"
#include "i_joystick.h"
#include "i_system.h"
#include "i_timer.h"
#include "i_video.h"
#include "m_argv.h"
#include "m_config.h"
#include "m_controls.h"
#include "tables.h"
#include "v_diskicon.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"

#include "kit.h"

#if defined(GAME_HERETIC) || defined(GAME_HEXEN)
extern boolean MenuActive, askforquit;  /* askforquit: a yes/no question up */
#define MENU_ACTIVE (MenuActive || askforquit)
#define INVENTORY 1
#else
extern boolean menuactive;
#define MENU_ACTIVE menuactive
#define INVENTORY 0
#endif

/* ---- what upstream's i_video.c and i_input.c define ---------------------------- */
char *video_driver = "";
boolean screenvisible = true;
int vanilla_keyboard_mapping = true;
boolean screensaver_mode = false;
int usegamma = 0;
pixel_t *I_VideoBuffer = NULL;
int screen_width = SCREENWIDTH;
int screen_height = SCREENHEIGHT;
int fullscreen = true;
int aspect_ratio_correct = 0;
int integer_scaling = 0;
int smooth_pixel_scaling = 0;
int vga_porch_flash = 0;
int force_software_renderer = 1;
int png_screenshots = 0;
char *window_position = "";
unsigned int joywait = 0;
int usemouse = 0;
float mouse_acceleration = 1.0f;
int mouse_threshold = 0;

static boolean initialized;
static byte palette[256 * 3];           /* the current one, gamma applied */

/* ---- video ------------------------------------------------------------------------ */
void I_InitGraphics(void)
{
    byte *doompal;

    I_VideoBuffer = malloc(SCREENWIDTH * SCREENHEIGHT);
    if (!I_VideoBuffer)
        I_Error("No memory for the frame");
    memset(I_VideoBuffer, 0, SCREENWIDTH * SCREENHEIGHT);
    V_RestoreBuffer();
    doompal = W_CacheLumpName(DEH_String("PLAYPAL"), PU_CACHE);
    I_SetPalette(doompal);

    /* the phone's bindings (the configuration may say otherwise: these
     * are what the keypad sends) */
    key_menu_confirm = KEY_ENTER;
    key_menu_abort = KEY_ESCAPE;
    key_prevweapon = ';';
    key_nextweapon = '\'';
    key_use = ' ';
    key_fire = KEY_RCTRL;
    key_speed = KEY_RSHIFT;
    key_strafeleft = ',';
    key_straferight = '.';
    key_menu_qsave = KEY_F6;
    key_menu_qload = KEY_F9;
    key_menu_quit = KEY_F10;
    key_map_toggle = KEY_TAB;
    initialized = true;
}

void I_ShutdownGraphics(void) { initialized = false; }

void I_SetPalette(byte *doompal)
{
    for (int i = 0; i < 256 * 3; i++)
        palette[i] = gammatable[usegamma][doompal[i]];
    kit_palette(palette, 256);
}

int I_GetPaletteIndex(int r, int g, int b)
{
    int best = 0, best_d = 1 << 30;

    for (int i = 0; i < 256; i++) {
        int dr = palette[3 * i] - r, dg = palette[3 * i + 1] - g, db = palette[3 * i + 2] - b;
        int d = dr * dr + dg * dg + db * db;

        if (d < best_d) {
            best_d = d;
            best = i;
            if (!d)
                break;
        }
    }
    return best;
}

void I_FinishUpdate(void)
{
    if (!initialized)
        return;
    V_DrawDiskIcon();
    kit_present8(I_VideoBuffer, SCREENWIDTH, SCREENHEIGHT, SCREENWIDTH);
    V_RestoreDiskBackground();
}

void I_ReadScreen(pixel_t *scr) { memcpy(scr, I_VideoBuffer, SCREENWIDTH * SCREENHEIGHT); }
void I_UpdateNoBlit(void) {}
void I_BeginRead(void) {}
void I_StartFrame(void) {}
void I_SetGrabMouseCallback(grabmouse_callback_t func) { (void)func; }
void I_DisplayFPSDots(boolean dots_on) { (void)dots_on; }
void I_SetWindowTitle(const char *title) { (void)title; }
void I_InitWindowTitle(void) {}
void I_RegisterWindowIcon(const unsigned int *icon, int width, int height) { (void)icon; (void)width; (void)height; }
void I_InitWindowIcon(void) {}
void I_CheckIsScreensaver(void) {}
void I_GraphicsCheckCommandLine(void) {}
void I_GetWindowPosition(int *x, int *y, int w, int h) { *x = *y = 0; (void)w; (void)h; }

void I_BindVideoVariables(void)
{
    M_BindIntVariable("usegamma", &usegamma);
}

void I_StartTextInput(int x1, int y1, int x2, int y2) { (void)x1; (void)y1; (void)x2; (void)y2; }
void I_StopTextInput(void) {}
void I_BindInputVariables(void) {}
void I_ReadMouse(void) {}

/* ---- keys ----------------------------------------------------------------------- */
static int translate(int code)
{
    switch (code) {
    case KIT_UP:     return KEY_UPARROW;
    case KIT_DOWN:   return KEY_DOWNARROW;
    case KIT_LEFT:   return KEY_LEFTARROW;
    case KIT_RIGHT:  return KEY_RIGHTARROW;
    case KIT_CENTER: return MENU_ACTIVE ? KEY_ENTER : KEY_RCTRL;
    case KIT_LSOFT:
    case KIT_0 + 5:  return MENU_ACTIVE ? KEY_ENTER : ' ';
    case KIT_RSOFT:  return KEY_ESCAPE;
    case KIT_DIAL:   return KEY_RSHIFT;
    case KIT_0 + 1:  return ',';
    case KIT_0 + 3:  return '.';
    case KIT_0 + 4:  return ';';
    case KIT_0 + 6:  return '\'';
    case KIT_0 + 7:  return KEY_F6;     /* quick save: a slot the first time */
    case KIT_0 + 9:  return KEY_F9;     /* quick load */
    case KIT_0:      return KEY_TAB;
    case KIT_STAR:   return INVENTORY ? '[' : KEY_MINUS;
    case KIT_HASH:   return INVENTORY ? ']' : KEY_EQUALS;
    case KIT_0 + 2:  return INVENTORY ? KEY_ENTER : 0;
    case KIT_END:    return KEY_F10;
    }
    return 0;
}

static void post(evtype_t type, int key)
{
    event_t ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.data1 = key;
    ev.data2 = type == ev_keydown ? key : 0;
    ev.data3 = type == ev_keydown ? key : 0;
    D_PostEvent(&ev);
}

static uint8_t sent[128];               /* what each key's press sent: its release too */
static boolean running;                 /* Dial: run on, run off */
#define SENT_FIRE 0xff                  /* the gamepad's fire button (in the menus) */

static void post_fire(boolean down)
{
    event_t ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = ev_joystick;
    ev.data1 = down ? 1 << joybfire : 0;
    D_PostEvent(&ev);
}

static void get_events(void)
{
    int code, key;
    bool down;

    if (kit_quit_asked())               /* the screen left: save and stop */
        I_Quit();
    while (kit_key(&code, &down)) {
        code &= 127;
        if (down) {
            if (MENU_ACTIVE && (code == KIT_CENTER || code == KIT_LSOFT || code == KIT_0 + 5)) {
                sent[code] = SENT_FIRE;
                post_fire(true);
                continue;
            }
            if (!(key = translate(code)))
                continue;
            sent[code] = (uint8_t)key;
            if (key == KEY_RSHIFT) {
                running = !running;
                post(running ? ev_keydown : ev_keyup, key);
                continue;
            }
            post(ev_keydown, key);
        } else {
            key = sent[code];
            sent[code] = 0;
            if (key == SENT_FIRE)
                post_fire(false);
            else if (key && key != KEY_RSHIFT)
                post(ev_keyup, key);
        }
    }
}

void I_StartTic(void)
{
    if (initialized)
        get_events();
}

/* ---- time ----------------------------------------------------------------------- */
static uint32_t basetime;

int I_GetTimeMS(void)
{
    uint32_t ms = kit_ms();

    if (!basetime)
        basetime = ms ? ms : 1;
    return (int)(ms - basetime);
}

int I_GetTime(void) { return (int)((int64_t)I_GetTimeMS() * TICRATE / 1000); }
void I_Sleep(int ms) { kit_sleep(ms > 0 ? (uint32_t)ms : 1); }
void I_WaitVBL(int count) { kit_sleep((uint32_t)count * 1000 / 70); }
void I_InitTimer(void) {}

/* ---- no joystick ------------------------------------------------------------------- */
int use_analog = 0;
int joystick_turn_sensitivity = 10;
int joystick_move_sensitivity = 10;
int joystick_look_sensitivity = 10;
void I_InitJoystick(void) {}
void I_ShutdownJoystick(void) {}
void I_UpdateJoystick(void) {}
void I_BindJoystickVariables(void) {}
