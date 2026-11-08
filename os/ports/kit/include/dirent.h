/* The game kit's <dirent.h> (newlib has none for this target): opendir,
 * readdir, closedir over the card's FatFs (kit_sys.c). Relative paths are
 * in the game's folder.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KIT_DIRENT_H
#define KIT_DIRENT_H

#include <sys/types.h>

#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8

struct dirent {
    ino_t d_ino;
    unsigned char d_type;
    char d_name[256];
};

typedef struct kit_dir DIR;

DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);

#endif
