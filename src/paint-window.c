#include "paint-window.h"

#include <glib/gstdio.h>
#include <math.h>

#include "paint-application.h"
#include "paint-canvas-page.h"
#include "paint-dialogs.h"
#include "paint-i18n.h"
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
  GtkLabel *info_layers_label;
  GtkLabel *info_zoom_label;
  GtkLabel *info_bg_label;
  GtkLabel *info_path_label;
  gboolean layers_visible;

  guint startup_fit_tick_id;
  int startup_fit_attempts;
  int startup_fit_last_w;
  int startup_fit_last_h;
  gboolean startup_fit_expect_maximize;
};

G_DEFINE_FINAL_TYPE (PaintWindow, paint_window, ADW_TYPE_APPLICATION_WINDOW)

/* Forward declarations */
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
    g_autoptr (GMenuItem) item = g_menu_item_new (_("No Recent Images"), NULL);
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
    g_menu_append_section (menu, _("Recent Images"), G_MENU_MODEL (section));
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
  g_autofree char *path = file ? g_file_get_parse_name (file) : g_strdup (_("Untitled Canvas"));
  adw_tab_page_set_tooltip (tab_page, path ? path : _("Untitled Canvas"));
}

void
paint_window_sync_state (PaintWindow *self)
{
  AdwTabPage *tab = adw_tab_view_get_selected_page (self->tab_view);
  PaintCanvasPage *page = get_active_canvas_page (self);
  if (!tab || !page) {
    adw_window_title_set_title (self->window_title, _("Paint"));
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
  GAction *invert_sel_act = g_action_map_lookup_action (G_ACTION_MAP (self), "invert-selection");
  if (invert_sel_act)
    g_simple_action_set_enabled (G_SIMPLE_ACTION (invert_sel_act),
                                 paint_canvas_has_selection (canvas));
  GAction *del_sel_act = g_action_map_lookup_action (G_ACTION_MAP (self), "delete-selection");
  if (del_sel_act)
    g_simple_action_set_enabled (G_SIMPLE_ACTION (del_sel_act),
                                 paint_canvas_has_selection (canvas));
  GAction *merge_act = g_action_map_lookup_action (G_ACTION_MAP (self), "merge-layer-down");
  if (merge_act)
    g_simple_action_set_enabled (G_SIMPLE_ACTION (merge_act),
                                 paint_document_get_active_layer (doc) > 0);
  GAction *flatten_act = g_action_map_lookup_action (G_ACTION_MAP (self), "flatten-layers");
  if (flatten_act)
    g_simple_action_set_enabled (G_SIMPLE_ACTION (flatten_act),
                                 paint_document_get_n_layers (doc) > 1);

  g_autofree char *zoom_str = g_strdup_printf ("%d%%", zoom_pct);
  g_autofree char *dims_str = g_strdup_printf ("%d × %d px",
                                               paint_document_get_width (doc),
                                               paint_document_get_height (doc));
  g_autofree char *layers_str = g_strdup_printf ("%d", paint_document_get_n_layers (doc));
  GFile *file = paint_document_get_file (doc);
  g_autofree char *path_str = file ? g_file_get_parse_name (file) : NULL;

  gtk_button_set_label (self->zoom_pct_btn, zoom_str);
  gtk_label_set_text (self->info_dims_label, dims_str);
  if (self->info_layers_label)
    gtk_label_set_text (self->info_layers_label, layers_str);
  gtk_label_set_text (self->info_zoom_label, zoom_str);
  gtk_label_set_text (self->info_bg_label,
                      paint_document_has_transparent_background (doc) ? _("Transparent") : _("White"));
  gtk_label_set_text (self->info_path_label, path_str ? path_str : _("Not saved yet"));

  if (paint_document_get_modified (doc)) {
    PaintPreferences *prefs = paint_window_get_preferences (self);
    GtkApplication *app = gtk_window_get_application (GTK_WINDOW (self));
    if (prefs && prefs->restore_session && app && ADW_IS_APPLICATION (app))
      paint_session_schedule_autosave (ADW_APPLICATION (app));
  }
}

/* ============================================================================
 * File Open / Save
 * ============================================================================ */

static GListModel *
build_image_filters (void)
{
  GListStore *filters = g_list_store_new (GTK_TYPE_FILE_FILTER);

  g_autoptr (GtkFileFilter) img_filter = gtk_file_filter_new ();
  gtk_file_filter_set_name (img_filter, _("Image Files (ORA, PNG, JPEG, WebP, BMP)"));
  gtk_file_filter_add_pattern (img_filter, "*.ora");
  gtk_file_filter_add_pattern (img_filter, "*.png");
  gtk_file_filter_add_pattern (img_filter, "*.jpg");
  gtk_file_filter_add_pattern (img_filter, "*.jpeg");
  gtk_file_filter_add_pattern (img_filter, "*.webp");
  gtk_file_filter_add_pattern (img_filter, "*.bmp");
  g_list_store_append (filters, img_filter);

  g_autoptr (GtkFileFilter) ora_filter = gtk_file_filter_new ();
  gtk_file_filter_set_name (ora_filter, _("OpenRaster Layered Project (*.ora)"));
  gtk_file_filter_add_pattern (ora_filter, "*.ora");
  g_list_store_append (filters, ora_filter);

  g_autoptr (GtkFileFilter) png_filter = gtk_file_filter_new ();
  gtk_file_filter_set_name (png_filter, _("PNG Image (*.png)"));
  gtk_file_filter_add_pattern (png_filter, "*.png");
  g_list_store_append (filters, png_filter);

  g_autoptr (GtkFileFilter) jpg_filter = gtk_file_filter_new ();
  gtk_file_filter_set_name (jpg_filter, _("JPEG Image (*.jpg, *.jpeg)"));
  gtk_file_filter_add_pattern (jpg_filter, "*.jpg");
  gtk_file_filter_add_pattern (jpg_filter, "*.jpeg");
  g_list_store_append (filters, jpg_filter);

  g_autoptr (GtkFileFilter) webp_filter = gtk_file_filter_new ();
  gtk_file_filter_set_name (webp_filter, _("WebP Image (*.webp)"));
  gtk_file_filter_add_pattern (webp_filter, "*.webp");
  g_list_store_append (filters, webp_filter);

  g_autoptr (GtkFileFilter) bmp_filter = gtk_file_filter_new ();
  gtk_file_filter_set_name (bmp_filter, _("BMP Image (*.bmp)"));
  gtk_file_filter_add_pattern (bmp_filter, "*.bmp");
  g_list_store_append (filters, bmp_filter);

  g_autoptr (GtkFileFilter) all_filter = gtk_file_filter_new ();
  gtk_file_filter_set_name (all_filter, _("All Files"));
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
  gtk_file_dialog_set_title (dialog, _("Open Image"));
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
    AdwDialog *err = adw_alert_dialog_new (_("Could Not Open Image"),
                                           error ? error->message : _("Unknown error"));
    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (err), "ok", _("OK"));
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
      paint_window_sync_state (self);
      return;
    }
  }

  paint_window_add_new_tab (self, doc);
}

static void
import_file_as_layer (PaintWindow *self, GFile *file)
{
  PaintCanvasPage *page = get_active_canvas_page (self);
  if (!page || !file)
    return;
  g_autoptr (GError) err = NULL;
  g_autoptr (GdkTexture) tex = paint_io_load_file_as_texture (file, &err);
  if (!tex) {
    AdwDialog *dlg = adw_alert_dialog_new (_("Could Not Import Image"),
                                           err ? err->message : _("Unsupported image file."));
    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dlg), "ok", _("OK"));
    adw_dialog_present (dlg, GTK_WIDGET (self));
    return;
  }
  g_autofree char *name = paint_io_layer_name_from_file (file);
  paint_canvas_add_layer_from_texture (paint_canvas_page_get_canvas (page), name, tex);
}

typedef struct {
  PaintWindow *window;
  PaintDocument *doc;
  GFile *target_file;
  void (*callback) (gboolean ok, gpointer data);
  gpointer callback_data;
} SaveContext;

static void
save_context_free (SaveContext *ctx)
{
  g_clear_object (&ctx->target_file);
  g_free (ctx);
}

static void
show_save_error (PaintWindow *self, const GError *error)
{
  AdwDialog *err = adw_alert_dialog_new (_("Could Not Save Image"),
                                         error ? error->message : _("Unknown error"));
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (err), "ok", _("OK"));
  adw_dialog_present (err, GTK_WIDGET (self));
}

static void
execute_save_to_file (SaveContext *ctx, GFile *file)
{
  gboolean ok = FALSE;
  g_autoptr (GError) error = NULL;
  if (paint_document_save_to_file (ctx->doc, file, &error)) {
    record_recent_file (ctx->window, paint_document_get_file (ctx->doc));
    paint_window_sync_state (ctx->window);
    ok = TRUE;
  } else {
    show_save_error (ctx->window, error);
  }
  if (ctx->callback)
    ctx->callback (ok, ctx->callback_data);
  save_context_free (ctx);
}

static void open_save_file_dialog (SaveContext *ctx, const char *suggested_name);

static void
on_flatten_warning_response (GObject *source, GAsyncResult *res, gpointer user_data)
{
  SaveContext *ctx = user_data;
  const char *choice = adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), res);
  if (g_strcmp0 (choice, "flatten") == 0) {
    g_autoptr (GFile) f = g_steal_pointer (&ctx->target_file);
    execute_save_to_file (ctx, f);
  } else if (g_strcmp0 (choice, "save-ora") == 0) {
    GFile *orig = ctx->target_file ? ctx->target_file : paint_document_get_file (ctx->doc);
    g_autofree char *base = orig ? g_file_get_basename (orig) : g_strdup ("Untitled");
    char *dot = strrchr (base, '.');
    if (dot && dot != base)
      *dot = '\0';
    g_autofree char *ora_name = g_strconcat (base, ".ora", NULL);
    g_clear_object (&ctx->target_file);
    open_save_file_dialog (ctx, ora_name);
  } else {
    if (ctx->callback)
      ctx->callback (FALSE, ctx->callback_data);
    save_context_free (ctx);
  }
}

static void
save_with_flatten_check (SaveContext *ctx, GFile *file)
{
  if (paint_document_has_layer_structure (ctx->doc) && !paint_io_is_ora_file (file)) {
    ctx->target_file = g_object_ref (file);
    g_autofree char *base = g_file_get_basename (file);
    g_autofree char *body = g_strdup_printf (
        _("This canvas has multiple layers or layer settings. Saving as “%s” will flatten layers in the saved file. Save as OpenRaster (.ora) to keep layers editable."),
        base ? base : "image");

    AdwDialog *dlg = adw_alert_dialog_new (_("Save Flat Image?"), body);
    adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dlg),
                                    "cancel", _("Cancel"),
                                    "flatten", _("Save Flattened"),
                                    "save-ora", _("Save as .ora…"),
                                    NULL);
    adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dlg),
                                              "save-ora",
                                              ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dlg), "save-ora");
    adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dlg), "cancel");
    adw_alert_dialog_set_prefer_wide_layout (ADW_ALERT_DIALOG (dlg), TRUE);
    adw_alert_dialog_choose (ADW_ALERT_DIALOG (dlg),
                             GTK_WIDGET (ctx->window),
                             NULL,
                             on_flatten_warning_response,
                             ctx);
    return;
  }

  execute_save_to_file (ctx, file);
}

static void
on_save_dialog_finish (GObject *source, GAsyncResult *res, gpointer user_data)
{
  SaveContext *ctx = user_data;
  GtkFileDialog *dialog = GTK_FILE_DIALOG (source);
  g_autoptr (GFile) file = gtk_file_dialog_save_finish (dialog, res, NULL);

  if (!file) {
    if (ctx->callback)
      ctx->callback (FALSE, ctx->callback_data);
    save_context_free (ctx);
    return;
  }

  save_with_flatten_check (ctx, file);
}

static void
open_save_file_dialog (SaveContext *ctx, const char *suggested_name)
{
  g_autoptr (GtkFileDialog) dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dialog, _("Save Image"));
  gtk_file_dialog_set_initial_name (dialog, suggested_name);
  g_autoptr (GListModel) filters = build_image_filters ();
  gtk_file_dialog_set_filters (dialog, filters);
  gtk_file_dialog_save (dialog, GTK_WINDOW (ctx->window), NULL, on_save_dialog_finish, ctx);
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

  SaveContext *ctx = g_new0 (SaveContext, 1);
  ctx->window = self;
  ctx->doc = doc;
  ctx->callback = cb;
  ctx->callback_data = user_data;

  GFile *existing = paint_document_get_file (doc);
  if (!save_as && existing) {
    save_with_flatten_check (ctx, existing);
    return;
  }

  g_autofree char *init_name = NULL;
  if (existing) {
    init_name = g_file_get_basename (existing);
  } else {
    init_name = g_strdup (paint_document_has_layer_structure (doc)
                            ? "Untitled.ora"
                            : "Untitled.png");
  }
  open_save_file_dialog (ctx, init_name);
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
  g_autofree char *name = file ? g_file_get_basename (file) : g_strdup (_("Untitled Canvas"));
  g_autofree char *body = g_strdup_printf (
      _("“%s” contains unsaved changes. Changes which are not saved will be permanently lost."),
      name);

  AdwDialog *dlg = adw_alert_dialog_new (_("Save Changes?"), body);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dlg),
                                  "cancel", _("Cancel"),
                                  "discard", _("Discard"),
                                  "save", file ? _("Save") : _("Save…"),
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
    return g_strdup (_("Not saved yet"));

  g_autoptr (GFile) parent = g_file_get_parent (file);
  if (!parent)
    return g_strdup (_("Not saved yet"));

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

PaintPreferences *
paint_window_get_preferences (PaintWindow *self)
{
  GtkApplication *app = self ? gtk_window_get_application (GTK_WINDOW (self)) : NULL;
  if (!app)
    app = GTK_APPLICATION (g_application_get_default ());
  if (app && PAINT_IS_APPLICATION (app))
    return paint_application_get_preferences (PAINT_APPLICATION (app));
  return NULL;
}

void
paint_window_compute_fit_window_size (PaintWindow *self, int *out_w, int *out_h)
{
  PaintPreferences *prefs = paint_window_get_preferences (self);
  int doc_w = prefs ? prefs->default_width : PAINT_DEFAULT_CANVAS_WIDTH;
  int doc_h = prefs ? prefs->default_height : PAINT_DEFAULT_CANVAS_HEIGHT;

  PaintCanvasPage *page = get_active_canvas_page (self);
  if (page) {
    PaintDocument *doc = paint_canvas_get_document (paint_canvas_page_get_canvas (page));
    if (doc) {
      doc_w = paint_document_get_width (doc);
      doc_h = paint_document_get_height (doc);
    }
  }

  int tab_extra = (self->tab_view && adw_tab_view_get_n_pages (self->tab_view) > 1) ? 38 : 0;
  int desired_w = doc_w + 80;
  int desired_h = doc_h + 140 + tab_extra;

  int max_w = 1400;
  int max_h = 920;
  GdkDisplay *display = gtk_widget_get_display (GTK_WIDGET (self));
  if (!display)
    display = gdk_display_get_default ();
  if (display) {
    GListModel *monitors = gdk_display_get_monitors (display);
    if (monitors && g_list_model_get_n_items (monitors) > 0) {
      g_autoptr (GdkMonitor) mon = GDK_MONITOR (g_list_model_get_item (monitors, 0));
      if (mon) {
        GdkRectangle geom;
        gdk_monitor_get_geometry (mon, &geom);
        if (geom.width > 0 && geom.height > 0) {
          max_w = MAX (640, (int) lround ((double) geom.width * 0.90));
          max_h = MAX (440, (int) lround ((double) geom.height * 0.88));
        }
      }
    }
  }

  *out_w = CLAMP (desired_w, 640, max_w);
  *out_h = CLAMP (desired_h, 440, max_h);
}

static gboolean
on_startup_fit_tick (GtkWidget *widget,
                     GdkFrameClock *clock G_GNUC_UNUSED,
                     gpointer user_data G_GNUC_UNUSED)
{
  PaintWindow *self = PAINT_WINDOW (widget);
  self->startup_fit_attempts++;

  PaintCanvasPage *page = get_active_canvas_page (self);
  if (!page) {
    if (self->startup_fit_attempts >= 60) {
      self->startup_fit_tick_id = 0;
      return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
  }

  int cur_w = gtk_widget_get_width (widget);
  int cur_h = gtk_widget_get_height (widget);
  if (cur_w <= 0 || cur_h <= 0) {
    if (self->startup_fit_attempts >= 60) {
      self->startup_fit_tick_id = 0;
      return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
  }

  gboolean max_pending = self->startup_fit_expect_maximize &&
                         !gtk_window_is_maximized (GTK_WINDOW (self)) &&
                         self->startup_fit_attempts < 15;
  gboolean fitted = paint_canvas_page_zoom_fit (page);
  if (fitted)
    paint_window_sync_state (self);

  if ((!fitted || cur_w != self->startup_fit_last_w ||
       cur_h != self->startup_fit_last_h || max_pending) &&
      self->startup_fit_attempts < 60) {
    self->startup_fit_last_w = cur_w;
    self->startup_fit_last_h = cur_h;
    return G_SOURCE_CONTINUE;
  }

  self->startup_fit_tick_id = 0;
  return G_SOURCE_REMOVE;
}

void
paint_window_schedule_startup_fit (PaintWindow *self, gboolean expect_maximize)
{
  if (self->startup_fit_tick_id != 0) {
    gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->startup_fit_tick_id);
    self->startup_fit_tick_id = 0;
  }
  self->startup_fit_attempts = 0;
  self->startup_fit_last_w = 0;
  self->startup_fit_last_h = 0;
  self->startup_fit_expect_maximize = expect_maximize;
  self->startup_fit_tick_id = gtk_widget_add_tick_callback (GTK_WIDGET (self),
                                                            on_startup_fit_tick,
                                                            NULL,
                                                            NULL);
}

static void
sync_tool_settings_colors_to_prefs (PaintWindow *self)
{
  PaintPreferences *prefs = paint_window_get_preferences (self);
  if (!prefs || !self->shared_settings)
    return;

  int n_rec = CLAMP (self->shared_settings->n_recent_colors, 0, PAINT_PREFS_MAX_RECENT_COLORS);
  prefs->n_recent_colors = n_rec;
  for (int i = 0; i < n_rec; i++)
    prefs->recent_colors[i] = self->shared_settings->recent_colors[i];

  int n_cust = CLAMP (self->shared_settings->n_custom_colors, 0, PAINT_PREFS_MAX_CUSTOM_COLORS);
  prefs->n_custom_colors = n_cust;
  for (int i = 0; i < n_cust; i++)
    prefs->custom_colors[i] = self->shared_settings->custom_colors[i];
}

void
paint_window_save_window_state (PaintWindow *self)
{
  if (!self)
    return;
  PaintPreferences *prefs = paint_window_get_preferences (self);
  if (!prefs)
    return;

  sync_tool_settings_colors_to_prefs (self);

  gboolean maximized = gtk_window_is_maximized (GTK_WINDOW (self));
  gboolean fullscreen = gtk_window_is_fullscreen (GTK_WINDOW (self));
  prefs->last_window_maximized = maximized;
  if (!maximized && !fullscreen) {
    int w = gtk_widget_get_width (GTK_WIDGET (self));
    int h = gtk_widget_get_height (GTK_WIDGET (self));
    if (w >= 360 && h >= 300) {
      prefs->last_window_width = CLAMP (w, 360, 8192);
      prefs->last_window_height = CLAMP (h, 300, 8192);
    }
  }
  paint_preferences_save (prefs);
}

void
paint_window_apply_startup_sizing (PaintWindow *self)
{
  if (!self)
    return;
  PaintPreferences *prefs = paint_window_get_preferences (self);
  PaintStartupWindowMode mode = prefs ? prefs->startup_window_mode : PAINT_WINDOW_SIZE_REMEMBER;
  gboolean expect_maximize = FALSE;

  switch (mode) {
  case PAINT_WINDOW_SIZE_FIT_CANVAS: {
    int w = PAINT_DEFAULT_WINDOW_WIDTH;
    int h = PAINT_DEFAULT_WINDOW_HEIGHT;
    paint_window_compute_fit_window_size (self, &w, &h);
    gtk_window_set_default_size (GTK_WINDOW (self), w, h);
    break;
  }
  case PAINT_WINDOW_SIZE_DEFAULT:
    gtk_window_set_default_size (GTK_WINDOW (self),
                                 PAINT_DEFAULT_WINDOW_WIDTH,
                                 PAINT_DEFAULT_WINDOW_HEIGHT);
    break;
  case PAINT_WINDOW_SIZE_REMEMBER:
  default: {
    int w = prefs ? prefs->last_window_width : PAINT_DEFAULT_WINDOW_WIDTH;
    int h = prefs ? prefs->last_window_height : PAINT_DEFAULT_WINDOW_HEIGHT;
    if (w < 360 || h < 300) {
      w = PAINT_DEFAULT_WINDOW_WIDTH;
      h = PAINT_DEFAULT_WINDOW_HEIGHT;
    }
    gtk_window_set_default_size (GTK_WINDOW (self), w, h);
    if (prefs && prefs->last_window_maximized) {
      gtk_window_maximize (GTK_WINDOW (self));
      expect_maximize = TRUE;
    }
    break;
  }
  }

  if (prefs && prefs->fit_canvas_on_startup)
    paint_window_schedule_startup_fit (self, expect_maximize);
}

static gboolean
on_window_close_request (GtkWindow *window, gpointer user_data G_GNUC_UNUSED)
{
  PaintWindow *self = PAINT_WINDOW (window);
  GtkApplication *app = gtk_window_get_application (window);
  PaintPreferences *prefs = paint_window_get_preferences (self);

  guint open_paint_windows = 0;
  if (app) {
    for (GList *l = gtk_application_get_windows (app); l != NULL; l = l->next) {
      if (PAINT_IS_WINDOW (l->data))
        open_paint_windows++;
    }
  }

  if (prefs && prefs->restore_session && open_paint_windows <= 1) {
    paint_session_cancel_autosave ();
    paint_window_save_window_state (self);
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

  if (dirty_tabs->len == 0) {
    paint_window_save_window_state (self);
    return FALSE;
  }

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
        _("Save Changes?"),
        _("Open canvases contain unsaved changes. Changes which are not saved will be permanently lost."));
    adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dlg),
                                    "cancel", _("Cancel"),
                                    "discard", _("Discard All"),
                                    "save", _("Save"),
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
                                        : g_strdup (_("Untitled Canvas (new)"));
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

static void
on_export_done (GFile *exported_file, gpointer user_data)
{
  PaintWindow *self = PAINT_WINDOW (user_data);
  if (exported_file)
    record_recent_file (self, exported_file);
}

static void act_export_image (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintWindow *self = PAINT_WINDOW (d);
  PaintCanvasPage *page = get_active_canvas_page (self);
  if (!page)
    return;
  PaintCanvas *canvas = paint_canvas_page_get_canvas (page);
  paint_canvas_commit_active_overlays (canvas);
  paint_dialog_show_export (GTK_WIDGET (self),
                            paint_canvas_get_document (canvas),
                            on_export_done,
                            self);
}

static void
on_print_draw_page (GtkPrintOperation *op G_GNUC_UNUSED,
                    GtkPrintContext *context,
                    int page_nr G_GNUC_UNUSED,
                    gpointer user_data)
{
  cairo_surface_t *surf = user_data;
  if (!surf || !context)
    return;

  cairo_t *cr = gtk_print_context_get_cairo_context (context);
  double pw = gtk_print_context_get_width (context);
  double ph = gtk_print_context_get_height (context);
  int sw = cairo_image_surface_get_width (surf);
  int sh = cairo_image_surface_get_height (surf);
  if (sw <= 0 || sh <= 0 || pw <= 0.0 || ph <= 0.0)
    return;

  double scale = MIN (pw / (double) sw, ph / (double) sh);
  double dx = (pw - (double) sw * scale) * 0.5;
  double dy = (ph - (double) sh * scale) * 0.5;

  cairo_save (cr);
  cairo_translate (cr, dx, dy);
  cairo_scale (cr, scale, scale);
  cairo_set_source_surface (cr, surf, 0.0, 0.0);
  cairo_pattern_set_filter (cairo_get_source (cr), CAIRO_FILTER_BEST);
  cairo_paint (cr);
  cairo_restore (cr);
}

static void act_print (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintWindow *self = PAINT_WINDOW (d);
  PaintCanvasPage *page = get_active_canvas_page (self);
  if (!page)
    return;

  PaintCanvas *canvas = paint_canvas_page_get_canvas (page);
  paint_canvas_commit_active_overlays (canvas);
  PaintDocument *doc = paint_canvas_get_document (canvas);
  cairo_surface_t *src = doc ? paint_document_get_surface (doc) : NULL;
  if (!src)
    return;

  int w = cairo_image_surface_get_width (src);
  int h = cairo_image_surface_get_height (src);
  cairo_surface_t *snapshot = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *cr = cairo_create (snapshot);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface (cr, src, 0.0, 0.0);
  cairo_paint (cr);
  cairo_destroy (cr);

  g_autoptr (GtkPrintOperation) op = gtk_print_operation_new ();
  gtk_print_operation_set_n_pages (op, 1);
  gtk_print_operation_set_use_full_page (op, FALSE);
  g_object_set_data_full (G_OBJECT (op),
                          "print-snapshot",
                          snapshot,
                          (GDestroyNotify) cairo_surface_destroy);
  g_signal_connect (op, "draw-page", G_CALLBACK (on_print_draw_page), snapshot);

  g_autoptr (GError) err = NULL;
  GtkPrintOperationResult res = gtk_print_operation_run (
      op,
      GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG,
      GTK_WINDOW (self),
      &err);
  if (res == GTK_PRINT_OPERATION_RESULT_ERROR) {
    AdwDialog *dlg = adw_alert_dialog_new (_("Could Not Print Image"),
                                           err ? err->message : _("Unknown printing error."));
    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dlg), "ok", _("OK"));
    adw_dialog_present (dlg, GTK_WIDGET (self));
  }
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
  PaintWindow *self = PAINT_WINDOW (d);
  PaintCanvasPage *page = get_active_canvas_page (self);
  if (page)
    paint_dialog_confirm_clear (GTK_WIDGET (self), paint_canvas_page_get_canvas (page));
}

static void act_resize (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintWindow *self = PAINT_WINDOW (d);
  PaintCanvasPage *page = get_active_canvas_page (self);
  if (page)
    paint_dialog_show_resize (GTK_WIDGET (self), paint_canvas_page_get_canvas (page));
}

static gboolean
forward_editable_action (PaintWindow *self, const char *action_name)
{
  GtkWidget *focus = gtk_window_get_focus (GTK_WINDOW (self));
  if (focus && (GTK_IS_EDITABLE (focus) || GTK_IS_TEXT (focus) || GTK_IS_TEXT_VIEW (focus))) {
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

static void act_copy_merged (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintWindow *self = PAINT_WINDOW (d);
  if (forward_editable_action (self, "clipboard.copy"))
    return;
  PaintCanvasPage *page = get_active_canvas_page (self);
  if (!page)
    return;
  g_autoptr (GdkTexture) tex = paint_canvas_copy_merged_selection (paint_canvas_page_get_canvas (page));
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

typedef enum {
  PASTE_FLOAT,
  PASTE_NEW_TAB,
  PASTE_AS_LAYER
} PasteTarget;

typedef struct {
  PaintWindow *window;
  PasteTarget target;
} PasteCtx;

static void
on_clipboard_read_texture_finish (GObject *source, GAsyncResult *res, gpointer user_data)
{
  PasteCtx *ctx = user_data;
  g_autoptr (GdkTexture) tex = gdk_clipboard_read_texture_finish (GDK_CLIPBOARD (source), res, NULL);
  if (tex) {
    if (ctx->target == PASTE_NEW_TAB) {
      PaintDocument *doc = paint_document_new_from_texture (tex);
      if (doc)
        paint_window_add_new_tab (ctx->window, doc);
    } else if (ctx->target == PASTE_AS_LAYER) {
      PaintCanvasPage *page = get_active_canvas_page (ctx->window);
      if (page) {
        paint_canvas_add_layer_from_texture (paint_canvas_page_get_canvas (page),
                                             _("Pasted Layer"),
                                             tex);
      }
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
request_clipboard_paste (PaintWindow *self, PasteTarget target)
{
  GdkClipboard *cb = gtk_widget_get_clipboard (GTK_WIDGET (self));
  PasteCtx *ctx = g_new0 (PasteCtx, 1);
  ctx->window = g_object_ref (self);
  ctx->target = target;
  gdk_clipboard_read_texture_async (cb, NULL, on_clipboard_read_texture_finish, ctx);
}

static void act_paste (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintWindow *self = PAINT_WINDOW (d);
  if (forward_editable_action (self, "clipboard.paste"))
    return;
  request_clipboard_paste (self, PASTE_FLOAT);
}

static void act_paste_as_layer (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  request_clipboard_paste (PAINT_WINDOW (d), PASTE_AS_LAYER);
}

static void act_paste_new_tab (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  request_clipboard_paste (PAINT_WINDOW (d), PASTE_NEW_TAB);
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

static void act_invert_selection (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (page)
    paint_canvas_invert_selection (paint_canvas_page_get_canvas (page));
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

static void act_trim_transparent (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (page)
    paint_canvas_trim_transparent (paint_canvas_page_get_canvas (page));
}

static void
apply_filter_sharpen_cb (cairo_surface_t *surf,
                         const cairo_rectangle_int_t *rect,
                         cairo_surface_t *mask,
                         gboolean alpha_lock,
                         gpointer user_data G_GNUC_UNUSED)
{
  paint_filter_sharpen (surf, rect, mask, alpha_lock, 0.8);
}

static void
apply_filter_invert_cb (cairo_surface_t *surf,
                        const cairo_rectangle_int_t *rect,
                        cairo_surface_t *mask,
                        gboolean alpha_lock,
                        gpointer user_data G_GNUC_UNUSED)
{
  paint_filter_invert (surf, rect, mask, alpha_lock);
}

static void
apply_filter_grayscale_cb (cairo_surface_t *surf,
                           const cairo_rectangle_int_t *rect,
                           cairo_surface_t *mask,
                           gboolean alpha_lock,
                           gpointer user_data G_GNUC_UNUSED)
{
  paint_filter_grayscale (surf, rect, mask, alpha_lock);
}

static void act_filter_brightness_contrast (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintWindow *self = PAINT_WINDOW (d);
  PaintCanvasPage *page = get_active_canvas_page (self);
  if (page)
    paint_dialog_show_brightness_contrast (GTK_WIDGET (self), paint_canvas_page_get_canvas (page));
}

static void act_filter_hsl (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintWindow *self = PAINT_WINDOW (d);
  PaintCanvasPage *page = get_active_canvas_page (self);
  if (page)
    paint_dialog_show_hsl (GTK_WIDGET (self), paint_canvas_page_get_canvas (page));
}

static void act_filter_blur (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintWindow *self = PAINT_WINDOW (d);
  PaintCanvasPage *page = get_active_canvas_page (self);
  if (page)
    paint_dialog_show_gaussian_blur (GTK_WIDGET (self), paint_canvas_page_get_canvas (page));
}

static void act_filter_sharpen (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (page)
    paint_canvas_apply_filter (paint_canvas_page_get_canvas (page), apply_filter_sharpen_cb, NULL);
}

static void act_filter_invert (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (page)
    paint_canvas_apply_filter (paint_canvas_page_get_canvas (page), apply_filter_invert_cb, NULL);
}

static void act_filter_grayscale (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (page)
    paint_canvas_apply_filter (paint_canvas_page_get_canvas (page), apply_filter_grayscale_cb, NULL);
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

  PaintPreferences *prefs = paint_window_get_preferences (self);
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

static void act_toggle_layers_state (GSimpleAction *action, GVariant *value, gpointer d)
{
  PaintWindow *self = PAINT_WINDOW (d);
  gboolean show = g_variant_get_boolean (value);
  self->layers_visible = show;
  g_simple_action_set_state (action, value);

  int n = adw_tab_view_get_n_pages (self->tab_view);
  for (int i = 0; i < n; i++) {
    AdwTabPage *tab = adw_tab_view_get_nth_page (self->tab_view, i);
    GtkWidget *child = adw_tab_page_get_child (tab);
    if (PAINT_IS_CANVAS_PAGE (child))
      paint_canvas_page_set_layers_visible (PAINT_CANVAS_PAGE (child), show);
  }
}

static void act_new_layer (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintWindow *self = PAINT_WINDOW (d);
  PaintCanvasPage *page = get_active_canvas_page (self);
  if (!page)
    return;
  paint_canvas_add_layer (paint_canvas_page_get_canvas (page), NULL);
  if (!self->layers_visible) {
    GAction *act = g_action_map_lookup_action (G_ACTION_MAP (self), "toggle-layers");
    if (act)
      g_action_change_state (act, g_variant_new_boolean (TRUE));
  }
}

static void act_duplicate_layer (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (!page)
    return;
  PaintCanvas *canvas = paint_canvas_page_get_canvas (page);
  PaintDocument *doc = paint_canvas_get_document (canvas);
  paint_canvas_duplicate_layer (canvas, paint_document_get_active_layer (doc));
}

static void act_merge_layer_down (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (!page)
    return;
  PaintCanvas *canvas = paint_canvas_page_get_canvas (page);
  PaintDocument *doc = paint_canvas_get_document (canvas);
  paint_canvas_merge_layer_down (canvas, paint_document_get_active_layer (doc));
}

static void act_flatten_layers (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (page)
    paint_canvas_flatten_layers (paint_canvas_page_get_canvas (page));
}

static void
on_import_layer_dialog_finish (GObject *source, GAsyncResult *res, gpointer user_data)
{
  PaintWindow *self = PAINT_WINDOW (user_data);
  GtkFileDialog *dialog = GTK_FILE_DIALOG (source);
  g_autoptr (GFile) file = gtk_file_dialog_open_finish (dialog, res, NULL);
  if (file)
    import_file_as_layer (self, file);
  g_object_unref (self);
}

static void act_import_layer (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintWindow *self = PAINT_WINDOW (d);
  g_autoptr (GtkFileDialog) dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dialog, _("Import Image as Layer"));
  g_autoptr (GListModel) filters = build_image_filters ();
  gtk_file_dialog_set_filters (dialog, filters);
  gtk_file_dialog_open (dialog, GTK_WINDOW (self), NULL,
                        on_import_layer_dialog_finish, g_object_ref (self));
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
    paint_canvas_page_select_last_select (page);
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

static void act_tool_gradient (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (page)
    paint_canvas_page_select_tool (page, "gradient");
}

static void act_tool_blur (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (page)
    paint_canvas_page_select_tool (page, "blur");
}

static void act_tool_pixelate (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintCanvasPage *page = get_active_canvas_page (PAINT_WINDOW (d));
  if (page)
    paint_canvas_page_select_tool (page, "pixelate");
}

static void
sync_all_color_pickers (PaintWindow *self)
{
  GtkApplication *app = gtk_window_get_application (GTK_WINDOW (self));
  if (app) {
    for (GList *l = gtk_application_get_windows (app); l != NULL; l = l->next) {
      if (!PAINT_IS_WINDOW (l->data))
        continue;
      PaintWindow *win = PAINT_WINDOW (l->data);
      int n = adw_tab_view_get_n_pages (win->tab_view);
      for (int i = 0; i < n; i++) {
        GtkWidget *child = adw_tab_page_get_child (adw_tab_view_get_nth_page (win->tab_view, i));
        if (PAINT_IS_CANVAS_PAGE (child)) {
          paint_canvas_page_sync_color_picker (PAINT_CANVAS_PAGE (child));
          gtk_widget_queue_draw (GTK_WIDGET (paint_canvas_page_get_canvas (PAINT_CANVAS_PAGE (child))));
        }
      }
    }
    return;
  }

  int n = adw_tab_view_get_n_pages (self->tab_view);
  for (int i = 0; i < n; i++) {
    GtkWidget *child = adw_tab_page_get_child (adw_tab_view_get_nth_page (self->tab_view, i));
    if (PAINT_IS_CANVAS_PAGE (child)) {
      paint_canvas_page_sync_color_picker (PAINT_CANVAS_PAGE (child));
      gtk_widget_queue_draw (GTK_WIDGET (paint_canvas_page_get_canvas (PAINT_CANVAS_PAGE (child))));
    }
  }
}

static void act_swap_colors (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintWindow *self = PAINT_WINDOW (d);
  paint_tool_settings_swap_colors (self->shared_settings);
  sync_all_color_pickers (self);
}

static void act_reset_colors (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer d)
{
  PaintWindow *self = PAINT_WINDOW (d);
  paint_tool_settings_reset_colors (self->shared_settings);
  sync_all_color_pickers (self);
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
  if (focus && (GTK_IS_EDITABLE (focus) || GTK_IS_TEXT (focus) || GTK_IS_TEXT_VIEW (focus)))
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

  PaintPreferences *prefs = paint_window_get_preferences (self);
  const char *matched_action = paint_preferences_match_local_keybind (prefs, keyval, state);
  if (matched_action) {
    if (g_str_has_prefix (matched_action, "app.")) {
      GtkApplication *app = gtk_window_get_application (GTK_WINDOW (self));
      if (app)
        g_action_group_activate_action (G_ACTION_GROUP (app), matched_action + 4, NULL);
    } else {
      gtk_widget_activate_action (GTK_WIDGET (self), matched_action, NULL);
    }
    return TRUE;
  }
  return FALSE;
}

static void
clear_all_space_pan (PaintWindow *self)
{
  int n = adw_tab_view_get_n_pages (self->tab_view);
  for (int i = 0; i < n; i++) {
    GtkWidget *child = adw_tab_page_get_child (adw_tab_view_get_nth_page (self->tab_view, i));
    if (PAINT_IS_CANVAS_PAGE (child))
      paint_canvas_page_set_space_pan (PAINT_CANVAS_PAGE (child), FALSE);
  }
}

static gboolean
on_alt_key_pressed (GtkEventControllerKey *ctrl G_GNUC_UNUSED,
                    guint keyval,
                    guint keycode G_GNUC_UNUSED,
                    GdkModifierType state G_GNUC_UNUSED,
                    gpointer user_data)
{
  PaintWindow *self = PAINT_WINDOW (user_data);
  if (keyval == GDK_KEY_Alt_L || keyval == GDK_KEY_Alt_R) {
    GtkWidget *focus = gtk_window_get_focus (GTK_WINDOW (self));
    if (focus && (GTK_IS_EDITABLE (focus) || GTK_IS_TEXT (focus) || GTK_IS_TEXT_VIEW (focus)))
      return FALSE;
    PaintCanvasPage *page = get_active_canvas_page (self);
    if (page)
      paint_canvas_set_alt_eyedropper (paint_canvas_page_get_canvas (page), TRUE);
  } else if (keyval == GDK_KEY_space) {
    GtkWidget *focus = gtk_window_get_focus (GTK_WINDOW (self));
    if (focus && (GTK_IS_EDITABLE (focus) || GTK_IS_TEXT (focus) || GTK_IS_TEXT_VIEW (focus)))
      return FALSE;
    PaintCanvasPage *page = get_active_canvas_page (self);
    PaintCanvas *canvas = page ? paint_canvas_page_get_canvas (page) : NULL;
    if (canvas && paint_canvas_is_drawing (canvas))
      return FALSE;
    if (page) {
      paint_canvas_page_set_space_pan (page, TRUE);
      return TRUE;
    }
  }
  return FALSE;
}

static void
on_alt_key_released (GtkEventControllerKey *ctrl G_GNUC_UNUSED,
                     guint keyval,
                     guint keycode G_GNUC_UNUSED,
                     GdkModifierType state G_GNUC_UNUSED,
                     gpointer user_data)
{
  PaintWindow *self = PAINT_WINDOW (user_data);
  if (keyval == GDK_KEY_Alt_L || keyval == GDK_KEY_Alt_R) {
    PaintCanvasPage *page = get_active_canvas_page (self);
    if (page)
      paint_canvas_set_alt_eyedropper (paint_canvas_page_get_canvas (page), FALSE);
  } else if (keyval == GDK_KEY_space) {
    clear_all_space_pan (self);
  }
}

static void
on_window_is_active_changed (GObject *obj,
                             GParamSpec *pspec G_GNUC_UNUSED,
                             gpointer user_data G_GNUC_UNUSED)
{
  PaintWindow *self = PAINT_WINDOW (obj);
  if (!gtk_window_is_active (GTK_WINDOW (self))) {
    PaintCanvasPage *page = get_active_canvas_page (self);
    if (page)
      paint_canvas_set_alt_eyedropper (paint_canvas_page_get_canvas (page), FALSE);
    clear_all_space_pan (self);
  }
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
  gtk_widget_set_tooltip_text (zoom_out, _("Zoom Out (Ctrl+-)"));
  gtk_actionable_set_action_name (GTK_ACTIONABLE (zoom_out), "win.zoom-out");

  GtkWidget *pct = gtk_button_new_with_label ("100%");
  gtk_widget_add_css_class (pct, "flat");
  gtk_widget_add_css_class (pct, "paint-zoom-label");
  gtk_widget_set_tooltip_text (pct, _("Reset Zoom (Ctrl+0)"));
  gtk_actionable_set_action_name (GTK_ACTIONABLE (pct), "win.zoom-reset");
  self->zoom_pct_btn = GTK_BUTTON (pct);

  GtkWidget *zoom_in = gtk_button_new_from_icon_name ("zoom-in-symbolic");
  gtk_widget_add_css_class (zoom_in, "circular");
  gtk_widget_add_css_class (zoom_in, "flat");
  gtk_widget_set_tooltip_text (zoom_in, _("Zoom In (Ctrl++)"));
  gtk_actionable_set_action_name (GTK_ACTIONABLE (zoom_in), "win.zoom-in");

  gtk_box_append (GTK_BOX (row), zoom_out);
  gtk_box_append (GTK_BOX (row), pct);
  gtk_box_append (GTK_BOX (row), zoom_in);
  return row;
}

static GtkWidget *
build_primary_menu (PaintWindow *self)
{
  g_autoptr (GMenu) menu = g_menu_new ();

  PaintPreferences *prefs = paint_window_get_preferences (self);
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
  g_menu_append (file_section, _("New Window"), "app.new-window");
  g_menu_append (file_section, _("Save"), "win.save");
  g_menu_append (file_section, _("Save As…"), "win.save-as");
  g_menu_append (file_section, _("Export Image…"), "win.export-image");
  g_menu_append (file_section, _("Print…"), "win.print");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (file_section));

  g_autoptr (GMenu) actions_section = g_menu_new ();

  g_autoptr (GMenu) edit_sub = g_menu_new ();
  g_menu_append (edit_sub, _("Copy"), "win.copy");
  g_menu_append (edit_sub, _("Copy Merged"), "win.copy-merged");
  g_menu_append (edit_sub, _("Cut"), "win.cut");
  g_menu_append (edit_sub, _("Paste"), "win.paste");
  g_menu_append (edit_sub, _("Paste as New Layer"), "win.paste-as-layer");
  g_menu_append (edit_sub, _("Paste as New Canvas"), "win.paste-new-tab");
  g_menu_append (edit_sub, _("Select All"), "win.select-all");
  g_menu_append (edit_sub, _("Invert Selection"), "win.invert-selection");
  g_menu_append_submenu (actions_section, _("Edit"), G_MENU_MODEL (edit_sub));

  g_autoptr (GMenu) canvas_sub = g_menu_new ();
  g_autoptr (GMenu) resize_sec = g_menu_new ();
  g_menu_append (resize_sec, _("Resize & Scale…"), "win.resize-canvas");
  g_menu_append_section (canvas_sub, NULL, G_MENU_MODEL (resize_sec));

  g_autoptr (GMenu) transform_sec = g_menu_new ();
  g_menu_append (transform_sec, _("Rotate Right 90°"), "win.rotate-cw");
  g_menu_append (transform_sec, _("Rotate Left 90°"), "win.rotate-ccw");
  g_menu_append (transform_sec, _("Flip Horizontal"), "win.flip-horizontal");
  g_menu_append (transform_sec, _("Flip Vertical"), "win.flip-vertical");
  g_menu_append (transform_sec, _("Crop to Selection"), "win.crop-selection");
  g_menu_append (transform_sec, _("Trim Transparent"), "win.trim-transparent");
  g_menu_append_section (canvas_sub, NULL, G_MENU_MODEL (transform_sec));

  g_autoptr (GMenu) clear_sec = g_menu_new ();
  g_menu_append (clear_sec, _("Clear Canvas"), "win.clear");
  g_menu_append_section (canvas_sub, NULL, G_MENU_MODEL (clear_sec));

  g_autoptr (GMenu) layers_sec = g_menu_new ();
  g_menu_append (layers_sec, _("New Layer"), "win.new-layer");
  g_menu_append (layers_sec, _("Import Image as Layer…"), "win.import-layer");
  g_menu_append (layers_sec, _("Merge Layer Down"), "win.merge-layer-down");
  g_menu_append (layers_sec, _("Flatten Layers"), "win.flatten-layers");
  g_menu_append_section (canvas_sub, _("Layers"), G_MENU_MODEL (layers_sec));
  g_menu_append_submenu (actions_section, _("Canvas"), G_MENU_MODEL (canvas_sub));

  g_autoptr (GMenu) filters_sub = g_menu_new ();
  g_autoptr (GMenu) adjust_sec = g_menu_new ();
  g_menu_append (adjust_sec, _("Brightness & Contrast…"), "win.filter-brightness-contrast");
  g_menu_append (adjust_sec, _("Hue & Saturation…"), "win.filter-hsl");
  g_menu_append (adjust_sec, _("Gaussian Blur…"), "win.filter-blur");
  g_menu_append_section (filters_sub, NULL, G_MENU_MODEL (adjust_sec));

  g_autoptr (GMenu) effects_sec = g_menu_new ();
  g_menu_append (effects_sec, _("Sharpen"), "win.filter-sharpen");
  g_menu_append (effects_sec, _("Invert Colors"), "win.filter-invert");
  g_menu_append (effects_sec, _("Grayscale"), "win.filter-grayscale");
  g_menu_append_section (filters_sub, NULL, G_MENU_MODEL (effects_sec));
  g_menu_append_submenu (actions_section, _("Filters"), G_MENU_MODEL (filters_sub));

  g_autoptr (GMenu) view_sub = g_menu_new ();
  g_menu_append (view_sub, _("Fit to Window"), "win.zoom-fit");
  g_menu_append (view_sub, _("Layers"), "win.toggle-layers");
  g_menu_append (view_sub, _("Pixel Grid"), "win.toggle-pixel-grid");
  g_menu_append (view_sub, _("Toggle Toolbar"), "win.toggle-toolbar");
  g_menu_append (view_sub, _("Fullscreen"), "win.toggle-fullscreen");
  g_menu_append_submenu (actions_section, _("View"), G_MENU_MODEL (view_sub));

  g_menu_append_section (menu, NULL, G_MENU_MODEL (actions_section));

  g_autoptr (GMenu) app_section = g_menu_new ();
  g_menu_append (app_section, _("Preferences"), "app.preferences");
  g_menu_append (app_section, _("Keyboard Shortcuts"), "app.shortcuts");
  g_menu_append (app_section, _("About Paint"), "app.about");
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

  GtkWidget *heading = gtk_label_new (_("Canvas Properties"));
  gtk_widget_add_css_class (heading, "heading");
  gtk_widget_set_halign (heading, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (box), heading);

  GtkWidget *grid = gtk_grid_new ();
  gtk_grid_set_column_spacing (GTK_GRID (grid), 18);
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);

  self->info_dims_label = add_info_row (GTK_GRID (grid), 0, _("Dimensions"));
  self->info_layers_label = add_info_row (GTK_GRID (grid), 1, _("Layers"));
  self->info_zoom_label = add_info_row (GTK_GRID (grid), 2, _("Zoom"));
  self->info_bg_label = add_info_row (GTK_GRID (grid), 3, _("Background"));
  self->info_path_label = add_info_row (GTK_GRID (grid), 4, _("Location"));
  gtk_label_set_max_width_chars (self->info_path_label, 28);
  gtk_label_set_ellipsize (self->info_path_label, PANGO_ELLIPSIZE_END);
  gtk_box_append (GTK_BOX (box), grid);

  GtkWidget *resize_btn = gtk_button_new_with_label (_("Resize & Scale…"));
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
  self->window_title = ADW_WINDOW_TITLE (adw_window_title_new (_("Untitled Canvas"), "640 × 380 · 100%"));
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), GTK_WIDGET (self->window_title));

  self->open_btn = ADW_SPLIT_BUTTON (adw_split_button_new ());
  adw_split_button_set_label (self->open_btn, _("Open"));
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->open_btn), _("Open Image (Ctrl+O)"));
  adw_split_button_set_dropdown_tooltip (self->open_btn, _("Recent Images"));
  g_signal_connect_swapped (self->open_btn, "clicked", G_CALLBACK (prompt_open_file), self);
  refresh_recent_menu (self);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), GTK_WIDGET (self->open_btn));

  GtkWidget *new_btn = gtk_button_new_from_icon_name ("tab-new-symbolic");
  gtk_widget_set_tooltip_text (new_btn, _("New Canvas (Ctrl+N)"));
  gtk_actionable_set_action_name (GTK_ACTIONABLE (new_btn), "win.new-tab");
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), new_btn);

  GtkWidget *menu_btn = gtk_menu_button_new ();
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (menu_btn), "open-menu-symbolic");
  gtk_menu_button_set_primary (GTK_MENU_BUTTON (menu_btn), TRUE);
  gtk_widget_set_tooltip_text (menu_btn, _("Main Menu (F10)"));
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (menu_btn), build_primary_menu (self));
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), menu_btn);

  GtkWidget *overview_btn = gtk_button_new_from_icon_name ("view-grid-symbolic");
  gtk_widget_set_tooltip_text (overview_btn, _("Tab Overview (Ctrl+Shift+O)"));
  gtk_actionable_set_action_name (GTK_ACTIONABLE (overview_btn), "win.tab-overview");
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), overview_btn);

  GtkWidget *info_btn = gtk_menu_button_new ();
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (info_btn), "info-outline-symbolic");
  gtk_widget_set_tooltip_text (info_btn, _("Canvas Details"));
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (info_btn), build_info_popover (self));
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), info_btn);

  GtkWidget *layers_btn = gtk_toggle_button_new ();
  gtk_button_set_icon_name (GTK_BUTTON (layers_btn), "layers-symbolic");
  gtk_widget_set_tooltip_text (layers_btn, _("Layers (Ctrl+L)"));
  gtk_actionable_set_action_name (GTK_ACTIONABLE (layers_btn), "win.toggle-layers");
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), layers_btn);

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
  PaintWindow *self = PAINT_WINDOW (user_data);
  clear_all_space_pan (self);
  paint_window_sync_state (self);
}

static void
on_page_state_changed (PaintCanvasPage *page G_GNUC_UNUSED, gpointer user_data)
{
  PaintWindow *self = PAINT_WINDOW (user_data);
  sync_tool_settings_colors_to_prefs (self);
  paint_window_sync_state (self);
}

static void
on_page_palette_changed (PaintCanvasPage *page G_GNUC_UNUSED, gpointer user_data)
{
  PaintWindow *self = PAINT_WINDOW (user_data);
  sync_tool_settings_colors_to_prefs (self);
  PaintPreferences *prefs = paint_window_get_preferences (self);
  if (prefs)
    paint_preferences_save (prefs);
  sync_all_color_pickers (self);
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
    g_signal_connect (child, "palette-changed", G_CALLBACK (on_page_palette_changed), self);
    update_tab_metadata (tab_page, PAINT_CANVAS_PAGE (child));
  }
  paint_window_sync_state (self);
}

static void
on_tab_page_detached (AdwTabView *view G_GNUC_UNUSED,
                      AdwTabPage *tab_page,
                      gint position G_GNUC_UNUSED,
                      gpointer user_data)
{
  PaintWindow *self = PAINT_WINDOW (user_data);
  GtkWidget *child = adw_tab_page_get_child (tab_page);
  if (PAINT_IS_CANVAS_PAGE (child)) {
    g_signal_handlers_disconnect_by_func (child, G_CALLBACK (on_page_state_changed), self);
    g_signal_handlers_disconnect_by_func (child, G_CALLBACK (on_page_palette_changed), self);
  }
  paint_window_sync_state (self);
  g_idle_add (close_if_empty_after_detach_idle, self);
}

typedef struct {
  PaintWindow *window;
  GPtrArray *files; /* owned GFile* */
} DropPromptCtx;

static void
drop_prompt_ctx_free (DropPromptCtx *ctx)
{
  if (!ctx)
    return;
  g_clear_pointer (&ctx->files, g_ptr_array_unref);
  g_clear_object (&ctx->window);
  g_free (ctx);
}

static void
on_drop_prompt_response (GObject *source, GAsyncResult *res, gpointer user_data)
{
  DropPromptCtx *ctx = user_data;
  const char *choice = adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), res);
  if (g_strcmp0 (choice, "add-layer") == 0) {
    for (guint i = 0; i < ctx->files->len; i++)
      import_file_as_layer (ctx->window, G_FILE (g_ptr_array_index (ctx->files, i)));
  } else if (g_strcmp0 (choice, "new-tab") == 0) {
    for (guint i = 0; i < ctx->files->len; i++)
      paint_window_open_file (ctx->window, G_FILE (g_ptr_array_index (ctx->files, i)));
  }
  drop_prompt_ctx_free (ctx);
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

  gboolean has_non_ora = FALSE;
  g_autoptr (GPtrArray) list = g_ptr_array_new_with_free_func (g_object_unref);
  for (GSList *l = files; l != NULL; l = l->next) {
    if (G_IS_FILE (l->data)) {
      GFile *f = G_FILE (l->data);
      g_ptr_array_add (list, g_object_ref (f));
      if (!paint_io_is_ora_file (f))
        has_non_ora = TRUE;
    }
  }
  g_slist_free (files);
  if (list->len == 0)
    return FALSE;

  PaintCanvasPage *page = get_active_canvas_page (self);
  PaintDocument *doc = page ? paint_canvas_get_document (paint_canvas_page_get_canvas (page)) : NULL;
  gboolean non_blank = doc &&
                       (paint_document_get_modified (doc) ||
                        paint_document_get_file (doc) != NULL ||
                        paint_document_can_undo (doc) ||
                        paint_document_get_n_layers (doc) > 1);

  if (non_blank && has_non_ora) {
    DropPromptCtx *ctx = g_new0 (DropPromptCtx, 1);
    ctx->window = g_object_ref (self);
    ctx->files = g_ptr_array_ref (list);

    AdwDialog *dlg = adw_alert_dialog_new (
        _("Add Dropped Image as Layer?"),
        _("Would you like to add the dropped image as a new layer in the current canvas, "
          "or open it in a new tab?"));
    adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dlg),
                                    "cancel", _("Cancel"),
                                    "new-tab", _("Open in New Tab"),
                                    "add-layer", _("Add as Layer"),
                                    NULL);
    adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dlg),
                                              "add-layer",
                                              ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dlg), "add-layer");
    adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dlg), "cancel");
    adw_alert_dialog_choose (ADW_ALERT_DIALOG (dlg),
                             GTK_WIDGET (self),
                             NULL,
                             on_drop_prompt_response,
                             ctx);
    return TRUE;
  }

  for (guint i = 0; i < list->len; i++)
    paint_window_open_file (self, G_FILE (g_ptr_array_index (list, i)));
  return TRUE;
}

void
paint_window_apply_preferences (PaintWindow *self)
{
  PaintPreferences *prefs = paint_window_get_preferences (self);
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
  PaintPreferences *prefs = paint_window_get_preferences (self);
  if (!doc && prefs)
    doc = paint_document_new (prefs->default_width, prefs->default_height);

  PaintCanvasPage *page = paint_canvas_page_new (doc, self->shared_settings);
  if (prefs) {
    PaintCanvas *canvas = paint_canvas_page_get_canvas (page);
    paint_canvas_set_show_cursor_ring (canvas, prefs->show_cursor_ring);
    paint_canvas_set_show_pixel_grid (canvas, prefs->show_pixel_grid);
    paint_canvas_page_set_osd_visible (page, prefs->show_toolbar);
  }
  paint_canvas_page_set_layers_visible (page, self->layers_visible);

  AdwTabPage *tab = adw_tab_view_append (self->tab_view, GTK_WIDGET (page));
  adw_tab_view_set_selected_page (self->tab_view, tab);
  return tab;
}

static void
paint_window_dispose (GObject *object)
{
  PaintWindow *self = PAINT_WINDOW (object);
  if (self->startup_fit_tick_id != 0) {
    gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->startup_fit_tick_id);
    self->startup_fit_tick_id = 0;
  }
  G_OBJECT_CLASS (paint_window_parent_class)->dispose (object);
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
  object_class->dispose = paint_window_dispose;
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
    { "export-image", act_export_image, NULL, NULL, NULL, { 0 } },
    { "print", act_print, NULL, NULL, NULL, { 0 } },
    { "undo", act_undo, NULL, NULL, NULL, { 0 } },
    { "redo", act_redo, NULL, NULL, NULL, { 0 } },
    { "copy", act_copy, NULL, NULL, NULL, { 0 } },
    { "copy-merged", act_copy_merged, NULL, NULL, NULL, { 0 } },
    { "cut", act_cut, NULL, NULL, NULL, { 0 } },
    { "paste", act_paste, NULL, NULL, NULL, { 0 } },
    { "paste-as-layer", act_paste_as_layer, NULL, NULL, NULL, { 0 } },
    { "paste-new-tab", act_paste_new_tab, NULL, NULL, NULL, { 0 } },
    { "select-all", act_select_all, NULL, NULL, NULL, { 0 } },
    { "invert-selection", act_invert_selection, NULL, NULL, NULL, { 0 } },
    { "delete-selection", act_delete_selection, NULL, NULL, NULL, { 0 } },
    { "crop-selection", act_crop_selection, NULL, NULL, NULL, { 0 } },
    { "rotate-cw", act_rotate_cw, NULL, NULL, NULL, { 0 } },
    { "rotate-ccw", act_rotate_ccw, NULL, NULL, NULL, { 0 } },
    { "flip-horizontal", act_flip_horizontal, NULL, NULL, NULL, { 0 } },
    { "flip-vertical", act_flip_vertical, NULL, NULL, NULL, { 0 } },
    { "trim-transparent", act_trim_transparent, NULL, NULL, NULL, { 0 } },
    { "filter-brightness-contrast", act_filter_brightness_contrast, NULL, NULL, NULL, { 0 } },
    { "filter-hsl", act_filter_hsl, NULL, NULL, NULL, { 0 } },
    { "filter-blur", act_filter_blur, NULL, NULL, NULL, { 0 } },
    { "filter-sharpen", act_filter_sharpen, NULL, NULL, NULL, { 0 } },
    { "filter-invert", act_filter_invert, NULL, NULL, NULL, { 0 } },
    { "filter-grayscale", act_filter_grayscale, NULL, NULL, NULL, { 0 } },
    { "clear", act_clear, NULL, NULL, NULL, { 0 } },
    { "resize-canvas", act_resize, NULL, NULL, NULL, { 0 } },
    { "zoom-in", act_zoom_in, NULL, NULL, NULL, { 0 } },
    { "zoom-out", act_zoom_out, NULL, NULL, NULL, { 0 } },
    { "zoom-reset", act_zoom_reset, NULL, NULL, NULL, { 0 } },
    { "zoom-fit", act_zoom_fit, NULL, NULL, NULL, { 0 } },
    { "toggle-pixel-grid", NULL, NULL, "true", act_toggle_pixel_grid_state, { 0 } },
    { "toggle-layers", NULL, NULL, "false", act_toggle_layers_state, { 0 } },
    { "new-layer", act_new_layer, NULL, NULL, NULL, { 0 } },
    { "import-layer", act_import_layer, NULL, NULL, NULL, { 0 } },
    { "duplicate-layer", act_duplicate_layer, NULL, NULL, NULL, { 0 } },
    { "merge-layer-down", act_merge_layer_down, NULL, NULL, NULL, { 0 } },
    { "flatten-layers", act_flatten_layers, NULL, NULL, NULL, { 0 } },
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
    { "tool-gradient", act_tool_gradient, NULL, NULL, NULL, { 0 } },
    { "tool-blur", act_tool_blur, NULL, NULL, NULL, { 0 } },
    { "tool-pixelate", act_tool_pixelate, NULL, NULL, NULL, { 0 } },
    { "brush-smaller", act_brush_smaller, NULL, NULL, NULL, { 0 } },
    { "brush-larger", act_brush_larger, NULL, NULL, NULL, { 0 } },
    { "swap-colors", act_swap_colors, NULL, NULL, NULL, { 0 } },
    { "reset-colors", act_reset_colors, NULL, NULL, NULL, { 0 } },
  };

  GApplication *app = g_application_get_default ();
  self->shared_settings = (app && PAINT_IS_APPLICATION (app))
                            ? paint_application_get_tool_settings (PAINT_APPLICATION (app))
                            : paint_tool_settings_new ();
  self->recent_paths = g_ptr_array_new_with_free_func (g_free);
  load_recent_files (self);

  gtk_window_set_title (GTK_WINDOW (self), _("Paint"));
  gtk_window_set_icon_name (GTK_WINDOW (self), PAINT_APP_ID);
  gtk_window_set_default_size (GTK_WINDOW (self),
                               PAINT_DEFAULT_WINDOW_WIDTH,
                               PAINT_DEFAULT_WINDOW_HEIGHT);

  g_action_map_add_action_entries (G_ACTION_MAP (self),
                                   win_actions,
                                   G_N_ELEMENTS (win_actions),
                                   self);

  PaintPreferences *prefs = paint_window_get_preferences (self);
  if (prefs) {
    GAction *grid_act = g_action_map_lookup_action (G_ACTION_MAP (self), "toggle-pixel-grid");
    if (grid_act)
      g_simple_action_set_state (G_SIMPLE_ACTION (grid_act),
                                 g_variant_new_boolean (prefs->show_pixel_grid));
  }

  GtkEventController *key_ctrl = gtk_event_controller_key_new ();
  g_signal_connect (key_ctrl, "key-pressed", G_CALLBACK (on_key_pressed), self);
  gtk_widget_add_controller (GTK_WIDGET (self), key_ctrl);

  GtkEventController *alt_ctrl = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (alt_ctrl, GTK_PHASE_CAPTURE);
  g_signal_connect (alt_ctrl, "key-pressed", G_CALLBACK (on_alt_key_pressed), self);
  g_signal_connect (alt_ctrl, "key-released", G_CALLBACK (on_alt_key_released), self);
  gtk_widget_add_controller (GTK_WIDGET (self), alt_ctrl);
  g_signal_connect (self, "notify::is-active", G_CALLBACK (on_window_is_active_changed), NULL);

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
  paint_window_apply_startup_sizing (self);
  return self;
}

AdwTabView *
paint_window_get_tab_view (PaintWindow *self)
{
  return self->tab_view;
}
