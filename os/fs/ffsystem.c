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
static bool made[FF_VOLUMES + 1];

/* Each f_mount creates the volume's lock again: initialise it once only,
 * a thread may still hold it or wait on it from before an unmount. */
int ff_mutex_create(int vol)
{
    if (!made[vol]) {
        mutex_init(&locks[vol]);
        made[vol] = true;
    }
    return 1;
}

/* fs_suspend: wait for the FatFs call in progress, keep others out while
 * unmounting (f_mount takes no lock itself). */
void ff_volume_lock(int vol)
{
    mutex_lock(&locks[vol], WAIT_FOREVER);
}

void ff_volume_unlock(int vol)
{
    mutex_unlock(&locks[vol]);
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
