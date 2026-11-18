/* Host tests: a stand-in for FatFs's ff.h (os/tests/host). The OS's own
 * headers include "ff.h"; with -Ios/tests/host first on the include path they
 * get this one. The files live in RAM (host.c, memfs), with a write budget
 * that models a power cut.
 *
 * Only what the OS files under test use: the names and meanings follow
 * FatFs R0.15 (the values of the FR_ codes too, so the OS's logs read the
 * same), the structures are our own.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HOST_FF_H
#define HOST_FF_H

#include <stdint.h>

typedef unsigned int UINT;
typedef unsigned char BYTE;
typedef uint16_t WORD;
typedef uint32_t FSIZE_t;
typedef uint32_t LBA_t;
typedef char TCHAR;

typedef enum {
    FR_OK = 0, FR_DISK_ERR, FR_INT_ERR, FR_NOT_READY, FR_NO_FILE, FR_NO_PATH,
    FR_INVALID_NAME, FR_DENIED, FR_EXIST, FR_INVALID_OBJECT, FR_WRITE_PROTECTED,
    FR_INVALID_DRIVE, FR_NOT_ENABLED, FR_NO_FILESYSTEM, FR_MKFS_ABORTED, FR_TIMEOUT,
    FR_LOCKED, FR_NOT_ENOUGH_CORE, FR_TOO_MANY_OPEN_FILES, FR_INVALID_PARAMETER
} FRESULT;

#define FA_READ           0x01
#define FA_WRITE          0x02
#define FA_OPEN_EXISTING  0x00
#define FA_CREATE_NEW     0x04
#define FA_CREATE_ALWAYS  0x08
#define FA_OPEN_ALWAYS    0x10
#define FA_OPEN_APPEND    0x30

#define AM_RDO  0x01
#define AM_HID  0x02
#define AM_SYS  0x04
#define AM_DIR  0x10
#define AM_ARC  0x20

struct memfile;

typedef struct {
    struct memfile *node;               /* NULL: not open */
    FSIZE_t fptr;
    BYTE flag;
} FIL;

typedef struct {
    int index;
    const char *dir;
} DIR;

typedef struct {
    FSIZE_t fsize;
    WORD fdate, ftime;
    BYTE fattrib;
    TCHAR altname[13];
    TCHAR fname[256];
} FILINFO;

typedef struct {
    int unused;
} FATFS;

FSIZE_t memfs_fsize(const FIL *fp);
#define f_size(fp)  memfs_fsize(fp)
#define f_tell(fp)  ((fp)->fptr)
#define f_eof(fp)   ((fp)->fptr == f_size(fp))
#define f_error(fp) 0

FRESULT f_open(FIL *fp, const TCHAR *path, BYTE mode);
FRESULT f_close(FIL *fp);
FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br);
FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw);
FRESULT f_lseek(FIL *fp, FSIZE_t ofs);
FRESULT f_truncate(FIL *fp);
FRESULT f_sync(FIL *fp);
FRESULT f_stat(const TCHAR *path, FILINFO *fno);
FRESULT f_unlink(const TCHAR *path);
FRESULT f_rename(const TCHAR *path_old, const TCHAR *path_new);
FRESULT f_mkdir(const TCHAR *path);
FRESULT f_opendir(DIR *dp, const TCHAR *path);
FRESULT f_readdir(DIR *dp, FILINFO *fno);
FRESULT f_closedir(DIR *dp);

#endif
