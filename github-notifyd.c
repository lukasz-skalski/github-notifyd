/* github-notifyd - GitHub Notifications Daemon
 *
 * Copyright (C) Lukasz Skalski <lukasz.skalski@op.pl>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <sys/stat.h>

#include <glib.h>
#include <glib-unix.h>
#include <jansson.h>
#include <curl/curl.h>
#include <libnotify/notify.h>

#ifdef HAVE_SYSTEMD
#include <systemd/sd-journal.h>
#endif

#ifndef ACCESS_TOKEN
#error TODO
#endif

#define QUOTE(name) #name
#define STR(macro) QUOTE(macro)
#define TOKEN_STR STR(ACCESS_TOKEN)

#define ACCESS_TOKEN_HEADER          "Authorization: token " TOKEN_STR
#define USER_AGENT_HEADER            "User-Agent: github-notifyd/1.0"

#define RESPONSE_CODE_OK             200
#define RESPONSE_CODE_NOT_MODIFIED   304
#define RESPONSE_CODE_UNAUTHORIZED   401
#define GITHUB_API_NOTIFICATIONS     "https://api.github.com/notifications"
#define SUMMARY                      "You have received a new GitHub Notification"

#define BODY                         "body"
#define BODY_HYPERLINKS              "body-hyperlinks"
#define BODY_MARKUP                  "body-markup"
#define PERSISTENCE                  "persistence"

#define TAG_BOLD                     "<b>"
#define TAG_BOLD_END                 "</b>"

static gboolean opt_no_daemon = FALSE;
static gboolean opt_no_avatar = FALSE;
static gboolean opt_persistent = FALSE;
static guint opt_interval = 45;

static GMainLoop *mainloop;
static gchar *name, *vendor;
static gchar *version, *spec_version;
static glong last_mod = 0;

typedef struct
{
  gchar  *repository;
  gchar  *repository_url;
  gchar  *type;
  gchar  *title;
  gchar  *user;
  gchar  *user_avatar;
  gchar  *reason;
} notification;

struct data_struct
{
  gchar  *data;
  gsize   size;
};


/*
 * notification server caps
 * more info: https://developer.gnome.org/notification-spec/#protocol
 */
enum {
  CAP_BODY = 0,
  CAP_BODY_HYPERLINKS,
  CAP_BODY_MARKUP,
  CAP_PERSISTENCE
};

static gboolean server_caps[] =
{
  FALSE, /* body            */
  FALSE, /* body-hyperlinks */
  FALSE, /* body-markup     */
  FALSE  /* persistence     */
};


/*
 * commandline options
 */
GOptionEntry entries[] =
{
  { "no-daemon", 'n', 0, G_OPTION_ARG_NONE, &opt_no_daemon, "Don't detach github-notifyd into the background", NULL},
  { "no-user-avatar", 'a', 0, G_OPTION_ARG_NONE, &opt_no_avatar, "Don't show user avatar as a notification icon", NULL},
  { "persistent-notifications", 'p', 0, G_OPTION_ARG_NONE, &opt_persistent, "Use persistent notifications", NULL},
  { "polling-interval", 'i', 0, G_OPTION_ARG_INT, &opt_interval, "Notifications polling interval [default: 45s]", NULL},
  { NULL }
};


/*
 * print_log function
 */
static void
print_log (gint msg_priority, const gchar *msg, ...)
{
  va_list arg;
  va_start(arg, msg);

  GString *log = g_string_new(NULL);
  g_string_vprintf (log, msg, arg);

#ifdef DEBUG
  g_print ("%s", log->str);
#endif

#ifdef HAVE_SYSTEMD
  sd_journal_print (msg_priority, log->str);
#else
  syslog (msg_priority, log->str);
#endif

  g_string_free(log, TRUE);
  va_end(arg);
}


/*
 * daemonize
 */
static void
daemonize (void)
{
  pid_t pid;
  pid_t sid;

  pid = fork();
  if (pid < 0)
    exit (EXIT_FAILURE);

  if (pid > 0)
    exit (EXIT_SUCCESS);

  umask(0);

  sid = setsid();
  if (sid < 0)
    exit (EXIT_FAILURE);

  if ((chdir("/")) < 0)
    exit (EXIT_FAILURE);

  close (STDIN_FILENO);
  close (STDOUT_FILENO);
  close (STDERR_FILENO);
}


/*
 * SIGINT handler
 */
static gboolean
sigint_handler ()
{
  g_main_loop_quit (mainloop);
  return TRUE;
}


/*
 * set notifications server caps
 */
static void
set_server_caps (gpointer data,
                 gpointer user_data)
{
  if (!g_strcmp0 (BODY, (gchar*) data))
    server_caps[CAP_BODY] = TRUE;

  if (!g_strcmp0 (BODY_HYPERLINKS, (gchar*) data))
    server_caps[CAP_BODY_HYPERLINKS] = TRUE;

  if (!g_strcmp0 (BODY_MARKUP, (gchar*) data))
    server_caps[CAP_BODY_MARKUP] = TRUE;

  if (!g_strcmp0 (PERSISTENCE, (gchar*) data))
    server_caps[CAP_PERSISTENCE] = TRUE;
}


/*
 * write callback
 */
static size_t
write_callback (void   *ptr,
                gsize   size,
                gsize   nmemb,
                void   *userdata)
{
  gsize real_size;
  struct data_struct *mem;

  real_size = size * nmemb;
  mem = (struct data_struct*) userdata;

  mem->data = realloc (mem->data, mem->size + real_size + 1);
  if (mem->data == NULL) {
    print_log (LOG_ERR, "curl request error: not enough memory\n");
    return 0;
  }

  memcpy (&(mem->data[mem->size]), ptr, real_size);
  mem->size += real_size;
  mem->data[mem->size] = 0;

  return real_size;
}


/*
 * curl request
 */
static gchar *
curl_request (const gchar  *url,
              gboolean      pass_ifmodsince,
              glong        *code)
{
  CURL *curl;
  CURLcode status;
  struct curl_slist *headers;
  struct data_struct chunk;

  headers = NULL;

  /* init buffer for incoming data */
  chunk.data = malloc(1);
  chunk.size = 0;

  /* init the curl session */
  curl_global_init(CURL_GLOBAL_ALL);
  curl = curl_easy_init();
  if (!curl)
    {
      print_log (LOG_ERR, "curl_easy_init() failed\n");
      goto exit_null;
    }

  /* set 'url' to use in the request */
  curl_easy_setopt(curl, CURLOPT_URL, url);

  /* GitHub API v3 requires a 'User-Agent' header */
  headers = curl_slist_append (headers, USER_AGENT_HEADER);

  /* set personal access token */
  headers = curl_slist_append (headers, ACCESS_TOKEN_HEADER);

  /* set custom HTTP headers */
  curl_easy_setopt (curl, CURLOPT_HTTPHEADER, headers);

  /* set callback for writing received data */
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);

  /* pass 'data_struct' to the callback function */
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);

  /* maximum time the request is allowed to take - 30s */
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

  /* set 'If-Modified-Since' value */
  if (pass_ifmodsince)
    {
      curl_easy_setopt(curl, CURLOPT_FILETIME, 1);
      curl_easy_setopt(curl, CURLOPT_TIMECONDITION, CURL_TIMECOND_IFMODSINCE);
      curl_easy_setopt(curl, CURLOPT_TIMEVALUE, last_mod);
    }

  /* perform a blocking request */
  status = curl_easy_perform (curl);
  if (status != CURLE_OK)
    {
      print_log (LOG_ERR, "curl_easy_perform() failed: %s\n", curl_easy_strerror(status));
      goto exit_null;
    }

  /* check response code */
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, code);
  if((*code != RESPONSE_CODE_OK) && (*code != RESPONSE_CODE_NOT_MODIFIED))
    {
      print_log (LOG_ERR, "curl request error: server responded with code %ld\n", *code);
      goto exit_null;
    }

  /* read 'Last-Modified' value */
  if (pass_ifmodsince)
    {
      if (*code != RESPONSE_CODE_NOT_MODIFIED)
        curl_easy_getinfo(curl, CURLINFO_FILETIME, &last_mod);
      else
        goto exit_null;
    }

  /* clean up curl stuff */
  curl_easy_cleanup (curl);
  curl_slist_free_all (headers);
  curl_global_cleanup();

  /* return received data */
  return chunk.data;

exit_null:

  if (chunk.data)
    free (chunk.data);
  if (curl)
    curl_easy_cleanup(curl);
  if (headers)
    curl_slist_free_all (headers);

  curl_global_cleanup();
  return NULL;
}


/*
 * download user avatar
 */
static gchar *
prepare_avatar (guint32       id,
                const gchar  *avatar_url)
{
  FILE *fp;
  CURL *curl;
  CURLcode status;
  gchar *path;

  fp = NULL;
  curl = NULL;
  path = NULL;

  /* prepare string containing an absolute path to image - /tmp/ID.png */
  if ((asprintf (&path, "/tmp/%d.png", id) == -1))
    return NULL;

  /* check whether a file exists */
  if (access (path, F_OK) == -1)
    {
      print_log (LOG_INFO, "downloading user avatar image\n");
      fp = fopen(path, "w");

      /* init the curl session */
      curl = curl_easy_init();
      if (curl)
        {
          /* use internal default function instead of callback */
          curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, NULL);

          /* write the data directly to file descriptor */
          curl_easy_setopt (curl, CURLOPT_WRITEDATA, fp);
          curl_easy_setopt (curl, CURLOPT_URL, avatar_url);

          /* perform a blocking request */
          status = curl_easy_perform (curl);
          if (status != CURLE_OK)
            {
              print_log (LOG_ERR, "curl_easy_perform() failed: %s\n", curl_easy_strerror(status));
              goto error;
            }
        }
      else
        goto error;

      /* some clean up */
      fclose (fp);
      curl_easy_cleanup (curl);
    }

  return path;

error:

  /* upss... something goes wrong */
  print_log (LOG_ERR, "cannot prepare user avatar image\n");

  if (fp)
    fclose (fp);
  if (curl)
    curl_easy_cleanup (curl);
  if (path)
    free (path);

  return NULL;
}


/*
 * show notification
 */
static void
show_notification (gpointer data,
                   gpointer user_data)
{
  NotifyNotification *notif_to_show;
  GString *body;
  gchar *newline, *bold, *bold_end;
  notification *notif;

  notif = (notification*) data;
  body = g_string_new (NULL);
  newline = "\n";
  bold = TAG_BOLD;
  bold_end = TAG_BOLD_END;

  /*
   * notification servers that do not support body tags should
   * filter them out, but to be sure let's remove them from body
   */
  if (!server_caps[CAP_BODY_MARKUP])
    {
      bold = "";
      bold_end = "";
    }

  /*
   * Exception 1: notification server on KDE (version 1.0)
   * doesn't understand '\n' - we have to replace it with <br\>
   */
  if ((g_strcmp0 (name, "Plasma") == 0) &&
      (g_strcmp0 (vendor, "KDE") == 0) &&
      (g_strcmp0 (version, "1.0") == 0))
    {
      newline = "<br/>";
    }

  /*
   * Exception 2: xfce4-notifyd notification server for Xfce,
   * doesn't support properly hyperlinks in the notifications
   */
  if ((g_strcmp0 (name, "Xfce Notify Daemon") == 0) &&
      (g_strcmp0 (vendor, "Xfce") == 0))
    {
      server_caps [CAP_BODY_HYPERLINKS] = FALSE;
    }

  /*
   * some notification servers implementations may only show the summary,
   * if server supports body text, let's append it to the notification
   */
  if (server_caps [CAP_BODY])
    {
      g_string_append_printf (body, "%sRepository:%s\t %s%s", bold, bold_end, notif->repository, newline);
      g_string_append_printf (body, "%sType:%s\t\t %s%s", bold, bold_end, notif->type, newline);
      g_string_append_printf (body, "%sTitle:%s\t\t %s%s", bold, bold_end, notif->title, newline);
      g_string_append_printf (body, "%sUser:%s\t\t %s", bold, bold_end, notif->user);

      /* check whether server supports hyperlinks in the notifications */
      if (server_caps [CAP_BODY_HYPERLINKS])
        {
          g_string_append_printf (body, "%s%sLink:%s\t\t <a href=%s>Link to Repository</a>",
                                  newline, bold, bold_end, notif->repository_url);
        }
    }

  /* create new notification */
  notif_to_show = notify_notification_new (SUMMARY, body->str, notif->user_avatar);

  /* persistent/transient */
  if (!opt_persistent)
    notify_notification_set_hint (notif_to_show, "transient", g_variant_new_boolean (TRUE));
  else
    if (!server_caps [CAP_PERSISTENCE])
      print_log (LOG_INFO, "notification server doesn't support persistent notifications\n");

  /* timeout and urgency */
  notify_notification_set_timeout (notif_to_show, NOTIFY_EXPIRES_DEFAULT);
  notify_notification_set_urgency (notif_to_show, NOTIFY_URGENCY_NORMAL);

  /* finally we can show notification */
  notify_notification_show (notif_to_show, NULL);

  /* it's time to clean up */
  g_string_free (body, TRUE);
  g_object_unref (G_OBJECT(notif_to_show));
}


/*
 * free notification
 */
static void
free_notification (gpointer data,
                   gpointer user_data)
{
  notification *notif;
  notif = (notification*) data;

  g_free (notif->repository);
  g_free (notif->repository_url);
  g_free (notif->type);
  g_free (notif->title);
  g_free (notif->user);
  g_free (notif->user_avatar);
  g_free (notif->reason);
}


/*
 * check GitHub notifications status
 */
static gboolean
check_github_notifications (gpointer user_data)
{
  NotifyNotification *error;
  GList *notifications_list;
  json_t *json_root, *json_local_root;
  json_error_t json_error;
  gchar *curl_response;
  guint json_cnt;
  glong return_code;

  error = NULL;
  notifications_list = NULL;
  json_root = NULL;
  curl_response = NULL;

  /* list all notifications */
  curl_response = curl_request (GITHUB_API_NOTIFICATIONS, TRUE, &return_code);
  if (curl_response == NULL)
    goto error;

  /* decode received JSON string */
  json_root = json_loads (curl_response, 0, &json_error);
  g_free (curl_response);

  if (!json_root)
    {
      print_log (LOG_ERR, "JSON error: on line %d: %s\n", json_error.line, json_error.text);
      goto error;
    }

  if (!json_is_array(json_root))
    {
      print_log (LOG_ERR, "JSON error: root is not an array\n");
      json_decref (json_root);
      goto error;
    }

  /* iterate over notifications array */
  for (json_cnt = 0; json_cnt < json_array_size (json_root); ++json_cnt)
    {
      notification *notif;
      json_t *json_notification, *json_obj;
      json_t *json_subject, *json_repository;

      curl_response = NULL;
      json_notification = NULL;
      json_obj = NULL;
      json_subject = NULL;
      json_repository = NULL;

      /* allocate memory for new notification */
      notif = g_new0 (notification, 1);

      json_notification = json_array_get (json_root, json_cnt);
      if (!json_is_object (json_notification))
        goto skip;

      /* read notification reason */
      json_obj = json_object_get (json_notification, "reason");
      if (json_is_string (json_obj))
        notif->reason = g_strdup (json_string_value (json_obj));
      else
        goto skip;

      /* read notification subject */
      json_subject = json_object_get (json_notification, "subject");
      if (!json_is_object (json_subject))
        goto skip;

      /* read notification type */
      json_obj = json_object_get (json_subject, "type");
      if (json_is_string (json_obj))
        notif->type = g_strdup (json_string_value (json_obj));
      else
        goto skip;

      /* read notification title */
      json_obj = json_object_get (json_subject, "title");
      if (json_is_string (json_obj))
        notif->title = g_strdup (json_string_value (json_obj));
      else
        goto skip;

      json_repository = json_object_get (json_notification, "repository");
      if (!json_is_object (json_repository))
        goto skip;

      /* read reposiotry name */
      json_obj = json_object_get (json_repository, "name");
      if (json_is_string (json_obj))
        notif->repository = g_strdup (json_string_value (json_obj));
      else
        goto skip;

      /* read url to repository */
      json_obj = json_object_get (json_repository, "html_url");
      if (json_is_string (json_obj))
        notif->repository_url = g_strdup (json_string_value (json_obj));
      else
        goto skip;

      /* let's request some additional info: user name and user avatar */
      json_obj = json_object_get (json_subject, "latest_comment_url");
      if (json_is_string (json_obj))
        {
          json_t *json_user;
          guint32 json_user_id;

          json_local_root = NULL;
          json_user = NULL;

          curl_response = curl_request (json_string_value (json_obj), FALSE, &return_code);
          if (curl_response == NULL)
            goto skip;

          json_local_root = json_loads (curl_response, 0, &json_error);
          g_free (curl_response);

          if (!json_local_root)
            {
              print_log (LOG_ERR, "JSON error: on line %d: %s\n", json_error.line, json_error.text);
              goto skip;
            }

          json_user = json_object_get (json_local_root, "user");
          if (!json_is_object (json_user))
            {
              json_decref (json_local_root);
              goto skip;
            }

          /* read user login */
          json_obj = json_object_get (json_user, "login");
          if (json_is_string (json_obj))
            notif->user = g_strdup (json_string_value (json_obj));
          else
            {
              json_decref (json_local_root);
              goto skip;
            }

          /* read user ID */
          json_obj = json_object_get (json_user, "id");
          if (json_is_number (json_obj))
            json_user_id = (guint32) json_number_value (json_obj);
          else
            {
              json_decref (json_local_root);
              goto skip;
            }

          /* read url to avatar */
          if (!opt_no_avatar)
            {
              json_obj = json_object_get (json_user, "avatar_url");
              if (json_is_string (json_obj))
                notif->user_avatar = prepare_avatar (json_user_id, json_string_value (json_obj));
              else
                {
                  json_decref (json_local_root);
                  goto skip;
                }
            }
          else
            notif->user_avatar = NULL;

          json_decref (json_local_root);
        }
      else
        goto skip;

      /* append new notification to 'notifications_list' */
      print_log (LOG_INFO, "new notification: respository=%s type=%s reason=%s\n",
                 notif->repository, notif->type, notif->reason);
      notifications_list = g_list_append (notifications_list, notif);
      continue;

skip:
      /* upss... something goes wrong */
      print_log (LOG_INFO, "invalid notification - %p\n", notif);
      free_notification (notif, NULL);
      continue;
    }

  /* show all received notifications */
  g_list_foreach (notifications_list, show_notification, NULL);

  /* clean up */
  g_list_foreach (notifications_list, free_notification, NULL);
  g_list_free (notifications_list);
  json_decref (json_root);

  return TRUE;

error:

  /* it's not error - we just don't have any new notifications to show */
  if (return_code == RESPONSE_CODE_NOT_MODIFIED)
    return TRUE;

  /* show error notification */
  if (return_code == RESPONSE_CODE_UNAUTHORIZED)
    error = notify_notification_new ("'github-notifyd' authorization error - please check access token value", NULL, NULL);
  else
    error = notify_notification_new ("'github-notifyd' undefinded error - please check the logs for more information", NULL, NULL);

  notify_notification_set_timeout (error, NOTIFY_EXPIRES_DEFAULT);
  notify_notification_set_urgency (error, NOTIFY_URGENCY_CRITICAL);
  notify_notification_show (error, NULL);

  g_object_unref (G_OBJECT(error));
  return TRUE;
}


/*
 * main function
 */
int
main (int argc, char **argv)
{
  GList           *server_caps;
  GOptionContext  *option_context;
  GError          *error;
  gint signal_id, exit_value;

  server_caps = NULL;
  option_context = NULL;
  error = NULL;
  exit_value = EXIT_SUCCESS;

  /* parse commandline options */
  option_context = g_option_context_new ("- GitHub Notifications Daemon");
  g_option_context_add_main_entries (option_context, entries, NULL);
  if (!g_option_context_parse (option_context, &argc, &argv, &error))
    {
      g_printerr ("%s: %s\n", argv[0], error->message);
      g_error_free (error);
      exit_value = EXIT_FAILURE;
      goto exit;
    }

  /* deamonize */
  if (!opt_no_daemon)
    daemonize();

  /* open syslog */
#ifndef HAVE_SYSTEMD
  openlog ("GitHub Notifications Daemon", LOG_NOWAIT|LOG_PID, LOG_USER);
#endif

  /* initialize mainloop */
  mainloop = g_main_loop_new (NULL, FALSE);

  /* initialize libnotify */
  notify_init ("GitHub Notifications Daemon");

  /* handle SIGINT */
  signal_id = g_unix_signal_add (SIGINT, sigint_handler, NULL);

  /* check notifications server capabilities */
  server_caps = notify_get_server_caps();
  if (!server_caps)
    {
      print_log (LOG_ERR, "failed to obtain server caps\n");
      exit_value = EXIT_FAILURE;
      goto exit;
    }

  g_list_foreach (server_caps, set_server_caps, NULL);
  g_list_foreach (server_caps, (GFunc)g_free, NULL);
  g_list_free (server_caps);

  /* ask notification-server for some additional info */
  if (!notify_get_server_info (&name, &vendor, &version, &spec_version))
    {
      print_log (LOG_ERR, "failed to receive info about notification server\n");
      exit_value = EXIT_FAILURE;
      goto exit;
    }
  print_log (LOG_INFO, "notification-server: name=%s vendor=%s version=%s spec_version=%s\n",
             name, vendor, version, spec_version);

  /* check polling interval value */
  if (opt_interval < 45)
    {
      print_log (LOG_ERR, "minimal polling interval value is 45 seconds\n");
      opt_interval = 45;
    }

  /* set 'check_github_notifications' callback function */
  if (!g_timeout_add_seconds (opt_interval, check_github_notifications, NULL))
    {
      print_log (LOG_ERR, "can't set 'check_github_notifications' callback fuction\n");
      exit_value = EXIT_FAILURE;
      goto exit;
    }

  /* enter to mainloop */
  print_log (LOG_INFO, "mainloop: polling interval=%dsec\n", opt_interval);
  g_main_loop_run (mainloop);

exit:

  /* it's over - let's go home */
  print_log (LOG_INFO, "it's over - let's go home\n");

  if (signal_id > 0)
    g_source_remove (signal_id);
  if (option_context != NULL)
    g_option_context_free (option_context);
  if (mainloop)
    g_main_loop_unref(mainloop);
  if (notify_is_initted())
    notify_uninit();

#ifndef HAVE_SYSTEMD
  closelog();
#endif

  return exit_value;
}
