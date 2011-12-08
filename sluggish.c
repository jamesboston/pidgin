/*
 * Sluggish Plugin
 *
 * Copyright (C) 2011, James Boston <pidgin@jamesboston.ca>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02111-1301, USA.
 *
 */

#define DEBUG_BUILD 1

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#ifndef PURPLE_PLUGINS
# define PURPLE_PLUGINS
#endif

#include <glib.h>
#include <notify.h>
#include <plugin.h>
#include <version.h>
#include <debug.h>
#include <idle.h>
#include <savedstatuses.h>
#include <request.h>
#include "internal.h"
#include "pluginpref.h"
#include "prefs.h"

#ifndef _WIN32
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/scrnsaver.h>
#endif /* !_WIN32 */

/* TODO: change ID and plugin pref paths */
#define IDLE_PLUGIN_ID "core-jboston-sluggish"

PurplePlugin *sluggish_plugin = NULL;

static int away = 0; /* TRUE if user active but status should be Away */
static int user_active = 0; /* 1 == activity within absence_sensitivity */
static int recently_touched = 0; /* 1 == activity within touch_sensitivity */
static time_t sluggish_expire_time = 0;
static time_t real_idle_start = 0; /* time of status changed to Away */
static int touch_sensitivity = 0;
static int absence_sensitivity = 0;
static int SLUGGISH_TIME = 0;

/*
 *     time functions
 */
#ifdef _WIN32
static DWORD
get_lastactive(void)
{
    DWORD result = 0;
    LASTINPUTINFO lii;
    memset(&lii, 0, sizeof(lii));
    lii.cbSize = sizeof(lii);
    if (GetLastInputInfo(&lii))
        result = lii.dwTime / 1000; /* milliseconds to seconds */
    return result;
}
#else /* use X11 for idle time */
static time_t
get_screensaver_idle(void){
    time_t result;
    Display *dpy;
    Window rootwin;
    int scr;
    static XScreenSaverInfo *mit_info = NULL;
    static int has_extension = -1;
    int event_base, error_base;

    if(!(dpy=XOpenDisplay(NULL))) {
        purple_debug_error("core-jboston-sluggish",
            "get_screensaver_idle: XOpenDisplay failed. "
            "Unable to use Xserver to retrieve idle time.");
        SLUGGISH_TIME = 0;  /* disable sluggish */
        return 0;
    }

    scr = DefaultScreen(dpy);
    rootwin = RootWindow(dpy, scr);

    if (has_extension == -1){
        has_extension = XScreenSaverQueryExtension(
            dpy, &event_base, &error_base);
    }

    if (has_extension){
        if (mit_info == NULL){
            mit_info = XScreenSaverAllocInfo();
        }
        XScreenSaverQueryInfo(dpy, rootwin, mit_info);
        result = (mit_info->idle) / 1000;
    } else {
        result = 0;;
    }

    XCloseDisplay(dpy);

    return result;
}
#endif /* !_WIN32 */

static time_t
get_time_idle(void)
{
    time_t now;
    time_t idle_time;
    time_t IDLE_PREF = purple_prefs_get_int(
        "/purple/away/mins_before_away") * 60;

#ifdef _WIN32
    now = GetTickCount() / 1000;
    idle_time = now - get_lastactive();
#else /* else if X11 */
    now = time(NULL);
    idle_time = get_screensaver_idle();
#endif /* !_WIN32 */

#ifdef DEBUG_BUILD
    purple_debug_info("Sluggish",
        "get_time_idle: (Not fake) idle_time = %ld\n", idle_time);
#endif
    recently_touched = (idle_time <= touch_sensitivity) ? 1 : 0;

    if ( away && SLUGGISH_TIME ) {

        if ( user_active && recently_touched ) {
            if ( now >= sluggish_expire_time ) {
                idle_time = 0;
            } else {
                idle_time = now - real_idle_start;
            }

        } else if ( user_active && !recently_touched ) {
            if ( idle_time >= absence_sensitivity ) {
                user_active = 0;
            }
            idle_time = now - real_idle_start;

        } else if ( !user_active && recently_touched ) {
            user_active = 1;
            sluggish_expire_time = now + SLUGGISH_TIME;
            idle_time = now - real_idle_start;

        } else if ( !user_active && !recently_touched ) {
            idle_time = now - real_idle_start;
        }
    }

    /* bug in idle.c
       idle_time can never equal user's idle preference exactly */
    if ( idle_time == IDLE_PREF ) {
        idle_time += 1;
    }
#ifdef DEBUG_BUILD
    purple_debug_info("Sluggish", "get_time_idle:\n"
        "Preferences\t: SLUGGISH_TIME = %d, IDLE_PREF=%ld\n"
        "User Status\t: user_active = %d, recently_touched = %d\n"
        "Timers\t\t: real_idle_start = %ld, (fake)idle_time = %ld, now = %ld, "
        "sluggish_expire_time = %ld, sluggish_expire_time - now = %ld\n",
        SLUGGISH_TIME, IDLE_PREF,
        user_active, recently_touched,
        real_idle_start, idle_time, now, sluggish_expire_time,
        sluggish_expire_time - now);
#endif
    return idle_time;
}
/**/
static PurpleIdleUiOps ui_ops =
{
#if defined(USE_SCREENSAVER) || defined(HAVE_IOKIT)
    get_time_idle,
#else
    NULL,
#endif /* USE_SCREENSAVER || HAVE_IOKIT */
    NULL,
    NULL,
    NULL,
    NULL
};

/*
 *     action callback for request API
 */

static void
action_set_sluggishness_ok (void *ignored, PurpleRequestFields *fields)
{
    SLUGGISH_TIME = purple_request_fields_get_integer(fields, "mins") * 60;
#ifdef _WIN32
    sluggish_expire_time = (GetTickCount() / 1000) + SLUGGISH_TIME;
#else
    sluggish_expire_time = time(NULL) + SLUGGISH_TIME;
#endif /* !_WIN32 */
    purple_prefs_set_int(
        "/plugins/core/jboston-sluggish/sluggishness", SLUGGISH_TIME / 60);
#ifdef DEBUG_BUILD
    purple_debug_info("Sluggish", "action_set_sluggishness_ok\n");
#endif
}

/*
 *     actions
 */

static void
action_set_sluggishness (PurplePluginAction * action)
{
    PurpleRequestFields *request;
    PurpleRequestFieldGroup *group;
    PurpleRequestField *field;
    int sluggish_pref = purple_prefs_get_int(
        "/plugins/core/jboston-sluggish/sluggishness");
    group = purple_request_field_group_new(NULL);

    field = purple_request_field_int_new("mins", "Minutes", sluggish_pref);
    purple_request_field_group_add_field(group, field);

    request = purple_request_fields_new();
    purple_request_fields_add_group(request, group);

    purple_request_fields(
        action->plugin,                           /* handle             */
        "Sluggish",                               /* title              */
        "Set Account Idle Time",                  /* primary info       */
        NULL,                                     /* secondary info     */
        request,                                  /* fields             */
        "_Set",                                   /* OK button  text    */
        G_CALLBACK(action_set_sluggishness_ok),   /* OK button callback */
        "_Cancel",                                /* Cancel button text */
        NULL,                                     /* Cancel calllback   */
        NULL,
        NULL,
        NULL,
        NULL);
#ifdef DEBUG_BUILD
    purple_debug_info("Sluggish", "action_set_sluggishness\n");
#endif
}

static void
action_cancel_sluggishness(PurplePluginAction * action)
{
    SLUGGISH_TIME = 0;
    purple_notify_info(sluggish_plugin, "Sluggishness Cancelled",
        "Normal idle reporting resumed.",
        "To restart set a new sluggish fudge factor.");
}

static GList *
plugin_actions (PurplePlugin * plugin, gpointer context)
{
    GList *list = NULL;
    PurplePluginAction *action = NULL;

    action = purple_plugin_action_new(
        "Set Sluggishness", action_set_sluggishness);
    list = g_list_append (list, action);

    action = purple_plugin_action_new(
        "Cancel Sluggishness", action_cancel_sluggishness);
    list = g_list_append (list, action);

    return list;
}

/*
 *     call back for account signal
 */

static void
account_status_changed(PurpleAccount *account, PurpleStatus *old,
    PurpleStatus *new, gpointer data)
{
    if ( strcmp((char*)purple_status_get_name(new), "Away") ) {
        away = 0;
        recently_touched = 0;
    } else {
        away = 1;
#ifdef _WIN32
        real_idle_start = get_lastactive() / 1000;
#else
        real_idle_start = time(NULL) - get_screensaver_idle();
#endif /* !_WIN32 */
    }

    user_active = 0;

#ifdef DEBUG_BUILD
    purple_debug_info("Sluggish",
        "account_status_changed: account = %s, changed from [%s] to [%s]\n",
        purple_account_get_username(account),
        purple_status_get_name(old),
        purple_status_get_name(new));
#endif
}

/*
 *     Preferences
 */

 static void
 preference_check (PurplePlugin *plugin)
 {
    SLUGGISH_TIME = purple_prefs_get_int(
        "/plugins/core/jboston-sluggish/sluggishness") * 60;
    absence_sensitivity = purple_prefs_get_int(
        "/plugins/core/jboston-sluggish/absence_sensitivity");
    touch_sensitivity = purple_prefs_get_int(
        "/plugins/core/jboston-sluggish/touch_sensitivity");

#ifdef DEBUG_BUILD
    purple_debug_info("Sluggish",
        "preference_check: SLUGGISH_TIME = %d, absence_sensitivity = %d, "
        "touch_sensitivity =%d\n",
        SLUGGISH_TIME, absence_sensitivity, touch_sensitivity);
#endif
 }

 static void
 preference_changed_cb(const char *name, PurplePrefType type,
        gconstpointer val, gpointer data)
{
    preference_check((PurplePlugin *)data);
#ifdef DEBUG_BUILD
    purple_debug_info("Sluggish", "preference_changed_cb\n");
#endif
}

 static PurplePluginPrefFrame *
get_plugin_pref_frame(PurplePlugin *plugin)
{
    PurplePluginPrefFrame *frame;
    PurplePluginPref *ppref;

    frame = purple_plugin_pref_frame_new();

    ppref = purple_plugin_pref_new_with_label("Settings");
    purple_plugin_pref_frame_add(frame, ppref);

    ppref = purple_plugin_pref_new_with_name_and_label(
        "/plugins/core/jboston-sluggish/sluggishness",
        "Sluggishness in minutes.");
    purple_plugin_pref_set_bounds(ppref, 0, 9999);
    purple_plugin_pref_frame_add(frame, ppref);

    ppref = purple_plugin_pref_new_with_label("Advanced Settings");
    purple_plugin_pref_frame_add(frame, ppref);

    ppref = purple_plugin_pref_new_with_name_and_label(
        "/plugins/core/jboston-sluggish/absence_sensitivity",
        "Absence sensitivity in seconds.");
    purple_plugin_pref_set_bounds(ppref, 0, 9999);
    purple_plugin_pref_frame_add(frame, ppref);

    ppref = purple_plugin_pref_new_with_name_and_label(
        "/plugins/core/jboston-sluggish/touch_sensitivity",
        "Recent touch sensitivity in seconds.");
    purple_plugin_pref_set_bounds(ppref, 0, 99);
    purple_plugin_pref_frame_add(frame, ppref);
#ifdef DEBUG_BUILD
    purple_debug_info("Sluggish", "get_plugin_pref_frame\n");
#endif

    return frame;
}

static PurplePluginUiInfo prefs_info =
{
    get_plugin_pref_frame,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};



/*
 *     Plugin scaffolding
 */

static gboolean
plugin_load (PurplePlugin * plugin)
{
    sluggish_plugin = plugin;

    purple_prefs_connect_callback(plugin,
        "/plugins/core/jboston-sluggish/sluggishness",
        preference_changed_cb, plugin);
    purple_prefs_connect_callback(plugin,
        "/plugins/core/jboston-sluggish/absence_sensitivity",
        preference_changed_cb, plugin);
    purple_prefs_connect_callback(plugin,
        "/plugins/core/jboston-sluggish/touch_sensitivity",
        preference_changed_cb, plugin);
    preference_check(sluggish_plugin);

    /* Replace the idle time calculator */
    purple_idle_set_ui_ops(&ui_ops);

    purple_signal_connect(purple_accounts_get_handle(),
        "account-status-changed", sluggish_plugin,
        PURPLE_CALLBACK(account_status_changed), NULL);

    purple_debug_info("Sluggish", "sluggish plugin loaded");

    return TRUE;
}

static PurplePluginInfo info =
{
    PURPLE_PLUGIN_MAGIC,
    PURPLE_MAJOR_VERSION,
    PURPLE_MINOR_VERSION,
    PURPLE_PLUGIN_STANDARD,
    NULL,
    0,
    NULL,
    PURPLE_PRIORITY_DEFAULT,

    IDLE_PLUGIN_ID,
    "Sluggish",
    "0.8b",
    "Sluggishly change from  Away to Available.",
    "Delay changing status from Away to Available until you've been back for X minutes.",
    "James Boston <pidgin@jamesboston.ca>",
    "http://jamesboston.ca",

    plugin_load,
    NULL,
    NULL,

    NULL,
    NULL,
    &prefs_info,
    plugin_actions,
    NULL,
    NULL,
    NULL,
    NULL
};

static void
init_plugin (PurplePlugin * plugin)
{
    purple_prefs_add_none("/plugins/core/jboston-sluggish");
    purple_prefs_add_int("/plugins/core/jboston-sluggish/sluggishness", 3);
    purple_prefs_add_int("/plugins/core/jboston-sluggish/absence_sensitivity",
        60);
    purple_prefs_add_int("/plugins/core/jboston-sluggish/touch_sensitivity",
        30);
#ifdef DEBUG_BUILD
    purple_debug_info("Sluggish", "init_plugin\n");
#endif
}

PURPLE_INIT_PLUGIN (sluggish, init_plugin, info)
