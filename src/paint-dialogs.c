#include "paint-dialogs.h"

#include <math.h>
#include <string.h>

#include "paint-i18n.h"

static void
on_clear_dialog_response (GObject *source, GAsyncResult *res, gpointer user_data)
{
  PaintCanvas *canvas = PAINT_CANVAS (user_data);
  if (g_strcmp0 (adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), res), "clear") == 0)
    paint_canvas_clear (canvas);
}

void
paint_dialog_confirm_clear (GtkWidget *parent, PaintCanvas *canvas)
{
  if (!canvas)
    return;
  PaintDocument *doc = paint_canvas_get_document (canvas);
  const char *body = paint_document_has_transparent_background (doc)
                       ? _("This will clear the entire canvas to transparent. You can still undo this action.")
                       : _("This will fill the entire canvas with the background color. You can still undo this action.");
  AdwDialog *dlg = adw_alert_dialog_new (_("Clear Canvas?"), body);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dlg),
                                  "cancel", _("Cancel"),
                                  "clear", _("Clear"),
                                  NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dlg),
                                            "clear",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dlg), "cancel");
  adw_alert_dialog_choose (ADW_ALERT_DIALOG (dlg),
                           parent,
                           NULL,
                           on_clear_dialog_response,
                           canvas);
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
    gtk_button_set_label (GTK_BUTTON (ctx->apply_btn), (mode == 1) ? _("Scale") : _("Resize"));
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

void
paint_dialog_show_resize (GtkWidget *parent, PaintCanvas *canvas)
{
  if (!canvas)
    return;
  PaintDocument *doc = paint_canvas_get_document (canvas);
  int orig_w = paint_document_get_width (doc);
  int orig_h = paint_document_get_height (doc);

  AdwDialog *dialog = adw_dialog_new ();
  adw_dialog_set_title (dialog, _("Resize & Scale"));
  adw_dialog_set_content_width (dialog, 360);

  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_show_start_title_buttons (ADW_HEADER_BAR (header), FALSE);
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);

  GtkWidget *cancel_btn = gtk_button_new_with_label (_("Cancel"));
  g_signal_connect_swapped (cancel_btn, "clicked", G_CALLBACK (adw_dialog_close), dialog);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), cancel_btn);

  GtkWidget *apply_btn = gtk_button_new_with_label (_("Resize"));
  gtk_widget_add_css_class (apply_btn, "suggested-action");
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), apply_btn);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 14);
  gtk_widget_set_margin_top (content, 16);
  gtk_widget_set_margin_bottom (content, 16);
  gtk_widget_set_margin_start (content, 16);
  gtk_widget_set_margin_end (content, 16);

  GtkWidget *group = adw_preferences_group_new ();

  const char *modes[] = { _("Crop / Pad Canvas"), _("Scale Image"), NULL };
  GtkWidget *mode_row = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (mode_row), _("Mode"));
  g_autoptr (GtkStringList) mode_model = gtk_string_list_new (modes);
  adw_combo_row_set_model (ADW_COMBO_ROW (mode_row), G_LIST_MODEL (mode_model));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), mode_row);

  GtkWidget *aspect_row = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (aspect_row), _("Keep Aspect Ratio"));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), aspect_row);

  GtkWidget *w_row = adw_spin_row_new_with_range (1.0, 8192.0, 16.0);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (w_row), _("Width (px)"));
  adw_spin_row_set_value (ADW_SPIN_ROW (w_row), (double) orig_w);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), w_row);

  GtkWidget *h_row = adw_spin_row_new_with_range (1.0, 8192.0, 16.0);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (h_row), _("Height (px)"));
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
  adw_dialog_present (dialog, parent);
}

/* ============================================================================
 * Export Image Dialog
 * ============================================================================ */

typedef struct {
  GtkWidget *parent;
  PaintDocument *doc;
  AdwDialog *dialog;
  AdwComboRow *format_row;
  AdwSpinRow *quality_row;
  AdwComboRow *matte_row;
  GtkColorDialogButton *matte_color_btn;
  gboolean has_transparency;
  PaintExportDoneCallback on_exported;
  gpointer user_data;
} ExportDialogCtx;

static const char *const EXPORT_FORMATS[] = { "png", "jpeg", "webp", "bmp" };
static const char *const EXPORT_EXTS[] = { ".png", ".jpg", ".webp", ".bmp" };

static void
sync_export_rows_visibility (ExportDialogCtx *ctx)
{
  guint fmt_idx = adw_combo_row_get_selected (ctx->format_row);
  gboolean has_quality = (fmt_idx == 1 || fmt_idx == 2); /* JPEG / WebP */
  gboolean is_jpeg = (fmt_idx == 1);
  gtk_widget_set_visible (GTK_WIDGET (ctx->quality_row), has_quality);
  gtk_widget_set_visible (GTK_WIDGET (ctx->matte_row), is_jpeg && ctx->has_transparency);

  guint matte_idx = adw_combo_row_get_selected (ctx->matte_row);
  gtk_widget_set_visible (GTK_WIDGET (ctx->matte_color_btn),
                          is_jpeg && ctx->has_transparency && matte_idx == 2);
}

static void
on_export_format_changed (GObject *obj G_GNUC_UNUSED,
                          GParamSpec *pspec G_GNUC_UNUSED,
                          gpointer user_data)
{
  sync_export_rows_visibility (user_data);
}

static void
on_export_matte_changed (GObject *obj G_GNUC_UNUSED,
                         GParamSpec *pspec G_GNUC_UNUSED,
                         gpointer user_data)
{
  sync_export_rows_visibility (user_data);
}

typedef struct {
  GtkWidget *parent;
  PaintDocument *doc;
  PaintExportOptions options;
  PaintExportDoneCallback on_exported;
  gpointer user_data;
} ExportFileSaveCtx;

static void
on_export_file_save_finish (GObject *source, GAsyncResult *res, gpointer user_data)
{
  ExportFileSaveCtx *sctx = user_data;
  GtkFileDialog *file_dlg = GTK_FILE_DIALOG (source);
  g_autoptr (GFile) file = gtk_file_dialog_save_finish (file_dlg, res, NULL);
  if (file) {
    g_autoptr (GError) error = NULL;
    if (paint_document_export_to_file (sctx->doc, file, &sctx->options, &error)) {
      if (sctx->on_exported)
        sctx->on_exported (file, sctx->user_data);
    } else {
      AdwDialog *err = adw_alert_dialog_new (_("Could Not Export Image"),
                                             error ? error->message : _("Unknown error"));
      adw_alert_dialog_add_response (ADW_ALERT_DIALOG (err), "ok", _("OK"));
      adw_dialog_present (err, sctx->parent);
    }
  }
  g_free (sctx);
}

static char *
build_suggested_export_name (PaintDocument *doc, const char *ext)
{
  GFile *orig = paint_document_get_file (doc);
  g_autofree char *base = orig ? g_file_get_basename (orig) : g_strdup ("Untitled");
  char *dot = strrchr (base, '.');
  if (dot && dot != base)
    *dot = '\0';
  return g_strconcat (base, ext, NULL);
}

static void
on_export_dialog_confirm (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  ExportDialogCtx *ctx = user_data;
  guint fmt_idx = MIN (adw_combo_row_get_selected (ctx->format_row),
                       (guint) G_N_ELEMENTS (EXPORT_FORMATS) - 1);

  ExportFileSaveCtx *sctx = g_new0 (ExportFileSaveCtx, 1);
  sctx->parent = ctx->parent;
  sctx->doc = ctx->doc;
  sctx->on_exported = ctx->on_exported;
  sctx->user_data = ctx->user_data;
  sctx->options.format = EXPORT_FORMATS[fmt_idx];
  sctx->options.quality = (int) lround (adw_spin_row_get_value (ctx->quality_row));

  guint matte_idx = adw_combo_row_get_selected (ctx->matte_row);
  if (matte_idx == 1) {
    sctx->options.matte_color = (GdkRGBA) { 0.0f, 0.0f, 0.0f, 1.0f };
  } else if (matte_idx == 2) {
    const GdkRGBA *picked = gtk_color_dialog_button_get_rgba (ctx->matte_color_btn);
    sctx->options.matte_color = picked ? *picked : (GdkRGBA) { 1.0f, 1.0f, 1.0f, 1.0f };
    sctx->options.matte_color.alpha = 1.0f;
  } else {
    sctx->options.matte_color = (GdkRGBA) { 1.0f, 1.0f, 1.0f, 1.0f };
  }

  g_autoptr (GtkFileDialog) fdlg = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (fdlg, _("Export Image"));
  g_autofree char *init_name = build_suggested_export_name (ctx->doc, EXPORT_EXTS[fmt_idx]);
  gtk_file_dialog_set_initial_name (fdlg, init_name);

  g_autoptr (GListStore) filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
  g_autoptr (GtkFileFilter) fmt_filter = gtk_file_filter_new ();
  if (fmt_idx == 0) {
    gtk_file_filter_set_name (fmt_filter, _("PNG Image (*.png)"));
    gtk_file_filter_add_pattern (fmt_filter, "*.png");
  } else if (fmt_idx == 1) {
    gtk_file_filter_set_name (fmt_filter, _("JPEG Image (*.jpg, *.jpeg)"));
    gtk_file_filter_add_pattern (fmt_filter, "*.jpg");
    gtk_file_filter_add_pattern (fmt_filter, "*.jpeg");
  } else if (fmt_idx == 2) {
    gtk_file_filter_set_name (fmt_filter, _("WebP Image (*.webp)"));
    gtk_file_filter_add_pattern (fmt_filter, "*.webp");
  } else {
    gtk_file_filter_set_name (fmt_filter, _("BMP Image (*.bmp)"));
    gtk_file_filter_add_pattern (fmt_filter, "*.bmp");
  }
  g_list_store_append (filters, fmt_filter);
  gtk_file_dialog_set_filters (fdlg, G_LIST_MODEL (filters));

  GtkWindow *win = GTK_IS_WINDOW (ctx->parent) ? GTK_WINDOW (ctx->parent) : NULL;
  adw_dialog_close (ctx->dialog);
  gtk_file_dialog_save (fdlg, win, NULL, on_export_file_save_finish, sctx);
}

void
paint_dialog_show_export (GtkWidget *parent,
                          PaintDocument *doc,
                          PaintExportDoneCallback on_exported,
                          gpointer user_data)
{
  if (!doc)
    return;

  AdwDialog *dialog = adw_dialog_new ();
  adw_dialog_set_title (dialog, _("Export Image"));
  adw_dialog_set_content_width (dialog, 360);

  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_show_start_title_buttons (ADW_HEADER_BAR (header), FALSE);
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);

  GtkWidget *cancel_btn = gtk_button_new_with_label (_("Cancel"));
  g_signal_connect_swapped (cancel_btn, "clicked", G_CALLBACK (adw_dialog_close), dialog);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), cancel_btn);

  GtkWidget *export_btn = gtk_button_new_with_label (_("Export…"));
  gtk_widget_add_css_class (export_btn, "suggested-action");
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), export_btn);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top (content, 16);
  gtk_widget_set_margin_bottom (content, 16);
  gtk_widget_set_margin_start (content, 16);
  gtk_widget_set_margin_end (content, 16);

  GtkWidget *group = adw_preferences_group_new ();

  const char *fmt_labels[] = {
    _("PNG (Lossless, Transparency)"),
    _("JPEG (Compact Photo)"),
    _("WebP (Modern, Transparency)"),
    _("BMP (Uncompressed)"),
    NULL,
  };
  GtkWidget *format_row = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (format_row), _("Format"));
  g_autoptr (GtkStringList) fmt_model = gtk_string_list_new (fmt_labels);
  adw_combo_row_set_model (ADW_COMBO_ROW (format_row), G_LIST_MODEL (fmt_model));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), format_row);

  GtkWidget *quality_row = adw_spin_row_new_with_range (1.0, 100.0, 2.0);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (quality_row), _("Quality"));
  adw_action_row_set_subtitle (ADW_ACTION_ROW (quality_row), _("Higher values preserve more detail"));
  adw_spin_row_set_value (ADW_SPIN_ROW (quality_row), 92.0);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), quality_row);

  const char *matte_labels[] = { _("White"), _("Black"), _("Custom Color"), NULL };
  GtkWidget *matte_row = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (matte_row), _("Background Matte"));
  adw_action_row_set_subtitle (ADW_ACTION_ROW (matte_row),
                               _("Replaces transparent areas in JPEG exports"));
  g_autoptr (GtkStringList) matte_model = gtk_string_list_new (matte_labels);
  adw_combo_row_set_model (ADW_COMBO_ROW (matte_row), G_LIST_MODEL (matte_model));

  g_autoptr (GtkColorDialog) color_dlg = gtk_color_dialog_new ();
  gtk_color_dialog_set_with_alpha (color_dlg, FALSE);
  GtkWidget *matte_btn = gtk_color_dialog_button_new (color_dlg);
  GdkRGBA default_matte = { 1.0f, 1.0f, 1.0f, 1.0f };
  gtk_color_dialog_button_set_rgba (GTK_COLOR_DIALOG_BUTTON (matte_btn), &default_matte);
  gtk_widget_set_valign (matte_btn, GTK_ALIGN_CENTER);
  adw_action_row_add_suffix (ADW_ACTION_ROW (matte_row), matte_btn);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), matte_row);

  gtk_box_append (GTK_BOX (content), group);

  ExportDialogCtx *ctx = g_new0 (ExportDialogCtx, 1);
  ctx->parent = parent;
  ctx->doc = doc;
  ctx->dialog = dialog;
  ctx->format_row = ADW_COMBO_ROW (format_row);
  ctx->quality_row = ADW_SPIN_ROW (quality_row);
  ctx->matte_row = ADW_COMBO_ROW (matte_row);
  ctx->matte_color_btn = GTK_COLOR_DIALOG_BUTTON (matte_btn);
  ctx->has_transparency = paint_surface_has_transparency (paint_document_get_surface (doc));
  ctx->on_exported = on_exported;
  ctx->user_data = user_data;
  g_object_set_data_full (G_OBJECT (dialog), "export-ctx", ctx, g_free);

  g_signal_connect (format_row, "notify::selected", G_CALLBACK (on_export_format_changed), ctx);
  g_signal_connect (matte_row, "notify::selected", G_CALLBACK (on_export_matte_changed), ctx);
  g_signal_connect (export_btn, "clicked", G_CALLBACK (on_export_dialog_confirm), ctx);
  sync_export_rows_visibility (ctx);

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), content);
  adw_dialog_set_child (dialog, toolbar);
  adw_dialog_present (dialog, parent);
}

/* ============================================================================
 * Live-Preview Filter Dialogs
 * ============================================================================ */

typedef struct {
  const char *label;
  double min;
  double max;
  double step;
  double def_val;
  int digits;
} FilterSliderSpec;

typedef struct {
  PaintCanvas *canvas;
  AdwDialog *dialog;
  GtkScale *scales[3];
  double defaults[3];
  int n_scales;
  PaintCanvasFilterFunc apply_fn;
  gboolean updating;
  gboolean applied;
  guint preview_source_id;
} FilterDialogCtx;

static void
filter_dialog_ctx_free (gpointer data)
{
  FilterDialogCtx *ctx = data;
  if (!ctx)
    return;
  g_clear_handle_id (&ctx->preview_source_id, g_source_remove);
  g_free (ctx);
}

static void
run_filter_preview_now (FilterDialogCtx *ctx)
{
  g_clear_handle_id (&ctx->preview_source_id, g_source_remove);
  paint_canvas_update_filter_preview (ctx->canvas, ctx->apply_fn, ctx);
}

static gboolean
on_filter_preview_timeout (gpointer user_data)
{
  FilterDialogCtx *ctx = user_data;
  ctx->preview_source_id = 0;
  paint_canvas_update_filter_preview (ctx->canvas, ctx->apply_fn, ctx);
  return G_SOURCE_REMOVE;
}

static void
on_filter_slider_changed (GtkRange *range G_GNUC_UNUSED, gpointer user_data)
{
  FilterDialogCtx *ctx = user_data;
  if (ctx->updating)
    return;
  g_clear_handle_id (&ctx->preview_source_id, g_source_remove);
  ctx->preview_source_id = g_timeout_add (16, on_filter_preview_timeout, ctx);
}

static void
on_filter_reset_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  FilterDialogCtx *ctx = user_data;
  ctx->updating = TRUE;
  for (int i = 0; i < ctx->n_scales; i++)
    gtk_range_set_value (GTK_RANGE (ctx->scales[i]), ctx->defaults[i]);
  ctx->updating = FALSE;
  run_filter_preview_now (ctx);
}

static void
on_filter_apply_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  FilterDialogCtx *ctx = user_data;
  if (ctx->preview_source_id != 0)
    run_filter_preview_now (ctx);
  ctx->applied = TRUE;
  paint_canvas_commit_filter_preview (ctx->canvas);
  adw_dialog_close (ctx->dialog);
}

static void
on_filter_dialog_closed (AdwDialog *dlg G_GNUC_UNUSED, gpointer user_data)
{
  FilterDialogCtx *ctx = user_data;
  g_clear_handle_id (&ctx->preview_source_id, g_source_remove);
  if (!ctx->applied)
    paint_canvas_cancel_filter_preview (ctx->canvas);
}

static void
show_filter_dialog (GtkWidget *parent,
                    PaintCanvas *canvas,
                    const char *title,
                    const FilterSliderSpec *specs,
                    int n_specs,
                    PaintCanvasFilterFunc apply_fn,
                    gboolean preview_on_open)
{
  if (!canvas || !paint_canvas_begin_filter_preview (canvas))
    return;

  AdwDialog *dialog = adw_dialog_new ();
  adw_dialog_set_title (dialog, title);
  adw_dialog_set_content_width (dialog, 380);

  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_show_start_title_buttons (ADW_HEADER_BAR (header), FALSE);
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);

  GtkWidget *cancel_btn = gtk_button_new_with_label (_("Cancel"));
  g_signal_connect_swapped (cancel_btn, "clicked", G_CALLBACK (adw_dialog_close), dialog);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), cancel_btn);

  GtkWidget *apply_btn = gtk_button_new_with_label (_("Apply"));
  gtk_widget_add_css_class (apply_btn, "suggested-action");
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), apply_btn);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top (content, 16);
  gtk_widget_set_margin_bottom (content, 16);
  gtk_widget_set_margin_start (content, 16);
  gtk_widget_set_margin_end (content, 16);

  GtkWidget *group = adw_preferences_group_new ();
  FilterDialogCtx *ctx = g_new0 (FilterDialogCtx, 1);
  ctx->canvas = canvas;
  ctx->dialog = dialog;
  ctx->n_scales = MIN (n_specs, 3);
  ctx->apply_fn = apply_fn;

  for (int i = 0; i < ctx->n_scales; i++) {
    ctx->defaults[i] = specs[i].def_val;

    GtkWidget *row = adw_action_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), specs[i].label);

    GtkWidget *scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL,
                                                 specs[i].min,
                                                 specs[i].max,
                                                 specs[i].step);
    gtk_range_set_value (GTK_RANGE (scale), specs[i].def_val);
    gtk_scale_set_digits (GTK_SCALE (scale), specs[i].digits);
    gtk_scale_set_draw_value (GTK_SCALE (scale), TRUE);
    gtk_scale_set_value_pos (GTK_SCALE (scale), GTK_POS_RIGHT);
    gtk_widget_set_size_request (scale, 185, -1);
    gtk_widget_set_valign (scale, GTK_ALIGN_CENTER);
    g_signal_connect (scale, "value-changed", G_CALLBACK (on_filter_slider_changed), ctx);

    ctx->scales[i] = GTK_SCALE (scale);
    adw_action_row_add_suffix (ADW_ACTION_ROW (row), scale);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), row);
  }

  gtk_box_append (GTK_BOX (content), group);

  GtkWidget *reset_btn = gtk_button_new_with_label (_("Reset"));
  gtk_widget_set_halign (reset_btn, GTK_ALIGN_START);
  g_signal_connect (reset_btn, "clicked", G_CALLBACK (on_filter_reset_clicked), ctx);
  gtk_box_append (GTK_BOX (content), reset_btn);

  g_object_set_data_full (G_OBJECT (dialog), "filter-ctx", ctx, filter_dialog_ctx_free);
  g_signal_connect (apply_btn, "clicked", G_CALLBACK (on_filter_apply_clicked), ctx);
  g_signal_connect (dialog, "closed", G_CALLBACK (on_filter_dialog_closed), ctx);

  if (preview_on_open)
    run_filter_preview_now (ctx);

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), content);
  adw_dialog_set_child (dialog, toolbar);
  adw_dialog_present (dialog, parent);
}

static void
apply_bc_preview (cairo_surface_t *surf,
                  const cairo_rectangle_int_t *rect,
                  cairo_surface_t *mask,
                  gboolean alpha_lock,
                  gpointer user_data)
{
  FilterDialogCtx *ctx = user_data;
  double brightness = gtk_range_get_value (GTK_RANGE (ctx->scales[0])) / 100.0;
  double contrast = gtk_range_get_value (GTK_RANGE (ctx->scales[1])) / 100.0;
  paint_filter_brightness_contrast (surf, rect, mask, alpha_lock, brightness, contrast);
}

void
paint_dialog_show_brightness_contrast (GtkWidget *parent, PaintCanvas *canvas)
{
  const FilterSliderSpec specs[] = {
    { _("Brightness"), -100.0, 100.0, 1.0, 0.0, 0 },
    { _("Contrast"), -100.0, 100.0, 1.0, 0.0, 0 },
  };
  show_filter_dialog (parent,
                      canvas,
                      _("Brightness & Contrast"),
                      specs,
                      G_N_ELEMENTS (specs),
                      apply_bc_preview,
                      FALSE);
}

static void
apply_hsl_preview (cairo_surface_t *surf,
                   const cairo_rectangle_int_t *rect,
                   cairo_surface_t *mask,
                   gboolean alpha_lock,
                   gpointer user_data)
{
  FilterDialogCtx *ctx = user_data;
  double hue = gtk_range_get_value (GTK_RANGE (ctx->scales[0]));
  double sat = gtk_range_get_value (GTK_RANGE (ctx->scales[1])) / 100.0;
  double light = gtk_range_get_value (GTK_RANGE (ctx->scales[2])) / 100.0;
  paint_filter_hsl (surf, rect, mask, alpha_lock, hue, sat, light);
}

void
paint_dialog_show_hsl (GtkWidget *parent, PaintCanvas *canvas)
{
  const FilterSliderSpec specs[] = {
    { _("Hue"), -180.0, 180.0, 1.0, 0.0, 0 },
    { _("Saturation"), -100.0, 100.0, 1.0, 0.0, 0 },
    { _("Lightness"), -100.0, 100.0, 1.0, 0.0, 0 },
  };
  show_filter_dialog (parent,
                      canvas,
                      _("Hue & Saturation"),
                      specs,
                      G_N_ELEMENTS (specs),
                      apply_hsl_preview,
                      FALSE);
}

static void
apply_blur_preview (cairo_surface_t *surf,
                    const cairo_rectangle_int_t *rect,
                    cairo_surface_t *mask,
                    gboolean alpha_lock,
                    gpointer user_data)
{
  FilterDialogCtx *ctx = user_data;
  double radius = gtk_range_get_value (GTK_RANGE (ctx->scales[0]));
  paint_filter_gaussian_blur (surf, rect, mask, alpha_lock, radius);
}

void
paint_dialog_show_gaussian_blur (GtkWidget *parent, PaintCanvas *canvas)
{
  const FilterSliderSpec specs[] = {
    { _("Radius (px)"), 0.0, 50.0, 0.5, 3.0, 1 },
  };
  show_filter_dialog (parent,
                      canvas,
                      _("Gaussian Blur"),
                      specs,
                      G_N_ELEMENTS (specs),
                      apply_blur_preview,
                      TRUE);
}
