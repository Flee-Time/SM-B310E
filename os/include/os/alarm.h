/* B310E OS — the alarm service (os/ui/alarms.c): up to ALARM_MAX alarms,
 * kept on the card, checked every second by the UI, rung by the OS itself
 * (the ringing screen, snooze), so they ring whatever app is open. The
 * Alarm app edits them through this interface.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_ALARM_H
#define OS_ALARM_H

#include <stdbool.h>
#include <stdint.h>

#define ALARM_MAX   8

extern bool alarm_ringing;
#define ALARM_ONCE  0                   /* days: no weekday set, rings once */

typedef struct {
    uint8_t on;
    uint8_t days;                       /* bit n: weekday n (0 Sunday .. 6 Saturday) */
    uint16_t minute;                    /* after midnight */
    char label[20];
} alarm_t;

int alarm_count(void);
bool alarm_get(int i, alarm_t *out);
/* Change alarm i, or add one with i == alarm_count(); its index, or -1. */
int alarm_set(int i, const alarm_t *a);
bool alarm_delete(int i);
/* The next alarm to ring from now: its index (-1: none on), the minutes
 * until it. */
int alarm_next(uint32_t *minutes_until);
/* "Mon Tue", "Every day", "Weekdays", "Once" */
void alarm_days_text(uint8_t days, char *out, int n);

void alarm_check(int h, int m);         /* the UI, once a second */

#endif
