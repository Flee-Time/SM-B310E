/* Host test: what test_db_glue.c (os/fs/db.c's unit) gives test_db.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef TEST_DB_H
#define TEST_DB_H

#include <stdbool.h>

void dbt_ram_lost(void);                /* db.c's statics cleared (a reboot) */
bool dbt_has_file(void);                /* ready, with its file, the card there */
bool dbt_file(void);                    /* db.file: not in RAM-only mode */
bool dbt_unwritten(void);               /* settings or records still in RAM only */
bool dbt_kv_dirty(void);
void dbt_break_ram(void);               /* HOST_PROVE: RAM changed behind the card's back */
const char *dbt_new_path(void);         /* DB_NEW */
const char *dbt_bad_path(void);         /* DB_BAD */

#endif
