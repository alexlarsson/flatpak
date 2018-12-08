/*
 * Copyright © 2018 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
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

#include "flatpak-cli-transaction.h"
#include "flatpak-transaction-private.h"
#include "flatpak-installation-private.h"
#include "flatpak-run-private.h"
#include "flatpak-table-printer.h"
#include "flatpak-utils-private.h"
#include "flatpak-error.h"
#include <glib/gi18n.h>


struct _FlatpakCliTransaction
{
  FlatpakTransaction parent;

  gboolean           disable_interaction;
  gboolean           stop_on_first_error;
  gboolean           aborted;
  GError            *first_operation_error;

  int                rows;
  int                cols;
  int                end_row;
  int                table_width;
  int                table_height;

  gboolean           progress_initialized;
  int                progress_last_width;

  int                n_ops;
  int                op;
  int                op_progress;

  gboolean           installing;
  gboolean           updating;
  gboolean           uninstalling;

  int                  download_col;

  FlatpakTablePrinter *printer;
  int                  progress_row;
  char                *progress_msg;
};

struct _FlatpakCliTransactionClass
{
  FlatpakCliTransactionClass parent_class;
};

G_DEFINE_TYPE (FlatpakCliTransaction, flatpak_cli_transaction, FLATPAK_TYPE_TRANSACTION);

static int
choose_remote_for_ref (FlatpakTransaction *transaction,
                       const char         *for_ref,
                       const char         *runtime_ref,
                       const char * const *remotes)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (transaction);
  int n_remotes = g_strv_length ((char **) remotes);
  int chosen = -1;
  const char *pref;

  pref = strchr (for_ref, '/') + 1;

  if (self->disable_interaction)
    {
      g_print (_("Required runtime for %s (%s) found in remote %s\n"),
               pref, runtime_ref, remotes[0]);
      chosen = 0;
    }
  else if (n_remotes == 1)
    {
      g_print (_("Required runtime for %s (%s) found in remote %s\n"),
               pref, runtime_ref, remotes[0]);
      if (flatpak_yes_no_prompt (TRUE, _("Do you want to install it?")))
        chosen = 0;
    }
  else
    {
      flatpak_format_choices ((const char **)remotes,
                              _("Required runtime for %s (%s) found in remotes: %s"),
                              pref, runtime_ref, remotes[0]);
      chosen = flatpak_number_prompt (TRUE, 0, n_remotes, _("Which do you want to install (0 to abort)?"));
      chosen -= 1; /* convert from base-1 to base-0 (and -1 to abort) */
    }

  return chosen;
}

static gboolean
add_new_remote (FlatpakTransaction            *transaction,
                FlatpakTransactionRemoteReason reason,
                const char                    *from_id,
                const char                    *remote_name,
                const char                    *url)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (transaction);

  if (self->disable_interaction)
    {
      g_print (_("Configuring %s as new remote '%s'"), url, remote_name);
      return TRUE;
    }

  if (reason == FLATPAK_TRANSACTION_REMOTE_GENERIC_REPO)
    {
      if (flatpak_yes_no_prompt (TRUE, /* default to yes on Enter */
                                 _("The remote '%s', refered to by '%s' at location %s contains additional applications.\n"
                                   "Should the remote be kept for future installations?"),
                                 remote_name, from_id, url))
        return TRUE;
    }
  else if (reason == FLATPAK_TRANSACTION_REMOTE_RUNTIME_DEPS)
    {
      if (flatpak_yes_no_prompt (TRUE, /* default to yes on Enter */
                                 _("The application %s depends on runtimes from:\n  %s\n"
                                   "Configure this as new remote '%s'"),
                                 from_id, url, remote_name))
        return TRUE;
    }

  return FALSE;
}

static char *
op_type_to_string (FlatpakTransactionOperationType operation_type)
{
  switch (operation_type)
    {
    case FLATPAK_TRANSACTION_OPERATION_INSTALL:
      return _("install");

    case FLATPAK_TRANSACTION_OPERATION_UPDATE:
      return _("update");

    case FLATPAK_TRANSACTION_OPERATION_INSTALL_BUNDLE:
      return _("install bundle");

    case FLATPAK_TRANSACTION_OPERATION_UNINSTALL:
      return _("uninstall");

    default:
      return "Unknown type"; /* Should not happen */
    }
}

#define BAR_LENGTH 20
#define BAR_CHARS " -=#"

static void
redraw (FlatpakCliTransaction *self)
{
  int top;
  int row;
  int col;
  int skip;

  top = self->end_row - self->table_height;
  if (top > 0)
    {
      row = top;
      skip = 0;
    }
  else
    {
      row = 1;
      skip = 1 - top;
    }

  g_print (FLATPAK_ANSI_ROW_N FLATPAK_ANSI_CLEAR, row);
  // we update table_height and end_row here, since we might have added to the table
  flatpak_table_printer_print_full (self->printer, skip, self->cols,
                                    &self->table_height, &self->table_width);
  flatpak_get_cursor_pos (&self->end_row, &col);
  self->end_row += 1;
}

static void
set_op_progress (FlatpakCliTransaction *self,
                 FlatpakTransactionOperation *op,
                 char progress)
{
  if (flatpak_fancy_output ())
    {
      int row = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (op), "row"));
      char cell[] = "[ ]";

      cell[1] = progress;
      flatpak_table_printer_set_cell (self->printer, row, 0, cell);
    }
}

static void
spin_op_progress (FlatpakCliTransaction *self,
                  FlatpakTransactionOperation *op)
{
  const char p[] = "/-\\|-";

  set_op_progress (self, op, p[self->op_progress++ % strlen (p)]);
}

static void
progress_changed_cb (FlatpakTransactionProgress *progress,
                     gpointer                    data)
{
  FlatpakCliTransaction *cli = data;
  FlatpakTransaction *self = FLATPAK_TRANSACTION (cli);
  FlatpakTransactionOperation *op = flatpak_transaction_get_current_operation (self);

  g_autoptr(GString) str = g_string_new ("");
  int i;
  int n_full, remainder, partial;
  int width, padded_width;
  g_autofree char *text = NULL;

  guint percent = flatpak_transaction_progress_get_progress (progress);
  g_autofree char *status = flatpak_transaction_progress_get_status (progress);

  spin_op_progress (cli, op);

  if (!cli->progress_initialized)
    {
      cli->progress_last_width = 0;
      cli->progress_initialized = TRUE;
    }

  g_string_append (str, cli->progress_msg);
  g_string_append (str, " [");

  n_full = (BAR_LENGTH * percent) / 100;
  remainder = percent - (n_full * 100 / BAR_LENGTH);
  partial = (remainder * strlen (BAR_CHARS) * BAR_LENGTH) / 100;

  for (i = 0; i < n_full; i++)
    g_string_append_c (str, BAR_CHARS[strlen (BAR_CHARS) - 1]);

  if (i < BAR_LENGTH)
    {
      g_string_append_c (str, BAR_CHARS[partial]);
      i++;
    }

  for (; i < BAR_LENGTH; i++)
    g_string_append (str, " ");

  g_string_append (str, "] ");
  g_string_append_printf (str, "%d%%", percent);

  if (g_str_has_suffix (status, ")"))
    {
      char *p = strrchr (status, '(');
      g_autofree char *speed = g_strndup (p + 1, strlen (p) - 2);
      g_string_append_printf (str, " %s", speed);
    }

  width = MIN (strlen (str->str), cli->cols);
  padded_width = MAX (cli->progress_last_width, width);
  cli->progress_last_width = width;
  text = g_strdup_printf ("%-*.*s", padded_width, padded_width, str->str);
  if (flatpak_fancy_output ())
    {
      flatpak_table_printer_set_cell (cli->printer, cli->progress_row, 0, text);
      if (flatpak_transaction_operation_get_operation_type (op) != FLATPAK_TRANSACTION_OPERATION_UNINSTALL)
        {
          guint64 max;
          guint64 transferred;
          g_autofree char *formatted_max = NULL;
          g_autofree char *formatted = NULL;
          g_autofree char *text = NULL;
          int row;

          max = flatpak_transaction_operation_get_download_size (op);
          formatted_max = g_format_size (max);
          transferred = flatpak_transaction_progress_get_bytes_transferred (progress);
          if (transferred < 1024) // avoid "bytes"
            formatted = g_strdup ("0.0 kB");
          else
            formatted = g_format_size (transferred);
          text = g_strdup_printf ("%s / %s", formatted, formatted_max);
          row = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (op), "row"));
          flatpak_table_printer_set_decimal_cell (cli->printer, row, cli->download_col, text);
        }
      redraw (cli);
    }
  else
    {
      g_print ("\r%s", text);
    }
}

static void
set_progress (FlatpakCliTransaction *self,
              const char *text)
{
  flatpak_table_printer_set_cell (self->printer, self->progress_row, 0, text);
}

static void
new_operation (FlatpakTransaction          *transaction,
               FlatpakTransactionOperation *op,
               FlatpakTransactionProgress  *progress)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (transaction);
  FlatpakTransactionOperationType op_type = flatpak_transaction_operation_get_operation_type (op);
  g_autofree char *text = NULL;

  self->op++;
  self->op_progress = 0;

  switch (op_type)
    {
    case FLATPAK_TRANSACTION_OPERATION_INSTALL_BUNDLE:
    case FLATPAK_TRANSACTION_OPERATION_INSTALL:
      if (self->n_ops == 1)
        text = g_strdup (_("Installing..."));
      else
        text = g_strdup_printf (("Installing %d/%d..."), self->op, self->n_ops);
      break;

    case FLATPAK_TRANSACTION_OPERATION_UPDATE:
      if (self->n_ops == 1)
        text = g_strdup (_("Updating..."));
      else
        text = g_strdup_printf (_("Updating %d/%d..."), self->op, self->n_ops);
      break;

    case FLATPAK_TRANSACTION_OPERATION_UNINSTALL:
      if (self->n_ops == 1)
        text = g_strdup (_("Uninstalling..."));
      else
        text = g_strdup_printf (_("Uninstalling %d/%d..."), self->op, self->n_ops);
      break;

    default:
      g_assert_not_reached ();
      break;
    }

  if (flatpak_fancy_output ())
    {
      set_progress (self, text);
      spin_op_progress (self, op);
      redraw (self);
    }
  else
    {
      int spaces = BAR_LENGTH + 10;

      if (self->progress_msg)
        spaces += (int)(strlen (self->progress_msg) - strlen (text));
      g_print ("\r%s%*s", text, spaces, "");
    }

  g_free (self->progress_msg);
  self->progress_msg = g_steal_pointer (&text);

  self->progress_initialized = FALSE;
  g_signal_connect (progress, "changed", G_CALLBACK (progress_changed_cb), self);
  flatpak_transaction_progress_set_update_frequency (progress, FLATPAK_CLI_UPDATE_FREQUENCY);
}

static void
operation_done (FlatpakTransaction          *transaction,
                FlatpakTransactionOperation *op,
                const char                  *commit,
                FlatpakTransactionResult     details)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (transaction);
  FlatpakTransactionOperationType op_type = flatpak_transaction_operation_get_operation_type (op);

  if (op_type == FLATPAK_TRANSACTION_OPERATION_UNINSTALL)
    set_op_progress (self, op, '-');
  else
    set_op_progress (self, op, '+');

  if (flatpak_fancy_output ())
    redraw (self);
}

static gboolean
operation_error (FlatpakTransaction            *transaction,
                 FlatpakTransactionOperation   *op,
                 const GError                  *error,
                 FlatpakTransactionErrorDetails detail)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (transaction);
  FlatpakTransactionOperationType op_type = flatpak_transaction_operation_get_operation_type (op);
  const char *ref = flatpak_transaction_operation_get_ref (op);
  g_autoptr(FlatpakRef) rref = flatpak_ref_parse (ref, NULL);
  g_autofree char *msg = NULL;
  gboolean non_fatal = (detail & FLATPAK_TRANSACTION_ERROR_DETAILS_NON_FATAL) != 0;
  const char *prefix;

  if (g_error_matches (error, FLATPAK_ERROR, FLATPAK_ERROR_SKIPPED))
    {
      set_op_progress (self, op, 'o');
      msg = g_strdup_printf (_("Info: %s was skipped"), flatpak_ref_get_name (rref));
      if (flatpak_fancy_output ())
        {
          flatpak_table_printer_set_cell (self->printer, self->progress_row, 0, msg);
          self->progress_row++;
          flatpak_table_printer_add_span (self->printer, "");
          flatpak_table_printer_finish_row (self->printer);
          redraw (self);
        }
      else
        {
          int spaces = BAR_LENGTH + 10;

          if (self->progress_msg)
            spaces += (int)(strlen (self->progress_msg) - strlen (msg));
          g_print ("\r%s%*s\n", msg, spaces, ""); /* override progress, and go to next line */
        }

      return TRUE;
    }

  set_op_progress (self, op, 'x');

  if (g_error_matches (error, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED))
    msg = g_strdup_printf (_("%s already installed"), flatpak_ref_get_name (rref));
  else if (g_error_matches (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED))
    msg = g_strdup_printf (_("%s not installed"), flatpak_ref_get_name (rref));
  else if (g_error_matches (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED))
    msg = g_strdup_printf (_("%s not installed"), flatpak_ref_get_name (rref));
  else if (g_error_matches (error, FLATPAK_ERROR, FLATPAK_ERROR_NEED_NEW_FLATPAK))
    msg = g_strdup_printf (_("%s needs a later flatpak version"), flatpak_ref_get_name (rref));
  else
    msg = g_strdup (error->message);

   if (!non_fatal && self->first_operation_error == NULL)
     g_propagate_prefixed_error (&self->first_operation_error,
                                 g_error_copy (error),
                                 _("Failed to %s %s: "),
                                 op_type_to_string (op_type), flatpak_ref_get_name (rref));

  prefix = non_fatal ? _("Warning:") : _("Error:");

  if (flatpak_fancy_output ())
    {
      g_autofree char *text = g_strconcat (prefix, " ", msg, NULL);
      flatpak_table_printer_set_cell (self->printer, self->progress_row, 0, text);
      self->progress_row++;
      flatpak_table_printer_add_span (self->printer, "");
      flatpak_table_printer_finish_row (self->printer);
      redraw (self);
    }
  else
    {
      int spaces = BAR_LENGTH + 10;

      if (self->progress_msg)
        spaces += (int)(strlen (self->progress_msg) - (strlen (prefix) + 1 + strlen (msg)));
      g_print ("\r%s %s%*s\n", prefix, msg, spaces, "");
    }

  if (!non_fatal && self->stop_on_first_error)
    return FALSE;

  return TRUE; /* Continue */
}

static void
end_of_lifed (FlatpakTransaction *transaction,
              const char         *ref,
              const char         *reason,
              const char         *rebase)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (transaction);
  g_autoptr(FlatpakRef) rref = flatpak_ref_parse (ref, NULL);
  g_autofree char *msg = NULL;

  if (rebase)
    msg = g_strdup_printf (_("Info: %s is end-of-life, in preference of %s"),
                           flatpak_ref_get_name (rref), rebase);
  else if (reason)
    msg = g_strdup_printf (_("Info: %s is end-of-life, with reason: %s\n"),
                           flatpak_ref_get_name (rref), reason);

  if (flatpak_fancy_output ())
    {
      flatpak_table_printer_set_cell (self->printer, self->progress_row, 0, msg);
      self->progress_row++;
      flatpak_table_printer_add_span (self->printer, "");
      flatpak_table_printer_finish_row (self->printer);
      redraw (self);
    }
  else
    {
      int spaces = BAR_LENGTH + 10;

      if (self->progress_msg)
        spaces += (int)(strlen (self->progress_msg) - strlen (msg));
      g_print ("\r%s%*s\n", msg, spaces, "");
    }
}


static int
cmpstringp (const void *p1, const void *p2)
{
  return strcmp (*(char * const *) p1, *(char * const *) p2);
}

static void
append_permissions (GPtrArray  *permissions,
                    GKeyFile   *metadata,
                    GKeyFile   *old_metadata,
                    const char *group)
{
  g_auto(GStrv) options = g_key_file_get_string_list (metadata, FLATPAK_METADATA_GROUP_CONTEXT, group, NULL, NULL);
  g_auto(GStrv) old_options = NULL;
  int i;

  if (options == NULL)
    return;

  qsort (options, g_strv_length (options), sizeof (const char *), cmpstringp);

  if (old_metadata)
    old_options = g_key_file_get_string_list (old_metadata, FLATPAK_METADATA_GROUP_CONTEXT, group, NULL, NULL);

  for (i = 0; options[i] != NULL; i++)
    {
      const char *option = options[i];
      if (option[0] == '!')
        continue;

      if (old_options && g_strv_contains ((const char * const *) old_options, option))
        continue;

      if (strcmp (group, FLATPAK_METADATA_KEY_DEVICES) == 0 && strcmp (option, "all") == 0)
        option = "devices";

      g_ptr_array_add (permissions, g_strdup (option));
    }
}

static void
append_bus (GPtrArray  *talk,
            GPtrArray  *own,
            GKeyFile   *metadata,
            GKeyFile   *old_metadata,
            const char *group)
{
  g_auto(GStrv) keys = NULL;
  gsize i, keys_count;

  keys = g_key_file_get_keys (metadata, group, &keys_count, NULL);
  if (keys == NULL)
    return;

  qsort (keys, g_strv_length (keys), sizeof (const char *), cmpstringp);

  for (i = 0; i < keys_count; i++)
    {
      const char *key = keys[i];
      g_autofree char *value = g_key_file_get_string (metadata, group, key, NULL);

      if (g_strcmp0 (value, "none") == 0)
        continue;

      if (old_metadata)
        {
          g_autofree char *old_value = g_key_file_get_string (old_metadata, group, key, NULL);
          if (g_strcmp0 (old_value, value) == 0)
            continue;
        }

      if (g_strcmp0 (value, "own") == 0)
        g_ptr_array_add (own, g_strdup (key));
      else
        g_ptr_array_add (talk, g_strdup (key));
    }
}

static void
append_tags (GPtrArray *tags_array,
             GKeyFile  *metadata,
             GKeyFile  *old_metadata)
{
  gsize i, size = 0;
  g_auto(GStrv) tags = g_key_file_get_string_list (metadata, FLATPAK_METADATA_GROUP_APPLICATION, "tags",
                                                   &size, NULL);
  g_auto(GStrv) old_tags = NULL;

  if (old_metadata)
    old_tags = g_key_file_get_string_list (old_metadata, FLATPAK_METADATA_GROUP_APPLICATION, "tags",
                                           NULL, NULL);

  for (i = 0; i < size; i++)
    {
      const char *tag = tags[i];
      if (old_tags == NULL || !g_strv_contains ((const char * const *)old_tags, tag))
        g_ptr_array_add (tags_array, g_strdup (tag));
    }
}

static void
print_perm_line (FlatpakTablePrinter *printer,
                 const char          *title,
                 GPtrArray           *items)
{
  g_autoptr(GString) res = g_string_new (NULL);
  int i;

  if (items->len == 0)
    return;

  if (flatpak_fancy_output ())
    g_string_append (res, FLATPAK_ANSI_FAINT_ON);

  g_string_append_printf (res, "      %s: ", title);
  for (i = 0; i < items->len; i++)
    {
      if (i != 0)
        g_string_append (res, ", ");
      g_string_append (res, (char *) items->pdata[i]);
    }

  if (flatpak_fancy_output ())
    g_string_append (res, FLATPAK_ANSI_FAINT_OFF);

  flatpak_table_printer_add_span (printer, res->str);
  flatpak_table_printer_finish_row (printer);
}

static void
print_permissions (FlatpakTablePrinter *printer,
                   GKeyFile            *metadata,
                   GKeyFile            *old_metadata,
                   const char          *ref)
{
  g_autoptr(GPtrArray) permissions = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GPtrArray) files = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GPtrArray) session_bus_talk = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GPtrArray) session_bus_own = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GPtrArray) system_bus_talk = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GPtrArray) system_bus_own = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GPtrArray) tags = g_ptr_array_new_with_free_func (g_free);

  if (metadata == NULL)
    return;

  /* Only apps have permissions */
  if (!g_str_has_prefix (ref, "app/"))
    return;

  append_permissions (permissions, metadata, old_metadata, FLATPAK_METADATA_KEY_SHARED);
  append_permissions (permissions, metadata, old_metadata, FLATPAK_METADATA_KEY_SOCKETS);
  append_permissions (permissions, metadata, old_metadata, FLATPAK_METADATA_KEY_DEVICES);
  append_permissions (permissions, metadata, old_metadata, FLATPAK_METADATA_KEY_FEATURES);

  print_perm_line (printer,
                            old_metadata ?  _("new permissions") : _("permissions"),
                            permissions);

  append_permissions (files, metadata, old_metadata, FLATPAK_METADATA_KEY_FILESYSTEMS);
  print_perm_line (printer,
                            old_metadata ?  _("new file access") : _("file access"),
                            files);

  append_bus (session_bus_talk, session_bus_own,
              metadata, old_metadata, FLATPAK_METADATA_GROUP_SESSION_BUS_POLICY);
  print_perm_line (printer,
                            old_metadata ? _("new dbus access") : _("dbus access"),
                            session_bus_talk);
  print_perm_line (printer,
                            old_metadata ? _("new dbus ownership") : _("dbus ownership"),
                            session_bus_own);

  append_bus (system_bus_talk, system_bus_own,
              metadata, old_metadata, FLATPAK_METADATA_GROUP_SYSTEM_BUS_POLICY);
  print_perm_line (printer,
                            old_metadata ? _("new system dbus access") : _("system dbus access"),
                   system_bus_talk);
  print_perm_line (printer,
                            old_metadata ? _("new system dbus ownership") : _("system dbus ownership"),
                            system_bus_own);
  append_tags (tags, metadata, old_metadata);
  print_perm_line (printer,
                            old_metadata ? _("new tags") : _("tags"),
                            tags);
}

static gboolean
transaction_ready (FlatpakTransaction *transaction)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (transaction);
  GList *ops = flatpak_transaction_get_operations (transaction);
  GList *l;
  const char *prompt;
  int i;
  FlatpakTablePrinter *printer;
  const char *op_shorthand[] = { "i", "u", "i", "r" };
  int col;

  if (ops == NULL)
    return TRUE;

  self->n_ops = g_list_length (ops);

  for (l = ops; l != NULL; l = l->next)
    {
      FlatpakTransactionOperation *op = l->data;
      FlatpakTransactionOperationType type = flatpak_transaction_operation_get_operation_type (op);

      switch (type)
        {
        case FLATPAK_TRANSACTION_OPERATION_UNINSTALL:
          self->uninstalling = TRUE;
          break;
        case FLATPAK_TRANSACTION_OPERATION_INSTALL:
        case FLATPAK_TRANSACTION_OPERATION_INSTALL_BUNDLE:
          self->installing = TRUE;
          break;
        case FLATPAK_TRANSACTION_OPERATION_UPDATE:
          self->updating = TRUE;
          break;
        default:;
        }
    }

  printer = self->printer = flatpak_table_printer_new ();
  i = 0;
  flatpak_table_printer_set_column_title (printer, i++, _("   "));
  flatpak_table_printer_set_column_title (printer, i++, _("ID"));
  flatpak_table_printer_set_column_title (printer, i++, _("Arch"));
  flatpak_table_printer_set_column_title (printer, i++, _("Branch"));

  if (self->installing + self->updating + self->uninstalling > 1)
    flatpak_table_printer_set_column_title (printer, i++, _("Change"));

  if (self->installing || self->updating)
    {
      g_autofree char *text1 = NULL;
      g_autofree char *text2 = NULL;
      g_autofree char *text = NULL;
      int size;

      flatpak_table_printer_set_column_title (printer, i++, _("Remote"));
      self->download_col = i;

      /* Avoid resizing the download column too much,
       * by making the title as long as typical content
       */
      text1 = g_strdup_printf ("< 999.9 kB (%s)", _("partial"));
      text2 = g_strdup_printf ("123.4 MB / 999.9 MB");
      size = MAX (strlen (text1), strlen (text2));
      text = g_strdup_printf ("%-*s", size, _("Download"));
      flatpak_table_printer_set_column_title (printer, i++, text);
    }

  for (l = ops; l != NULL; l = l->next)
    {
      FlatpakTransactionOperation *op = l->data;
      FlatpakTransactionOperationType type = flatpak_transaction_operation_get_operation_type (op);
      const char *ref = flatpak_transaction_operation_get_ref (op);
      const char *remote = flatpak_transaction_operation_get_remote (op);
      g_auto(GStrv) parts = flatpak_decompose_ref (ref, NULL);

      flatpak_table_printer_add_column (printer, "   ");
      flatpak_table_printer_add_column (printer, parts[1]);
      flatpak_table_printer_add_column (printer, parts[2]);
      flatpak_table_printer_add_column (printer, parts[3]);

      if (self->installing + self->updating + self->uninstalling > 1)
        flatpak_table_printer_add_column (printer, op_shorthand[type]);

      if (type == FLATPAK_TRANSACTION_OPERATION_INSTALL ||
          type == FLATPAK_TRANSACTION_OPERATION_INSTALL_BUNDLE ||
          type == FLATPAK_TRANSACTION_OPERATION_UPDATE)
        {
          g_autoptr(FlatpakRef) rref = flatpak_ref_parse (ref, NULL);
          guint64 download_size;
          g_autofree char *formatted = NULL;
          g_autofree char *text = NULL;

          download_size = flatpak_transaction_operation_get_download_size (op);
          formatted = g_format_size (download_size);

          flatpak_table_printer_add_column (printer, remote);
          if (g_str_has_suffix (flatpak_ref_get_name (rref), ".Locale"))
            text = g_strdup_printf ("< %s (%s)", formatted, _("partial"));
          else
            text = g_strdup_printf ("< %s", formatted);
          flatpak_table_printer_add_decimal_column (printer, text);
        }

      g_object_set_data (G_OBJECT (op), "row", GINT_TO_POINTER (flatpak_table_printer_get_current_row (printer)));
      flatpak_table_printer_finish_row (printer);

      if (type == FLATPAK_TRANSACTION_OPERATION_INSTALL ||
          type == FLATPAK_TRANSACTION_OPERATION_INSTALL_BUNDLE ||
          type == FLATPAK_TRANSACTION_OPERATION_UPDATE)
        {
          GKeyFile *metadata = flatpak_transaction_operation_get_metadata (op);
          GKeyFile *old_metadata = flatpak_transaction_operation_get_old_metadata (op);

          print_permissions (printer, metadata, old_metadata, ref);
        }
    }

  flatpak_get_window_size (&self->rows, &self->cols);

  g_print ("\n");

  flatpak_table_printer_print_full (printer, 0, self->cols,
                                    &self->table_height, &self->table_width);

  g_print ("\n");

  if (!self->disable_interaction)
    {
      g_print ("\n");

      if (self->uninstalling && (self->installing || self->updating))
        prompt = _("Proceed with these changes?");
      else if (self->uninstalling)
        prompt = _("Proceed with uninstall?");
      else
        prompt = _("Proceed with installation?");

      if (!flatpak_yes_no_prompt (TRUE, "%s", prompt))
        {
          g_list_free_full (ops, g_object_unref);
          return FALSE;
        }
    }
  else
    g_print ("\n\n");

  for (l = ops; l; l = l->next)
    {
      FlatpakTransactionOperation *op = l->data;
      set_op_progress (self, op, ' ');
    }

  g_list_free_full (ops, g_object_unref);

  flatpak_table_printer_add_span (printer, "");
  flatpak_table_printer_finish_row (printer);
  flatpak_table_printer_add_span (printer, "");
  self->progress_row = flatpak_table_printer_get_current_row (printer);
  flatpak_table_printer_finish_row (printer);

  self->table_height += 2;

  flatpak_get_cursor_pos (&self->end_row, &col);

  if (flatpak_fancy_output ())
    redraw (self);

  return TRUE;
}

static void
flatpak_cli_transaction_finalize (GObject *object)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (object);

  if (self->first_operation_error)
    g_error_free (self->first_operation_error);

  g_free (self->progress_msg);

  G_OBJECT_CLASS (flatpak_cli_transaction_parent_class)->finalize (object);
}

static void
flatpak_cli_transaction_init (FlatpakCliTransaction *self)
{
}

static void
flatpak_cli_transaction_class_init (FlatpakCliTransactionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  FlatpakTransactionClass *transaction_class = FLATPAK_TRANSACTION_CLASS (klass);

  object_class->finalize = flatpak_cli_transaction_finalize;
  transaction_class->add_new_remote = add_new_remote;
  transaction_class->ready = transaction_ready;
  transaction_class->new_operation = new_operation;
  transaction_class->operation_done = operation_done;
  transaction_class->operation_error = operation_error;
  transaction_class->choose_remote_for_ref = choose_remote_for_ref;
  transaction_class->end_of_lifed = end_of_lifed;
}

FlatpakTransaction *
flatpak_cli_transaction_new (FlatpakDir *dir,
                             gboolean    disable_interaction,
                             gboolean    stop_on_first_error,
                             GError    **error)
{
  g_autoptr(FlatpakInstallation) installation = NULL;
  g_autoptr(FlatpakCliTransaction) self = NULL;

  flatpak_dir_set_no_interaction (dir, disable_interaction);

  installation = flatpak_installation_new_for_dir (dir, NULL, error);
  if (installation == NULL)
    return NULL;

  self = g_initable_new (FLATPAK_TYPE_CLI_TRANSACTION,
                         NULL, error,
                         "installation", installation,
                         NULL);
  if (self == NULL)
    return NULL;

  self->disable_interaction = disable_interaction;
  self->stop_on_first_error = stop_on_first_error;

  flatpak_transaction_add_default_dependency_sources (FLATPAK_TRANSACTION (self));

  return (FlatpakTransaction *) g_steal_pointer (&self);
}

gboolean
flatpak_cli_transaction_add_install (FlatpakTransaction *transaction,
                                     const char         *remote,
                                     const char         *ref,
                                     const char        **subpaths,
                                     GError            **error)
{
  g_autoptr(GError) local_error = NULL;

  if (!flatpak_transaction_add_install (transaction, remote, ref, subpaths, &local_error))
    {
      if (g_error_matches (local_error, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED))
        {
          g_printerr (_("Skipping: %s\n"), local_error->message);
          return TRUE;
        }

      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  return TRUE;
}


gboolean
flatpak_cli_transaction_run (FlatpakTransaction *transaction,
                             GCancellable       *cancellable,
                             GError            **error)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (transaction);

  g_autoptr(GError) local_error = NULL;
  gboolean res;

  res = flatpak_transaction_run (transaction, cancellable, &local_error);

  if (res && self->n_ops > 0)
    {
      const char *text;

      if (self->uninstalling + self->installing + self->updating > 1)
        text = _("Changes complete.");
      else if (self->uninstalling)
        text = _("Uninstall complete.");
      else if (self->installing)
        text = _("Installation complete.");
      else
        text = _("Updates complete.");

      if (flatpak_fancy_output ())
        {
          set_progress (self, text);
          redraw (self);
        }
      else
        {
          int spaces = BAR_LENGTH + 10;

          if (self->progress_msg)
            spaces += (int)(strlen (self->progress_msg) - strlen (text));
          g_print ("\r%s%*s", text, spaces, "");
        }

      g_print ("\n");
    }

  /* If we got some weird error (i.e. not ABORTED because we chose to abort
     on an error, report that */
  if (!res)
    {
      if (g_error_matches (local_error, FLATPAK_ERROR, FLATPAK_ERROR_ABORTED))
        {
          self->aborted = TRUE;
        }
      else
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return FALSE;
        }
    }

  if (self->first_operation_error)
    {
      /* We always want to return an error if there was some kind of operation error,
         as that causes the main CLI to return an error status. */

      if (self->stop_on_first_error)
        {
          /* For the install/stop_on_first_error we return the first operation error,
             as we have not yet printed it.  */

          g_propagate_error (error, g_steal_pointer (&self->first_operation_error));
          return FALSE;
        }
      else
        {
          /* For updates/!stop_on_first_error we already printed all errors so we make up
             a different one. */

          return flatpak_fail (error, _("There were one or more errors"));
        }
    }

  return TRUE;
}

gboolean
flatpak_cli_transaction_was_aborted (FlatpakTransaction *transaction)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (transaction);

  return self->aborted;
}
