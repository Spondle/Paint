#include "paint-canvas-page.h"

#include <math.h>

#include "paint-color-picker.h"
#include "paint-i18n.h"
#include "paint-layers-panel.h"

#define WORKSPACE_MARGIN_TOP 16
#define WORKSPACE_MARGIN_BOTTOM 72
#define WORKSPACE_MARGIN_START 24
#define WORKSPACE_MARGIN_END 24

static const int BRUSH_PRESETS[] = { 2, 6, 12, 24, 48 };

enum {
  SIGNAL_STATE_CHANGED,
  SIGNAL_PALETTE_CHANGED,
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

  PaintLayersPanel *layers_panel;
  PaintColorPicker *color_picker;

  GtkWidget *osd_pill;
  GtkWidget *undo_btn;
  GtkWidget *redo_btn;
  GtkWidget *select_btn;
  GtkWidget *select_btn_icon;
  GtkWidget *select_popover;
  GtkWidget *retouch_btn;
  GtkWidget *retouch_btn_icon;
  GtkWidget *retouch_popover;
  GtkWidget *shapes_btn;
  GtkWidget *shapes_btn_icon;
  GtkWidget *shapes_popover;
  GtkWidget *shape_outline_btn;
  GtkWidget *shape_fill_btn;
  GtkWidget *transparent_btn;
  GtkWidget *transparent_btn_icon;
  GtkWidget *transparent_popover;
  GtkToggleButton *dummy_primary_btn;
  GtkToggleButton *dummy_select_btn;
  GtkToggleButton *dummy_retouch_btn;
  GtkToggleButton *dummy_shape_btn;
  GtkToggleButton *dummy_transparent_btn;
  const char *last_select_tool_id;
  const char *last_retouch_tool_id;
  const char *last_shape_tool_id;
  const char *last_transparency_tool_id;
  const char *previous_tool_id;
  GtkWidget *size_btn;
  GtkWidget *size_popover;
  GtkScale *size_scale;
  GtkWidget *brush_style_btns[3];
  GtkWidget *selection_bar;
  GtkWidget *fill_options_bar;
  GtkWidget *fill_tol_scale;
  GtkWidget *fill_tol_label;
  GtkWidget *fill_sample_all_btn;
  GtkWidget *gradient_bar;
  GtkWidget *gradient_linear_btn;
  GtkWidget *gradient_radial_btn;
  GtkWidget *text_bar;
  GtkWidget *text_view;
  GtkTextBuffer *text_buffer;
  GtkWidget *text_font_btn;
  GtkWidget *text_size_spin;
  GtkWidget *text_bold_btn;
  GtkWidget *text_italic_btn;
  GtkWidget *text_align_btns[3];
  GtkWidget *text_outline_btn;
  GtkWidget *text_bg_pill_btn;
  GtkWidget *coords_hud;
  GtkWidget *context_menu;
  GHashTable *tool_buttons; /* char* -> GtkToggleButton* */

  gboolean cursor_inside;
  int cursor_x;
  int cursor_y;
  gboolean edge_resizing;
  int edge_drag_orig_w;
  int edge_drag_orig_h;
  int edge_preview_w;
  int edge_preview_h;
  gboolean space_pan_active;
  gboolean space_pan_dragging;

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

  if (tot_w > gtk_adjustment_get_upper (hadj))
    gtk_adjustment_set_upper (hadj, tot_w);
  gtk_adjustment_set_value (hadj, target_h);

  if (tot_h > gtk_adjustment_get_upper (vadj))
    gtk_adjustment_set_upper (vadj, tot_h);
  gtk_adjustment_set_value (vadj, target_v);
}

static void
update_coords_hud (PaintCanvasPage *self)
{
  if (!self || !self->coords_hud || !self->canvas)
    return;
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  if (!doc)
    return;

  if (self->edge_resizing) {
    g_autofree char *txt = g_strdup_printf (_("Resize: %d × %d px"),
                                            self->edge_preview_w,
                                            self->edge_preview_h);
    gtk_label_set_text (GTK_LABEL (self->coords_hud), txt);
    return;
  }

  g_autoptr (GString) str = g_string_new ("");
  if (self->cursor_inside)
    g_string_append_printf (str, "%d, %d px · ", self->cursor_x, self->cursor_y);

  cairo_rectangle_int_t sr = { 0 };
  if (paint_canvas_get_selection_mask_and_rect (self->canvas, &sr, NULL))
    g_string_append_printf (str, _("Sel %d × %d · "), sr.width, sr.height);

  g_string_append_printf (str, "%d × %d px",
                          paint_document_get_width (doc),
                          paint_document_get_height (doc));
  gtk_label_set_text (GTK_LABEL (self->coords_hud), str->str);
}

static void
on_canvas_cursor_moved (PaintCanvas *canvas G_GNUC_UNUSED,
                        gboolean inside,
                        int x,
                        int y,
                        gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  self->cursor_inside = inside;
  self->cursor_x = x;
  self->cursor_y = y;
  update_coords_hud (self);
}

static void
on_canvas_changed (PaintCanvas *canvas G_GNUC_UNUSED, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  paint_canvas_page_sync_controls (self);
  update_coords_hud (self);
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

static void sync_overlay_bars (PaintCanvasPage *self);

static void
sync_gradient_icon (PaintCanvasPage *self)
{
  GtkToggleButton *gbtn = g_hash_table_lookup (self->tool_buttons, "gradient");
  if (!gbtn)
    return;
  const PaintTool *gtool = paint_tools_find ("gradient");
  gtk_button_set_icon_name (GTK_BUTTON (gbtn),
                            paint_tool_get_icon_name (gtool, self->settings));
}

static void
sync_tool_ui (PaintCanvasPage *self, const PaintTool *tool)
{
  if (!tool)
    return;

  self->syncing = TRUE;
  if (self->dummy_primary_btn &&
      (tool->is_select || tool->is_retouch || tool->is_shape || tool->is_transparency)) {
    gtk_toggle_button_set_active (self->dummy_primary_btn, TRUE);
  }
  if (self->dummy_select_btn && !tool->is_select)
    gtk_toggle_button_set_active (self->dummy_select_btn, TRUE);
  if (self->dummy_retouch_btn && !tool->is_retouch)
    gtk_toggle_button_set_active (self->dummy_retouch_btn, TRUE);
  if (self->dummy_shape_btn && !tool->is_shape)
    gtk_toggle_button_set_active (self->dummy_shape_btn, TRUE);
  if (self->dummy_transparent_btn && !tool->is_transparency)
    gtk_toggle_button_set_active (self->dummy_transparent_btn, TRUE);

  if (self->select_btn) {
    if (tool->is_select) {
      self->last_select_tool_id = tool->id;
      if (self->select_btn_icon)
        gtk_image_set_from_icon_name (GTK_IMAGE (self->select_btn_icon), tool->icon_name);
      gtk_widget_add_css_class (self->select_btn, "paint-tool-active");
    } else {
      gtk_widget_remove_css_class (self->select_btn, "paint-tool-active");
    }
  }
  if (self->retouch_btn) {
    if (tool->is_retouch) {
      self->last_retouch_tool_id = tool->id;
      if (self->retouch_btn_icon)
        gtk_image_set_from_icon_name (GTK_IMAGE (self->retouch_btn_icon), tool->icon_name);
      gtk_widget_add_css_class (self->retouch_btn, "paint-tool-active");
    } else {
      gtk_widget_remove_css_class (self->retouch_btn, "paint-tool-active");
    }
  }
  if (self->shapes_btn) {
    if (tool->is_shape) {
      self->last_shape_tool_id = tool->id;
      if (self->shapes_btn_icon)
        gtk_image_set_from_icon_name (GTK_IMAGE (self->shapes_btn_icon),
                                      paint_tool_get_icon_name (tool, self->settings));
      gtk_widget_add_css_class (self->shapes_btn, "paint-tool-active");
    } else {
      gtk_widget_remove_css_class (self->shapes_btn, "paint-tool-active");
    }
  }
  if (self->transparent_btn) {
    if (tool->is_transparency) {
      self->last_transparency_tool_id = tool->id;
      if (self->transparent_btn_icon)
        gtk_image_set_from_icon_name (GTK_IMAGE (self->transparent_btn_icon), tool->icon_name);
      gtk_widget_add_css_class (self->transparent_btn, "paint-tool-active");
    } else {
      gtk_widget_remove_css_class (self->transparent_btn, "paint-tool-active");
    }
  }

  GtkToggleButton *active_btn = g_hash_table_lookup (self->tool_buttons, tool->id);
  if (active_btn && !gtk_toggle_button_get_active (active_btn))
    gtk_toggle_button_set_active (active_btn, TRUE);
  sync_gradient_icon (self);
  self->syncing = FALSE;
  sync_overlay_bars (self);
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
    if (tool->is_select && self->select_popover)
      gtk_popover_popdown (GTK_POPOVER (self->select_popover));
    if (tool->is_retouch && self->retouch_popover)
      gtk_popover_popdown (GTK_POPOVER (self->retouch_popover));
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
on_picker_color_changed (gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (self->canvas && paint_canvas_has_active_text (self->canvas))
    gtk_widget_queue_draw (GTK_WIDGET (self->canvas));
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
on_space_pan_begin (GtkGestureDrag *gesture,
                    double start_x,
                    double start_y,
                    gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (!self->space_pan_active) {
    gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_DENIED);
    return;
  }
  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  self->space_pan_dragging = TRUE;
  on_pan_begin (gesture, start_x, start_y, self);
  paint_canvas_set_pan_override (self->canvas, TRUE, TRUE);
  gtk_widget_set_cursor_from_name (GTK_WIDGET (self->scrolled), "grabbing");
}

static void
on_space_pan_update (GtkGestureDrag *gesture,
                     double offset_x,
                     double offset_y,
                     gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (!self->space_pan_dragging)
    return;
  on_pan_update (gesture, offset_x, offset_y, self);
}

static void
on_space_pan_end (GtkGestureDrag *gesture G_GNUC_UNUSED,
                  double offset_x G_GNUC_UNUSED,
                  double offset_y G_GNUC_UNUSED,
                  gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (!self->space_pan_dragging)
    return;
  self->space_pan_dragging = FALSE;
  paint_canvas_set_pan_override (self->canvas, self->space_pan_active, FALSE);
  gtk_widget_set_cursor_from_name (GTK_WIDGET (self->scrolled),
                                   self->space_pan_active ? "grab" : NULL);
}

static void
on_space_pan_cancel (GtkGesture *gesture G_GNUC_UNUSED,
                     GdkEventSequence *sequence G_GNUC_UNUSED,
                     gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (!self->space_pan_dragging)
    return;
  self->space_pan_dragging = FALSE;
  paint_canvas_set_pan_override (self->canvas, self->space_pan_active, FALSE);
  gtk_widget_set_cursor_from_name (GTK_WIDGET (self->scrolled),
                                   self->space_pan_active ? "grab" : NULL);
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

  GtkWidget *heading = gtk_label_new (_("Brush Size"));
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
  GtkWidget *style_lbl = gtk_label_new (_("Style"));
  gtk_widget_add_css_class (style_lbl, "paint-dim-label");
  gtk_widget_set_halign (style_lbl, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (style_section), style_lbl);

  GtkWidget *style_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_box_set_homogeneous (GTK_BOX (style_box), TRUE);
  gtk_widget_set_hexpand (style_box, TRUE);
  gtk_widget_add_css_class (style_box, "paint-shape-mode");
  const char *style_labels[3] = { _("Smooth"), _("Soft"), _("Pixel") };
  GtkToggleButton *first_style = NULL;
  for (int i = 0; i < 3; i++) {
    GtkWidget *sbtn = gtk_toggle_button_new_with_label (style_labels[i]);
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
  GtkWidget *heading = gtk_label_new (_("Shapes"));
  gtk_widget_add_css_class (heading, "heading");
  gtk_widget_set_halign (heading, GTK_ALIGN_START);
  gtk_widget_set_hexpand (heading, TRUE);
  gtk_box_append (GTK_BOX (header), heading);

  GtkWidget *mode_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class (mode_box, "paint-shape-mode");

  self->shape_outline_btn = gtk_toggle_button_new_with_label (_("Outline"));
  self->shape_fill_btn = gtk_toggle_button_new_with_label (_("Filled"));
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
    gtk_widget_set_tooltip_text (btn, _(tools[i].tooltip));

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

  GtkWidget *hint = gtk_label_new (_("Hold Shift for proportions · Space to move"));
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
build_tool_group_popover (PaintCanvasPage *self,
                          const char *heading_text,
                          const char *hint_text,
                          gboolean is_select,
                          gboolean is_retouch,
                          gboolean is_transparency,
                          GtkToggleButton **out_dummy_btn,
                          const char *extra_clear_label)
{
  GtkWidget *pop = gtk_popover_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_top (box, 10);
  gtk_widget_set_margin_bottom (box, 10);
  gtk_widget_set_margin_start (box, 12);
  gtk_widget_set_margin_end (box, 12);

  GtkWidget *heading = gtk_label_new (heading_text);
  gtk_widget_add_css_class (heading, "heading");
  gtk_widget_set_halign (heading, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (box), heading);

  GtkWidget *grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_homogeneous (GTK_GRID (grid), TRUE);

  gsize n_tools = 0;
  const PaintTool *tools = paint_tools_get_all (&n_tools);
  GtkToggleButton *first_btn = NULL;
  int idx = 0;
  for (gsize i = 0; i < n_tools; i++) {
    gboolean match = (is_select && tools[i].is_select) ||
                     (is_retouch && tools[i].is_retouch) ||
                     (is_transparency && tools[i].is_transparency);
    if (!match)
      continue;

    GtkWidget *btn = gtk_toggle_button_new ();
    gtk_widget_add_css_class (btn, "flat");
    gtk_widget_add_css_class (btn, "paint-transparency-tile");
    gtk_widget_set_tooltip_text (btn, _(tools[i].tooltip));

    GtkWidget *content = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign (content, GTK_ALIGN_START);
    GtkWidget *img = gtk_image_new_from_icon_name (
        paint_tool_get_icon_name (&tools[i], self->settings));
    gtk_image_set_pixel_size (GTK_IMAGE (img), 16);
    GtkWidget *lbl = gtk_label_new (_(tools[i].name));
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

    gtk_grid_attach (GTK_GRID (grid), btn, idx % 2, idx / 2, 1, 1);
    idx++;
  }

  GtkWidget *dummy = gtk_toggle_button_new ();
  gtk_widget_set_visible (dummy, FALSE);
  if (first_btn)
    gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (dummy), first_btn);
  gtk_box_append (GTK_BOX (box), dummy);
  if (out_dummy_btn)
    *out_dummy_btn = GTK_TOGGLE_BUTTON (dummy);

  gtk_box_append (GTK_BOX (box), grid);

  if (extra_clear_label) {
    GtkWidget *clear_btn = gtk_button_new_with_label (extra_clear_label);
    gtk_widget_add_css_class (clear_btn, "paint-transparency-clear");
    g_signal_connect (clear_btn, "clicked", G_CALLBACK (on_clear_transparent_clicked), self);
    gtk_box_append (GTK_BOX (box), clear_btn);
  }

  if (hint_text) {
    GtkWidget *hint = gtk_label_new (hint_text);
    gtk_widget_add_css_class (hint, "paint-dim-label");
    gtk_widget_set_halign (hint, GTK_ALIGN_START);
    gtk_box_append (GTK_BOX (box), hint);
  }

  gtk_popover_set_child (GTK_POPOVER (pop), box);
  return GTK_POPOVER (pop);
}

static GtkPopover *
build_transparency_popover (PaintCanvasPage *self)
{
  self->transparent_popover = GTK_WIDGET (build_tool_group_popover (
      self,
      _("Transparency"),
      _("Save as PNG or WebP to preserve transparency"),
      FALSE, FALSE, TRUE,
      &self->dummy_transparent_btn,
      _("Clear Canvas to Transparent")));
  return GTK_POPOVER (self->transparent_popover);
}

static GtkPopover *
build_select_popover (PaintCanvasPage *self)
{
  self->select_popover = GTK_WIDGET (build_tool_group_popover (
      self,
      _("Selection"),
      _("Hold Shift for 1:1 · Drag corners to resize"),
      TRUE, FALSE, FALSE,
      &self->dummy_select_btn,
      NULL));
  return GTK_POPOVER (self->select_popover);
}

static GtkPopover *
build_retouch_popover (PaintCanvasPage *self)
{
  self->retouch_popover = GTK_WIDGET (build_tool_group_popover (
      self,
      _("Retouch"),
      _("Uses current brush size"),
      FALSE, TRUE, FALSE,
      &self->dummy_retouch_btn,
      NULL));
  return GTK_POPOVER (self->retouch_popover);
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
  if (!self->canvas || !self->settings)
    return;
  gboolean has_text = paint_canvas_has_active_text (self->canvas);
  gboolean has_sel = paint_canvas_has_selection (self->canvas);
  const char *tid = self->settings->active_tool_id;
  gboolean show_fill = !has_text && !has_sel &&
                       (g_strcmp0 (tid, "fill") == 0 ||
                        g_strcmp0 (tid, "transparent-fill") == 0 ||
                        g_strcmp0 (tid, "select-wand") == 0);
  gboolean show_grad = !has_text && !has_sel && (g_strcmp0 (tid, "gradient") == 0);

  if (self->text_bar)
    gtk_widget_set_visible (self->text_bar, has_text);
  if (self->selection_bar)
    gtk_widget_set_visible (self->selection_bar, !has_text && has_sel);
  if (self->fill_options_bar)
    gtk_widget_set_visible (self->fill_options_bar, show_fill);
  if (self->gradient_bar)
    gtk_widget_set_visible (self->gradient_bar, show_grad);
}

static void
on_picker_palette_changed (gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  g_signal_emit (self, signals[SIGNAL_PALETTE_CHANGED], 0);
}

static void
on_canvas_color_picked (PaintCanvas *canvas G_GNUC_UNUSED, gboolean finished, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  paint_color_picker_sync (self->color_picker);
  if (finished) {
    paint_color_picker_sync_recent (self->color_picker);
    g_signal_emit (self, signals[SIGNAL_PALETTE_CHANGED], 0);
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
  update_coords_hud (self);
  g_signal_emit (self, signals[SIGNAL_STATE_CHANGED], 0);
}

static void
on_canvas_text_state_changed (PaintCanvas *canvas, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  gboolean active = paint_canvas_has_active_text (canvas);
  sync_overlay_bars (self);
  if (active && self->text_buffer && self->text_view) {
    self->syncing = TRUE;
    gtk_text_buffer_set_text (self->text_buffer, paint_canvas_get_active_text (canvas), -1);
    self->syncing = FALSE;
    gtk_widget_grab_focus (self->text_view);
  } else if (!active && self->canvas) {
    gtk_widget_grab_focus (GTK_WIDGET (self->canvas));
  }
}

static void
on_text_buffer_changed (GtkTextBuffer *buffer, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (self->syncing)
    return;
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds (buffer, &start, &end);
  g_autofree char *txt = gtk_text_buffer_get_text (buffer, &start, &end, FALSE);
  paint_canvas_set_active_text (self->canvas, txt);
}

static gboolean
on_text_view_key_pressed (GtkEventControllerKey *ctrl G_GNUC_UNUSED,
                          guint keyval,
                          guint keycode G_GNUC_UNUSED,
                          GdkModifierType state,
                          gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (keyval == GDK_KEY_Escape) {
    paint_canvas_cancel_text (self->canvas);
    return TRUE;
  }
  if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) &&
      (state & GDK_CONTROL_MASK)) {
    paint_canvas_commit_text (self->canvas);
    return TRUE;
  }
  return FALSE;
}

static void
on_text_font_desc_changed (GObject *obj, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (self->syncing)
    return;
  PangoFontDescription *desc = gtk_font_dialog_button_get_font_desc (GTK_FONT_DIALOG_BUTTON (obj));
  const char *family = desc ? pango_font_description_get_family (desc) : "Sans";
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

static void
on_text_align_toggled (GtkToggleButton *btn, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (self->syncing || !gtk_toggle_button_get_active (btn))
    return;
  int al = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (btn), "paint-text-align"));
  self->settings->text_align = (PangoAlignment) CLAMP (al, 0, 2);
  gtk_widget_queue_draw (GTK_WIDGET (self->canvas));
}

static void
on_text_outline_toggled (GtkToggleButton *btn, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (self->syncing)
    return;
  self->settings->text_outline = gtk_toggle_button_get_active (btn);
  gtk_widget_queue_draw (GTK_WIDGET (self->canvas));
}

static void
on_text_bg_pill_toggled (GtkToggleButton *btn, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (self->syncing)
    return;
  self->settings->text_bg_pill = gtk_toggle_button_get_active (btn);
  gtk_widget_queue_draw (GTK_WIDGET (self->canvas));
}

static void
on_fill_tol_changed (GtkRange *range, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (self->syncing)
    return;
  int val = (int) lround (gtk_range_get_value (range));
  self->settings->fill_tolerance = CLAMP (val, 0, 255);
  g_autofree char *lbl = g_strdup_printf ("%d", self->settings->fill_tolerance);
  if (self->fill_tol_label)
    gtk_label_set_text (GTK_LABEL (self->fill_tol_label), lbl);
}

static void
on_fill_sample_all_toggled (GtkToggleButton *btn, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (self->syncing)
    return;
  self->settings->fill_sample_all_layers = gtk_toggle_button_get_active (btn);
}

static void
on_gradient_mode_toggled (GtkToggleButton *btn, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (self->syncing || !gtk_toggle_button_get_active (btn))
    return;
  self->settings->gradient_radial = (GTK_WIDGET (btn) == self->gradient_radial_btn);
  sync_gradient_icon (self);
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

  add_action_pill_button (bar, "edit-copy-symbolic", _("Copy Selection (Ctrl+C)"), "win.copy");
  add_action_pill_button (bar, "edit-cut-symbolic", _("Cut Selection (Ctrl+X)"), "win.cut");
  add_action_pill_button (bar, "tool-crop-symbolic", _("Crop to Selection (Ctrl+Shift+X)"), "win.crop-selection");
  GtkWidget *inv_btn = gtk_button_new_with_label (_("Invert"));
  gtk_widget_add_css_class (inv_btn, "flat");
  gtk_widget_set_tooltip_text (inv_btn, _("Invert Selection (Ctrl+Shift+I)"));
  gtk_actionable_set_action_name (GTK_ACTIONABLE (inv_btn), "win.invert-selection");
  gtk_box_append (GTK_BOX (bar), inv_btn);
  gtk_box_append (GTK_BOX (bar), gtk_separator_new (GTK_ORIENTATION_VERTICAL));
  add_action_pill_button (bar, "object-rotate-left-symbolic", _("Rotate Selection Left 90°"), "win.rotate-ccw");
  add_action_pill_button (bar, "object-rotate-right-symbolic", _("Rotate Selection Right 90°"), "win.rotate-cw");
  add_action_pill_button (bar, "object-flip-horizontal-symbolic", _("Flip Selection Horizontally"), "win.flip-horizontal");
  add_action_pill_button (bar, "object-flip-vertical-symbolic", _("Flip Selection Vertically"), "win.flip-vertical");
  gtk_box_append (GTK_BOX (bar), gtk_separator_new (GTK_ORIENTATION_VERTICAL));
  add_action_pill_button (bar, "edit-delete-symbolic", _("Delete Selection (Del)"), "win.delete-selection");

  GtkWidget *done_btn = gtk_button_new_from_icon_name ("object-select-symbolic");
  gtk_widget_add_css_class (done_btn, "suggested-action");
  gtk_widget_set_tooltip_text (done_btn, _("Apply Selection (Enter)"));
  g_signal_connect_swapped (done_btn, "clicked", G_CALLBACK (paint_canvas_commit_selection), self->canvas);
  gtk_box_append (GTK_BOX (bar), done_btn);
  return bar;
}

static GtkWidget *
build_fill_options_bar (PaintCanvasPage *self)
{
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class (bar, "paint-osd-pill");
  gtk_widget_add_css_class (bar, "paint-sub-pill");
  gtk_widget_set_halign (bar, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (bar, GTK_ALIGN_END);
  gtk_widget_set_margin_bottom (bar, 68);
  gtk_widget_set_visible (bar, FALSE);

  GtkWidget *tol_lbl = gtk_label_new (_("Tolerance"));
  gtk_widget_add_css_class (tol_lbl, "paint-dim-label");
  gtk_box_append (GTK_BOX (bar), tol_lbl);

  self->fill_tol_scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0.0, 255.0, 1.0);
  gtk_widget_set_size_request (self->fill_tol_scale, 130, -1);
  gtk_scale_set_draw_value (GTK_SCALE (self->fill_tol_scale), FALSE);
  gtk_range_set_value (GTK_RANGE (self->fill_tol_scale), (double) self->settings->fill_tolerance);
  g_signal_connect (self->fill_tol_scale, "value-changed", G_CALLBACK (on_fill_tol_changed), self);
  gtk_box_append (GTK_BOX (bar), self->fill_tol_scale);

  g_autofree char *val_str = g_strdup_printf ("%d", self->settings->fill_tolerance);
  self->fill_tol_label = gtk_label_new (val_str);
  gtk_widget_add_css_class (self->fill_tol_label, "paint-color-alpha-label");
  gtk_label_set_xalign (GTK_LABEL (self->fill_tol_label), 1.0f);
  gtk_box_append (GTK_BOX (bar), self->fill_tol_label);

  gtk_box_append (GTK_BOX (bar), gtk_separator_new (GTK_ORIENTATION_VERTICAL));

  self->fill_sample_all_btn = gtk_toggle_button_new_with_label (_("Sample All Layers"));
  gtk_widget_add_css_class (self->fill_sample_all_btn, "flat");
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->fill_sample_all_btn),
                                self->settings->fill_sample_all_layers);
  g_signal_connect (self->fill_sample_all_btn, "toggled",
                    G_CALLBACK (on_fill_sample_all_toggled), self);
  gtk_box_append (GTK_BOX (bar), self->fill_sample_all_btn);
  return bar;
}

static GtkWidget *
build_gradient_bar (PaintCanvasPage *self)
{
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class (bar, "paint-osd-pill");
  gtk_widget_add_css_class (bar, "paint-sub-pill");
  gtk_widget_add_css_class (bar, "paint-shape-mode");
  gtk_widget_set_halign (bar, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (bar, GTK_ALIGN_END);
  gtk_widget_set_margin_bottom (bar, 68);
  gtk_widget_set_visible (bar, FALSE);

  self->gradient_linear_btn = gtk_toggle_button_new ();
  GtkWidget *lbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_box_append (GTK_BOX (lbox), gtk_image_new_from_icon_name ("tool-gradient-linear-symbolic"));
  gtk_box_append (GTK_BOX (lbox), gtk_label_new (_("Linear")));
  gtk_button_set_child (GTK_BUTTON (self->gradient_linear_btn), lbox);
  gtk_widget_add_css_class (self->gradient_linear_btn, "flat");

  self->gradient_radial_btn = gtk_toggle_button_new ();
  GtkWidget *rbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_box_append (GTK_BOX (rbox), gtk_image_new_from_icon_name ("tool-gradient-radial-symbolic"));
  gtk_box_append (GTK_BOX (rbox), gtk_label_new (_("Radial")));
  gtk_button_set_child (GTK_BUTTON (self->gradient_radial_btn), rbox);
  gtk_widget_add_css_class (self->gradient_radial_btn, "flat");

  gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (self->gradient_radial_btn),
                               GTK_TOGGLE_BUTTON (self->gradient_linear_btn));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->gradient_linear_btn),
                                !self->settings->gradient_radial);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->gradient_radial_btn),
                                self->settings->gradient_radial);
  g_signal_connect (self->gradient_linear_btn, "toggled",
                    G_CALLBACK (on_gradient_mode_toggled), self);
  g_signal_connect (self->gradient_radial_btn, "toggled",
                    G_CALLBACK (on_gradient_mode_toggled), self);

  gtk_box_append (GTK_BOX (bar), self->gradient_linear_btn);
  gtk_box_append (GTK_BOX (bar), self->gradient_radial_btn);
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

  GtkWidget *tscroll = gtk_scrolled_window_new ();
  gtk_widget_add_css_class (tscroll, "paint-text-view-scroll");
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (tscroll),
                                  GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_width (GTK_SCROLLED_WINDOW (tscroll), 170);
  gtk_scrolled_window_set_min_content_height (GTK_SCROLLED_WINDOW (tscroll), 28);
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (tscroll), 64);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (tscroll), TRUE);

  self->text_view = gtk_text_view_new ();
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (self->text_view), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_accepts_tab (GTK_TEXT_VIEW (self->text_view), FALSE);
  self->text_buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->text_view));
  g_signal_connect (self->text_buffer, "changed", G_CALLBACK (on_text_buffer_changed), self);

  GtkEventController *tkey = gtk_event_controller_key_new ();
  g_signal_connect (tkey, "key-pressed", G_CALLBACK (on_text_view_key_pressed), self);
  gtk_widget_add_controller (self->text_view, tkey);

  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (tscroll), self->text_view);
  gtk_box_append (GTK_BOX (bar), tscroll);

  GtkFontDialog *fd = gtk_font_dialog_new ();
  self->text_font_btn = gtk_font_dialog_button_new (fd);
  gtk_font_dialog_button_set_level (GTK_FONT_DIALOG_BUTTON (self->text_font_btn),
                                    GTK_FONT_LEVEL_FAMILY);
  gtk_font_dialog_button_set_use_font (GTK_FONT_DIALOG_BUTTON (self->text_font_btn), FALSE);
  PangoFontDescription *init_desc = pango_font_description_new ();
  pango_font_description_set_family (init_desc,
                                     (self->settings->text_font_family && *self->settings->text_font_family)
                                       ? self->settings->text_font_family
                                       : "Sans");
  gtk_font_dialog_button_set_font_desc (GTK_FONT_DIALOG_BUTTON (self->text_font_btn), init_desc);
  pango_font_description_free (init_desc);
  gtk_widget_set_tooltip_text (self->text_font_btn, _("Font Family"));
  g_signal_connect (self->text_font_btn, "notify::font-desc",
                    G_CALLBACK (on_text_font_desc_changed), self);
  gtk_box_append (GTK_BOX (bar), self->text_font_btn);

  self->text_size_spin = gtk_spin_button_new_with_range (8.0, 256.0, 2.0);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->text_size_spin), self->settings->text_font_size);
  gtk_widget_set_tooltip_text (self->text_size_spin, _("Font Size (px)"));
  g_signal_connect (self->text_size_spin, "value-changed", G_CALLBACK (on_text_size_changed), self);
  gtk_box_append (GTK_BOX (bar), self->text_size_spin);

  self->text_bold_btn = gtk_toggle_button_new ();
  GtkWidget *b_lbl = gtk_label_new (NULL);
  gtk_label_set_markup (GTK_LABEL (b_lbl), "<b>B</b>");
  gtk_button_set_child (GTK_BUTTON (self->text_bold_btn), b_lbl);
  gtk_widget_add_css_class (self->text_bold_btn, "flat");
  gtk_widget_set_tooltip_text (self->text_bold_btn, _("Bold"));
  g_signal_connect (self->text_bold_btn, "toggled", G_CALLBACK (on_text_bold_toggled), self);
  gtk_box_append (GTK_BOX (bar), self->text_bold_btn);

  self->text_italic_btn = gtk_toggle_button_new ();
  GtkWidget *i_lbl = gtk_label_new (NULL);
  gtk_label_set_markup (GTK_LABEL (i_lbl), "<i>I</i>");
  gtk_button_set_child (GTK_BUTTON (self->text_italic_btn), i_lbl);
  gtk_widget_add_css_class (self->text_italic_btn, "flat");
  gtk_widget_set_tooltip_text (self->text_italic_btn, _("Italic"));
  g_signal_connect (self->text_italic_btn, "toggled", G_CALLBACK (on_text_italic_toggled), self);
  gtk_box_append (GTK_BOX (bar), self->text_italic_btn);

  static const char *const ALIGN_ICONS[3] = {
    "format-justify-left-symbolic",
    "format-justify-center-symbolic",
    "format-justify-right-symbolic",
  };
  GtkToggleButton *first_align = NULL;
  for (int i = 0; i < 3; i++) {
    GtkWidget *abtn = gtk_toggle_button_new ();
    gtk_button_set_icon_name (GTK_BUTTON (abtn), ALIGN_ICONS[i]);
    gtk_widget_add_css_class (abtn, "flat");
    if (!first_align)
      first_align = GTK_TOGGLE_BUTTON (abtn);
    else
      gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (abtn), first_align);
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (abtn),
                                  (int) self->settings->text_align == i);
    g_object_set_data (G_OBJECT (abtn), "paint-text-align", GINT_TO_POINTER (i));
    g_signal_connect (abtn, "toggled", G_CALLBACK (on_text_align_toggled), self);
    gtk_box_append (GTK_BOX (bar), abtn);
    self->text_align_btns[i] = abtn;
  }

  self->text_outline_btn = gtk_toggle_button_new_with_label (_("Outline"));
  gtk_widget_add_css_class (self->text_outline_btn, "flat");
  gtk_widget_set_tooltip_text (self->text_outline_btn, _("Stroke Text with Secondary Color"));
  g_signal_connect (self->text_outline_btn, "toggled", G_CALLBACK (on_text_outline_toggled), self);
  gtk_box_append (GTK_BOX (bar), self->text_outline_btn);

  self->text_bg_pill_btn = gtk_toggle_button_new_with_label (_("Fill"));
  gtk_widget_add_css_class (self->text_bg_pill_btn, "flat");
  gtk_widget_set_tooltip_text (self->text_bg_pill_btn, _("Background Pill (Secondary Color)"));
  g_signal_connect (self->text_bg_pill_btn, "toggled", G_CALLBACK (on_text_bg_pill_toggled), self);
  gtk_box_append (GTK_BOX (bar), self->text_bg_pill_btn);

  GtkWidget *apply_btn = gtk_button_new_from_icon_name ("object-select-symbolic");
  gtk_widget_add_css_class (apply_btn, "suggested-action");
  gtk_widget_set_tooltip_text (apply_btn, _("Apply Text (Ctrl+Enter)"));
  g_signal_connect_swapped (apply_btn, "clicked", G_CALLBACK (paint_canvas_commit_text), self->canvas);
  gtk_box_append (GTK_BOX (bar), apply_btn);

  GtkWidget *cancel_btn = gtk_button_new_from_icon_name ("window-close-symbolic");
  gtk_widget_add_css_class (cancel_btn, "flat");
  gtk_widget_set_tooltip_text (cancel_btn, _("Cancel Text (Esc)"));
  g_signal_connect_swapped (cancel_btn, "clicked", G_CALLBACK (paint_canvas_cancel_text), self->canvas);
  gtk_box_append (GTK_BOX (bar), cancel_btn);

  return bar;
}

static void
on_canvas_hidden_layer_blocked (PaintCanvas *canvas G_GNUC_UNUSED, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (self->toast_overlay) {
    AdwToast *toast = adw_toast_new (_("Active layer is hidden"));
    adw_toast_set_timeout (toast, 2);
    adw_toast_overlay_add_toast (self->toast_overlay, toast);
  }
}

static void
on_canvas_locked_layer_blocked (PaintCanvas *canvas G_GNUC_UNUSED, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (self->toast_overlay) {
    AdwToast *toast = adw_toast_new (_("Active layer is locked"));
    adw_toast_set_timeout (toast, 2);
    adw_toast_overlay_add_toast (self->toast_overlay, toast);
  }
}

typedef enum {
  RESIZE_AXIS_EAST = 1,
  RESIZE_AXIS_SOUTH = 2,
  RESIZE_AXIS_SE = 3,
} ResizeHandleAxis;

static void
on_resize_handle_begin (GtkGestureDrag *gesture,
                        double start_x G_GNUC_UNUSED,
                        double start_y G_GNUC_UNUSED,
                        gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  if (!doc)
    return;
  self->edge_resizing = TRUE;
  self->edge_drag_orig_w = paint_document_get_width (doc);
  self->edge_drag_orig_h = paint_document_get_height (doc);
  self->edge_preview_w = self->edge_drag_orig_w;
  self->edge_preview_h = self->edge_drag_orig_h;
  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  update_coords_hud (self);
}

static void
on_resize_handle_update (GtkGestureDrag *gesture,
                         double offset_x,
                         double offset_y,
                         gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (!self->edge_resizing)
    return;
  ResizeHandleAxis axis =
      (ResizeHandleAxis) GPOINTER_TO_INT (g_object_get_data (G_OBJECT (gesture), "resize-axis"));
  double zoom = MAX (0.01, paint_canvas_get_zoom (self->canvas));
  int dx = (axis & RESIZE_AXIS_EAST) ? (int) lround (offset_x / zoom) : 0;
  int dy = (axis & RESIZE_AXIS_SOUTH) ? (int) lround (offset_y / zoom) : 0;
  self->edge_preview_w = CLAMP (self->edge_drag_orig_w + dx, 1, 8192);
  self->edge_preview_h = CLAMP (self->edge_drag_orig_h + dy, 1, 8192);
  update_coords_hud (self);
}

static void
on_resize_handle_end (GtkGestureDrag *gesture G_GNUC_UNUSED,
                      double offset_x G_GNUC_UNUSED,
                      double offset_y G_GNUC_UNUSED,
                      gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (!self->edge_resizing)
    return;
  self->edge_resizing = FALSE;
  if (self->edge_preview_w != self->edge_drag_orig_w ||
      self->edge_preview_h != self->edge_drag_orig_h) {
    paint_canvas_resize (self->canvas, self->edge_preview_w, self->edge_preview_h);
  } else {
    update_coords_hud (self);
  }
}

static void
on_resize_handle_cancel (GtkGesture *gesture G_GNUC_UNUSED,
                         GdkEventSequence *sequence G_GNUC_UNUSED,
                         gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (!self->edge_resizing)
    return;
  self->edge_resizing = FALSE;
  update_coords_hud (self);
}

static GtkWidget *
make_resize_handle (PaintCanvasPage *self,
                    ResizeHandleAxis axis,
                    int width,
                    int height,
                    GtkAlign halign,
                    GtkAlign valign,
                    const char *cursor_name,
                    const char *tooltip)
{
  GtkWidget *h = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (h, "paint-canvas-resize-handle");
  gtk_widget_set_size_request (h, width, height);
  gtk_widget_set_halign (h, halign);
  gtk_widget_set_valign (h, valign);
  gtk_widget_set_cursor_from_name (h, cursor_name);
  gtk_widget_set_tooltip_text (h, tooltip);

  GtkGesture *drag = gtk_gesture_drag_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (drag), GDK_BUTTON_PRIMARY);
  g_object_set_data (G_OBJECT (drag), "resize-axis", GINT_TO_POINTER (axis));
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_resize_handle_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_resize_handle_update), self);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_resize_handle_end), self);
  g_signal_connect (drag, "cancel", G_CALLBACK (on_resize_handle_cancel), self);
  gtk_widget_add_controller (h, GTK_EVENT_CONTROLLER (drag));
  return h;
}

static GtkWidget *
build_canvas_context_menu (PaintCanvasPage *self)
{
  g_autoptr (GMenu) menu = g_menu_new ();

  g_autoptr (GMenu) clip_sec = g_menu_new ();
  g_menu_append (clip_sec, _("Cut"), "win.cut");
  g_menu_append (clip_sec, _("Copy"), "win.copy");
  g_menu_append (clip_sec, _("Copy Merged"), "win.copy-merged");
  g_menu_append (clip_sec, _("Paste"), "win.paste");
  g_menu_append (clip_sec, _("Paste as New Layer"), "win.paste-as-layer");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (clip_sec));

  g_autoptr (GMenu) sel_sec = g_menu_new ();
  g_menu_append (sel_sec, _("Select All"), "win.select-all");
  g_menu_append (sel_sec, _("Invert Selection"), "win.invert-selection");
  g_menu_append (sel_sec, _("Crop to Selection"), "win.crop-selection");
  g_menu_append (sel_sec, _("Delete Selection"), "win.delete-selection");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (sel_sec));

  g_autoptr (GMenu) canvas_sec = g_menu_new ();
  g_menu_append (canvas_sec, _("Fit to Window"), "win.zoom-fit");
  g_menu_append (canvas_sec, _("Reset Zoom"), "win.zoom-reset");
  g_menu_append (canvas_sec, _("Resize & Scale…"), "win.resize-canvas");
  g_menu_append (canvas_sec, _("Clear Canvas"), "win.clear");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (canvas_sec));

  GtkWidget *pop = gtk_popover_menu_new_from_model (G_MENU_MODEL (menu));
  gtk_widget_set_parent (pop, GTK_WIDGET (self->canvas));
  gtk_popover_set_has_arrow (GTK_POPOVER (pop), FALSE);
  return pop;
}

static void
on_canvas_secondary_click (GtkGestureClick *gesture G_GNUC_UNUSED,
                           int n_press G_GNUC_UNUSED,
                           double x,
                           double y,
                           gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (!self->context_menu)
    return;
  GdkRectangle rect = { (int) lround (x), (int) lround (y), 1, 1 };
  gtk_popover_set_pointing_to (GTK_POPOVER (self->context_menu), &rect);
  gtk_popover_popup (GTK_POPOVER (self->context_menu));
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

  self->layers_panel = paint_layers_panel_new (self->canvas);
  self->overlay = GTK_OVERLAY (gtk_overlay_new ());
  adw_overlay_split_view_set_sidebar (self->split_view, GTK_WIDGET (self->layers_panel));
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

  GtkWidget *resize_grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (resize_grid), 4);
  gtk_grid_set_column_spacing (GTK_GRID (resize_grid), 4);
  gtk_grid_attach (GTK_GRID (resize_grid), GTK_WIDGET (self->canvas), 0, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (resize_grid),
                   make_resize_handle (self, RESIZE_AXIS_EAST, 10, 36,
                                       GTK_ALIGN_START, GTK_ALIGN_CENTER,
                                       "ew-resize", _("Drag to Resize Canvas Width")),
                   1, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (resize_grid),
                   make_resize_handle (self, RESIZE_AXIS_SOUTH, 36, 10,
                                       GTK_ALIGN_CENTER, GTK_ALIGN_START,
                                       "ns-resize", _("Drag to Resize Canvas Height")),
                   0, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (resize_grid),
                   make_resize_handle (self, RESIZE_AXIS_SE, 10, 10,
                                       GTK_ALIGN_START, GTK_ALIGN_START,
                                       "nwse-resize", _("Drag to Resize Canvas Width & Height")),
                   1, 1, 1, 1);
  gtk_box_append (GTK_BOX (container), resize_grid);

  gtk_scrolled_window_set_child (self->scrolled, container);
  GtkWidget *viewport = gtk_scrolled_window_get_child (self->scrolled);
  if (GTK_IS_VIEWPORT (viewport))
    gtk_viewport_set_scroll_to_focus (GTK_VIEWPORT (viewport), FALSE);
  gtk_overlay_set_child (self->overlay, GTK_WIDGET (self->scrolled));

  GtkGesture *space_pan = gtk_gesture_drag_new ();
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (space_pan), GTK_PHASE_CAPTURE);
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (space_pan), GDK_BUTTON_PRIMARY);
  g_signal_connect (space_pan, "drag-begin", G_CALLBACK (on_space_pan_begin), self);
  g_signal_connect (space_pan, "drag-update", G_CALLBACK (on_space_pan_update), self);
  g_signal_connect (space_pan, "drag-end", G_CALLBACK (on_space_pan_end), self);
  g_signal_connect (space_pan, "cancel", G_CALLBACK (on_space_pan_cancel), self);
  gtk_widget_add_controller (GTK_WIDGET (self->scrolled), GTK_EVENT_CONTROLLER (space_pan));

  self->context_menu = build_canvas_context_menu (self);
  GtkGesture *rclick = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (rclick), GDK_BUTTON_SECONDARY);
  g_signal_connect (rclick, "pressed", G_CALLBACK (on_canvas_secondary_click), self);
  gtk_widget_add_controller (GTK_WIDGET (self->canvas), GTK_EVENT_CONTROLLER (rclick));

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
  gtk_widget_set_tooltip_text (self->undo_btn, _("Undo (Ctrl+Z)"));
  g_signal_connect_swapped (self->undo_btn, "clicked", G_CALLBACK (paint_canvas_undo), self->canvas);

  self->redo_btn = gtk_button_new_from_icon_name ("edit-redo-symbolic");
  gtk_widget_add_css_class (self->redo_btn, "flat");
  gtk_widget_set_tooltip_text (self->redo_btn, _("Redo (Ctrl+Shift+Z)"));
  g_signal_connect_swapped (self->redo_btn, "clicked", G_CALLBACK (paint_canvas_redo), self->canvas);

  gtk_box_append (GTK_BOX (undo_box), self->undo_btn);
  gtk_box_append (GTK_BOX (undo_box), self->redo_btn);
  gtk_box_append (GTK_BOX (pill), undo_box);

  gtk_box_append (GTK_BOX (pill), gtk_separator_new (GTK_ORIENTATION_VERTICAL));

  GtkWidget *tools_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 1);

  GtkWidget *sel_content = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
  self->select_btn_icon = gtk_image_new_from_icon_name ("tool-select-symbolic");
  GtkWidget *sel_arrow = gtk_image_new_from_icon_name ("pan-down-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (sel_arrow), 12);
  gtk_widget_set_opacity (sel_arrow, 0.75);
  gtk_box_append (GTK_BOX (sel_content), self->select_btn_icon);
  gtk_box_append (GTK_BOX (sel_content), sel_arrow);

  self->select_btn = gtk_menu_button_new ();
  gtk_widget_add_css_class (self->select_btn, "flat");
  gtk_widget_add_css_class (self->select_btn, "paint-select-button");
  gtk_menu_button_set_child (GTK_MENU_BUTTON (self->select_btn), sel_content);
  gtk_widget_set_tooltip_text (self->select_btn, _("Selection Tools (M)"));
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (self->select_btn),
                                GTK_WIDGET (build_select_popover (self)));
  gtk_box_append (GTK_BOX (tools_box), self->select_btn);

  gsize n_tools = 0;
  const PaintTool *tools = paint_tools_get_all (&n_tools);
  GtkToggleButton *first_btn = NULL;
  for (gsize i = 0; i < n_tools; i++) {
    if (tools[i].is_shape || tools[i].is_transparency ||
        tools[i].is_select || tools[i].is_retouch) {
      continue;
    }
    GtkWidget *btn = gtk_toggle_button_new ();
    gtk_button_set_icon_name (GTK_BUTTON (btn), tools[i].icon_name);
    gtk_widget_add_css_class (btn, "flat");
    gtk_widget_set_tooltip_text (btn, _(tools[i].tooltip));
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

  GtkWidget *ret_content = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
  self->retouch_btn_icon = gtk_image_new_from_icon_name ("tool-blur-symbolic");
  GtkWidget *ret_arrow = gtk_image_new_from_icon_name ("pan-down-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (ret_arrow), 12);
  gtk_widget_set_opacity (ret_arrow, 0.75);
  gtk_box_append (GTK_BOX (ret_content), self->retouch_btn_icon);
  gtk_box_append (GTK_BOX (ret_content), ret_arrow);

  self->retouch_btn = gtk_menu_button_new ();
  gtk_widget_add_css_class (self->retouch_btn, "flat");
  gtk_widget_add_css_class (self->retouch_btn, "paint-retouch-button");
  gtk_menu_button_set_child (GTK_MENU_BUTTON (self->retouch_btn), ret_content);
  gtk_widget_set_tooltip_text (self->retouch_btn, _("Retouch Brushes (Blur / Pixelate)"));
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (self->retouch_btn),
                                GTK_WIDGET (build_retouch_popover (self)));
  gtk_box_append (GTK_BOX (tools_box), self->retouch_btn);

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
  gtk_widget_set_tooltip_text (self->transparent_btn, _("Transparency (T)"));
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
  gtk_widget_set_tooltip_text (self->shapes_btn, _("Shapes (S)"));
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
  gtk_widget_set_tooltip_text (self->size_btn, _("Brush Size ([ / ])"));
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (self->size_btn), GTK_WIDGET (build_size_popover (self)));
  gtk_box_append (GTK_BOX (pill), self->size_btn);

  self->color_picker = paint_color_picker_new (self->settings,
                                               GTK_WIDGET (self->canvas),
                                               on_picker_color_changed,
                                               self);
  paint_color_picker_set_palette_callback (self->color_picker, on_picker_palette_changed, self);
  gtk_box_append (GTK_BOX (pill), paint_color_picker_get_button (self->color_picker));

  self->osd_pill = pill;
  gtk_overlay_add_overlay (self->overlay, pill);

  self->selection_bar = build_selection_bar (self);
  gtk_overlay_add_overlay (self->overlay, self->selection_bar);

  self->fill_options_bar = build_fill_options_bar (self);
  gtk_overlay_add_overlay (self->overlay, self->fill_options_bar);

  self->gradient_bar = build_gradient_bar (self);
  gtk_overlay_add_overlay (self->overlay, self->gradient_bar);

  self->text_bar = build_text_bar (self);
  gtk_overlay_add_overlay (self->overlay, self->text_bar);

  self->coords_hud = gtk_label_new ("");
  gtk_widget_add_css_class (self->coords_hud, "paint-coords-hud");
  gtk_widget_set_halign (self->coords_hud, GTK_ALIGN_START);
  gtk_widget_set_valign (self->coords_hud, GTK_ALIGN_END);
  gtk_widget_set_margin_start (self->coords_hud, 16);
  gtk_widget_set_margin_bottom (self->coords_hud, 16);
  gtk_widget_set_can_target (self->coords_hud, FALSE);
  gtk_overlay_add_overlay (self->overlay, self->coords_hud);

  g_signal_connect (self->canvas, "changed", G_CALLBACK (on_canvas_changed), self);
  g_signal_connect (self->canvas, "zoom-changed", G_CALLBACK (on_canvas_zoom_changed), self);
  g_signal_connect (self->canvas, "color-picked", G_CALLBACK (on_canvas_color_picked), self);
  g_signal_connect (self->canvas, "selection-changed", G_CALLBACK (on_canvas_selection_changed), self);
  g_signal_connect (self->canvas, "text-state-changed", G_CALLBACK (on_canvas_text_state_changed), self);
  g_signal_connect (self->canvas, "hidden-layer-blocked", G_CALLBACK (on_canvas_hidden_layer_blocked), self);
  g_signal_connect (self->canvas, "locked-layer-blocked", G_CALLBACK (on_canvas_locked_layer_blocked), self);
  g_signal_connect (self->canvas, "cursor-moved", G_CALLBACK (on_canvas_cursor_moved), self);
  update_coords_hud (self);
}

static void
paint_canvas_page_dispose (GObject *object)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (object);
  g_clear_pointer (&self->context_menu, gtk_widget_unparent);
  G_OBJECT_CLASS (paint_canvas_page_parent_class)->dispose (object);
}

static void
paint_canvas_page_finalize (GObject *object)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (object);
  g_clear_pointer (&self->color_picker, paint_color_picker_free);
  g_clear_pointer (&self->tool_buttons, g_hash_table_unref);
  G_OBJECT_CLASS (paint_canvas_page_parent_class)->finalize (object);
}

static void
paint_canvas_page_class_init (PaintCanvasPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = paint_canvas_page_dispose;
  object_class->finalize = paint_canvas_page_finalize;

  signals[SIGNAL_STATE_CHANGED] = g_signal_new ("state-changed",
                                                G_TYPE_FROM_CLASS (klass),
                                                G_SIGNAL_RUN_FIRST,
                                                0, NULL, NULL, NULL,
                                                G_TYPE_NONE, 0);
  signals[SIGNAL_PALETTE_CHANGED] = g_signal_new ("palette-changed",
                                                  G_TYPE_FROM_CLASS (klass),
                                                  G_SIGNAL_RUN_FIRST,
                                                  0, NULL, NULL, NULL,
                                                  G_TYPE_NONE, 0);
}

static void
paint_canvas_page_init (PaintCanvasPage *self)
{
  self->tool_buttons = g_hash_table_new (g_str_hash, g_str_equal);
  self->last_zoom = 1.0;
  self->last_select_tool_id = "select";
  self->last_retouch_tool_id = "blur";
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
paint_canvas_page_select_last_select (PaintCanvasPage *self)
{
  const char *sel_id = self->last_select_tool_id ? self->last_select_tool_id : "select";
  paint_canvas_page_select_tool (self, sel_id);
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
paint_canvas_page_sync_color_picker (PaintCanvasPage *self)
{
  if (!self || !self->color_picker)
    return;
  paint_color_picker_sync (self->color_picker);
  paint_color_picker_sync_recent (self->color_picker);
  paint_color_picker_sync_custom (self->color_picker);
  gtk_widget_queue_draw (GTK_WIDGET (self->canvas));
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

  double avail_w = MAX (32.0, view_w - (double) (WORKSPACE_MARGIN_START + WORKSPACE_MARGIN_END + 10));
  double avail_h = MAX (32.0, view_h - (double) (WORKSPACE_MARGIN_TOP + WORKSPACE_MARGIN_BOTTOM + 10));
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
paint_canvas_page_set_space_pan (PaintCanvasPage *self, gboolean active)
{
  if (!self || self->space_pan_active == active)
    return;
  self->space_pan_active = active;
  if (!active)
    self->space_pan_dragging = FALSE;
  if (self->canvas)
    paint_canvas_set_pan_override (self->canvas, active, self->space_pan_dragging);
  if (self->scrolled) {
    gtk_widget_set_cursor_from_name (GTK_WIDGET (self->scrolled),
                                     active ? (self->space_pan_dragging ? "grabbing" : "grab") : NULL);
  }
}

void
paint_canvas_page_set_layers_visible (PaintCanvasPage *self, gboolean visible)
{
  if (!self || !self->split_view)
    return;
  adw_overlay_split_view_set_show_sidebar (self->split_view, visible);
  if (visible)
    paint_layers_panel_sync (self->layers_panel, TRUE);
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
  for (int i = 0; i < 3; i++) {
    if (self->text_align_btns[i]) {
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->text_align_btns[i]),
                                    (int) self->settings->text_align == i);
    }
  }
  if (self->text_outline_btn)
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->text_outline_btn), self->settings->text_outline);
  if (self->text_bg_pill_btn)
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->text_bg_pill_btn), self->settings->text_bg_pill);
  if (self->text_font_btn) {
    const char *fam = (self->settings->text_font_family && *self->settings->text_font_family)
                        ? self->settings->text_font_family
                        : "Sans";
    PangoFontDescription *cur = gtk_font_dialog_button_get_font_desc (
        GTK_FONT_DIALOG_BUTTON (self->text_font_btn));
    const char *cur_fam = cur ? pango_font_description_get_family (cur) : NULL;
    if (g_strcmp0 (cur_fam, fam) != 0) {
      PangoFontDescription *nd = pango_font_description_new ();
      pango_font_description_set_family (nd, fam);
      gtk_font_dialog_button_set_font_desc (GTK_FONT_DIALOG_BUTTON (self->text_font_btn), nd);
      pango_font_description_free (nd);
    }
  }
  if (self->fill_tol_scale)
    gtk_range_set_value (GTK_RANGE (self->fill_tol_scale), (double) self->settings->fill_tolerance);
  if (self->fill_tol_label) {
    g_autofree char *tlbl = g_strdup_printf ("%d", self->settings->fill_tolerance);
    gtk_label_set_text (GTK_LABEL (self->fill_tol_label), tlbl);
  }
  if (self->fill_sample_all_btn)
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->fill_sample_all_btn),
                                  self->settings->fill_sample_all_layers);
  if (self->gradient_linear_btn && self->gradient_radial_btn) {
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->gradient_linear_btn),
                                  !self->settings->gradient_radial);
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->gradient_radial_btn),
                                  self->settings->gradient_radial);
  }
  self->syncing = FALSE;
  paint_color_picker_sync (self->color_picker);
  paint_color_picker_sync_recent (self->color_picker);
  paint_color_picker_sync_custom (self->color_picker);
  sync_overlay_bars (self);
  paint_layers_panel_sync (self->layers_panel, paint_canvas_page_get_layers_visible (self));
}

char *
paint_canvas_page_dup_title (PaintCanvasPage *self)
{
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  GFile *file = paint_document_get_file (doc);
  g_autofree char *base = file ? g_file_get_basename (file) : g_strdup (_("Untitled Canvas"));
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
