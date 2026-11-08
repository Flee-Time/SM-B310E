/* Wolf4SDL on B310E OS: no SDL. The part of SDL 2's interface Wolf4SDL
 * uses, implemented on the game kit (sdl_kit.c): 8-bit surfaces with a
 * palette (the screen too: Wolf4SDL's screenBits 8 path), the "texture"
 * the screen goes to as the kit's frame, the phone's keys as keyboard
 * scancode events, the time. Not SDL's code; its names and numbers.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WOLF_SDL_H
#define WOLF_SDL_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t Uint8;
typedef int8_t Sint8;
typedef uint16_t Uint16;
typedef int16_t Sint16;
typedef uint32_t Uint32;
typedef int32_t Sint32;
typedef enum { SDL_FALSE, SDL_TRUE } SDL_bool;

#define SDL_INIT_AUDIO    0x10u
#define SDL_INIT_VIDEO    0x20u
#define SDL_INIT_JOYSTICK 0x200u
int SDL_Init(Uint32 flags);
void SDL_Quit(void);
const char *SDL_GetError(void);
void SDL_Delay(Uint32 ms);
Uint32 SDL_GetTicks(void);
#define SDL_HINT_RENDER_SCALE_QUALITY "SDL_RENDER_SCALE_QUALITY"
SDL_bool SDL_SetHint(const char *name, const char *value);

#define SDL_MESSAGEBOX_ERROR       0x10u
#define SDL_MESSAGEBOX_INFORMATION 0x40u
int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, void *window);

/* ---- video ------------------------------------------------------------------------ */
typedef struct { Uint8 r, g, b, a; } SDL_Color;
#define SDL_ALPHA_OPAQUE 255
typedef struct { int ncolors; SDL_Color *colors; } SDL_Palette;
typedef struct { SDL_Palette *palette; Uint8 BitsPerPixel, BytesPerPixel; } SDL_PixelFormat;
typedef struct SDL_Surface {
    Uint32 flags;
    SDL_PixelFormat *format;
    int w, h, pitch;
    void *pixels;
    SDL_Palette pal;                    /* (the shim's) */
    SDL_PixelFormat fmt;
    SDL_Color colors[256];
} SDL_Surface;
typedef struct { int x, y, w, h; } SDL_Rect;
typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture SDL_Texture;

#define SDL_WINDOWPOS_UNDEFINED 0x1fff0000
#define SDL_WINDOW_FULLSCREEN 0x1u
#define SDL_WINDOW_OPENGL 0x2u
#define SDL_RENDERER_ACCELERATED 0x2u
#define SDL_RENDERER_PRESENTVSYNC 0x4u
#define SDL_PIXELFORMAT_ARGB8888 0x16362004u
#define SDL_TEXTUREACCESS_STREAMING 1
#define SDL_BLENDMODE_BLEND 1
#define SDL_MUSTLOCK(s) 0
#define SDL_DISABLE 0
#define SDL_IGNORE 0

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags);
void SDL_DestroyWindow(SDL_Window *w);
SDL_Renderer *SDL_CreateRenderer(SDL_Window *w, int index, Uint32 flags);
void SDL_DestroyRenderer(SDL_Renderer *r);
int SDL_SetRenderDrawBlendMode(SDL_Renderer *r, int mode);
SDL_Texture *SDL_CreateTexture(SDL_Renderer *r, Uint32 format, int access, int w, int h);
void SDL_DestroyTexture(SDL_Texture *t);
int SDL_UpdateTexture(SDL_Texture *t, const SDL_Rect *rect, const void *pixels, int pitch);
int SDL_RenderCopy(SDL_Renderer *r, SDL_Texture *t, const SDL_Rect *src, const SDL_Rect *dst);
void SDL_RenderPresent(SDL_Renderer *r);
SDL_bool SDL_PixelFormatEnumToMasks(Uint32 format, int *bpp, Uint32 *r, Uint32 *g, Uint32 *b,
                                    Uint32 *a);
SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int w, int h, int depth, Uint32 r, Uint32 g,
                                  Uint32 b, Uint32 a);
void SDL_FreeSurface(SDL_Surface *s);
int SDL_SetPaletteColors(SDL_Palette *p, const SDL_Color *colors, int first, int n);
int SDL_LockSurface(SDL_Surface *s);
void SDL_UnlockSurface(SDL_Surface *s);
int SDL_FillRect(SDL_Surface *s, const SDL_Rect *rect, Uint32 color);
int SDL_BlitSurface(SDL_Surface *src, const SDL_Rect *sr, SDL_Surface *dst, SDL_Rect *dr);
Uint32 SDL_MapRGBA(const SDL_PixelFormat *f, Uint8 r, Uint8 g, Uint8 b, Uint8 a);
int SDL_SaveBMP(SDL_Surface *s, const char *file);
int SDL_ShowCursor(int toggle);

/* ---- input ------------------------------------------------------------------------ */
typedef enum {
    SDL_SCANCODE_UNKNOWN = 0,
    SDL_SCANCODE_A = 4, SDL_SCANCODE_B, SDL_SCANCODE_C, SDL_SCANCODE_D, SDL_SCANCODE_E,
    SDL_SCANCODE_F, SDL_SCANCODE_G, SDL_SCANCODE_H, SDL_SCANCODE_I, SDL_SCANCODE_J,
    SDL_SCANCODE_K, SDL_SCANCODE_L, SDL_SCANCODE_M, SDL_SCANCODE_N, SDL_SCANCODE_O,
    SDL_SCANCODE_P, SDL_SCANCODE_Q, SDL_SCANCODE_R, SDL_SCANCODE_S, SDL_SCANCODE_T,
    SDL_SCANCODE_U, SDL_SCANCODE_V, SDL_SCANCODE_W, SDL_SCANCODE_X, SDL_SCANCODE_Y,
    SDL_SCANCODE_Z,
    SDL_SCANCODE_1 = 30, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5,
    SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8, SDL_SCANCODE_9, SDL_SCANCODE_0,
    SDL_SCANCODE_RETURN = 40, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_TAB,
    SDL_SCANCODE_SPACE, SDL_SCANCODE_MINUS, SDL_SCANCODE_EQUALS,
    SDL_SCANCODE_CAPSLOCK = 57,
    SDL_SCANCODE_F1 = 58, SDL_SCANCODE_F2, SDL_SCANCODE_F3, SDL_SCANCODE_F4, SDL_SCANCODE_F5,
    SDL_SCANCODE_F6, SDL_SCANCODE_F7, SDL_SCANCODE_F8, SDL_SCANCODE_F9, SDL_SCANCODE_F10,
    SDL_SCANCODE_F11, SDL_SCANCODE_F12,
    SDL_SCANCODE_SCROLLLOCK = 71, SDL_SCANCODE_PAUSE, SDL_SCANCODE_INSERT, SDL_SCANCODE_HOME,
    SDL_SCANCODE_PAGEUP, SDL_SCANCODE_DELETE, SDL_SCANCODE_END, SDL_SCANCODE_PAGEDOWN,
    SDL_SCANCODE_RIGHT, SDL_SCANCODE_LEFT, SDL_SCANCODE_DOWN, SDL_SCANCODE_UP,
    SDL_SCANCODE_KP_ENTER = 88, SDL_SCANCODE_KP_1, SDL_SCANCODE_KP_2, SDL_SCANCODE_KP_3,
    SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_5, SDL_SCANCODE_KP_6, SDL_SCANCODE_KP_7,
    SDL_SCANCODE_KP_8,
    SDL_SCANCODE_LCTRL = 224, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_LALT, SDL_SCANCODE_LGUI,
    SDL_SCANCODE_RCTRL, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RALT,
    SDL_NUM_SCANCODES = 512
} SDL_Scancode;
typedef int32_t SDL_Keycode;
#define KMOD_NUM 0x1000

enum {
    SDL_QUIT = 0x100, SDL_KEYDOWN = 0x300, SDL_KEYUP, SDL_TEXTINPUT = 0x303,
    SDL_MOUSEMOTION = 0x400, SDL_JOYBUTTONDOWN = 0x603, SDL_JOYBUTTONUP,
};
#define SDL_TEXTINPUTEVENT_TEXT_SIZE 32
typedef struct { SDL_Scancode scancode; SDL_Keycode sym; Uint16 mod; } SDL_Keysym;
typedef struct { Uint32 type; Uint8 state, repeat; SDL_Keysym keysym; } SDL_KeyboardEvent;
typedef struct { Uint32 type; char text[SDL_TEXTINPUTEVENT_TEXT_SIZE]; } SDL_TextInputEvent;
typedef struct { Uint32 type; Uint8 button; } SDL_JoyButtonEvent;
typedef union SDL_Event {
    Uint32 type;
    SDL_KeyboardEvent key;
    SDL_TextInputEvent text;
    SDL_JoyButtonEvent jbutton;
} SDL_Event;

int SDL_PollEvent(SDL_Event *e);
int SDL_WaitEvent(SDL_Event *e);
Uint8 SDL_EventState(Uint32 type, int state);
int SDL_GetModState(void);
#define SDL_BUTTON(x) (1u << ((x) - 1))
#define SDL_BUTTON_LEFT 1
#define SDL_BUTTON_MIDDLE 2
#define SDL_BUTTON_RIGHT 3
Uint32 SDL_GetMouseState(int *x, int *y);
Uint32 SDL_GetRelativeMouseState(int *x, int *y);
int SDL_SetRelativeMouseMode(int on);
void SDL_SetWindowGrab(SDL_Window *w, int on);
void SDL_WarpMouseInWindow(SDL_Window *w, int x, int y);

typedef struct SDL_Joystick SDL_Joystick;
#define SDL_HAT_UP 1
#define SDL_HAT_RIGHT 2
#define SDL_HAT_DOWN 4
#define SDL_HAT_LEFT 8
int SDL_NumJoysticks(void);
SDL_Joystick *SDL_JoystickOpen(int index);
void SDL_JoystickClose(SDL_Joystick *j);
void SDL_JoystickUpdate(void);
int SDL_JoystickNumButtons(SDL_Joystick *j);
int SDL_JoystickNumHats(SDL_Joystick *j);
Sint16 SDL_JoystickGetAxis(SDL_Joystick *j, int axis);
Uint8 SDL_JoystickGetHat(SDL_Joystick *j, int hat);
Uint8 SDL_JoystickGetButton(SDL_Joystick *j, int button);

/* ---- audio (SDL_mixer.h has the mixer) ---------------------------------------------- */
#define AUDIO_S16 0x8010
#define SDL_AUDIO_ALLOW_FREQUENCY_CHANGE 1
typedef struct SDL_RWops { const Uint8 *data; size_t size; } SDL_RWops;
SDL_RWops *SDL_RWFromMem(void *mem, int size);

#ifdef __cplusplus
}
#endif

#endif
