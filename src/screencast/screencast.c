/*
 * Copyright © 2026
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 */


#include <math.h>

#include <gio/gio.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <json-glib/json-glib.h>

#include "xdg-desktop-portal-dbus.h"
#include "common-dbus.h"

#include "screencast.h"
#include "screencastdialog.h"
#include "muffinscreencast.h"
#include "request.h"
#include "session.h"
#include "utils.h"

#define CINNAMON_SCREEN_CAST_SCRIPT \
  "global.get_window_actors().map(a => a.meta_window)." \
  "filter(w => w && !w.is_skip_taskbar() && " \
  "w.get_window_type() !== 1 && w.get_window_type() !== 2)." \
  "map(w => [String(w.get_id()), w.get_title() || '', w.get_wm_class() || ''])"

#define ITEMS_PRESENT_AFTER 4

typedef enum _ScreenCastDisplayRotation
{
  SCREEN_CAST_DISPLAY_ROTATION_NONE,
  SCREEN_CAST_DISPLAY_ROTATION_90,
  SCREEN_CAST_DISPLAY_ROTATION_180,
  SCREEN_CAST_DISPLAY_ROTATION_270,
  SCREEN_CAST_DISPLAY_ROTATION_FLIPPED,
  SCREEN_CAST_DISPLAY_ROTATION_90_FLIPPED,
  SCREEN_CAST_DISPLAY_ROTATION_180_FLIPPED,
  SCREEN_CAST_DISPLAY_ROTATION_270_FLIPPED,
} ScreenCastDisplayRotation;

typedef enum _ScreenCastLayoutMode
{
  SCREEN_CAST_LAYOUT_MODE_LOGICAL = 1,
  SCREEN_CAST_LAYOUT_MODE_PHYSICAL = 2,
  SCREEN_CAST_LAYOUT_MODE_LOGICAL_WITH_UI_SCALE = 3,
} ScreenCastLayoutMode;

static MuffinScreenCast *muffin_screen_cast;
static OrgCinnamon *cinnamon;

static GVariant *
new_empty_results (void)
{
  return g_variant_new_array (G_VARIANT_TYPE ("{sv}"), NULL, 0);
}

static void
picker_item_free (gpointer data)
{
  PickerItem *item = data;

  g_free (item->title);
  g_free (item->subtitle);
  g_free (item->connector);
  g_free (item);
}

static PickerItem *
picker_item_new (ScreenCastSourceType type,
                 guint32 id)
{
  PickerItem *item;

  item = g_new0 (PickerItem, 1);
  item->type = type;
  item->id = id;

  return item;
}

static void
monitor_free (gpointer data)
{
  MuffinMonitor *monitor = data;

  g_free (monitor->connector);
  g_free (monitor->label);
  g_free (monitor);
}

static gboolean
monitor_is_rotated (ScreenCastDisplayRotation rotation)
{
  return rotation == SCREEN_CAST_DISPLAY_ROTATION_90 ||
         rotation == SCREEN_CAST_DISPLAY_ROTATION_270 ||
         rotation == SCREEN_CAST_DISPLAY_ROTATION_90_FLIPPED ||
         rotation == SCREEN_CAST_DISPLAY_ROTATION_270_FLIPPED;
}

static MuffinMonitor *
muffin_monitor_new (const char *connector,
                    const char *label,
                    gboolean primary,
                    const ScreenCastRect *rect,
                    double scale,
                    ScreenCastDisplayRotation rotation,
                    ScreenCastLayoutMode layout_mode,
                    int ui_scaling_factor)
{
  MuffinMonitor *monitor;

  monitor = g_new0 (MuffinMonitor, 1);
  monitor->connector = g_strdup (connector);
  monitor->label = g_strdup (label);
  monitor->primary = primary;
  monitor->rect = *rect;

  if (layout_mode == SCREEN_CAST_LAYOUT_MODE_LOGICAL_WITH_UI_SCALE)
    {
      monitor->rect.width = round (rect->width / (scale * ui_scaling_factor));
      monitor->rect.height = round (rect->height / (scale * ui_scaling_factor));
    }
  else if (layout_mode == SCREEN_CAST_LAYOUT_MODE_LOGICAL)
    {
      monitor->rect.width = round (rect->width / scale);
      monitor->rect.height = round (rect->height / scale);
    }

  if (monitor_is_rotated (rotation))
    {
      int tmp = monitor->rect.width;

      monitor->rect.width = monitor->rect.height;
      monitor->rect.height = tmp;
    }

  return monitor;
}

static void
read_current_width_and_height (GVariantIter *modes,
                               int *width,
                               int *height)
{
  while (TRUE)
    {
      g_autoptr(GVariant) variant = NULL;
      g_autoptr(GVariant) properties_variant = NULL;
      gboolean is_current;

      if (!g_variant_iter_next (modes, "@(siiddada{sv})", &variant))
        break;

      g_variant_get (variant, "(siiddad@a{sv})",
                     NULL, /* mode_id */
                     width,
                     height,
                     NULL, /* refresh_rate */
                     NULL, /* preferred_scale */
                     NULL, /* supported_scales */
                     &properties_variant);
      if (!g_variant_lookup (properties_variant, "is-current", "b", &is_current))
        is_current = FALSE;

      if (is_current)
        return;
    }

  *width = 0;
  *height = 0;
}

static GPtrArray *
get_monitors (GDBusConnection *connection)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) monitors = NULL;
  g_autoptr(GVariant) logical_monitors = NULL;
  g_autoptr(GVariant) properties = NULL;
  g_autoptr(OrgCinnamonMuffinDisplayConfig) proxy = NULL;
  g_autoptr(GPtrArray) result = NULL;
  GVariantIter monitors_iter;
  GVariantIter logical_iter;
  GHashTable *monitor_map;
  ScreenCastLayoutMode layout_mode = SCREEN_CAST_LAYOUT_MODE_LOGICAL;
  int ui_scaling_factor = 1;
  guint serial;

  proxy = org_cinnamon_muffin_display_config_proxy_new_sync (connection,
                                                             G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                                             "org.cinnamon.Muffin.DisplayConfig",
                                                             "/org/cinnamon/Muffin/DisplayConfig",
                                                             NULL,
                                                             &error);
  if (!proxy)
    {
      g_warning ("Failed to acquire org.cinnamon.Muffin.DisplayConfig proxy: %s",
                 error->message);
      return NULL;
    }

  if (!org_cinnamon_muffin_display_config_call_get_current_state_sync (proxy,
                                                                       &serial,
                                                                       &monitors,
                                                                       &logical_monitors,
                                                                       &properties,
                                                                       NULL,
                                                                       &error))
    {
      g_warning ("Failed to get display state: %s", error->message);
      return NULL;
    }

  g_variant_lookup (properties, "layout-mode", "u", &layout_mode);

  if (!g_variant_lookup (properties, "legacy-ui-scaling-factor",
                         "i", &ui_scaling_factor) ||
      ui_scaling_factor < 1)
    ui_scaling_factor = 1;

  monitor_map = g_hash_table_new_full (g_str_hash, g_str_equal,
                                       g_free, monitor_free);

  g_variant_iter_init (&monitors_iter, monitors);
  while (TRUE)
    {
      g_autoptr(GVariant) monitor_variant = NULL;
      g_autofree char *connector = NULL;
      g_autofree char *vendor = NULL;
      g_autofree char *product = NULL;
      g_autofree char *serial_s = NULL;
      g_autoptr(GVariantIter) modes = NULL;
      g_autoptr(GVariant) monitor_properties = NULL;
      g_autofree char *display_name = NULL;
      MuffinMonitor *monitor;

      if (!g_variant_iter_next (&monitors_iter, "@((ssss)a(siiddada{sv})a{sv})",
                                &monitor_variant))
        break;

      g_variant_get (monitor_variant, "((ssss)a(siiddada{sv})@a{sv})",
                     &connector,
                     &vendor,
                     &product,
                     &serial_s,
                     &modes,
                     &monitor_properties);
      if (!g_variant_lookup (monitor_properties, "display-name", "s", &display_name))
        display_name = g_strdup (connector);

      monitor = g_new0 (MuffinMonitor, 1);
      monitor->connector = g_strdup (connector);
      monitor->label = g_strdup (display_name);
      read_current_width_and_height (modes,
                                     &monitor->rect.width,
                                     &monitor->rect.height);

      g_hash_table_insert (monitor_map, g_strdup (connector), monitor);
    }

  result = g_ptr_array_new_with_free_func (monitor_free);

  g_variant_iter_init (&logical_iter, logical_monitors);
  while (TRUE)
    {
      g_autoptr(GVariant) logical_variant = NULL;
      g_autoptr(GVariantIter) monitors_iter_inner = NULL;
      g_autoptr(GString) label_builder = NULL;
      MuffinMonitor *first_monitor = NULL;
      ScreenCastRect rect;
      int x, y;
      double scale;
      guint rotation;
      gboolean is_primary;

      if (!g_variant_iter_next (&logical_iter, "@(iiduba(ssss)a{sv})",
                                &logical_variant))
        break;

      g_variant_get (logical_variant, "(iiduba(ssss)a{sv})",
                     &x, &y, &scale, &rotation, &is_primary,
                     &monitors_iter_inner, NULL);

      label_builder = g_string_new (NULL);
      while (TRUE)
        {
          g_autoptr(GVariant) monitor_variant = NULL;
          g_autofree char *connector = NULL;
          MuffinMonitor *monitor;

          if (!g_variant_iter_next (monitors_iter_inner, "@(ssss)",
                                    &monitor_variant))
            break;

          g_variant_get (monitor_variant, "(ssss)",
                         &connector, NULL, NULL, NULL);

          monitor = g_hash_table_lookup (monitor_map, connector);
          if (!monitor)
            continue;

          if (!first_monitor)
            first_monitor = monitor;

          if (label_builder->len > 0)
            g_string_append_c (label_builder, ' ');
          g_string_append (label_builder, monitor->label);
        }

      if (!first_monitor)
        continue;

      rect.x = x;
      rect.y = y;
      rect.width = first_monitor->rect.width;
      rect.height = first_monitor->rect.height;

      g_ptr_array_add (result,
                       muffin_monitor_new (first_monitor->connector,
                                           label_builder->str,
                                           is_primary,
                                           &rect,
                                           scale,
                                           (ScreenCastDisplayRotation) rotation,
                                           layout_mode,
                                           ui_scaling_factor));
    }

  g_hash_table_destroy (monitor_map);

  return g_steal_pointer (&result);
}

static GPtrArray *
get_windows (GDBusConnection *connection)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(JsonParser) parser = NULL;
  JsonNode *root;
  JsonArray *array;
  g_autoptr(GPtrArray) result = NULL;
  gboolean success;
  g_autofree char *response = NULL;

  if (!cinnamon)
    return NULL;

  if (!org_cinnamon_call_eval_sync (cinnamon,
                                    CINNAMON_SCREEN_CAST_SCRIPT,
                                    &success,
                                    &response,
                                    NULL,
                                    &error))
    {
      g_warning ("Failed to query windows: %s", error->message);
      return NULL;
    }

  if (!success)
    {
      g_warning ("Cinnamon Eval failed to list windows");
      return NULL;
    }

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, response, -1, &error))
    {
      g_warning ("Failed to parse window list: %s", error->message);
      return NULL;
    }

  root = json_parser_get_root (parser);
  if (!root || !JSON_NODE_HOLDS_ARRAY (root))
    return NULL;

  array = json_node_get_array (root);
  result = g_ptr_array_new_with_free_func (NULL);

  for (guint i = 0; i < json_array_get_length (array); i++)
    {
      JsonNode *element = json_array_get_element (array, i);
      JsonArray *entry;
      PickerItem *item;

      if (!element || !JSON_NODE_HOLDS_ARRAY (element))
        continue;

      entry = json_node_get_array (element);
      if (json_array_get_length (entry) < 3)
        continue;

      item = picker_item_new (SCREEN_CAST_SOURCE_TYPE_WINDOW, i);
      item->window_id = (guint64) g_ascii_strtoull (json_array_get_string_element (entry, 0),
                                                    NULL, 10);
      item->title = g_strdup (json_array_get_string_element (entry, 1));
      item->subtitle = g_strdup (json_array_get_string_element (entry, 2));

      g_ptr_array_add (result, item);
    }

  return g_steal_pointer (&result);
}

typedef struct _ScreenCastSession
{
  Session parent;

  ScreenCastSelection selection;
  MuffinScreenCastSession *muffin_session;
  gboolean started;
  gboolean start_pending;
  gboolean remote;
  gboolean sources_selected;
  gboolean unlocked;
  guint32 devices;
  GDBusProxy *remote_proxy;
  char *remote_id;
  GDBusProxy *locker;
  GtkWidget *indicator;
  GtkWidget *consent;
  char *owner;
  char *app_id;
  guint owner_watch;
  GHashTable *pressed_keys;
  GHashTable *pressed_keysyms;
  GHashTable *pressed_buttons;
  gpointer pending;
} ScreenCastSession;

typedef struct _ScreenCastSessionClass
{
  SessionClass parent_class;
} ScreenCastSessionClass;

static GType screencast_session_get_type (void);
G_DEFINE_TYPE (ScreenCastSession, screencast_session, session_get_type ())

static gboolean close_session_idle (gpointer user_data);
static void cancel_pending_start (ScreenCastSession *session);
static gboolean session_is_unlocked (ScreenCastSession *session);
static void show_sharing_indicator (ScreenCastSession *session);
static gboolean remote_desktop_init (GDBusConnection *bus, GError **error);
static gboolean setup_session (ScreenCastSession *session, GDBusMethodInvocation *invocation,
                                const char *app_id, GError **error);
static gboolean session_owned (ScreenCastSession *session, GDBusMethodInvocation *invocation,
                               const char *app_id);
static void release_remote_input (ScreenCastSession *session);

static gboolean
start_remote_session (ScreenCastSession *session, GError **error)
{
  g_autoptr(GVariant) reply = NULL;
  reply = g_dbus_proxy_call_sync (session->remote_proxy, "Start", NULL,
                                  G_DBUS_CALL_FLAGS_NONE, 5000, NULL, error);
  return reply != NULL;
}

static void
terminate_sharing (ScreenCastSession *session)
{
  if (session->parent.closed)
    return;
  xdp_impl_session_emit_closed (XDP_IMPL_SESSION (session));
  session_close ((Session *) session);
}

static void
screencast_session_close (Session *session)
{
  ScreenCastSession *screencast_session = (ScreenCastSession *)session;
  GError *error = NULL;

  g_debug ("Closing screencast session");

  cancel_pending_start (screencast_session);
  if (screencast_session->consent)
    gtk_dialog_response (GTK_DIALOG (screencast_session->consent), GTK_RESPONSE_CANCEL);
  if (screencast_session->indicator)
    {
      gtk_widget_destroy (screencast_session->indicator);
      screencast_session->indicator = NULL;
    }
  if (screencast_session->remote_proxy)
    {
      release_remote_input (screencast_session);
      g_autoptr(GVariant) reply = NULL;
      reply = g_dbus_proxy_call_sync (screencast_session->remote_proxy, "Stop",
                                      NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
      g_clear_object (&screencast_session->remote_proxy);
    }

  if (screencast_session->muffin_session)
    {
      muffin_screen_cast_session_stop (screencast_session->muffin_session,
                                       &error);
      if (error)
        {
          g_warning ("Failed to stop muffin screencast session: %s",
                     error->message);
          g_clear_error (&error);
        }

      g_clear_object (&screencast_session->muffin_session);
    }
}

static void
screencast_session_finalize (GObject *object)
{
  ScreenCastSession *screencast_session = (ScreenCastSession *)object;

  g_clear_object (&screencast_session->muffin_session);
  if (screencast_session->owner_watch)
    g_bus_unwatch_name (screencast_session->owner_watch);
  g_clear_object (&screencast_session->locker);
  g_free (screencast_session->owner);
  g_free (screencast_session->app_id);
  g_free (screencast_session->remote_id);
  g_clear_pointer (&screencast_session->pressed_keys, g_hash_table_unref);
  g_clear_pointer (&screencast_session->pressed_keysyms, g_hash_table_unref);
  g_clear_pointer (&screencast_session->pressed_buttons, g_hash_table_unref);

  G_OBJECT_CLASS (screencast_session_parent_class)->finalize (object);
}

static void
screencast_session_init (ScreenCastSession *screencast_session)
{
  screencast_session->selection.source_types =
    SCREEN_CAST_SOURCE_TYPE_MONITOR | SCREEN_CAST_SOURCE_TYPE_WINDOW;
  screencast_session->selection.cursor_mode = SCREEN_CAST_CURSOR_MODE_HIDDEN;
}

static void
screencast_session_class_init (ScreenCastSessionClass *klass)
{
  GObjectClass *gobject_class;
  SessionClass *session_class;

  gobject_class = (GObjectClass *)klass;
  gobject_class->finalize = screencast_session_finalize;

  session_class = (SessionClass *)klass;
  session_class->close = screencast_session_close;
}

static ScreenCastSession *
screencast_session_new (const char *session_handle)
{
  ScreenCastSession *screencast_session;

  g_debug ("Creating screencast session %s", session_handle);

  screencast_session = g_object_new (screencast_session_get_type (),
                                     "id", session_handle,
                                     NULL);

  return screencast_session;
}

typedef struct _StartOperation
{
  ScreenCastSession *session;
  XdpImplScreenCast *impl;
  GDBusMethodInvocation *invocation;
  Request *request;
  ScreenCastDialog *dialog;
  GPtrArray *items;
  MuffinScreenCastSession *muffin_session;
  char *preview_dir;
  guint preview_idle_id;
  guint preview_index;
  guint defer_id;
  GArray *selected_indices;
  int busy_count;
  gboolean presented;
  gboolean completed;
  gboolean finished_pending;
} StartOperation;

static void
start_operation_free (StartOperation *op)
{
  if (op->preview_idle_id)
    {
      g_source_remove (op->preview_idle_id);
      op->preview_idle_id = 0;
    }

  if (op->defer_id)
    {
      g_source_remove (op->defer_id);
      op->defer_id = 0;
    }

  if (op->preview_dir)
    {
      g_rmdir (op->preview_dir);
      g_free (op->preview_dir);
    }

  if (op->muffin_session)
    g_signal_handlers_disconnect_by_data (op->muffin_session, op);

  g_clear_object (&op->muffin_session);
  g_clear_object (&op->request);

  if (op->dialog)
    {
      screencast_dialog_destroy (op->dialog);
      op->dialog = NULL;
    }

  if (op->selected_indices)
    {
      g_array_free (op->selected_indices, TRUE);
      op->selected_indices = NULL;
    }

  g_clear_pointer (&op->items, g_ptr_array_unref);
  g_clear_object (&op->session);

  g_free (op);
}

static void
start_operation_complete (StartOperation *op,
                          guint response,
                          GVariant *results)
{
  if (op->completed)
    {
      if (results)
        g_variant_unref (results);
      return;
    }

  op->completed = TRUE;
  op->session->pending = NULL;
  op->session->start_pending = FALSE;
  if (response != 0 && !op->session->parent.closed)
    {
      xdp_impl_session_emit_closed (XDP_IMPL_SESSION (op->session));
      g_idle_add (close_session_idle, g_object_ref (op->session));
    }

  if (op->request->exported)
    request_unexport (op->request);

  if (!results)
    results = new_empty_results ();

  g_dbus_method_invocation_return_value (op->invocation,
                                         g_variant_new ("(u@a{sv})", response, results));

  if (op->busy_count > 0)
    {
      op->finished_pending = TRUE;
      return;
    }

  start_operation_free (op);
}

static void
cancel_pending_start (ScreenCastSession *session)
{
  StartOperation *op = session->pending;
  if (op)
    start_operation_complete (op, 1, NULL);
}

static void
start_operation_busy_enter (StartOperation *op)
{
  op->busy_count++;
}

static gboolean
start_operation_busy_leave (StartOperation *op)
{
  op->busy_count--;

  if (op->busy_count > 0 || !op->finished_pending)
    return TRUE;

  start_operation_free (op);

  return FALSE;
}

static void
start_operation_fail (StartOperation *op,
                      const char *message)
{
  g_warning ("Screencast start failed: %s", message);
  start_operation_complete (op, 2, NULL);
}

static void
start_operation_cancel (StartOperation *op)
{
  start_operation_complete (op, 1, NULL);
}

static gboolean
close_session_idle (gpointer user_data)
{
  Session *session = user_data;

  if (!session->closed)
    session_close (session);

  g_object_unref (session);

  return G_SOURCE_REMOVE;
}

static void
on_muffin_session_closed (MuffinScreenCastSession *muffin_session,
                          gpointer user_data)
{
  ScreenCastSession *session = user_data;

  g_debug ("Muffin screen cast session closed");

  xdp_impl_session_emit_closed (XDP_IMPL_SESSION (session));
  g_idle_add (close_session_idle, g_object_ref (session));
}

static void
on_muffin_session_closed_during_start (MuffinScreenCastSession *muffin_session,
                                       gpointer user_data)
{
  StartOperation *op = user_data;

  if (op->completed)
    return;

  start_operation_fail (op, "Muffin screen cast session closed");
}

static void
on_muffin_session_ready (MuffinScreenCastSession *muffin_session,
                         gpointer user_data)
{
  StartOperation *op = user_data;
  GVariantBuilder streams_builder;
  GVariantBuilder results_builder;
  GVariant *results;

  if (op->completed)
    return;
  if (!session_is_unlocked (op->session))
    {
      start_operation_fail (op, "The desktop is locked");
      return;
    }

  g_variant_builder_init (&streams_builder, G_VARIANT_TYPE ("a(ua{sv})"));
  muffin_screen_cast_session_add_stream_properties (muffin_session,
                                                    &streams_builder);

  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&results_builder, "{sv}",
                         "streams",
                         g_variant_builder_end (&streams_builder));
  if (op->session->remote)
    g_variant_builder_add (&results_builder, "{sv}", "devices",
                           g_variant_new_uint32 (op->session->devices));
  results = g_variant_builder_end (&results_builder);

  op->session->started = TRUE;
  show_sharing_indicator (op->session);

  g_signal_connect (muffin_session, "closed",
                    G_CALLBACK (on_muffin_session_closed), op->session);

  start_operation_complete (op, 0, results);
}

static gboolean
start_muffin_session (StartOperation *op,
                      GArray *selected_indices)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) selections = NULL;
  MuffinScreenCastSession *muffin_session;
  gboolean alive = TRUE;
  guint i;

  start_operation_busy_enter (op);
  if (!session_is_unlocked (op->session))
    {
      start_operation_fail (op, "The desktop is locked");
      goto out;
    }

  selections = g_ptr_array_new_with_free_func (NULL);
  for (i = 0; i < selected_indices->len; i++)
    {
      guint index = g_array_index (selected_indices, guint, i);

      if (index < op->items->len)
        g_ptr_array_add (selections, g_ptr_array_index (op->items, index));
    }

  if (selections->len == 0)
    {
      start_operation_fail (op, "No source selected");
      goto out;
    }

  muffin_session = muffin_screen_cast_create_session (muffin_screen_cast,
                                                      op->session->remote_id,
                                                      &error);
  if (!muffin_session)
    {
      start_operation_fail (op, error->message);
      goto out;
    }

  op->muffin_session = muffin_session;
  op->session->muffin_session = g_object_ref (muffin_session);

  g_signal_connect (muffin_session, "ready",
                    G_CALLBACK (on_muffin_session_ready), op);
  g_signal_connect (muffin_session, "closed",
                    G_CALLBACK (on_muffin_session_closed_during_start), op);

  for (i = 0; i < selections->len; i++)
    {
      PickerItem *selection;
      gboolean recorded;

      selection = g_ptr_array_index (selections, i);

      if (selection->type == SCREEN_CAST_SOURCE_TYPE_MONITOR)
        recorded = muffin_screen_cast_session_record_monitor (muffin_session,
                                                              selection->id,
                                                              selection->connector,
                                                              op->session->selection.cursor_mode,
                                                              &error);
      else
        recorded = muffin_screen_cast_session_record_window (muffin_session,
                                                             selection->id,
                                                             selection->window_id,
                                                             op->session->selection.cursor_mode,
                                                             &error);

      if (!recorded)
        {
          g_signal_handlers_disconnect_by_data (muffin_session, op);
          muffin_screen_cast_session_abort (muffin_session);
          g_clear_pointer ((GObject **) &op->muffin_session, g_object_unref);
          g_clear_pointer ((GObject **) &op->session->muffin_session, g_object_unref);
          start_operation_fail (op, error->message);
          goto out;
        }
    }

  if (!(op->session->remote ?
        start_remote_session (op->session, &error) :
        muffin_screen_cast_session_start (muffin_session, &error)))
    {
      g_signal_handlers_disconnect_by_data (muffin_session, op);
      muffin_screen_cast_session_abort (muffin_session);
      g_clear_pointer ((GObject **) &op->muffin_session, g_object_unref);
      g_clear_pointer ((GObject **) &op->session->muffin_session, g_object_unref);
      start_operation_fail (op, error->message);
      goto out;
    }
out:
  alive = start_operation_busy_leave (op);

  return alive;
}

static gboolean
start_muffin_session_timeout (gpointer user_data)
{
  StartOperation *op = user_data;
  GArray *indices = g_steal_pointer (&op->selected_indices);

  op->defer_id = 0;
  start_muffin_session (op, indices);
  g_array_free (indices, TRUE);

  return G_SOURCE_REMOVE;
}

static void
on_dialog_done (ScreenCastDialog *dialog,
                gboolean accepted,
                GArray *selected_indices,
                gpointer user_data)
{
  StartOperation *op = user_data;
  guint i;

  if (op->completed)
    return;

  op->dialog = NULL;

  if (!accepted || !selected_indices || selected_indices->len == 0)
    {
      start_operation_cancel (op);
      return;
    }

  op->selected_indices = g_array_new (FALSE, FALSE, sizeof (guint));
  for (i = 0; i < selected_indices->len; i++)
    g_array_append_val (op->selected_indices,
                        g_array_index (selected_indices, guint, i));

  op->session->start_pending = TRUE;
  op->defer_id = g_timeout_add (100, start_muffin_session_timeout, op);
}

static gboolean
on_request_close (XdpImplRequest *object,
                  GDBusMethodInvocation *invocation,
                  gpointer user_data)
{
  StartOperation *op = user_data;

  if (op->completed)
    return FALSE;

  start_operation_cancel (op);

  return FALSE;
}

static GdkPixbuf *
capture_preview (PickerItem *item,
                 const char *path)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *script = NULL;
  g_autofree char *response = NULL;
  g_autoptr(GdkPixbuf) pixbuf = NULL;
  gboolean success = FALSE;

  if (!cinnamon)
    return NULL;

  if (item->type == SCREEN_CAST_SOURCE_TYPE_MONITOR)
    {
      script = g_strdup_printf (
        "var f='%s';var l=new imports.gi.GLib.MainLoop(null,false);var o=false;"
        "var s=new imports.gi.Cinnamon.Screenshot();var u=global.ui_scale||1;"
        "s.screenshot_area(false,Math.round(%d*u),Math.round(%d*u),"
        "Math.round(%d*u),Math.round(%d*u),f,false,"
        "function(a,b){o=b;l.quit();});"
        "l.run();o;",
        path,
        item->rect.x,
        item->rect.y,
        item->rect.width,
        item->rect.height);
    }
  else if (item->type == SCREEN_CAST_SOURCE_TYPE_WINDOW)
    {
      script = g_strdup_printf (
        "var f='%s';var l=new imports.gi.GLib.MainLoop(null,false);var o=false;"
        "var s=new imports.gi.Cinnamon.Screenshot();"
        "s.screenshot_window_by_id(%" G_GUINT64_FORMAT ",false,false,f,false,"
        "function(a,b){o=b;l.quit();});"
        "l.run();o;",
        path,
        item->window_id);
    }
  else
    {
      return NULL;
    }

  if (!org_cinnamon_call_eval_sync (cinnamon,
                                    script,
                                    &success,
                                    &response,
                                    NULL,
                                    &error))
    {
      g_warning ("Failed to capture preview: %s", error->message);
      return NULL;
    }

  if (!success)
    {
      g_warning ("Cinnamon Eval failed to capture preview");
      return NULL;
    }

  if (!g_file_test (path, G_FILE_TEST_EXISTS))
    return NULL;

  pixbuf = gdk_pixbuf_new_from_file (path, &error);
  if (!pixbuf)
    g_warning ("Failed to load preview %s: %s", path, error->message);

  return g_steal_pointer (&pixbuf);
}

static void
maybe_present_dialog (StartOperation *op)
{
  if (op->presented || !op->dialog)
    return;

  if (op->preview_index >= ITEMS_PRESENT_AFTER || op->preview_index >= op->items->len)
    {
      op->presented = TRUE;
      screencast_dialog_present (op->dialog);
    }
}

static gboolean
on_preview_idle (gpointer user_data)
{
  StartOperation *op = user_data;
  PickerItem *item;
  g_autoptr(GdkPixbuf) preview = NULL;
  g_autofree char *path = NULL;
  gboolean keep_going = FALSE;

  start_operation_busy_enter (op);

  if (op->dialog && op->preview_index < op->items->len)
    {
      item = g_ptr_array_index (op->items, op->preview_index);
      path = g_strdup_printf ("%s/%u.png", op->preview_dir, op->preview_index);
      preview = capture_preview (item, path);
      g_remove (path);

      if (op->dialog)
        screencast_dialog_set_item_preview (op->dialog, op->preview_index, preview);
      op->preview_index++;

      maybe_present_dialog (op);

      keep_going = op->preview_index < op->items->len;
    }
  else
    {
      maybe_present_dialog (op);
    }

  if (!start_operation_busy_leave (op))
    return G_SOURCE_REMOVE;

  if (!keep_going)
    op->preview_idle_id = 0;

  return keep_going ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static gboolean
handle_start (XdpImplScreenCast *object,
              GDBusMethodInvocation *invocation,
              const char *arg_handle,
              const char *arg_session_handle,
              const char *arg_app_id,
              const char *arg_parent_window,
              GVariant *arg_options)
{
  ScreenCastSession *session;
  StartOperation *op;
  GDBusConnection *connection;
  guint i;

  g_debug ("Got new Start request");

  connection = g_dbus_method_invocation_get_connection (invocation);

  session = (ScreenCastSession *)lookup_session (arg_session_handle);
  if (!session ||
      !G_TYPE_CHECK_INSTANCE_TYPE (session, screencast_session_get_type ()))
    {
      g_warning ("No session found for %s", arg_session_handle);
      g_dbus_method_invocation_return_value (invocation,
                                             g_variant_new ("(u@a{sv})", 2, new_empty_results ()));
      return TRUE;
    }

  if (!session_owned (session, invocation, arg_app_id) ||
      (session->remote && !session->remote_proxy) ||
      session->started || session->start_pending || !session_is_unlocked (session))
    {
      g_dbus_method_invocation_return_value (invocation,
                                             g_variant_new ("(u@a{sv})", 2, new_empty_results ()));
      return TRUE;
    }

  op = g_new0 (StartOperation, 1);
  op->session = g_object_ref (session);
  op->impl = object;
  op->invocation = invocation;
  op->request = request_new (g_dbus_method_invocation_get_sender (invocation),
                             arg_app_id,
                             arg_handle);
  op->items = g_ptr_array_new_with_free_func (picker_item_free);
  session->start_pending = TRUE;
  session->pending = op;

  if (session->selection.source_types & SCREEN_CAST_SOURCE_TYPE_MONITOR)
    {
      g_autoptr(GPtrArray) monitors = NULL;

      monitors = get_monitors (connection);
      for (i = 0; monitors && i < monitors->len; i++)
        {
          MuffinMonitor *monitor = g_ptr_array_index (monitors, i);
          PickerItem *item;

          item = picker_item_new (SCREEN_CAST_SOURCE_TYPE_MONITOR, i);
          item->title = g_strdup (monitor->label);
          item->subtitle = g_strdup_printf ("%d×%d%s",
                                            monitor->rect.width,
                                            monitor->rect.height,
                                            monitor->primary ? _("  (Primary)") : "");
          item->connector = g_strdup (monitor->connector);
          item->rect = monitor->rect;
          g_ptr_array_add (op->items, item);
        }
    }

  if (session->selection.source_types & SCREEN_CAST_SOURCE_TYPE_WINDOW)
    {
      g_autoptr(GPtrArray) windows = NULL;
      guint base_id = op->items->len;

      windows = get_windows (connection);
      for (i = 0; windows && i < windows->len; i++)
        {
          PickerItem *window = g_ptr_array_index (windows, i);

          window->id = base_id + i;
          g_ptr_array_add (op->items, window);
        }
    }

  if (op->items->len == 0)
    {
      start_operation_fail (op, "No shareable sources found");
      return TRUE;
    }

  g_signal_connect (op->request, "handle-close",
                    G_CALLBACK (on_request_close), op);
  request_export (op->request, connection);

  op->dialog = screencast_dialog_new (_("Choose what to share"),
                                      session->selection.multiple);

  op->preview_dir = g_dir_make_tmp ("xdp-xapp-preview-XXXXXX", NULL);

  for (i = 0; i < op->items->len; i++)
    {
      PickerItem *item = g_ptr_array_index (op->items, i);

      screencast_dialog_add_item (op->dialog, item->title, item->subtitle);
    }

  screencast_dialog_set_done_func (op->dialog, on_dialog_done, op);

  if (op->preview_dir)
    op->preview_idle_id = g_idle_add (on_preview_idle, op);
  else
    screencast_dialog_present (op->dialog);

  return TRUE;
}

static gboolean
handle_select_sources (XdpImplScreenCast *object,
                       GDBusMethodInvocation *invocation,
                       const char *arg_handle,
                       const char *arg_session_handle,
                       const char *arg_app_id,
                       GVariant *arg_options)
{
  ScreenCastSession *session;
  guint32 types = 0;
  gboolean multiple = FALSE;
  guint32 cursor_mode = SCREEN_CAST_CURSOR_MODE_HIDDEN;

  g_debug ("Got new SelectSources request");

  session = (ScreenCastSession *)lookup_session (arg_session_handle);
  if (!session ||
      !G_TYPE_CHECK_INSTANCE_TYPE (session, screencast_session_get_type ()) ||
      !session_owned (session, invocation, arg_app_id) ||
      session->started || session->start_pending)
    {
      g_warning ("No session found for %s", arg_session_handle);
      xdp_impl_screen_cast_complete_select_sources (object, invocation, 2,
                                                    new_empty_results ());
      return TRUE;
    }

  g_variant_lookup (arg_options, "types", "u", &types);
  g_variant_lookup (arg_options, "multiple", "b", &multiple);
  g_variant_lookup (arg_options, "cursor_mode", "u", &cursor_mode);
  session->sources_selected = TRUE;

  session->selection.multiple = multiple;
  session->selection.cursor_mode = cursor_mode;

  if (types)
    session->selection.source_types = types;
  else
    session->selection.source_types =
      SCREEN_CAST_SOURCE_TYPE_MONITOR | SCREEN_CAST_SOURCE_TYPE_WINDOW;

  g_debug ("Screencast selection: types=%u multiple=%d cursor_mode=%u",
           session->selection.source_types,
           session->selection.multiple,
           session->selection.cursor_mode);

  xdp_impl_screen_cast_complete_select_sources (object, invocation, 0,
                                                new_empty_results ());

  return TRUE;
}

static gboolean
handle_create_session (XdpImplScreenCast *object,
                       GDBusMethodInvocation *invocation,
                       const char *arg_handle,
                       const char *arg_session_handle,
                       const char *arg_app_id,
                       GVariant *arg_options)
{
  g_autoptr(GError) error = NULL;
  int response;
  Session *session;

  g_debug ("Got new CreateSession request");
  if (lookup_session (arg_session_handle))
    {
      xdp_impl_screen_cast_complete_create_session (object, invocation, 2, new_empty_results ());
      return TRUE;
    }

  session = (Session *)screencast_session_new (arg_session_handle);
  if (!setup_session ((ScreenCastSession *) session, invocation, arg_app_id, &error))
    {
      g_object_unref (session);
      xdp_impl_screen_cast_complete_create_session (object, invocation, 2, new_empty_results ());
      return TRUE;
    }

  if (!session_export (session,
                       g_dbus_method_invocation_get_connection (invocation),
                       &error))
    {
      g_clear_object (&session);
      g_warning ("Failed to create screencast session: %s", error->message);
      response = 2;
    }
  else
    {
      response = 0;
    }

  xdp_impl_screen_cast_complete_create_session (object, invocation, response,
                                                new_empty_results ());

  return TRUE;
}

gboolean
screen_cast_init (GDBusConnection *bus,
                  GError **error)
{
  GDBusInterfaceSkeleton *helper;

  cinnamon = org_cinnamon_proxy_new_sync (bus,
                                          G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                          "org.Cinnamon",
                                          "/org/Cinnamon",
                                          NULL,
                                          error);
  if (!cinnamon)
    return FALSE;

  muffin_screen_cast = muffin_screen_cast_new (bus);

  helper = G_DBUS_INTERFACE_SKELETON (xdp_impl_screen_cast_skeleton_new ());

  g_object_set (helper,
                "version", 3,
                "available-source-types",
                SCREEN_CAST_SOURCE_TYPE_MONITOR | SCREEN_CAST_SOURCE_TYPE_WINDOW,
                "available-cursor-modes",
                SCREEN_CAST_CURSOR_MODE_HIDDEN |
                SCREEN_CAST_CURSOR_MODE_EMBEDDED |
                SCREEN_CAST_CURSOR_MODE_METADATA,
                NULL);

  g_signal_connect (helper, "handle-create-session",
                    G_CALLBACK (handle_create_session), NULL);
  g_signal_connect (helper, "handle-select-sources",
                    G_CALLBACK (handle_select_sources), NULL);
  g_signal_connect (helper, "handle-start",
                    G_CALLBACK (handle_start), NULL);

  if (!g_dbus_interface_skeleton_export (helper,
                                         bus,
                                         DESKTOP_PORTAL_OBJECT_PATH,
                                         error))
    return FALSE;

  g_debug ("providing %s", g_dbus_interface_skeleton_get_info (helper)->name);

  return remote_desktop_init (bus, error);
}

static gboolean
session_owned (ScreenCastSession *session, GDBusMethodInvocation *invocation, const char *app_id)
{
  return !session->parent.closed &&
         g_strcmp0 (session->owner, g_dbus_method_invocation_get_sender (invocation)) == 0 &&
         (!app_id || g_strcmp0 (session->app_id, app_id) == 0);
}

static gboolean
session_is_unlocked (ScreenCastSession *session)
{
  g_autoptr(GVariant) reply = NULL;
  g_autofree char *owner = NULL;
  gboolean active = TRUE;
  if (!session->locker)
    return FALSE;
  owner = g_dbus_proxy_get_name_owner (session->locker);
  if (!owner)
    return FALSE;
  reply = g_dbus_proxy_call_sync (session->locker, "GetActive", NULL,
                                  G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
  if (!reply)
    return FALSE;
  g_variant_get (reply, "(b)", &active);
  session->unlocked = !active;
  return session->unlocked;
}

static void
on_locker_signal (GDBusProxy *proxy, const char *sender, const char *signal,
                  GVariant *parameters, gpointer data)
{
  gboolean active;
  if (g_str_equal (signal, "ActiveChanged"))
    {
      g_variant_get (parameters, "(b)", &active);
      ((ScreenCastSession *) data)->unlocked = !active;
      if (active)
        terminate_sharing (data);
    }
}

static void
on_service_owner_changed (GObject *object, GParamSpec *pspec, gpointer data)
{
  g_autofree char *owner = g_dbus_proxy_get_name_owner (G_DBUS_PROXY (object));
  if (!owner)
    terminate_sharing (data);
}

static void
on_frontend_vanished (GDBusConnection *connection, const char *name, gpointer data)
{
  terminate_sharing (data);
}

static gboolean
setup_session (ScreenCastSession *session, GDBusMethodInvocation *invocation,
               const char *app_id, GError **error)
{
  GDBusConnection *connection = g_dbus_method_invocation_get_connection (invocation);
  session->owner = g_strdup (g_dbus_method_invocation_get_sender (invocation));
  session->app_id = g_strdup (app_id);
  session->locker = g_dbus_proxy_new_sync (connection, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                          NULL, "org.cinnamon.ScreenSaver",
                                          "/org/cinnamon/ScreenSaver", "org.cinnamon.ScreenSaver", NULL, error);
  if (!session->locker)
    return FALSE;
  g_signal_connect (session->locker, "g-signal", G_CALLBACK (on_locker_signal), session);
  g_signal_connect (session->locker, "notify::g-name-owner", G_CALLBACK (on_service_owner_changed), session);
  session->owner_watch = g_bus_watch_name_on_connection (connection, session->owner,
                                                         G_BUS_NAME_WATCHER_FLAGS_NONE, NULL,
                                                         on_frontend_vanished, session, NULL);
  return TRUE;
}

static void
on_stop_sharing (GtkButton *button, gpointer data)
{
  terminate_sharing (data);
}

static gboolean
on_indicator_delete (GtkWidget *widget, GdkEvent *event, gpointer data)
{
  terminate_sharing (data);
  return TRUE;
}

static void
show_sharing_indicator (ScreenCastSession *session)
{
  GtkWidget *box, *label, *button;
  g_autofree char *text = NULL;
  session->indicator = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title (GTK_WINDOW (session->indicator), _("Desktop sharing"));
  gtk_window_set_keep_above (GTK_WINDOW (session->indicator), TRUE);
  gtk_window_set_resizable (GTK_WINDOW (session->indicator), FALSE);
  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_container_set_border_width (GTK_CONTAINER (box), 12);
  text = g_strdup_printf (session->remote ? _("%s is controlling your desktop") :
                                            _("%s is sharing your screen"),
                          session->app_id[0] ? session->app_id : _("An application"));
  label = gtk_label_new (text);
  button = gtk_button_new_with_label (_("Stop sharing"));
  gtk_container_add (GTK_CONTAINER (session->indicator), box);
  gtk_box_pack_start (GTK_BOX (box), label, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (box), button, FALSE, FALSE, 0);
  g_signal_connect (button, "clicked", G_CALLBACK (on_stop_sharing), session);
  g_signal_connect (session->indicator, "delete-event", G_CALLBACK (on_indicator_delete), session);
  gtk_widget_show_all (session->indicator);
}

static void
release_pressed (ScreenCastSession *session, GHashTable *pressed, const char *method, gboolean signed_code)
{
  GHashTableIter iter;
  gpointer key;
  if (!pressed)
    return;
  g_hash_table_iter_init (&iter, pressed);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      GVariant *parameters = signed_code ?
        g_variant_new ("(ib)", GPOINTER_TO_INT (key), FALSE) :
        g_variant_new ("(ub)", GPOINTER_TO_UINT (key), FALSE);
      g_dbus_proxy_call (session->remote_proxy, method, parameters,
                         G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, NULL);
    }
  g_hash_table_remove_all (pressed);
}

static void
release_remote_input (ScreenCastSession *session)
{
  release_pressed (session, session->pressed_keys, "NotifyKeyboardKeycode", FALSE);
  release_pressed (session, session->pressed_keysyms, "NotifyKeyboardKeysym", FALSE);
  release_pressed (session, session->pressed_buttons, "NotifyPointerButton", TRUE);
}

static void
on_remote_signal (GDBusProxy *proxy, const char *sender, const char *signal,
                  GVariant *parameters, gpointer data)
{
  if (g_str_equal (signal, "Closed"))
    terminate_sharing (data);
}

static gboolean
create_remote_session (ScreenCastSession *session, GDBusConnection *connection, GError **error)
{
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GVariant) id = NULL;
  const char *path;
  reply = g_dbus_connection_call_sync (connection, "org.cinnamon.Muffin.RemoteDesktop",
                                       "/org/cinnamon/Muffin/RemoteDesktop",
                                       "org.cinnamon.Muffin.RemoteDesktop", "CreateSession", NULL,
                                       G_VARIANT_TYPE ("(o)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, error);
  if (!reply)
    return FALSE;
  g_variant_get (reply, "(&o)", &path);
  session->remote_proxy = g_dbus_proxy_new_sync (connection, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                                NULL, "org.cinnamon.Muffin.RemoteDesktop", path,
                                                "org.cinnamon.Muffin.RemoteDesktop.Session", NULL, error);
  if (!session->remote_proxy)
    return FALSE;
  id = g_dbus_proxy_get_cached_property (session->remote_proxy, "SessionId");
  if (!id)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Remote desktop session has no ID");
      return FALSE;
    }
  session->remote_id = g_variant_dup_string (id, NULL);
  g_signal_connect (session->remote_proxy, "g-signal", G_CALLBACK (on_remote_signal), session);
  g_signal_connect (session->remote_proxy, "notify::g-name-owner", G_CALLBACK (on_service_owner_changed), session);
  return TRUE;
}

typedef struct
{
  ScreenCastSession *session;
  Request *request;
  GDBusMethodInvocation *invocation;
  char *handle;
  char *parent_window;
  GVariant *options;
} RemoteConsent;

static void
remote_result (GDBusMethodInvocation *invocation, guint response, GVariant *results)
{
  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(u@a{sv})", response,
                                                        results ? results : new_empty_results ()));
}

static void
on_remote_consent (GtkDialog *dialog, gint response, gpointer data)
{
  RemoteConsent *consent = data;
  ScreenCastSession *session = consent->session;
  g_autoptr(GError) error = NULL;
  gboolean accepted = response == GTK_RESPONSE_ACCEPT && !session->parent.closed &&
                      session_is_unlocked (session);

  session->consent = NULL;
  session->start_pending = FALSE;
  gtk_widget_destroy (GTK_WIDGET (dialog));
  if (consent->request->exported)
    request_unexport (consent->request);
  if (accepted)
    accepted = create_remote_session (session,
                                      g_dbus_method_invocation_get_connection (consent->invocation), &error);
  if (!accepted)
    {
      remote_result (consent->invocation, response == GTK_RESPONSE_ACCEPT ? 2 : 1, NULL);
      terminate_sharing (session);
    }
  else if (session->sources_selected)
    handle_start (NULL, consent->invocation, consent->handle, session->parent.id,
                   session->app_id, consent->parent_window, consent->options);
  else if (start_remote_session (session, &error))
    {
      GVariantBuilder results;
      session->started = TRUE;
      show_sharing_indicator (session);
      g_variant_builder_init (&results, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&results, "{sv}", "devices", g_variant_new_uint32 (session->devices));
      remote_result (consent->invocation, 0, g_variant_builder_end (&results));
    }
  else
    {
      remote_result (consent->invocation, 2, NULL);
      terminate_sharing (session);
    }
  g_object_unref (consent->request);
  g_object_unref (consent->session);
  g_object_unref (consent->invocation);
  g_variant_unref (consent->options);
  g_free (consent->handle);
  g_free (consent->parent_window);
  g_free (consent);
}

static gboolean
on_remote_request_close (XdpImplRequest *request, GDBusMethodInvocation *invocation, gpointer data)
{
  ScreenCastSession *session = data;
  if (session->consent)
    gtk_dialog_response (GTK_DIALOG (session->consent), GTK_RESPONSE_CANCEL);
  return FALSE;
}

static void
request_remote_consent (ScreenCastSession *session, GDBusMethodInvocation *invocation,
                         const char *handle, const char *parent_window, GVariant *options)
{
  RemoteConsent *consent = g_new0 (RemoteConsent, 1);
  const char *devices = session->devices == 3 ? _("keyboard and pointer") :
                        session->devices == 1 ? _("keyboard") : _("pointer");
  consent->session = g_object_ref (session);
  consent->invocation = g_object_ref (invocation);
  consent->handle = g_strdup (handle);
  consent->parent_window = g_strdup (parent_window);
  consent->options = g_variant_ref (options);
  consent->request = request_new (session->owner, session->app_id, handle);
  request_export (consent->request, g_dbus_method_invocation_get_connection (invocation));
  g_signal_connect (consent->request, "handle-close", G_CALLBACK (on_remote_request_close), session);
  session->start_pending = TRUE;
  session->consent = gtk_message_dialog_new (NULL, 0, GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
                                             _("Allow %s to control your %s?"),
                                             session->app_id[0] ? session->app_id : _("an application"), devices);
  gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (session->consent),
                                             _("Remote input can operate applications as you. "
                                               "Sharing stops when you lock the screen or choose Stop sharing."));
  gtk_dialog_add_buttons (GTK_DIALOG (session->consent), _("Cancel"), GTK_RESPONSE_CANCEL,
                           _("Allow control"), GTK_RESPONSE_ACCEPT, NULL);
  gtk_dialog_set_default_response (GTK_DIALOG (session->consent), GTK_RESPONSE_CANCEL);
  g_signal_connect (session->consent, "response", G_CALLBACK (on_remote_consent), consent);
  gtk_widget_show (session->consent);
}

static void
input_finished (GObject *object, GAsyncResult *result, gpointer data)
{
  GDBusMethodInvocation *invocation = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (object), result, &error);
  if (reply)
    g_dbus_method_invocation_return_value (invocation, NULL);
  else
    g_dbus_method_invocation_return_gerror (invocation, error);
  g_object_unref (invocation);
}

static void
remote_method_call (GDBusConnection *connection, const char *sender, const char *path,
                    const char *interface, const char *method, GVariant *parameters,
                    GDBusMethodInvocation *invocation, gpointer data)
{
  g_autoptr(GVariant) options = NULL;
  g_autoptr(GError) error = NULL;
  ScreenCastSession *session;
  const char *handle, *session_handle, *app_id, *parent_window = NULL;
  GVariant *forward = NULL;
  const char *forward_method = method;
  guint32 state = 0, code = 0, node;
  gint32 button;
  double x, y;
  GHashTable *pressed = NULL;

  if (g_str_equal (method, "CreateSession"))
    {
      g_variant_get (parameters, "(&o&o&s@a{sv})", &handle, &session_handle, &app_id, &options);
      if (lookup_session (session_handle))
        {
          remote_result (invocation, 2, NULL);
          return;
        }
      session = screencast_session_new (session_handle);
      session->remote = TRUE;
      session->devices = 3;
      session->pressed_keys = g_hash_table_new (g_direct_hash, g_direct_equal);
      session->pressed_keysyms = g_hash_table_new (g_direct_hash, g_direct_equal);
      session->pressed_buttons = g_hash_table_new (g_direct_hash, g_direct_equal);
      if (!setup_session (session, invocation, app_id, &error) ||
          !session_export ((Session *) session, connection, &error))
        {
          g_object_unref (session);
          remote_result (invocation, 2, NULL);
        }
      else
        remote_result (invocation, 0, NULL);
      return;
    }
  if (g_str_equal (method, "SelectDevices") || g_str_equal (method, "Start"))
    {
      if (g_str_equal (method, "Start"))
        g_variant_get (parameters, "(&o&o&s&s@a{sv})",
                       &handle, &session_handle, &app_id, &parent_window, &options);
      else
        g_variant_get (parameters, "(&o&o&s@a{sv})", &handle, &session_handle, &app_id, &options);
      session = (ScreenCastSession *) lookup_session (session_handle);
      if (!session || !G_TYPE_CHECK_INSTANCE_TYPE (session, screencast_session_get_type ()) ||
          !session->remote || !session_owned (session, invocation, app_id) ||
          session->started || session->start_pending)
        {
          remote_result (invocation, 2, NULL);
          return;
        }
      if (g_str_equal (method, "SelectDevices"))
        {
          guint32 types = 3;
          g_variant_lookup (options, "types", "u", &types);
          if (types == 0 || (types & ~3))
            remote_result (invocation, 2, NULL);
          else
            {
              session->devices = types;
              remote_result (invocation, 0, NULL);
            }
        }
      else if (!session_is_unlocked (session))
        remote_result (invocation, 2, NULL);
      else
        request_remote_consent (session, invocation, handle, parent_window, options);
      return;
    }
  if (!g_str_has_prefix (method, "Notify"))
    {
      g_dbus_method_invocation_return_error (invocation, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                             "This remote desktop method is not supported");
      return;
    }
  g_variant_get_child (parameters, 0, "&o", &session_handle);
  session = (ScreenCastSession *) lookup_session (session_handle);
  if (!session || !G_TYPE_CHECK_INSTANCE_TYPE (session, screencast_session_get_type ()) ||
      !session->remote || !session_owned (session, invocation, NULL) || !session->started ||
      !session->remote_proxy || !session->unlocked ||
      ((g_str_has_prefix (method, "NotifyKeyboard") ? 1 : 2) & session->devices) == 0)
    goto refused;
  if (g_str_equal (method, "NotifyKeyboardKeycode") || g_str_equal (method, "NotifyKeyboardKeysym"))
    {
      g_variant_get_child (parameters, 2, "i", &button);
      code = button;
      g_variant_get_child (parameters, 3, "u", &state);
      if (state > 1 || button < 0)
        goto refused;
      pressed = g_str_equal (method, "NotifyKeyboardKeycode") ? session->pressed_keys : session->pressed_keysyms;
      forward = g_variant_new ("(ub)", code, state != 0);
    }
  else if (g_str_equal (method, "NotifyPointerButton"))
    {
      g_variant_get_child (parameters, 2, "i", &button);
      g_variant_get_child (parameters, 3, "u", &state);
      if (state > 1)
        goto refused;
      code = button;
      pressed = session->pressed_buttons;
      forward = g_variant_new ("(ib)", button, state != 0);
    }
  else if (g_str_equal (method, "NotifyPointerMotionAbsolute"))
    {
      const char *stream;
      g_variant_get_child (parameters, 2, "u", &node);
      g_variant_get_child (parameters, 3, "d", &x);
      g_variant_get_child (parameters, 4, "d", &y);
      stream = session->muffin_session ?
        muffin_screen_cast_session_stream_path (session->muffin_session, node) : NULL;
      if (!stream || !isfinite (x) || !isfinite (y))
        goto refused;
      forward = g_variant_new ("(sdd)", stream, x, y);
    }
  else if (g_str_equal (method, "NotifyPointerMotion") || g_str_equal (method, "NotifyPointerAxis"))
    {
      g_variant_get_child (parameters, 2, "d", &x);
      g_variant_get_child (parameters, 3, "d", &y);
      if (!isfinite (x) || !isfinite (y))
        goto refused;
      if (g_str_equal (method, "NotifyPointerMotion"))
        {
          forward_method = "NotifyPointerMotionRelative";
          forward = g_variant_new ("(dd)", x, y);
        }
      else
        {
          gboolean finish = FALSE;
          options = g_variant_get_child_value (parameters, 1);
          g_variant_lookup (options, "finish", "b", &finish);
          forward = g_variant_new ("(ddu)", x, y, finish ? 1 : 0);
        }
    }
  else if (g_str_equal (method, "NotifyPointerAxisDiscrete"))
    {
      g_variant_get_child (parameters, 2, "u", &code);
      g_variant_get_child (parameters, 3, "i", &button);
      if (code > 1)
        goto refused;
      forward = g_variant_new ("(ui)", code, button);
    }
  else
    goto refused;
  if (pressed)
    {
      if (state)
        g_hash_table_add (pressed, GUINT_TO_POINTER (code));
      else
        g_hash_table_remove (pressed, GUINT_TO_POINTER (code));
    }
  g_dbus_proxy_call (session->remote_proxy, forward_method, forward,
                     G_DBUS_CALL_FLAGS_NONE, 5000, NULL, input_finished, g_object_ref (invocation));
  return;
refused:
  g_dbus_method_invocation_return_error (invocation, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                         "Remote input is not authorized for this session");
}

static GVariant *
remote_get_property (GDBusConnection *connection, const char *sender, const char *path,
                      const char *interface, const char *property, GError **error, gpointer data)
{
  if (g_str_equal (property, "version"))
    return g_variant_new_uint32 (1);
  if (g_str_equal (property, "AvailableDeviceTypes"))
    return g_variant_new_uint32 (3);
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Unknown property");
  return NULL;
}

static gboolean
remote_desktop_init (GDBusConnection *bus, GError **error)
{
  static const GDBusInterfaceVTable vtable = {
    .method_call = remote_method_call,
    .get_property = remote_get_property,
  };
  g_autoptr(XdpImplRemoteDesktop) helper = xdp_impl_remote_desktop_skeleton_new ();
  return g_dbus_connection_register_object (bus, DESKTOP_PORTAL_OBJECT_PATH,
                                             g_dbus_interface_skeleton_get_info (G_DBUS_INTERFACE_SKELETON (helper)),
                                             &vtable, NULL, NULL, error) != 0;
}
