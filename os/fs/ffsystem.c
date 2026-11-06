/* B310E OS — FatFs memory and locking hooks on the kernel.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ff.h"
#include "os/kernel.h"

void *ff_memalloc(UINT msize)
{
    return kmalloc(msize);
}

void ff_memfree(void *mblock)
{
    kfree(mblock);
}

/* one per volume, plus FatFs's system lock at index FF_VOLUMES */
static mutex_t locks[FF_VOLUMES + 1];

int ff_mutex_create(int vol)
{
    mutex_init(&locks[vol]);
    return 1;
}

void ff_mutex_delete(int vol)
{
    (void)vol;
}

int ff_mutex_take(int vol)
{
    return mutex_lock(&locks[vol], FF_FS_TIMEOUT) == WAIT_OK;
}

void ff_mutex_give(int vol)
{
    mutex_unlock(&locks[vol]);
}
