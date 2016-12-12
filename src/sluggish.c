/*
 * Sluggish Plugin
 *
 * Copyright (C) 2011, James Boston <pidgin@jamesboston.ca>
 * Copyright (C) 2016, Ben Kibbey <bjk@luxsci.net>
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

#define DEBUG_BUILD

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#ifndef PURPLE_PLUGINS
# define PURPLE_PLUGINS
#endif

#include <glib.h>
#include <idle.h>
#include <savedstatuses.h>
#include <request.h>

#ifndef _WIN32
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/scrnsaver.h>
#endif /* !_WIN32 */

#ifndef N_
#define N_(s) s
#endif

PurplePlugin *sluggish_plugin = NULL;

static int away = 0; /* TRUE if user active but status should be Away */
static int user_active = 0; /* 1 == activity within absence_sensitivity */
static int recently_touched = 0; /* 1 == activity within touch_sensitivity */
static time_t sluggish_expire_time = 0;
static time_t real_idle_start = 0; /* time of status changed to Away */
static int touch_sensitivity = 0;
static int absence_sensitivity = 0;
static int sluggish_time = 0;

#define SLUGGISH_VERSION    "0.9"
#define SLUGGISH_NAME        N_("Sluggish")
#define SLUGGISH_SUMMARY	N_("Sluggishly change from  Away to Available.")
#define SLUGGISH_DESCRIPTION N_("Delay changing status from Away to Available until you've been back for X minutes.")
#define SLUGGISH_AUTHOR      "James Boston <pidgin@jamesboston.ca>"
#define SLUGGISH_URL         "http://jamesboston.ca"
#define SLUGGISH_ID          "jboston-sluggish"

#define PREF_PREFIX			"/plugins/core/" SLUGGISH_ID
#define PREF_SLUGGISHNESS		PREF_PREFIX "/sluggishness"
#define PREF_TOUCH_SENSITIVITY		PREF_PREFIX "/touch-sensitivity"
#define PREF_ABSENSE_SENSITIVITY	PREF_PREFIX "/absense-sensitivity"

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
        purple_debug_error(SLUGGISH_ID,
            "get_screensaver_idle: XOpenDisplay failed. "
            "Unable to use Xserver to retrieve idle time.");
        sluggish_time = 0;  /* disable sluggish */
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
    time_t idle_pref = purple_prefs_get_int(
        "/purple/away/mins_before_away") * 60;

#ifdef _WIN32
    now = GetTickCount() / 1000;
    idle_time = now - get_lastactive();
#else /* else if X11 */
    now = time(NULL);
    idle_time = get_screensaver_idle();
#endif /* !_WIN32 */

#ifdef DEBUG_BUILD
    purple_debug_info(SLUGGISH_ID,
        "get_time_idle: (Not fake) idle_time = %ld\n", idle_time);
#endif
    recently_touched = (idle_time <= touch_sensitivity) ? 1 : 0;

    if ( away && sluggish_time ) {

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
            sluggish_expire_time = now + sluggish_time;
            idle_time = now - real_idle_start;

        } else if ( !user_active && !recently_touched ) {
            idle_time = now - real_idle_start;
        }
    }

    /* bug in idle.c
       idle_time can never equal user's idle preference exactly */
    if ( idle_time == idle_pref ) {
        idle_time += 1;
    }
#ifdef DEBUG_BUILD
    purple_debug_info(SLUGGISH_ID, "get_time_idle:\n"
        "Preferences\t: sluggish_time = %d, idle_pref=%ld\n"
        "User Status\t: user_active = %d, recently_touched = %d\n"
        "Timers\t\t: real_idle_start = %ld, (fake)idle_time = %ld, now = %ld, "
        "sluggish_expire_time = %ld, sluggish_expire_time - now = %ld\n",
        sluggish_time, idle_pref,
        user_active, recently_touched,
        real_idle_start, idle_time, now, sluggish_expire_time,
        sluggish_expire_time - now);
#endif
    return idle_time;
}

/*
 *     action callback for request API
 */

static void
sluggish_prefs_ok(gpointer _unused, PurpleRequestFields *fields)
{
    int n;

    n = purple_request_fields_get_integer(fields, "mins");
    sluggish_time = n * 60;
#ifdef _WIN32
    sluggish_expire_time = (GetTickCount() / 1000) + sluggish_time;
#else
    sluggish_expire_time = time(NULL) + sluggish_time;
#endif /* !_WIN32 */
    purple_prefs_set_int(PREF_SLUGGISHNESS, n);

    if (purple_request_fields_exists (fields, "touch"))
      {
        touch_sensitivity = purple_request_fields_get_integer(fields, "touch");
        purple_prefs_set_int(PREF_TOUCH_SENSITIVITY, touch_sensitivity);
      }

    if (purple_request_fields_exists (fields, "absense"))
      {
        absence_sensitivity = purple_request_fields_get_integer(fields, "absense");
        purple_prefs_set_int(PREF_ABSENSE_SENSITIVITY, absence_sensitivity);
      }

#ifdef DEBUG_BUILD
    purple_debug_info(SLUGGISH_ID, "sluggish_prefs_ok\n");
#endif
}

/*
 *     actions
 */
static void
action_set_sluggishness (PurplePluginAction *action)
{
    PurpleRequestFields *request;
    PurpleRequestFieldGroup *group;
    PurpleRequestField *field;
    int n;

    group = purple_request_field_group_new(NULL);

    n = purple_prefs_get_int(PREF_SLUGGISHNESS);
    field = purple_request_field_int_new ("mins", "Sluggishness in minutes",
                                          n, 0, 9999);
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
        G_CALLBACK(sluggish_prefs_ok),		  /* OK button callback */
        "_Cancel",                                /* Cancel button text */
        NULL,                                     /* Cancel calllback   */
        NULL,
        NULL);
#ifdef DEBUG_BUILD
    purple_debug_info(SLUGGISH_ID, "action_set_sluggishness\n");
#endif
}

static void
action_cancel_sluggishness(PurplePluginAction * action)
{
    sluggish_time = 0;
    purple_notify_info(sluggish_plugin, "Sluggishness Cancelled",
        "Normal idle reporting resumed.",
        "To restart set a new sluggish fudge factor.", NULL);
}

static GList *
sluggish_actions_get (PurplePlugin *plugin)
{
    GList *list = NULL;
    PurplePluginAction *action = NULL;

    action = purple_plugin_action_new(
        N_("Set Sluggishness"), action_set_sluggishness);
    list = g_list_append (list, action);

    action = purple_plugin_action_new(
        N_("Cancel Sluggishness"), action_cancel_sluggishness);
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
    if ( g_strcmp0((char*)purple_status_get_name(new), "Away") ) {
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
    purple_debug_info(SLUGGISH_ID,
        "account_status_changed: account = %s, changed from [%s] to [%s]\n",
        purple_account_get_username(account),
        purple_status_get_name(old),
        purple_status_get_name(new));
#endif
}

static PurplePluginPrefFrame *
sluggish_prefs_get(PurplePlugin *plugin)
{
    PurpleRequestFields *fields;
    PurpleRequestFieldGroup *group;
    PurpleRequestField *field;
    gpointer handle;
    int n;

    fields = purple_request_fields_new();
    group = purple_request_field_group_new(NULL);
    purple_request_fields_add_group(fields, group);

    n = purple_prefs_get_int(PREF_SLUGGISHNESS);
    field = purple_request_field_int_new ("mins", N_("Sluggishness in minutes"),
                                          n, 0, 9999);
    purple_request_field_group_add_field(group, field);


    n = purple_prefs_get_int(PREF_ABSENSE_SENSITIVITY);
    field = purple_request_field_int_new ("absense",
                                          N_("Absence sensitivity in seconds"),
                                          n, 0, 9999);
    purple_request_field_group_add_field(group, field);

    n = purple_prefs_get_int(PREF_TOUCH_SENSITIVITY);
    field = purple_request_field_int_new ("touch",
                                          N_("Recent touch sensitivity in seconds"),
                                          n, 0, 99);
    purple_request_field_group_add_field(group, field);

    handle = purple_request_fields(plugin,
                                   N_("Sluggish"), NULL, NULL, fields,
                                   N_("OK"), (GCallback)sluggish_prefs_ok,
                                   N_("Cancel"), NULL, NULL, NULL);

#ifdef DEBUG_BUILD
    purple_debug_info(SLUGGISH_ID, "sluggish_prefs_get\n");
#endif
    return handle;
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
 *     Plugin scaffolding
 */

static gboolean
plugin_load(PurplePlugin *plugin, GError **error)
{
    sluggish_plugin = plugin;

    purple_prefs_add_none(PREF_PREFIX);
    purple_prefs_add_int(PREF_SLUGGISHNESS, 3);
    purple_prefs_add_int(PREF_ABSENSE_SENSITIVITY, 60);
    purple_prefs_add_int(PREF_TOUCH_SENSITIVITY, 30);
#ifdef DEBUG_BUILD
    purple_debug_info(SLUGGISH_ID, "plugin_load\n");
#endif

    sluggish_time = purple_prefs_get_int(PREF_SLUGGISHNESS) * 60;
    absence_sensitivity = purple_prefs_get_int(PREF_ABSENSE_SENSITIVITY);
    touch_sensitivity = purple_prefs_get_int(PREF_TOUCH_SENSITIVITY);

#ifdef DEBUG_BUILD
    purple_debug_info(SLUGGISH_ID,
        "plugin_load: sluggish_time = %d, absence_sensitivity = %d, "
        "touch_sensitivity =%d\n",
        sluggish_time, absence_sensitivity, touch_sensitivity);
#endif

    /* Replace the idle time calculator */
    purple_idle_init();
    purple_idle_set_ui_ops(&ui_ops);

    purple_signal_connect(purple_accounts_get_handle(),
        "account-status-changed", sluggish_plugin,
        PURPLE_CALLBACK(account_status_changed), NULL);

    purple_debug_info(SLUGGISH_ID, "sluggish plugin loaded");

    return TRUE;
}

static gboolean
plugin_unload(PurplePlugin *plugin, GError **error)
{
  purple_idle_uninit();
  sluggish_plugin = NULL;
  return TRUE;
}

static PurplePluginInfo *
plugin_query(GError **error)
{
  const gchar * const authors[] = {
      SLUGGISH_AUTHOR,
      "Ben Kibbey <bjk@luxsci.net>",
      NULL
  };

  return purple_plugin_info_new(
                                "id",           SLUGGISH_ID,
                                "name",         SLUGGISH_NAME,
                                "version",      SLUGGISH_VERSION,
                                "category",     N_("Idle"),
                                "summary",      SLUGGISH_SUMMARY,
                                "description",  SLUGGISH_DESCRIPTION,
                                "authors",      authors,
                                "website",      "http://jamesboston.ca",
                                "abi-version",  PURPLE_ABI_VERSION,
                                "pref-request-cb", sluggish_prefs_get,
                                "actions-cb", sluggish_actions_get,
                                NULL
  );
}

PURPLE_PLUGIN_INIT(sluggish_idle, plugin_query, plugin_load, plugin_unload);
