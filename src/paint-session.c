#include "paint-session.h"

#include <glib/gstdio.h>

#include "paint-canvas-page.h"
#include "paint-preferences.h"
#include "paint-window.h"

static guint autosave_timer_id = 0;

static char *
session_dir_path (void)
{
  return g_build_filename (g_get_user_state_dir (), "paint", "session", NULL);
}

static gboolean
write_surface_png_atomic (cairo_surface_t *surface, const char *dest_path)
{
  if (!surface || !dest_path)
    return FALSE;
  g_autofree char *tmp_path = g_strconcat (dest_path, ".tmp", NULL);
  if (cairo_surface_write_to_png (surface, tmp_path) != CAIRO_STATUS_SUCCESS) {
    g_unlink (tmp_path);
    return FALSE;
  }
  if (g_rename (tmp_path, dest_path) != 0) {
    g_unlink (tmp_path);
    return FALSE;
  }
  return TRUE;
}

void
paint_window_clear_session (void)
{
  paint_session_cancel_autosave ();

  g_autofree char *dir = session_dir_path ();
  if (!g_file_test (dir, G_FILE_TEST_IS_DIR))
    return;

  GDir *d = g_dir_open (dir, 0, NULL);
  if (d) {
    const char *name;
    while ((name = g_dir_read_name (d)) != NULL) {
      g_autofree char *full = g_build_filename (dir, name, NULL);
      g_unlink (full);
    }
    g_dir_close (d);
  }
}

static void
save_sessions_internal (AdwApplication *app, gboolean commit_overlays)
{
  if (!app)
    return;

  g_autofree char *dir = session_dir_path ();
  g_mkdir_with_parents (dir, 0755);

  g_autoptr (GHashTable) keep_files = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_hash_table_add (keep_files, g_strdup ("session.ini"));

  g_autoptr (GKeyFile) kf = g_key_file_new ();
  int win_idx = 0;

  for (GList *l = gtk_application_get_windows (GTK_APPLICATION (app)); l != NULL; l = l->next) {
    if (!PAINT_IS_WINDOW (l->data))
      continue;
    PaintWindow *win = PAINT_WINDOW (l->data);
    AdwTabView *tab_view = paint_window_get_tab_view (win);
    int n_pages = adw_tab_view_get_n_pages (tab_view);
    if (n_pages == 0)
      continue;

    g_autofree char *win_grp = g_strdup_printf ("Window.%d", win_idx);
    PaintPreferences *prefs = paint_window_get_preferences (win);
    gboolean is_max = gtk_window_is_maximized (GTK_WINDOW (win));
    gboolean is_fs = gtk_window_is_fullscreen (GTK_WINDOW (win));
    g_key_file_set_boolean (kf, win_grp, "Maximized", is_max);
    int w_width = (!is_max && !is_fs)
                    ? gtk_widget_get_width (GTK_WIDGET (win))
                    : (prefs ? prefs->last_window_width : 0);
    int w_height = (!is_max && !is_fs)
                     ? gtk_widget_get_height (GTK_WIDGET (win))
                     : (prefs ? prefs->last_window_height : 0);
    if (w_width >= 360 && w_height >= 300) {
      g_key_file_set_integer (kf, win_grp, "Width", w_width);
      g_key_file_set_integer (kf, win_grp, "Height", w_height);
    }

    AdwTabPage *selected = adw_tab_view_get_selected_page (tab_view);
    int active_idx = 0;
    int saved_tabs = 0;

    for (int i = 0; i < n_pages; i++) {
      AdwTabPage *tab = adw_tab_view_get_nth_page (tab_view, i);
      GtkWidget *child = adw_tab_page_get_child (tab);
      if (!PAINT_IS_CANVAS_PAGE (child))
        continue;
      if (tab == selected)
        active_idx = saved_tabs;

      PaintCanvasPage *page = PAINT_CANVAS_PAGE (child);
      PaintCanvas *canvas = paint_canvas_page_get_canvas (page);
      if (commit_overlays)
        paint_canvas_commit_active_overlays (canvas);
      PaintDocument *doc = paint_canvas_get_document (canvas);

      g_autofree char *tab_grp = g_strdup_printf ("Tab.%d.%d", win_idx, saved_tabs);
      GFile *orig_file = paint_document_get_file (doc);
      gboolean modified = paint_document_get_modified (doc);

      if (orig_file) {
        g_autofree char *orig_path = g_file_get_path (orig_file);
        if (orig_path)
          g_key_file_set_string (kf, tab_grp, "FilePath", orig_path);
        g_autofree char *orig_uri = g_file_get_uri (orig_file);
        if (orig_uri)
          g_key_file_set_string (kf, tab_grp, "FileUri", orig_uri);
      }

      int n_layers = paint_document_get_n_layers (doc);
      gboolean multi_or_custom = paint_document_has_layer_structure (doc);
      if (multi_or_custom) {
        g_key_file_set_integer (kf, tab_grp, "LayerCount", n_layers);
        g_key_file_set_integer (kf, tab_grp, "ActiveLayer", paint_document_get_active_layer (doc));
        for (int li = 0; li < n_layers; li++) {
          g_autofree char *draft_name = g_strdup_printf ("draft-%d-%d-L%d.png", win_idx, saved_tabs, li);
          g_autofree char *draft_path = g_build_filename (dir, draft_name, NULL);
          cairo_surface_t *lsurf = paint_document_layer_get_surface (doc, li);
          if (write_surface_png_atomic (lsurf, draft_path)) {
            g_hash_table_add (keep_files, g_strdup (draft_name));
            g_autofree char *k_file = g_strdup_printf ("Layer.%d.File", li);
            g_autofree char *k_name = g_strdup_printf ("Layer.%d.Name", li);
            g_autofree char *k_vis = g_strdup_printf ("Layer.%d.Visible", li);
            g_autofree char *k_op = g_strdup_printf ("Layer.%d.Opacity", li);
            g_autofree char *k_blend = g_strdup_printf ("Layer.%d.BlendMode", li);
            g_autofree char *k_alock = g_strdup_printf ("Layer.%d.AlphaLock", li);
            g_autofree char *k_lock = g_strdup_printf ("Layer.%d.Locked", li);
            g_key_file_set_string (kf, tab_grp, k_file, draft_name);
            g_key_file_set_string (kf, tab_grp, k_name, paint_document_layer_get_name (doc, li));
            g_key_file_set_boolean (kf, tab_grp, k_vis, paint_document_layer_get_visible (doc, li));
            g_key_file_set_double (kf, tab_grp, k_op, paint_document_layer_get_opacity (doc, li));
            g_key_file_set_integer (kf, tab_grp, k_blend, (int) paint_document_layer_get_blend_mode (doc, li));
            g_key_file_set_boolean (kf, tab_grp, k_alock, paint_document_layer_get_alpha_lock (doc, li));
            g_key_file_set_boolean (kf, tab_grp, k_lock, paint_document_layer_get_locked (doc, li));
          }
        }
      } else if (modified || !orig_file) {
        g_autofree char *draft_name = g_strdup_printf ("draft-%d-%d.png", win_idx, saved_tabs);
        g_autofree char *draft_path = g_build_filename (dir, draft_name, NULL);
        if (write_surface_png_atomic (paint_document_get_surface (doc), draft_path)) {
          g_hash_table_add (keep_files, g_strdup (draft_name));
          g_key_file_set_string (kf, tab_grp, "DraftFile", draft_name);
        }
      }

      g_key_file_set_boolean (kf, tab_grp, "Modified", modified);
      g_key_file_set_double (kf, tab_grp, "Zoom", paint_canvas_get_zoom (canvas));
      saved_tabs++;
    }

    g_key_file_set_integer (kf, win_grp, "TabCount", saved_tabs);
    g_key_file_set_integer (kf, win_grp, "ActiveTab", active_idx);
    win_idx++;
  }

  g_key_file_set_integer (kf, "Session", "WindowCount", win_idx);
  g_autofree char *ini_path = g_build_filename (dir, "session.ini", NULL);
  gsize ini_len = 0;
  g_autofree char *ini_data = g_key_file_to_data (kf, &ini_len, NULL);
  if (ini_data && g_file_set_contents (ini_path, ini_data, (gssize) ini_len, NULL)) {
    GDir *d = g_dir_open (dir, 0, NULL);
    if (d) {
      const char *name;
      while ((name = g_dir_read_name (d)) != NULL) {
        if (!g_hash_table_contains (keep_files, name)) {
          g_autofree char *full = g_build_filename (dir, name, NULL);
          g_unlink (full);
        }
      }
      g_dir_close (d);
    }
  }
}

gboolean
paint_window_restore_session (AdwApplication *app)
{
  g_autofree char *dir = session_dir_path ();
  g_autofree char *ini_path = g_build_filename (dir, "session.ini", NULL);
  g_autoptr (GKeyFile) kf = g_key_file_new ();
  if (!g_key_file_load_from_file (kf, ini_path, G_KEY_FILE_NONE, NULL))
    return FALSE;

  int win_count = g_key_file_get_integer (kf, "Session", "WindowCount", NULL);
  if (win_count <= 0)
    return FALSE;

  gboolean restored_any = FALSE;
  for (int w = 0; w < win_count; w++) {
    g_autofree char *win_grp = g_strdup_printf ("Window.%d", w);
    int tab_count = g_key_file_get_integer (kf, win_grp, "TabCount", NULL);
    if (tab_count <= 0)
      continue;

    PaintWindow *win = paint_window_new_empty (app);
    AdwTabView *tab_view = paint_window_get_tab_view (win);

    for (int t = 0; t < tab_count; t++) {
      g_autofree char *tab_grp = g_strdup_printf ("Tab.%d.%d", w, t);
      g_autofree char *file_path = g_key_file_get_string (kf, tab_grp, "FilePath", NULL);
      g_autofree char *file_uri = g_key_file_get_string (kf, tab_grp, "FileUri", NULL);
      g_autofree char *draft_name = g_key_file_get_string (kf, tab_grp, "DraftFile", NULL);
      gboolean modified = g_key_file_get_boolean (kf, tab_grp, "Modified", NULL);
      double zoom = g_key_file_has_key (kf, tab_grp, "Zoom", NULL)
                      ? g_key_file_get_double (kf, tab_grp, "Zoom", NULL)
                      : 1.0;

      g_autoptr (GFile) orig_file = NULL;
      if (file_uri)
        orig_file = g_file_new_for_uri (file_uri);
      else if (file_path)
        orig_file = g_file_new_for_path (file_path);

      PaintDocument *doc = NULL;
      int layer_count = g_key_file_has_key (kf, tab_grp, "LayerCount", NULL)
                          ? g_key_file_get_integer (kf, tab_grp, "LayerCount", NULL)
                          : 0;
      if (layer_count >= 1) {
        g_autofree char *l0_file_name = g_key_file_get_string (kf, tab_grp, "Layer.0.File", NULL);
        if (l0_file_name) {
          g_autofree char *l0_path = g_build_filename (dir, l0_file_name, NULL);
          g_autoptr (GFile) l0_file = g_file_new_for_path (l0_path);
          doc = paint_document_new_from_draft (l0_file, orig_file, modified, NULL);
        }
        if (doc) {
          g_autofree char *l0_name = g_key_file_get_string (kf, tab_grp, "Layer.0.Name", NULL);
          if (l0_name)
            paint_document_layer_set_name (doc, 0, l0_name);
          gboolean l0_vis = g_key_file_has_key (kf, tab_grp, "Layer.0.Visible", NULL)
                              ? g_key_file_get_boolean (kf, tab_grp, "Layer.0.Visible", NULL)
                              : TRUE;
          paint_document_layer_set_visible (doc, 0, l0_vis);
          double l0_op = g_key_file_has_key (kf, tab_grp, "Layer.0.Opacity", NULL)
                           ? g_key_file_get_double (kf, tab_grp, "Layer.0.Opacity", NULL)
                           : 1.0;
          paint_document_layer_set_opacity (doc, 0, l0_op);
          int l0_blend = g_key_file_has_key (kf, tab_grp, "Layer.0.BlendMode", NULL)
                           ? g_key_file_get_integer (kf, tab_grp, "Layer.0.BlendMode", NULL)
                           : PAINT_BLEND_NORMAL;
          paint_document_layer_set_blend_mode (
              doc, 0, (PaintBlendMode) CLAMP (l0_blend, 0, PAINT_BLEND_LIGHTEN));
          gboolean l0_alock = g_key_file_has_key (kf, tab_grp, "Layer.0.AlphaLock", NULL)
                                ? g_key_file_get_boolean (kf, tab_grp, "Layer.0.AlphaLock", NULL)
                                : FALSE;
          gboolean l0_lock = g_key_file_has_key (kf, tab_grp, "Layer.0.Locked", NULL)
                               ? g_key_file_get_boolean (kf, tab_grp, "Layer.0.Locked", NULL)
                               : FALSE;
          paint_document_layer_set_locks_no_undo (doc, 0, l0_alock, l0_lock);

          for (int li = 1; li < layer_count; li++) {
            g_autofree char *k_file = g_strdup_printf ("Layer.%d.File", li);
            g_autofree char *k_name = g_strdup_printf ("Layer.%d.Name", li);
            g_autofree char *k_vis = g_strdup_printf ("Layer.%d.Visible", li);
            g_autofree char *k_op = g_strdup_printf ("Layer.%d.Opacity", li);
            g_autofree char *k_blend = g_strdup_printf ("Layer.%d.BlendMode", li);
            g_autofree char *k_alock = g_strdup_printf ("Layer.%d.AlphaLock", li);
            g_autofree char *k_lock = g_strdup_printf ("Layer.%d.Locked", li);

            g_autofree char *lfile_name = g_key_file_get_string (kf, tab_grp, k_file, NULL);
            g_autofree char *lname = g_key_file_get_string (kf, tab_grp, k_name, NULL);
            gboolean lvis = g_key_file_has_key (kf, tab_grp, k_vis, NULL)
                              ? g_key_file_get_boolean (kf, tab_grp, k_vis, NULL)
                              : TRUE;
            double lop = g_key_file_has_key (kf, tab_grp, k_op, NULL)
                           ? g_key_file_get_double (kf, tab_grp, k_op, NULL)
                           : 1.0;
            int lblend = g_key_file_has_key (kf, tab_grp, k_blend, NULL)
                           ? g_key_file_get_integer (kf, tab_grp, k_blend, NULL)
                           : PAINT_BLEND_NORMAL;
            gboolean lalock = g_key_file_has_key (kf, tab_grp, k_alock, NULL)
                                ? g_key_file_get_boolean (kf, tab_grp, k_alock, NULL)
                                : FALSE;
            gboolean llock = g_key_file_has_key (kf, tab_grp, k_lock, NULL)
                               ? g_key_file_get_boolean (kf, tab_grp, k_lock, NULL)
                               : FALSE;

            if (lfile_name) {
              g_autofree char *lpath = g_build_filename (dir, lfile_name, NULL);
              g_autoptr (GFile) lfile = g_file_new_for_path (lpath);
              if (paint_document_append_restored_layer (
                      doc,
                      lname ? lname : "Layer",
                      lfile,
                      lvis,
                      lop,
                      (PaintBlendMode) CLAMP (lblend, 0, PAINT_BLEND_LIGHTEN))) {
                paint_document_layer_set_locks_no_undo (
                    doc, paint_document_get_n_layers (doc) - 1, lalock, llock);
              }
            }
          }

          int active_layer = g_key_file_has_key (kf, tab_grp, "ActiveLayer", NULL)
                               ? g_key_file_get_integer (kf, tab_grp, "ActiveLayer", NULL)
                               : 0;
          paint_document_finish_session_restore (doc, active_layer, modified);
        }
      } else if (draft_name) {
        g_autofree char *draft_path = g_build_filename (dir, draft_name, NULL);
        g_autoptr (GFile) draft_file = g_file_new_for_path (draft_path);
        doc = paint_document_new_from_draft (draft_file, orig_file, modified, NULL);
      } else if (orig_file && g_file_query_exists (orig_file, NULL)) {
        doc = paint_document_new_from_file (orig_file, NULL);
      }

      if (doc) {
        AdwTabPage *tab = paint_window_add_new_tab (win, doc);
        GtkWidget *child = adw_tab_page_get_child (tab);
        if (PAINT_IS_CANVAS_PAGE (child))
          paint_canvas_set_zoom (paint_canvas_page_get_canvas (PAINT_CANVAS_PAGE (child)), zoom);
      }
    }

    if (adw_tab_view_get_n_pages (tab_view) == 0) {
      gtk_window_destroy (GTK_WINDOW (win));
      continue;
    }

    int active = g_key_file_get_integer (kf, win_grp, "ActiveTab", NULL);
    int n_pages = adw_tab_view_get_n_pages (tab_view);
    if (active >= 0 && active < n_pages)
      adw_tab_view_set_selected_page (tab_view,
                                      adw_tab_view_get_nth_page (tab_view, active));

    PaintPreferences *prefs = paint_window_get_preferences (win);
    PaintStartupWindowMode mode = prefs ? prefs->startup_window_mode : PAINT_WINDOW_SIZE_REMEMBER;
    gboolean expect_maximize = FALSE;

    switch (mode) {
    case PAINT_WINDOW_SIZE_FIT_CANVAS: {
      int fit_w = PAINT_DEFAULT_WINDOW_WIDTH;
      int fit_h = PAINT_DEFAULT_WINDOW_HEIGHT;
      paint_window_compute_fit_window_size (win, &fit_w, &fit_h);
      gtk_window_set_default_size (GTK_WINDOW (win), fit_w, fit_h);
      break;
    }
    case PAINT_WINDOW_SIZE_DEFAULT:
      gtk_window_set_default_size (GTK_WINDOW (win),
                                   PAINT_DEFAULT_WINDOW_WIDTH,
                                   PAINT_DEFAULT_WINDOW_HEIGHT);
      break;
    case PAINT_WINDOW_SIZE_REMEMBER:
    default: {
      int win_w = g_key_file_get_integer (kf, win_grp, "Width", NULL);
      int win_h = g_key_file_get_integer (kf, win_grp, "Height", NULL);
      if (win_w < 360 || win_h < 300) {
        win_w = prefs ? prefs->last_window_width : PAINT_DEFAULT_WINDOW_WIDTH;
        win_h = prefs ? prefs->last_window_height : PAINT_DEFAULT_WINDOW_HEIGHT;
      }
      if (win_w >= 360 && win_h >= 300)
        gtk_window_set_default_size (GTK_WINDOW (win), win_w, win_h);
      gboolean win_max = g_key_file_has_key (kf, win_grp, "Maximized", NULL)
                           ? g_key_file_get_boolean (kf, win_grp, "Maximized", NULL)
                           : (prefs ? prefs->last_window_maximized : FALSE);
      if (win_max) {
        gtk_window_maximize (GTK_WINDOW (win));
        expect_maximize = TRUE;
      }
      break;
    }
    }

    paint_window_sync_state (win);
    if (prefs && prefs->fit_canvas_on_startup)
      paint_window_schedule_startup_fit (win, expect_maximize);
    gtk_window_present (GTK_WINDOW (win));
    restored_any = TRUE;
  }

  return restored_any;
}

void
paint_window_save_all_sessions (AdwApplication *app)
{
  save_sessions_internal (app, TRUE);
}

static gboolean
on_autosave_timeout (gpointer user_data)
{
  autosave_timer_id = 0;
  GApplication *gapp = g_application_get_default ();
  if (gapp && ADW_IS_APPLICATION (gapp) && gapp == user_data)
    save_sessions_internal (ADW_APPLICATION (gapp), FALSE);
  return G_SOURCE_REMOVE;
}

void
paint_session_schedule_autosave (AdwApplication *app)
{
  if (!app || autosave_timer_id != 0)
    return;
  autosave_timer_id = g_timeout_add_seconds (30, on_autosave_timeout, app);
}

void
paint_session_cancel_autosave (void)
{
  g_clear_handle_id (&autosave_timer_id, g_source_remove);
}
