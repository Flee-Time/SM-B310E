/* B310E OS — notifications (ui/notify.c) and the quick panel (ui/panel.c).
 *
 * Center held on Home or the Menu opens the quick panel: the notifications
 * apps and the OS posted (each with up to three buttons), then the
 * volumes, the brightness and the torch. A notification's icon is in the
 * status bar while it is posted.
 *
 * A notification is the poster's own struct, in its static data; the OS
 * keeps a pointer to it. An app that has one posted stays loaded, so its
 * callbacks stay valid with no screen of it open (the Music app's buttons
 * work while the music plays on), until it removes the notification or
 * its update() returns false; then the app is unloaded as usual.
 *
 *     static void act(notify_t *n, int i) { if (i == 1) media_queue_toggle(); }
 *     static bool update(notify_t *n)
 *     {
 *         snprintf(n->text, sizeof(n->text), "%s", media_queue_now());
 *         n->action[1] = media_state() == MEDIA_PAUSED ? "Play" : "Pause";
 *         return media_queue_active();    // false: it goes
 *     }
 *     static notify_t note = { .title = "Music", .action = { "<<", "Pause", ">>" },
 *                              .act = act, .update = update };
 *     ...
 *     notify_post(&note);
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_NOTIFY_H
#define OS_NOTIFY_H

#include <stdbool.h>
#include "os/ui.h"

#define NOTIFY_ACTIONS 3
#define NOTIFY_MAX     8

typedef struct notify notify_t;
struct notify {
    const icon_t *icon;                 /* NULL: a bell */
    char title[24];
    char text[40];                      /* the second line; "" none */
    const char *action[NOTIFY_ACTIONS]; /* button labels, NULL: no button */
    int progress;                       /* 1..1000: a bar; 0 none */
    void (*act)(notify_t *n, int action);   /* a button was pressed */
    bool (*update)(notify_t *n);        /* about once a second and before the
                                         * panel draws: refresh the texts;
                                         * false removes it. NULL: kept */
    void *ctx;                          /* the poster's */
    /* the OS's: */
    notify_t *next;
    void *owner;                        /* the app that posted it, NULL the OS */
    uint32_t seq;                       /* order of posting, newest first */
};

/* Add it (again: brought to the top). Returns false when NOTIFY_MAX are up. */
bool notify_post(notify_t *n);
void notify_remove(notify_t *n);
int notify_count(void);
notify_t *notify_get(int i);            /* newest first; NULL past the end */

/* the OS's */
void notify_tick(void);                 /* update() each, once a second */
bool notify_owned(const void *app);     /* the loader keeps such an app */
void notify_drop_owner(const void *app);    /* an app unloaded anyway */
void notify_run(notify_t *n, int action);   /* n->act as its app */
screen_t *ui_panel(void);               /* ui/panel.c */
void ui_panel_host(screen_t *s);        /* held Center there opens the panel */

#endif
