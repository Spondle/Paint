#include "paint-color-picker.h"

#include <math.h>
#include <string.h>

#include "paint-i18n.h"

typedef enum {
  PICKER_SV,
  PICKER_HUE,
  PICKER_ALPHA
} PickerKind;

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

struct _PaintColorPicker {
  PaintToolSettings *settings;
  GtkWidget *focus_on_enter;
  PaintColorChangedCallback on_changed;
  gpointer user_data;
  PaintColorChangedCallback on_palette;
  gpointer palette_data;

  GtkWidget *root_box;
  GtkWidget *color_btn;
  GtkWidget *swap_btn;
  GtkWidget *color_swatch;
  GtkWidget *primary_target_btn;
  GtkWidget *secondary_target_btn;
  GtkWidget *color_sv_area;
  GtkWidget *color_hue_area;
  GtkWidget *color_alpha_area;
  GtkWidget *color_preview_area;
  GtkWidget *color_alpha_label;
  GtkWidget *color_hex_entry;
  GtkWidget *recent_colors_section;
  GtkWidget *recent_colors_box;
  GtkWidget *custom_colors_box;

  gboolean editing_secondary;
  float hsv_h;
  float hsv_s;
  float hsv_v;
  float hsv_a;
  gboolean syncing;
  gboolean syncing_external;
};

static inline GdkRGBA *
target_color_ptr (PaintColorPicker *self)
{
  return self->editing_secondary ? &self->settings->secondary_color
                                 : &self->settings->color;
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

void
paint_draw_checkerboard (cairo_t *cr, double x, double y, double w, double h, double cell)
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
update_color_widgets (PaintColorPicker *self)
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
  if (!self->syncing_external && self->on_changed)
    self->on_changed (self->user_data);

  if (self->color_alpha_label) {
    int pct = (int) lroundf (self->hsv_a * 100.0f);
    g_autofree char *alpha_lbl = g_strdup_printf ("%d%%", pct);
    gtk_label_set_text (GTK_LABEL (self->color_alpha_label), alpha_lbl);
  }

  if (self->color_hex_entry && !gtk_widget_has_focus (self->color_hex_entry)) {
    const GdkRGBA *c = target_color_ptr (self);
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

void
paint_color_picker_sync (PaintColorPicker *self)
{
  if (!self || !self->settings)
    return;
  const GdkRGBA *c = target_color_ptr (self);
  float cur_r = 0.0f, cur_g = 0.0f, cur_b = 0.0f;
  gtk_hsv_to_rgb (self->hsv_h, self->hsv_s, self->hsv_v, &cur_r, &cur_g, &cur_b);
  if (fabsf (cur_r - c->red) > 1e-3f ||
      fabsf (cur_g - c->green) > 1e-3f ||
      fabsf (cur_b - c->blue) > 1e-3f ||
      fabsf (self->hsv_a - c->alpha) > 1e-3f) {
    gtk_rgb_to_hsv (c->red, c->green, c->blue, &self->hsv_h, &self->hsv_s, &self->hsv_v);
    self->hsv_a = c->alpha;
  }
  self->syncing_external = TRUE;
  update_color_widgets (self);
  self->syncing_external = FALSE;
}

static void
apply_hsv_to_settings (PaintColorPicker *self)
{
  float r = 0.0f, g = 0.0f, b = 0.0f;
  gtk_hsv_to_rgb (self->hsv_h, self->hsv_s, self->hsv_v, &r, &g, &b);
  *target_color_ptr (self) = (GdkRGBA) { r, g, b, self->hsv_a };
  update_color_widgets (self);
}

static void
draw_circle_swatch (cairo_t *cr, double cx, double cy, double r, const GdkRGBA *color)
{
  cairo_save (cr);
  cairo_arc (cr, cx, cy, r, 0.0, 2.0 * G_PI);
  cairo_clip (cr);
  if (color->alpha < 0.999f)
    paint_draw_checkerboard (cr, cx - r, cy - r, r * 2.0, r * 2.0, 4.0);
  cairo_set_source_rgba (cr, color->red, color->green, color->blue, color->alpha);
  cairo_paint (cr);
  cairo_restore (cr);

  cairo_set_line_width (cr, 1.0);
  cairo_set_source_rgba (cr, 0.5, 0.5, 0.55, 0.55);
  cairo_arc (cr, cx, cy, r - 0.5, 0.0, 2.0 * G_PI);
  cairo_stroke (cr);
}

static void
draw_color_circle (GtkDrawingArea *area,
                   cairo_t *cr,
                   int width,
                   int height,
                   gpointer user_data)
{
  const GdkRGBA *rgba = (const GdkRGBA *) g_object_get_data (G_OBJECT (area), "paint-preset-rgba");
  PaintColorPicker *self = user_data;
  const GdkRGBA *color = rgba ? rgba : &self->settings->color;

  double size = MIN (width, height) - 2.0;
  double cx = width * 0.5;
  double cy = height * 0.5;
  double r = MAX (2.0, size * 0.5);
  draw_circle_swatch (cr, cx, cy, r, color);
}

static void
draw_dual_swatch (GtkDrawingArea *area G_GNUC_UNUSED,
                  cairo_t *cr,
                  int width,
                  int height,
                  gpointer user_data)
{
  PaintColorPicker *self = user_data;
  if (!self || !self->settings)
    return;
  double r = MAX (5.0, MIN (width, height) * 0.34);
  draw_circle_swatch (cr, (double) width - r - 1.5, (double) height - r - 1.5, r,
                      &self->settings->secondary_color);
  draw_circle_swatch (cr, r + 1.5, r + 1.5, r + 0.8,
                      &self->settings->color);
}

static void
draw_color_preview (GtkDrawingArea *area G_GNUC_UNUSED,
                    cairo_t *cr,
                    int width,
                    int height,
                    gpointer user_data)
{
  PaintColorPicker *self = user_data;
  const GdkRGBA *c = target_color_ptr (self);

  cairo_save (cr);
  draw_rounded_rect_path (cr, 0.5, 0.5, width - 1.0, height - 1.0, 6.0);
  cairo_clip (cr);
  if (c->alpha < 0.999f)
    paint_draw_checkerboard (cr, 0.0, 0.0, width, height, 5.0);
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
  PaintColorPicker *self = user_data;
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
  PaintColorPicker *self = user_data;

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
  PaintColorPicker *self = user_data;
  const GdkRGBA *c = target_color_ptr (self);

  cairo_save (cr);
  draw_rounded_rect_path (cr, 0.5, 0.5, width - 1.0, height - 1.0, 8.0);
  cairo_clip (cr);

  paint_draw_checkerboard (cr, 0.0, 0.0, width, height, 6.0);
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
update_picker_from_point (PaintColorPicker *self, GtkWidget *area, PickerKind kind, double x, double y)
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
  PaintColorPicker *self = user_data;
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
  PaintColorPicker *self = user_data;
  GtkWidget *area = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
  PickerKind kind = (PickerKind) GPOINTER_TO_INT (g_object_get_data (G_OBJECT (gesture), "paint-picker-kind"));
  const double *sx = g_object_get_data (G_OBJECT (gesture), "paint-start-x");
  const double *sy = g_object_get_data (G_OBJECT (gesture), "paint-start-y");
  if (!sx || !sy)
    return;

  update_picker_from_point (self, area, kind, *sx + offset_x, *sy + offset_y);
}

static void
attach_picker_drag (GtkWidget *area, PickerKind kind, PaintColorPicker *self)
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
  PaintColorPicker *self = user_data;
  const GdkRGBA *rgba = (const GdkRGBA *) g_object_get_data (G_OBJECT (btn), "paint-preset-color");
  if (!rgba)
    return;
  *target_color_ptr (self) = *rgba;
  paint_color_picker_sync (self);
  if (self->on_changed)
    self->on_changed (self->user_data);
}

static void
apply_hex_entry_text (PaintColorPicker *self, gboolean strict_hex)
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
    *target_color_ptr (self) = parsed;
    paint_color_picker_sync (self);
    if (self->on_changed)
      self->on_changed (self->user_data);
  }
}

static void
on_hex_entry_changed (GtkEditable *editable G_GNUC_UNUSED, gpointer user_data)
{
  apply_hex_entry_text (user_data, TRUE);
}

static void
on_hex_entry_activate (GtkEntry *entry G_GNUC_UNUSED, gpointer user_data)
{
  PaintColorPicker *self = user_data;
  apply_hex_entry_text (self, FALSE);
  if (self->focus_on_enter)
    gtk_widget_grab_focus (self->focus_on_enter);
  update_color_widgets (self);
}

void
paint_color_picker_sync_recent (PaintColorPicker *self)
{
  if (!self || !self->recent_colors_box || !self->recent_colors_section || !self->settings)
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
on_custom_swatch_right_click (GtkGestureClick *gesture,
                              int n_press G_GNUC_UNUSED,
                              double x G_GNUC_UNUSED,
                              double y G_GNUC_UNUSED,
                              gpointer user_data)
{
  PaintColorPicker *self = user_data;
  GtkWidget *btn = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
  int idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (btn), "paint-custom-idx"));
  paint_tool_settings_remove_custom_color (self->settings, idx);
  paint_color_picker_sync_custom (self);
  if (self->on_palette)
    self->on_palette (self->palette_data);
}

static void
on_add_custom_color_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  PaintColorPicker *self = user_data;
  paint_tool_settings_add_custom_color (self->settings, target_color_ptr (self));
  paint_color_picker_sync_custom (self);
  if (self->on_palette)
    self->on_palette (self->palette_data);
}

void
paint_color_picker_sync_custom (PaintColorPicker *self)
{
  if (!self || !self->custom_colors_box || !self->settings)
    return;

  GtkWidget *child = gtk_widget_get_first_child (self->custom_colors_box);
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling (child);
    gtk_flow_box_remove (GTK_FLOW_BOX (self->custom_colors_box), child);
    child = next;
  }

  for (int i = 0; i < self->settings->n_custom_colors; i++) {
    GtkWidget *btn = gtk_button_new ();
    gtk_widget_add_css_class (btn, "flat");
    gtk_widget_add_css_class (btn, "paint-color-preset");
    gtk_widget_set_tooltip_text (btn, _("Click to select · Right-click to remove"));
    GtkWidget *dot = gtk_drawing_area_new ();
    gtk_widget_set_size_request (dot, 18, 18);
    g_object_set_data_full (G_OBJECT (dot), "paint-preset-rgba",
                            g_memdup2 (&self->settings->custom_colors[i], sizeof (GdkRGBA)), g_free);
    gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (dot), draw_color_circle, self, NULL);
    gtk_button_set_child (GTK_BUTTON (btn), dot);
    g_object_set_data_full (G_OBJECT (btn), "paint-preset-color",
                            g_memdup2 (&self->settings->custom_colors[i], sizeof (GdkRGBA)), g_free);
    g_object_set_data (G_OBJECT (btn), "paint-custom-idx", GINT_TO_POINTER (i));
    g_signal_connect (btn, "clicked", G_CALLBACK (on_color_preset_clicked), self);

    GtkGesture *rclick = gtk_gesture_click_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (rclick), GDK_BUTTON_SECONDARY);
    g_signal_connect (rclick, "pressed", G_CALLBACK (on_custom_swatch_right_click), self);
    gtk_widget_add_controller (btn, GTK_EVENT_CONTROLLER (rclick));

    gtk_flow_box_append (GTK_FLOW_BOX (self->custom_colors_box), btn);
  }
}

static void
on_color_target_toggled (GtkToggleButton *btn, gpointer user_data)
{
  PaintColorPicker *self = user_data;
  if (self->syncing || !gtk_toggle_button_get_active (btn))
    return;
  self->editing_secondary = (GTK_WIDGET (btn) == self->secondary_target_btn);
  paint_color_picker_sync (self);
}

static void
on_swap_colors_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  PaintColorPicker *self = user_data;
  paint_tool_settings_swap_colors (self->settings);
  paint_color_picker_sync (self);
  if (self->on_changed)
    self->on_changed (self->user_data);
}

static void
on_reset_colors_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  PaintColorPicker *self = user_data;
  paint_tool_settings_reset_colors (self->settings);
  paint_color_picker_sync (self);
  if (self->on_changed)
    self->on_changed (self->user_data);
}

static void
on_color_popover_closed (GtkPopover *popover G_GNUC_UNUSED, gpointer user_data)
{
  PaintColorPicker *self = user_data;
  paint_tool_settings_add_recent_color (self->settings, target_color_ptr (self));
  paint_color_picker_sync_recent (self);
  if (self->on_palette)
    self->on_palette (self->palette_data);
}

static GtkPopover *
build_color_popover (PaintColorPicker *self)
{
  GtkWidget *pop = gtk_popover_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_top (box, 12);
  gtk_widget_set_margin_bottom (box, 12);
  gtk_widget_set_margin_start (box, 12);
  gtk_widget_set_margin_end (box, 12);

  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *target_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_add_css_class (target_box, "paint-shape-mode");

  self->primary_target_btn = gtk_toggle_button_new_with_label (_("Primary"));
  self->secondary_target_btn = gtk_toggle_button_new_with_label (_("Secondary"));
  gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (self->secondary_target_btn),
                               GTK_TOGGLE_BUTTON (self->primary_target_btn));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->primary_target_btn), TRUE);
  g_signal_connect (self->primary_target_btn, "toggled", G_CALLBACK (on_color_target_toggled), self);
  g_signal_connect (self->secondary_target_btn, "toggled", G_CALLBACK (on_color_target_toggled), self);
  gtk_box_append (GTK_BOX (target_box), self->primary_target_btn);
  gtk_box_append (GTK_BOX (target_box), self->secondary_target_btn);
  gtk_box_append (GTK_BOX (header), target_box);

  GtkWidget *reset_btn = gtk_button_new_with_label ("B/W");
  gtk_widget_add_css_class (reset_btn, "flat");
  gtk_widget_set_tooltip_text (reset_btn, _("Reset Black & White (D)"));
  g_signal_connect (reset_btn, "clicked", G_CALLBACK (on_reset_colors_clicked), self);
  gtk_box_append (GTK_BOX (header), reset_btn);

  GtkWidget *h_spacer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand (h_spacer, TRUE);
  gtk_box_append (GTK_BOX (header), h_spacer);

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
  GtkWidget *alpha_title = gtk_label_new (_("Opacity"));
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
  GtkWidget *recent_lbl = gtk_label_new (_("Recent"));
  gtk_widget_add_css_class (recent_lbl, "paint-dim-label");
  gtk_widget_set_halign (recent_lbl, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (self->recent_colors_section), recent_lbl);

  self->recent_colors_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_halign (self->recent_colors_box, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (self->recent_colors_section), self->recent_colors_box);
  gtk_box_append (GTK_BOX (box), self->recent_colors_section);

  GtkWidget *custom_section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  GtkWidget *custom_hdr = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *custom_lbl = gtk_label_new (_("Palette"));
  gtk_widget_add_css_class (custom_lbl, "paint-dim-label");
  gtk_widget_set_halign (custom_lbl, GTK_ALIGN_START);
  gtk_widget_set_hexpand (custom_lbl, TRUE);
  gtk_box_append (GTK_BOX (custom_hdr), custom_lbl);

  GtkWidget *add_custom_btn = gtk_button_new_from_icon_name ("list-add-symbolic");
  gtk_widget_add_css_class (add_custom_btn, "flat");
  gtk_widget_set_tooltip_text (add_custom_btn, _("Save Current Color to Palette"));
  g_signal_connect (add_custom_btn, "clicked", G_CALLBACK (on_add_custom_color_clicked), self);
  gtk_box_append (GTK_BOX (custom_hdr), add_custom_btn);
  gtk_box_append (GTK_BOX (custom_section), custom_hdr);

  self->custom_colors_box = gtk_flow_box_new ();
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (self->custom_colors_box), GTK_SELECTION_NONE);
  gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (self->custom_colors_box), 8);
  gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (self->custom_colors_box), 4);
  gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (self->custom_colors_box), 2);
  gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (self->custom_colors_box), 2);
  gtk_box_append (GTK_BOX (custom_section), self->custom_colors_box);
  gtk_box_append (GTK_BOX (box), custom_section);

  gtk_popover_set_child (GTK_POPOVER (pop), box);
  g_signal_connect (pop, "closed", G_CALLBACK (on_color_popover_closed), self);
  paint_color_picker_sync (self);
  paint_color_picker_sync_recent (self);
  paint_color_picker_sync_custom (self);
  return GTK_POPOVER (pop);
}

PaintColorPicker *
paint_color_picker_new (PaintToolSettings *settings,
                        GtkWidget *focus_on_enter,
                        PaintColorChangedCallback on_changed,
                        gpointer user_data)
{
  PaintColorPicker *self = g_new0 (PaintColorPicker, 1);
  self->settings = settings;
  self->focus_on_enter = focus_on_enter;
  self->on_changed = on_changed;
  self->user_data = user_data;

  self->color_swatch = gtk_drawing_area_new ();
  gtk_widget_set_size_request (self->color_swatch, 26, 24);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (self->color_swatch),
                                  draw_dual_swatch, self, NULL);

  self->color_btn = gtk_menu_button_new ();
  gtk_widget_add_css_class (self->color_btn, "flat");
  gtk_widget_add_css_class (self->color_btn, "paint-color-button");
  gtk_menu_button_set_child (GTK_MENU_BUTTON (self->color_btn), self->color_swatch);
  gtk_widget_set_tooltip_text (self->color_btn, _("Primary & Secondary Colors"));
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (self->color_btn),
                               GTK_WIDGET (build_color_popover (self)));

  self->swap_btn = gtk_button_new_from_icon_name ("color-swap-symbolic");
  gtk_widget_add_css_class (self->swap_btn, "flat");
  gtk_widget_set_tooltip_text (self->swap_btn, _("Swap Primary & Secondary Colors (Shift+X)"));
  g_signal_connect (self->swap_btn, "clicked", G_CALLBACK (on_swap_colors_clicked), self);

  self->root_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 1);
  gtk_box_append (GTK_BOX (self->root_box), self->color_btn);
  gtk_box_append (GTK_BOX (self->root_box), self->swap_btn);
  return self;
}

void
paint_color_picker_free (PaintColorPicker *picker)
{
  g_free (picker);
}

GtkWidget *
paint_color_picker_get_button (PaintColorPicker *picker)
{
  return picker ? picker->root_box : NULL;
}

void
paint_color_picker_set_palette_callback (PaintColorPicker *picker,
                                         PaintColorChangedCallback on_palette,
                                         gpointer user_data)
{
  if (!picker)
    return;
  picker->on_palette = on_palette;
  picker->palette_data = user_data;
}
