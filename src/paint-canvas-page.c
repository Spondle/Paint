#include "paint-canvas-page.h"

#include <math.h>

#define WORKSPACE_MARGIN_TOP 16
#define WORKSPACE_MARGIN_BOTTOM 72
#define WORKSPACE_MARGIN_START 24
#define WORKSPACE_MARGIN_END 24

static const int BRUSH_PRESETS[] = { 2, 6, 12, 24, 48 };

static const GdkRGBA COLOR_PRESETS[] = {
  { 0.00f, 0.00f, 0.00f, 1.0f }, /* Black */
  { 1.00f, 1.00f, 1.00f, 1.0f }, /* White */
  { 0.47f, 0.47f, 0.49f, 1.0f }, /* Gray */
  { 0.88f, 0.11f, 0.14f, 1.0f }, /* Red */
  { 1.00f, 0.47f, 0.00f, 1.0f }, /* Orange */
  { 0.96f, 0.83f, 0.18f, 1.0f }, /* Yellow */
  { 0.18f, 0.76f, 0.49f, 1.0f }, /* Green */
  { 0.11f, 0.44f, 0.85f, 1.0f }, /* Blue */
};

typedef enum {
  PICKER_SV,
  PICKER_HUE,
  PICKER_ALPHA
} PickerKind;

enum {
  SIGNAL_STATE_CHANGED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _PaintCanvasPage {
  AdwBin parent_instance;

  PaintToolSettings *settings; /* unowned */
  PaintCanvas *canvas;
  AdwToastOverlay *toast_overlay;
  AdwOverlaySplitView *split_view;
  GtkOverlay *overlay;
  GtkScrolledWindow *scrolled;

  GtkWidget *layers_list;
  GtkWidget *layers_add_btn;
  GtkWidget *layers_dup_btn;
  GtkWidget *layers_merge_btn;
  GtkWidget *layers_del_btn;
  GtkWidget *layers_up_btn;
  GtkWidget *layers_down_btn;
  GtkWidget *layers_flatten_btn;
  GtkWidget *layers_blend_dropdown;
  GtkWidget *layers_opacity_scale;
  GtkWidget *layers_opacity_label;
  PaintDocument *last_layers_doc;
  guint64 last_layers_revision;
  GPtrArray *layer_thumbs; /* unowned GtkWidget* thumbnails */

  GtkWidget *osd_pill;
  GtkWidget *undo_btn;
  GtkWidget *redo_btn;
  GtkWidget *shapes_btn;
  GtkWidget *shapes_btn_icon;
  GtkWidget *shapes_popover;
  GtkWidget *shape_outline_btn;
  GtkWidget *shape_fill_btn;
  GtkWidget *transparent_btn;
  GtkWidget *transparent_btn_icon;
  GtkWidget *transparent_popover;
  GtkToggleButton *dummy_primary_btn;
  GtkToggleButton *dummy_shape_btn;
  GtkToggleButton *dummy_transparent_btn;
  const char *last_shape_tool_id;
  const char *last_transparency_tool_id;
  const char *previous_tool_id;
  GtkWidget *size_btn;
  GtkWidget *size_popover;
  GtkScale *size_scale;
  GtkWidget *brush_style_btns[3];
  GtkWidget *color_btn;
  GtkWidget *color_swatch;
  GtkWidget *color_sv_area;
  GtkWidget *color_hue_area;
  GtkWidget *color_alpha_area;
  GtkWidget *color_preview_area;
  GtkWidget *color_alpha_label;
  GtkWidget *color_hex_entry;
  GtkWidget *recent_colors_section;
  GtkWidget *recent_colors_box;
  GtkWidget *selection_bar;
  GtkWidget *text_bar;
  GtkWidget *text_entry;
  GtkWidget *text_font_dropdown;
  GtkWidget *text_size_spin;
  GtkWidget *text_bold_btn;
  GtkWidget *text_italic_btn;
  GHashTable *tool_buttons; /* char* -> GtkToggleButton* */

  float hsv_h;
  float hsv_s;
  float hsv_v;
  float hsv_a;

  double pan_start_h;
  double pan_start_v;
  double pinch_start_zoom;
  double last_zoom;
  gboolean has_pointer;
  double pointer_x;
  double pointer_y;
  gboolean pinch_active;
  gboolean pinch_has_anchor;
  double pinch_doc_x;
  double pinch_doc_y;
  double pinch_anchor_x;
  double pinch_anchor_y;
  gboolean has_staged_anchor;
  double staged_doc_x;
  double staged_doc_y;
  double staged_view_x;
  double staged_view_y;
  gboolean syncing;
};

G_DEFINE_FINAL_TYPE (PaintCanvasPage, paint_canvas_page, ADW_TYPE_BIN)

static void
get_viewport_center (PaintCanvasPage *self, double *vx, double *vy)
{
  GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment (self->scrolled);
  GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment (self->scrolled);
  *vx = gtk_adjustment_get_page_size (hadj) * 0.5;
  *vy = gtk_adjustment_get_page_size (vadj) * 0.5;
}

static void
viewport_to_doc_point (PaintCanvasPage *self,
                       double vx,
                       double vy,
                       double zoom,
                       double *doc_x,
                       double *doc_y)
{
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  if (!doc || zoom <= 0.0) {
    *doc_x = 0.0;
    *doc_y = 0.0;
    return;
  }

  double dw = (double) paint_document_get_width (doc);
  double dh = (double) paint_document_get_height (doc);
  graphene_point_t pt;
  if (gtk_widget_compute_point (GTK_WIDGET (self->scrolled),
                                GTK_WIDGET (self->canvas),
                                &GRAPHENE_POINT_INIT ((float) vx, (float) vy),
                                &pt)) {
    *doc_x = CLAMP ((double) pt.x / zoom, 0.0, dw);
    *doc_y = CLAMP ((double) pt.y / zoom, 0.0, dh);
  } else {
    *doc_x = dw * 0.5;
    *doc_y = dh * 0.5;
  }
}

static void
stage_zoom_anchor (PaintCanvasPage *self, double vx, double vy)
{
  viewport_to_doc_point (self, vx, vy, self->last_zoom, &self->staged_doc_x, &self->staged_doc_y);
  self->staged_view_x = vx;
  self->staged_view_y = vy;
  self->has_staged_anchor = TRUE;
}

static void
apply_scroll_for_doc_anchor (PaintCanvasPage *self,
                             double zoom,
                             double doc_x,
                             double doc_y,
                             double view_x,
                             double view_y)
{
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  if (!doc || !gtk_widget_get_mapped (GTK_WIDGET (self->canvas)))
    return;

  GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment (self->scrolled);
  GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment (self->scrolled);
  double page_w = gtk_adjustment_get_page_size (hadj);
  double page_h = gtk_adjustment_get_page_size (vadj);
  if (page_w <= 0.0 || page_h <= 0.0)
    return;

  int cw = MAX (1, (int) lround ((double) paint_document_get_width (doc) * zoom));
  int ch = MAX (1, (int) lround ((double) paint_document_get_height (doc) * zoom));
  double tot_w = (double) cw + WORKSPACE_MARGIN_START + WORKSPACE_MARGIN_END;
  double tot_h = (double) ch + WORKSPACE_MARGIN_TOP + WORKSPACE_MARGIN_BOTTOM;

  double max_h = MAX (0.0, tot_w - page_w);
  double max_v = MAX (0.0, tot_h - page_h);
  double target_h = CLAMP ((double) WORKSPACE_MARGIN_START + doc_x * zoom - view_x, 0.0, max_h);
  double target_v = CLAMP ((double) WORKSPACE_MARGIN_TOP + doc_y * zoom - view_y, 0.0, max_v);

  /* Raise adjustment upper before setting value so zooming in does not clamp prior to layout */
  if (tot_w > gtk_adjustment_get_upper (hadj))
    gtk_adjustment_set_upper (hadj, tot_w);
  gtk_adjustment_set_value (hadj, target_h);

  if (tot_h > gtk_adjustment_get_upper (vadj))
    gtk_adjustment_set_upper (vadj, tot_h);
  gtk_adjustment_set_value (vadj, target_v);
}

static void sync_layers_panel (PaintCanvasPage *self);

static void
on_canvas_changed (PaintCanvas *canvas G_GNUC_UNUSED, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  paint_canvas_page_sync_controls (self);
  g_signal_emit (self, signals[SIGNAL_STATE_CHANGED], 0);
}

static void
on_canvas_zoom_changed (PaintCanvas *canvas G_GNUC_UNUSED,
                        double zoom,
                        gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  double old_zoom = self->last_zoom;
  self->last_zoom = zoom;

  double doc_x, doc_y, view_x, view_y;
  if (self->has_staged_anchor) {
    doc_x = self->staged_doc_x;
    doc_y = self->staged_doc_y;
    view_x = self->staged_view_x;
    view_y = self->staged_view_y;
    self->has_staged_anchor = FALSE;
  } else {
    get_viewport_center (self, &view_x, &view_y);
    viewport_to_doc_point (self, view_x, view_y, old_zoom, &doc_x, &doc_y);
  }
  apply_scroll_for_doc_anchor (self, zoom, doc_x, doc_y, view_x, view_y);

  g_signal_emit (self, signals[SIGNAL_STATE_CHANGED], 0);
}

static void
sync_shape_icons (PaintCanvasPage *self)
{
  gsize n_tools = 0;
  const PaintTool *tools = paint_tools_get_all (&n_tools);
  for (gsize i = 0; i < n_tools; i++) {
    if (!tools[i].is_shape)
      continue;
    GtkWidget *btn = g_hash_table_lookup (self->tool_buttons, tools[i].id);
    if (!btn)
      continue;
    GtkWidget *child = gtk_button_get_child (GTK_BUTTON (btn));
    if (GTK_IS_IMAGE (child))
      gtk_image_set_from_icon_name (GTK_IMAGE (child),
                                    paint_tool_get_icon_name (&tools[i], self->settings));
  }

  if (self->shapes_btn_icon) {
    const char *cur_icon = gtk_image_get_icon_name (GTK_IMAGE (self->shapes_btn_icon));
    if (g_strcmp0 (cur_icon, "tool-shapes-symbolic") != 0) {
      const char *shape_id = self->last_shape_tool_id ? self->last_shape_tool_id : "shape-rectangle";
      const PaintTool *shape_tool = paint_tools_find (shape_id);
      gtk_image_set_from_icon_name (GTK_IMAGE (self->shapes_btn_icon),
                                    paint_tool_get_icon_name (shape_tool, self->settings));
    }
  }
}

static void
sync_tool_ui (PaintCanvasPage *self, const PaintTool *tool)
{
  if (!tool)
    return;

  self->syncing = TRUE;
  if (tool->is_shape) {
    self->last_shape_tool_id = tool->id;
    if (self->dummy_primary_btn)
      gtk_toggle_button_set_active (self->dummy_primary_btn, TRUE);
    if (self->dummy_transparent_btn)
      gtk_toggle_button_set_active (self->dummy_transparent_btn, TRUE);
    if (self->shapes_btn_icon)
      gtk_image_set_from_icon_name (GTK_IMAGE (self->shapes_btn_icon),
                                    paint_tool_get_icon_name (tool, self->settings));
    if (self->shapes_btn)
      gtk_widget_add_css_class (self->shapes_btn, "paint-tool-active");
    if (self->transparent_btn)
      gtk_widget_remove_css_class (self->transparent_btn, "paint-tool-active");
  } else if (tool->is_transparency) {
    self->last_transparency_tool_id = tool->id;
    if (self->dummy_primary_btn)
      gtk_toggle_button_set_active (self->dummy_primary_btn, TRUE);
    if (self->dummy_shape_btn)
      gtk_toggle_button_set_active (self->dummy_shape_btn, TRUE);
    if (self->transparent_btn_icon)
      gtk_image_set_from_icon_name (GTK_IMAGE (self->transparent_btn_icon), tool->icon_name);
    if (self->transparent_btn)
      gtk_widget_add_css_class (self->transparent_btn, "paint-tool-active");
    if (self->shapes_btn)
      gtk_widget_remove_css_class (self->shapes_btn, "paint-tool-active");
  } else {
    if (self->dummy_shape_btn)
      gtk_toggle_button_set_active (self->dummy_shape_btn, TRUE);
    if (self->dummy_transparent_btn)
      gtk_toggle_button_set_active (self->dummy_transparent_btn, TRUE);
    if (self->shapes_btn)
      gtk_widget_remove_css_class (self->shapes_btn, "paint-tool-active");
    if (self->transparent_btn)
      gtk_widget_remove_css_class (self->transparent_btn, "paint-tool-active");
  }

  GtkToggleButton *active_btn = g_hash_table_lookup (self->tool_buttons, tool->id);
  if (active_btn && !gtk_toggle_button_get_active (active_btn))
    gtk_toggle_button_set_active (active_btn, TRUE);
  self->syncing = FALSE;
}

static void
on_tool_toggled (GtkToggleButton *btn, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (self->syncing || !gtk_toggle_button_get_active (btn))
    return;
  const char *tool_id = g_object_get_data (G_OBJECT (btn), "paint-tool-id");
  if (tool_id) {
    if (g_strcmp0 (self->settings->active_tool_id, tool_id) != 0)
      paint_canvas_commit_active_overlays (self->canvas);
    if (g_strcmp0 (tool_id, "eyedropper") == 0 &&
        self->settings->active_tool_id &&
        g_strcmp0 (self->settings->active_tool_id, "eyedropper") != 0) {
      self->previous_tool_id = self->settings->active_tool_id;
    }
    self->settings->active_tool_id = tool_id;
    const PaintTool *tool = paint_tools_find (tool_id);
    sync_tool_ui (self, tool);
    if (tool->is_shape && self->shapes_popover)
      gtk_popover_popdown (GTK_POPOVER (self->shapes_popover));
    if (tool->is_transparency && self->transparent_popover)
      gtk_popover_popdown (GTK_POPOVER (self->transparent_popover));
    gtk_widget_queue_draw (GTK_WIDGET (self->canvas));
  }
}

static void
on_brush_style_toggled (GtkToggleButton *btn, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (self->syncing || !gtk_toggle_button_get_active (btn))
    return;
  int idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (btn), "paint-brush-style"));
  self->settings->brush_style = (PaintBrushStyle) CLAMP (idx, 0, 2);
  gtk_widget_queue_draw (GTK_WIDGET (self->canvas));
}

static void
on_brush_style_clicked (GtkButton *btn, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (self->syncing)
    return;
  int idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (btn), "paint-brush-style"));
  self->settings->brush_style = (PaintBrushStyle) CLAMP (idx, 0, 2);
  if (g_strcmp0 (self->settings->active_tool_id, "brush") != 0)
    paint_canvas_page_select_tool (self, "brush");
  if (self->size_popover)
    gtk_popover_popdown (GTK_POPOVER (self->size_popover));
  gtk_widget_grab_focus (GTK_WIDGET (self->canvas));
  gtk_widget_queue_draw (GTK_WIDGET (self->canvas));
}

static void
on_shape_mode_toggled (GtkToggleButton *btn, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (self->syncing || !gtk_toggle_button_get_active (btn))
    return;
  self->settings->shape_filled = (GTK_WIDGET (btn) == self->shape_fill_btn);
  sync_shape_icons (self);
}

static void
on_size_scale_changed (GtkRange *range, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (self->syncing)
    return;
  double val = gtk_range_get_value (range);
  self->settings->size = val;
  g_autofree char *label = g_strdup_printf ("%d px", (int) lround (val));
  gtk_menu_button_set_label (GTK_MENU_BUTTON (self->size_btn), label);
  gtk_widget_queue_draw (GTK_WIDGET (self->canvas));
}

static void
on_preset_clicked (GtkButton *btn, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  int val = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (btn), "paint-preset-px"));
  paint_canvas_page_set_brush_size (self, (double) val);
}

static void
draw_rounded_rect_path (cairo_t *cr, double x, double y, double w, double h, double r)
{
  double radius = MIN (r, MIN (w, h) * 0.5);
  cairo_new_sub_path (cr);
  cairo_arc (cr, x + w - radius, y + radius, radius, -G_PI_2, 0.0);
  cairo_arc (cr, x + w - radius, y + h - radius, radius, 0.0, G_PI_2);
  cairo_arc (cr, x + radius, y + h - radius, radius, G_PI_2, G_PI);
  cairo_arc (cr, x + radius, y + radius, radius, G_PI, 3.0 * G_PI_2);
  cairo_close_path (cr);
}

static void
draw_checkerboard (cairo_t *cr, double x, double y, double w, double h, double cell)
{
  cairo_set_source_rgb (cr, 0.86, 0.86, 0.88);
  cairo_rectangle (cr, x, y, w, h);
  cairo_fill (cr);

  cairo_set_source_rgb (cr, 0.64, 0.64, 0.68);
  int cols = (int) ceil (w / cell);
  int rows = (int) ceil (h / cell);
  for (int row = 0; row < rows; row++) {
    for (int col = (row & 1); col < cols; col += 2) {
      double cx = x + col * cell;
      double cy = y + row * cell;
      cairo_rectangle (cr, cx, cy, MIN (cell, x + w - cx), MIN (cell, y + h - cy));
    }
  }
  cairo_fill (cr);
}

static void
draw_picker_cursor (cairo_t *cr, double x, double y, double radius)
{
  cairo_set_line_width (cr, 2.5);
  cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.45);
  cairo_arc (cr, x, y, radius, 0.0, 2.0 * G_PI);
  cairo_stroke (cr);

  cairo_set_line_width (cr, 1.5);
  cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.95);
  cairo_arc (cr, x, y, radius, 0.0, 2.0 * G_PI);
  cairo_stroke (cr);
}

static void
update_color_widgets (PaintCanvasPage *self)
{
  if (self->color_swatch)
    gtk_widget_queue_draw (self->color_swatch);
  if (self->color_sv_area)
    gtk_widget_queue_draw (self->color_sv_area);
  if (self->color_hue_area)
    gtk_widget_queue_draw (self->color_hue_area);
  if (self->color_alpha_area)
    gtk_widget_queue_draw (self->color_alpha_area);
  if (self->color_preview_area)
    gtk_widget_queue_draw (self->color_preview_area);
  if (self->canvas && paint_canvas_has_active_text (self->canvas))
    gtk_widget_queue_draw (GTK_WIDGET (self->canvas));

  if (self->color_alpha_label) {
    int pct = (int) lroundf (self->hsv_a * 100.0f);
    g_autofree char *alpha_lbl = g_strdup_printf ("%d%%", pct);
    gtk_label_set_text (GTK_LABEL (self->color_alpha_label), alpha_lbl);
  }

  if (self->color_hex_entry && !gtk_widget_has_focus (self->color_hex_entry)) {
    const GdkRGBA *c = &self->settings->color;
    int r = CLAMP ((int) lroundf (c->red * 255.0f), 0, 255);
    int g = CLAMP ((int) lroundf (c->green * 255.0f), 0, 255);
    int b = CLAMP ((int) lroundf (c->blue * 255.0f), 0, 255);
    int a = CLAMP ((int) lroundf (c->alpha * 255.0f), 0, 255);
    g_autofree char *hex = (a < 255)
                             ? g_strdup_printf ("#%02X%02X%02X%02X", r, g, b, a)
                             : g_strdup_printf ("#%02X%02X%02X", r, g, b);
    self->syncing = TRUE;
    gtk_editable_set_text (GTK_EDITABLE (self->color_hex_entry), hex);
    self->syncing = FALSE;
  }
}

static void
sync_hsv_from_settings (PaintCanvasPage *self)
{
  const GdkRGBA *c = &self->settings->color;
  float cur_r = 0.0f, cur_g = 0.0f, cur_b = 0.0f;
  gtk_hsv_to_rgb (self->hsv_h, self->hsv_s, self->hsv_v, &cur_r, &cur_g, &cur_b);
  if (fabsf (cur_r - c->red) > 1e-3f ||
      fabsf (cur_g - c->green) > 1e-3f ||
      fabsf (cur_b - c->blue) > 1e-3f ||
      fabsf (self->hsv_a - c->alpha) > 1e-3f) {
    gtk_rgb_to_hsv (c->red, c->green, c->blue, &self->hsv_h, &self->hsv_s, &self->hsv_v);
    self->hsv_a = c->alpha;
  }
  update_color_widgets (self);
}

static void
apply_hsv_to_settings (PaintCanvasPage *self)
{
  float r = 0.0f, g = 0.0f, b = 0.0f;
  gtk_hsv_to_rgb (self->hsv_h, self->hsv_s, self->hsv_v, &r, &g, &b);
  self->settings->color = (GdkRGBA) { r, g, b, self->hsv_a };
  update_color_widgets (self);
}

static void
draw_color_circle (GtkDrawingArea *area,
                   cairo_t *cr,
                   int width,
                   int height,
                   gpointer user_data)
{
  const GdkRGBA *rgba = (const GdkRGBA *) g_object_get_data (G_OBJECT (area), "paint-preset-rgba");
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  const GdkRGBA *color = rgba ? rgba : &self->settings->color;

  double size = MIN (width, height) - 2.0;
  double cx = width * 0.5;
  double cy = height * 0.5;
  double r = MAX (2.0, size * 0.5);

  cairo_save (cr);
  cairo_arc (cr, cx, cy, r, 0.0, 2.0 * G_PI);
  cairo_clip (cr);
  if (color->alpha < 0.999f)
    draw_checkerboard (cr, cx - r, cy - r, r * 2.0, r * 2.0, 4.0);
  cairo_set_source_rgba (cr, color->red, color->green, color->blue, color->alpha);
  cairo_paint (cr);
  cairo_restore (cr);

  cairo_set_line_width (cr, 1.0);
  cairo_set_source_rgba (cr, 0.5, 0.5, 0.55, 0.45);
  cairo_arc (cr, cx, cy, r - 0.5, 0.0, 2.0 * G_PI);
  cairo_stroke (cr);
}

static void
draw_color_preview (GtkDrawingArea *area G_GNUC_UNUSED,
                    cairo_t *cr,
                    int width,
                    int height,
                    gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  const GdkRGBA *c = &self->settings->color;

  cairo_save (cr);
  draw_rounded_rect_path (cr, 0.5, 0.5, width - 1.0, height - 1.0, 6.0);
  cairo_clip (cr);
  if (c->alpha < 0.999f)
    draw_checkerboard (cr, 0.0, 0.0, width, height, 5.0);
  cairo_set_source_rgba (cr, c->red, c->green, c->blue, c->alpha);
  cairo_paint (cr);
  cairo_restore (cr);

  cairo_set_line_width (cr, 1.0);
  cairo_set_source_rgba (cr, 0.5, 0.5, 0.55, 0.45);
  draw_rounded_rect_path (cr, 0.5, 0.5, width - 1.0, height - 1.0, 6.0);
  cairo_stroke (cr);
}

static void
draw_sv_area (GtkDrawingArea *area G_GNUC_UNUSED,
              cairo_t *cr,
              int width,
              int height,
              gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  float hr = 1.0f, hg = 0.0f, hb = 0.0f;
  gtk_hsv_to_rgb (self->hsv_h, 1.0f, 1.0f, &hr, &hg, &hb);

  cairo_save (cr);
  draw_rounded_rect_path (cr, 0.5, 0.5, width - 1.0, height - 1.0, 8.0);
  cairo_clip (cr);

  cairo_pattern_t *sat = cairo_pattern_create_linear (0.0, 0.0, width, 0.0);
  cairo_pattern_add_color_stop_rgb (sat, 0.0, 1.0, 1.0, 1.0);
  cairo_pattern_add_color_stop_rgb (sat, 1.0, hr, hg, hb);
  cairo_set_source (cr, sat);
  cairo_paint (cr);
  cairo_pattern_destroy (sat);

  cairo_pattern_t *val = cairo_pattern_create_linear (0.0, 0.0, 0.0, height);
  cairo_pattern_add_color_stop_rgba (val, 0.0, 0.0, 0.0, 0.0, 0.0);
  cairo_pattern_add_color_stop_rgba (val, 1.0, 0.0, 0.0, 0.0, 1.0);
  cairo_set_source (cr, val);
  cairo_paint (cr);
  cairo_pattern_destroy (val);

  draw_picker_cursor (cr,
                      self->hsv_s * width,
                      (1.0f - self->hsv_v) * height,
                      6.0);
  cairo_restore (cr);

  cairo_set_line_width (cr, 1.0);
  cairo_set_source_rgba (cr, 0.5, 0.5, 0.55, 0.35);
  draw_rounded_rect_path (cr, 0.5, 0.5, width - 1.0, height - 1.0, 8.0);
  cairo_stroke (cr);
}

static void
draw_hue_area (GtkDrawingArea *area G_GNUC_UNUSED,
               cairo_t *cr,
               int width,
               int height,
               gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);

  cairo_save (cr);
  draw_rounded_rect_path (cr, 0.5, 0.5, width - 1.0, height - 1.0, 8.0);
  cairo_clip (cr);

  cairo_pattern_t *hue = cairo_pattern_create_linear (0.0, 0.0, 0.0, height);
  cairo_pattern_add_color_stop_rgb (hue, 0.0 / 6.0, 1.0, 0.0, 0.0);
  cairo_pattern_add_color_stop_rgb (hue, 1.0 / 6.0, 1.0, 1.0, 0.0);
  cairo_pattern_add_color_stop_rgb (hue, 2.0 / 6.0, 0.0, 1.0, 0.0);
  cairo_pattern_add_color_stop_rgb (hue, 3.0 / 6.0, 0.0, 1.0, 1.0);
  cairo_pattern_add_color_stop_rgb (hue, 4.0 / 6.0, 0.0, 0.0, 1.0);
  cairo_pattern_add_color_stop_rgb (hue, 5.0 / 6.0, 1.0, 0.0, 1.0);
  cairo_pattern_add_color_stop_rgb (hue, 6.0 / 6.0, 1.0, 0.0, 0.0);
  cairo_set_source (cr, hue);
  cairo_paint (cr);
  cairo_pattern_destroy (hue);

  double cy = CLAMP (self->hsv_h * height, 4.0, height - 4.0);
  draw_picker_cursor (cr, width * 0.5, cy, 5.5);
  cairo_restore (cr);

  cairo_set_line_width (cr, 1.0);
  cairo_set_source_rgba (cr, 0.5, 0.5, 0.55, 0.35);
  draw_rounded_rect_path (cr, 0.5, 0.5, width - 1.0, height - 1.0, 8.0);
  cairo_stroke (cr);
}

static void
draw_alpha_area (GtkDrawingArea *area G_GNUC_UNUSED,
                 cairo_t *cr,
                 int width,
                 int height,
                 gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  const GdkRGBA *c = &self->settings->color;

  cairo_save (cr);
  draw_rounded_rect_path (cr, 0.5, 0.5, width - 1.0, height - 1.0, 8.0);
  cairo_clip (cr);

  draw_checkerboard (cr, 0.0, 0.0, width, height, 6.0);
  cairo_pattern_t *alpha = cairo_pattern_create_linear (0.0, 0.0, width, 0.0);
  cairo_pattern_add_color_stop_rgba (alpha, 0.0, c->red, c->green, c->blue, 0.0);
  cairo_pattern_add_color_stop_rgba (alpha, 1.0, c->red, c->green, c->blue, 1.0);
  cairo_set_source (cr, alpha);
  cairo_paint (cr);
  cairo_pattern_destroy (alpha);

  double cx = CLAMP (self->hsv_a * width, 5.0, width - 5.0);
  draw_picker_cursor (cr, cx, height * 0.5, 5.5);
  cairo_restore (cr);

  cairo_set_line_width (cr, 1.0);
  cairo_set_source_rgba (cr, 0.5, 0.5, 0.55, 0.35);
  draw_rounded_rect_path (cr, 0.5, 0.5, width - 1.0, height - 1.0, 8.0);
  cairo_stroke (cr);
}

static void
update_picker_from_point (PaintCanvasPage *self, GtkWidget *area, PickerKind kind, double x, double y)
{
  int w = MAX (1, gtk_widget_get_width (area));
  int h = MAX (1, gtk_widget_get_height (area));
  float nx = (float) CLAMP (x / (double) w, 0.0, 1.0);
  float ny = (float) CLAMP (y / (double) h, 0.0, 1.0);

  if (kind == PICKER_SV) {
    self->hsv_s = nx;
    self->hsv_v = 1.0f - ny;
  } else if (kind == PICKER_HUE) {
    self->hsv_h = ny;
  } else {
    self->hsv_a = nx;
  }
  apply_hsv_to_settings (self);
}

static void
on_picker_drag_begin (GtkGestureDrag *gesture,
                      double start_x,
                      double start_y,
                      gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  GtkWidget *area = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
  PickerKind kind = (PickerKind) GPOINTER_TO_INT (g_object_get_data (G_OBJECT (gesture), "paint-picker-kind"));

  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  g_object_set_data_full (G_OBJECT (gesture), "paint-start-x",
                          g_memdup2 (&start_x, sizeof (double)), g_free);
  g_object_set_data_full (G_OBJECT (gesture), "paint-start-y",
                          g_memdup2 (&start_y, sizeof (double)), g_free);
  update_picker_from_point (self, area, kind, start_x, start_y);
}

static void
on_picker_drag_update (GtkGestureDrag *gesture,
                       double offset_x,
                       double offset_y,
                       gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  GtkWidget *area = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
  PickerKind kind = (PickerKind) GPOINTER_TO_INT (g_object_get_data (G_OBJECT (gesture), "paint-picker-kind"));
  const double *sx = g_object_get_data (G_OBJECT (gesture), "paint-start-x");
  const double *sy = g_object_get_data (G_OBJECT (gesture), "paint-start-y");
  if (!sx || !sy)
    return;

  update_picker_from_point (self, area, kind, *sx + offset_x, *sy + offset_y);
}

static void
attach_picker_drag (GtkWidget *area, PickerKind kind, PaintCanvasPage *self)
{
  GtkGesture *drag = gtk_gesture_drag_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (drag), GDK_BUTTON_PRIMARY);
  g_object_set_data (G_OBJECT (drag), "paint-picker-kind", GINT_TO_POINTER ((int) kind));
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_picker_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_picker_drag_update), self);
  gtk_widget_add_controller (area, GTK_EVENT_CONTROLLER (drag));
}

static void
on_color_preset_clicked (GtkButton *btn, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  const GdkRGBA *rgba = (const GdkRGBA *) g_object_get_data (G_OBJECT (btn), "paint-preset-color");
  if (!rgba)
    return;
  self->settings->color = *rgba;
  sync_hsv_from_settings (self);
}

static void
apply_hex_entry_text (PaintCanvasPage *self, gboolean strict_hex)
{
  if (self->syncing || !self->color_hex_entry)
    return;
  const char *text = gtk_editable_get_text (GTK_EDITABLE (self->color_hex_entry));
  if (!text)
    return;
  if (strict_hex) {
    gsize len = strlen (text);
    if (text[0] != '#' || (len != 7 && len != 9))
      return;
  }
  GdkRGBA parsed;
  if (gdk_rgba_parse (&parsed, text)) {
    self->settings->color = parsed;
    sync_hsv_from_settings (self);
  }
}

static void
on_hex_entry_changed (GtkEditable *editable G_GNUC_UNUSED, gpointer user_data)
{
  apply_hex_entry_text (PAINT_CANVAS_PAGE (user_data), TRUE);
}

static void
on_hex_entry_activate (GtkEntry *entry G_GNUC_UNUSED, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  apply_hex_entry_text (self, FALSE);
  gtk_widget_grab_focus (GTK_WIDGET (self->canvas));
  update_color_widgets (self);
}

static void
sync_recent_colors (PaintCanvasPage *self)
{
  if (!self->recent_colors_box || !self->recent_colors_section || !self->settings)
    return;

  GtkWidget *child = gtk_widget_get_first_child (self->recent_colors_box);
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling (child);
    gtk_box_remove (GTK_BOX (self->recent_colors_box), child);
    child = next;
  }

  if (self->settings->n_recent_colors <= 0) {
    gtk_widget_set_visible (self->recent_colors_section, FALSE);
    return;
  }

  gtk_widget_set_visible (self->recent_colors_section, TRUE);
  for (int i = 0; i < self->settings->n_recent_colors; i++) {
    GtkWidget *btn = gtk_button_new ();
    gtk_widget_add_css_class (btn, "flat");
    gtk_widget_add_css_class (btn, "paint-color-preset");
    GtkWidget *dot = gtk_drawing_area_new ();
    gtk_widget_set_size_request (dot, 18, 18);
    g_object_set_data_full (G_OBJECT (dot), "paint-preset-rgba",
                            g_memdup2 (&self->settings->recent_colors[i], sizeof (GdkRGBA)), g_free);
    gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (dot), draw_color_circle, self, NULL);
    gtk_button_set_child (GTK_BUTTON (btn), dot);
    g_object_set_data_full (G_OBJECT (btn), "paint-preset-color",
                            g_memdup2 (&self->settings->recent_colors[i], sizeof (GdkRGBA)), g_free);
    g_signal_connect (btn, "clicked", G_CALLBACK (on_color_preset_clicked), self);
    gtk_box_append (GTK_BOX (self->recent_colors_box), btn);
  }
}

static void
on_color_popover_closed (GtkPopover *popover G_GNUC_UNUSED, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  paint_tool_settings_add_recent_color (self->settings, &self->settings->color);
  sync_recent_colors (self);
}

static GtkPopover *
build_color_popover (PaintCanvasPage *self)
{
  GtkWidget *pop = gtk_popover_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_top (box, 12);
  gtk_widget_set_margin_bottom (box, 12);
  gtk_widget_set_margin_start (box, 12);
  gtk_widget_set_margin_end (box, 12);

  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *heading = gtk_label_new ("Brush Color");
  gtk_widget_add_css_class (heading, "heading");
  gtk_widget_set_hexpand (heading, TRUE);
  gtk_widget_set_halign (heading, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (header), heading);

  self->color_preview_area = gtk_drawing_area_new ();
  gtk_widget_set_size_request (self->color_preview_area, 28, 22);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (self->color_preview_area),
                                  draw_color_preview, self, NULL);
  gtk_box_append (GTK_BOX (header), self->color_preview_area);

  self->color_hex_entry = gtk_entry_new ();
  gtk_widget_add_css_class (self->color_hex_entry, "paint-color-hex");
  gtk_entry_set_max_length (GTK_ENTRY (self->color_hex_entry), 9);
  gtk_editable_set_width_chars (GTK_EDITABLE (self->color_hex_entry), 9);
  g_signal_connect (self->color_hex_entry, "changed", G_CALLBACK (on_hex_entry_changed), self);
  g_signal_connect (self->color_hex_entry, "activate", G_CALLBACK (on_hex_entry_activate), self);
  gtk_box_append (GTK_BOX (header), self->color_hex_entry);
  gtk_box_append (GTK_BOX (box), header);

  GtkWidget *pick_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);

  self->color_sv_area = gtk_drawing_area_new ();
  gtk_widget_set_size_request (self->color_sv_area, 204, 168);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (self->color_sv_area),
                                  draw_sv_area, self, NULL);
  attach_picker_drag (self->color_sv_area, PICKER_SV, self);
  gtk_box_append (GTK_BOX (pick_row), self->color_sv_area);

  self->color_hue_area = gtk_drawing_area_new ();
  gtk_widget_set_size_request (self->color_hue_area, 22, 168);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (self->color_hue_area),
                                  draw_hue_area, self, NULL);
  attach_picker_drag (self->color_hue_area, PICKER_HUE, self);
  gtk_box_append (GTK_BOX (pick_row), self->color_hue_area);

  gtk_box_append (GTK_BOX (box), pick_row);

  GtkWidget *alpha_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *alpha_title = gtk_label_new ("Opacity");
  gtk_widget_add_css_class (alpha_title, "dim-label");
  gtk_box_append (GTK_BOX (alpha_row), alpha_title);

  self->color_alpha_area = gtk_drawing_area_new ();
  gtk_widget_set_hexpand (self->color_alpha_area, TRUE);
  gtk_widget_set_size_request (self->color_alpha_area, 128, 20);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (self->color_alpha_area),
                                  draw_alpha_area, self, NULL);
  attach_picker_drag (self->color_alpha_area, PICKER_ALPHA, self);
  gtk_box_append (GTK_BOX (alpha_row), self->color_alpha_area);

  self->color_alpha_label = gtk_label_new ("100%");
  gtk_widget_add_css_class (self->color_alpha_label, "paint-color-alpha-label");
  gtk_label_set_xalign (GTK_LABEL (self->color_alpha_label), 1.0f);
  gtk_box_append (GTK_BOX (alpha_row), self->color_alpha_label);
  gtk_box_append (GTK_BOX (box), alpha_row);

  GtkWidget *presets = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_halign (presets, GTK_ALIGN_CENTER);
  for (gsize i = 0; i < G_N_ELEMENTS (COLOR_PRESETS); i++) {
    GtkWidget *btn = gtk_button_new ();
    gtk_widget_add_css_class (btn, "flat");
    gtk_widget_add_css_class (btn, "paint-color-preset");
    GtkWidget *dot = gtk_drawing_area_new ();
    gtk_widget_set_size_request (dot, 18, 18);
    g_object_set_data (G_OBJECT (dot), "paint-preset-rgba", (gpointer) &COLOR_PRESETS[i]);
    gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (dot), draw_color_circle, self, NULL);
    gtk_button_set_child (GTK_BUTTON (btn), dot);
    g_object_set_data (G_OBJECT (btn), "paint-preset-color", (gpointer) &COLOR_PRESETS[i]);
    g_signal_connect (btn, "clicked", G_CALLBACK (on_color_preset_clicked), self);
    gtk_box_append (GTK_BOX (presets), btn);
  }
  gtk_box_append (GTK_BOX (box), presets);

  self->recent_colors_section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  GtkWidget *recent_lbl = gtk_label_new ("Recent");
  gtk_widget_add_css_class (recent_lbl, "paint-dim-label");
  gtk_widget_set_halign (recent_lbl, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (self->recent_colors_section), recent_lbl);

  self->recent_colors_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_halign (self->recent_colors_box, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (self->recent_colors_section), self->recent_colors_box);
  gtk_box_append (GTK_BOX (box), self->recent_colors_section);

  gtk_popover_set_child (GTK_POPOVER (pop), box);
  g_signal_connect (pop, "closed", G_CALLBACK (on_color_popover_closed), self);
  sync_hsv_from_settings (self);
  sync_recent_colors (self);
  return GTK_POPOVER (pop);
}

static void
on_scrolled_motion (GtkEventControllerMotion *ctrl G_GNUC_UNUSED,
                    double x,
                    double y,
                    gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  self->has_pointer = TRUE;
  self->pointer_x = x;
  self->pointer_y = y;
}

static void
on_scrolled_leave (GtkEventControllerMotion *ctrl G_GNUC_UNUSED, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  self->has_pointer = FALSE;
}

static gboolean
on_scroll (GtkEventControllerScroll *ctrl,
           double dx G_GNUC_UNUSED,
           double dy,
           gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  GdkModifierType state = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (ctrl));
  if (state & GDK_CONTROL_MASK) {
    double vx, vy;
    if (self->has_pointer) {
      vx = self->pointer_x;
      vy = self->pointer_y;
    } else {
      get_viewport_center (self, &vx, &vy);
    }
    stage_zoom_anchor (self, vx, vy);
    if (dy < 0)
      paint_canvas_zoom_in (self->canvas);
    else if (dy > 0)
      paint_canvas_zoom_out (self->canvas);
    self->has_staged_anchor = FALSE;
    return TRUE;
  }
  return FALSE;
}

static void
on_pan_begin (GtkGestureDrag *gesture G_GNUC_UNUSED,
              double start_x G_GNUC_UNUSED,
              double start_y G_GNUC_UNUSED,
              gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  self->pan_start_h = gtk_adjustment_get_value (gtk_scrolled_window_get_hadjustment (self->scrolled));
  self->pan_start_v = gtk_adjustment_get_value (gtk_scrolled_window_get_vadjustment (self->scrolled));
}

static void
on_pan_update (GtkGestureDrag *gesture G_GNUC_UNUSED,
               double offset_x,
               double offset_y,
               gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  gtk_adjustment_set_value (gtk_scrolled_window_get_hadjustment (self->scrolled),
                            self->pan_start_h - offset_x);
  gtk_adjustment_set_value (gtk_scrolled_window_get_vadjustment (self->scrolled),
                            self->pan_start_v - offset_y);
}

static void
get_pinch_anchor (PaintCanvasPage *self, GtkGesture *gesture, double *ax, double *ay)
{
  if (gtk_gesture_get_bounding_box_center (gesture, ax, ay))
    return;
  if (self->has_pointer) {
    *ax = self->pointer_x;
    *ay = self->pointer_y;
    return;
  }
  get_viewport_center (self, ax, ay);
}

static void
on_pinch_begin (GtkGesture *gesture,
                GdkEventSequence *seq G_GNUC_UNUSED,
                gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  gtk_gesture_set_state (gesture, GTK_EVENT_SEQUENCE_CLAIMED);
  self->pinch_active = TRUE;
  self->pinch_start_zoom = paint_canvas_get_zoom (self->canvas);

  double ax, ay;
  get_pinch_anchor (self, gesture, &ax, &ay);
  viewport_to_doc_point (self, ax, ay, self->pinch_start_zoom,
                         &self->pinch_doc_x, &self->pinch_doc_y);
  self->pinch_anchor_x = ax;
  self->pinch_anchor_y = ay;
  self->pinch_has_anchor = TRUE;
}

static void
on_pinch_scale_changed (GtkGestureZoom *gesture,
                        double scale,
                        gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  double ax = self->pinch_anchor_x;
  double ay = self->pinch_anchor_y;
  get_pinch_anchor (self, GTK_GESTURE (gesture), &ax, &ay);
  self->pinch_anchor_x = ax;
  self->pinch_anchor_y = ay;

  if (self->pinch_has_anchor) {
    self->staged_doc_x = self->pinch_doc_x;
    self->staged_doc_y = self->pinch_doc_y;
    self->staged_view_x = ax;
    self->staged_view_y = ay;
    self->has_staged_anchor = TRUE;
  }
  paint_canvas_set_zoom (self->canvas, self->pinch_start_zoom * scale);
  self->has_staged_anchor = FALSE;
}

static void
on_pinch_update (GtkGesture *gesture,
                 GdkEventSequence *seq,
                 gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (!self->pinch_active || !self->pinch_has_anchor || !gtk_gesture_is_recognized (gesture))
    return;

  GdkEvent *ev = gtk_gesture_get_last_event (gesture, seq);
  if (!ev || gdk_event_get_event_type (ev) != GDK_TOUCH_UPDATE)
    return;

  double ax, ay;
  if (gtk_gesture_get_bounding_box_center (gesture, &ax, &ay) &&
      hypot (ax - self->pinch_anchor_x, ay - self->pinch_anchor_y) >= 0.5) {
    self->pinch_anchor_x = ax;
    self->pinch_anchor_y = ay;
    apply_scroll_for_doc_anchor (self,
                                 paint_canvas_get_zoom (self->canvas),
                                 self->pinch_doc_x,
                                 self->pinch_doc_y,
                                 ax,
                                 ay);
  }
}

static void
on_pinch_end (GtkGesture *gesture G_GNUC_UNUSED,
              GdkEventSequence *seq G_GNUC_UNUSED,
              gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  self->pinch_active = FALSE;
  self->pinch_has_anchor = FALSE;
}

static GtkPopover *
build_size_popover (PaintCanvasPage *self)
{
  GtkWidget *pop = gtk_popover_new ();
  gtk_popover_set_position (GTK_POPOVER (pop), GTK_POS_TOP);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top (box, 10);
  gtk_widget_set_margin_bottom (box, 10);
  gtk_widget_set_margin_start (box, 12);
  gtk_widget_set_margin_end (box, 12);

  GtkWidget *heading = gtk_label_new ("Brush Size");
  gtk_widget_add_css_class (heading, "heading");
  gtk_widget_set_halign (heading, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (box), heading);

  GtkWidget *scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 1.0, 96.0, 1.0);
  gtk_widget_set_size_request (scale, 190, -1);
  gtk_range_set_value (GTK_RANGE (scale), self->settings->size);
  gtk_scale_set_draw_value (GTK_SCALE (scale), TRUE);
  gtk_scale_set_value_pos (GTK_SCALE (scale), GTK_POS_RIGHT);
  g_signal_connect (scale, "value-changed", G_CALLBACK (on_size_scale_changed), self);
  gtk_box_append (GTK_BOX (box), scale);
  self->size_scale = GTK_SCALE (scale);

  GtkWidget *presets = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_halign (presets, GTK_ALIGN_CENTER);
  for (gsize i = 0; i < G_N_ELEMENTS (BRUSH_PRESETS); i++) {
    int px = BRUSH_PRESETS[i];
    g_autofree char *lbl = g_strdup_printf ("%d", px);
    GtkWidget *btn = gtk_button_new_with_label (lbl);
    gtk_widget_add_css_class (btn, "flat");
    g_object_set_data (G_OBJECT (btn), "paint-preset-px", GINT_TO_POINTER (px));
    g_signal_connect (btn, "clicked", G_CALLBACK (on_preset_clicked), self);
    gtk_box_append (GTK_BOX (presets), btn);
  }
  gtk_box_append (GTK_BOX (box), presets);

  GtkWidget *style_section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  GtkWidget *style_lbl = gtk_label_new ("Style");
  gtk_widget_add_css_class (style_lbl, "paint-dim-label");
  gtk_widget_set_halign (style_lbl, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (style_section), style_lbl);

  GtkWidget *style_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_box_set_homogeneous (GTK_BOX (style_box), TRUE);
  gtk_widget_set_hexpand (style_box, TRUE);
  gtk_widget_add_css_class (style_box, "paint-shape-mode");
  static const char *STYLE_LABELS[3] = { "Smooth", "Soft", "Pixel" };
  GtkToggleButton *first_style = NULL;
  for (int i = 0; i < 3; i++) {
    GtkWidget *sbtn = gtk_toggle_button_new_with_label (STYLE_LABELS[i]);
    if (!first_style)
      first_style = GTK_TOGGLE_BUTTON (sbtn);
    else
      gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (sbtn), first_style);
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (sbtn),
                                  (int) self->settings->brush_style == i);
    g_object_set_data (G_OBJECT (sbtn), "paint-brush-style", GINT_TO_POINTER (i));
    g_signal_connect (sbtn, "toggled", G_CALLBACK (on_brush_style_toggled), self);
    g_signal_connect (sbtn, "clicked", G_CALLBACK (on_brush_style_clicked), self);
    gtk_box_append (GTK_BOX (style_box), sbtn);
    self->brush_style_btns[i] = sbtn;
  }
  gtk_box_append (GTK_BOX (style_section), style_box);
  gtk_box_append (GTK_BOX (box), style_section);

  gtk_popover_set_child (GTK_POPOVER (pop), box);
  self->size_popover = pop;
  return GTK_POPOVER (pop);
}

static GtkPopover *
build_shapes_popover (PaintCanvasPage *self)
{
  GtkWidget *pop = gtk_popover_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_top (box, 10);
  gtk_widget_set_margin_bottom (box, 10);
  gtk_widget_set_margin_start (box, 12);
  gtk_widget_set_margin_end (box, 12);

  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 16);
  GtkWidget *heading = gtk_label_new ("Shapes");
  gtk_widget_add_css_class (heading, "heading");
  gtk_widget_set_halign (heading, GTK_ALIGN_START);
  gtk_widget_set_hexpand (heading, TRUE);
  gtk_box_append (GTK_BOX (header), heading);

  GtkWidget *mode_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class (mode_box, "paint-shape-mode");

  self->shape_outline_btn = gtk_toggle_button_new_with_label ("Outline");
  self->shape_fill_btn = gtk_toggle_button_new_with_label ("Filled");
  gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (self->shape_fill_btn),
                               GTK_TOGGLE_BUTTON (self->shape_outline_btn));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->shape_outline_btn),
                                !self->settings->shape_filled);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->shape_fill_btn),
                                self->settings->shape_filled);
  g_signal_connect (self->shape_outline_btn, "toggled", G_CALLBACK (on_shape_mode_toggled), self);
  g_signal_connect (self->shape_fill_btn, "toggled", G_CALLBACK (on_shape_mode_toggled), self);
  gtk_box_append (GTK_BOX (mode_box), self->shape_outline_btn);
  gtk_box_append (GTK_BOX (mode_box), self->shape_fill_btn);
  gtk_box_append (GTK_BOX (header), mode_box);
  gtk_box_append (GTK_BOX (box), header);

  GtkWidget *grid = gtk_grid_new ();
  gtk_widget_set_halign (grid, GTK_ALIGN_CENTER);
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 6);

  gsize n_tools = 0;
  const PaintTool *tools = paint_tools_get_all (&n_tools);
  GtkToggleButton *first_shape = NULL;
  int idx = 0;
  for (gsize i = 0; i < n_tools; i++) {
    if (!tools[i].is_shape)
      continue;

    GtkWidget *btn = gtk_toggle_button_new ();
    gtk_widget_add_css_class (btn, "flat");
    gtk_widget_add_css_class (btn, "paint-shape-tile");
    gtk_widget_set_tooltip_text (btn, tools[i].tooltip);

    GtkWidget *img = gtk_image_new_from_icon_name (
        paint_tool_get_icon_name (&tools[i], self->settings));
    gtk_image_set_pixel_size (GTK_IMAGE (img), 20);
    gtk_button_set_child (GTK_BUTTON (btn), img);

    if (!first_shape)
      first_shape = GTK_TOGGLE_BUTTON (btn);
    else
      gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (btn), first_shape);

    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (btn),
                                  g_strcmp0 (tools[i].id, self->settings->active_tool_id) == 0);
    g_object_set_data (G_OBJECT (btn), "paint-tool-id", (gpointer) tools[i].id);
    g_signal_connect (btn, "toggled", G_CALLBACK (on_tool_toggled), self);
    g_hash_table_insert (self->tool_buttons, (gpointer) tools[i].id, btn);

    gtk_grid_attach (GTK_GRID (grid), btn, idx % 3, idx / 3, 1, 1);
    idx++;
  }

  GtkWidget *dummy_shape = gtk_toggle_button_new ();
  gtk_widget_set_visible (dummy_shape, FALSE);
  if (first_shape)
    gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (dummy_shape), first_shape);
  gtk_box_append (GTK_BOX (box), dummy_shape);
  self->dummy_shape_btn = GTK_TOGGLE_BUTTON (dummy_shape);

  gtk_box_append (GTK_BOX (box), grid);

  GtkWidget *hint = gtk_label_new ("Hold Shift for proportions · Space to move");
  gtk_widget_add_css_class (hint, "paint-dim-label");
  gtk_widget_set_halign (hint, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (box), hint);

  gtk_popover_set_child (GTK_POPOVER (pop), box);
  self->shapes_popover = pop;
  return GTK_POPOVER (pop);
}

static void
on_clear_transparent_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (self->transparent_popover)
    gtk_popover_popdown (GTK_POPOVER (self->transparent_popover));
  paint_canvas_clear_transparent (self->canvas);
}

static GtkPopover *
build_transparency_popover (PaintCanvasPage *self)
{
  GtkWidget *pop = gtk_popover_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_top (box, 10);
  gtk_widget_set_margin_bottom (box, 10);
  gtk_widget_set_margin_start (box, 12);
  gtk_widget_set_margin_end (box, 12);

  GtkWidget *heading = gtk_label_new ("Transparency");
  gtk_widget_add_css_class (heading, "heading");
  gtk_widget_set_halign (heading, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (box), heading);

  GtkWidget *modes = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_set_homogeneous (GTK_BOX (modes), TRUE);

  gsize n_tools = 0;
  const PaintTool *tools = paint_tools_get_all (&n_tools);
  GtkToggleButton *first_btn = NULL;
  for (gsize i = 0; i < n_tools; i++) {
    if (!tools[i].is_transparency)
      continue;

    GtkWidget *btn = gtk_toggle_button_new ();
    gtk_widget_add_css_class (btn, "flat");
    gtk_widget_add_css_class (btn, "paint-transparency-tile");
    gtk_widget_set_tooltip_text (btn, tools[i].tooltip);

    GtkWidget *content = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign (content, GTK_ALIGN_CENTER);
    GtkWidget *img = gtk_image_new_from_icon_name (
        paint_tool_get_icon_name (&tools[i], self->settings));
    gtk_image_set_pixel_size (GTK_IMAGE (img), 16);
    GtkWidget *lbl = gtk_label_new (tools[i].name);
    gtk_box_append (GTK_BOX (content), img);
    gtk_box_append (GTK_BOX (content), lbl);
    gtk_button_set_child (GTK_BUTTON (btn), content);

    if (!first_btn)
      first_btn = GTK_TOGGLE_BUTTON (btn);
    else
      gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (btn), first_btn);

    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (btn),
                                  g_strcmp0 (tools[i].id, self->settings->active_tool_id) == 0);
    g_object_set_data (G_OBJECT (btn), "paint-tool-id", (gpointer) tools[i].id);
    g_signal_connect (btn, "toggled", G_CALLBACK (on_tool_toggled), self);
    g_hash_table_insert (self->tool_buttons, (gpointer) tools[i].id, btn);

    gtk_box_append (GTK_BOX (modes), btn);
  }

  GtkWidget *dummy_transparent = gtk_toggle_button_new ();
  gtk_widget_set_visible (dummy_transparent, FALSE);
  if (first_btn)
    gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (dummy_transparent), first_btn);
  gtk_box_append (GTK_BOX (box), dummy_transparent);
  self->dummy_transparent_btn = GTK_TOGGLE_BUTTON (dummy_transparent);

  gtk_box_append (GTK_BOX (box), modes);

  GtkWidget *clear_btn = gtk_button_new_with_label ("Clear Canvas to Transparent");
  gtk_widget_add_css_class (clear_btn, "paint-transparency-clear");
  g_signal_connect (clear_btn, "clicked", G_CALLBACK (on_clear_transparent_clicked), self);
  gtk_box_append (GTK_BOX (box), clear_btn);

  GtkWidget *hint = gtk_label_new ("Save as PNG or WebP to preserve transparency");
  gtk_widget_add_css_class (hint, "paint-dim-label");
  gtk_widget_set_halign (hint, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (box), hint);

  gtk_popover_set_child (GTK_POPOVER (pop), box);
  self->transparent_popover = pop;
  return GTK_POPOVER (pop);
}

static void
on_adjustment_changed (GtkAdjustment *adj G_GNUC_UNUSED, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  paint_canvas_check_viewport (self->canvas);
}

static void
on_adjustment_prop_changed (GObject *obj G_GNUC_UNUSED,
                            GParamSpec *pspec G_GNUC_UNUSED,
                            gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  paint_canvas_check_viewport (self->canvas);
}

static void
sync_overlay_bars (PaintCanvasPage *self)
{
  if (!self->canvas)
    return;
  gboolean has_text = paint_canvas_has_active_text (self->canvas);
  gboolean has_sel = paint_canvas_has_selection (self->canvas);
  if (self->text_bar)
    gtk_widget_set_visible (self->text_bar, has_text);
  if (self->selection_bar)
    gtk_widget_set_visible (self->selection_bar, !has_text && has_sel);
}

static void
on_canvas_color_picked (PaintCanvas *canvas G_GNUC_UNUSED, gboolean finished, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  sync_hsv_from_settings (self);
  if (finished) {
    sync_recent_colors (self);
    if (g_strcmp0 (self->settings->active_tool_id, "eyedropper") == 0) {
      const char *prev = (self->previous_tool_id &&
                          g_strcmp0 (self->previous_tool_id, "eyedropper") != 0)
                           ? self->previous_tool_id
                           : "brush";
      paint_canvas_page_select_tool (self, prev);
    }
  }
  gtk_widget_queue_draw (GTK_WIDGET (self->canvas));
}

static void
on_canvas_selection_changed (PaintCanvas *canvas G_GNUC_UNUSED, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  sync_overlay_bars (self);
  g_signal_emit (self, signals[SIGNAL_STATE_CHANGED], 0);
}

static void
on_canvas_text_state_changed (PaintCanvas *canvas, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  gboolean active = paint_canvas_has_active_text (canvas);
  sync_overlay_bars (self);
  if (active && self->text_entry) {
    self->syncing = TRUE;
    gtk_editable_set_text (GTK_EDITABLE (self->text_entry),
                           paint_canvas_get_active_text (canvas));
    self->syncing = FALSE;
    gtk_widget_grab_focus (self->text_entry);
  } else if (!active && self->canvas) {
    gtk_widget_grab_focus (GTK_WIDGET (self->canvas));
  }
}

static void
on_text_entry_changed (GtkEditable *editable, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (self->syncing)
    return;
  paint_canvas_set_active_text (self->canvas, gtk_editable_get_text (editable));
}

static void
on_text_entry_activate (GtkEntry *entry G_GNUC_UNUSED, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  paint_canvas_commit_text (self->canvas);
}

static gboolean
on_text_entry_key_pressed (GtkEventControllerKey *ctrl G_GNUC_UNUSED,
                           guint keyval,
                           guint keycode G_GNUC_UNUSED,
                           GdkModifierType state G_GNUC_UNUSED,
                           gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (keyval == GDK_KEY_Escape) {
    paint_canvas_cancel_text (self->canvas);
    return TRUE;
  }
  return FALSE;
}

static void
on_text_font_changed (GObject *obj, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (self->syncing)
    return;
  static const char *FAMILIES[] = { "Sans", "Serif", "Monospace" };
  guint idx = gtk_drop_down_get_selected (GTK_DROP_DOWN (obj));
  const char *family = (idx < G_N_ELEMENTS (FAMILIES)) ? FAMILIES[idx] : "Sans";
  paint_tool_settings_set_text_font_family (self->settings, family);
  gtk_widget_queue_draw (GTK_WIDGET (self->canvas));
}

static void
on_text_size_changed (GtkSpinButton *spin, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (self->syncing)
    return;
  self->settings->text_font_size = gtk_spin_button_get_value (spin);
  gtk_widget_queue_draw (GTK_WIDGET (self->canvas));
}

static void
on_text_bold_toggled (GtkToggleButton *btn, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (self->syncing)
    return;
  self->settings->text_bold = gtk_toggle_button_get_active (btn);
  gtk_widget_queue_draw (GTK_WIDGET (self->canvas));
}

static void
on_text_italic_toggled (GtkToggleButton *btn, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (self->syncing)
    return;
  self->settings->text_italic = gtk_toggle_button_get_active (btn);
  gtk_widget_queue_draw (GTK_WIDGET (self->canvas));
}

static GtkWidget *
add_action_pill_button (GtkWidget *box,
                        const char *icon_name,
                        const char *tooltip,
                        const char *action_name)
{
  GtkWidget *btn = gtk_button_new_from_icon_name (icon_name);
  gtk_widget_add_css_class (btn, "flat");
  gtk_widget_set_tooltip_text (btn, tooltip);
  if (action_name)
    gtk_actionable_set_action_name (GTK_ACTIONABLE (btn), action_name);
  gtk_box_append (GTK_BOX (box), btn);
  return btn;
}

static GtkWidget *
build_selection_bar (PaintCanvasPage *self)
{
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_add_css_class (bar, "paint-osd-pill");
  gtk_widget_add_css_class (bar, "paint-sub-pill");
  gtk_widget_set_halign (bar, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (bar, GTK_ALIGN_END);
  gtk_widget_set_margin_bottom (bar, 68);
  gtk_widget_set_visible (bar, FALSE);

  add_action_pill_button (bar, "edit-copy-symbolic", "Copy Selection (Ctrl+C)", "win.copy");
  add_action_pill_button (bar, "edit-cut-symbolic", "Cut Selection (Ctrl+X)", "win.cut");
  add_action_pill_button (bar, "tool-crop-symbolic", "Crop to Selection (Ctrl+Shift+X)", "win.crop-selection");
  gtk_box_append (GTK_BOX (bar), gtk_separator_new (GTK_ORIENTATION_VERTICAL));
  add_action_pill_button (bar, "object-rotate-left-symbolic", "Rotate Selection Left 90°", "win.rotate-ccw");
  add_action_pill_button (bar, "object-rotate-right-symbolic", "Rotate Selection Right 90°", "win.rotate-cw");
  add_action_pill_button (bar, "object-flip-horizontal-symbolic", "Flip Selection Horizontally", "win.flip-horizontal");
  add_action_pill_button (bar, "object-flip-vertical-symbolic", "Flip Selection Vertically", "win.flip-vertical");
  gtk_box_append (GTK_BOX (bar), gtk_separator_new (GTK_ORIENTATION_VERTICAL));
  add_action_pill_button (bar, "edit-delete-symbolic", "Delete Selection (Del)", "win.delete-selection");

  GtkWidget *done_btn = gtk_button_new_from_icon_name ("object-select-symbolic");
  gtk_widget_add_css_class (done_btn, "suggested-action");
  gtk_widget_set_tooltip_text (done_btn, "Apply Selection (Enter)");
  g_signal_connect_swapped (done_btn, "clicked", G_CALLBACK (paint_canvas_commit_selection), self->canvas);
  gtk_box_append (GTK_BOX (bar), done_btn);
  return bar;
}

static GtkWidget *
build_text_bar (PaintCanvasPage *self)
{
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class (bar, "paint-osd-pill");
  gtk_widget_add_css_class (bar, "paint-sub-pill");
  gtk_widget_set_halign (bar, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (bar, GTK_ALIGN_END);
  gtk_widget_set_margin_bottom (bar, 68);
  gtk_widget_set_visible (bar, FALSE);

  self->text_entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (self->text_entry), "Type text…");
  gtk_editable_set_width_chars (GTK_EDITABLE (self->text_entry), 16);
  g_signal_connect (self->text_entry, "changed", G_CALLBACK (on_text_entry_changed), self);
  g_signal_connect (self->text_entry, "activate", G_CALLBACK (on_text_entry_activate), self);
  GtkEventController *ekey = gtk_event_controller_key_new ();
  g_signal_connect (ekey, "key-pressed", G_CALLBACK (on_text_entry_key_pressed), self);
  gtk_widget_add_controller (self->text_entry, ekey);
  gtk_box_append (GTK_BOX (bar), self->text_entry);

  static const char *const FONT_ITEMS[] = { "Sans", "Serif", "Monospace", NULL };
  self->text_font_dropdown = gtk_drop_down_new_from_strings (FONT_ITEMS);
  gtk_widget_set_tooltip_text (self->text_font_dropdown, "Font Family");
  g_signal_connect (self->text_font_dropdown, "notify::selected", G_CALLBACK (on_text_font_changed), self);
  gtk_box_append (GTK_BOX (bar), self->text_font_dropdown);

  self->text_size_spin = gtk_spin_button_new_with_range (8.0, 256.0, 2.0);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->text_size_spin), self->settings->text_font_size);
  gtk_widget_set_tooltip_text (self->text_size_spin, "Font Size (px)");
  g_signal_connect (self->text_size_spin, "value-changed", G_CALLBACK (on_text_size_changed), self);
  gtk_box_append (GTK_BOX (bar), self->text_size_spin);

  self->text_bold_btn = gtk_toggle_button_new ();
  GtkWidget *b_lbl = gtk_label_new (NULL);
  gtk_label_set_markup (GTK_LABEL (b_lbl), "<b>B</b>");
  gtk_button_set_child (GTK_BUTTON (self->text_bold_btn), b_lbl);
  gtk_widget_add_css_class (self->text_bold_btn, "flat");
  gtk_widget_set_tooltip_text (self->text_bold_btn, "Bold");
  g_signal_connect (self->text_bold_btn, "toggled", G_CALLBACK (on_text_bold_toggled), self);
  gtk_box_append (GTK_BOX (bar), self->text_bold_btn);

  self->text_italic_btn = gtk_toggle_button_new ();
  GtkWidget *i_lbl = gtk_label_new (NULL);
  gtk_label_set_markup (GTK_LABEL (i_lbl), "<i>I</i>");
  gtk_button_set_child (GTK_BUTTON (self->text_italic_btn), i_lbl);
  gtk_widget_add_css_class (self->text_italic_btn, "flat");
  gtk_widget_set_tooltip_text (self->text_italic_btn, "Italic");
  g_signal_connect (self->text_italic_btn, "toggled", G_CALLBACK (on_text_italic_toggled), self);
  gtk_box_append (GTK_BOX (bar), self->text_italic_btn);

  GtkWidget *apply_btn = gtk_button_new_from_icon_name ("object-select-symbolic");
  gtk_widget_add_css_class (apply_btn, "suggested-action");
  gtk_widget_set_tooltip_text (apply_btn, "Apply Text (Enter)");
  g_signal_connect_swapped (apply_btn, "clicked", G_CALLBACK (paint_canvas_commit_text), self->canvas);
  gtk_box_append (GTK_BOX (bar), apply_btn);

  GtkWidget *cancel_btn = gtk_button_new_from_icon_name ("window-close-symbolic");
  gtk_widget_add_css_class (cancel_btn, "flat");
  gtk_widget_set_tooltip_text (cancel_btn, "Cancel Text (Esc)");
  g_signal_connect_swapped (cancel_btn, "clicked", G_CALLBACK (paint_canvas_cancel_text), self->canvas);
  gtk_box_append (GTK_BOX (bar), cancel_btn);

  return bar;
}

static void
on_canvas_hidden_layer_blocked (PaintCanvas *canvas G_GNUC_UNUSED, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (self->toast_overlay) {
    AdwToast *toast = adw_toast_new ("Active layer is hidden");
    adw_toast_set_timeout (toast, 2);
    adw_toast_overlay_add_toast (self->toast_overlay, toast);
  }
}

static const char *const LAYER_BLEND_LABELS[] = {
  "Normal", "Multiply", "Screen", "Overlay", "Darken", "Lighten", NULL
};

static void
draw_layer_thumb (GtkDrawingArea *area,
                  cairo_t *cr,
                  int width,
                  int height,
                  gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  PaintDocument *doc = self->canvas ? paint_canvas_get_document (self->canvas) : NULL;
  int idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (area), "paint-layer-idx"));
  cairo_surface_t *surf = doc ? paint_document_layer_get_surface (doc, idx) : NULL;
  if (!surf)
    return;

  double bw = MAX (1.0, (double) width - 2.0);
  double bh = MAX (1.0, (double) height - 2.0);
  double sw = (double) MAX (1, cairo_image_surface_get_width (surf));
  double sh = (double) MAX (1, cairo_image_surface_get_height (surf));
  double scale = MIN (bw / sw, bh / sh);
  double dw = MAX (1.0, sw * scale);
  double dh = MAX (1.0, sh * scale);
  double dx = 1.0 + (bw - dw) * 0.5;
  double dy = 1.0 + (bh - dh) * 0.5;

  cairo_save (cr);
  cairo_rectangle (cr, dx, dy, dw, dh);
  cairo_clip (cr);
  draw_checkerboard (cr, dx, dy, dw, dh, 4.0);
  cairo_translate (cr, dx, dy);
  cairo_scale (cr, scale, scale);
  cairo_set_source_surface (cr, surf, 0.0, 0.0);
  cairo_pattern_set_filter (cairo_get_source (cr), CAIRO_FILTER_BILINEAR);
  cairo_paint (cr);
  cairo_restore (cr);

  cairo_set_line_width (cr, 1.0);
  cairo_set_source_rgba (cr, 0.5, 0.5, 0.55, 0.45);
  cairo_rectangle (cr, dx - 0.5, dy - 0.5, dw + 1.0, dh + 1.0);
  cairo_stroke (cr);
}

typedef struct {
  PaintCanvasPage *page;
  int layer_index;
  GtkWidget *popover;
  GtkWidget *entry;
} LayerRenameCtx;

static void
on_layer_rename_apply (GtkWidget *widget G_GNUC_UNUSED, gpointer user_data)
{
  LayerRenameCtx *ctx = user_data;
  PaintCanvasPage *page = ctx->page;
  int idx = ctx->layer_index;
  GtkWidget *pop = ctx->popover;
  g_autofree char *name = g_strdup (gtk_editable_get_text (GTK_EDITABLE (ctx->entry)));
  gtk_popover_popdown (GTK_POPOVER (pop));
  paint_canvas_set_layer_name (page->canvas, idx, name);
}

static void
on_layer_rename_closed (GtkPopover *pop, gpointer user_data G_GNUC_UNUSED)
{
  if (gtk_widget_get_parent (GTK_WIDGET (pop)))
    gtk_widget_unparent (GTK_WIDGET (pop));
}

static void
on_layer_row_double_click (GtkGestureClick *gesture,
                           int n_press,
                           double x G_GNUC_UNUSED,
                           double y G_GNUC_UNUSED,
                           gpointer user_data)
{
  if (n_press != 2)
    return;

  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  GtkWidget *row = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
  int idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "paint-layer-idx"));
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  if (!doc)
    return;

  GtkWidget *pop = gtk_popover_new ();
  gtk_widget_set_parent (pop, row);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_top (box, 8);
  gtk_widget_set_margin_bottom (box, 8);
  gtk_widget_set_margin_start (box, 8);
  gtk_widget_set_margin_end (box, 8);

  GtkWidget *entry = gtk_entry_new ();
  gtk_editable_set_text (GTK_EDITABLE (entry), paint_document_layer_get_name (doc, idx));
  gtk_editable_set_width_chars (GTK_EDITABLE (entry), 16);
  gtk_box_append (GTK_BOX (box), entry);

  GtkWidget *ok_btn = gtk_button_new_with_label ("Rename");
  gtk_widget_add_css_class (ok_btn, "suggested-action");
  gtk_box_append (GTK_BOX (box), ok_btn);

  LayerRenameCtx *ctx = g_new0 (LayerRenameCtx, 1);
  ctx->page = self;
  ctx->layer_index = idx;
  ctx->popover = pop;
  ctx->entry = entry;
  g_object_set_data_full (G_OBJECT (pop), "rename-ctx", ctx, g_free);

  g_signal_connect (entry, "activate", G_CALLBACK (on_layer_rename_apply), ctx);
  g_signal_connect (ok_btn, "clicked", G_CALLBACK (on_layer_rename_apply), ctx);
  g_signal_connect (pop, "closed", G_CALLBACK (on_layer_rename_closed), NULL);

  gtk_popover_set_child (GTK_POPOVER (pop), box);
  gtk_popover_popup (GTK_POPOVER (pop));
  gtk_widget_grab_focus (entry);
  gtk_editable_select_region (GTK_EDITABLE (entry), 0, -1);
}

static void
on_layer_row_selected (GtkListBox *box G_GNUC_UNUSED,
                       GtkListBoxRow *row,
                       gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (self->syncing || !row)
    return;
  int idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "paint-layer-idx"));
  paint_canvas_set_active_layer (self->canvas, idx);
}

static void
on_layer_vis_clicked (GtkButton *btn, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (self->syncing)
    return;
  int idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (btn), "paint-layer-idx"));
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  gboolean vis = paint_document_layer_get_visible (doc, idx);
  paint_canvas_set_layer_visible (self->canvas, idx, !vis);
}

static void
on_layer_add_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  paint_canvas_add_layer (self->canvas, NULL);
}

static void
on_layer_dup_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  paint_canvas_duplicate_layer (self->canvas, paint_document_get_active_layer (doc));
}

static void
on_layer_merge_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  paint_canvas_merge_layer_down (self->canvas, paint_document_get_active_layer (doc));
}

static void
on_layer_del_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  paint_canvas_remove_layer (self->canvas, paint_document_get_active_layer (doc));
}

static void
on_layer_up_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  int active = paint_document_get_active_layer (doc);
  paint_canvas_move_layer (self->canvas, active, active + 1);
}

static void
on_layer_down_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  int active = paint_document_get_active_layer (doc);
  paint_canvas_move_layer (self->canvas, active, active - 1);
}

static void
on_layer_flatten_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  paint_canvas_flatten_layers (self->canvas);
}

static void
on_layer_blend_changed (GObject *obj, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (self->syncing)
    return;
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  guint sel = gtk_drop_down_get_selected (GTK_DROP_DOWN (obj));
  if (sel <= PAINT_BLEND_LIGHTEN)
    paint_canvas_set_layer_blend_mode (self->canvas, paint_document_get_active_layer (doc), (PaintBlendMode) sel);
}

static void
on_layer_opacity_changed (GtkRange *range, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (self->syncing)
    return;
  double val = gtk_range_get_value (range);
  g_autofree char *pct = g_strdup_printf ("%d%%", (int) lround (val));
  gtk_label_set_text (GTK_LABEL (self->layers_opacity_label), pct);
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  paint_canvas_set_layer_opacity (self->canvas, paint_document_get_active_layer (doc), val / 100.0);
}

static void
sync_layers_panel (PaintCanvasPage *self)
{
  if (!self->layers_list || !self->canvas)
    return;
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  if (!doc)
    return;

  guint64 rev = paint_document_get_layers_revision (doc);
  if (self->last_layers_doc == doc && self->last_layers_revision == rev) {
    if (paint_canvas_page_get_layers_visible (self)) {
      for (guint i = 0; i < self->layer_thumbs->len; i++)
        gtk_widget_queue_draw (GTK_WIDGET (g_ptr_array_index (self->layer_thumbs, i)));
    }
    return;
  }

  self->last_layers_doc = doc;
  self->last_layers_revision = rev;
  self->syncing = TRUE;

  g_ptr_array_set_size (self->layer_thumbs, 0);
  GtkWidget *child = gtk_widget_get_first_child (self->layers_list);
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling (child);
    gtk_list_box_remove (GTK_LIST_BOX (self->layers_list), child);
    child = next;
  }

  int n = paint_document_get_n_layers (doc);
  int active = CLAMP (paint_document_get_active_layer (doc), 0, MAX (0, n - 1));
  GtkListBoxRow *active_row = NULL;

  for (int idx = n - 1; idx >= 0; idx--) {
    GtkWidget *row = gtk_list_box_row_new ();
    g_object_set_data (G_OBJECT (row), "paint-layer-idx", GINT_TO_POINTER (idx));

    GtkWidget *hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class (hbox, "paint-layer-row");
    gboolean vis = paint_document_layer_get_visible (doc, idx);
    if (!vis)
      gtk_widget_add_css_class (hbox, "paint-layer-hidden");

    GtkWidget *thumb = gtk_drawing_area_new ();
    gtk_widget_set_size_request (thumb, 36, 28);
    g_object_set_data (G_OBJECT (thumb), "paint-layer-idx", GINT_TO_POINTER (idx));
    gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (thumb), draw_layer_thumb, self, NULL);
    gtk_box_append (GTK_BOX (hbox), thumb);
    g_ptr_array_add (self->layer_thumbs, thumb);

    GtkWidget *meta = gtk_box_new (GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_hexpand (meta, TRUE);
    gtk_widget_set_valign (meta, GTK_ALIGN_CENTER);

    const char *name = paint_document_layer_get_name (doc, idx);
    GtkWidget *name_lbl = gtk_label_new (name);
    gtk_label_set_ellipsize (GTK_LABEL (name_lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign (GTK_LABEL (name_lbl), 0.0f);
    if (idx == active)
      gtk_widget_add_css_class (name_lbl, "heading");
    gtk_box_append (GTK_BOX (meta), name_lbl);

    double opacity = paint_document_layer_get_opacity (doc, idx);
    PaintBlendMode blend = paint_document_layer_get_blend_mode (doc, idx);
    int op_pct = (int) lround (opacity * 100.0);
    if (blend != PAINT_BLEND_NORMAL || op_pct < 100) {
      g_autofree char *sub_txt = NULL;
      if (blend != PAINT_BLEND_NORMAL && op_pct < 100)
        sub_txt = g_strdup_printf ("%s · %d%%", LAYER_BLEND_LABELS[blend], op_pct);
      else if (blend != PAINT_BLEND_NORMAL)
        sub_txt = g_strdup (LAYER_BLEND_LABELS[blend]);
      else
        sub_txt = g_strdup_printf ("%d%%", op_pct);
      GtkWidget *sub_lbl = gtk_label_new (sub_txt);
      gtk_label_set_xalign (GTK_LABEL (sub_lbl), 0.0f);
      gtk_widget_add_css_class (sub_lbl, "caption");
      gtk_widget_add_css_class (sub_lbl, "dim-label");
      gtk_box_append (GTK_BOX (meta), sub_lbl);
    }
    gtk_box_append (GTK_BOX (hbox), meta);

    GtkWidget *vis_btn = gtk_button_new_from_icon_name (
        vis ? "view-reveal-symbolic" : "view-conceal-symbolic");
    gtk_widget_add_css_class (vis_btn, "flat");
    if (!vis)
      gtk_widget_add_css_class (vis_btn, "dim-label");
    gtk_widget_set_valign (vis_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text (vis_btn, vis ? "Hide Layer" : "Show Layer");
    g_object_set_data (G_OBJECT (vis_btn), "paint-layer-idx", GINT_TO_POINTER (idx));
    g_signal_connect (vis_btn, "clicked", G_CALLBACK (on_layer_vis_clicked), self);
    gtk_box_append (GTK_BOX (hbox), vis_btn);

    GtkGesture *dbl = gtk_gesture_click_new ();
    g_signal_connect (dbl, "pressed", G_CALLBACK (on_layer_row_double_click), self);
    gtk_widget_add_controller (row, GTK_EVENT_CONTROLLER (dbl));

    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), hbox);
    gtk_list_box_append (GTK_LIST_BOX (self->layers_list), row);
    if (idx == active)
      active_row = GTK_LIST_BOX_ROW (row);
  }

  if (active_row)
    gtk_list_box_select_row (GTK_LIST_BOX (self->layers_list), active_row);

  PaintBlendMode cur_blend = paint_document_layer_get_blend_mode (doc, active);
  double cur_opacity = paint_document_layer_get_opacity (doc, active);
  gtk_drop_down_set_selected (GTK_DROP_DOWN (self->layers_blend_dropdown), (guint) cur_blend);
  gtk_range_set_value (GTK_RANGE (self->layers_opacity_scale), cur_opacity * 100.0);
  g_autofree char *op_str = g_strdup_printf ("%d%%", (int) lround (cur_opacity * 100.0));
  gtk_label_set_text (GTK_LABEL (self->layers_opacity_label), op_str);

  gtk_widget_set_sensitive (self->layers_merge_btn, active > 0);
  gtk_widget_set_sensitive (self->layers_del_btn, n > 1);
  gtk_widget_set_sensitive (self->layers_up_btn, active < n - 1);
  gtk_widget_set_sensitive (self->layers_down_btn, active > 0);
  gtk_widget_set_sensitive (self->layers_flatten_btn, n > 1);

  self->syncing = FALSE;
}

static GtkWidget *
build_layers_sidebar (PaintCanvasPage *self)
{
  GtkWidget *side = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (side, "paint-layers-sidebar");

  GtkWidget *top = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_add_css_class (top, "paint-layers-toolbar");
  gtk_widget_set_margin_top (top, 8);
  gtk_widget_set_margin_bottom (top, 6);
  gtk_widget_set_margin_start (top, 12);
  gtk_widget_set_margin_end (top, 8);

  GtkWidget *heading = gtk_label_new ("Layers");
  gtk_widget_add_css_class (heading, "heading");
  gtk_widget_set_hexpand (heading, TRUE);
  gtk_widget_set_halign (heading, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (top), heading);

  self->layers_add_btn = gtk_button_new_from_icon_name ("list-add-symbolic");
  gtk_widget_add_css_class (self->layers_add_btn, "flat");
  gtk_widget_set_tooltip_text (self->layers_add_btn, "New Layer (Ctrl+Shift+L)");
  g_signal_connect (self->layers_add_btn, "clicked", G_CALLBACK (on_layer_add_clicked), self);
  gtk_box_append (GTK_BOX (top), self->layers_add_btn);

  self->layers_dup_btn = gtk_button_new_from_icon_name ("edit-copy-symbolic");
  gtk_widget_add_css_class (self->layers_dup_btn, "flat");
  gtk_widget_set_tooltip_text (self->layers_dup_btn, "Duplicate Layer");
  g_signal_connect (self->layers_dup_btn, "clicked", G_CALLBACK (on_layer_dup_clicked), self);
  gtk_box_append (GTK_BOX (top), self->layers_dup_btn);

  self->layers_merge_btn = gtk_button_new_from_icon_name ("layer-merge-down-symbolic");
  gtk_widget_add_css_class (self->layers_merge_btn, "flat");
  gtk_widget_set_tooltip_text (self->layers_merge_btn, "Merge Layer Down");
  g_signal_connect (self->layers_merge_btn, "clicked", G_CALLBACK (on_layer_merge_clicked), self);
  gtk_box_append (GTK_BOX (top), self->layers_merge_btn);

  self->layers_del_btn = gtk_button_new_from_icon_name ("list-remove-symbolic");
  gtk_widget_add_css_class (self->layers_del_btn, "flat");
  gtk_widget_set_tooltip_text (self->layers_del_btn, "Delete Layer");
  g_signal_connect (self->layers_del_btn, "clicked", G_CALLBACK (on_layer_del_clicked), self);
  gtk_box_append (GTK_BOX (top), self->layers_del_btn);
  gtk_box_append (GTK_BOX (side), top);

  GtkWidget *props = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_bottom (props, 8);
  gtk_widget_set_margin_start (props, 12);
  gtk_widget_set_margin_end (props, 12);

  GtkWidget *blend_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *blend_lbl = gtk_label_new ("Blend");
  gtk_widget_add_css_class (blend_lbl, "paint-dim-label");
  gtk_widget_set_halign (blend_lbl, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (blend_row), blend_lbl);

  self->layers_blend_dropdown = gtk_drop_down_new_from_strings (LAYER_BLEND_LABELS);
  gtk_widget_set_hexpand (self->layers_blend_dropdown, TRUE);
  g_signal_connect (self->layers_blend_dropdown, "notify::selected",
                    G_CALLBACK (on_layer_blend_changed), self);
  gtk_box_append (GTK_BOX (blend_row), self->layers_blend_dropdown);
  gtk_box_append (GTK_BOX (props), blend_row);

  GtkWidget *op_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *op_lbl = gtk_label_new ("Opacity");
  gtk_widget_add_css_class (op_lbl, "paint-dim-label");
  gtk_box_append (GTK_BOX (op_row), op_lbl);

  self->layers_opacity_scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0.0, 100.0, 1.0);
  gtk_widget_set_hexpand (self->layers_opacity_scale, TRUE);
  gtk_scale_set_draw_value (GTK_SCALE (self->layers_opacity_scale), FALSE);
  gtk_range_set_value (GTK_RANGE (self->layers_opacity_scale), 100.0);
  g_signal_connect (self->layers_opacity_scale, "value-changed",
                    G_CALLBACK (on_layer_opacity_changed), self);
  gtk_box_append (GTK_BOX (op_row), self->layers_opacity_scale);

  self->layers_opacity_label = gtk_label_new ("100%");
  gtk_widget_add_css_class (self->layers_opacity_label, "paint-color-alpha-label");
  gtk_label_set_xalign (GTK_LABEL (self->layers_opacity_label), 1.0f);
  gtk_box_append (GTK_BOX (op_row), self->layers_opacity_label);
  gtk_box_append (GTK_BOX (props), op_row);
  gtk_box_append (GTK_BOX (side), props);

  gtk_box_append (GTK_BOX (side), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

  GtkWidget *list_scroll = gtk_scrolled_window_new ();
  gtk_widget_set_vexpand (list_scroll, TRUE);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (list_scroll),
                                  GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);

  self->layers_list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (self->layers_list), GTK_SELECTION_SINGLE);
  gtk_widget_add_css_class (self->layers_list, "navigation-sidebar");
  gtk_widget_add_css_class (self->layers_list, "paint-layers-list");
  g_signal_connect (self->layers_list, "row-selected", G_CALLBACK (on_layer_row_selected), self);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (list_scroll), self->layers_list);
  gtk_box_append (GTK_BOX (side), list_scroll);

  gtk_box_append (GTK_BOX (side), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

  GtkWidget *bot = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class (bot, "paint-layers-toolbar");
  gtk_widget_set_margin_top (bot, 6);
  gtk_widget_set_margin_bottom (bot, 6);
  gtk_widget_set_margin_start (bot, 8);
  gtk_widget_set_margin_end (bot, 8);

  self->layers_up_btn = gtk_button_new_from_icon_name ("go-up-symbolic");
  gtk_widget_add_css_class (self->layers_up_btn, "flat");
  gtk_widget_set_tooltip_text (self->layers_up_btn, "Move Layer Up");
  g_signal_connect (self->layers_up_btn, "clicked", G_CALLBACK (on_layer_up_clicked), self);
  gtk_box_append (GTK_BOX (bot), self->layers_up_btn);

  self->layers_down_btn = gtk_button_new_from_icon_name ("go-down-symbolic");
  gtk_widget_add_css_class (self->layers_down_btn, "flat");
  gtk_widget_set_tooltip_text (self->layers_down_btn, "Move Layer Down");
  g_signal_connect (self->layers_down_btn, "clicked", G_CALLBACK (on_layer_down_clicked), self);
  gtk_box_append (GTK_BOX (bot), self->layers_down_btn);

  GtkWidget *spacer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand (spacer, TRUE);
  gtk_box_append (GTK_BOX (bot), spacer);

  self->layers_flatten_btn = gtk_button_new_with_label ("Flatten");
  gtk_widget_add_css_class (self->layers_flatten_btn, "flat");
  gtk_widget_set_tooltip_text (self->layers_flatten_btn, "Flatten All Layers");
  g_signal_connect (self->layers_flatten_btn, "clicked", G_CALLBACK (on_layer_flatten_clicked), self);
  gtk_box_append (GTK_BOX (bot), self->layers_flatten_btn);

  gtk_box_append (GTK_BOX (side), bot);
  return side;
}

static void
build_ui (PaintCanvasPage *self)
{
  self->toast_overlay = ADW_TOAST_OVERLAY (adw_toast_overlay_new ());
  adw_bin_set_child (ADW_BIN (self), GTK_WIDGET (self->toast_overlay));

  self->split_view = ADW_OVERLAY_SPLIT_VIEW (adw_overlay_split_view_new ());
  adw_overlay_split_view_set_sidebar_position (self->split_view, GTK_PACK_END);
  adw_overlay_split_view_set_show_sidebar (self->split_view, FALSE);
  adw_overlay_split_view_set_collapsed (self->split_view, FALSE);
  adw_overlay_split_view_set_enable_show_gesture (self->split_view, FALSE);
  adw_overlay_split_view_set_enable_hide_gesture (self->split_view, FALSE);
  adw_overlay_split_view_set_min_sidebar_width (self->split_view, 230.0);
  adw_overlay_split_view_set_max_sidebar_width (self->split_view, 265.0);

  self->overlay = GTK_OVERLAY (gtk_overlay_new ());
  adw_overlay_split_view_set_sidebar (self->split_view, build_layers_sidebar (self));
  adw_overlay_split_view_set_content (self->split_view, GTK_WIDGET (self->overlay));
  adw_toast_overlay_set_child (self->toast_overlay, GTK_WIDGET (self->split_view));

  self->scrolled = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  gtk_scrolled_window_set_kinetic_scrolling (self->scrolled, FALSE);
  gtk_scrolled_window_set_propagate_natural_width (self->scrolled, FALSE);
  gtk_scrolled_window_set_propagate_natural_height (self->scrolled, FALSE);
  gtk_widget_set_hexpand (GTK_WIDGET (self->scrolled), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self->scrolled), TRUE);
  gtk_widget_add_css_class (GTK_WIDGET (self->scrolled), "paint-workspace");

  GtkWidget *container = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand (container, TRUE);
  gtk_widget_set_vexpand (container, TRUE);
  gtk_widget_set_halign (container, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (container, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (container, WORKSPACE_MARGIN_TOP);
  gtk_widget_set_margin_bottom (container, WORKSPACE_MARGIN_BOTTOM);
  gtk_widget_set_margin_start (container, WORKSPACE_MARGIN_START);
  gtk_widget_set_margin_end (container, WORKSPACE_MARGIN_END);
  gtk_box_append (GTK_BOX (container), GTK_WIDGET (self->canvas));

  gtk_scrolled_window_set_child (self->scrolled, container);
  GtkWidget *viewport = gtk_scrolled_window_get_child (self->scrolled);
  if (GTK_IS_VIEWPORT (viewport))
    gtk_viewport_set_scroll_to_focus (GTK_VIEWPORT (viewport), FALSE);
  gtk_overlay_set_child (self->overlay, GTK_WIDGET (self->scrolled));

  GtkEventController *motion = gtk_event_controller_motion_new ();
  gtk_event_controller_set_propagation_phase (motion, GTK_PHASE_CAPTURE);
  g_signal_connect (motion, "motion", G_CALLBACK (on_scrolled_motion), self);
  g_signal_connect (motion, "leave", G_CALLBACK (on_scrolled_leave), self);
  gtk_widget_add_controller (GTK_WIDGET (self->scrolled), motion);

  GtkEventController *scroll = gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  g_signal_connect (scroll, "scroll", G_CALLBACK (on_scroll), self);
  gtk_widget_add_controller (GTK_WIDGET (self->scrolled), scroll);

  GtkGesture *pan = gtk_gesture_drag_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (pan), GDK_BUTTON_MIDDLE);
  g_signal_connect (pan, "drag-begin", G_CALLBACK (on_pan_begin), self);
  g_signal_connect (pan, "drag-update", G_CALLBACK (on_pan_update), self);
  gtk_widget_add_controller (GTK_WIDGET (self->scrolled), GTK_EVENT_CONTROLLER (pan));

  GtkGesture *pinch = gtk_gesture_zoom_new ();
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (pinch), GTK_PHASE_CAPTURE);
  g_signal_connect (pinch, "begin", G_CALLBACK (on_pinch_begin), self);
  g_signal_connect (pinch, "scale-changed", G_CALLBACK (on_pinch_scale_changed), self);
  g_signal_connect (pinch, "update", G_CALLBACK (on_pinch_update), self);
  g_signal_connect (pinch, "end", G_CALLBACK (on_pinch_end), self);
  g_signal_connect (pinch, "cancel", G_CALLBACK (on_pinch_end), self);
  gtk_widget_add_controller (GTK_WIDGET (self->scrolled), GTK_EVENT_CONTROLLER (pinch));

  GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment (self->scrolled);
  GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment (self->scrolled);
  g_signal_connect (hadj, "value-changed", G_CALLBACK (on_adjustment_changed), self);
  g_signal_connect (vadj, "value-changed", G_CALLBACK (on_adjustment_changed), self);
  g_signal_connect (hadj, "notify::page-size", G_CALLBACK (on_adjustment_prop_changed), self);
  g_signal_connect (vadj, "notify::page-size", G_CALLBACK (on_adjustment_prop_changed), self);

  /* Floating OSD pill */
  GtkWidget *pill = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_add_css_class (pill, "paint-osd-pill");
  gtk_widget_set_halign (pill, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (pill, GTK_ALIGN_END);
  gtk_widget_set_margin_bottom (pill, 16);

  GtkWidget *undo_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 1);
  self->undo_btn = gtk_button_new_from_icon_name ("edit-undo-symbolic");
  gtk_widget_add_css_class (self->undo_btn, "flat");
  gtk_widget_set_tooltip_text (self->undo_btn, "Undo (Ctrl+Z)");
  g_signal_connect_swapped (self->undo_btn, "clicked", G_CALLBACK (paint_canvas_undo), self->canvas);

  self->redo_btn = gtk_button_new_from_icon_name ("edit-redo-symbolic");
  gtk_widget_add_css_class (self->redo_btn, "flat");
  gtk_widget_set_tooltip_text (self->redo_btn, "Redo (Ctrl+Shift+Z)");
  g_signal_connect_swapped (self->redo_btn, "clicked", G_CALLBACK (paint_canvas_redo), self->canvas);

  gtk_box_append (GTK_BOX (undo_box), self->undo_btn);
  gtk_box_append (GTK_BOX (undo_box), self->redo_btn);
  gtk_box_append (GTK_BOX (pill), undo_box);

  gtk_box_append (GTK_BOX (pill), gtk_separator_new (GTK_ORIENTATION_VERTICAL));

  GtkWidget *tools_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 1);
  gsize n_tools = 0;
  const PaintTool *tools = paint_tools_get_all (&n_tools);
  GtkToggleButton *first_btn = NULL;
  for (gsize i = 0; i < n_tools; i++) {
    if (tools[i].is_shape || tools[i].is_transparency)
      continue;
    GtkWidget *btn = gtk_toggle_button_new ();
    gtk_button_set_icon_name (GTK_BUTTON (btn), tools[i].icon_name);
    gtk_widget_add_css_class (btn, "flat");
    gtk_widget_set_tooltip_text (btn, tools[i].tooltip);
    if (!first_btn)
      first_btn = GTK_TOGGLE_BUTTON (btn);
    else
      gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (btn), first_btn);

    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (btn),
                                  g_strcmp0 (tools[i].id, self->settings->active_tool_id) == 0);
    g_object_set_data (G_OBJECT (btn), "paint-tool-id", (gpointer) tools[i].id);
    g_signal_connect (btn, "toggled", G_CALLBACK (on_tool_toggled), self);
    gtk_box_append (GTK_BOX (tools_box), btn);
    g_hash_table_insert (self->tool_buttons, (gpointer) tools[i].id, btn);
  }

  GtkWidget *dummy_primary = gtk_toggle_button_new ();
  gtk_widget_set_visible (dummy_primary, FALSE);
  if (first_btn)
    gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (dummy_primary), first_btn);
  gtk_box_append (GTK_BOX (tools_box), dummy_primary);
  self->dummy_primary_btn = GTK_TOGGLE_BUTTON (dummy_primary);

  GtkWidget *trans_content = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
  self->transparent_btn_icon = gtk_image_new_from_icon_name ("tool-transparent-symbolic");
  GtkWidget *trans_arrow = gtk_image_new_from_icon_name ("pan-down-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (trans_arrow), 12);
  gtk_widget_set_opacity (trans_arrow, 0.75);
  gtk_box_append (GTK_BOX (trans_content), self->transparent_btn_icon);
  gtk_box_append (GTK_BOX (trans_content), trans_arrow);

  self->transparent_btn = gtk_menu_button_new ();
  gtk_widget_add_css_class (self->transparent_btn, "flat");
  gtk_widget_add_css_class (self->transparent_btn, "paint-transparent-button");
  gtk_menu_button_set_child (GTK_MENU_BUTTON (self->transparent_btn), trans_content);
  gtk_widget_set_tooltip_text (self->transparent_btn, "Transparency (T)");
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (self->transparent_btn),
                                GTK_WIDGET (build_transparency_popover (self)));
  gtk_box_append (GTK_BOX (tools_box), self->transparent_btn);

  GtkWidget *shapes_content = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
  self->shapes_btn_icon = gtk_image_new_from_icon_name ("tool-shapes-symbolic");
  GtkWidget *shapes_arrow = gtk_image_new_from_icon_name ("pan-down-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (shapes_arrow), 12);
  gtk_widget_set_opacity (shapes_arrow, 0.75);
  gtk_box_append (GTK_BOX (shapes_content), self->shapes_btn_icon);
  gtk_box_append (GTK_BOX (shapes_content), shapes_arrow);

  self->shapes_btn = gtk_menu_button_new ();
  gtk_widget_add_css_class (self->shapes_btn, "flat");
  gtk_widget_add_css_class (self->shapes_btn, "paint-shapes-button");
  gtk_menu_button_set_child (GTK_MENU_BUTTON (self->shapes_btn), shapes_content);
  gtk_widget_set_tooltip_text (self->shapes_btn, "Shapes (S)");
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (self->shapes_btn),
                                GTK_WIDGET (build_shapes_popover (self)));
  gtk_box_append (GTK_BOX (tools_box), self->shapes_btn);

  gtk_box_append (GTK_BOX (pill), tools_box);

  gtk_box_append (GTK_BOX (pill), gtk_separator_new (GTK_ORIENTATION_VERTICAL));

  self->size_btn = gtk_menu_button_new ();
  gtk_widget_add_css_class (self->size_btn, "flat");
  gtk_widget_add_css_class (self->size_btn, "paint-size-button");
  g_autofree char *size_lbl = g_strdup_printf ("%d px", (int) lround (self->settings->size));
  gtk_menu_button_set_label (GTK_MENU_BUTTON (self->size_btn), size_lbl);
  gtk_widget_set_tooltip_text (self->size_btn, "Brush Size ([ / ])");
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (self->size_btn), GTK_WIDGET (build_size_popover (self)));
  gtk_box_append (GTK_BOX (pill), self->size_btn);

  self->color_swatch = gtk_drawing_area_new ();
  gtk_widget_set_size_request (self->color_swatch, 22, 22);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (self->color_swatch),
                                  draw_color_circle, self, NULL);

  self->color_btn = gtk_menu_button_new ();
  gtk_widget_add_css_class (self->color_btn, "flat");
  gtk_widget_add_css_class (self->color_btn, "paint-color-button");
  gtk_menu_button_set_child (GTK_MENU_BUTTON (self->color_btn), self->color_swatch);
  gtk_widget_set_tooltip_text (self->color_btn, "Brush Color");
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (self->color_btn), GTK_WIDGET (build_color_popover (self)));
  gtk_box_append (GTK_BOX (pill), self->color_btn);

  self->osd_pill = pill;
  gtk_overlay_add_overlay (self->overlay, pill);

  self->selection_bar = build_selection_bar (self);
  gtk_overlay_add_overlay (self->overlay, self->selection_bar);

  self->text_bar = build_text_bar (self);
  gtk_overlay_add_overlay (self->overlay, self->text_bar);

  g_signal_connect (self->canvas, "changed", G_CALLBACK (on_canvas_changed), self);
  g_signal_connect (self->canvas, "zoom-changed", G_CALLBACK (on_canvas_zoom_changed), self);
  g_signal_connect (self->canvas, "color-picked", G_CALLBACK (on_canvas_color_picked), self);
  g_signal_connect (self->canvas, "selection-changed", G_CALLBACK (on_canvas_selection_changed), self);
  g_signal_connect (self->canvas, "text-state-changed", G_CALLBACK (on_canvas_text_state_changed), self);
  g_signal_connect (self->canvas, "hidden-layer-blocked", G_CALLBACK (on_canvas_hidden_layer_blocked), self);
}

static void
paint_canvas_page_finalize (GObject *object)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (object);
  g_clear_pointer (&self->layer_thumbs, g_ptr_array_unref);
  g_clear_pointer (&self->tool_buttons, g_hash_table_unref);
  G_OBJECT_CLASS (paint_canvas_page_parent_class)->finalize (object);
}

static void
paint_canvas_page_class_init (PaintCanvasPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = paint_canvas_page_finalize;

  signals[SIGNAL_STATE_CHANGED] = g_signal_new ("state-changed",
                                                G_TYPE_FROM_CLASS (klass),
                                                G_SIGNAL_RUN_FIRST,
                                                0, NULL, NULL, NULL,
                                                G_TYPE_NONE, 0);
}

static void
paint_canvas_page_init (PaintCanvasPage *self)
{
  self->tool_buttons = g_hash_table_new (g_str_hash, g_str_equal);
  self->layer_thumbs = g_ptr_array_new ();
  self->last_zoom = 1.0;
  self->last_shape_tool_id = "shape-rectangle";
  self->last_transparency_tool_id = "transparent";
  self->previous_tool_id = "brush";
}

PaintCanvasPage *
paint_canvas_page_new (PaintDocument *doc, PaintToolSettings *settings)
{
  PaintCanvasPage *self = g_object_new (PAINT_TYPE_CANVAS_PAGE, NULL);
  self->settings = settings;
  self->canvas = paint_canvas_new (doc, settings);
  self->last_zoom = paint_canvas_get_zoom (self->canvas);
  build_ui (self);
  paint_canvas_page_sync_controls (self);
  return self;
}

PaintCanvas *
paint_canvas_page_get_canvas (PaintCanvasPage *self)
{
  return self->canvas;
}

void
paint_canvas_page_select_tool (PaintCanvasPage *self, const char *tool_id)
{
  GtkToggleButton *btn = g_hash_table_lookup (self->tool_buttons, tool_id);
  if (btn)
    gtk_toggle_button_set_active (btn, TRUE);
}

void
paint_canvas_page_select_last_shape (PaintCanvasPage *self)
{
  const char *shape_id = self->last_shape_tool_id ? self->last_shape_tool_id : "shape-rectangle";
  paint_canvas_page_select_tool (self, shape_id);
}

void
paint_canvas_page_select_last_transparency (PaintCanvasPage *self)
{
  const char *tool_id = self->last_transparency_tool_id
                          ? self->last_transparency_tool_id
                          : "transparent";
  paint_canvas_page_select_tool (self, tool_id);
}

void
paint_canvas_page_set_brush_size (PaintCanvasPage *self, double size)
{
  double clamped = CLAMP (size, 1.0, 96.0);
  self->settings->size = clamped;
  self->syncing = TRUE;
  gtk_range_set_value (GTK_RANGE (self->size_scale), clamped);
  g_autofree char *lbl = g_strdup_printf ("%d px", (int) lround (clamped));
  gtk_menu_button_set_label (GTK_MENU_BUTTON (self->size_btn), lbl);
  self->syncing = FALSE;
  gtk_widget_queue_draw (GTK_WIDGET (self->canvas));
}

void
paint_canvas_page_adjust_brush_size (PaintCanvasPage *self, double delta)
{
  paint_canvas_page_set_brush_size (self, self->settings->size + delta);
}

gboolean
paint_canvas_page_zoom_fit (PaintCanvasPage *self)
{
  if (!self || !self->canvas || !self->scrolled)
    return FALSE;
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  if (!doc)
    return FALSE;

  double view_w = (double) gtk_widget_get_width (GTK_WIDGET (self->scrolled));
  double view_h = (double) gtk_widget_get_height (GTK_WIDGET (self->scrolled));
  if (view_w <= 0.0 || view_h <= 0.0) {
    GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment (self->scrolled);
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment (self->scrolled);
    view_w = gtk_adjustment_get_page_size (hadj);
    view_h = gtk_adjustment_get_page_size (vadj);
  }
  if (view_w <= 0.0 || view_h <= 0.0)
    return FALSE;

  double avail_w = MAX (32.0, view_w - (double) (WORKSPACE_MARGIN_START + WORKSPACE_MARGIN_END));
  double avail_h = MAX (32.0, view_h - (double) (WORKSPACE_MARGIN_TOP + WORKSPACE_MARGIN_BOTTOM));
  double dw = (double) MAX (1, paint_document_get_width (doc));
  double dh = (double) MAX (1, paint_document_get_height (doc));

  double fit = MIN (avail_w / dw, avail_h / dh);
  fit = CLAMP (fit, paint_canvas_get_min_zoom (), paint_canvas_get_max_zoom ());
  paint_canvas_set_zoom (self->canvas, fit);
  return TRUE;
}

void
paint_canvas_page_toggle_osd (PaintCanvasPage *self)
{
  gtk_widget_set_visible (self->osd_pill, !gtk_widget_get_visible (self->osd_pill));
}

void
paint_canvas_page_set_osd_visible (PaintCanvasPage *self, gboolean visible)
{
  gtk_widget_set_visible (self->osd_pill, visible);
}

void
paint_canvas_page_set_layers_visible (PaintCanvasPage *self, gboolean visible)
{
  if (!self || !self->split_view)
    return;
  adw_overlay_split_view_set_show_sidebar (self->split_view, visible);
  if (visible)
    sync_layers_panel (self);
}

gboolean
paint_canvas_page_get_layers_visible (PaintCanvasPage *self)
{
  return (self && self->split_view)
           ? adw_overlay_split_view_get_show_sidebar (self->split_view)
           : FALSE;
}

void
paint_canvas_page_sync_controls (PaintCanvasPage *self)
{
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  gtk_widget_set_sensitive (self->undo_btn, paint_document_can_undo (doc));
  gtk_widget_set_sensitive (self->redo_btn, paint_document_can_redo (doc));

  const PaintTool *active_tool = paint_tools_find (self->settings->active_tool_id);
  sync_tool_ui (self, active_tool);

  self->syncing = TRUE;
  if (self->shape_outline_btn && self->shape_fill_btn) {
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->shape_outline_btn),
                                  !self->settings->shape_filled);
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->shape_fill_btn),
                                  self->settings->shape_filled);
  }
  sync_shape_icons (self);

  gtk_range_set_value (GTK_RANGE (self->size_scale), self->settings->size);
  g_autofree char *lbl = g_strdup_printf ("%d px", (int) lround (self->settings->size));
  gtk_menu_button_set_label (GTK_MENU_BUTTON (self->size_btn), lbl);

  for (int i = 0; i < 3; i++) {
    if (self->brush_style_btns[i])
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->brush_style_btns[i]),
                                    (int) self->settings->brush_style == i);
  }
  if (self->text_size_spin)
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->text_size_spin), self->settings->text_font_size);
  if (self->text_bold_btn)
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->text_bold_btn), self->settings->text_bold);
  if (self->text_italic_btn)
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->text_italic_btn), self->settings->text_italic);
  if (self->text_font_dropdown) {
    guint sel = 0;
    if (g_strcmp0 (self->settings->text_font_family, "Serif") == 0)
      sel = 1;
    else if (g_strcmp0 (self->settings->text_font_family, "Monospace") == 0)
      sel = 2;
    gtk_drop_down_set_selected (GTK_DROP_DOWN (self->text_font_dropdown), sel);
  }
  self->syncing = FALSE;
  sync_hsv_from_settings (self);
  sync_recent_colors (self);
  sync_overlay_bars (self);
  sync_layers_panel (self);
}

char *
paint_canvas_page_dup_title (PaintCanvasPage *self)
{
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  GFile *file = paint_document_get_file (doc);
  g_autofree char *base = file ? g_file_get_basename (file) : g_strdup ("Untitled Canvas");
  if (paint_document_get_modified (doc))
    return g_strdup_printf ("• %s", base);
  return g_strdup (base);
}

char *
paint_canvas_page_dup_subtitle (PaintCanvasPage *self)
{
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  int w = paint_document_get_width (doc);
  int h = paint_document_get_height (doc);
  int zoom_pct = (int) lround (paint_canvas_get_zoom (self->canvas) * 100.0);
  GFile *file = paint_document_get_file (doc);
  if (file) {
    g_autoptr (GFile) parent = g_file_get_parent (file);
    g_autofree char *parent_path = parent ? g_file_get_path (parent) : NULL;
    if (parent_path)
      return g_strdup_printf ("%s · %d × %d · %d%%", parent_path, w, h, zoom_pct);
  }
  return g_strdup_printf ("%d × %d · %d%%", w, h, zoom_pct);
}
