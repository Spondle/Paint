#include "paint-window.h"

#include <glib/gstdio.h>
#include <math.h>

#include "paint-application.h"
#include "paint-canvas-page.h"
#include "paint-preferences.h"
#include "paint-tools.h"

#define MAX_RECENT_FILES 8

struct _PaintWindow {
  AdwApplicationWindow parent_instance;

  PaintToolSettings *shared_settings;
  GPtrArray *recent_paths; /* element-type: char* */

  AdwTabOverview *tab_overview;
  AdwTabView *tab_view;
  AdwWindowTitle *window_title;
  AdwSplitButton *open_btn;
  GtkButton *zoom_pct_btn;
  GtkLabel *info_dims_label;
  GtkLabel *info_zoom_label;
  GtkLabel *info_bg_label;
  GtkLabel *info_path_label;
};

G_DEFINE_FINAL_TYPE (PaintWindow, paint_window, ADW_TYPE_APPLICATION_WINDOW)

/* Forward declarations */
static void sync_window_state (PaintWindow *self);
static void prompt_open_file (PaintWindow *self);
static void save_current (PaintWindow *self,
                          gboolean save_as,
                          GCallback on_saved,
                          gpointer user_data);

static PaintCanvasPage *
get_active_canvas_page (PaintWindow *self)
{
  AdwTabPage *tab = adw_tab_view_get_selected_page (self->tab_view);
  if (!tab)
    return NULL;
  GtkWidget *child = adw_tab_page_get_child (tab);
  return PAINT_IS_CANVAS_PAGE (child) ? PAINT_CANVAS_PAGE (child) : NULL;
}

static char *
recent_state_path (void)
{
  return g_build_filename (g_get_user_state_dir (), "paint", "recent.ini", NULL);
}

static void
load_recent_files (PaintWindow *self)
{
  g_autofree char *path = recent_state_path ();
  g_autoptr (GKeyFile) kf = g_key_file_new ();
  if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, NULL))
    return;

  gsize len = 0;
  g_auto (GStrv) entries = g_key_file_get_string_list (kf, "Recent", "Paths", &len, NULL);
  if (!entries)
    return;

  for (gsize i = 0; i < len && self->recent_paths->len < MAX_RECENT_FILES; i++) {
    if (g_file_test (entries[i], G_FILE_TEST_EXISTS))
      g_ptr_array_add (self->recent_paths, g_strdup (entries[i]));
  }
}

static void
refresh_recent_menu (PaintWindow *self)
{
  g_autoptr (GMenu) menu = g_menu_new ();
  if (self->recent_paths->len == 0) {
    g_autoptr (GMenuItem) item = g_menu_item_new ("No Recent Images", NULL);
    g_menu_append_item (menu, item);
  } else {
    g_autoptr (GMenu) section = g_menu_new ();
    for (guint i = 0; i < self->recent_paths->len; i++) {
      const char *p = g_ptr_array_index (self->recent_paths, i);
      g_autofree char *base = g_path_get_basename (p);
      g_autoptr (GMenuItem) item = g_menu_item_new (base, NULL);
      g_menu_item_set_action_and_target_value (item,
                                               "win.open-recent",
                                               g_variant_new_string (p));
      g_menu_append_item (section, item);
    }
    g_menu_append_section (menu, "Recent Images", G_MENU_MODEL (section));
  }
  adw_split_button_set_menu_model (self->open_btn, G_MENU_MODEL (menu));
}

static void
record_recent_file (PaintWindow *self, GFile *file)
{
  g_autofree char *path = g_file_get_path (file);
  if (!path)
    return;

  for (guint i = 0; i < self->recent_paths->len; i++) {
    const char *existing = g_ptr_array_index (self->recent_paths, i);
    if (g_strcmp0 (existing, path) == 0) {
      g_ptr_array_remove_index (self->recent_paths, i);
      break;
    }
  }
  g_ptr_array_insert (self->recent_paths, 0, g_strdup (path));
  while (self->recent_paths->len > MAX_RECENT_FILES)
    g_ptr_array_remove_index (self->recent_paths, self->recent_paths->len - 1);

  g_autofree char *state_file = recent_state_path ();
  g_autofree char *dir = g_path_get_dirname (state_file);
  g_mkdir_with_parents (dir, 0755);

  g_autoptr (GKeyFile) kf = g_key_file_new ();
  g_key_file_set_string_list (kf,
                              "Recent",
                              "Paths",
                              (const char *const *) self->recent_paths->pdata,
                              self->recent_paths->len);
  g_key_file_save_to_file (kf, state_file, NULL);
  refresh_recent_menu (self);
}

static void
update_tab_metadata (AdwTabPage *tab_page, PaintCanvasPage *page)
{
  g_autofree char *title = paint_canvas_page_dup_title (page);
  adw_tab_page_set_title (tab_page, title);

  PaintDocument *doc = paint_canvas_get_document (paint_canvas_page_get_canvas (page));
  GFile *file = paint_document_get_file (doc);
  g_autofree char *path = file ? g_file_get_path (file) : g_strdup ("Untitled Canvas");
  adw_tab_page_set_tooltip (tab_page, path ? path : "Untitled Canvas");
}

static void
sync_window_state (PaintWindow *self)
{
  AdwTabPage *tab = adw_tab_view_get_selected_page (self->tab_view);
  PaintCanvasPage *page = get_active_canvas_page (self);
  if (!tab || !page) {
    adw_window_title_set_title (self->window_title, "Paint");
    adw_window_title_set_subtitle (self->window_title, "");
    return;
  }

  paint_canvas_page_sync_controls (page);
  update_tab_metadata (tab, page);

  g_autofree char *title = paint_canvas_page_dup_title (page);
  g_autofree char *subtitle = paint_canvas_page_dup_subtitle (page);
  adw_window_title_set_title (self->window_title, title);
  adw_window_title_set_subtitle (self->window_title, subtitle);

  PaintCanvas *canvas = paint_canvas_page_get_canvas (page);
  PaintDocument *doc = paint_canvas_get_document (canvas);
  double zoom = paint_canvas_get_zoom (canvas);
  int zoom_pct = (int) lround (zoom * 100.0);

  GAction *zoom_in_act = g_action_map_lookup_action (G_ACTION_MAP (self), "zoom-in");
  if (zoom_in_act)
    g_simple_action_set_enabled (G_SIMPLE_ACTION (zoom_in_act),
                                 zoom < paint_canvas_get_max_zoom () - 1e-4);
  GAction *zoom_out_act = g_action_map_lookup_action (G_ACTION_MAP (self), "zoom-out");
  if (zoom_out_act)
    g_simple_action_set_enabled (G_SIMPLE_ACTION (zoom_out_act),
                                 zoom > paint_canvas_get_min_zoom () + 1e-4);
  GAction *crop_act = g_action_map_lookup_action (G_ACTION_MAP (self), "crop-selection");
  if (crop_act)
    g_simple_action_set_enabled (G_SIMPLE_ACTION (crop_act),
                                 paint_canvas_has_selection (canvas));

  g_autofree char *zoom_str = g_strdup_printf ("%d%%", zoom_pct);
  g_autofree char *dims_str = g_strdup_printf ("%d × %d px",
                                               paint_document_get_width (doc),
                                               paint_document_get_height (doc));
  GFile *file = paint_document_get_file (doc);
  g_autofree char *path_str = file ? g_file_get_path (file) : NULL;

  gtk_button_set_label (self->zoom_pct_btn, zoom_str);
  gtk_label_set_text (self->info_dims_label, dims_str);
  gtk_label_set_text (self->info_zoom_label, zoom_str);
  gtk_label_set_text (self->info_bg_label,
                      paint_document_has_transparent_background (doc) ? "Transparent" : "White");
  gtk_label_set_text (self->info_path_label, path_str ? path_str : "Not saved yet");
}

/* ============================================================================
 * File Open / Save
 * ============================================================================ */

static GListModel *
build_image_filters (void)
{
  GListStore *filters = g_list_store_new (GTK_TYPE_FILE_FILTER);

  g_autoptr (GtkFileFilter) img_filter = gtk_file_filter_new ();
  gtk_file_filter_set_name (img_filter, "Image Files (PNG, JPEG, WebP, BMP)");
  gtk_file_filter_add_pattern (img_filter, "*.png");
  gtk_file_filter_add_pattern (img_filter, "*.jpg");
  gtk_file_filter_add_pattern (img_filter, "*.jpeg");
  gtk_file_filter_add_pattern (img_filter, "*.webp");
  gtk_file_filter_add_pattern (img_filter, "*.bmp");
  g_list_store_append (filters, img_filter);

  g_autoptr (GtkFileFilter) all_filter = gtk_file_filter_new ();
  gtk_file_filter_set_name (all_filter, "All Files");
  gtk_file_filter_add_pattern (all_filter, "*");
  g_list_store_append (filters, all_filter);

  return G_LIST_MODEL (filters);
}

static void
on_open_dialog_finish (GObject *source, GAsyncResult *res, gpointer user_data)
{
  PaintWindow *self = PAINT_WINDOW (user_data);
  GtkFileDialog *dialog = GTK_FILE_DIALOG (source);
  g_autoptr (GFile) file = gtk_file_dialog_open_finish (dialog, res, NULL);
  if (file)
    paint_window_open_file (self, file);
}

static void
prompt_open_file (PaintWindow *self)
{
  g_autoptr (GtkFileDialog) dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dialog, "Open Image");
  g_autoptr (GListModel) filters = build_image_filters ();
  gtk_file_dialog_set_filters (dialog, filters);
  gtk_file_dialog_open (dialog, GTK_WINDOW (self), NULL, on_open_dialog_finish, self);
}

void
paint_window_open_file (PaintWindow *self, GFile *file)
{
  g_autoptr (GError) error = NULL;
  PaintDocument *doc = paint_document_new_from_file (file, &error);
  if (!doc) {
    AdwDialog *err = adw_alert_dialog_new ("Could Not Open Image",
                                           error ? error->message : "Unknown error");
    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (err), "ok", "OK");
    adw_dialog_present (err, GTK_WIDGET (self));
    return;
  }

  record_recent_file (self, file);

  PaintCanvasPage *current = get_active_canvas_page (self);
  if (current) {
    PaintCanvas *canvas = paint_canvas_page_get_canvas (current);
    PaintDocument *cur_doc = paint_canvas_get_document (canvas);
    if (!paint_document_get_modified (cur_doc) &&
        !paint_document_get_file (cur_doc) &&
        !paint_document_can_undo (cur_doc)) {
      paint_canvas_set_document (canvas, doc);
      sync_window_state (self);
      return;
    }
  }

  paint_window_add_new_tab (self, doc);
}

typedef struct {
  PaintWindow *window;
  PaintDocument *doc;
  void (*callback) (gboolean ok, gpointer data);
  gpointer callback_data;
} SaveContext;

static void
show_save_error (PaintWindow *self, const GError *error)
{
  AdwDialog *err = adw_alert_dialog_new ("Could Not Save Image",
                                         error ? error->message : "Unknown error");
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (err), "ok", "OK");
  adw_dialog_present (err, GTK_WIDGET (self));
}

static void
on_save_dialog_finish (GObject *source, GAsyncResult *res, gpointer user_data)
{
  SaveContext *ctx = user_data;
  GtkFileDialog *dialog = GTK_FILE_DIALOG (source);
  g_autoptr (GFile) file = gtk_file_dialog_save_finish (dialog, res, NULL);
  gboolean ok = FALSE;

  if (file) {
    g_autoptr (GError) error = NULL;
    if (paint_document_save_to_file (ctx->doc, file, &error)) {
      record_recent_file (ctx->window, paint_document_get_file (ctx->doc));
      sync_window_state (ctx->window);
      ok = TRUE;
    } else {
      show_save_error (ctx->window, error);
    }
  }

  if (ctx->callback)
    ctx->callback (ok, ctx->callback_data);
  g_free (ctx);
}

static void
save_current (PaintWindow *self,
              gboolean save_as,
              GCallback on_saved,
              gpointer user_data)
{
  void (*cb) (gboolean, gpointer) = (void (*) (gboolean, gpointer)) on_saved;
  PaintCanvasPage *page = get_active_canvas_page (self);
  if (!page) {
    if (cb)
      cb (FALSE, user_data);
    return;
  }

  PaintCanvas *canvas = paint_canvas_page_get_canvas (page);
  paint_canvas_commit_active_overlays (canvas);
  PaintDocument *doc = paint_canvas_get_document (canvas);
  GFile *existing = paint_document_get_file (doc);
  if (!save_as && existing) {
    g_autoptr (GError) error = NULL;
    if (paint_document_save_to_file (doc, existing, &error)) {
      record_recent_file (self, paint_document_get_file (doc));
      sync_window_state (self);
      if (cb)
        cb (TRUE, user_data);
    } else {
      show_save_error (self, error);
      if (cb)
        cb (FALSE, user_data);
    }
    return;
  }

  g_autoptr (GtkFileDialog) dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dialog, "Save Image");
  g_autofree char *init_name = existing ? g_file_get_basename (existing) : g_strdup ("Untitled.png");
  gtk_file_dialog_set_initial_name (dialog, init_name);
  g_autoptr (GListModel) filters = build_image_filters ();
  gtk_file_dialog_set_filters (dialog, filters);

  SaveContext *ctx = g_new0 (SaveContext, 1);
  ctx->window = self;
  ctx->doc = doc;
  ctx->callback = cb;
  ctx->callback_data = user_data;

  gtk_file_dialog_save (dialog, GTK_WINDOW (self), NULL, on_save_dialog_finish, ctx);
}

/* ============================================================================
 * Tab Closing & Unsaved Prompts
 * ============================================================================ */

static gboolean
ensure_one_tab_idle (gpointer user_data)
{
  PaintWindow *self = PAINT_WINDOW (user_data);
  if (adw_tab_view_get_n_pages (self->tab_view) == 0 && gtk_widget_get_visible (GTK_WIDGET (self)))
    paint_window_add_new_tab (self, NULL);
  return G_SOURCE_REMOVE;
}

typedef struct {
  PaintWindow *window;
  AdwTabPage *tab_page;
} TabCloseContext;

static void
on_tab_save_before_close (gboolean ok, gpointer user_data)
{
  TabCloseContext *ctx = user_data;
  adw_tab_view_close_page_finish (ctx->window->tab_view, ctx->tab_page, ok);
  if (ok)
    g_idle_add (ensure_one_tab_idle, ctx->window);
  g_free (ctx);
}

static void
on_discard_tab_response (GObject *source, GAsyncResult *res, gpointer user_data)
{
  TabCloseContext *ctx = user_data;
  const char *choice = adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), res);

  if (g_strcmp0 (choice, "discard") == 0) {
    adw_tab_view_close_page_finish (ctx->window->tab_view, ctx->tab_page, TRUE);
    g_idle_add (ensure_one_tab_idle, ctx->window);
    g_free (ctx);
  } else if (g_strcmp0 (choice, "save") == 0) {
    adw_tab_view_set_selected_page (ctx->window->tab_view, ctx->tab_page);
    save_current (ctx->window, FALSE, G_CALLBACK (on_tab_save_before_close), ctx);
  } else {
    adw_tab_view_close_page_finish (ctx->window->tab_view, ctx->tab_page, FALSE);
    g_free (ctx);
  }
}

static AdwDialog *
create_single_save_changes_dialog (PaintDocument *doc)
{
  GFile *file = paint_document_get_file (doc);
  g_autofree char *name = file ? g_file_get_basename (file) : g_strdup ("Untitled Canvas");
  g_autofree char *body = g_strdup_printf (
      "“%s” contains unsaved changes. Changes which are not saved will be permanently lost.",
      name);

  AdwDialog *dlg = adw_alert_dialog_new ("Save Changes?", body);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dlg),
                                  "cancel", "Cancel",
                                  "discard", "Discard",
                                  "save", file ? "Save" : "Save…",
                                  NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dlg),
                                            "discard",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dlg),
                                            "save",
                                            ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dlg), "save");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dlg), "cancel");
  adw_alert_dialog_set_prefer_wide_layout (ADW_ALERT_DIALOG (dlg), TRUE);
  return dlg;
}

static gboolean
on_close_page (AdwTabView *view G_GNUC_UNUSED, AdwTabPage *tab_page, gpointer user_data)
{
  PaintWindow *self = PAINT_WINDOW (user_data);
  GtkWidget *child = adw_tab_page_get_child (tab_page);
  if (PAINT_IS_CANVAS_PAGE (child)) {
    PaintDocument *doc = paint_canvas_get_document (
        paint_canvas_page_get_canvas (PAINT_CANVAS_PAGE (child)));
    if (paint_document_get_modified (doc)) {
      AdwDialog *dlg = create_single_save_changes_dialog (doc);

      TabCloseContext *ctx = g_new0 (TabCloseContext, 1);
      ctx->window = self;
      ctx->tab_page = tab_page;
      adw_alert_dialog_choose (ADW_ALERT_DIALOG (dlg),
                               GTK_WIDGET (self),
                               NULL,
                               on_discard_tab_response,
                               ctx);
      return TRUE;
    }
  }
  g_idle_add (ensure_one_tab_idle, self);
  return FALSE;
}

typedef struct {
  AdwTabPage *tab_page;
  GtkCheckButton *check;
} WindowCloseItem;

typedef struct {
  PaintWindow *window;
  AdwAlertDialog *dialog;
  GPtrArray *items;
  GPtrArray *to_save;
  GPtrArray *to_discard;
  guint save_idx;
} WindowCloseContext;

static void
window_close_item_free (gpointer data)
{
  WindowCloseItem *item = data;
  g_clear_object (&item->tab_page);
  g_free (item);
}

static void
window_close_context_free (WindowCloseContext *ctx)
{
  g_clear_pointer (&ctx->items, g_ptr_array_unref);
  g_clear_pointer (&ctx->to_save, g_ptr_array_unref);
  g_clear_pointer (&ctx->to_discard, g_ptr_array_unref);
  g_free (ctx);
}

static void
mark_tab_page_clean (PaintWindow *self, AdwTabPage *tab_page)
{
  if (adw_tab_view_get_page_position (self->tab_view, tab_page) < 0)
    return;
  GtkWidget *child = adw_tab_page_get_child (tab_page);
  if (!PAINT_IS_CANVAS_PAGE (child))
    return;
  PaintDocument *doc = paint_canvas_get_document (
      paint_canvas_page_get_canvas (PAINT_CANVAS_PAGE (child)));
  paint_document_set_modified (doc, FALSE);
}

static char *
format_folder_for_display (GFile *file)
{
  if (!file)
    return g_strdup ("Not saved yet");

  g_autoptr (GFile) parent = g_file_get_parent (file);
  if (!parent)
    return g_strdup ("Not saved yet");

  g_autofree char *path = g_file_get_path (parent);
  if (!path)
    return g_file_get_uri (parent);

  const char *home = g_get_home_dir ();
  if (home && *home) {
    gsize home_len = strlen (home);
    if (strcmp (path, home) == 0)
      return g_strdup ("~");
    if (strncmp (path, home, home_len) == 0 && path[home_len] == '/')
      return g_strconcat ("~", path + home_len, NULL);
  }
  return g_steal_pointer (&path);
}

static void
on_window_close_check_toggled (GtkCheckButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  WindowCloseContext *ctx = user_data;
  if (!ctx->dialog || !ctx->items)
    return;

  gboolean any_checked = FALSE;
  for (guint i = 0; i < ctx->items->len; i++) {
    WindowCloseItem *item = g_ptr_array_index (ctx->items, i);
    if (item->check && gtk_check_button_get_active (item->check)) {
      any_checked = TRUE;
      break;
    }
  }
  adw_alert_dialog_set_response_enabled (ctx->dialog, "save", any_checked);
}

static void window_close_save_next (WindowCloseContext *ctx);

static void
on_window_save_step_done (gboolean ok, gpointer user_data)
{
  WindowCloseContext *ctx = user_data;
  if (!ok) {
    window_close_context_free (ctx);
    return;
  }
  ctx->save_idx++;
  window_close_save_next (ctx);
}

static void
window_close_save_next (WindowCloseContext *ctx)
{
  while (ctx->save_idx < ctx->to_save->len) {
    AdwTabPage *tab_page = g_ptr_array_index (ctx->to_save, ctx->save_idx);
    if (adw_tab_view_get_page_position (ctx->window->tab_view, tab_page) < 0) {
      ctx->save_idx++;
      continue;
    }
    adw_tab_view_set_selected_page (ctx->window->tab_view, tab_page);
    save_current (ctx->window, FALSE, G_CALLBACK (on_window_save_step_done), ctx);
    return;
  }

  for (guint i = 0; i < ctx->to_discard->len; i++) {
    AdwTabPage *tab_page = g_ptr_array_index (ctx->to_discard, i);
    mark_tab_page_clean (ctx->window, tab_page);
  }
  PaintWindow *win = ctx->window;
  window_close_context_free (ctx);
  gtk_window_close (GTK_WINDOW (win));
}

static void
on_discard_window_response (GObject *source, GAsyncResult *res, gpointer user_data)
{
  WindowCloseContext *ctx = user_data;
  ctx->dialog = NULL;
  const char *choice = adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), res);

  if (g_strcmp0 (choice, "discard") == 0) {
    for (guint i = 0; i < ctx->items->len; i++) {
      WindowCloseItem *item = g_ptr_array_index (ctx->items, i);
      mark_tab_page_clean (ctx->window, item->tab_page);
    }
    PaintWindow *win = ctx->window;
    window_close_context_free (ctx);
    gtk_window_close (GTK_WINDOW (win));
    return;
  }

  if (g_strcmp0 (choice, "save") == 0) {
    ctx->to_save = g_ptr_array_new_with_free_func (g_object_unref);
    ctx->to_discard = g_ptr_array_new_with_free_func (g_object_unref);
    for (guint i = 0; i < ctx->items->len; i++) {
      WindowCloseItem *item = g_ptr_array_index (ctx->items, i);
      if (!item->check || gtk_check_button_get_active (item->check))
        g_ptr_array_add (ctx->to_save, g_object_ref (item->tab_page));
      else
        g_ptr_array_add (ctx->to_discard, g_object_ref (item->tab_page));
    }
    g_clear_pointer (&ctx->items, g_ptr_array_unref);
    ctx->save_idx = 0;
    window_close_save_next (ctx);
    return;
  }

  window_close_context_free (ctx);
}

static PaintPreferences *
get_app_preferences (PaintWindow *self)
{
  GtkApplication *app = self ? gtk_window_get_application (GTK_WINDOW (self)) : NULL;
  if (!app)
    app = GTK_APPLICATION (g_application_get_default ());
  if (app && PAINT_IS_APPLICATION (app))
    return paint_application_get_preferences (PAINT_APPLICATION (app));
  return NULL;
}

static char *
session_dir_path (void)
{
  return g_build_filename (g_get_user_state_dir (), "paint", "session", NULL);
}

void
paint_window_clear_session (void)
{
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

void
paint_window_save_all_sessions (AdwApplication *app)
{
  paint_window_clear_session ();

  g_autofree char *dir = session_dir_path ();
  g_mkdir_with_parents (dir, 0755);

  g_autoptr (GKeyFile) kf = g_key_file_new ();
  int win_idx = 0;

  for (GList *l = gtk_application_get_windows (GTK_APPLICATION (app)); l != NULL; l = l->next) {
    if (!PAINT_IS_WINDOW (l->data))
      continue;
    PaintWindow *win = PAINT_WINDOW (l->data);
    int n_pages = adw_tab_view_get_n_pages (win->tab_view);
    if (n_pages == 0)
      continue;

    g_autofree char *win_grp = g_strdup_printf ("Window.%d", win_idx);
    int w_width = gtk_widget_get_width (GTK_WIDGET (win));
    int w_height = gtk_widget_get_height (GTK_WIDGET (win));
    if (w_width > 0 && w_height > 0) {
      g_key_file_set_integer (kf, win_grp, "Width", w_width);
      g_key_file_set_integer (kf, win_grp, "Height", w_height);
    }

    AdwTabPage *selected = adw_tab_view_get_selected_page (win->tab_view);
    int active_idx = 0;
    int saved_tabs = 0;

    for (int i = 0; i < n_pages; i++) {
      AdwTabPage *tab = adw_tab_view_get_nth_page (win->tab_view, i);
      GtkWidget *child = adw_tab_page_get_child (tab);
      if (!PAINT_IS_CANVAS_PAGE (child))
        continue;
      if (tab == selected)
        active_idx = saved_tabs;

      PaintCanvasPage *page = PAINT_CANVAS_PAGE (child);
      PaintCanvas *canvas = paint_canvas_page_get_canvas (page);
      paint_canvas_commit_active_overlays (canvas);
      PaintDocument *doc = paint_canvas_get_document (canvas);

      g_autofree char *tab_grp = g_strdup_printf ("Tab.%d.%d", win_idx, saved_tabs);
      GFile *orig_file = paint_document_get_file (doc);
      gboolean modified = paint_document_get_modified (doc);

      if (orig_file) {
        g_autofree char *orig_path = g_file_get_path (orig_file);
        if (orig_path)
          g_key_file_set_string (kf, tab_grp, "FilePath", orig_path);
      }

      if (modified || !orig_file) {
        g_autofree char *draft_name = g_strdup_printf ("draft-%d-%d.png", win_idx, saved_tabs);
        g_autofree char *draft_path = g_build_filename (dir, draft_name, NULL);
        if (cairo_surface_write_to_png (paint_document_get_surface (doc), draft_path) == CAIRO_STATUS_SUCCESS)
          g_key_file_set_string (kf, tab_grp, "DraftFile", draft_name);
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
  g_key_file_save_to_file (kf, ini_path, NULL);
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
    int win_w = g_key_file_get_integer (kf, win_grp, "Width", NULL);
    int win_h = g_key_file_get_integer (kf, win_grp, "Height", NULL);
    if (win_w > 200 && win_h > 200)
      gtk_window_set_default_size (GTK_WINDOW (win), win_w, win_h);

    for (int t = 0; t < tab_count; t++) {
      g_autofree char *tab_grp = g_strdup_printf ("Tab.%d.%d", w, t);
      g_autofree char *file_path = g_key_file_get_string (kf, tab_grp, "FilePath", NULL);
      g_autofree char *draft_name = g_key_file_get_string (kf, tab_grp, "DraftFile", NULL);
      gboolean modified = g_key_file_get_boolean (kf, tab_grp, "Modified", NULL);
      double zoom = g_key_file_has_key (kf, tab_grp, "Zoom", NULL)
                      ? g_key_file_get_double (kf, tab_grp, "Zoom", NULL)
                      : 1.0;

      g_autoptr (GFile) orig_file = file_path ? g_file_new_for_path (file_path) : NULL;
      PaintDocument *doc = NULL;
      if (draft_name) {
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

    if (adw_tab_view_get_n_pages (win->tab_view) == 0) {
      gtk_window_destroy (GTK_WINDOW (win));
      continue;
    }

    int active = g_key_file_get_integer (kf, win_grp, "ActiveTab", NULL);
    int n_pages = adw_tab_view_get_n_pages (win->tab_view);
    if (active >= 0 && active < n_pages)
      adw_tab_view_set_selected_page (win->tab_view,
                                      adw_tab_view_get_nth_page (win->tab_view, active));
    sync_window_state (win);
    gtk_window_present (GTK_WINDOW (win));
    restored_any = TRUE;
  }

  return restored_any;
}

static gboolean
on_window_close_request (GtkWindow *window, gpointer user_data G_GNUC_UNUSED)
{
  PaintWindow *self = PAINT_WINDOW (window);
  GtkApplication *app = gtk_window_get_application (window);
  PaintPreferences *prefs = get_app_preferences (self);

  guint open_paint_windows = 0;
  if (app) {
    for (GList *l = gtk_application_get_windows (app); l != NULL; l = l->next) {
      if (PAINT_IS_WINDOW (l->data))
        open_paint_windows++;
    }
  }

  if (prefs && prefs->restore_session && open_paint_windows <= 1) {
    paint_window_save_all_sessions (ADW_APPLICATION (app));
    return FALSE;
  }

  if (!prefs || !prefs->restore_session)
    paint_window_clear_session ();

  g_autoptr (GPtrArray) dirty_tabs = g_ptr_array_new ();
  int n = adw_tab_view_get_n_pages (self->tab_view);
  for (int i = 0; i < n; i++) {
    AdwTabPage *tab = adw_tab_view_get_nth_page (self->tab_view, i);
    GtkWidget *child = adw_tab_page_get_child (tab);
    if (PAINT_IS_CANVAS_PAGE (child)) {
      PaintDocument *doc = paint_canvas_get_document (
          paint_canvas_page_get_canvas (PAINT_CANVAS_PAGE (child)));
      if (paint_document_get_modified (doc))
        g_ptr_array_add (dirty_tabs, tab);
    }
  }

  if (dirty_tabs->len == 0)
    return FALSE;

  WindowCloseContext *ctx = g_new0 (WindowCloseContext, 1);
  ctx->window = self;
  ctx->items = g_ptr_array_new_with_free_func (window_close_item_free);

  AdwDialog *dlg = NULL;
  if (dirty_tabs->len == 1) {
    AdwTabPage *tab = g_ptr_array_index (dirty_tabs, 0);
    PaintDocument *doc = paint_canvas_get_document (
        paint_canvas_page_get_canvas (PAINT_CANVAS_PAGE (adw_tab_page_get_child (tab))));
    dlg = create_single_save_changes_dialog (doc);

    WindowCloseItem *item = g_new0 (WindowCloseItem, 1);
    item->tab_page = g_object_ref (tab);
    g_ptr_array_add (ctx->items, item);
  } else {
    dlg = adw_alert_dialog_new (
        "Save Changes?",
        "Open canvases contain unsaved changes. Changes which are not saved will be permanently lost.");
    adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dlg),
                                    "cancel", "Cancel",
                                    "discard", "Discard All",
                                    "save", "Save",
                                    NULL);
    adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dlg),
                                              "discard",
                                              ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dlg),
                                              "save",
                                              ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dlg), "save");
    adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dlg), "cancel");
    adw_alert_dialog_set_prefer_wide_layout (ADW_ALERT_DIALOG (dlg), TRUE);

    GtkWidget *list = gtk_list_box_new ();
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class (list, "boxed-list");

    for (guint i = 0; i < dirty_tabs->len; i++) {
      AdwTabPage *tab = g_ptr_array_index (dirty_tabs, i);
      PaintDocument *doc = paint_canvas_get_document (
          paint_canvas_page_get_canvas (PAINT_CANVAS_PAGE (adw_tab_page_get_child (tab))));
      GFile *file = paint_document_get_file (doc);
      g_autofree char *raw_title = file ? g_file_get_basename (file)
                                        : g_strdup ("Untitled Canvas (new)");
      g_autofree char *raw_sub = format_folder_for_display (file);
      g_autofree char *title = g_markup_escape_text (raw_title, -1);
      g_autofree char *sub = g_markup_escape_text (raw_sub, -1);

      GtkWidget *row = adw_action_row_new ();
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), sub);

      GtkWidget *check = gtk_check_button_new ();
      gtk_check_button_set_active (GTK_CHECK_BUTTON (check), TRUE);
      gtk_widget_set_valign (check, GTK_ALIGN_CENTER);
      adw_action_row_add_prefix (ADW_ACTION_ROW (row), check);
      adw_action_row_set_activatable_widget (ADW_ACTION_ROW (row), check);
      gtk_list_box_append (GTK_LIST_BOX (list), row);

      WindowCloseItem *item = g_new0 (WindowCloseItem, 1);
      item->tab_page = g_object_ref (tab);
      item->check = GTK_CHECK_BUTTON (check);
      g_ptr_array_add (ctx->items, item);

      g_signal_connect (check, "toggled", G_CALLBACK (on_window_close_check_toggled), ctx);
    }

    adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dlg), list);
  }

  ctx->dialog = ADW_ALERT_DIALOG (dlg);
  adw_alert_dialog_choose (ADW_ALERT_DIALOG (dlg),
                           GTK_WIDGET (self),
                           NULL,
                           on_discard_window_response,
                           ctx);
  return TRUE;
}

/* ============================================================================
 * Clear & Resize Dialogs
 * ============================================================================ */

static void
on_clear_dialog_response (GObject *source, GAsyncResult *res, gpointer user_data)
{
  PaintWindow *self = PAINT_WINDOW (user_data);
  if (g_strcmp0 (adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), res), "clear") == 0) {
    PaintCanvasPage *page = get_active_canvas_page (self);
    if (page)
      paint_canvas_clear (paint_canvas_page_get_canvas (page));
  }
}

static void
confirm_clear_canvas (PaintWindow *self)
{
  PaintCanvasPage *page = get_active_canvas_page (self);
  if (!page)
    return;
  PaintDocument *doc = paint_canvas_get_document (paint_canvas_page_get_canvas (page));
  const char *body = paint_document_has_transparent_background (doc)
                       ? "This will clear the entire canvas to transparent. You can still undo this action."
                       : "This will fill the entire canvas with the background color. You can still undo this action.";
  AdwDialog *dlg = adw_alert_dialog_new ("Clear Canvas?", body);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dlg),
                                  "cancel", "Cancel",
                                  "clear", "Clear",
                                  NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dlg),
                                            "clear",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dlg), "cancel");
  adw_alert_dialog_choose (ADW_ALERT_DIALOG (dlg),
                           GTK_WIDGET (self),
                           NULL,
                           on_clear_dialog_response,
                           self);
}

typedef struct {
  PaintCanvas *canvas;
  AdwDialog *dialog;
  AdwComboRow *mode_row;
  AdwSwitchRow *aspect_row;
  AdwSpinRow *w_row;
  AdwSpinRow *h_row;
  GtkWidget *apply_btn;
  double aspect_ratio;
  gboolean updating;
} ResizeContext;

static void
on_resize_mode_changed (GObject *obj G_GNUC_UNUSED,
                        GParamSpec *pspec G_GNUC_UNUSED,
                        gpointer user_data)
{
  ResizeContext *ctx = user_data;
  guint mode = adw_combo_row_get_selected (ctx->mode_row);
  if (ctx->apply_btn)
    gtk_button_set_label (GTK_BUTTON (ctx->apply_btn), (mode == 1) ? "Scale" : "Resize");
  if (mode == 1 && !adw_switch_row_get_active (ctx->aspect_row)) {
    ctx->updating = TRUE;
    adw_switch_row_set_active (ctx->aspect_row, TRUE);
    ctx->updating = FALSE;
  }
}

static void
on_resize_width_changed (GObject *obj G_GNUC_UNUSED,
                         GParamSpec *pspec G_GNUC_UNUSED,
                         gpointer user_data)
{
  ResizeContext *ctx = user_data;
  if (ctx->updating || !adw_switch_row_get_active (ctx->aspect_row))
    return;
  ctx->updating = TRUE;
  double w = adw_spin_row_get_value (ctx->w_row);
  double h = CLAMP (round (w / ctx->aspect_ratio), 1.0, 8192.0);
  adw_spin_row_set_value (ctx->h_row, h);
  ctx->updating = FALSE;
}

static void
on_resize_height_changed (GObject *obj G_GNUC_UNUSED,
                          GParamSpec *pspec G_GNUC_UNUSED,
                          gpointer user_data)
{
  ResizeContext *ctx = user_data;
  if (ctx->updating || !adw_switch_row_get_active (ctx->aspect_row))
    return;
  ctx->updating = TRUE;
  double h = adw_spin_row_get_value (ctx->h_row);
  double w = CLAMP (round (h * ctx->aspect_ratio), 1.0, 8192.0);
  adw_spin_row_set_value (ctx->w_row, w);
  ctx->updating = FALSE;
}

static void
on_resize_preset_clicked (GtkButton *btn, gpointer user_data)
{
  ResizeContext *ctx = user_data;
  int w = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (btn), "preset-w"));
  int h = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (btn), "preset-h"));
  ctx->updating = TRUE;
  adw_spin_row_set_value (ctx->w_row, (double) w);
  adw_spin_row_set_value (ctx->h_row, (double) h);
  ctx->updating = FALSE;
}

static void
on_resize_apply (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  ResizeContext *ctx = user_data;
  int w = (int) lround (adw_spin_row_get_value (ctx->w_row));
  int h = (int) lround (adw_spin_row_get_value (ctx->h_row));
  if (adw_combo_row_get_selected (ctx->mode_row) == 1)
    paint_canvas_scale (ctx->canvas, w, h);
  else
    paint_canvas_resize (ctx->canvas, w, h);
  adw_dialog_close (ctx->dialog);
}

static void
show_resize_dialog (PaintWindow *self)
{
  PaintCanvasPage *page = get_active_canvas_page (self);
  if (!page)
    return;
  PaintCanvas *canvas = paint_canvas_page_get_canvas (page);
  PaintDocument *doc = paint_canvas_get_document (canvas);
  int orig_w = paint_document_get_width (doc);
  int orig_h = paint_document_get_height (doc);

  AdwDialog *dialog = adw_dialog_new ();
  adw_dialog_set_title (dialog, "Resize & Scale");
  adw_dialog_set_content_width (dialog, 360);

  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_show_start_title_buttons (ADW_HEADER_BAR (header), FALSE);
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);

  GtkWidget *cancel_btn = gtk_button_new_with_label ("Cancel");
  g_signal_connect_swapped (cancel_btn, "clicked", G_CALLBACK (adw_dialog_close), dialog);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), cancel_btn);

  GtkWidget *apply_btn = gtk_button_new_with_label ("Resize");
  gtk_widget_add_css_class (apply_btn, "suggested-action");
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), apply_btn);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 14);
  gtk_widget_set_margin_top (content, 16);
  gtk_widget_set_margin_bottom (content, 16);
  gtk_widget_set_margin_start (content, 16);
  gtk_widget_set_margin_end (content, 16);

  GtkWidget *group = adw_preferences_group_new ();

  static const char *const MODES[] = { "Crop / Pad Canvas", "Scale Image", NULL };
  GtkWidget *mode_row = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (mode_row), "Mode");
  g_autoptr (GtkStringList) mode_model = gtk_string_list_new (MODES);
  adw_combo_row_set_model (ADW_COMBO_ROW (mode_row), G_LIST_MODEL (mode_model));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), mode_row);

  GtkWidget *aspect_row = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (aspect_row), "Keep Aspect Ratio");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), aspect_row);

  GtkWidget *w_row = adw_spin_row_new_with_range (1.0, 8192.0, 16.0);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (w_row), "Width (px)");
  adw_spin_row_set_value (ADW_SPIN_ROW (w_row), (double) orig_w);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), w_row);

  GtkWidget *h_row = adw_spin_row_new_with_range (1.0, 8192.0, 16.0);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (h_row), "Height (px)");
  adw_spin_row_set_value (ADW_SPIN_ROW (h_row), (double) orig_h);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), h_row);
  gtk_box_append (GTK_BOX (content), group);

  ResizeContext *ctx = g_new0 (ResizeContext, 1);
  ctx->canvas = canvas;
  ctx->dialog = dialog;
  ctx->mode_row = ADW_COMBO_ROW (mode_row);
  ctx->aspect_row = ADW_SWITCH_ROW (aspect_row);
  ctx->w_row = ADW_SPIN_ROW (w_row);
  ctx->h_row = ADW_SPIN_ROW (h_row);
  ctx->apply_btn = apply_btn;
  ctx->aspect_ratio = (double) MAX (1, orig_w) / (double) MAX (1, orig_h);
  g_object_set_data_full (G_OBJECT (dialog), "resize-ctx", ctx, g_free);

  g_signal_connect (mode_row, "notify::selected", G_CALLBACK (on_resize_mode_changed), ctx);
  g_signal_connect (w_row, "notify::value", G_CALLBACK (on_resize_width_changed), ctx);
  g_signal_connect (h_row, "notify::value", G_CALLBACK (on_resize_height_changed), ctx);

  static const int PRESETS[][2] = {
    { PAINT_DEFAULT_CANVAS_WIDTH, PAINT_DEFAULT_CANVAS_HEIGHT },
    { 800, 500 },
    { 1024, 1024 },
    { 1280, 720 },
  };
  GtkWidget *presets = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign (presets, GTK_ALIGN_CENTER);
  for (gsize i = 0; i < G_N_ELEMENTS (PRESETS); i++) {
    g_autofree char *lbl = g_strdup_printf ("%d×%d", PRESETS[i][0], PRESETS[i][1]);
    GtkWidget *pbtn = gtk_button_new_with_label (lbl);
    gtk_widget_add_css_class (pbtn, "pill");
    g_object_set_data (G_OBJECT (pbtn), "preset-w", GINT_TO_POINTER (PRESETS[i][0]));
    g_object_set_data (G_OBJECT (pbtn), "preset-h", GINT_TO_POINTER (PRESETS[i][1]));
    g_signal_connect (pbtn, "clicked", G_CALLBACK (on_resize_preset_clicked), ctx);
    gtk_box_append (GTK_BOX (presets), pbtn);
  }
  gtk_box_append (GTK_BOX (content), presets);

  g_signal_connect (apply_btn, "clicked", G_CALLBACK (on_resize_apply), ctx);

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), content);
  adw_dialog_set_child (dialog, toolbar);
  adw_dialog_present (dialog, GTK_WIDGET (self));
}

/* ============================================================================
 * Actions & Shortcuts
 * ============================================================================ */

static void act_new_tab (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  paint_window_add_new_tab (PAINT_WINDOW (d), NULL);
}

static void act_close_tab (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintWindow *self = PAINT_WINDOW (d);
  AdwTabPage *tab = adw_tab_view_get_selected_page (self->tab_view);
  if (tab)
    adw_tab_view_close_page (self->tab_view, tab);
}

static void act_open (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  prompt_open_file (PAINT_WINDOW (d));
}

static void act_open_recent (GSimpleAction *a G_GNUC_UNUSED, GVariant *param, gpointer d)
{
  if (param) {
    g_autoptr (GFile) file = g_file_new_for_path (g_variant_get_string (param, NULL));
    paint_window_open_file (PAINT_WINDOW (d), file);
  }
}

static void act_save (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  save_current (PAINT_WINDOW (d), FALSE, NULL, NULL);
}

static void act_save_as (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  save_current (PAINT_WINDOW (d), TRUE, NULL, NULL);
}

static void act_undo (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (page)
    paint_canvas_undo (paint_canvas_page_get_canvas (page));
}

static void act_redo (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (page)
    paint_canvas_redo (paint_canvas_page_get_canvas (page));
}

static void act_clear (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  confirm_clear_canvas (PAINT_WINDOW (d));
}

static void act_resize (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  show_resize_dialog (PAINT_WINDOW (d));
}

static gboolean
forward_editable_action (PaintWindow *self, const char *action_name)
{
  GtkWidget *focus = gtk_window_get_focus (GTK_WINDOW (self));
  if (focus && (GTK_IS_EDITABLE (focus) || GTK_IS_TEXT (focus))) {
    gtk_widget_activate_action (focus, action_name, NULL);
    return TRUE;
  }
  return FALSE;
}

static void act_copy (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintWindow *self = PAINT_WINDOW (d);
  if (forward_editable_action (self, "clipboard.copy"))
    return;
  PaintCanvasPage *page = get_active_canvas_page (self);
  if (!page)
    return;
  g_autoptr (GdkTexture) tex = paint_canvas_copy_selection (paint_canvas_page_get_canvas (page));
  if (tex) {
    GdkClipboard *cb = gtk_widget_get_clipboard (GTK_WIDGET (self));
    gdk_clipboard_set_texture (cb, tex);
  }
}

static void act_cut (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintWindow *self = PAINT_WINDOW (d);
  if (forward_editable_action (self, "clipboard.cut"))
    return;
  PaintCanvasPage *page = get_active_canvas_page (self);
  if (!page)
    return;
  g_autoptr (GdkTexture) tex = paint_canvas_cut_selection (paint_canvas_page_get_canvas (page));
  if (tex) {
    GdkClipboard *cb = gtk_widget_get_clipboard (GTK_WIDGET (self));
    gdk_clipboard_set_texture (cb, tex);
  }
}

typedef struct {
  PaintWindow *window;
  gboolean new_tab;
} PasteCtx;

static void
on_clipboard_read_texture_finish (GObject *source, GAsyncResult *res, gpointer user_data)
{
  PasteCtx *ctx = user_data;
  g_autoptr (GdkTexture) tex = gdk_clipboard_read_texture_finish (GDK_CLIPBOARD (source), res, NULL);
  if (tex) {
    if (ctx->new_tab) {
      PaintDocument *doc = paint_document_new_from_texture (tex);
      if (doc)
        paint_window_add_new_tab (ctx->window, doc);
    } else {
      PaintCanvasPage *page = get_active_canvas_page (ctx->window);
      if (page) {
        paint_canvas_page_select_tool (page, "select");
        paint_canvas_paste_texture (paint_canvas_page_get_canvas (page), tex);
      }
    }
  }
  g_object_unref (ctx->window);
  g_free (ctx);
}

static void
request_clipboard_paste (PaintWindow *self, gboolean new_tab)
{
  GdkClipboard *cb = gtk_widget_get_clipboard (GTK_WIDGET (self));
  PasteCtx *ctx = g_new0 (PasteCtx, 1);
  ctx->window = g_object_ref (self);
  ctx->new_tab = new_tab;
  gdk_clipboard_read_texture_async (cb, NULL, on_clipboard_read_texture_finish, ctx);
}

static void act_paste (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintWindow *self = PAINT_WINDOW (d);
  if (forward_editable_action (self, "clipboard.paste"))
    return;
  request_clipboard_paste (self, FALSE);
}

static void act_paste_new_tab (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  request_clipboard_paste (PAINT_WINDOW (d), TRUE);
}

static void act_select_all (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintWindow *self = PAINT_WINDOW (d);
  if (forward_editable_action (self, "selection.select-all"))
    return;
  PaintCanvasPage *page = get_active_canvas_page (self);
  if (page) {
    paint_canvas_page_select_tool (page, "select");
    paint_canvas_select_all (paint_canvas_page_get_canvas (page));
  }
}

static void act_delete_selection (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (page)
    paint_canvas_delete_selection (paint_canvas_page_get_canvas (page));
}

static void act_crop_selection (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (page)
    paint_canvas_crop_to_selection (paint_canvas_page_get_canvas (page));
}

static void act_rotate_cw (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (page)
    paint_canvas_rotate_90 (paint_canvas_page_get_canvas (page), TRUE);
}

static void act_rotate_ccw (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (page)
    paint_canvas_rotate_90 (paint_canvas_page_get_canvas (page), FALSE);
}

static void act_flip_horizontal (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (page)
    paint_canvas_flip (paint_canvas_page_get_canvas (page), TRUE);
}

static void act_flip_vertical (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (page)
    paint_canvas_flip (paint_canvas_page_get_canvas (page), FALSE);
}

static void act_zoom_fit (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (page)
    paint_canvas_page_zoom_fit (page);
}

static void act_toggle_pixel_grid_state (GSimpleAction *action, GVariant *value, gpointer d)
{
  PaintWindow *self = PAINT_WINDOW (d);
  gboolean show = g_variant_get_boolean (value);
  g_simple_action_set_state (action, value);

  PaintPreferences *prefs = get_app_preferences (self);
  if (prefs) {
    prefs->show_pixel_grid = show;
    paint_preferences_save (prefs);
    GtkApplication *app = gtk_window_get_application (GTK_WINDOW (self));
    if (app) {
      for (GList *l = gtk_application_get_windows (app); l != NULL; l = l->next) {
        if (PAINT_IS_WINDOW (l->data))
          paint_window_apply_preferences (PAINT_WINDOW (l->data));
      }
      return;
    }
  }
  paint_window_apply_preferences (self);
}

static void act_zoom_in (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (page)
    paint_canvas_zoom_in (paint_canvas_page_get_canvas (page));
}

static void act_zoom_out (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (page)
    paint_canvas_zoom_out (paint_canvas_page_get_canvas (page));
}

static void act_zoom_reset (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (page)
    paint_canvas_zoom_reset (paint_canvas_page_get_canvas (page));
}

static void act_toggle_toolbar (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (page)
    paint_canvas_page_toggle_osd (page);
}

static void act_toggle_fullscreen (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  GtkWindow *win = GTK_WINDOW (d);
  if (gtk_window_is_fullscreen (win))
    gtk_window_unfullscreen (win);
  else
    gtk_window_fullscreen (win);
}

static void act_tab_overview (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintWindow *self = PAINT_WINDOW (d);
  adw_tab_overview_set_open (self->tab_overview, !adw_tab_overview_get_open (self->tab_overview));
}

static void act_tool_select (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (page)
    paint_canvas_page_select_tool (page, "select");
}

static void act_tool_brush (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (page)
    paint_canvas_page_select_tool (page, "brush");
}

static void act_tool_line (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (page)
    paint_canvas_page_select_tool (page, "line");
}

static void act_tool_eraser (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (page)
    paint_canvas_page_select_tool (page, "eraser");
}

static void act_tool_fill (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (page)
    paint_canvas_page_select_tool (page, "fill");
}

static void act_tool_text (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (page)
    paint_canvas_page_select_tool (page, "text");
}

static void act_tool_eyedropper (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (page)
    paint_canvas_page_select_tool (page, "eyedropper");
}

static void act_tool_transparent (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (page)
    paint_canvas_page_select_last_transparency (page);
}

static void act_tool_shape (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (page)
    paint_canvas_page_select_last_shape (page);
}

static void act_brush_smaller (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (page)
    paint_canvas_page_adjust_brush_size (page, -2.0);
}

static void act_brush_larger (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (page)
    paint_canvas_page_adjust_brush_size (page, 2.0);
}

static gboolean
on_key_pressed (GtkEventControllerKey *ctrl G_GNUC_UNUSED,
                guint keyval,
                guint keycode G_GNUC_UNUSED,
                GdkModifierType state,
                gpointer user_data)
{
  PaintWindow *self = PAINT_WINDOW (user_data);
  if ((state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) != 0)
    return FALSE;

  GtkWidget *focus = gtk_window_get_focus (GTK_WINDOW (self));
  if (focus && (GTK_IS_EDITABLE (focus) || GTK_IS_TEXT (focus)))
    return FALSE;

  PaintCanvasPage *page = get_active_canvas_page (self);
  PaintCanvas *canvas = page ? paint_canvas_page_get_canvas (page) : NULL;

  if (keyval == GDK_KEY_Escape) {
    if (canvas && paint_canvas_has_active_text (canvas)) {
      paint_canvas_cancel_text (canvas);
      return TRUE;
    }
    if (canvas && paint_canvas_has_selection (canvas)) {
      paint_canvas_cancel_selection (canvas);
      return TRUE;
    }
    if (gtk_window_is_fullscreen (GTK_WINDOW (self))) {
      gtk_window_unfullscreen (GTK_WINDOW (self));
      return TRUE;
    }
    return FALSE;
  }

  if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
    if (canvas && (paint_canvas_has_active_text (canvas) || paint_canvas_has_selection (canvas))) {
      paint_canvas_commit_active_overlays (canvas);
      return TRUE;
    }
    return FALSE;
  }

  if ((keyval == GDK_KEY_Delete || keyval == GDK_KEY_BackSpace) &&
      canvas && paint_canvas_has_selection (canvas)) {
    paint_canvas_delete_selection (canvas);
    return TRUE;
  }

  if (canvas && paint_canvas_has_selection (canvas)) {
    int step = (state & GDK_SHIFT_MASK) ? 10 : 1;
    if (keyval == GDK_KEY_Left) {
      paint_canvas_nudge_selection (canvas, -step, 0);
      return TRUE;
    }
    if (keyval == GDK_KEY_Right) {
      paint_canvas_nudge_selection (canvas, step, 0);
      return TRUE;
    }
    if (keyval == GDK_KEY_Up) {
      paint_canvas_nudge_selection (canvas, 0, -step);
      return TRUE;
    }
    if (keyval == GDK_KEY_Down) {
      paint_canvas_nudge_selection (canvas, 0, step);
      return TRUE;
    }
  }

  if ((state & GDK_SHIFT_MASK) != 0)
    return FALSE;

  if (keyval == GDK_KEY_m || keyval == GDK_KEY_M) {
    gtk_widget_activate_action (GTK_WIDGET (self), "win.tool-select", NULL);
    return TRUE;
  }
  if (keyval == GDK_KEY_b || keyval == GDK_KEY_B) {
    gtk_widget_activate_action (GTK_WIDGET (self), "win.tool-brush", NULL);
    return TRUE;
  }
  if (keyval == GDK_KEY_l || keyval == GDK_KEY_L) {
    gtk_widget_activate_action (GTK_WIDGET (self), "win.tool-line", NULL);
    return TRUE;
  }
  if (keyval == GDK_KEY_e || keyval == GDK_KEY_E) {
    gtk_widget_activate_action (GTK_WIDGET (self), "win.tool-eraser", NULL);
    return TRUE;
  }
  if (keyval == GDK_KEY_f || keyval == GDK_KEY_F) {
    gtk_widget_activate_action (GTK_WIDGET (self), "win.tool-fill", NULL);
    return TRUE;
  }
  if (keyval == GDK_KEY_x || keyval == GDK_KEY_X) {
    gtk_widget_activate_action (GTK_WIDGET (self), "win.tool-text", NULL);
    return TRUE;
  }
  if (keyval == GDK_KEY_i || keyval == GDK_KEY_I) {
    gtk_widget_activate_action (GTK_WIDGET (self), "win.tool-eyedropper", NULL);
    return TRUE;
  }
  if (keyval == GDK_KEY_t || keyval == GDK_KEY_T) {
    gtk_widget_activate_action (GTK_WIDGET (self), "win.tool-transparent", NULL);
    return TRUE;
  }
  if (keyval == GDK_KEY_s || keyval == GDK_KEY_S) {
    gtk_widget_activate_action (GTK_WIDGET (self), "win.tool-shape", NULL);
    return TRUE;
  }
  if (keyval == GDK_KEY_bracketleft) {
    gtk_widget_activate_action (GTK_WIDGET (self), "win.brush-smaller", NULL);
    return TRUE;
  }
  if (keyval == GDK_KEY_bracketright) {
    gtk_widget_activate_action (GTK_WIDGET (self), "win.brush-larger", NULL);
    return TRUE;
  }
  return FALSE;
}

/* ============================================================================
 * UI Construction
 * ============================================================================ */

static GtkWidget *
build_zoom_row (PaintWindow *self)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class (row, "paint-menu-zoom-row");
  gtk_widget_set_halign (row, GTK_ALIGN_CENTER);

  GtkWidget *zoom_out = gtk_button_new_from_icon_name ("zoom-out-symbolic");
  gtk_widget_add_css_class (zoom_out, "circular");
  gtk_widget_add_css_class (zoom_out, "flat");
  gtk_widget_set_tooltip_text (zoom_out, "Zoom Out (Ctrl+-)");
  gtk_actionable_set_action_name (GTK_ACTIONABLE (zoom_out), "win.zoom-out");

  GtkWidget *pct = gtk_button_new_with_label ("100%");
  gtk_widget_add_css_class (pct, "flat");
  gtk_widget_add_css_class (pct, "paint-zoom-label");
  gtk_widget_set_tooltip_text (pct, "Reset Zoom (Ctrl+0)");
  gtk_actionable_set_action_name (GTK_ACTIONABLE (pct), "win.zoom-reset");
  self->zoom_pct_btn = GTK_BUTTON (pct);

  GtkWidget *zoom_in = gtk_button_new_from_icon_name ("zoom-in-symbolic");
  gtk_widget_add_css_class (zoom_in, "circular");
  gtk_widget_add_css_class (zoom_in, "flat");
  gtk_widget_set_tooltip_text (zoom_in, "Zoom In (Ctrl++)");
  gtk_actionable_set_action_name (GTK_ACTIONABLE (zoom_in), "win.zoom-in");

  GtkWidget *zoom_fit = gtk_button_new_from_icon_name ("zoom-fit-best-symbolic");
  gtk_widget_add_css_class (zoom_fit, "circular");
  gtk_widget_add_css_class (zoom_fit, "flat");
  gtk_widget_set_tooltip_text (zoom_fit, "Fit to Window (Ctrl+9)");
  gtk_actionable_set_action_name (GTK_ACTIONABLE (zoom_fit), "win.zoom-fit");

  gtk_box_append (GTK_BOX (row), zoom_out);
  gtk_box_append (GTK_BOX (row), pct);
  gtk_box_append (GTK_BOX (row), zoom_in);
  gtk_box_append (GTK_BOX (row), zoom_fit);
  return row;
}

static GtkWidget *
build_primary_menu (PaintWindow *self)
{
  g_autoptr (GMenu) menu = g_menu_new ();

  GApplication *app = g_application_get_default ();
  PaintPreferences *prefs = (app && PAINT_IS_APPLICATION (app))
                              ? paint_application_get_preferences (PAINT_APPLICATION (app))
                              : NULL;
  if (prefs) {
    g_autoptr (GMenu) theme_section = g_menu_new ();
    g_autoptr (GMenuItem) theme_item = g_menu_item_new (NULL, NULL);
    g_menu_item_set_attribute (theme_item, "custom", "s", "theme_selector");
    g_menu_append_item (theme_section, theme_item);
    g_menu_append_section (menu, NULL, G_MENU_MODEL (theme_section));
  }

  g_autoptr (GMenu) zoom_section = g_menu_new ();
  g_autoptr (GMenuItem) zoom_item = g_menu_item_new (NULL, NULL);
  g_menu_item_set_attribute (zoom_item, "custom", "s", "zoom_controls");
  g_menu_append_item (zoom_section, zoom_item);
  g_menu_append_section (menu, NULL, G_MENU_MODEL (zoom_section));

  g_autoptr (GMenu) file_section = g_menu_new ();
  g_menu_append (file_section, "New Window", "app.new-window");
  g_menu_append (file_section, "Save", "win.save");
  g_menu_append (file_section, "Save As…", "win.save-as");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (file_section));

  g_autoptr (GMenu) edit_section = g_menu_new ();
  g_menu_append (edit_section, "Copy", "win.copy");
  g_menu_append (edit_section, "Cut", "win.cut");
  g_menu_append (edit_section, "Paste", "win.paste");
  g_menu_append (edit_section, "Paste as New Canvas", "win.paste-new-tab");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (edit_section));

  g_autoptr (GMenu) canvas_section = g_menu_new ();
  g_menu_append (canvas_section, "Resize & Scale…", "win.resize-canvas");
  g_autoptr (GMenu) transform_sub = g_menu_new ();
  g_menu_append (transform_sub, "Rotate Right 90°", "win.rotate-cw");
  g_menu_append (transform_sub, "Rotate Left 90°", "win.rotate-ccw");
  g_menu_append (transform_sub, "Flip Horizontal", "win.flip-horizontal");
  g_menu_append (transform_sub, "Flip Vertical", "win.flip-vertical");
  g_menu_append (transform_sub, "Crop to Selection", "win.crop-selection");
  g_menu_append_submenu (canvas_section, "Transform", G_MENU_MODEL (transform_sub));
  g_menu_append (canvas_section, "Clear Canvas", "win.clear");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (canvas_section));

  g_autoptr (GMenu) view_section = g_menu_new ();
  g_menu_append (view_section, "Pixel Grid", "win.toggle-pixel-grid");
  g_menu_append (view_section, "Fullscreen", "win.toggle-fullscreen");
  g_menu_append (view_section, "Toggle Toolbar", "win.toggle-toolbar");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (view_section));

  g_autoptr (GMenu) app_section = g_menu_new ();
  g_menu_append (app_section, "Preferences", "app.preferences");
  g_menu_append (app_section, "Keyboard Shortcuts", "app.shortcuts");
  g_menu_append (app_section, "About Paint", "app.about");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (app_section));

  GtkWidget *popover = gtk_popover_menu_new_from_model (G_MENU_MODEL (menu));
  if (prefs) {
    gtk_popover_menu_add_child (GTK_POPOVER_MENU (popover),
                                paint_theme_selector_new (prefs, NULL, NULL),
                                "theme_selector");
  }
  gtk_popover_menu_add_child (GTK_POPOVER_MENU (popover), build_zoom_row (self), "zoom_controls");
  return popover;
}

static GtkLabel *
add_info_row (GtkGrid *grid, int row_idx, const char *key)
{
  GtkWidget *k_lbl = gtk_label_new (key);
  gtk_widget_add_css_class (k_lbl, "paint-dim-label");
  gtk_widget_set_halign (k_lbl, GTK_ALIGN_START);

  GtkWidget *v_lbl = gtk_label_new ("—");
  gtk_widget_set_halign (v_lbl, GTK_ALIGN_END);
  gtk_widget_set_hexpand (v_lbl, TRUE);
  gtk_label_set_selectable (GTK_LABEL (v_lbl), TRUE);

  gtk_grid_attach (grid, k_lbl, 0, row_idx, 1, 1);
  gtk_grid_attach (grid, v_lbl, 1, row_idx, 1, 1);
  return GTK_LABEL (v_lbl);
}

static GtkWidget *
build_info_popover (PaintWindow *self)
{
  GtkWidget *pop = gtk_popover_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_add_css_class (box, "paint-info-grid");
  gtk_widget_set_margin_top (box, 10);
  gtk_widget_set_margin_bottom (box, 10);
  gtk_widget_set_margin_start (box, 12);
  gtk_widget_set_margin_end (box, 12);

  GtkWidget *heading = gtk_label_new ("Canvas Properties");
  gtk_widget_add_css_class (heading, "heading");
  gtk_widget_set_halign (heading, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (box), heading);

  GtkWidget *grid = gtk_grid_new ();
  gtk_grid_set_column_spacing (GTK_GRID (grid), 18);
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);

  self->info_dims_label = add_info_row (GTK_GRID (grid), 0, "Dimensions");
  self->info_zoom_label = add_info_row (GTK_GRID (grid), 1, "Zoom");
  self->info_bg_label = add_info_row (GTK_GRID (grid), 2, "Background");
  self->info_path_label = add_info_row (GTK_GRID (grid), 3, "Location");
  gtk_label_set_max_width_chars (self->info_path_label, 28);
  gtk_label_set_ellipsize (self->info_path_label, PANGO_ELLIPSIZE_END);
  gtk_box_append (GTK_BOX (box), grid);

  GtkWidget *resize_btn = gtk_button_new_with_label ("Resize & Scale…");
  gtk_actionable_set_action_name (GTK_ACTIONABLE (resize_btn), "win.resize-canvas");
  g_signal_connect_swapped (resize_btn, "clicked", G_CALLBACK (gtk_popover_popdown), pop);
  gtk_box_append (GTK_BOX (box), resize_btn);

  gtk_popover_set_child (GTK_POPOVER (pop), box);
  return pop;
}

static GtkWidget *
build_header_bar (PaintWindow *self)
{
  GtkWidget *header = adw_header_bar_new ();
  self->window_title = ADW_WINDOW_TITLE (adw_window_title_new ("Untitled Canvas", "640 × 380 · 100%"));
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), GTK_WIDGET (self->window_title));

  /* Start: Open split button + New Tab button (Text Editor / Ptyxis style) */
  self->open_btn = ADW_SPLIT_BUTTON (adw_split_button_new ());
  adw_split_button_set_label (self->open_btn, "Open");
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->open_btn), "Open Image (Ctrl+O)");
  adw_split_button_set_dropdown_tooltip (self->open_btn, "Recent Images");
  g_signal_connect_swapped (self->open_btn, "clicked", G_CALLBACK (prompt_open_file), self);
  refresh_recent_menu (self);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), GTK_WIDGET (self->open_btn));

  GtkWidget *new_btn = gtk_button_new_from_icon_name ("tab-new-symbolic");
  gtk_widget_set_tooltip_text (new_btn, "New Canvas (Ctrl+N)");
  gtk_actionable_set_action_name (GTK_ACTIONABLE (new_btn), "win.new-tab");
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), new_btn);

  /* End: Primary Menu, Tab Overview grid, Info popover */
  GtkWidget *menu_btn = gtk_menu_button_new ();
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (menu_btn), "open-menu-symbolic");
  gtk_menu_button_set_primary (GTK_MENU_BUTTON (menu_btn), TRUE);
  gtk_widget_set_tooltip_text (menu_btn, "Main Menu (F10)");
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (menu_btn), build_primary_menu (self));
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), menu_btn);

  GtkWidget *overview_btn = gtk_button_new_from_icon_name ("view-grid-symbolic");
  gtk_widget_set_tooltip_text (overview_btn, "Tab Overview (Ctrl+Shift+O)");
  gtk_actionable_set_action_name (GTK_ACTIONABLE (overview_btn), "win.tab-overview");
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), overview_btn);

  GtkWidget *info_btn = gtk_menu_button_new ();
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (info_btn), "info-outline-symbolic");
  gtk_widget_set_tooltip_text (info_btn, "Canvas Details");
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (info_btn), build_info_popover (self));
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), info_btn);

  return header;
}

static AdwTabPage *
on_overview_create_tab (AdwTabOverview *overview G_GNUC_UNUSED, gpointer user_data)
{
  return paint_window_add_new_tab (PAINT_WINDOW (user_data), NULL);
}

static void
on_selected_page_changed (GObject *obj G_GNUC_UNUSED,
                          GParamSpec *pspec G_GNUC_UNUSED,
                          gpointer user_data)
{
  sync_window_state (PAINT_WINDOW (user_data));
}

static void
on_page_state_changed (PaintCanvasPage *page G_GNUC_UNUSED, gpointer user_data)
{
  sync_window_state (PAINT_WINDOW (user_data));
}

static AdwTabView *
on_tab_create_window (AdwTabView *view G_GNUC_UNUSED, gpointer user_data)
{
  PaintWindow *self = PAINT_WINDOW (user_data);
  GtkApplication *app = gtk_window_get_application (GTK_WINDOW (self));
  PaintWindow *new_win = paint_window_new_empty (ADW_APPLICATION (app));
  gtk_window_present (GTK_WINDOW (new_win));
  return new_win->tab_view;
}

static gboolean
close_if_empty_after_detach_idle (gpointer user_data)
{
  PaintWindow *self = PAINT_WINDOW (user_data);
  if (adw_tab_view_get_n_pages (self->tab_view) == 0 &&
      !adw_tab_view_get_is_transferring_page (self->tab_view)) {
    GtkApplication *app = gtk_window_get_application (GTK_WINDOW (self));
    int other_windows = 0;
    if (app) {
      for (GList *l = gtk_application_get_windows (app); l != NULL; l = l->next) {
        if (PAINT_IS_WINDOW (l->data) && l->data != self)
          other_windows++;
      }
    }
    if (other_windows > 0)
      gtk_window_destroy (GTK_WINDOW (self));
  }
  return G_SOURCE_REMOVE;
}

static void
on_tab_page_attached (AdwTabView *view G_GNUC_UNUSED,
                      AdwTabPage *tab_page,
                      gint position G_GNUC_UNUSED,
                      gpointer user_data)
{
  PaintWindow *self = PAINT_WINDOW (user_data);
  GtkWidget *child = adw_tab_page_get_child (tab_page);
  if (PAINT_IS_CANVAS_PAGE (child)) {
    g_signal_connect (child, "state-changed", G_CALLBACK (on_page_state_changed), self);
    update_tab_metadata (tab_page, PAINT_CANVAS_PAGE (child));
  }
  sync_window_state (self);
}

static void
on_tab_page_detached (AdwTabView *view G_GNUC_UNUSED,
                      AdwTabPage *tab_page,
                      gint position G_GNUC_UNUSED,
                      gpointer user_data)
{
  PaintWindow *self = PAINT_WINDOW (user_data);
  GtkWidget *child = adw_tab_page_get_child (tab_page);
  if (PAINT_IS_CANVAS_PAGE (child))
    g_signal_handlers_disconnect_by_func (child, G_CALLBACK (on_page_state_changed), self);
  sync_window_state (self);
  g_idle_add (close_if_empty_after_detach_idle, self);
}

static gboolean
on_files_dropped (GtkDropTarget *target G_GNUC_UNUSED,
                  const GValue *value,
                  double x G_GNUC_UNUSED,
                  double y G_GNUC_UNUSED,
                  gpointer user_data)
{
  PaintWindow *self = PAINT_WINDOW (user_data);
  if (!G_VALUE_HOLDS (value, GDK_TYPE_FILE_LIST))
    return FALSE;

  GdkFileList *file_list = g_value_get_boxed (value);
  GSList *files = file_list ? gdk_file_list_get_files (file_list) : NULL;
  if (!files)
    return FALSE;

  for (GSList *l = files; l != NULL; l = l->next) {
    if (G_IS_FILE (l->data))
      paint_window_open_file (self, G_FILE (l->data));
  }
  g_slist_free (files);
  return TRUE;
}

void
paint_window_apply_preferences (PaintWindow *self)
{
  PaintPreferences *prefs = get_app_preferences (self);
  if (!prefs)
    return;

  GAction *grid_act = g_action_map_lookup_action (G_ACTION_MAP (self), "toggle-pixel-grid");
  if (grid_act)
    g_simple_action_set_state (G_SIMPLE_ACTION (grid_act),
                               g_variant_new_boolean (prefs->show_pixel_grid));

  int n = adw_tab_view_get_n_pages (self->tab_view);
  for (int i = 0; i < n; i++) {
    AdwTabPage *tab = adw_tab_view_get_nth_page (self->tab_view, i);
    GtkWidget *child = adw_tab_page_get_child (tab);
    if (PAINT_IS_CANVAS_PAGE (child)) {
      PaintCanvasPage *page = PAINT_CANVAS_PAGE (child);
      PaintCanvas *canvas = paint_canvas_page_get_canvas (page);
      paint_canvas_set_show_cursor_ring (canvas, prefs->show_cursor_ring);
      paint_canvas_set_show_pixel_grid (canvas, prefs->show_pixel_grid);
      paint_canvas_page_set_osd_visible (page, prefs->show_toolbar);
    }
  }
}

AdwTabPage *
paint_window_add_new_tab (PaintWindow *self, PaintDocument *doc)
{
  PaintPreferences *prefs = get_app_preferences (self);
  if (!doc && prefs)
    doc = paint_document_new (prefs->default_width, prefs->default_height);

  PaintCanvasPage *page = paint_canvas_page_new (doc, self->shared_settings);
  if (prefs) {
    PaintCanvas *canvas = paint_canvas_page_get_canvas (page);
    paint_canvas_set_show_cursor_ring (canvas, prefs->show_cursor_ring);
    paint_canvas_set_show_pixel_grid (canvas, prefs->show_pixel_grid);
    paint_canvas_page_set_osd_visible (page, prefs->show_toolbar);
  }

  AdwTabPage *tab = adw_tab_view_append (self->tab_view, GTK_WIDGET (page));
  adw_tab_view_set_selected_page (self->tab_view, tab);
  return tab;
}

static void
paint_window_finalize (GObject *object)
{
  PaintWindow *self = PAINT_WINDOW (object);
  g_clear_pointer (&self->recent_paths, g_ptr_array_unref);
  G_OBJECT_CLASS (paint_window_parent_class)->finalize (object);
}

static void
paint_window_class_init (PaintWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = paint_window_finalize;
}

static void
paint_window_init (PaintWindow *self)
{
  static const GActionEntry win_actions[] = {
    { "new-tab", act_new_tab, NULL, NULL, NULL, { 0 } },
    { "close-tab", act_close_tab, NULL, NULL, NULL, { 0 } },
    { "open", act_open, NULL, NULL, NULL, { 0 } },
    { "open-recent", act_open_recent, "s", NULL, NULL, { 0 } },
    { "save", act_save, NULL, NULL, NULL, { 0 } },
    { "save-as", act_save_as, NULL, NULL, NULL, { 0 } },
    { "undo", act_undo, NULL, NULL, NULL, { 0 } },
    { "redo", act_redo, NULL, NULL, NULL, { 0 } },
    { "copy", act_copy, NULL, NULL, NULL, { 0 } },
    { "cut", act_cut, NULL, NULL, NULL, { 0 } },
    { "paste", act_paste, NULL, NULL, NULL, { 0 } },
    { "paste-new-tab", act_paste_new_tab, NULL, NULL, NULL, { 0 } },
    { "select-all", act_select_all, NULL, NULL, NULL, { 0 } },
    { "delete-selection", act_delete_selection, NULL, NULL, NULL, { 0 } },
    { "crop-selection", act_crop_selection, NULL, NULL, NULL, { 0 } },
    { "rotate-cw", act_rotate_cw, NULL, NULL, NULL, { 0 } },
    { "rotate-ccw", act_rotate_ccw, NULL, NULL, NULL, { 0 } },
    { "flip-horizontal", act_flip_horizontal, NULL, NULL, NULL, { 0 } },
    { "flip-vertical", act_flip_vertical, NULL, NULL, NULL, { 0 } },
    { "clear", act_clear, NULL, NULL, NULL, { 0 } },
    { "resize-canvas", act_resize, NULL, NULL, NULL, { 0 } },
    { "zoom-in", act_zoom_in, NULL, NULL, NULL, { 0 } },
    { "zoom-out", act_zoom_out, NULL, NULL, NULL, { 0 } },
    { "zoom-reset", act_zoom_reset, NULL, NULL, NULL, { 0 } },
    { "zoom-fit", act_zoom_fit, NULL, NULL, NULL, { 0 } },
    { "toggle-pixel-grid", NULL, NULL, "true", act_toggle_pixel_grid_state, { 0 } },
    { "toggle-toolbar", act_toggle_toolbar, NULL, NULL, NULL, { 0 } },
    { "toggle-fullscreen", act_toggle_fullscreen, NULL, NULL, NULL, { 0 } },
    { "tab-overview", act_tab_overview, NULL, NULL, NULL, { 0 } },
    { "tool-select", act_tool_select, NULL, NULL, NULL, { 0 } },
    { "tool-brush", act_tool_brush, NULL, NULL, NULL, { 0 } },
    { "tool-line", act_tool_line, NULL, NULL, NULL, { 0 } },
    { "tool-eraser", act_tool_eraser, NULL, NULL, NULL, { 0 } },
    { "tool-fill", act_tool_fill, NULL, NULL, NULL, { 0 } },
    { "tool-text", act_tool_text, NULL, NULL, NULL, { 0 } },
    { "tool-eyedropper", act_tool_eyedropper, NULL, NULL, NULL, { 0 } },
    { "tool-transparent", act_tool_transparent, NULL, NULL, NULL, { 0 } },
    { "tool-shape", act_tool_shape, NULL, NULL, NULL, { 0 } },
    { "brush-smaller", act_brush_smaller, NULL, NULL, NULL, { 0 } },
    { "brush-larger", act_brush_larger, NULL, NULL, NULL, { 0 } },
  };

  GApplication *app = g_application_get_default ();
  self->shared_settings = (app && PAINT_IS_APPLICATION (app))
                            ? paint_application_get_tool_settings (PAINT_APPLICATION (app))
                            : paint_tool_settings_new ();
  self->recent_paths = g_ptr_array_new_with_free_func (g_free);
  load_recent_files (self);

  gtk_window_set_title (GTK_WINDOW (self), "Paint");
  gtk_window_set_icon_name (GTK_WINDOW (self), PAINT_APP_ID);
  gtk_window_set_default_size (GTK_WINDOW (self), 720, 520);

  g_action_map_add_action_entries (G_ACTION_MAP (self),
                                   win_actions,
                                   G_N_ELEMENTS (win_actions),
                                   self);

  PaintPreferences *prefs = get_app_preferences (self);
  if (prefs) {
    GAction *grid_act = g_action_map_lookup_action (G_ACTION_MAP (self), "toggle-pixel-grid");
    if (grid_act)
      g_simple_action_set_state (G_SIMPLE_ACTION (grid_act),
                                 g_variant_new_boolean (prefs->show_pixel_grid));
  }

  GtkEventController *key_ctrl = gtk_event_controller_key_new ();
  g_signal_connect (key_ctrl, "key-pressed", G_CALLBACK (on_key_pressed), self);
  gtk_widget_add_controller (GTK_WIDGET (self), key_ctrl);

  self->tab_view = ADW_TAB_VIEW (adw_tab_view_new ());
  g_signal_connect (self->tab_view, "notify::selected-page", G_CALLBACK (on_selected_page_changed), self);
  g_signal_connect (self->tab_view, "close-page", G_CALLBACK (on_close_page), self);
  g_signal_connect (self->tab_view, "create-window", G_CALLBACK (on_tab_create_window), self);
  g_signal_connect (self->tab_view, "page-attached", G_CALLBACK (on_tab_page_attached), self);
  g_signal_connect (self->tab_view, "page-detached", G_CALLBACK (on_tab_page_detached), self);

  GtkWidget *toolbar_view = adw_toolbar_view_new ();
  adw_toolbar_view_set_top_bar_style (ADW_TOOLBAR_VIEW (toolbar_view), ADW_TOOLBAR_RAISED_BORDER);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), build_header_bar (self));

  GtkWidget *tab_bar = GTK_WIDGET (adw_tab_bar_new ());
  adw_tab_bar_set_view (ADW_TAB_BAR (tab_bar), self->tab_view);
  adw_tab_bar_set_autohide (ADW_TAB_BAR (tab_bar), TRUE);
  adw_tab_bar_set_expand_tabs (ADW_TAB_BAR (tab_bar), TRUE);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), tab_bar);

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), GTK_WIDGET (self->tab_view));

  self->tab_overview = ADW_TAB_OVERVIEW (adw_tab_overview_new ());
  adw_tab_overview_set_view (self->tab_overview, self->tab_view);
  adw_tab_overview_set_enable_new_tab (self->tab_overview, TRUE);
  adw_tab_overview_set_child (self->tab_overview, toolbar_view);
  g_signal_connect (self->tab_overview, "create-tab", G_CALLBACK (on_overview_create_tab), self);

  adw_application_window_set_content (ADW_APPLICATION_WINDOW (self), GTK_WIDGET (self->tab_overview));
  GtkDropTarget *drop = gtk_drop_target_new (GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
  g_signal_connect (drop, "drop", G_CALLBACK (on_files_dropped), self);
  gtk_widget_add_controller (GTK_WIDGET (self->tab_overview), GTK_EVENT_CONTROLLER (drop));
  g_signal_connect (self, "close-request", G_CALLBACK (on_window_close_request), NULL);
}

PaintWindow *
paint_window_new_empty (AdwApplication *app)
{
  return g_object_new (PAINT_TYPE_WINDOW, "application", app, NULL);
}

PaintWindow *
paint_window_new (AdwApplication *app)
{
  PaintWindow *self = paint_window_new_empty (app);
  paint_window_add_new_tab (self, NULL);
  return self;
}

AdwTabView *
paint_window_get_tab_view (PaintWindow *self)
{
  return self->tab_view;
}
