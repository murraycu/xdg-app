#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "libglnx/libglnx.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "document-portal/xdp-dbus.h"

#include "xdg-app-dbus.h"

char outdir[] = "/tmp/xdp-test-XXXXXX";

GTestDBus *dbus;
GDBusConnection *session_bus;
XdpDbusDocuments *documents;
char *mountpoint;

static char *
make_doc_dir (const char *id, const char *app)
{
  if (app)
    return g_build_filename (mountpoint, "by-app", app, id, NULL);
  else
    return g_build_filename (mountpoint, id, NULL);
}

static char *
make_doc_path (const char *id, const char *basename, const char *app)
{
  g_autofree char *dir = make_doc_dir (id, app);
  return g_build_filename (dir, basename, NULL);
}

static void
assert_doc_has_contents (const char *id, const char *basename, const char *app, const char *expected_contents)
{
  g_autofree char *path = make_doc_path (id, basename, app);
  g_autofree char *real_contents = NULL;
  gsize real_contents_length;
  GError *error = NULL;

  g_file_get_contents (path, &real_contents, &real_contents_length, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (real_contents, ==, expected_contents);
  g_assert_cmpuint (real_contents_length, ==, strlen (expected_contents));
}

static void
assert_doc_not_exist (const char *id, const char *basename, const char *app)
{
  g_autofree char *path = make_doc_path (id, basename, app);
  struct stat buf;
  int res, fd;

  res = stat (path, &buf);
  g_assert_cmpint (res, ==, -1);
  g_assert_cmpint (errno, ==, ENOENT);

  fd = open (path, O_RDONLY);
  g_assert_cmpint (fd, ==, -1);
  g_assert_cmpint (errno, ==, ENOENT);
}

static char *
export_file (const char *path, gboolean unique)
{
  int fd, fd_id;
  GUnixFDList *fd_list = NULL;
  g_autoptr(GVariant) reply = NULL;
  GError *error = NULL;
  char *doc_id;

  fd = open (path, O_PATH | O_CLOEXEC);
  g_assert (fd >= 0);

  fd_list = g_unix_fd_list_new ();
  fd_id = g_unix_fd_list_append (fd_list, fd, &error);
  g_assert_no_error (error);
  close (fd);

  reply = g_dbus_connection_call_with_unix_fd_list_sync (session_bus,
                                                         "org.freedesktop.portal.Documents",
                                                         "/org/freedesktop/portal/documents",
                                                         "org.freedesktop.portal.Documents",
                                                         "Add",
                                                         g_variant_new ("(hbb)", fd_id, !unique, FALSE),
                                                         G_VARIANT_TYPE ("(s)"),
                                                         G_DBUS_CALL_FLAGS_NONE,
                                                         30000,
                                                         fd_list, NULL,
                                                         NULL,
                                                         &error);
  g_object_unref (fd_list);
  g_assert (reply != NULL);
  g_assert_no_error (error);

  g_variant_get (reply, "(s)", &doc_id);
  g_assert (doc_id != NULL);
  return doc_id;
}

static char *
export_new_file (const char *basename, const char *contents, gboolean unique)
{
  g_autofree char *path = NULL;
  g_autofree char *id = NULL;
  g_autofree char *doc_path = NULL;
  GError *error = NULL;

  path = g_build_filename (outdir, basename, NULL);

  g_file_set_contents (path, contents, -1, &error);
  g_assert_no_error (error);

  return export_file (path, unique);
}

static gboolean
update_doc (const char *id, const char *basename, const char *app, const char *contents, GError **error)
{
  g_autofree char *path = make_doc_path (id, basename, app);

  return g_file_set_contents (path, contents, -1, error);
}

static void
grant_permissions (const char *id, const char *app, gboolean write)
{
  g_autoptr(GPtrArray) permissions = g_ptr_array_new ();
  GError *error = NULL;

  g_ptr_array_add (permissions, "read");
  if (write)
    g_ptr_array_add (permissions, "write");
  g_ptr_array_add (permissions, NULL);

  xdp_dbus_documents_call_grant_permissions_sync (documents,
                                                  id,
                                                  app,
                                                  (const char **)permissions->pdata,
                                                  NULL,
                                                  &error);
  g_assert_no_error (error);
}

static void
test_create_doc (void)
{
  g_autofree char *id = NULL;
  const char *basename = "a-file";
  GError *error = NULL;

  id = export_new_file (basename, "content", FALSE);

  assert_doc_has_contents (id, basename, NULL, "content");
  assert_doc_not_exist (id, basename, "com.test.App1");
  assert_doc_not_exist (id, basename, "com.test.App2");
  assert_doc_not_exist (id, "another-file", NULL);
  assert_doc_not_exist ("anotherid", basename, NULL);

  grant_permissions (id, "com.test.App1", FALSE);

  assert_doc_has_contents (id, basename, "com.test.App1", "content");
  assert_doc_not_exist (id, basename, "com.test.App2");

  update_doc (id, basename, NULL, "content2", &error);
  g_assert_no_error (error);
  assert_doc_has_contents (id, basename, NULL, "content2");
  assert_doc_has_contents (id, basename, "com.test.App1", "content2");
  assert_doc_not_exist (id, basename, "com.test.App2");

  update_doc (id, basename, "com.test.App1", "content3", &error);
  g_assert_error (error, G_FILE_ERROR, G_FILE_ERROR_ACCES);
  g_clear_error (&error);
}

static void
test_recursive_doc (void)
{
  g_autofree char *id = NULL;
  g_autofree char *id2 = NULL;
  g_autofree char *id3 = NULL;
  const char *basename = "recursive-file";
  g_autofree char *path = NULL;
  g_autofree char *app_path = NULL;

  id = export_new_file (basename, "recursive-content", FALSE);

  assert_doc_has_contents (id, basename, NULL, "recursive-content");

  path = make_doc_path (id, basename, NULL);
  g_print ("path: %s\n", path);

  id2 = export_file (path, FALSE);

  g_assert_cmpstr (id, ==, id2);

  grant_permissions (id, "com.test.App1", FALSE);

  app_path = make_doc_path (id, basename, "com.test.App1");

  id3 = export_file (app_path, FALSE);

  g_assert_cmpstr (id, ==, id3);
}

int
main (int argc, char **argv)
{
  int res;
  gboolean inited;
  GError *error = NULL;
  gint exit_status;

  g_mkdtemp (outdir);
  g_print ("outdir: %s\n", outdir);

  g_setenv ("XDG_RUNTIME_DIR", outdir, TRUE);
  g_setenv ("XDG_DATA_HOME", outdir, TRUE);

  dbus = g_test_dbus_new (G_TEST_DBUS_NONE);
  g_test_dbus_add_service_dir (dbus, TEST_SERVICES);
  g_test_dbus_up (dbus);

  /* g_test_dbus_up unsets this, so re-set */
  g_setenv ("XDG_RUNTIME_DIR", outdir, TRUE);

  g_spawn_command_line_sync (DOC_PORTAL " -d", NULL, NULL, &exit_status, &error);
  g_assert_cmpint (exit_status, ==, 0);
  g_assert_no_error (error);

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  g_assert_no_error (error);

  documents = xdp_dbus_documents_proxy_new_sync (session_bus, 0,
                                                 "org.freedesktop.portal.Documents",
                                                 "/org/freedesktop/portal/documents",
                                                 NULL, &error);
  g_assert_no_error (error);
  g_assert (documents != NULL);

  inited = xdp_dbus_documents_call_get_mount_point_sync (documents, &mountpoint,
                                                         NULL, &error);
  g_assert_no_error (error);
  g_assert (inited);
  g_assert (mountpoint != NULL);

  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/db/create_doc", test_create_doc);
  g_test_add_func ("/db/recursive_doc", test_recursive_doc);

  res = g_test_run ();

  g_free (mountpoint);

  g_object_unref (documents);

  g_dbus_connection_close_sync (session_bus, NULL, &error);
  g_assert_no_error (error);

  g_object_unref (session_bus);

  g_test_dbus_down (dbus);

  g_object_unref (dbus);

  /* We race on the unmount of the fuse fs, which causes the rm -rf to stop at the doc dir.
     This makes the chance of completely removing the directory higher */
  sleep (1);

  glnx_shutil_rm_rf_at (-1, outdir, NULL, NULL);

  return res;
}
