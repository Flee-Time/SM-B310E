/* B310E boot loader — shared constants.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef BOOT_H
#define BOOT_H

#define BOOT_VERSION     0x00010014     /* 1.20 */
#define BOOT_VERSION_STR "v1.20"
#define BOOT_TAIL_MAGIC  0x4c494154     /* "TAIL": boot.ld, start.S */
#define SFC_SIZE_REG     0x20a00200     /* the NOR size the windows decode, MiB */
#define STOCK_ENTRY      0x000046e4     /* the stock PBL (NOR word 0x20 before) */
#define NOR_BASE_ADDR    0x003fc000     /* where the install writes the loader */
#define EARLY_STACK_TOP  0x40009000     /* sdboot's stack: the no-key path */

/* Header (start.S): +0x18 the default with no key and no request. */
#define MODE_STOCK       0              /* the stock firmware (as sdboot) */
#define MODE_MENU        1              /* the menu, on an END power-on */
#define MODE_PROGRAM     2              /* the program at +0x20, else the menu */
#define DEFAULT_MODE     MODE_MENU
#define HDR_PATH_LEN     48
#define DEFAULT_PROGRAM  "payloads/b310e-os.bin"

/* A request across a reset: the high byte of analog 0x82001148. On the
 * phone that register keeps its value over the watchdog reset (2026-11-09:
 * 0xb2 to B310E OS with the cable in) while 0x82001140 and 0x82001144 read
 * 0 after a reset; the stock's restart mode is its low five bits and it does not
 * read the high byte (NOR 0x1a630, 0x1a686). Ours are 0xb0-0xb3; the loader
 * clears one it takes and leaves any other value.
 * The stock's reset-mode byte, APB 0x8b000228 bits 15:8, does not carry a
 * request: it reads 0x9a after a watchdog reset whatever was written. It
 * only tells that this was a reset (early_request's raw). */
#define REQ_REG          0x82001148u
/* Its low five bits are the stock's restart mode. END on its charging screen
 * restarts with mode 8 (keypad handler NOR 0x445a0, 0x1818a), END possibly
 * already let go: read here as a power-on by the user. */
#define STOCK_MODE_MASK  0x1fu
#define STOCK_MODE_START 8u
#define HWRST_REG        0x8b000228
#define REQ_STOCK        0xb0           /* the stock firmware, once */
#define REQ_MENU         0xb1           /* the menu */
#define REQ_OS           0xb2           /* payloads/b310e-os.bin */
#define REQ_PATH         0xb3           /* the program named in PSRAM below */

/* REQ_PATH: a block near the top of PSRAM (in the B310E OS's MMU table,
 * which is dead once it resets). It must reach PSRAM before the reset:
 * written uncached, or cleaned from the D-cache. */
#define REQ_BLOCK        0x043fff00
#define REQ_MAGIC        0x51523342     /* "B3RQ" */
#define REQ_PATH_LEN     56             /* NUL-terminated */
/* block: magic, path[56], check = ~(magic + sum of the 14 path words) */

#define EARLY_STAY       0x02           /* boot_early() flags */
#define EARLY_END        0x04           /* END held: a power-on by the user */
#define AUTOBOOT_FILE    "autoboot.txt" /* in the payload folder */

/* A word the early code fills before .bss exists (above __bss_end, below
 * the stack; boot.ld asserts the room): the EIC mask at power-on. */
#define EARLY_SAVE_DMSK  0x40003ffc
/* The stock's reset status (boot_mode.c): bits 0-1 the charger, bit 2 set
 * until the backlight is switched on (its LCD init only initialises the
 * panel while it is set), bit 7 the reset pin. */
#define ANA_RST_STS      0x82001320u

/* Code and data the no-key path uses: kept below NOR 4 MiB (boot.ld). */
#define EARLY __attribute__((section(".early.text")))

#define PAYLOAD_DIR      "payloads"     /* raw images run at LOAD_ADDR */
#define LEGACY_DIR       "progs"        /* the fpdoom-era name, until renamed */
#define OS_NAME          "b310e-os.bin"
#define LOAD_ADDR        0x04000000     /* PSRAM base: programs run here */
#define LOAD_MAX         0x003f0000     /* up to the frame buffer */
#define FRAME_ADDR       0x043f2000     /* as B310E OS: 128 x 160 RGB565 */

#ifndef __ASSEMBLER__
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define REG32(a) (*(volatile uint32_t *)(a))
#define REG16(a) (*(volatile uint16_t *)(a))

typedef struct {
    uint32_t branch, stock_entry;
    char magic[8];
    uint32_t size, version, mode, reserved;
    char program[HDR_PATH_LEN];
    uint32_t crc, spare[3];
} boot_header_t;

extern const boot_header_t _start;

/* What the loader saw and did, for the QEMU tests and a look over USB. */
typedef struct {
    uint32_t magic;                     /* BOOT_STATUS_MAGIC */
    uint32_t stage;                     /* STAGE_* */
    uint32_t early;                     /* boot_early()'s result */
    uint32_t request;                   /* the request taken, or 0 */
    uint32_t sd_code;                   /* sdcard_init(), 0 ok */
    uint32_t fs_code;                   /* f_mount(), 0 ok */
    uint32_t items;                     /* menu entries */
    uint32_t sel;
    uint32_t pinmap;                    /* pairs written, 0 none */
    uint32_t loaded;                    /* bytes of the program */
    uint32_t keys;                      /* key presses seen */
    uint32_t panel;                     /* LCD panel id */
    uint32_t countdown;                 /* autoboot seconds left, -1 none */
    char message[32];                   /* the menu's status line */
} boot_status_t;

#define BOOT_STATUS_MAGIC 0x53423342    /* "B3BS" */
enum { STAGE_INIT = 1, STAGE_CARD, STAGE_MENU, STAGE_LOAD, STAGE_ENTER, STAGE_STOCK };
/* keypad_poll() returns the stock key codes (B310E OS drivers.h KEY_*) */
extern volatile boot_status_t boot_status;

/* start.S */
void boot_enter(uint32_t addr) __attribute__((noreturn));
void boot_chain_stock(uint32_t entry) __attribute__((noreturn));
void boot_main(uint32_t early) __attribute__((noreturn));
uint32_t boot_early(void);
void early_signal(int n);

/* hw.c */
void delay(uint32_t n);
uint32_t ms_now(void);
void ms_wait(uint32_t ms);
uint32_t adi_read(uint32_t addr);
bool adi_write(uint32_t addr, uint32_t value);
void chip_init(void);
void chip_save(void);                   /* the power-on clock and PSRAM setup */
void stock_jump(void) __attribute__((noreturn));
int pinmap_apply(void);
void backlight_on(int percent);
void backlight_off_stock(void);
void watchdog_reset(void);
void power_off(void);                   /* returns only if it failed */
void keypad_open(void);
int keypad_poll(void);                  /* a pressed key's KEY_*, or 0 */
void keypad_wait_release(uint32_t ms);

/* lcd.c: the panel; drawing is B310E OS's ui/gfx.c into lcd_back, which
 * the link map puts on FRAME_ADDR (the LCDC's source) */
#define LCD_W 128
#define LCD_H 160
uint32_t lcd_start(void);               /* returns the panel id */
void lcd_flush(void);
void lcd_stop(void);                    /* the last frame out */
void lcd_off(void);                     /* before the stock: the last frame out */

/* sd.c */
int card_open(void);                    /* 0 ok, else the init stage */
int card_read(uint32_t lba, uint8_t *buf, unsigned count);
int card_write(uint32_t lba, const uint8_t *buf, unsigned count);
void card_close(void);

/* battery.c (B310E OS drivers/battery.c) */
enum { CHG_NONE, CHG_CHARGING, CHG_DONE };
int battery_mv(void);                   /* -1 unknown */
int battery_percent(int mv);            /* -1 unknown */
int charge_state(void);

/* lib.c */
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
char *strchr(const char *s, int c);
int strcasecmp_(const char *a, const char *b);
void str_copy(char *d, const char *s, size_t n);

#endif /* __ASSEMBLER__ */
#endif
