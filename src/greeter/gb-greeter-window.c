/* gb-greeter-window.c
 *
 * Copyright (C) 2015 Christian Hergert <christian@hergert.me>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define G_LOG_DOMAIN "gb-greeter-window"

#include <glib/gi18n.h>
#include <ide.h>

#include "egg-signal-group.h"

#include "gb-greeter-project-row.h"
#include "gb-greeter-window.h"
#include "gb-scrolled-window.h"

struct _GbGreeterWindow
{
  GtkApplicationWindow  parent_instance;

  EggSignalGroup       *signal_group;
  IdeRecentProjects    *recent_projects;

  GtkWidget            *header_bar;
  GtkListBox           *my_projects_list_box;
  GtkListBox           *other_projects_list_box;
};

G_DEFINE_TYPE (GbGreeterWindow, gb_greeter_window, GTK_TYPE_APPLICATION_WINDOW)

enum {
  PROP_0,
  PROP_RECENT_PROJECTS,
  LAST_PROP
};

static GParamSpec *gParamSpecs [LAST_PROP];

IdeRecentProjects *
gb_greeter_window_get_recent_projects (GbGreeterWindow *self)
{
  g_return_val_if_fail (GB_IS_GREETER_WINDOW (self), NULL);

  return self->recent_projects;
}

void
gb_greeter_window_set_recent_projects (GbGreeterWindow   *self,
                                       IdeRecentProjects *recent_projects)
{
  g_return_if_fail (GB_IS_GREETER_WINDOW (self));
  g_return_if_fail (!recent_projects || IDE_IS_RECENT_PROJECTS (recent_projects));

  if (g_set_object (&self->recent_projects, recent_projects))
    {
      egg_signal_group_set_target (self->signal_group, recent_projects);
      g_object_notify_by_pspec (G_OBJECT (self), gParamSpecs [PROP_RECENT_PROJECTS]);
    }
}

static void
gb_greeter_window_row_header_cb (GtkListBoxRow *row,
                                 GtkListBoxRow *before,
                                 gpointer       user_data)
{
  g_assert (GTK_IS_LIST_BOX_ROW (row));

  if (before != NULL)
    {
      GtkWidget *header;

      header = g_object_new (GTK_TYPE_SEPARATOR,
                             "orientation", GTK_ORIENTATION_HORIZONTAL,
                             "visible", TRUE,
                             NULL);
      gtk_list_box_row_set_header (row, header);
    }
}

static void
gb_greeter_window__recent_projects_items_changed (GListModel *list_model,
                                                  guint       position,
                                                  guint       removed,
                                                  guint       added,
                                                  gpointer    user_data)
{
  IdeRecentProjects *recent_projects = (IdeRecentProjects *)list_model;

  g_assert (G_IS_LIST_MODEL (list_model));
  g_assert (IDE_IS_RECENT_PROJECTS (recent_projects));
}

static void
gb_greeter_window_finalize (GObject *object)
{
  GbGreeterWindow *self = (GbGreeterWindow *)object;

  g_clear_object (&self->signal_group);
  g_clear_object (&self->recent_projects);

  G_OBJECT_CLASS (gb_greeter_window_parent_class)->finalize (object);
}

static void
gb_greeter_window_get_property (GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  GbGreeterWindow *self = GB_GREETER_WINDOW (object);

  switch (prop_id)
    {
    case PROP_RECENT_PROJECTS:
      g_value_set_object (value, gb_greeter_window_get_recent_projects (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gb_greeter_window_set_property (GObject      *object,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  GbGreeterWindow *self = GB_GREETER_WINDOW (object);

  switch (prop_id)
    {
    case PROP_RECENT_PROJECTS:
      gb_greeter_window_set_recent_projects (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gb_greeter_window_class_init (GbGreeterWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->finalize = gb_greeter_window_finalize;
  object_class->get_property = gb_greeter_window_get_property;
  object_class->set_property = gb_greeter_window_set_property;

  gParamSpecs [PROP_RECENT_PROJECTS] =
    g_param_spec_object ("recent-projects",
                         _("Recent Projects"),
                         _("The recent projects that have been mined."),
                         IDE_TYPE_RECENT_PROJECTS,
                         (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_RECENT_PROJECTS,
                                   gParamSpecs [PROP_RECENT_PROJECTS]);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/builder/ui/gb-greeter-window.ui");
  gtk_widget_class_bind_template_child (widget_class, GbGreeterWindow, header_bar);
  gtk_widget_class_bind_template_child (widget_class, GbGreeterWindow, my_projects_list_box);
  gtk_widget_class_bind_template_child (widget_class, GbGreeterWindow, other_projects_list_box);

  g_type_ensure (GB_TYPE_GREETER_PROJECT_ROW);
  g_type_ensure (GB_TYPE_SCROLLED_WINDOW);
}

static void
gb_greeter_window_init (GbGreeterWindow *self)
{
  self->signal_group = egg_signal_group_new (IDE_TYPE_RECENT_PROJECTS);

  egg_signal_group_connect_object (self->signal_group,
                                   "items-changed",
                                   G_CALLBACK (gb_greeter_window__recent_projects_items_changed),
                                   self,
                                   G_CONNECT_SWAPPED);

  gtk_widget_init_template (GTK_WIDGET (self));

  gtk_list_box_set_header_func (self->my_projects_list_box,
                                gb_greeter_window_row_header_cb,
                                NULL, NULL);
}