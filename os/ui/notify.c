/* B310E OS — notifications: a list of the posters' own structs, newest
 * first. Every call is from the UI thread (apps run there; OS services
 * post from their UI-loop hooks), so the list needs no lock.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/app.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/notify.h"
#include "os/ui.h"

static notify_t *head;
static uint32_t seq;

static void unlink(notify_t *n)
{
    for (notify_t **pp = &head; *pp; pp = &(*pp)->next)
        if (*pp == n) {
            *pp = n->next;
            n->next = NULL;
            return;
        }
}

static bool posted(const notify_t *n)
{
    for (notify_t *p = head; p; p = p->next)
        if (p == n)
            return true;
    return false;
}

bool notify_post(notify_t *n)
{
    if (!n)
        return false;
    if (posted(n)) {
        unlink(n);                      /* to the top again */
    } else {
        if (notify_count() >= NOTIFY_MAX)
            return false;
        n->owner = app_self();
        klog("notify: \"%s\" from %s", n->title, n->owner ? app_name(n->owner) : "the OS");
    }
    n->seq = ++seq;
    n->next = head;
    head = n;
    ui_redraw();                        /* its icon in the status bar */
    return true;
}

void notify_remove(notify_t *n)
{
    if (!posted(n))
        return;
    unlink(n);
    if (n->owner)
        ui_collect_apps();              /* its app may go now */
    ui_redraw();
}

int notify_count(void)
{
    int c = 0;

    for (notify_t *p = head; p; p = p->next)
        c++;
    return c;
}

notify_t *notify_get(int i)
{
    notify_t *p = head;

    while (p && i-- > 0)
        p = p->next;
    return p;
}

static bool call_update(notify_t *n)
{
    app_t *saved = app_current;
    bool keep;

    if (!n->update)
        return true;
    app_current = n->owner;
    keep = n->update(n);
    app_current = saved;
    return keep;
}

void notify_tick(void)
{
    notify_t *p = head, *next;

    for (; p; p = next) {
        next = p->next;
        if (!call_update(p))
            notify_remove(p);
    }
}

void notify_run(notify_t *n, int action)
{
    app_t *saved = app_current;

    if (!posted(n) || !n->act || action < 0 || action >= NOTIFY_ACTIONS || !n->action[action])
        return;
    klog("notify: \"%s\": %s", n->title, n->action[action]);
    app_current = n->owner;
    n->act(n, action);
    app_current = saved;
    if (posted(n) && !call_update(n))
        notify_remove(n);
    ui_redraw();
}

bool notify_owned(const void *app)
{
    for (notify_t *p = head; p; p = p->next)
        if (p->owner == app)
            return true;
    return false;
}

void notify_drop_owner(const void *app)
{
    notify_t *p = head, *next;

    for (; p; p = next) {
        next = p->next;
        if (p->owner == app)
            unlink(p);
    }
}
