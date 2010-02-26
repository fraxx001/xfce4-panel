/*
 * Copyright (C) 2008-2010 Nick Schermer <nick@xfce.org>
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_MATH_H
#include <math.h>
#endif

#include <gtk/gtk.h>
#include <exo/exo.h>
#include <libwnck/libwnck.h>
#include <libxfce4panel/libxfce4panel.h>
#include <common/panel-private.h>
#include <common/panel-debug.h>

#ifdef GDK_WINDOWING_X11
#include <X11/Xlib.h>
#include <gdk/gdkx.h>
#include <X11/extensions/shape.h>
#endif

#include "tasklist-widget.h"



#define DEFAULT_BUTTON_SIZE          (25)
#define DEFAULT_MAX_BUTTON_LENGTH    (200)
#define DEFAULT_MENU_ICON_SIZE       (16)
#define DEFAULT_MIN_BUTTON_LENGTH    (DEFAULT_MAX_BUTTON_LENGTH / 2)
#define DEFAULT_ICON_LUCENCY         (50)
#define DEFAULT_ELLIPSIZE_MODE       (PANGO_ELLIPSIZE_END)
#define DEFAULT_MENU_MAX_WIDTH_CHARS (24)
#define ARROW_BUTTON_SIZE            (20)
#define WIREFRAME_SIZE               (5) /* same as xfwm4 */



/* locking helpers for tasklist->locked */
#define xfce_taskbar_lock(tasklist)      G_STMT_START { XFCE_TASKLIST (tasklist)->locked++; } G_STMT_END
#define xfce_taskbar_unlock(tasklist)    G_STMT_START { \
                                           if (XFCE_TASKLIST (tasklist)->locked > 0) \
                                             XFCE_TASKLIST (tasklist)->locked--; \
                                           else \
                                             panel_assert_not_reached (); \
                                         } G_STMT_END
#define xfce_taskbar_is_locked(tasklist) (XFCE_TASKLIST (tasklist)->locked > 0)

#define xfce_tasklist_button_visible(child,tasklist,active_ws) \
  ((!tasklist->only_minimized || wnck_window_is_minimized (child->window)) \
   && (tasklist->all_workspaces \
       || wnck_window_is_pinned (child->window) \
       || wnck_window_get_workspace (child->window) == active_ws))



enum
{
  PROP_0,
  PROP_GROUPING,
  PROP_INCLUDE_ALL_WORKSPACES,
  PROP_FLAT_BUTTONS,
  PROP_SWITCH_WORKSPACE_ON_UNMINIMIZE,
  PROP_SHOW_LABELS,
  PROP_SHOW_ONLY_MINIMIZED,
  PROP_SHOW_WIREFRAMES,
  PROP_SHOW_HANDLE,
  PROP_SORT_ORDER
};

struct _XfceTasklistClass
{
  GtkContainerClass __parent__;
};

struct _XfceTasklist
{
  GtkContainer __parent__;

  /* lock counter */
  gint                  locked;

  /* the screen of this tasklist */
  WnckScreen           *screen;

  /* window children in the tasklist */
  GSList               *windows;

  /* windows we monitor, but that are excluded from the tasklist */
  GSList               *skipped_windows;

  /* arrow button of the overflow menu */
  GtkWidget            *arrow_button;

  /* classgroups of all the windows in the taskbar */
  GSList               *class_groups;

  /* normal or iconbox style */
  guint                 show_labels : 1;

  /* size of the panel pluin */
  gint                  size;

  /* orientation of the tasklist */
  guint                 horizontal : 1;

  /* relief of the tasklist buttons */
  GtkReliefStyle        button_relief;

  /* whether we show windows from all workspaces or
   * only the active workspace */
  guint                 all_workspaces : 1;

  /* whether we switch to another workspace when we try to
   * unminimize a window on another workspace */
  guint                 switch_workspace : 1;

  /* whether we only show monimized windows in the
   * tasklist */
  guint                 only_minimized : 1;

  /* whether we show wireframes when hovering a button in
   * the tasklist */
  guint                 show_wireframes : 1;

  /* button grouping mode */
  XfceTasklistGrouping  grouping;

  /* sorting order of the buttons */
  XfceTasklistSortOrder sort_order;

  /* dummy property */
  guint                 show_handle : 1;

#ifdef GDK_WINDOWING_X11
  /* wireframe window */
  Window                wireframe_window;
#endif

  /* gtk style properties */
  gint                  max_button_length;
  gint                  min_button_length;
  gint                  max_button_size;
  PangoEllipsizeMode    ellipsize_mode;
  gint                  minimized_icon_lucency;
  gint                  menu_icon_size;
  gint                  menu_max_width_chars;

  gint n_windows;
};

typedef enum
{
  XFCE_TASKLIST_BUTTON_TYPE_WINDOW,
  XFCE_TASKLIST_BUTTON_TYPE_GROUP,
  XFCE_TASKLIST_BUTTON_TYPE_MENU,
}
XfceTasklistChildType;

typedef struct _XfceTasklistChild XfceTasklistChild;
struct _XfceTasklistChild
{
  /* type of this button */
  XfceTasklistChildType  type;

  /* pointer to the tasklist */
  XfceTasklist           *tasklist;

  /* button widgets */
  GtkWidget              *button;
  GtkWidget              *box;
  GtkWidget              *icon;
  GtkWidget              *label;

  /* unique id for sorting by insert time,
   * simply increased for each new button */
  guint                   unique_id;

  /* last time this window was focused */
  GTimeVal                last_focused;

  /* list of windows in case of a group button */
  GSList                 *windows;

  /* wnck information */
  WnckWindow             *window;
  WnckClassGroup         *class_group;
};



static void xfce_tasklist_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
static void xfce_tasklist_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
static void xfce_tasklist_finalize (GObject          *object);
static void xfce_tasklist_size_request (GtkWidget *widget, GtkRequisition *requisition);
static void xfce_tasklist_size_allocate (GtkWidget *widget, GtkAllocation *allocation);
static void xfce_tasklist_style_set (GtkWidget *widget, GtkStyle *previous_style);
static void xfce_tasklist_realize (GtkWidget *widget);
static void xfce_tasklist_unrealize (GtkWidget *widget);
static void xfce_tasklist_remove (GtkContainer *container, GtkWidget *widget);
static void xfce_tasklist_forall (GtkContainer *container, gboolean include_internals, GtkCallback callback, gpointer callback_data);
static GType xfce_tasklist_child_type (GtkContainer *container);
static void xfce_tasklist_arrow_button_toggled (GtkWidget *button, XfceTasklist *tasklist);
static void xfce_tasklist_connect_screen (XfceTasklist *tasklist);
static void xfce_tasklist_disconnect_screen (XfceTasklist *tasklist);
static void xfce_tasklist_active_window_changed (WnckScreen *screen, WnckWindow  *previous_window, XfceTasklist *tasklist);
static void xfce_tasklist_active_workspace_changed (WnckScreen *screen, WnckWorkspace *previous_workspace, XfceTasklist *tasklist);
static void xfce_tasklist_window_added (WnckScreen *screen, WnckWindow *window, XfceTasklist *tasklist);
static void xfce_tasklist_window_removed (WnckScreen *screen, WnckWindow *window, XfceTasklist *tasklist);
static void xfce_tasklist_viewports_changed (WnckScreen *screen, XfceTasklist *tasklist);
static void xfce_tasklist_skipped_windows_state_changed (WnckWindow *window, WnckWindowState changed_state, WnckWindowState new_state, XfceTasklist *tasklist);
static void xfce_tasklist_sort (XfceTasklist *tasklist);
static GtkWidget *xfce_tasklist_get_panel_plugin (XfceTasklist *tasklist);

/* wireframe */
#ifdef GDK_WINDOWING_X11
static void xfce_tasklist_wireframe_hide (XfceTasklist *tasklist);
static void xfce_tasklist_wireframe_destroy (XfceTasklist *tasklist);
static void xfce_tasklist_wireframe_update (XfceTasklist *tasklist, XfceTasklistChild *child);
#endif

/* tasklist buttons */
static gint xfce_tasklist_button_compare (gconstpointer child_a, gconstpointer child_b, gpointer user_data);
static GtkWidget *xfce_tasklist_button_proxy_menu_item (XfceTasklistChild *child);
static XfceTasklistChild *xfce_tasklist_button_new (WnckWindow *window, XfceTasklist *tasklist);

/* tasklist group buttons */
static void xfce_tasklist_group_button_remove (XfceTasklist *tasklist, WnckClassGroup *class_group);
static XfceTasklistChild *xfce_tasklist_group_button_new (WnckClassGroup *class_group, XfceTasklist *tasklist);

/* potential public functions */
static void xfce_tasklist_set_include_all_workspaces (XfceTasklist *tasklist, gboolean all_workspaces);
static void xfce_tasklist_set_button_relief (XfceTasklist *tasklist, GtkReliefStyle button_relief);
static void xfce_tasklist_set_show_labels (XfceTasklist *tasklist, gboolean show_labels);
static void xfce_tasklist_set_show_only_minimized (XfceTasklist *tasklist, gboolean only_minimized);
static void xfce_tasklist_set_show_wireframes (XfceTasklist *tasklist, gboolean show_wireframes);



G_DEFINE_TYPE (XfceTasklist, xfce_tasklist, GTK_TYPE_CONTAINER)



static GtkIconSize menu_icon_size = GTK_ICON_SIZE_INVALID;



static void
xfce_tasklist_class_init (XfceTasklistClass *klass)
{
  GObjectClass      *gobject_class;
  GtkWidgetClass    *gtkwidget_class;
  GtkContainerClass *gtkcontainer_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->get_property = xfce_tasklist_get_property;
  gobject_class->set_property = xfce_tasklist_set_property;
  gobject_class->finalize = xfce_tasklist_finalize;

  gtkwidget_class = GTK_WIDGET_CLASS (klass);
  gtkwidget_class->size_request = xfce_tasklist_size_request;
  gtkwidget_class->size_allocate = xfce_tasklist_size_allocate;
  gtkwidget_class->style_set = xfce_tasklist_style_set;
  gtkwidget_class->realize = xfce_tasklist_realize;
  gtkwidget_class->unrealize = xfce_tasklist_unrealize;

  gtkcontainer_class = GTK_CONTAINER_CLASS (klass);
  gtkcontainer_class->add = NULL;
  gtkcontainer_class->remove = xfce_tasklist_remove;
  gtkcontainer_class->forall = xfce_tasklist_forall;
  gtkcontainer_class->child_type = xfce_tasklist_child_type;

  g_object_class_install_property (gobject_class,
                                   PROP_GROUPING,
                                   g_param_spec_uint ("grouping",
                                                      NULL, NULL,
                                                      XFCE_TASKLIST_GROUPING_MIN,
                                                      XFCE_TASKLIST_GROUPING_MAX,
                                                      XFCE_TASKLIST_GROUPING_DEFAULT,
                                                      EXO_PARAM_READWRITE));

  g_object_class_install_property (gobject_class,
                                   PROP_INCLUDE_ALL_WORKSPACES,
                                   g_param_spec_boolean ("include-all-workspaces",
                                                         NULL, NULL,
                                                         FALSE,
                                                         EXO_PARAM_READWRITE));

  g_object_class_install_property (gobject_class,
                                   PROP_FLAT_BUTTONS,
                                   g_param_spec_boolean ("flat-buttons",
                                                         NULL, NULL,
                                                         FALSE,
                                                         EXO_PARAM_READWRITE));

  g_object_class_install_property (gobject_class,
                                   PROP_SWITCH_WORKSPACE_ON_UNMINIMIZE,
                                   g_param_spec_boolean ("switch-workspace-on-unminimize",
                                                         NULL, NULL,
                                                         TRUE,
                                                         EXO_PARAM_READWRITE));

  g_object_class_install_property (gobject_class,
                                   PROP_SHOW_LABELS,
                                   g_param_spec_boolean ("show-labels",
                                                         NULL, NULL,
                                                         TRUE,
                                                         EXO_PARAM_READWRITE));

  g_object_class_install_property (gobject_class,
                                   PROP_SHOW_ONLY_MINIMIZED,
                                   g_param_spec_boolean ("show-only-minimized",
                                                         NULL, NULL,
                                                         FALSE,
                                                         EXO_PARAM_READWRITE));

  g_object_class_install_property (gobject_class,
                                   PROP_SHOW_WIREFRAMES,
                                   g_param_spec_boolean ("show-wireframes",
                                                         NULL, NULL,
                                                         FALSE,
                                                         EXO_PARAM_READWRITE));

  g_object_class_install_property (gobject_class,
                                   PROP_SHOW_HANDLE,
                                   g_param_spec_boolean ("show-handle",
                                                         NULL, NULL,
                                                         TRUE,
                                                         EXO_PARAM_READWRITE));

  g_object_class_install_property (gobject_class,
                                   PROP_SORT_ORDER,
                                   g_param_spec_uint ("sort-order",
                                                      NULL, NULL,
                                                      XFCE_TASKLIST_SORT_ORDER_MIN,
                                                      XFCE_TASKLIST_SORT_ORDER_MAX,
                                                      XFCE_TASKLIST_SORT_ORDER_DEFAULT,
                                                      EXO_PARAM_READWRITE));

  gtk_widget_class_install_style_property (gtkwidget_class,
                                           g_param_spec_int ("max-button-length",
                                                             NULL,
                                                             "The maximum length of a window button",
                                                             -1, G_MAXINT,
                                                             DEFAULT_MAX_BUTTON_LENGTH,
                                                             EXO_PARAM_READABLE));

  gtk_widget_class_install_style_property (gtkwidget_class,
                                           g_param_spec_int ("min-button-length",
                                                             NULL,
                                                             "The minumum length of a window button",
                                                             1, G_MAXINT,
                                                             DEFAULT_MIN_BUTTON_LENGTH,
                                                             EXO_PARAM_READABLE));

  gtk_widget_class_install_style_property (gtkwidget_class,
                                           g_param_spec_int ("max-button-size",
                                                             NULL,
                                                             "The maximum size of a window button",
                                                             1, G_MAXINT,
                                                             DEFAULT_BUTTON_SIZE,
                                                             EXO_PARAM_READABLE));

  gtk_widget_class_install_style_property (gtkwidget_class,
                                           g_param_spec_enum ("ellipsize-mode",
                                                              NULL,
                                                              "The ellipsize mode used for the button label",
                                                              PANGO_TYPE_ELLIPSIZE_MODE,
                                                              DEFAULT_ELLIPSIZE_MODE,
                                                              EXO_PARAM_READABLE));

  gtk_widget_class_install_style_property (gtkwidget_class,
                                           g_param_spec_int ("minimized-icon-lucency",
                                                             NULL,
                                                             "Lucent percentage of minimized icons",
                                                             0, 100,
                                                             DEFAULT_ICON_LUCENCY,
                                                             EXO_PARAM_READABLE));
  gtk_widget_class_install_style_property (gtkwidget_class,
                                           g_param_spec_int ("menu-max-width-chars",
                                                             NULL,
                                                             "Maximum chars in the overflow menu labels",
                                                             0, G_MAXINT,
                                                             DEFAULT_MENU_MAX_WIDTH_CHARS,
                                                             EXO_PARAM_READABLE));

  menu_icon_size = gtk_icon_size_from_name ("panel-tasklist-menu");
  if (menu_icon_size == GTK_ICON_SIZE_INVALID)
    menu_icon_size = gtk_icon_size_register ("panel-tasklist-menu",
                                             DEFAULT_MENU_ICON_SIZE,
                                             DEFAULT_MENU_ICON_SIZE);
}



static void
xfce_tasklist_init (XfceTasklist *tasklist)
{
  GTK_WIDGET_SET_FLAGS (tasklist, GTK_NO_WINDOW);

  tasklist->locked = 0;
  tasklist->screen = NULL;
  tasklist->windows = NULL;
  tasklist->skipped_windows = NULL;
  tasklist->horizontal = TRUE;
  tasklist->all_workspaces = FALSE;
  tasklist->button_relief = GTK_RELIEF_NORMAL;
  tasklist->switch_workspace = TRUE;
  tasklist->only_minimized = FALSE;
  tasklist->show_labels = TRUE;
  tasklist->class_groups = NULL;
  tasklist->show_wireframes = FALSE;
  tasklist->show_handle = TRUE;
#ifdef GDK_WINDOWING_X11
  tasklist->wireframe_window = 0;
#endif
  tasklist->max_button_length = DEFAULT_MAX_BUTTON_LENGTH;
  tasklist->min_button_length = DEFAULT_MIN_BUTTON_LENGTH;
  tasklist->max_button_size = DEFAULT_BUTTON_SIZE;
  tasklist->minimized_icon_lucency = DEFAULT_ICON_LUCENCY;
  tasklist->ellipsize_mode = DEFAULT_ELLIPSIZE_MODE;
  tasklist->grouping = XFCE_TASKLIST_GROUPING_DEFAULT;
  tasklist->sort_order = XFCE_TASKLIST_SORT_ORDER_DEFAULT;
  tasklist->menu_icon_size = DEFAULT_MENU_ICON_SIZE;
  tasklist->menu_max_width_chars = DEFAULT_MENU_MAX_WIDTH_CHARS;

  /* widgets for the overflow menu */
  tasklist->arrow_button = xfce_arrow_button_new (GTK_ARROW_DOWN);
  gtk_widget_set_parent (tasklist->arrow_button, GTK_WIDGET (tasklist));
  gtk_button_set_relief (GTK_BUTTON (tasklist->arrow_button), tasklist->button_relief);
  g_signal_connect (G_OBJECT (tasklist->arrow_button), "toggled",
      G_CALLBACK (xfce_tasklist_arrow_button_toggled), tasklist);
  gtk_widget_show (tasklist->arrow_button);
}



static void
xfce_tasklist_get_property (GObject    *object,
                            guint       prop_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
  XfceTasklist *tasklist = XFCE_TASKLIST (object);

  switch (prop_id)
    {
    case PROP_GROUPING:
      g_value_set_uint (value, tasklist->grouping);
      break;

    case PROP_INCLUDE_ALL_WORKSPACES:
      g_value_set_boolean (value, tasklist->all_workspaces);
      break;

    case PROP_FLAT_BUTTONS:
      g_value_set_boolean (value, !!(tasklist->button_relief == GTK_RELIEF_NONE));
      break;

    case PROP_SWITCH_WORKSPACE_ON_UNMINIMIZE:
      g_value_set_boolean (value, tasklist->switch_workspace);
      break;

    case PROP_SHOW_LABELS:
      g_value_set_boolean (value, tasklist->show_labels);
      break;

    case PROP_SHOW_ONLY_MINIMIZED:
      g_value_set_boolean (value, tasklist->only_minimized);
      break;

    case PROP_SHOW_WIREFRAMES:
      g_value_set_boolean (value, tasklist->show_wireframes);
      break;

    case PROP_SHOW_HANDLE:
      g_value_set_boolean (value, tasklist->show_handle);
      break;

    case PROP_SORT_ORDER:
      g_value_set_uint (value, tasklist->sort_order);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}



static void
xfce_tasklist_set_property (GObject      *object,
                            guint         prop_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
  XfceTasklist          *tasklist = XFCE_TASKLIST (object);
  XfceTasklistSortOrder  sort_order;

  switch (prop_id)
    {
    case PROP_GROUPING:
      tasklist->grouping = g_value_get_uint (value);
      break;

    case PROP_INCLUDE_ALL_WORKSPACES:
      xfce_tasklist_set_include_all_workspaces (tasklist, g_value_get_boolean (value));
      break;

    case PROP_FLAT_BUTTONS:
      xfce_tasklist_set_button_relief (tasklist,
                                       g_value_get_boolean (value) ?
                                         GTK_RELIEF_NONE : GTK_RELIEF_NORMAL);
      break;

    case PROP_SHOW_LABELS:
      xfce_tasklist_set_show_labels (tasklist, g_value_get_boolean (value));
      break;

    case PROP_SWITCH_WORKSPACE_ON_UNMINIMIZE:
      tasklist->switch_workspace = g_value_get_boolean (value);
      break;

    case PROP_SHOW_ONLY_MINIMIZED:
      xfce_tasklist_set_show_only_minimized (tasklist, g_value_get_boolean (value));
      break;

    case PROP_SHOW_WIREFRAMES:
      xfce_tasklist_set_show_wireframes (tasklist, g_value_get_boolean (value));
      break;

    case PROP_SHOW_HANDLE:
      tasklist->show_handle = g_value_get_boolean (value);
      break;

    case PROP_SORT_ORDER:
      sort_order = g_value_get_uint (value);
      if (tasklist->sort_order != sort_order)
        {
          tasklist->sort_order = sort_order;
          xfce_tasklist_sort (tasklist);
        }
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}



static void
xfce_tasklist_finalize (GObject *object)
{
  XfceTasklist *tasklist = XFCE_TASKLIST (object);

  /* data that should already be freed when disconnecting the screen */
  panel_return_if_fail (tasklist->windows == NULL);
  panel_return_if_fail (tasklist->skipped_windows == NULL);
  panel_return_if_fail (tasklist->screen == NULL);

  /* free the class group list, this is not empty because the
   * windows are (probably) still openened */
  if (tasklist->class_groups != NULL)
    g_slist_free (tasklist->class_groups);

#ifdef GDK_WINDOWING_X11
  /* destroy the wireframe window */
  xfce_tasklist_wireframe_destroy (tasklist);
#endif

  (*G_OBJECT_CLASS (xfce_tasklist_parent_class)->finalize) (object);
}



static void
xfce_tasklist_size_request (GtkWidget      *widget,
                            GtkRequisition *requisition)
{
  XfceTasklist   *tasklist = XFCE_TASKLIST (widget);
  gint            rows, cols;
  gint            n_windows;
  GtkRequisition  child_req;
  gint            length;
  GSList *li;
  XfceTasklistChild *child;

  for (li = tasklist->windows, n_windows = 0; li != NULL; li = li->next)
    {
      child = li->data;

      if (GTK_WIDGET_VISIBLE (child->button))
        {
          gtk_widget_size_request (child->button, &child_req);

          if (child->type == XFCE_TASKLIST_BUTTON_TYPE_WINDOW
              || child->type == XFCE_TASKLIST_BUTTON_TYPE_MENU)
            {
              child->type = XFCE_TASKLIST_BUTTON_TYPE_WINDOW;
              n_windows++;
            }
        }
    }

  tasklist->n_windows = n_windows;

  if (n_windows == 0)
    {
      length = 0;
    }
  else
    {
      rows = tasklist->size / tasklist->max_button_size;
      rows = CLAMP (rows, 1, n_windows);

      cols = n_windows / rows;
      if (cols * rows < n_windows)
        cols++;

      if (!tasklist->show_labels)
        length = (tasklist->size / rows) * cols;
      else if (tasklist->max_button_length != -1)
        length = cols * tasklist->max_button_length;
      else
        length = cols * DEFAULT_MAX_BUTTON_LENGTH;
    }

  /* set the requested sizes */
  if (tasklist->horizontal)
    {
      requisition->width = length;
      requisition->height = tasklist->size;
    }
  else
    {
      requisition->width = tasklist->size;
      requisition->height = length;
    }
}



static gint
xfce_tasklist_size_sort_window (gconstpointer a,
                                gconstpointer b)
{
  const XfceTasklistChild *child_a = a;
  const XfceTasklistChild *child_b = b;
  glong                    diff;

  diff = child_a->last_focused.tv_sec - child_b->last_focused.tv_sec;
  if (diff != 0)
    return CLAMP (diff, -1, 1);

  diff = child_a->last_focused.tv_usec - child_b->last_focused.tv_usec;
  return CLAMP (diff, -1, 1);
}



static void
xfce_tasklist_size_layout (XfceTasklist *tasklist,
                           GtkAllocation *alloc,
                           gint *n_rows,
                           gint *n_cols,
                           gboolean *arrow_visible)
{
  gint rows;
  gint min_button_length;
  gint cols;
  GSList *windows_scored = NULL, *li;
  XfceTasklistChild *child;
  gint max_button_length;
  gint n_buttons;
  gint n_buttons_target;

  rows = alloc->height / tasklist->max_button_size;
  if (rows < 1)
    rows = 1;

  cols = tasklist->n_windows / rows;
  if (cols * rows < tasklist->n_windows)
    cols++;

  if (tasklist->show_labels)
    min_button_length = tasklist->min_button_length;
  else
    min_button_length = alloc->height / rows;

  *arrow_visible = FALSE;

  if (min_button_length * cols <= alloc->width)
    {
      /* all the windows seem to fit */
      *n_rows = rows;
      *n_cols = cols;
    }
  else
    {
      /* we need to group something, first create a list with the
       * windows most suitable for grouping at the beginning that are
       * (should be) currently visible */
      for (li = tasklist->windows; li != NULL; li = li->next)
        {
          child = li->data;
          if (GTK_WIDGET_VISIBLE (child->button))
            windows_scored = g_slist_insert_sorted (windows_scored, child,
                                                    xfce_tasklist_size_sort_window);
        }

      panel_return_if_fail (g_slist_length (windows_scored) == (guint) tasklist->n_windows);

      if (!tasklist->show_labels)
        max_button_length = min_button_length;
      else if (tasklist->max_button_length != -1)
        max_button_length = tasklist->max_button_length;
      else
        max_button_length = DEFAULT_MAX_BUTTON_LENGTH;

      n_buttons = tasklist->n_windows;
      n_buttons_target = ((alloc->width / max_button_length) + 1) * rows;

      if (tasklist->grouping == XFCE_TASKLIST_GROUPING_AUTO)
        {
          /* try creating group buttons */
        }

      /* we now push the windows with the lowest score in the
       * overflow menu */
      if (n_buttons > n_buttons_target)
        {
          panel_debug (PANEL_DEBUG_DOMAIN_TASKLIST,
                       "Putting %d windows in overflow menu",
                       n_buttons - n_buttons_target);

          for (li = windows_scored;
               n_buttons > n_buttons_target && li != NULL;
               li = li->next, n_buttons--)
            {
              child = li->data;
              child->type = XFCE_TASKLIST_BUTTON_TYPE_MENU;
            }

          *arrow_visible = TRUE;
        }

      g_slist_free (windows_scored);

      cols = n_buttons / rows;
      if (cols * rows < n_buttons)
        cols++;

      *n_rows = rows;
      *n_cols = cols;
    }
}



static void
xfce_tasklist_size_allocate (GtkWidget     *widget,
                             GtkAllocation *allocation)
{
  XfceTasklist      *tasklist = XFCE_TASKLIST (widget);
  gint               rows, cols;
  gint               row;
  GtkAllocation      area = *allocation;
  GSList            *li;
  XfceTasklistChild *child;
  gint               i;
  GtkAllocation      child_alloc;
  gboolean           direction_rtl = gtk_widget_get_direction (widget) == GTK_TEXT_DIR_RTL;
  gint               w, x, y, h;
  gint               area_x, area_width;
  gboolean           arrow_visible;
  GtkRequisition     child_req;

  panel_return_if_fail (GTK_WIDGET_VISIBLE (tasklist->arrow_button));

  /* set widget allocation */
  widget->allocation = *allocation;

  /* swap integers with vertical orientation */
  if (!tasklist->horizontal)
    TRANSPOSE_AREA (area);
  panel_return_if_fail (area.height == tasklist->size);

  /* TODO if we compare the allocation with the requisition we can
   * do a fast path to the child allocation, i think */

  /* useless but hides compiler warning */
  w = h = x = y = rows = cols = 0;

  xfce_tasklist_size_layout (tasklist, &area, &rows, &cols, &arrow_visible);

  /* allocate the arrow button for the overflow menu */
  child_alloc.width = ARROW_BUTTON_SIZE;
  child_alloc.height = area.height;

  if (arrow_visible)
    {
      child_alloc.x = area.x;
      child_alloc.y = area.y;

      if (!direction_rtl)
        child_alloc.x += area.width - ARROW_BUTTON_SIZE;
      else
        area.x += ARROW_BUTTON_SIZE;

      area.width -= ARROW_BUTTON_SIZE;

      /* position the arrow in the correct position */
      if (!tasklist->horizontal)
        TRANSPOSE_AREA (child_alloc);
    }
  else
    {
      child_alloc.x = child_alloc.y = -9999;
    }

  gtk_widget_size_allocate (tasklist->arrow_button, &child_alloc);

  area_x = area.x;
  area_width = area.width;

  /* allocate all the children */
  for (li = tasklist->windows, i = 0; li != NULL; li = li->next)
    {
      child = li->data;

      /* skip hidden buttons */
      if (!GTK_WIDGET_VISIBLE (child->button))
        continue;

      if (G_LIKELY (child->type == XFCE_TASKLIST_BUTTON_TYPE_WINDOW))
        {
          row = (i % rows);
          if (row == 0)
            {
              w = area_width / cols--;
              if (tasklist->max_button_length > 0
                  && w > tasklist->max_button_length)
               w = tasklist->max_button_length;
              x = area_x;

              area_width -= w;
              area_x += w;

              y = area.y;
              h = area.height;
            }

          child_alloc.y = y;
          child_alloc.x = x;
          child_alloc.width = MAX (w, 1); /* TODO this is a workaround */
          child_alloc.height = h / (rows - row);

          h -= child_alloc.height;
          y += child_alloc.height;

          if (direction_rtl)
            child_alloc.x = area.x + area.width - (child_alloc.x - area.x) - child_alloc.width;

          /* allocate the child */
          if (!tasklist->horizontal)
            TRANSPOSE_AREA (child_alloc);

          /* increase the position counter */
          i++;
        }
      else
        {
          gtk_widget_get_child_requisition (child->button, &child_req);

          /* move the button offscreen */
          child_alloc.y = child_alloc.x = -9999;
          child_alloc.width = child_req.width;
          child_alloc.height = child_req.height;
        }

      gtk_widget_size_allocate (child->button, &child_alloc);
    }
}



static void
xfce_tasklist_style_set (GtkWidget *widget,
                         GtkStyle  *previous_style)
{
  XfceTasklist *tasklist = XFCE_TASKLIST (widget);
  gint          max_button_length;
  gint          max_button_size;
  gint          min_button_length;
  gint          w, h;

  /* let gtk update the widget style */
  (*GTK_WIDGET_CLASS (xfce_tasklist_parent_class)->style_set) (widget, previous_style);

  /* read the style properties */
  gtk_widget_style_get (GTK_WIDGET (tasklist),
                        "max-button-length", &max_button_length,
                        "min-button-length", &min_button_length,
                        "ellipsize-mode", &tasklist->ellipsize_mode,
                        "max-button-size", &max_button_size,
                        "minimized-icon-lucency", &tasklist->minimized_icon_lucency,
                        "menu-max-width-chars", &tasklist->menu_max_width_chars,
                        NULL);

  if (gtk_icon_size_lookup (menu_icon_size, &w, &h))
    tasklist->menu_icon_size = MIN (w, h);

  /* update the widget */
  if (tasklist->max_button_length != max_button_length
      || tasklist->max_button_size != max_button_size
      || tasklist->min_button_length != min_button_length)
    {
      if (max_button_length > 0)
        {
          /* prevent abuse of the min/max button length */
          tasklist->max_button_length = MAX (min_button_length, max_button_length);
          tasklist->min_button_length = MIN (min_button_length, max_button_length);
        }
      else
        {
          tasklist->max_button_length = max_button_length;
          tasklist->min_button_length = min_button_length;
        }

      tasklist->max_button_size = max_button_size;

      gtk_widget_queue_resize (widget);
    }
}



static void
xfce_tasklist_realize (GtkWidget *widget)
{
  XfceTasklist *tasklist = XFCE_TASKLIST (widget);

  (*GTK_WIDGET_CLASS (xfce_tasklist_parent_class)->realize) (widget);

  /* we now have a screen */
  xfce_tasklist_connect_screen (tasklist);
}



static void
xfce_tasklist_unrealize (GtkWidget *widget)
{
  XfceTasklist *tasklist = XFCE_TASKLIST (widget);

  /* we're going to loose the screen */
  xfce_tasklist_disconnect_screen (tasklist);

  (*GTK_WIDGET_CLASS (xfce_tasklist_parent_class)->unrealize) (widget);
}



static void
xfce_tasklist_remove (GtkContainer *container,
                      GtkWidget    *widget)
{
  XfceTasklist      *tasklist = XFCE_TASKLIST (container);
  gboolean           was_visible;
  XfceTasklistChild *child;
  GSList            *li;

  for (li = tasklist->windows; li != NULL; li = li->next)
    {
      child = li->data;

      if (child->button == widget)
        {
          tasklist->windows = g_slist_delete_link (tasklist->windows, li);

          was_visible = GTK_WIDGET_VISIBLE (widget);

          gtk_widget_unparent (child->button);

          g_slice_free (XfceTasklistChild, child);

          /* queue a resize if needed */
          if (G_LIKELY (was_visible))
            gtk_widget_queue_resize (GTK_WIDGET (container));

          break;
        }
    }
}



static void
xfce_tasklist_forall (GtkContainer *container,
                      gboolean      include_internals,
                      GtkCallback   callback,
                      gpointer      callback_data)
{
  XfceTasklist      *tasklist = XFCE_TASKLIST (container);
  GSList            *children = tasklist->windows;
  XfceTasklistChild *child;

  if (include_internals)
    (* callback) (tasklist->arrow_button, callback_data);

  while (children != NULL)
    {
      child = children->data;
      children = children->next;

      (* callback) (child->button, callback_data);
    }
}



static GType
xfce_tasklist_child_type (GtkContainer *container)
{
  return GTK_TYPE_WIDGET;
}



static void
xfce_tasklist_arrow_button_menu_destroy (GtkWidget    *menu,
                                         XfceTasklist *tasklist)
{
  panel_return_if_fail (XFCE_IS_TASKLIST (tasklist));
  panel_return_if_fail (GTK_IS_TOGGLE_BUTTON (tasklist->arrow_button));
  panel_return_if_fail (GTK_IS_WIDGET (menu));

  gtk_widget_destroy (menu);

  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (tasklist->arrow_button), FALSE);

#ifdef GDK_WINDOWING_X11
  /* make sure the wireframe is hidden */
  xfce_tasklist_wireframe_hide (tasklist);
#endif
}



static void
xfce_tasklist_arrow_button_toggled (GtkWidget    *button,
                                    XfceTasklist *tasklist)
{
  GSList            *li;
  XfceTasklistChild *child;
  GtkWidget         *mi;
  GtkWidget         *menu;

  panel_return_if_fail (XFCE_IS_TASKLIST (tasklist));
  panel_return_if_fail (GTK_IS_TOGGLE_BUTTON (button));
  panel_return_if_fail (tasklist->arrow_button == button);

  if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (button)))
    {
      menu = gtk_menu_new ();
      g_signal_connect (G_OBJECT (menu), "selection-done",
          G_CALLBACK (xfce_tasklist_arrow_button_menu_destroy), tasklist);

      for (li = tasklist->windows; li != NULL; li = li->next)
        {
          child = li->data;

          if (child->type != XFCE_TASKLIST_BUTTON_TYPE_MENU)
            continue;

          mi = xfce_tasklist_button_proxy_menu_item (child);
          gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
          gtk_widget_show (mi);
        }

      gtk_menu_attach_to_widget (GTK_MENU (menu), button, NULL);
      gtk_menu_popup (GTK_MENU (menu), NULL, NULL,
                      xfce_panel_plugin_position_menu,
                      xfce_tasklist_get_panel_plugin (tasklist),
                      1, gtk_get_current_event_time ());
    }
}



static void
xfce_tasklist_connect_screen (XfceTasklist *tasklist)
{
  GdkScreen *screen;

  panel_return_if_fail (XFCE_IS_TASKLIST (tasklist));
  panel_return_if_fail (tasklist->screen == NULL);

  /* set the new screen */
  screen = gtk_widget_get_screen (GTK_WIDGET (tasklist));
  tasklist->screen = wnck_screen_get (gdk_screen_get_number (screen));

  /* monitor screen changes */
  g_signal_connect (G_OBJECT (tasklist->screen), "active-window-changed",
      G_CALLBACK (xfce_tasklist_active_window_changed), tasklist);
  g_signal_connect (G_OBJECT (tasklist->screen), "active-workspace-changed",
      G_CALLBACK (xfce_tasklist_active_workspace_changed), tasklist);
  g_signal_connect (G_OBJECT (tasklist->screen), "window-opened",
      G_CALLBACK (xfce_tasklist_window_added), tasklist);
  g_signal_connect (G_OBJECT (tasklist->screen), "window-closed",
      G_CALLBACK (xfce_tasklist_window_removed), tasklist);
  g_signal_connect (G_OBJECT (tasklist->screen), "viewports-changed",
      G_CALLBACK (xfce_tasklist_viewports_changed), tasklist);
}



static void
xfce_tasklist_disconnect_screen (XfceTasklist *tasklist)
{
  GSList            *li, *lnext;
  XfceTasklistChild *child;
  guint              n;

  panel_return_if_fail (XFCE_IS_TASKLIST (tasklist));
  panel_return_if_fail (WNCK_IS_SCREEN (tasklist->screen));

  /* disconnect monitor signals */
  n = g_signal_handlers_disconnect_matched (G_OBJECT (tasklist->screen),
      G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, tasklist);
  panel_return_if_fail (n == 5);

  /* disconnect from all skipped windows */
  for (li = tasklist->skipped_windows; li != NULL; li = lnext)
    {
      lnext = li->next;
      panel_return_if_fail (wnck_window_is_skip_tasklist (WNCK_WINDOW (li->data)));
      xfce_tasklist_window_removed (tasklist->screen, li->data, tasklist);
    }

  /* remove all the windows */
  for (li = tasklist->windows; li != NULL; li = lnext)
    {
      lnext = li->next;
      child = li->data;

      /* do a fake window remove */
      if (G_LIKELY (child->type != XFCE_TASKLIST_BUTTON_TYPE_GROUP))
        xfce_tasklist_window_removed (tasklist->screen, child->window, tasklist);
      else
        xfce_tasklist_group_button_remove (tasklist, child->class_group);
    }

  panel_assert (tasklist->windows == NULL);
  panel_assert (tasklist->skipped_windows == NULL);

  tasklist->screen = NULL;
}



static void
xfce_tasklist_active_window_changed (WnckScreen   *screen,
                                     WnckWindow   *previous_window,
                                     XfceTasklist *tasklist)
{
  WnckWindow        *active_window;
  GSList            *li;
  XfceTasklistChild *child;

  panel_return_if_fail (WNCK_IS_SCREEN (screen));
  panel_return_if_fail (previous_window == NULL || WNCK_IS_WINDOW (previous_window));
  panel_return_if_fail (XFCE_IS_TASKLIST (tasklist));
  panel_return_if_fail (tasklist->screen == screen);

  /* get the new active window */
  active_window = wnck_screen_get_active_window (screen);

  /* lock the taskbar */
  xfce_taskbar_lock (tasklist);

  for (li = tasklist->windows; li != NULL; li = li->next)
    {
      child = li->data;

      /* skip hidden buttons */
      /* TODO the visible check probably breaks with grouping */
      if (!GTK_WIDGET_VISIBLE (child->button)
          || !(child->window == previous_window
               || child->window == active_window))
        continue;

      /* update timestamp for window */
      if (child->window == active_window)
        g_get_current_time (&child->last_focused);

      /* set the toggle button state */
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (child->button),
                                    !!(child->window == active_window));
    }

  /* release the lock */
  xfce_taskbar_unlock (tasklist);
}



static void
xfce_tasklist_active_workspace_changed (WnckScreen    *screen,
                                        WnckWorkspace *previous_workspace,
                                        XfceTasklist  *tasklist)
{
  GSList            *li;
  WnckWorkspace     *active_ws;
  XfceTasklistChild *child;

  panel_return_if_fail (WNCK_IS_SCREEN (screen));
  panel_return_if_fail (previous_workspace == NULL || WNCK_IS_WORKSPACE (previous_workspace));
  panel_return_if_fail (XFCE_IS_TASKLIST (tasklist));
  panel_return_if_fail (tasklist->screen == screen);

  /* leave when we show tasks from all workspaces or are locked */
  if (xfce_taskbar_is_locked (tasklist)
      || tasklist->all_workspaces)
    return;

  /* walk all the children and hide buttons on other workspaces */
  active_ws = wnck_screen_get_active_workspace (screen);
  for (li = tasklist->windows; li != NULL; li = li->next)
    {
      child = li->data;

      if (child->type == XFCE_TASKLIST_BUTTON_TYPE_GROUP
          || xfce_tasklist_button_visible (child, tasklist, active_ws))
        gtk_widget_show (child->button);
      else
        gtk_widget_hide (child->button);
    }
}



static void
xfce_tasklist_window_added (WnckScreen   *screen,
                            WnckWindow   *window,
                            XfceTasklist *tasklist)
{
  XfceTasklistChild *child;

  panel_return_if_fail (WNCK_IS_SCREEN (screen));
  panel_return_if_fail (WNCK_IS_WINDOW (window));
  panel_return_if_fail (XFCE_IS_TASKLIST (tasklist));
  panel_return_if_fail (tasklist->screen == screen);
  panel_return_if_fail (wnck_window_get_screen (window) == screen);

  /* ignore this window, but watch it for state changes */
  if (wnck_window_is_skip_tasklist (window))
    {
      tasklist->skipped_windows = g_slist_prepend (tasklist->skipped_windows, window);
      g_signal_connect (G_OBJECT (window), "state-changed",
          G_CALLBACK (xfce_tasklist_skipped_windows_state_changed), tasklist);

      return;
    }

  /* create new window button */
  child = xfce_tasklist_button_new (window, tasklist);

  /* initial visibility of the function */
  if (xfce_tasklist_button_visible (child, tasklist, wnck_screen_get_active_workspace (screen)))
    gtk_widget_show (child->button);

  if (G_LIKELY (child->class_group != NULL))
    {
      /* we need to ref the class group else the value returned from
       * wnck_window_get_class_group() is null */
      panel_return_if_fail (WNCK_IS_CLASS_GROUP (child->class_group));
      g_object_ref (G_OBJECT (child->class_group));

      /* prepend the class group if it's new */
      if (g_slist_find (tasklist->class_groups, child->class_group) == NULL)
        {
          tasklist->class_groups = g_slist_prepend (tasklist->class_groups,
                                                    child->class_group);
        }
    }

  gtk_widget_queue_resize (GTK_WIDGET (tasklist));
}



static void
xfce_tasklist_window_removed (WnckScreen   *screen,
                              WnckWindow   *window,
                              XfceTasklist *tasklist)
{
  GSList            *li;
  XfceTasklistChild *child;
  GList             *windows, *lp;
  gboolean           remove_class_group = TRUE;
  guint              n;

  panel_return_if_fail (WNCK_IS_SCREEN (screen));
  panel_return_if_fail (WNCK_IS_WINDOW (window));
  panel_return_if_fail (XFCE_IS_TASKLIST (tasklist));
  panel_return_if_fail (tasklist->screen == screen);

  /* check if the window is in our skipped window list */
  if (wnck_window_is_skip_tasklist (window)
      && (li = g_slist_find (tasklist->skipped_windows, window)) != NULL)
    {
      tasklist->skipped_windows = g_slist_delete_link (tasklist->skipped_windows, li);
      g_signal_handlers_disconnect_by_func (G_OBJECT (window),
          G_CALLBACK (xfce_tasklist_skipped_windows_state_changed), tasklist);

      return;
    }

  /* remove the child from the taskbar */
  for (li = tasklist->windows; li != NULL; li = li->next)
    {
      child = li->data;

      if (child->window == window)
        {
          if (child->class_group != NULL)
            {
              /* remove the class group from the internal list if this
               * was the last window in the group */
              windows = wnck_class_group_get_windows (child->class_group);
              for (lp = windows; remove_class_group && lp != NULL; lp = lp->next)
                if (!wnck_window_is_skip_tasklist (WNCK_WINDOW (lp->data)))
                  remove_class_group = FALSE;

              if (remove_class_group)
                {
                  tasklist->class_groups = g_slist_remove (tasklist->class_groups,
                                                           child->class_group);
                }

              panel_return_if_fail (WNCK_IS_CLASS_GROUP (child->class_group));
              g_object_unref (G_OBJECT (child->class_group));
            }

          /* disconnect from all the window watch functions */
          panel_return_if_fail (WNCK_IS_WINDOW (window));
          n = g_signal_handlers_disconnect_matched (G_OBJECT (window),
              G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, child);
          panel_return_if_fail (n == 4);

          /* destroy the button, this will free the child data in the
           * container remove function */
          gtk_widget_destroy (child->button);

          break;
        }
    }
}



static void
xfce_tasklist_viewports_changed (WnckScreen   *screen,
                                 XfceTasklist *tasklist)
{
  panel_return_if_fail (WNCK_IS_SCREEN (screen));
  panel_return_if_fail (XFCE_IS_TASKLIST (tasklist));
  panel_return_if_fail (tasklist->screen == screen);

  /* TODO, rebuild the tasklist (only when we filter windows on
   * this monitor?) */
}



static void
xfce_tasklist_skipped_windows_state_changed (WnckWindow      *window,
                                             WnckWindowState  changed_state,
                                             WnckWindowState  new_state,
                                             XfceTasklist    *tasklist)
{
  panel_return_if_fail (XFCE_IS_TASKLIST (tasklist));
  panel_return_if_fail (WNCK_IS_WINDOW (window));
  panel_return_if_fail (g_slist_find (tasklist->skipped_windows, window) != NULL);

  if (PANEL_HAS_FLAG (changed_state, WNCK_WINDOW_STATE_SKIP_TASKLIST))
    {
      /* remove from list */
      tasklist->skipped_windows = g_slist_remove (tasklist->skipped_windows, window);
      g_signal_handlers_disconnect_by_func (G_OBJECT (window),
          G_CALLBACK (xfce_tasklist_skipped_windows_state_changed), tasklist);

      /* pretend a normal window insert */
      xfce_tasklist_window_added (wnck_window_get_screen (window), window, tasklist);
    }
}



static void
xfce_tasklist_sort (XfceTasklist *tasklist)
{
  panel_return_if_fail (XFCE_IS_TASKLIST (tasklist));

  tasklist->windows = g_slist_sort_with_data (tasklist->windows,
                                               xfce_tasklist_button_compare,
                                               tasklist);

  gtk_widget_queue_resize (GTK_WIDGET (tasklist));
}



static GtkWidget *
xfce_tasklist_get_panel_plugin (XfceTasklist *tasklist)
{
  GtkWidget *p;

  panel_return_val_if_fail (XFCE_IS_TASKLIST (tasklist), NULL);

  /* look in the parents for the panel plugin */
  for (p = GTK_WIDGET (tasklist); p != NULL; p = gtk_widget_get_parent (p))
    if (g_type_is_a (G_OBJECT_TYPE (p), XFCE_TYPE_PANEL_PLUGIN))
      return p;

  return NULL;
}



/**
 * Wire Frame
 **/
#ifdef GDK_WINDOWING_X11
static void
xfce_tasklist_wireframe_hide (XfceTasklist *tasklist)
{
  GdkDisplay *dpy;

  panel_return_if_fail (XFCE_IS_TASKLIST (tasklist));

  if (tasklist->wireframe_window != 0)
    {
      /* unmap the window */
      dpy = gtk_widget_get_display (GTK_WIDGET (tasklist));
      XUnmapWindow (GDK_DISPLAY_XDISPLAY (dpy), tasklist->wireframe_window);
    }
}



static void
xfce_tasklist_wireframe_destroy (XfceTasklist *tasklist)
{
  GdkDisplay *dpy;

  panel_return_if_fail (XFCE_IS_TASKLIST (tasklist));

  if (tasklist->wireframe_window != 0)
    {
      /* unmap and destroy the window */
      dpy = gtk_widget_get_display (GTK_WIDGET (tasklist));
      XUnmapWindow (GDK_DISPLAY_XDISPLAY (dpy), tasklist->wireframe_window);
      XDestroyWindow (GDK_DISPLAY_XDISPLAY (dpy), tasklist->wireframe_window);

      tasklist->wireframe_window = 0;
    }
}



static void
xfce_tasklist_wireframe_update (XfceTasklist      *tasklist,
                                XfceTasklistChild *child)
{
  Display              *dpy;
  GdkDisplay           *gdpy;
  gint                  x, y, width, height;
  XSetWindowAttributes  attrs;
  GC                    gc;
  XRectangle            xrect;

  panel_return_if_fail (XFCE_IS_TASKLIST (tasklist));
  panel_return_if_fail (tasklist->show_wireframes == TRUE);
  panel_return_if_fail (WNCK_IS_WINDOW (child->window));

  /* get the window geometry */
  wnck_window_get_geometry (child->window, &x, &y, &width, &height);

  gdpy = gtk_widget_get_display (GTK_WIDGET (tasklist));
  dpy = GDK_DISPLAY_XDISPLAY (gdpy);

  if (G_LIKELY (tasklist->wireframe_window != 0))
    {
      /* reposition the wireframe */
      XMoveResizeWindow (dpy, tasklist->wireframe_window, x, y, width, height);

      /* full window rectangle */
      xrect.x = 0;
      xrect.y = 0;
      xrect.width = width;
      xrect.height = height;

      /* we need to restore the window first */
      XShapeCombineRectangles (dpy, tasklist->wireframe_window, ShapeBounding,
                               0, 0, &xrect, 1, ShapeSet, Unsorted);
    }
  else
    {
      /* set window attributes */
      attrs.override_redirect = True;
      attrs.background_pixel = 0x000000;

      /* create new window */
      tasklist->wireframe_window = XCreateWindow (dpy, DefaultRootWindow (dpy),
                                                  x, y, width, height, 0,
                                                  CopyFromParent, InputOutput,
                                                  CopyFromParent,
                                                  CWOverrideRedirect | CWBackPixel,
                                                  &attrs);
    }

  /* create rectangle what will be 'transparent' in the window */
  xrect.x = WIREFRAME_SIZE;
  xrect.y = WIREFRAME_SIZE;
  xrect.width = width - WIREFRAME_SIZE * 2;
  xrect.height = height - WIREFRAME_SIZE * 2;

  /* substruct rectangle from the window */
  XShapeCombineRectangles (dpy, tasklist->wireframe_window, ShapeBounding,
                           0, 0, &xrect, 1, ShapeSubtract, Unsorted);

  /* map the window */
  XMapWindow (dpy, tasklist->wireframe_window);

  /* create a white gc */
  gc = XCreateGC (dpy, tasklist->wireframe_window, 0, NULL);
  XSetForeground (dpy, gc, 0xffffff);

  /* draw the outer white rectangle */
  XDrawRectangle (dpy, tasklist->wireframe_window, gc,
                  0, 0, width - 1, height - 1);

  /* draw the inner white rectangle */
  XDrawRectangle (dpy, tasklist->wireframe_window, gc,
                  WIREFRAME_SIZE - 1, WIREFRAME_SIZE - 1,
                  width - 2 * (WIREFRAME_SIZE - 1) - 1,
                  height - 2 * (WIREFRAME_SIZE - 1) - 1);

  XFreeGC (dpy, gc);
}
#endif



/**
 * Tasklist Buttons
 **/
static gint
xfce_tasklist_button_compare (gconstpointer child_a,
                              gconstpointer child_b,
                              gpointer      user_data)
{
  const XfceTasklistChild *a = child_a, *b = child_b;
  XfceTasklist            *tasklist = XFCE_TASKLIST (user_data);
  gint                     retval;
  WnckClassGroup          *class_group_a, *class_group_b;
  const gchar             *name_a = NULL, *name_b = NULL;
  WnckWorkspace           *workspace_a, *workspace_b;
  gint                     num_a, num_b;

  panel_return_val_if_fail (a->type == XFCE_TASKLIST_BUTTON_TYPE_GROUP
                            || WNCK_IS_WINDOW (a->window), 0);
  panel_return_val_if_fail (b->type == XFCE_TASKLIST_BUTTON_TYPE_GROUP
                            || WNCK_IS_WINDOW (b->window), 0);

  if (tasklist->all_workspaces)
    {
      /* get workspace (this is slightly inefficient because the WnckWindow
       * also stores the workspace number, not the structure, and we use that
       * for comparing too */
      workspace_a = a->window != NULL ? wnck_window_get_workspace (a->window) : NULL;
      workspace_b = b->window != NULL ? wnck_window_get_workspace (b->window) : NULL;

      /* skip this if windows are in same worspace, or both pinned (== NULL) */
      if (workspace_a != workspace_b)
        {
          /* NULL means the window is pinned */
          if (workspace_a == NULL)
            workspace_a = wnck_screen_get_active_workspace (tasklist->screen);
          if (workspace_b == NULL)
            workspace_b = wnck_screen_get_active_workspace (tasklist->screen);

          /* compare by workspace number */
          num_a = wnck_workspace_get_number (workspace_a);
          num_b = wnck_workspace_get_number (workspace_b);
          if (num_a != num_b)
            return num_a - num_b;
        }
    }

  if (tasklist->sort_order == XFCE_TASKLIST_SORT_ORDER_GROUP_TITLE
      || tasklist->sort_order == XFCE_TASKLIST_SORT_ORDER_GROUP_TIMESTAMP)
    {
      /* compare by class group names */
      class_group_a = a->class_group;
      class_group_b = b->class_group;

      /* skip this if windows are in same group (or both NULL) */
      if (class_group_a != class_group_b)
        {
          /* get the group name if available */
          if (G_LIKELY (class_group_a != NULL))
            name_a = wnck_class_group_get_name (class_group_a);
          if (G_LIKELY (class_group_b != NULL))
            name_b = wnck_class_group_get_name (class_group_b);

          /* if there is no class group name, use the window name */
          if (G_UNLIKELY (exo_str_is_empty (name_a)))
            name_a = wnck_window_get_name (a->window);
          if (G_UNLIKELY (exo_str_is_empty (name_b)))
            name_b = wnck_window_get_name (b->window);

          retval = strcasecmp (name_a, name_b);
          if (retval != 0)
            return retval;
        }
      else if (a->type != b->type)
        {
          /* put the group in front of the other window buttons
           * with the same group */
          return b->type - a->type;
        }
    }

  if (tasklist->sort_order == XFCE_TASKLIST_SORT_ORDER_TIMESTAMP
      || tasklist->sort_order == XFCE_TASKLIST_SORT_ORDER_GROUP_TIMESTAMP)
    {
      return a->unique_id - b->unique_id;
    }
  else
    {
      if (a->window != NULL)
        name_a = wnck_window_get_name (a->window);
      else if (a->class_group != NULL)
        name_a = wnck_class_group_get_name (a->class_group);
      else
        name_a = "";

      if (b->window != NULL)
        name_b = wnck_window_get_name (b->window);
      else if (b->class_group != NULL)
        name_b = wnck_class_group_get_name (b->class_group);
      else
        name_b = "";

      return strcasecmp (name_a, name_b);
    }
}



static void
xfce_tasklist_button_icon_changed (WnckWindow        *window,
                                   XfceTasklistChild *child)
{
  GdkPixbuf    *pixbuf;
  GdkPixbuf    *lucent = NULL;
  XfceTasklist *tasklist = child->tasklist;

  panel_return_if_fail (XFCE_IS_TASKLIST (tasklist));
  panel_return_if_fail (XFCE_IS_PANEL_IMAGE (child->icon));
  panel_return_if_fail (WNCK_IS_WINDOW (window));
  panel_return_if_fail (child->window == window);

  /* 0 means icons are disabled */
  if (tasklist->minimized_icon_lucency == 0)
    return;

  /* get the window icon */
  if (tasklist->show_labels)
    pixbuf = wnck_window_get_mini_icon (window);
  else
    pixbuf = wnck_window_get_icon (window);

  /* leave when there is no valid pixbuf */
  if (G_UNLIKELY (pixbuf == NULL))
    {
      xfce_panel_image_clear (XFCE_PANEL_IMAGE (child->icon));
      return;
    }

  /* create a spotlight version of the icon when minimized */
  if (!tasklist->only_minimized
      && tasklist->minimized_icon_lucency < 100
      && wnck_window_is_minimized (window))
    {
      lucent = exo_gdk_pixbuf_lucent (pixbuf, tasklist->minimized_icon_lucency);
      if (G_UNLIKELY (lucent != NULL))
        pixbuf = lucent;
    }

  xfce_panel_image_set_from_pixbuf (XFCE_PANEL_IMAGE (child->icon), pixbuf);

  if (lucent != NULL && lucent != pixbuf)
    g_object_unref (G_OBJECT (lucent));
}



static void
xfce_tasklist_button_name_changed (WnckWindow        *window,
                                   XfceTasklistChild *child)
{
  const gchar *name;
  gchar       *label = NULL;

  panel_return_if_fail (window == NULL || child->window == window);
  panel_return_if_fail (WNCK_IS_WINDOW (child->window));
  panel_return_if_fail (XFCE_IS_TASKLIST (child->tasklist));

  name = wnck_window_get_name (child->window);
  gtk_widget_set_tooltip_text (GTK_WIDGET (child->button), name);

  /* create the button label */
  if (!child->tasklist->only_minimized
      && wnck_window_is_minimized (child->window))
    name = label = g_strdup_printf ("[%s]", name);
  else if (wnck_window_is_shaded (child->window))
    name = label = g_strdup_printf ("=%s=", name);

  gtk_label_set_text (GTK_LABEL (child->label), name);

  g_free (label);

  /* if window is null, we have not inserted the button the in
   * tasklist, so no need to sort, because we insert with sorting */
  if (window != NULL)
    xfce_tasklist_sort (child->tasklist);
}



static void
xfce_tasklist_button_state_changed (WnckWindow        *window,
                                    WnckWindowState    changed_state,
                                    WnckWindowState    new_state,
                                    XfceTasklistChild *child)
{
  gboolean      blink;
  WnckScreen   *screen;
  XfceTasklist *tasklist;

  panel_return_if_fail (WNCK_IS_WINDOW (window));
  panel_return_if_fail (child->window == window);
  panel_return_if_fail (XFCE_IS_TASKLIST (child->tasklist));

  /* remove if the new state is hidding the window from the tasklist */
  if (PANEL_HAS_FLAG (changed_state, WNCK_WINDOW_STATE_SKIP_TASKLIST))
    {
      screen = wnck_window_get_screen (window);
      tasklist = child->tasklist;

      /* remove button from tasklist */
      xfce_tasklist_window_removed (screen, window, child->tasklist);

      /* add the window to the skipped_windows list */
      xfce_tasklist_window_added (screen, window, tasklist);

      return;
    }

  /* update the button name */
  if (PANEL_HAS_FLAG (changed_state, WNCK_WINDOW_STATE_SHADED | WNCK_WINDOW_STATE_MINIMIZED)
      && !child->tasklist->only_minimized)
    xfce_tasklist_button_name_changed (window, child);

  /* update the button icon if needed */
  if (PANEL_HAS_FLAG (changed_state, WNCK_WINDOW_STATE_MINIMIZED))
    {
      if (G_UNLIKELY (child->tasklist->only_minimized))
        {
          if (PANEL_HAS_FLAG (new_state, WNCK_WINDOW_STATE_MINIMIZED))
            gtk_widget_show (child->button);
          else
            gtk_widget_hide (child->button);
        }
      else
        {
          /* update the icon (lucent) */
          xfce_tasklist_button_icon_changed (window, child);
        }
    }

  /* update the blinking state */
  if (PANEL_HAS_FLAG (changed_state, WNCK_WINDOW_STATE_DEMANDS_ATTENTION)
      || PANEL_HAS_FLAG (changed_state, WNCK_WINDOW_STATE_URGENT))
    {
      /* only start blinking if the window requesting urgentcy
       * notification is not the active window */
      blink = wnck_window_or_transient_needs_attention (window);
      if (!blink || (blink && !wnck_window_is_active (window)))
        xfce_arrow_button_set_blinking (XFCE_ARROW_BUTTON (child->button), blink);
    }
}



static void
xfce_tasklist_button_workspace_changed (WnckWindow        *window,
                                        XfceTasklistChild *child)
{
  panel_return_if_fail (child->window == window);
  panel_return_if_fail (XFCE_IS_TASKLIST (child->tasklist));

  if (child->tasklist->all_workspaces)
    xfce_tasklist_sort (child->tasklist);
  else if (!wnck_window_is_pinned (child->window))
    gtk_widget_hide (child->button);
}



#ifdef GDK_WINDOWING_X11
static void
xfce_tasklist_button_geometry_changed (WnckWindow        *window,
                                       XfceTasklistChild *child)
{
  panel_return_if_fail (child->window == window);
  panel_return_if_fail (XFCE_IS_TASKLIST (child->tasklist));

  xfce_tasklist_wireframe_update (child->tasklist, child);
}



static gboolean
xfce_tasklist_button_leave_notify_event (GtkWidget         *button,
                                         GdkEventCrossing  *event,
                                         XfceTasklistChild *child)
{
  panel_return_val_if_fail (XFCE_IS_TASKLIST (child->tasklist), FALSE);
  panel_return_val_if_fail (child->type == XFCE_TASKLIST_BUTTON_TYPE_WINDOW
                            || child->type == XFCE_TASKLIST_BUTTON_TYPE_MENU, FALSE);

  /* disconnect signals */
  g_signal_handlers_disconnect_by_func (button,
      xfce_tasklist_button_leave_notify_event, child);
  g_signal_handlers_disconnect_by_func (child->window,
      xfce_tasklist_button_geometry_changed, child);

  /* unmap and destroy the wireframe window */
  xfce_tasklist_wireframe_hide (child->tasklist);

  return FALSE;
}
#endif



static gboolean
xfce_tasklist_button_enter_notify_event (GtkWidget         *button,
                                         GdkEventCrossing  *event,
                                         XfceTasklistChild *child)
{
  panel_return_val_if_fail (XFCE_IS_TASKLIST (child->tasklist), FALSE);
  panel_return_val_if_fail (child->type == XFCE_TASKLIST_BUTTON_TYPE_WINDOW
                            || child->type == XFCE_TASKLIST_BUTTON_TYPE_MENU, FALSE);
  panel_return_val_if_fail (GTK_IS_WIDGET (button), FALSE);
  panel_return_val_if_fail (WNCK_IS_WINDOW (child->window), FALSE);

#ifdef GDK_WINDOWING_X11
  /* leave when there is nothing to do */
  if (!child->tasklist->show_wireframes)
    return FALSE;

  /* show wireframe for the child */
  xfce_tasklist_wireframe_update (child->tasklist, child);

  /* connect signal to destroy the window when the user leaves the button */
  g_signal_connect (G_OBJECT (button), "leave-notify-event",
      G_CALLBACK (xfce_tasklist_button_leave_notify_event), child);

  /* watch geometry changes */
  g_signal_connect (G_OBJECT (child->window), "geometry-changed",
      G_CALLBACK (xfce_tasklist_button_geometry_changed), child);
#endif

  return FALSE;
}



static gboolean
xfce_tasklist_button_button_press_event (GtkWidget         *button,
                                         GdkEventButton    *event,
                                         XfceTasklistChild *child)
{
  GtkWidget     *menu, *panel_plugin;
  WnckWorkspace *workspace;

  panel_return_val_if_fail (XFCE_IS_TASKLIST (child->tasklist), FALSE);
  panel_return_val_if_fail (child->type == XFCE_TASKLIST_BUTTON_TYPE_WINDOW

                            || child->type == XFCE_TASKLIST_BUTTON_TYPE_MENU, FALSE);
  if (event->type != GDK_BUTTON_PRESS
      || xfce_taskbar_is_locked (child->tasklist))
    return FALSE;

  /* send the event to the panel plugin if control is pressed */
  if (PANEL_HAS_FLAG (event->state, GDK_CONTROL_MASK))
    {
      /* send the event to the panel plugin */
      panel_plugin = xfce_tasklist_get_panel_plugin (child->tasklist);
      if (G_LIKELY (panel_plugin != NULL))
        gtk_widget_event (panel_plugin, (GdkEvent *) event);

      return TRUE;
    }

  if (event->button == 1)
    {
      if (wnck_window_is_active (child->window))
        {
          wnck_window_minimize (child->window);
        }
      else
        {
          /* only switch workspaces if we show windows from other
           * workspaces don't switch when switch on minimize is disabled
           * and the window is minimized */
          if (child->tasklist->all_workspaces
              && (!wnck_window_is_minimized (child->window)
                  || child->tasklist->switch_workspace))
            {
              /* get the screen of this window and the workspaces */
              workspace = wnck_window_get_workspace (child->window);

              /* switch to the correct workspace */
              if (workspace != NULL
                  && workspace != wnck_screen_get_active_workspace (child->tasklist->screen))
                wnck_workspace_activate (workspace, event->time - 1);
            }

          wnck_window_activate_transient (child->window, event->time);
        }

      return TRUE;
    }
  else if (event->button == 3)
    {
      menu = wnck_action_menu_new (child->window);
      g_signal_connect (G_OBJECT (menu), "selection-done",
          G_CALLBACK (gtk_widget_destroy), NULL);

      gtk_menu_attach_to_widget (GTK_MENU (menu), button, NULL);
      gtk_menu_popup (GTK_MENU (menu), NULL, NULL,
                      xfce_panel_plugin_position_menu,
                      xfce_tasklist_get_panel_plugin (child->tasklist),
                      event->button,
                      event->time);

      return TRUE;
    }

  return FALSE;
}



static void
xfce_tasklist_button_enter_notify_event_disconnected (gpointer  data,
                                                      GClosure *closure)
{
  XfceTasklistChild *child = data;

  panel_return_if_fail (WNCK_IS_WINDOW (child->window));

  /* we need to detach the geometry watch because that is connected
   * to the window we proxy and thus not disconnected when the
   * proxy dies */
  g_signal_handlers_disconnect_by_func (child->window,
      xfce_tasklist_button_geometry_changed, child);
}



static GtkWidget *
xfce_tasklist_button_proxy_menu_item (XfceTasklistChild *child)
{
  GtkWidget    *mi;
  GtkWidget    *image;
  GtkWidget    *label;
  XfceTasklist *tasklist = child->tasklist;

  panel_return_val_if_fail (XFCE_IS_TASKLIST (child->tasklist), NULL);
  panel_return_val_if_fail (child->type == XFCE_TASKLIST_BUTTON_TYPE_MENU, NULL);
  panel_return_val_if_fail (GTK_IS_LABEL (child->label), NULL);

  mi = gtk_image_menu_item_new ();
  exo_binding_new (G_OBJECT (child->label), "label", G_OBJECT (mi), "label");
  exo_binding_new (G_OBJECT (child->label), "label", G_OBJECT (mi), "tooltip-text");

  label = gtk_bin_get_child (GTK_BIN (mi));
  panel_return_val_if_fail (GTK_IS_LABEL (label), NULL);
  gtk_label_set_max_width_chars (GTK_LABEL (label), tasklist->menu_max_width_chars);
  gtk_label_set_ellipsize (GTK_LABEL (label), tasklist->ellipsize_mode);

  if (G_LIKELY (tasklist->menu_icon_size > 0))
    {
      image = xfce_panel_image_new ();
      gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (mi), image);
      xfce_panel_image_set_size (XFCE_PANEL_IMAGE (image), tasklist->menu_icon_size);
      exo_binding_new (G_OBJECT (child->icon), "pixbuf", G_OBJECT (image), "pixbuf");
      gtk_widget_show (image);
    }

  g_signal_connect_data (G_OBJECT (mi), "enter-notify-event",
      G_CALLBACK (xfce_tasklist_button_enter_notify_event), child,
      xfce_tasklist_button_enter_notify_event_disconnected, 0);
  g_signal_connect (G_OBJECT (mi), "button-press-event",
      G_CALLBACK (xfce_tasklist_button_button_press_event), child);

  /* TODO bold labels for urgent windows */
  /* TODO item dnd */

  return mi;
}



static XfceTasklistChild *
xfce_tasklist_button_new (WnckWindow   *window,
                          XfceTasklist *tasklist)
{
  XfceTasklistChild *child;
  static guint       unique_id_counter = 0;

  panel_return_val_if_fail (XFCE_IS_TASKLIST (tasklist), NULL);
  panel_return_val_if_fail (WNCK_IS_WINDOW (window), NULL);

  /* avoid integer overflows */
  if (G_UNLIKELY (unique_id_counter >= G_MAXUINT))
    unique_id_counter = 0;

  child = g_slice_new0 (XfceTasklistChild);
  child->type = XFCE_TASKLIST_BUTTON_TYPE_WINDOW;
  child->tasklist = tasklist;
  child->window = window;
  child->unique_id = unique_id_counter++;
  child->class_group = wnck_window_get_class_group (window);

  /* create the window button */
  child->button = xfce_arrow_button_new (GTK_ARROW_NONE);
  gtk_widget_set_parent (child->button, GTK_WIDGET (tasklist));
  gtk_button_set_relief (GTK_BUTTON (child->button),
                         child->tasklist->button_relief);

  /* note that the same signals should be in the proxy menu item too */
  g_signal_connect (G_OBJECT (child->button), "enter-notify-event",
      G_CALLBACK (xfce_tasklist_button_enter_notify_event), child);
  g_signal_connect (G_OBJECT (child->button), "button-press-event",
      G_CALLBACK (xfce_tasklist_button_button_press_event), child);

  child->box = xfce_hvbox_new (child->tasklist->horizontal ?
      GTK_ORIENTATION_HORIZONTAL : GTK_ORIENTATION_VERTICAL, FALSE, 6);
  gtk_container_add (GTK_CONTAINER (child->button), child->box);
  gtk_widget_show (child->box);

  child->icon = xfce_panel_image_new ();
  if (child->tasklist->show_labels)
    gtk_box_pack_start (GTK_BOX (child->box), child->icon, FALSE, TRUE, 0);
  else
    gtk_box_pack_start (GTK_BOX (child->box), child->icon, TRUE, TRUE, 0);
  if (child->tasklist->minimized_icon_lucency > 0)
    gtk_widget_show (child->icon);

  child->label = gtk_label_new (NULL);
  gtk_box_pack_start (GTK_BOX (child->box), child->label, TRUE, TRUE, 0);
  if (child->tasklist->horizontal)
    {
      gtk_misc_set_alignment (GTK_MISC (child->label), 0.0, 0.5);
      gtk_label_set_ellipsize (GTK_LABEL (child->label), child->tasklist->ellipsize_mode);
    }
  else
    {
      gtk_label_set_angle (GTK_LABEL (child->label), 270);
      gtk_misc_set_alignment (GTK_MISC (child->label), 0.50, 0.00);
      /* TODO can we already ellipsize here? */
    }

  /* don't show the label if we're in iconbox style */
  if (child->tasklist->show_labels)
    gtk_widget_show (child->label);

  /* monitor window changes */
  g_signal_connect (G_OBJECT (window), "icon-changed",
      G_CALLBACK (xfce_tasklist_button_icon_changed), child);
  g_signal_connect (G_OBJECT (window), "name-changed",
      G_CALLBACK (xfce_tasklist_button_name_changed), child);
  g_signal_connect (G_OBJECT (window), "state-changed",
      G_CALLBACK (xfce_tasklist_button_state_changed), child);
  g_signal_connect (G_OBJECT (window), "workspace-changed",
      G_CALLBACK (xfce_tasklist_button_workspace_changed), child);

  /* poke functions */
  xfce_tasklist_button_icon_changed (window, child);
  xfce_tasklist_button_name_changed (NULL, child);

  /* insert */
  tasklist->windows = g_slist_insert_sorted_with_data (tasklist->windows, child,
                                                        xfce_tasklist_button_compare,
                                                        tasklist);

  return child;
}



/**
 * Group Buttons
 **/
static void
xfce_tasklist_group_button_menu_minimize_all (XfceTasklistChild *child)
{
  GSList            *li;
  XfceTasklistChild *win_child;

  panel_return_if_fail (child->type == XFCE_TASKLIST_BUTTON_TYPE_GROUP);
  panel_return_if_fail (WNCK_IS_CLASS_GROUP (child->class_group));

  for (li = child->windows; li != NULL; li = li->next)
    {
      win_child = li->data;
      panel_return_if_fail (win_child->type == XFCE_TASKLIST_BUTTON_TYPE_WINDOW);
      panel_return_if_fail (WNCK_IS_WINDOW (win_child->window));
      wnck_window_minimize (win_child->window);
    }
}



static void
xfce_tasklist_group_button_menu_unminimize_all (XfceTasklistChild *child)
{
  GSList            *li;
  XfceTasklistChild *win_child;

  panel_return_if_fail (child->type == XFCE_TASKLIST_BUTTON_TYPE_GROUP);
  panel_return_if_fail (WNCK_IS_CLASS_GROUP (child->class_group));

  for (li = child->windows; li != NULL; li = li->next)
    {
      win_child = li->data;
      panel_return_if_fail (win_child->type == XFCE_TASKLIST_BUTTON_TYPE_WINDOW);
      panel_return_if_fail (WNCK_IS_WINDOW (win_child->window));
      wnck_window_unminimize (win_child->window, gtk_get_current_event_time ());
    }
}



static void
xfce_tasklist_group_button_menu_maximize_all (XfceTasklistChild *child)
{
  GSList            *li;
  XfceTasklistChild *win_child;

  panel_return_if_fail (child->type == XFCE_TASKLIST_BUTTON_TYPE_GROUP);
  panel_return_if_fail (WNCK_IS_CLASS_GROUP (child->class_group));

  for (li = child->windows; li != NULL; li = li->next)
    {
      win_child = li->data;
      panel_return_if_fail (win_child->type == XFCE_TASKLIST_BUTTON_TYPE_WINDOW);
      panel_return_if_fail (WNCK_IS_WINDOW (win_child->window));
      wnck_window_maximize (win_child->window);
    }
}



static void
xfce_tasklist_group_button_menu_unmaximize_all (XfceTasklistChild *child)
{
  GSList            *li;
  XfceTasklistChild *win_child;

  panel_return_if_fail (child->type == XFCE_TASKLIST_BUTTON_TYPE_GROUP);
  panel_return_if_fail (WNCK_IS_CLASS_GROUP (child->class_group));

  for (li = child->windows; li != NULL; li = li->next)
    {
      win_child = li->data;
      panel_return_if_fail (win_child->type == XFCE_TASKLIST_BUTTON_TYPE_WINDOW);
      panel_return_if_fail (WNCK_IS_WINDOW (win_child->window));
      wnck_window_unmaximize (win_child->window);
    }
}



static void
xfce_tasklist_group_button_menu_close_all (XfceTasklistChild *child)
{
  GSList            *li;
  XfceTasklistChild *win_child;

  panel_return_if_fail (WNCK_IS_CLASS_GROUP (child->class_group));

  for (li = child->windows; li != NULL; li = li->next)
    {
      win_child = li->data;
      panel_return_if_fail (win_child->type == XFCE_TASKLIST_BUTTON_TYPE_WINDOW);
      panel_return_if_fail (WNCK_IS_WINDOW (win_child->window));
      wnck_window_close (win_child->window, gtk_get_current_event_time ());
    }
}



static GtkWidget *
xfce_tasklist_group_button_menu (XfceTasklistChild *child,
                                 gboolean           action_menu_entries)
{
  GSList            *li;
  XfceTasklistChild *win_child;
  GtkWidget         *mi;
  GtkWidget         *menu;
  GtkWidget         *image;

  panel_return_val_if_fail (XFCE_IS_TASKLIST (child->tasklist), NULL);
  panel_return_val_if_fail (WNCK_IS_CLASS_GROUP (child->class_group), NULL);

  menu = gtk_menu_new ();

  for (li = child->windows; li != NULL; li = li->next)
    {
      win_child = li->data;
      panel_return_val_if_fail (win_child->type == XFCE_TASKLIST_BUTTON_TYPE_WINDOW, NULL);

      mi = xfce_tasklist_button_proxy_menu_item (win_child);
      gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
      gtk_widget_show (mi);

      if (action_menu_entries)
        gtk_menu_item_set_submenu (GTK_MENU_ITEM (mi),
            wnck_action_menu_new (win_child->window));
    }

  if (action_menu_entries)
    {
      mi = gtk_separator_menu_item_new ();
      gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
      gtk_widget_show (mi);

      mi = gtk_image_menu_item_new_with_mnemonic (_("Mi_nimize All"));
      gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
      g_signal_connect_swapped (G_OBJECT (mi), "activate",
          G_CALLBACK (xfce_tasklist_group_button_menu_minimize_all), child);
      gtk_widget_show (mi);
      image = gtk_image_new_from_stock ("wnck-stock-minimize", GTK_ICON_SIZE_MENU);
      gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (mi), image);
      gtk_widget_show (image);

      mi =  gtk_image_menu_item_new_with_mnemonic (_("Un_minimize All"));
      gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
      g_signal_connect_swapped (G_OBJECT (mi), "activate",
          G_CALLBACK (xfce_tasklist_group_button_menu_unminimize_all), child);
      gtk_widget_show (mi);

      mi = gtk_image_menu_item_new_with_mnemonic (_("Ma_ximize All"));
      gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
      g_signal_connect_swapped (G_OBJECT (mi), "activate",
          G_CALLBACK (xfce_tasklist_group_button_menu_maximize_all), child);
      gtk_widget_show (mi);
      image = gtk_image_new_from_stock ("wnck-stock-maximize", GTK_ICON_SIZE_MENU);
      gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (mi), image);
      gtk_widget_show (image);

      mi =  gtk_image_menu_item_new_with_mnemonic (_("_Unmaximize All"));
      gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
      g_signal_connect_swapped (G_OBJECT (mi), "activate",
          G_CALLBACK (xfce_tasklist_group_button_menu_unmaximize_all), child);
      gtk_widget_show (mi);

      mi = gtk_separator_menu_item_new ();
      gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
      gtk_widget_show (mi);

      mi = gtk_image_menu_item_new_with_mnemonic(_("_Close All"));
      gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
      g_signal_connect_swapped (G_OBJECT (mi), "activate",
          G_CALLBACK (xfce_tasklist_group_button_menu_close_all), child);
      gtk_widget_show (mi);

      image = gtk_image_new_from_stock ("wnck-stock-delete", GTK_ICON_SIZE_MENU);
      gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (mi), image);
      gtk_widget_show (image);
    }

  return menu;
}



static void
xfce_tasklist_group_button_menu_destroy (GtkWidget         *menu,
                                         XfceTasklistChild *child)
{
  panel_return_if_fail (XFCE_IS_TASKLIST (child->tasklist));
  panel_return_if_fail (GTK_IS_TOGGLE_BUTTON (child->button));
  panel_return_if_fail (GTK_IS_WIDGET (menu));

  gtk_widget_destroy (menu);

  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (child->button), FALSE);

#ifdef GDK_WINDOWING_X11
  /* make sure the wireframe is hidden */
  xfce_tasklist_wireframe_hide (child->tasklist);
#endif
}



static gboolean
xfce_tasklist_group_button_button_press_event (GtkWidget         *button,
                                               GdkEventButton    *event,
                                               XfceTasklistChild *child)
{
  GtkWidget *panel_plugin;
  GtkWidget *menu;

  panel_return_val_if_fail (XFCE_IS_TASKLIST (child->tasklist), FALSE);
  panel_return_val_if_fail (child->type == XFCE_TASKLIST_BUTTON_TYPE_GROUP, FALSE);

  if (event->type != GDK_BUTTON_PRESS
      || xfce_taskbar_is_locked (child->tasklist))
    return FALSE;

  /* send the event to the panel plugin if control is pressed */
  if (PANEL_HAS_FLAG (event->state, GDK_CONTROL_MASK))
    {
      /* send the event to the panel plugin */
      panel_plugin = xfce_tasklist_get_panel_plugin (child->tasklist);
      if (G_LIKELY (panel_plugin != NULL))
        gtk_widget_event (panel_plugin, (GdkEvent *) event);

      return TRUE;
    }

  if (event->button == 1 || event->button == 3)
    {
      menu = xfce_tasklist_group_button_menu (child, event->button == 3);
      g_signal_connect (G_OBJECT (menu), "selection-done",
          G_CALLBACK (xfce_tasklist_group_button_menu_destroy), child);

      gtk_menu_attach_to_widget (GTK_MENU (menu), button, NULL);
      gtk_menu_popup (GTK_MENU (menu), NULL, NULL,
                      xfce_panel_plugin_position_menu,
                      xfce_tasklist_get_panel_plugin (child->tasklist),
                      event->button,
                      event->time);

      return TRUE;
    }

  return FALSE;
}



static void
xfce_tasklist_group_button_name_changed (WnckClassGroup    *class_group,
                                         XfceTasklistChild *child)
{
  const gchar *name;
  gchar       *label;

  panel_return_if_fail (class_group == NULL || child->class_group == class_group);
  panel_return_if_fail (XFCE_IS_TASKLIST (child->tasklist));
  panel_return_if_fail (WNCK_IS_CLASS_GROUP (child->class_group));

  /* create the button label */
  name = wnck_class_group_get_name (child->class_group);
  if (!exo_str_is_empty (name))
    label = g_strdup_printf ("%s (%d)", name, 0);
  else
    label = g_strdup_printf ("(%d)", 0);

  gtk_label_set_text (GTK_LABEL (child->label), label);

  g_free (label);

  /* don't sort if there is no need to update the sorting (ie. only number
   * of windows is changed or button is not inserted in the tasklist yet */
  if (class_group != NULL)
    xfce_tasklist_sort (child->tasklist);
}



static void
xfce_tasklist_group_button_icon_changed (WnckClassGroup    *class_group,
                                         XfceTasklistChild *child)
{
  GdkPixbuf *pixbuf;

  panel_return_if_fail (XFCE_IS_TASKLIST (child->tasklist));
  panel_return_if_fail (WNCK_IS_CLASS_GROUP (class_group));
  panel_return_if_fail (child->class_group == class_group);
  panel_return_if_fail (XFCE_IS_PANEL_IMAGE (child->icon));

  /* 0 means icons are disabled, athough the grouping button does
   * not use lucient icons */
  if (child->tasklist->minimized_icon_lucency == 0)
    return;

  /* get the class group icon */
  if (child->tasklist->show_labels)
    pixbuf = wnck_class_group_get_mini_icon (class_group);
  else
    pixbuf = wnck_class_group_get_icon (class_group);

  if (G_LIKELY (pixbuf != NULL))
    xfce_panel_image_set_from_pixbuf (XFCE_PANEL_IMAGE (child->icon), pixbuf);
  else
    xfce_panel_image_clear (XFCE_PANEL_IMAGE (child->icon));
}



static void
xfce_tasklist_group_button_remove (XfceTasklist   *tasklist,
                                   WnckClassGroup *class_group)
{
  XfceTasklistChild *child;
  GSList            *li;
  guint              n;

  panel_return_if_fail (XFCE_IS_TASKLIST (tasklist));
  panel_return_if_fail (WNCK_IS_CLASS_GROUP (class_group));

  for (li = tasklist->windows; li != NULL; li = li->next)
    {
      child = li->data;

      if (child->type == XFCE_TASKLIST_BUTTON_TYPE_GROUP
          && child->class_group == class_group)
        {
          /* disconnect from all the group watch functions */
          n = g_signal_handlers_disconnect_matched (G_OBJECT (class_group),
              G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, child);
          panel_return_if_fail (n == 2);

          /* destroy the button, this will free the child data in the
           * container remove function */
          gtk_widget_destroy (child->button);

          break;
        }
    }

  return;
  /* TODO remove */
  xfce_tasklist_group_button_new(class_group, tasklist);
}



static XfceTasklistChild *
xfce_tasklist_group_button_new (WnckClassGroup *class_group,
                                XfceTasklist   *tasklist)
{
  XfceTasklistChild *child;

  panel_return_val_if_fail (XFCE_IS_TASKLIST (tasklist), NULL);
  panel_return_val_if_fail (WNCK_IS_CLASS_GROUP (class_group), NULL);

  child = g_slice_new0 (XfceTasklistChild);
  child->type = XFCE_TASKLIST_BUTTON_TYPE_GROUP;
  child->tasklist = tasklist;
  child->class_group = class_group;

  /* create the window button */
  child->button = xfce_arrow_button_new (GTK_ARROW_UP);
  gtk_widget_set_parent (child->button, GTK_WIDGET (tasklist));
  gtk_button_set_relief (GTK_BUTTON (child->button),
                         child->tasklist->button_relief);

  /* note that the same signals should be in the proxy menu item too */
  g_signal_connect (G_OBJECT (child->button), "button-press-event",
      G_CALLBACK (xfce_tasklist_group_button_button_press_event), child);

  child->box = xfce_hvbox_new (child->tasklist->horizontal ?
      GTK_ORIENTATION_HORIZONTAL : GTK_ORIENTATION_VERTICAL, FALSE, 6);
  gtk_container_add (GTK_CONTAINER (child->button), child->box);
  gtk_widget_show (child->box);

  child->icon = xfce_panel_image_new ();
  if (child->tasklist->show_labels)
    gtk_box_pack_start (GTK_BOX (child->box), child->icon, FALSE, TRUE, 0);
  else
    gtk_box_pack_start (GTK_BOX (child->box), child->icon, TRUE, TRUE, 0);
  if (child->tasklist->minimized_icon_lucency > 0)
    gtk_widget_show (child->icon);

  child->label = gtk_label_new (NULL);
  gtk_box_pack_start (GTK_BOX (child->box), child->label, TRUE, TRUE, 0);
  if (child->tasklist->horizontal)
    {
      gtk_misc_set_alignment (GTK_MISC (child->label), 0.0, 0.5);
      gtk_label_set_ellipsize (GTK_LABEL (child->label), child->tasklist->ellipsize_mode);
    }
  else
    {
      gtk_label_set_angle (GTK_LABEL (child->label), 270);
      gtk_misc_set_alignment (GTK_MISC (child->label), 0.50, 0.00);
      /* TODO can we already ellipsize here? */
    }

  /* don't show the label if we're in iconbox style */
  if (child->tasklist->show_labels)
    gtk_widget_show (child->label);

  /* monitor class group changes */
  g_signal_connect (G_OBJECT (class_group), "icon-changed",
      G_CALLBACK (xfce_tasklist_group_button_icon_changed), child);
  g_signal_connect (G_OBJECT (class_group), "name-changed",
      G_CALLBACK (xfce_tasklist_group_button_name_changed), child);

  /* poke functions */
  xfce_tasklist_group_button_icon_changed (class_group, child);
  xfce_tasklist_group_button_name_changed (NULL, child);

  /* insert */
  tasklist->windows = g_slist_insert_sorted_with_data (tasklist->windows, child,
                                                        xfce_tasklist_button_compare,
                                                        tasklist);

  return child;
  /* TODO remove */
  xfce_tasklist_group_button_remove(tasklist,class_group);
}



/**
 * Potential Public Functions
 **/
static void
xfce_tasklist_set_include_all_workspaces (XfceTasklist *tasklist,
                                          gboolean      all_workspaces)
{
  GSList            *li;
  XfceTasklistChild *child;

  panel_return_if_fail (XFCE_IS_TASKLIST (tasklist));

  all_workspaces = !!all_workspaces;
  if (tasklist->all_workspaces != all_workspaces)
    {
      tasklist->all_workspaces = all_workspaces;

      if (all_workspaces)
        {
          /* make sure all buttons are visible */
          for (li = tasklist->windows; li != NULL; li = li->next)
            {
              child = li->data;
              gtk_widget_show (child->button);
            }
        }
      else
        {
          /* trigger signal */
          xfce_tasklist_active_workspace_changed (tasklist->screen,
                                                  NULL, tasklist);
        }
    }
}



static void
xfce_tasklist_set_button_relief (XfceTasklist   *tasklist,
                                 GtkReliefStyle  button_relief)
{
  GSList            *li;
  XfceTasklistChild *child;

  panel_return_if_fail (XFCE_IS_TASKLIST (tasklist));

  if (tasklist->button_relief != button_relief)
    {
      tasklist->button_relief = button_relief;

      /* change the relief of all buttons in the list */
      for (li = tasklist->windows; li != NULL; li = li->next)
        {
          child = li->data;
          gtk_button_set_relief (GTK_BUTTON (child->button),
                                 button_relief);
        }

      /* arrow button for overflow menu */
      gtk_button_set_relief (GTK_BUTTON (tasklist->arrow_button),
                             button_relief);
    }
}



static void
xfce_tasklist_set_show_labels (XfceTasklist *tasklist,
                               gboolean      show_labels)
{
  GSList            *li;
  XfceTasklistChild *child;

  panel_return_if_fail (XFCE_IS_TASKLIST (tasklist));

  if (tasklist->show_labels != show_labels)
    {
      tasklist->show_labels = show_labels;

      /* change the mode of all the buttons */
      for (li = tasklist->windows; li != NULL; li = li->next)
        {
          child = li->data;

          /* show or hide the label */
          if (show_labels)
            {
              gtk_widget_show (child->label);
              gtk_box_set_child_packing (GTK_BOX (child->box),
                                         child->icon,
                                         FALSE, FALSE, 0,
                                         GTK_PACK_START);
            }
          else
            {
              gtk_widget_hide (child->label);
              gtk_box_set_child_packing (GTK_BOX (child->box),
                                         child->icon,
                                         TRUE, TRUE, 0,
                                         GTK_PACK_START);
            }

          /* update the icon (we use another size for
           * icon box mode) */
          xfce_tasklist_button_icon_changed (child->window, child);
        }
    }
}



static void
xfce_tasklist_set_show_only_minimized (XfceTasklist *tasklist,
                                       gboolean      only_minimized)
{
  GSList            *li;
  XfceTasklistChild *child;

  panel_return_if_fail (XFCE_IS_TASKLIST (tasklist));

  if (tasklist->only_minimized != only_minimized)
    {
      tasklist->only_minimized = !!only_minimized;

      /* update the tasklist */
      for (li = tasklist->windows; li != NULL; li = li->next)
        {
          child = li->data;

          /* update the icons of the minimized windows */
          if (wnck_window_is_minimized (child->window))
            {
              xfce_tasklist_button_icon_changed (child->window, child);
              xfce_tasklist_button_name_changed (child->window, child);
            }

          /* if we show all workspaces, update the visibility here */
          if (tasklist->all_workspaces
              && !wnck_window_is_minimized (child->window))
            {
              if (only_minimized)
                gtk_widget_hide (child->button);
              else
                gtk_widget_show (child->button);
            }
        }

      /* update the buttons when we show only the active workspace */
      if (!tasklist->all_workspaces)
        xfce_tasklist_active_workspace_changed (tasklist->screen,
                                                NULL, tasklist);
    }
}



static void
xfce_tasklist_set_show_wireframes (XfceTasklist *tasklist,
                                   gboolean      show_wireframes)
{
  panel_return_if_fail (XFCE_IS_TASKLIST (tasklist));

  tasklist->show_wireframes = !!show_wireframes;

#ifdef GDK_WINDOWING_X11
  /* destroy the window if needed */
  xfce_tasklist_wireframe_destroy (tasklist);
#endif
}



void
xfce_tasklist_set_orientation (XfceTasklist   *tasklist,
                               GtkOrientation  orientation)
{
  gboolean           horizontal;
  GSList            *li;
  XfceTasklistChild *child;

  panel_return_if_fail (XFCE_IS_TASKLIST (tasklist));

  horizontal = !!(orientation == GTK_ORIENTATION_HORIZONTAL);

  if (tasklist->horizontal != horizontal)
    {
      tasklist->horizontal = horizontal;

      /* update the tasklist */
      for (li = tasklist->windows; li != NULL; li = li->next)
        {
          child = li->data;

          /* update task box */
          xfce_hvbox_set_orientation (XFCE_HVBOX (child->box), orientation);

          /* update the label */
          if (horizontal)
            {
              gtk_misc_set_alignment (GTK_MISC (child->label), 0.0, 0.5);
              gtk_label_set_angle (GTK_LABEL (child->label), 0);
              gtk_label_set_ellipsize (GTK_LABEL (child->label),
                                       child->tasklist->ellipsize_mode);
            }
          else
            {
              gtk_misc_set_alignment (GTK_MISC (child->label), 0.50, 0.00);
              gtk_label_set_angle (GTK_LABEL (child->label), 270);
              gtk_label_set_ellipsize (GTK_LABEL (child->label), PANGO_ELLIPSIZE_NONE);
            }
        }

      gtk_widget_queue_resize (GTK_WIDGET (tasklist));
    }
}



void
xfce_tasklist_set_size (XfceTasklist *tasklist,
                        gint          size)
{
  panel_return_if_fail (XFCE_IS_TASKLIST (tasklist));

  if (tasklist->size != size)
    {
      tasklist->size = size;
      gtk_widget_queue_resize (GTK_WIDGET (tasklist));
    }
}
