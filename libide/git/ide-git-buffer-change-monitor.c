/* ide-git-buffer-change-monitor.c
 *
 * Copyright (C) 2015 Christian Hergert <christian@hergert.me>
 *
 * This file is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 3 of the
 * License, or (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <glib/gi18n.h>
#include <libgit2-glib/ggit.h>

#include "ide-buffer.h"
#include "ide-file.h"
#include "ide-git-buffer-change-monitor.h"

/**
 * SECTION:ide-git-buffer-change-monitor
 *
 * This module provides line change monitoring when used in conjunction with an IdeGitVcs.
 * The changes are generated by comparing the buffer contents to the version found inside of
 * the git repository.
 *
 * To enable us to avoid blocking the main loop, the actual diff is performed in a background
 * thread. To avoid threading issues with the rest of LibIDE, this module creates a copy of the
 * loaded repository. A single thread will be dispatched for the context and all reload tasks
 * will be performed from that thread.
 *
 * Upon completion of the diff, the results will be passed back to the primary thread and the
 * state updated for use by line change renderer in the source view.
 */

struct _IdeGitBufferChangeMonitor
{
  IdeBufferChangeMonitor parent_instance;

  IdeBuffer      *buffer;
  GgitRepository *repository;
  GHashTable     *state;

  GgitBlob       *cached_blob;

  guint           state_dirty : 1;
  guint           in_calculation : 1;
};

typedef struct
{
  GgitRepository *repository;
  GHashTable     *state;
  GFile          *file;
  GBytes         *content;
  GgitBlob       *blob;
} DiffTask;

G_DEFINE_TYPE (IdeGitBufferChangeMonitor,
               ide_git_buffer_change_monitor,
               IDE_TYPE_BUFFER_CHANGE_MONITOR)

enum {
  PROP_0,
  PROP_REPOSITORY,
  LAST_PROP
};

static GParamSpec  *gParamSpecs [LAST_PROP];
static GAsyncQueue *gWorkQueue;
static GThread     *gWorkThread;

static void
diff_task_free (gpointer data)
{
  DiffTask *diff = data;

  if (diff)
    {
      g_clear_object (&diff->file);
      g_clear_object (&diff->blob);
      g_clear_object (&diff->repository);
      g_clear_pointer (&diff->state, g_hash_table_unref);
      g_clear_pointer (&diff->content, g_bytes_unref);
    }
}

static GHashTable *
ide_git_buffer_change_monitor_calculate_finish (IdeGitBufferChangeMonitor  *self,
                                                GAsyncResult               *result,
                                                GError                    **error)
{
  GTask *task = (GTask *)result;
  DiffTask *diff;

  g_assert (IDE_IS_GIT_BUFFER_CHANGE_MONITOR (self));
  g_assert (G_IS_TASK (result));

  diff = g_task_get_task_data (task);

  /* Keep the blob around for future use */
  if (diff->blob != self->cached_blob)
    g_set_object (&self->cached_blob, diff->blob);

  return g_task_propagate_pointer (task, error);
}

static void
ide_git_buffer_change_monitor_calculate_async (IdeGitBufferChangeMonitor *self,
                                               GCancellable              *cancellable,
                                               GAsyncReadyCallback        callback,
                                               gpointer                   user_data)
{
  g_autoptr(GTask) task = NULL;
  DiffTask *diff;
  IdeFile *file;
  GFile *gfile;

  g_assert (IDE_IS_GIT_BUFFER_CHANGE_MONITOR (self));
  g_assert (!cancellable || G_IS_CANCELLABLE (cancellable));
  g_assert (self->buffer != NULL);
  g_assert (self->repository != NULL);

  self->state_dirty = FALSE;

  task = g_task_new (self, cancellable, callback, user_data);

  file = ide_buffer_get_file (self->buffer);
  gfile = ide_file_get_file (file);

  if (!gfile)
    {
      g_task_return_new_error (task,
                               G_IO_ERROR,
                               G_IO_ERROR_NOT_FOUND,
                               _("Cannot provide diff, no backing file provided."));
      return;
    }

  diff = g_slice_new0 (DiffTask);
  diff->file = g_object_ref (gfile);
  diff->repository = g_object_ref (self->repository);
  diff->state = g_hash_table_new (g_direct_hash, g_direct_equal);
  diff->content = ide_buffer_get_content (self->buffer);
  diff->blob = self->cached_blob ? g_object_ref (self->cached_blob) : NULL;

  g_task_set_task_data (task, diff, diff_task_free);

  self->in_calculation = TRUE;

  g_async_queue_push (gWorkQueue, g_object_ref (task));
}

static IdeBufferLineChange
ide_git_buffer_change_monitor_get_change (IdeBufferChangeMonitor *monitor,
                                          const GtkTextIter      *iter)
{
  IdeGitBufferChangeMonitor *self = (IdeGitBufferChangeMonitor *)monitor;
  gpointer key;
  gpointer value;

  g_return_val_if_fail (IDE_IS_GIT_BUFFER_CHANGE_MONITOR (self), IDE_BUFFER_LINE_CHANGE_NONE);
  g_return_val_if_fail (iter, IDE_BUFFER_LINE_CHANGE_NONE);

  if (!self->state)
    return IDE_BUFFER_LINE_CHANGE_NONE;

  key = GINT_TO_POINTER (gtk_text_iter_get_line (iter) + 1);
  value = g_hash_table_lookup (self->state, key);

  return GPOINTER_TO_INT (value);
}

static void
ide_git_buffer_change_monitor_set_repository (IdeGitBufferChangeMonitor *self,
                                              GgitRepository            *repository)
{
  g_return_if_fail (IDE_IS_GIT_BUFFER_CHANGE_MONITOR (self));
  g_return_if_fail (GGIT_IS_REPOSITORY (repository));

  g_set_object (&self->repository, repository);
}

static void
ide_git_buffer_change_monitor__calculate_cb (GObject      *object,
                                             GAsyncResult *result,
                                             gpointer      user_data_unused)
{
  IdeGitBufferChangeMonitor *self = (IdeGitBufferChangeMonitor *)object;
  g_autoptr(GHashTable) ret = NULL;
  g_autoptr(GError) error = NULL;

  g_assert (IDE_IS_GIT_BUFFER_CHANGE_MONITOR (self));

  self->in_calculation = FALSE;

  ret = ide_git_buffer_change_monitor_calculate_finish (self, result, &error);

  if (!ret)
    {
      g_message ("%s", error->message);
    }
  else
    {
      g_clear_pointer (&self->state, g_hash_table_unref);
      self->state = g_hash_table_ref (ret);
    }

  ide_buffer_change_monitor_emit_changed (IDE_BUFFER_CHANGE_MONITOR (self));

  /*
   * Recalculate the state if the buffer has changed since we submitted our request.
   */
  if (self->state_dirty)
    ide_git_buffer_change_monitor_calculate_async (self,
                                                   NULL,
                                                   ide_git_buffer_change_monitor__calculate_cb,
                                                   NULL);
}

static void
ide_git_buffer_change_monitor__buffer_changed_cb (IdeGitBufferChangeMonitor *self,
                                                  IdeBuffer                 *buffer)
{
  g_assert (IDE_IS_GIT_BUFFER_CHANGE_MONITOR (self));
  g_assert (IDE_IS_BUFFER (buffer));

  self->state_dirty = TRUE;

  /* We recalculate state upon completion of current request */
  if (self->in_calculation)
    return;

  /*
   * TODO:
   *
   * we shouldn't do this here:
   *
   * instead we should hook to insert/delete-range signals and determine if there was a multiline
   * change. also, if the line has not changed, we need to recalculate. but we can avoid the
   * check if the line has already changed and no \n were added/removed.
   */
  ide_git_buffer_change_monitor_calculate_async (self,
                                                 NULL,
                                                 ide_git_buffer_change_monitor__calculate_cb,
                                                 NULL);
}

static void
ide_git_buffer_change_monitor_set_buffer (IdeBufferChangeMonitor *monitor,
                                          IdeBuffer              *buffer)
{
  IdeGitBufferChangeMonitor *self = (IdeGitBufferChangeMonitor *)monitor;

  g_return_if_fail (IDE_IS_GIT_BUFFER_CHANGE_MONITOR (self));
  g_return_if_fail (IDE_IS_BUFFER (buffer));
  g_return_if_fail (!self->buffer);

  self->buffer = g_object_ref (buffer);

  g_signal_connect_object (self->buffer,
                           "changed",
                           G_CALLBACK (ide_git_buffer_change_monitor__buffer_changed_cb),
                           self,
                           G_CONNECT_SWAPPED);
}

static gint
diff_line_cb (GgitDiffDelta *delta,
              GgitDiffHunk  *hunk,
              GgitDiffLine  *line,
              gpointer       user_data)
{
  GgitDiffLineType type;
  GHashTable *hash = user_data;
  gint new_lineno;
  gint old_lineno;
  gint adjust;

  g_return_val_if_fail (delta, GGIT_ERROR_GIT_ERROR);
  g_return_val_if_fail (hunk, GGIT_ERROR_GIT_ERROR);
  g_return_val_if_fail (line, GGIT_ERROR_GIT_ERROR);
  g_return_val_if_fail (hash, GGIT_ERROR_GIT_ERROR);

  type = ggit_diff_line_get_origin (line);

  if ((type != GGIT_DIFF_LINE_ADDITION) && (type != GGIT_DIFF_LINE_DELETION))
    return 0;

  new_lineno = ggit_diff_line_get_new_lineno (line);
  old_lineno = ggit_diff_line_get_old_lineno (line);

  switch (type)
    {
    case GGIT_DIFF_LINE_ADDITION:
      if (g_hash_table_lookup (hash, GINT_TO_POINTER (new_lineno)))
        g_hash_table_replace (hash,
                              GINT_TO_POINTER (new_lineno),
                              GINT_TO_POINTER (IDE_BUFFER_LINE_CHANGE_CHANGED));
      else
        g_hash_table_insert (hash,
                             GINT_TO_POINTER (new_lineno),
                             GINT_TO_POINTER (IDE_BUFFER_LINE_CHANGE_ADDED));
      break;

    case GGIT_DIFF_LINE_DELETION:
      adjust = (ggit_diff_hunk_get_new_start (hunk) - ggit_diff_hunk_get_old_start (hunk));
      old_lineno += adjust;
      if (g_hash_table_lookup (hash, GINT_TO_POINTER (old_lineno)))
        g_hash_table_replace (hash,
                              GINT_TO_POINTER (old_lineno),
                              GINT_TO_POINTER (IDE_BUFFER_LINE_CHANGE_CHANGED));
      else
        g_hash_table_insert (hash,
                             GINT_TO_POINTER (old_lineno),
                             GINT_TO_POINTER (IDE_BUFFER_LINE_CHANGE_DELETED));
      break;

    case GGIT_DIFF_LINE_CONTEXT:
    case GGIT_DIFF_LINE_CONTEXT_EOFNL:
    case GGIT_DIFF_LINE_ADD_EOFNL:
    case GGIT_DIFF_LINE_DEL_EOFNL:
    case GGIT_DIFF_LINE_FILE_HDR:
    case GGIT_DIFF_LINE_HUNK_HDR:
    case GGIT_DIFF_LINE_BINARY:
    default:
      break;
    }

  return 0;
}

static gboolean
ide_git_buffer_change_monitor_calculate_threaded (IdeGitBufferChangeMonitor  *self,
                                                  DiffTask                   *diff,
                                                  GError                    **error)
{
  g_autofree gchar *relative_path = NULL;
  g_autoptr(GFile) workdir = NULL;
  const guint8 *data;
  gsize data_len = 0;

  g_assert (IDE_IS_GIT_BUFFER_CHANGE_MONITOR (self));
  g_assert (diff);
  g_assert (G_IS_FILE (diff->file));
  g_assert (diff->state);
  g_assert (GGIT_IS_REPOSITORY (diff->repository));
  g_assert (diff->content);
  g_assert (!diff->blob || GGIT_IS_BLOB (diff->blob));
  g_assert (error);
  g_assert (!*error);

  workdir = ggit_repository_get_workdir (diff->repository);

  if (!workdir)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_INVALID_FILENAME,
                   _("Repository does not have a working directory."));
      return FALSE;
    }

  relative_path = g_file_get_relative_path (workdir, diff->file);

  if (!relative_path)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_INVALID_FILENAME,
                   _("File is not under control of git working directory."));
      return FALSE;
    }

  /*
   * Find the blob if necessary. This will be cached by the main thread for us on the way out
   * of the async operation.
   */
  if (!diff->blob)
    {
      GgitOId *entry_oid = NULL;
      GgitOId *oid = NULL;
      GgitObject *blob = NULL;
      GgitObject *commit = NULL;
      GgitRef *head = NULL;
      GgitTree *tree = NULL;
      GgitTreeEntry *entry = NULL;

      head = ggit_repository_get_head (diff->repository, error);
      if (!head)
        goto cleanup;

      oid = ggit_ref_get_target (head);
      if (!oid)
        goto cleanup;

      commit = ggit_repository_lookup (diff->repository, oid, GGIT_TYPE_COMMIT, error);
      if (!commit)
        goto cleanup;

      tree = ggit_commit_get_tree (GGIT_COMMIT (commit));
      if (!tree)
        goto cleanup;

      entry = ggit_tree_get_by_path (tree, relative_path, error);
      if (!entry)
        goto cleanup;

      entry_oid = ggit_tree_entry_get_id (entry);
      if (!entry_oid)
        goto cleanup;

      blob = ggit_repository_lookup (diff->repository, entry_oid, GGIT_TYPE_BLOB, error);
      if (!blob)
        goto cleanup;

      diff->blob = g_object_ref (blob);

    cleanup:
      g_clear_object (&blob);
      g_clear_pointer (&entry_oid, ggit_oid_free);
      g_clear_pointer (&entry, ggit_tree_entry_unref);
      g_clear_object (&tree);
      g_clear_object (&commit);
      g_clear_pointer (&oid, ggit_oid_free);
      g_clear_object (&head);
    }

  if (!diff->blob)
    {
      if ((*error) == NULL)
        g_set_error (error,
                     G_IO_ERROR,
                     G_IO_ERROR_NOT_FOUND,
                     _("The request file does not exist within the git index."));
      return FALSE;
    }

  data = g_bytes_get_data (diff->content, &data_len);

  ggit_diff_blob_to_buffer (diff->blob, relative_path, data, data_len, relative_path,
                            NULL, NULL, NULL, diff_line_cb, (gpointer)diff->state, error);

  return ((*error) == NULL);
}

static gpointer
ide_git_buffer_change_monitor_worker (gpointer data)
{
  GAsyncQueue *queue = data;
  GTask *task;

  g_assert (queue);

  while ((task = g_async_queue_pop (queue)))
    {
      IdeGitBufferChangeMonitor *self;
      DiffTask *diff;
      GError *error = NULL;

      self = g_task_get_source_object (task);
      diff = g_task_get_task_data (task);

      if (!ide_git_buffer_change_monitor_calculate_threaded (self, diff, &error))
        g_task_return_error (task, error);
      else
        g_task_return_pointer (task, g_hash_table_ref (diff->state),
                               (GDestroyNotify)g_hash_table_unref);

      g_object_unref (task);
    }

  return NULL;
}

static void
ide_git_buffer_change_monitor_dispose (GObject *object)
{
  IdeGitBufferChangeMonitor *self = (IdeGitBufferChangeMonitor *)object;

  g_clear_object (&self->cached_blob);
  g_clear_object (&self->buffer);
  g_clear_object (&self->repository);

  G_OBJECT_CLASS (ide_git_buffer_change_monitor_parent_class)->dispose (object);
}

static void
ide_git_buffer_change_monitor_set_property (GObject      *object,
                                            guint         prop_id,
                                            const GValue *value,
                                            GParamSpec   *pspec)
{
  IdeGitBufferChangeMonitor *self = IDE_GIT_BUFFER_CHANGE_MONITOR (object);

  switch (prop_id)
    {
    case PROP_REPOSITORY:
      ide_git_buffer_change_monitor_set_repository (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
ide_git_buffer_change_monitor_class_init (IdeGitBufferChangeMonitorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  IdeBufferChangeMonitorClass *parent_class = IDE_BUFFER_CHANGE_MONITOR_CLASS (klass);

  object_class->dispose = ide_git_buffer_change_monitor_dispose;
  object_class->set_property = ide_git_buffer_change_monitor_set_property;

  parent_class->set_buffer = ide_git_buffer_change_monitor_set_buffer;
  parent_class->get_change = ide_git_buffer_change_monitor_get_change;

  gParamSpecs [PROP_REPOSITORY] =
    g_param_spec_object ("repository",
                         _("Repository"),
                         _("The repository to use for calculating diffs."),
                         GGIT_TYPE_REPOSITORY,
                         (G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_REPOSITORY, gParamSpecs [PROP_REPOSITORY]);

  gWorkQueue = g_async_queue_new ();
  gWorkThread = g_thread_new ("IdeGitBufferChangeMonitorWorker",
                              ide_git_buffer_change_monitor_worker,
                              gWorkQueue);
}

static void
ide_git_buffer_change_monitor_init (IdeGitBufferChangeMonitor *self)
{
}
