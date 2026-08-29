/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2009 Bob Cousins
 * Copyright (C) 2026 by B310E-OS project
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

/*
 * B310E-OS Rockbox port — b310e/apps/keymaps/keymap-b310e.c
 * (GPLv2, Rockbox-derived; modeled on Rockbox's keymap-mini2440.c /
 * keymap-echor1.c)
 *
 * B310E keypad (see button-target.h for the physical layout):
 *   d-pad = directions; CENTER = SELECT/OK; DIAL = PLAY;
 *   LSOFT = MENU; RSOFT = BACK; END = POWER;
 *   2/8 = VOL_UP/VOL_DOWN; 4/6 = PREV/NEXT; STAR = HOME (quickscreen);
 *   HASH = HASH (hotkey); digits 0/1/3/5/7/9 for the keyboard.
 */

#include "config.h"
#include "action.h"
#include "button.h"
#include "settings.h"

/* {Action Code, Button code, Prereq button code} */

static const struct button_mapping button_context_standard[] = {
    {ACTION_STD_PREV,           BUTTON_UP,                  BUTTON_NONE},
    {ACTION_STD_PREVREPEAT,     BUTTON_UP|BUTTON_REPEAT,    BUTTON_NONE},
    {ACTION_STD_NEXT,           BUTTON_DOWN,                BUTTON_NONE},
    {ACTION_STD_NEXTREPEAT,     BUTTON_DOWN|BUTTON_REPEAT,  BUTTON_NONE},

    {ACTION_STD_CANCEL,         BUTTON_BACK,                BUTTON_NONE},
    {ACTION_STD_CANCEL,         BUTTON_POWER,               BUTTON_NONE},

    {ACTION_STD_CONTEXT,        BUTTON_SELECT|BUTTON_REPEAT, BUTTON_SELECT},

    {ACTION_STD_QUICKSCREEN,    BUTTON_HOME|BUTTON_REPEAT,  BUTTON_HOME},
    {ACTION_STD_MENU,           BUTTON_MENU|BUTTON_REL,     BUTTON_MENU},

    {ACTION_STD_OK,             BUTTON_SELECT|BUTTON_REL,   BUTTON_SELECT},
    {ACTION_STD_OK,             BUTTON_RIGHT,               BUTTON_NONE},

    LAST_ITEM_IN_LIST
}; /* button_context_standard */

static const struct button_mapping button_context_wps[] = {
    {ACTION_WPS_PLAY,           BUTTON_PLAY|BUTTON_REL,     BUTTON_PLAY},
    {ACTION_WPS_STOP,           BUTTON_POWER|BUTTON_REL,    BUTTON_POWER},

    {ACTION_WPS_SKIPNEXT,       BUTTON_NEXT|BUTTON_REL,     BUTTON_NEXT},
    {ACTION_WPS_SKIPPREV,       BUTTON_PREV|BUTTON_REL,     BUTTON_PREV},

    {ACTION_WPS_SEEKBACK,       BUTTON_LEFT|BUTTON_REPEAT,  BUTTON_NONE},
    {ACTION_WPS_SEEKFWD,        BUTTON_RIGHT|BUTTON_REPEAT, BUTTON_NONE},
    {ACTION_WPS_STOPSEEK,       BUTTON_LEFT|BUTTON_REL,     BUTTON_LEFT|BUTTON_REPEAT},
    {ACTION_WPS_STOPSEEK,       BUTTON_RIGHT|BUTTON_REL,    BUTTON_RIGHT|BUTTON_REPEAT},

    {ACTION_WPS_VOLDOWN,        BUTTON_VOL_DOWN|BUTTON_REPEAT, BUTTON_NONE},
    {ACTION_WPS_VOLDOWN,        BUTTON_VOL_DOWN,            BUTTON_NONE},
    {ACTION_WPS_VOLUP,          BUTTON_VOL_UP|BUTTON_REPEAT,   BUTTON_NONE},
    {ACTION_WPS_VOLUP,          BUTTON_VOL_UP,              BUTTON_NONE},

    {ACTION_WPS_QUICKSCREEN,    BUTTON_HOME|BUTTON_REPEAT,  BUTTON_HOME},
    {ACTION_WPS_MENU,           BUTTON_MENU|BUTTON_REL,     BUTTON_MENU},
    {ACTION_WPS_CONTEXT,        BUTTON_SELECT|BUTTON_REPEAT, BUTTON_SELECT},

    {ACTION_WPS_HOTKEY,         BUTTON_HOME|BUTTON_MENU,    BUTTON_NONE},
    {ACTION_WPS_BROWSE,         BUTTON_SELECT|BUTTON_REL,   BUTTON_SELECT},

    LAST_ITEM_IN_LIST
}; /* button_context_wps */

static const struct button_mapping button_context_list[] = {
    {ACTION_LISTTREE_PGUP,      BUTTON_HOME|BUTTON_UP,          BUTTON_NONE},
    {ACTION_LISTTREE_PGUP,      BUTTON_HOME|BUTTON_UP|BUTTON_REPEAT, BUTTON_NONE},
    {ACTION_LISTTREE_PGDOWN,    BUTTON_HOME|BUTTON_DOWN,        BUTTON_NONE},
    {ACTION_LISTTREE_PGDOWN,    BUTTON_HOME|BUTTON_DOWN|BUTTON_REPEAT, BUTTON_NONE},
#ifdef HAVE_VOLUME_IN_LIST
    {ACTION_LIST_VOLUP,         BUTTON_VOL_UP|BUTTON_REPEAT,    BUTTON_NONE},
    {ACTION_LIST_VOLUP,         BUTTON_VOL_UP,                  BUTTON_NONE},
    {ACTION_LIST_VOLDOWN,       BUTTON_VOL_DOWN,                BUTTON_NONE},
    {ACTION_LIST_VOLDOWN,       BUTTON_VOL_DOWN|BUTTON_REPEAT,  BUTTON_NONE},
#endif

    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_STD)
}; /* button_context_list */

static const struct button_mapping button_context_tree[] = {
    {ACTION_TREE_WPS,           BUTTON_PLAY|BUTTON_REL,     BUTTON_PLAY},
    {ACTION_TREE_STOP,          BUTTON_POWER,               BUTTON_NONE},
    {ACTION_TREE_STOP,          BUTTON_POWER|BUTTON_REL,    BUTTON_POWER},
    {ACTION_TREE_STOP,          BUTTON_POWER|BUTTON_REPEAT, BUTTON_NONE},
    {ACTION_TREE_HOTKEY,        BUTTON_HOME|BUTTON_MENU,    BUTTON_NONE},

    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_LIST)
}; /* button_context_tree */

static const struct button_mapping button_context_listtree_scroll_with_combo[] = {
    {ACTION_NONE,               BUTTON_HOME,                BUTTON_NONE},
    {ACTION_TREE_PGLEFT,        BUTTON_HOME|BUTTON_LEFT,    BUTTON_NONE},
    {ACTION_TREE_ROOT_INIT,     BUTTON_HOME|BUTTON_LEFT|BUTTON_REPEAT, BUTTON_HOME|BUTTON_LEFT},
    {ACTION_TREE_PGLEFT,        BUTTON_HOME|BUTTON_LEFT|BUTTON_REPEAT, BUTTON_NONE},
    {ACTION_TREE_PGRIGHT,       BUTTON_HOME|BUTTON_RIGHT,   BUTTON_NONE},
    {ACTION_TREE_PGRIGHT,       BUTTON_HOME|BUTTON_RIGHT|BUTTON_REPEAT, BUTTON_NONE},
    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_CUSTOM|CONTEXT_TREE),
};

static const struct button_mapping button_context_listtree_scroll_without_combo[] = {
    {ACTION_NONE,               BUTTON_LEFT,                BUTTON_NONE},
    {ACTION_STD_CANCEL,         BUTTON_LEFT|BUTTON_REL,     BUTTON_LEFT},
    {ACTION_TREE_ROOT_INIT,     BUTTON_LEFT|BUTTON_REPEAT,  BUTTON_LEFT},
    {ACTION_TREE_PGLEFT,        BUTTON_LEFT|BUTTON_REPEAT,  BUTTON_NONE},
    {ACTION_NONE,               BUTTON_RIGHT,               BUTTON_NONE},
    {ACTION_STD_OK,             BUTTON_RIGHT|BUTTON_REL,    BUTTON_RIGHT},
    {ACTION_TREE_PGRIGHT,       BUTTON_RIGHT|BUTTON_REPEAT, BUTTON_NONE},
    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_CUSTOM|CONTEXT_TREE),
};

static const struct button_mapping button_context_settings[] = {
    {ACTION_SETTINGS_INC,           BUTTON_UP,                  BUTTON_NONE},
    {ACTION_SETTINGS_INCREPEAT,     BUTTON_UP|BUTTON_REPEAT,    BUTTON_NONE},
    {ACTION_SETTINGS_DEC,           BUTTON_DOWN,                BUTTON_NONE},
    {ACTION_SETTINGS_DECREPEAT,     BUTTON_DOWN|BUTTON_REPEAT,  BUTTON_NONE},
    {ACTION_STD_PREV,               BUTTON_LEFT,                BUTTON_NONE},
    {ACTION_STD_PREVREPEAT,         BUTTON_LEFT|BUTTON_REPEAT,  BUTTON_NONE},
    {ACTION_STD_NEXT,               BUTTON_RIGHT,               BUTTON_NONE},
    {ACTION_STD_NEXTREPEAT,         BUTTON_RIGHT|BUTTON_REPEAT, BUTTON_NONE},

    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_STD)
}; /* button_context_settings */

static const struct button_mapping button_context_settings_right_is_inc[] = {
    {ACTION_SETTINGS_INC,           BUTTON_RIGHT,               BUTTON_NONE},
    {ACTION_SETTINGS_INCREPEAT,     BUTTON_RIGHT|BUTTON_REPEAT, BUTTON_NONE},
    {ACTION_SETTINGS_DEC,           BUTTON_LEFT,                BUTTON_NONE},
    {ACTION_SETTINGS_DECREPEAT,     BUTTON_LEFT|BUTTON_REPEAT,  BUTTON_NONE},
    {ACTION_STD_PREV,               BUTTON_UP,                  BUTTON_NONE},
    {ACTION_STD_PREVREPEAT,         BUTTON_UP|BUTTON_REPEAT,    BUTTON_NONE},
    {ACTION_STD_NEXT,               BUTTON_DOWN,                BUTTON_NONE},
    {ACTION_STD_NEXTREPEAT,         BUTTON_DOWN|BUTTON_REPEAT,  BUTTON_NONE},

    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_STD)
}; /* button_context_settings_right_is_inc */

static const struct button_mapping button_context_yesno[] = {
    {ACTION_YESNO_ACCEPT,           BUTTON_SELECT,              BUTTON_NONE},
    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_STD)
}; /* button_context_yesno */

static const struct button_mapping button_context_colorchooser[] = {
    {ACTION_STD_OK,                 BUTTON_PLAY|BUTTON_REL,     BUTTON_NONE},
    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_CUSTOM|CONTEXT_SETTINGS),
}; /* button_context_colorchooser */

static const struct button_mapping button_context_eq[] = {
    {ACTION_STD_OK,                 BUTTON_SELECT|BUTTON_REL,   BUTTON_NONE},
    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_CUSTOM|CONTEXT_SETTINGS),
}; /* button_context_eq */

static const struct button_mapping button_context_quickscreen[] = {
    {ACTION_QS_TOP,         BUTTON_UP,                      BUTTON_NONE},
    {ACTION_QS_TOP,         BUTTON_UP|BUTTON_REPEAT,        BUTTON_NONE},
    {ACTION_QS_DOWN,        BUTTON_DOWN,                    BUTTON_NONE},
    {ACTION_QS_DOWN,        BUTTON_DOWN|BUTTON_REPEAT,      BUTTON_NONE},
    {ACTION_QS_LEFT,        BUTTON_LEFT,                    BUTTON_NONE},
    {ACTION_QS_LEFT,        BUTTON_LEFT|BUTTON_REPEAT,      BUTTON_NONE},
    {ACTION_QS_RIGHT,       BUTTON_RIGHT,                   BUTTON_NONE},
    {ACTION_QS_RIGHT,       BUTTON_RIGHT|BUTTON_REPEAT,     BUTTON_NONE},
    {ACTION_STD_CANCEL,     BUTTON_HOME,                    BUTTON_NONE},

    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_STD)
}; /* button_context_quickscreen */

static const struct button_mapping button_context_keyboard[] = {
    {ACTION_KBD_LEFT,         BUTTON_LEFT,                  BUTTON_NONE},
    {ACTION_KBD_LEFT,         BUTTON_LEFT|BUTTON_REPEAT,    BUTTON_NONE},
    {ACTION_KBD_RIGHT,        BUTTON_RIGHT,                 BUTTON_NONE},
    {ACTION_KBD_RIGHT,        BUTTON_RIGHT|BUTTON_REPEAT,   BUTTON_NONE},
    {ACTION_KBD_SELECT,       BUTTON_SELECT,                BUTTON_NONE},
    {ACTION_KBD_UP,           BUTTON_UP,                    BUTTON_NONE},
    {ACTION_KBD_UP,           BUTTON_UP|BUTTON_REPEAT,      BUTTON_NONE},
    {ACTION_KBD_DOWN,         BUTTON_DOWN,                  BUTTON_NONE},
    {ACTION_KBD_DOWN,         BUTTON_DOWN|BUTTON_REPEAT,    BUTTON_NONE},
    {ACTION_KBD_BACKSPACE,    BUTTON_BACK,                  BUTTON_NONE},
    {ACTION_KBD_BACKSPACE,    BUTTON_BACK|BUTTON_REPEAT,    BUTTON_NONE},
    {ACTION_KBD_DONE,         BUTTON_PLAY|BUTTON_REL,       BUTTON_PLAY},
    {ACTION_KBD_ABORT,        BUTTON_POWER|BUTTON_REL,      BUTTON_POWER},
    {ACTION_KBD_MORSE_INPUT,  BUTTON_HOME|BUTTON_POWER,     BUTTON_NONE},
    {ACTION_KBD_MORSE_SELECT, BUTTON_SELECT|BUTTON_REL,     BUTTON_NONE},

    LAST_ITEM_IN_LIST
}; /* button_context_keyboard */

const struct button_mapping* get_context_mapping(int context)
{
    switch (context)
    {
        case CONTEXT_STD:
            return button_context_standard;
        case CONTEXT_WPS:
            return button_context_wps;

        case CONTEXT_LIST:
            return button_context_list;
        case CONTEXT_MAINMENU:
        case CONTEXT_TREE:
            if (global_settings.hold_lr_for_scroll_in_list)
                return button_context_listtree_scroll_without_combo;
            else
                return button_context_listtree_scroll_with_combo;
        case CONTEXT_CUSTOM|CONTEXT_TREE:
            return button_context_tree;

        case CONTEXT_SETTINGS:
            return button_context_settings;
        case CONTEXT_CUSTOM|CONTEXT_SETTINGS:
            return button_context_settings_right_is_inc;

        case CONTEXT_SETTINGS_COLOURCHOOSER:
            return button_context_colorchooser;
        case CONTEXT_SETTINGS_EQ:
            return button_context_eq;

        case CONTEXT_YESNOSCREEN:
            return button_context_yesno;
        case CONTEXT_QUICKSCREEN:
            return button_context_quickscreen;
        case CONTEXT_KEYBOARD:
            return button_context_keyboard;
    }
    return button_context_standard;
}
