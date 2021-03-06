/* builder-source-git.c
 *
 * Copyright (C) 2015 Red Hat, Inc
 *
 * This file is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/statfs.h>

#include "builder-utils.h"
#include "libgsystem.h"

#include "builder-source-git.h"
#include "builder-utils.h"

struct BuilderSourceGit {
  BuilderSource parent;

  char *url;
  char *branch;
};

typedef struct {
  BuilderSourceClass parent_class;
} BuilderSourceGitClass;

G_DEFINE_TYPE (BuilderSourceGit, builder_source_git, BUILDER_TYPE_SOURCE);

enum {
  PROP_0,
  PROP_URL,
  PROP_BRANCH,
  LAST_PROP
};

static gboolean git_mirror_repo (const char *repo_url,
                                 const char *ref,
                                 BuilderContext *context,
                                 GError **error);


static void
builder_source_git_finalize (GObject *object)
{
  BuilderSourceGit *self = (BuilderSourceGit *)object;

  g_free (self->url);
  g_free (self->branch);

  G_OBJECT_CLASS (builder_source_git_parent_class)->finalize (object);
}

static void
builder_source_git_get_property (GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  BuilderSourceGit *self = BUILDER_SOURCE_GIT (object);

  switch (prop_id)
    {
    case PROP_URL:
      g_value_set_string (value, self->url);
      break;

    case PROP_BRANCH:
      g_value_set_string (value, self->branch);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
builder_source_git_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  BuilderSourceGit *self = BUILDER_SOURCE_GIT (object);

  switch (prop_id)
    {
    case PROP_URL:
      g_free (self->url);
      self->url = g_value_dup_string (value);
      break;

    case PROP_BRANCH:
      g_free (self->branch);
      self->branch = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

typedef struct
{
  GError *error;
  GError *splice_error;
  GMainLoop *loop;
  int refs;
} GitData;

static gboolean git (GFile *dir,
                     char **output,
                     GError **error,
                     const gchar            *argv1,
                     ...) G_GNUC_NULL_TERMINATED;


static void
git_data_exit (GitData *data)
{
  data->refs--;
  if (data->refs == 0)
    g_main_loop_quit (data->loop);
}

static void
git_output_spliced_cb (GObject    *obj,
                       GAsyncResult  *result,
                       gpointer       user_data)
{
  GitData *data = user_data;

  g_output_stream_splice_finish (G_OUTPUT_STREAM (obj), result, &data->splice_error);
  git_data_exit (data);
}

static void
git_exit_cb (GObject    *obj,
             GAsyncResult  *result,
             gpointer       user_data)
{
  GitData *data = user_data;

  g_subprocess_wait_check_finish (G_SUBPROCESS (obj), result, &data->error);
  git_data_exit (data);
}

static gboolean
git (GFile        *dir,
     char        **output,
     GError      **error,
     const gchar  *argv1,
     ...)
{
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  g_autoptr(GSubprocess) subp = NULL;
  GPtrArray *args;
  const gchar *arg;
  GInputStream *in;
  g_autoptr(GOutputStream) out = NULL;
  g_autoptr(GMainLoop) loop = NULL;
  va_list ap;
  GitData data = {0};

  args = g_ptr_array_new ();
  g_ptr_array_add (args, "git");
  va_start (ap, argv1);
  g_ptr_array_add (args, (gchar *) argv1);
  while ((arg = va_arg (ap, const gchar *)))
    g_ptr_array_add (args, (gchar *) arg);
  g_ptr_array_add (args, NULL);
  va_end (ap);

  launcher = g_subprocess_launcher_new (0);

  if (output)
    g_subprocess_launcher_set_flags (launcher, G_SUBPROCESS_FLAGS_STDOUT_PIPE);

  if (dir)
    {
      g_autofree char *path = g_file_get_path (dir);
      g_subprocess_launcher_set_cwd (launcher, path);
    }

  subp = g_subprocess_launcher_spawnv (launcher, (const gchar * const *) args->pdata, error);
  g_ptr_array_free (args, TRUE);

  if (subp == NULL)
    return FALSE;

  loop = g_main_loop_new (NULL, FALSE);

  data.loop = loop;
  data.refs = 1;

  if (output)
    {
      data.refs++;
      in = g_subprocess_get_stdout_pipe (subp);
      out = g_memory_output_stream_new_resizable ();
      g_output_stream_splice_async  (out,
                                     in,
                                     G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                     0,
                                     NULL,
                                     git_output_spliced_cb,
                                     &data);
    }

  g_subprocess_wait_async (subp, NULL, git_exit_cb, &data);

  g_main_loop_run (loop);

  if (data.error)
    {
      g_propagate_error (error, data.error);
      g_clear_error (&data.splice_error);
      return FALSE;
    }

  if (out)
    {
      if (data.splice_error)
        {
          g_propagate_error (error, data.splice_error);
          return FALSE;
        }

      /* Null terminate */
      g_output_stream_write (out, "\0", 1, NULL, NULL);
      *output = g_memory_output_stream_steal_data (G_MEMORY_OUTPUT_STREAM (out));
    }

  return TRUE;
}

static GFile *
git_get_mirror_dir (const char *url,
                    BuilderContext *context)
{
  g_autoptr(GFile) git_dir = NULL;
  g_autofree char *filename = NULL;
  g_autofree char *git_dir_path = NULL;

  git_dir = g_file_get_child (builder_context_get_state_dir (context),
                              "git");

  git_dir_path = g_file_get_path (git_dir);
  g_mkdir_with_parents (git_dir_path, 0755);

  filename = builder_uri_to_filename (url);
  return g_file_get_child (git_dir, filename);
}

static const char *
get_branch (BuilderSourceGit *self)
{
  if (self->branch)
    return self->branch;
  else
    return "master";
}

static char *
git_get_current_commit (GFile *repo_dir,
                        const char *branch,
                        BuilderContext *context,
                        GError **error)
{
  char *output = NULL;

  if (!git (repo_dir, &output, error,
            "rev-parse", branch, NULL))
    return NULL;

  /* Trim trailing whitespace */
  g_strchomp (output);

  return output;
}

char *
make_absolute_url (const char *orig_parent, const char *orig_relpath, GError **error)
{
  g_autofree char *parent = g_strdup (orig_parent);
  const char *relpath = orig_relpath;
  char *method;
  char *parent_path;

  if (!g_str_has_prefix (relpath, "../"))
    return g_strdup (orig_relpath);

  if (parent[strlen(parent) - 1] == '/')
    parent[strlen(parent) - 1] = 0;

  method = strstr (parent, "://");
  if (method == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid uri %s", orig_parent);
      return NULL;
    }

  parent_path = strchr (method + 3, '/');
  if (parent_path == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid uri %s", orig_parent);
      return NULL;
    }

  while (g_str_has_prefix (relpath, "../"))
    {
      char *last_slash = strrchr (parent_path, '/');
      if (last_slash == NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid relative path %s for uri %s", orig_relpath, orig_parent);
          return NULL;
        }
      relpath += 3;
      *last_slash = 0;
    }

  return g_strconcat (parent, "/", relpath, NULL);
}

static gboolean
git_mirror_submodules (const char *repo_url,
                       GFile *mirror_dir,
                       const char *revision,
                       BuilderContext *context,
                       GError **error)
{
  g_autofree char *mirror_dir_path = NULL;
  g_autoptr(GFile) checkout_dir_template = NULL;
  g_autoptr(GFile) checkout_dir = NULL;
  g_autofree char *checkout_dir_path = NULL;
  g_autofree char *submodule_status = NULL;

  mirror_dir_path = g_file_get_path (mirror_dir);

  checkout_dir_template = g_file_get_child (builder_context_get_state_dir (context),
                                            "tmp-checkout-XXXXXX");
  checkout_dir_path = g_file_get_path (checkout_dir_template);

  if (g_mkdtemp (checkout_dir_path) == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Can't create temporary checkout directory");
      return FALSE;
    }

  checkout_dir = g_file_new_for_path (checkout_dir_path);

  if (!git (NULL, NULL, error,
            "clone", "-q", "--no-checkout", mirror_dir_path, checkout_dir_path, NULL))
    return FALSE;

  if (!git (checkout_dir, NULL, error, "checkout", "-q", "-f", revision, NULL))
    return FALSE;

  if (!git (checkout_dir, &submodule_status, error,
            "submodule", "status", NULL))
    return FALSE;

  if (submodule_status)
    {
      int i;
      g_auto(GStrv) lines = g_strsplit (submodule_status, "\n", -1);
      for (i = 0; lines[i] != NULL; i++)
        {
          g_autofree char *url = NULL;
          g_autofree char *option = NULL;
          g_autofree char *old = NULL;
          g_auto(GStrv) words = NULL;
          if (*lines[i] == 0)
            continue;
          words = g_strsplit (lines[i] + 1, " ", 3);

          option = g_strdup_printf ("submodule.%s.url", words[1]);
          if (!git (checkout_dir, &url, error,
                    "config", "-f", ".gitmodules", option, NULL))
            return FALSE;
          /* Trim trailing whitespace */
          g_strchomp (url);

          old = url;
          url = make_absolute_url (repo_url, old, error);
          if (url == NULL)
            return FALSE;

          if (!git_mirror_repo (url, words[0], context, error))
            return FALSE;
        }
    }

  if (!gs_shutil_rm_rf (checkout_dir, NULL, error))
    return FALSE;

  return TRUE;
}

static gboolean
git_mirror_repo (const char *repo_url,
                 const char *ref,
                 BuilderContext *context,
                 GError **error)
{
  g_autoptr(GFile) mirror_dir = NULL;
  g_autofree char *current_commit = NULL;

  mirror_dir = git_get_mirror_dir (repo_url, context);

  if (!g_file_query_exists (mirror_dir, NULL))
    {
      g_autofree char *filename = g_file_get_basename (mirror_dir);
      g_autoptr(GFile) parent = g_file_get_parent (mirror_dir);
      g_autofree char *filename_tmp = g_strconcat (filename, ".clone_tmp", NULL);
      g_autoptr(GFile) mirror_dir_tmp = g_file_get_child (parent, filename_tmp);

      if (!git (parent, NULL, error,
                "clone", "--mirror", repo_url,  filename_tmp, NULL) ||
          !g_file_move (mirror_dir_tmp, mirror_dir, 0, NULL, NULL, NULL, error))
        return FALSE;
    }
  else
    {
      if (!git (mirror_dir, NULL, error,
                "fetch", NULL))
        return FALSE;
    }

  current_commit = git_get_current_commit (mirror_dir, ref, context, error);
  if (current_commit == NULL)
    return FALSE;

  if (!git_mirror_submodules (repo_url, mirror_dir, current_commit, context, error))
    return FALSE;

  return TRUE;
}

static gboolean
builder_source_git_download (BuilderSource *source,
                             BuilderContext *context,
                             GError **error)
{
  BuilderSourceGit *self = BUILDER_SOURCE_GIT (source);
  g_autoptr(GFile) mirror_dir = NULL;

  if (self->url == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "URL not specified");
      return FALSE;
    }

  if (!git_mirror_repo (self->url,
                        get_branch (self),
                        context,
                        error))
    return FALSE;

  return TRUE;
}

static gboolean
git_extract_submodule (const char *repo_url,
                       GFile *checkout_dir,
                       BuilderContext *context,
                       GError **error)
{
  g_autofree char *submodule_status = NULL;

  if (!git (checkout_dir, &submodule_status, error,
            "submodule", "status", NULL))
    return FALSE;

  if (submodule_status)
    {
      int i;
      g_auto(GStrv) lines = g_strsplit (submodule_status, "\n", -1);
      for (i = 0; lines[i] != NULL; i++)
        {
          g_autoptr(GFile) mirror_dir = NULL;
          g_autoptr(GFile) child_dir = NULL;
          g_autofree char *child_url = NULL;
          g_autofree char *option = NULL;
          g_autofree char *child_relative_url = NULL;
          g_autofree char *mirror_dir_as_url = NULL;
          g_auto(GStrv) words = NULL;
          if (*lines[i] == 0)
            continue;
          words = g_strsplit (lines[i] + 1, " ", 3);

          option = g_strdup_printf ("submodule.%s.url", words[1]);
          if (!git (checkout_dir, &child_relative_url, error,
                    "config", "-f", ".gitmodules", option, NULL))
            return FALSE;

          /* Trim trailing whitespace */
          g_strchomp (child_relative_url);

          g_print ("processing submodule %s\n", words[1]);

          child_url = make_absolute_url (repo_url, child_relative_url, error);
          if (child_url == NULL)
            return FALSE;

          mirror_dir = git_get_mirror_dir (child_url, context);

          mirror_dir_as_url = g_file_get_uri (mirror_dir);

          if (!git (checkout_dir, NULL, error,
                    "config", option, mirror_dir_as_url, NULL))
            return FALSE;

          if (!git (checkout_dir, NULL, error,
                    "submodule", "update", "--init", words[1], NULL))
            return FALSE;

          child_dir = g_file_resolve_relative_path (checkout_dir, words[1]);

          if (!git_extract_submodule (child_url, child_dir, context, error))
            return FALSE;
        }
    }

  return TRUE;
}

static gboolean
builder_source_git_extract (BuilderSource *source,
                            GFile *dest,
                            BuilderContext *context,
                            GError **error)
{
  BuilderSourceGit *self = BUILDER_SOURCE_GIT (source);
  g_autoptr(GFile) mirror_dir = NULL;
  g_autofree char *mirror_dir_path = NULL;
  g_autofree char *dest_path = NULL;

  mirror_dir = git_get_mirror_dir (self->url, context);

  mirror_dir_path = g_file_get_path (mirror_dir);
  dest_path = g_file_get_path (dest);

  if (!git (NULL, NULL, error,
            "clone", "--branch", get_branch (self), mirror_dir_path, dest_path, NULL))
    return FALSE;

  if (!git_extract_submodule (self->url, dest, context, error))
    return FALSE;

  return TRUE;
}

static void
builder_source_git_checksum (BuilderSource  *source,
                             BuilderCache   *cache,
                             BuilderContext *context)
{
  BuilderSourceGit *self = BUILDER_SOURCE_GIT (source);
  g_autoptr(GFile) mirror_dir = NULL;
  g_autofree char *current_commit;
  g_autoptr(GError) error = NULL;

  builder_cache_checksum_str (cache, self->url);
  builder_cache_checksum_str (cache, self->branch);

  mirror_dir = git_get_mirror_dir (self->url, context);

  current_commit = git_get_current_commit (mirror_dir, get_branch (self), context, &error);
  if (current_commit)
    builder_cache_checksum_str (cache, current_commit);
  else if (error)
    g_warning ("Failed to get current git checksum: %s", error->message);
}

static void
builder_source_git_class_init (BuilderSourceGitClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  BuilderSourceClass *source_class = BUILDER_SOURCE_CLASS (klass);

  object_class->finalize = builder_source_git_finalize;
  object_class->get_property = builder_source_git_get_property;
  object_class->set_property = builder_source_git_set_property;

  source_class->download = builder_source_git_download;
  source_class->extract = builder_source_git_extract;
  source_class->checksum = builder_source_git_checksum;

  g_object_class_install_property (object_class,
                                   PROP_URL,
                                   g_param_spec_string ("url",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_BRANCH,
                                   g_param_spec_string ("branch",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
}

static void
builder_source_git_init (BuilderSourceGit *self)
{
}
