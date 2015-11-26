/*
 * Copyright © 2014 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include "libgsystem.h"
#include "libglnx/libglnx.h"

#include "xdg-app-builtins.h"
#include "xdg-app-utils.h"
#include "xdg-app-dbus.h"
#include "xdg-app-run.h"

static char *opt_arch;
static char *opt_branch;
static char *opt_command;
static gboolean opt_devel;
static char *opt_runtime;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, "Arch to use", "ARCH" },
  { "command", 0, 0, G_OPTION_ARG_STRING, &opt_command, "Command to run", "COMMAND" },
  { "branch", 0, 0, G_OPTION_ARG_STRING, &opt_branch, "Branch to use", "BRANCH" },
  { "devel", 'd', 0, G_OPTION_ARG_NONE, &opt_devel, "Use development runtime", NULL },
  { "runtime", 0, 0, G_OPTION_ARG_STRING, &opt_runtime, "Runtime to use", "RUNTIME" },
  { NULL }
};

static void
dbus_spawn_child_setup (gpointer user_data)
{
  int fd = GPOINTER_TO_INT (user_data);
  fcntl (fd, F_SETFD, 0);
}

gboolean
xdg_app_builtin_run (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(XdgAppDeploy) app_deploy = NULL;
  g_autoptr(XdgAppDeploy) runtime_deploy = NULL;
  g_autoptr(GFile) app_files = NULL;
  g_autoptr(GFile) runtime_files = NULL;
  g_autoptr(GFile) app_id_dir = NULL;
  g_autoptr(GFile) app_cache_dir = NULL;
  g_autoptr(GFile) app_data_dir = NULL;
  g_autoptr(GFile) app_config_dir = NULL;
  g_autoptr(GFile) home = NULL;
  g_autoptr(GFile) user_font1 = NULL;
  g_autoptr(GFile) user_font2 = NULL;
  g_autoptr(XdgAppSessionHelper) session_helper = NULL;
  g_autofree char *runtime = NULL;
  g_autofree char *default_command = NULL;
  g_autofree char *runtime_ref = NULL;
  g_autofree char *app_ref = NULL;
  g_autofree char *doc_mount_path = NULL;
  g_autoptr(GKeyFile) metakey = NULL;
  g_autoptr(GKeyFile) runtime_metakey = NULL;
  g_autoptr(GPtrArray) argv_array = NULL;
  g_auto(GStrv) envp = NULL;
  g_autoptr(GPtrArray) dbus_proxy_argv = NULL;
  g_autofree char *monitor_path = NULL;
  const char *app;
  const char *branch = "master";
  const char *command = "/bin/sh";
  int i;
  int rest_argv_start, rest_argc;
  int sync_proxy_pipes[2];
  g_autoptr(XdgAppContext) arg_context = NULL;
  g_autoptr(XdgAppContext) app_context = NULL;
  g_autoptr(XdgAppContext) overrides = NULL;
  g_autoptr(GDBusConnection) session_bus = NULL;

  context = g_option_context_new ("APP [args...] - Run an app");

  rest_argc = 0;
  for (i = 1; i < argc; i++)
    {
      /* The non-option is the command, take it out of the arguments */
      if (argv[i][0] != '-')
        {
          rest_argv_start = i;
          rest_argc = argc - i;
          argc = i;
          break;
        }
    }

  arg_context = xdg_app_context_new ();
  g_option_context_add_group (context, xdg_app_context_get_options (arg_context));

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, XDG_APP_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (rest_argc == 0)
    return usage_error (context, "APP must be specified", error);

  app = argv[rest_argv_start];

  if (opt_branch)
    branch = opt_branch;

  if (!xdg_app_is_valid_name (app))
    return xdg_app_fail (error, "'%s' is not a valid application name", app);

  if (!xdg_app_is_valid_branch (branch))
    return xdg_app_fail (error, "'%s' is not a valid branch name", branch);

  app_ref = xdg_app_build_app_ref (app, branch, opt_arch);

  app_deploy = xdg_app_find_deploy_for_ref (app_ref, cancellable, error);
  if (app_deploy == NULL)
    return FALSE;

  metakey = xdg_app_deploy_get_metadata (app_deploy);

  argv_array = g_ptr_array_new_with_free_func (g_free);
  dbus_proxy_argv = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (argv_array, g_strdup (HELPER));
  g_ptr_array_add (argv_array, g_strdup ("-l"));

  if (!xdg_app_run_add_extension_args (argv_array, metakey, app_ref, cancellable, error))
    return FALSE;

  if (opt_runtime)
    runtime = opt_runtime;
  else
    {
      runtime = g_key_file_get_string (metakey, "Application", opt_devel ? "sdk" : "runtime", error);
      if (*error)
        return FALSE;
    }

  runtime_ref = g_build_filename ("runtime", runtime, NULL);

  runtime_deploy = xdg_app_find_deploy_for_ref (runtime_ref, cancellable, error);
  if (runtime_deploy == NULL)
    return FALSE;

  runtime_metakey = xdg_app_deploy_get_metadata (runtime_deploy);

  app_context = xdg_app_context_new ();
  xdg_app_context_set_session_bus_policy (app_context, "org.freedesktop.portal.Desktop", XDG_APP_POLICY_TALK);
  xdg_app_context_set_session_bus_policy (app_context, "org.freedesktop.portal.Documents", XDG_APP_POLICY_TALK);

  if (!xdg_app_context_load_metadata (app_context, runtime_metakey, error))
    return FALSE;
  if (!xdg_app_context_load_metadata (app_context, metakey, error))
    return FALSE;

  overrides = xdg_app_deploy_get_overrides (app_deploy);
  xdg_app_context_merge (app_context, overrides);

  xdg_app_context_merge (app_context, arg_context);

    {
      g_autofree char *tmp_path = NULL;
      g_autofree char *path = NULL;
      int fd;

      fd = g_file_open_tmp ("xdg-app-context-XXXXXX", &tmp_path, NULL);
      if (fd >= 0)
        {
          g_autoptr(GKeyFile) keyfile = NULL;

          close (fd);

          keyfile = g_key_file_new ();

          g_key_file_set_string (keyfile, "Application", "name", app);
          g_key_file_set_string (keyfile, "Application", "runtime", runtime_ref);

          xdg_app_context_save_metadata (app_context, keyfile);

          if (!g_key_file_save_to_file (keyfile, tmp_path, error))
            return FALSE;

          g_ptr_array_add (argv_array, g_strdup ("-M"));
          g_ptr_array_add (argv_array, g_strdup_printf ("/run/user/%d/xdg-app-info=%s", getuid(), tmp_path));
        }
    }

  if (!xdg_app_run_add_extension_args (argv_array, runtime_metakey, runtime_ref, cancellable, error))
    return FALSE;

  if ((app_id_dir = xdg_app_ensure_data_dir (app, cancellable, error)) == NULL)
      return FALSE;

  app_cache_dir = g_file_get_child (app_id_dir, "cache");
  g_ptr_array_add (argv_array, g_strdup ("-B"));
  g_ptr_array_add (argv_array, g_strdup_printf ("/var/cache=%s", gs_file_get_path_cached (app_cache_dir)));

  app_data_dir = g_file_get_child (app_id_dir, "data");
  g_ptr_array_add (argv_array, g_strdup ("-B"));
  g_ptr_array_add (argv_array, g_strdup_printf ("/var/data=%s", gs_file_get_path_cached (app_data_dir)));

  app_config_dir = g_file_get_child (app_id_dir, "config");
  g_ptr_array_add (argv_array, g_strdup ("-B"));
  g_ptr_array_add (argv_array, g_strdup_printf ("/var/config=%s", gs_file_get_path_cached (app_config_dir)));

  app_files = xdg_app_deploy_get_files (app_deploy);
  runtime_files = xdg_app_deploy_get_files (runtime_deploy);

  default_command = g_key_file_get_string (metakey, "Application", "command", error);
  if (*error)
    return FALSE;
  if (opt_command)
    command = opt_command;
  else
    command = default_command;

  session_helper = xdg_app_session_helper_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
								  G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
								  "org.freedesktop.XdgApp",
								  "/org/freedesktop/XdgApp/SessionHelper",
								  NULL, NULL);
  if (session_helper &&
      xdg_app_session_helper_call_request_monitor_sync (session_helper,
                                                        &monitor_path,
                                                        NULL, NULL))
    {
      g_ptr_array_add (argv_array, g_strdup ("-m"));
      g_ptr_array_add (argv_array, monitor_path);
    }
  else
    g_ptr_array_add (argv_array, g_strdup ("-r"));

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  if (session_bus)
    {
      g_autoptr (GError) local_error = NULL;
      g_autoptr (GDBusMessage) reply = NULL;
      g_autoptr (GDBusMessage) msg = g_dbus_message_new_method_call ("org.freedesktop.portal.Documents",
                                                                     "/org/freedesktop/portal/documents",
                                                                     "org.freedesktop.portal.Documents",
                                                                     "GetMountPoint");
      g_dbus_message_set_body (msg, g_variant_new ("()"));
      reply = g_dbus_connection_send_message_with_reply_sync (session_bus, msg,
                                                              G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                                              30000,
                                                              NULL,
                                                              NULL,
                                                              NULL);
      if (reply)
        {
          if (g_dbus_message_to_gerror (reply, &local_error))
            {
              g_warning ("Can't get document portal: %s\n", local_error->message);
            }
          else
            g_variant_get (g_dbus_message_get_body (reply),
                           "(^ay)", &doc_mount_path);
        }
    }

  xdg_app_run_add_environment_args (argv_array, dbus_proxy_argv, doc_mount_path,
                                    app, app_context, app_id_dir);

  g_ptr_array_add (argv_array, g_strdup ("-b"));
  g_ptr_array_add (argv_array, g_strdup_printf ("/run/host/fonts=%s", SYSTEM_FONTS_DIR));

  if (opt_devel)
    g_ptr_array_add (argv_array, g_strdup ("-c"));

  home = g_file_new_for_path (g_get_home_dir ());
  user_font1 = g_file_resolve_relative_path (home, ".local/share/fonts");
  user_font2 = g_file_resolve_relative_path (home, ".fonts");

  if (g_file_query_exists (user_font1, NULL))
    {
      g_autofree char *path = g_file_get_path (user_font1);
      g_ptr_array_add (argv_array, g_strdup ("-b"));
      g_ptr_array_add (argv_array, g_strdup_printf ("/run/host/user-fonts=%s", path));
    }
  else if (g_file_query_exists (user_font2, NULL))
    {
      g_autofree char *path = g_file_get_path (user_font2);
      g_ptr_array_add (argv_array, g_strdup ("-b"));
      g_ptr_array_add (argv_array, g_strdup_printf ("/run/host/user-fonts=%s", path));
    }

  /* Must run this before spawning the dbus proxy, to ensure it
     ends up in the app cgroup */
  xdg_app_run_in_transient_unit (app);

  if (dbus_proxy_argv->len > 0)
    {
      char x;

      if (pipe (sync_proxy_pipes) < 0)
	{
	  g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "Unable to create sync pipe");
	  return FALSE;
	}

      g_ptr_array_insert (dbus_proxy_argv, 0, g_strdup (DBUSPROXY));
      g_ptr_array_insert (dbus_proxy_argv, 1, g_strdup_printf ("--fd=%d", sync_proxy_pipes[1]));

      g_ptr_array_add (dbus_proxy_argv, NULL); /* NULL terminate */

      if (!g_spawn_async (NULL,
			  (char **)dbus_proxy_argv->pdata,
			  NULL,
			  G_SPAWN_SEARCH_PATH,
			  dbus_spawn_child_setup,
			  GINT_TO_POINTER (sync_proxy_pipes[1]),
			  NULL, error))
	return FALSE;

      close (sync_proxy_pipes[1]);

      /* Sync with proxy, i.e. wait until its listening on the sockets */
      if (read (sync_proxy_pipes[0], &x, 1) != 1)
	{
	  g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "Failed to sync with dbus proxy");
	  return FALSE;
	}

      g_ptr_array_add (argv_array, g_strdup ("-S"));
      g_ptr_array_add (argv_array, g_strdup_printf ("%d", sync_proxy_pipes[0]));
    }

  g_ptr_array_add (argv_array, g_strdup ("-a"));
  g_ptr_array_add (argv_array, g_file_get_path (app_files));
  g_ptr_array_add (argv_array, g_strdup ("-I"));
  g_ptr_array_add (argv_array, g_strdup (app));
  g_ptr_array_add (argv_array, g_file_get_path (runtime_files));

  g_ptr_array_add (argv_array, g_strdup (command));
  for (i = 1; i < rest_argc; i++)
    g_ptr_array_add (argv_array, g_strdup (argv[rest_argv_start + i]));

  g_ptr_array_add (argv_array, NULL);

  envp = g_get_environ ();
  envp = xdg_app_run_apply_env_default (envp);

  envp = xdg_app_run_apply_env_vars (envp, app_context);

  envp = xdg_app_run_apply_env_appid (envp, app_id_dir);

  if (execvpe (HELPER, (char **)argv_array->pdata, envp) == -1)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "Unable to start app");
      return FALSE;
    }

  /* Not actually reached... */
  return TRUE;
}
