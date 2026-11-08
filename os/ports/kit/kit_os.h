/* The game kit inside: between kit_sys.c (newlib's side: the system calls)
 * and kit_os.c (the OS's side, with the SDK's app.h). Plain C both ways.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIT_OS_H
#define KIT_OS_H

#include <stddef.h>
#include <stdint.h>

#define KOS_READ   1
#define KOS_WRITE  2
#define KOS_CREATE 4                    /* create, or truncate */
#define KOS_ALWAYS 8                    /* open, or create if it is not there */
#define KOS_NEW    16                   /* create; fail if it is there */

int kos_file_size(void);                /* bytes of the OS's open file */
int kos_open(void *f, const char *path, int mode);
int kos_read(void *f, void *buf, unsigned n, unsigned *got);
int kos_write(void *f, const void *buf, unsigned n, unsigned *put);
int kos_seek(void *f, unsigned long pos);
unsigned long kos_tell(void *f);
unsigned long kos_size(void *f);
int kos_sync(void *f);
int kos_close(void *f);
int kos_unlink(const char *path);
int kos_rename(const char *from, const char *to);
int kos_mkdir(const char *path);
int kos_stat(const char *path, unsigned long *size, int *is_dir, uint32_t *mtime);
uint32_t kos_time(void);
int kos_dir_size(void);                 /* bytes of the OS's open directory */
int kos_opendir(void *d, const char *path);
int kos_readdir(void *d, char *name, int n, int *is_dir);   /* 0, or -1 at the end */
void kos_closedir(void *d);                /* seconds since 1970, the RTC's */

void *kos_arena(size_t *size);          /* the game's block */
void kos_error(const char *text);       /* why the game stopped: shown */

int kit_run(int argc, char **argv);     /* kit_main(), exit() back here */
unsigned kit_io_kb(void);               /* read from the card so far */
unsigned kit_io_ms(void);               /* and how long that took */
void kit_close_files(void);             /* the game was ended: what it had open */
void kit_io_report(const char *when);   /* the card reads so far, to the log */

#endif
