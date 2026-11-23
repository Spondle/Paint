#include "paint-layers-panel.h"

#include <math.h>

#include "paint-color-picker.h"
#include "paint-i18n.h"

static const char *const LAYER_BLEND_LABELS[] = {
  "Normal", "Multiply", "Screen", "Overlay", "Darken", "Lighten", NULL
};

struct _PaintLayersPanel {
  AdwBin parent_instance;

  PaintCanvas *canvas; /* unowned */
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
  GtkWidget *layers_alpha_lock_btn;
  GtkWidget *layers_lock_btn;
  PaintDocument *last_layers_doc;
  guint64 last_layers_revision;
  GPtrArray *layer_thumbs; /* unowned GtkWidget* thumbnails */
  int solo_layer_idx;
  GArray *solo_saved_vis; /* element-type: gboolean */
  gboolean syncing;
};

G_DEFINE_FINAL_TYPE (PaintLayersPanel, paint_layers_panel, ADW_TYPE_BIN)

static void
draw_layer_thumb (GtkDrawingArea *area,
                  cairo_t *cr,
                  int width,
                  int height,
                  gpointer user_data)
{
  PaintLayersPanel *self = PAINT_LAYERS_PANEL (user_data);
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
  paint_draw_checkerboard (cr, dx, dy, dw, dh, 4.0);
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
  PaintLayersPanel *panel;
  int layer_index;
  GtkWidget *popover;
  GtkWidget *entry;
} LayerRenameCtx;

static void
on_layer_popover_closed (GtkPopover *pop, gpointer user_data G_GNUC_UNUSED)
{
  if (gtk_widget_get_parent (GTK_WIDGET (pop)))
    gtk_widget_unparent (GTK_WIDGET (pop));
}

static void
on_layer_rename_apply (GtkWidget *widget G_GNUC_UNUSED, gpointer user_data)
{
  LayerRenameCtx *ctx = user_data;
  PaintLayersPanel *panel = ctx->panel;
  int idx = ctx->layer_index;
  GtkWidget *pop = ctx->popover;
  g_autofree char *name = g_strdup (gtk_editable_get_text (GTK_EDITABLE (ctx->entry)));
  gtk_popover_popdown (GTK_POPOVER (pop));
  paint_canvas_set_layer_name (panel->canvas, idx, name);
}

static void
open_layer_rename_popover (PaintLayersPanel *self, GtkWidget *row, int idx)
{
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  if (!doc || !row || !self->layers_list)
    return;

  GtkWidget *pop = gtk_popover_new ();
  gtk_widget_set_parent (pop, self->layers_list);
  graphene_rect_t bounds;
  if (gtk_widget_compute_bounds (row, self->layers_list, &bounds)) {
    GdkRectangle rect = {
      (int) lroundf (bounds.origin.x),
      (int) lroundf (bounds.origin.y),
      MAX (1, (int) lroundf (bounds.size.width)),
      MAX (1, (int) lroundf (bounds.size.height))
    };
    gtk_popover_set_pointing_to (GTK_POPOVER (pop), &rect);
  }

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_top (box, 8);
  gtk_widget_set_margin_bottom (box, 8);
  gtk_widget_set_margin_start (box, 8);
  gtk_widget_set_margin_end (box, 8);

  GtkWidget *entry = gtk_entry_new ();
  gtk_editable_set_text (GTK_EDITABLE (entry), paint_document_layer_get_name (doc, idx));
  gtk_editable_set_width_chars (GTK_EDITABLE (entry), 16);
  gtk_box_append (GTK_BOX (box), entry);

  GtkWidget *ok_btn = gtk_button_new_with_label (_("Rename"));
  gtk_widget_add_css_class (ok_btn, "suggested-action");
  gtk_box_append (GTK_BOX (box), ok_btn);

  LayerRenameCtx *ctx = g_new0 (LayerRenameCtx, 1);
  ctx->panel = self;
  ctx->layer_index = idx;
  ctx->popover = pop;
  ctx->entry = entry;
  g_object_set_data_full (G_OBJECT (pop), "rename-ctx", ctx, g_free);

  g_signal_connect (entry, "activate", G_CALLBACK (on_layer_rename_apply), ctx);
  g_signal_connect (ok_btn, "clicked", G_CALLBACK (on_layer_rename_apply), ctx);
  g_signal_connect (pop, "closed", G_CALLBACK (on_layer_popover_closed), NULL);

  gtk_popover_set_child (GTK_POPOVER (pop), box);
  gtk_popover_popup (GTK_POPOVER (pop));
  gtk_widget_grab_focus (entry);
  gtk_editable_select_region (GTK_EDITABLE (entry), 0, -1);
}

static void
toggle_solo_layer (PaintLayersPanel *self, int idx)
{
  PaintDocument *doc = self->canvas ? paint_canvas_get_document (self->canvas) : NULL;
  if (!doc)
    return;
  int n = paint_document_get_n_layers (doc);
  if (idx < 0 || idx >= n)
    return;

  if (self->solo_layer_idx == idx &&
      self->solo_saved_vis &&
      (int) self->solo_saved_vis->len == n) {
    const gboolean *saved = (const gboolean *) (void *) self->solo_saved_vis->data;
    self->solo_layer_idx = -1;
    paint_canvas_set_layers_visibility (self->canvas, saved, n);
    g_array_set_size (self->solo_saved_vis, 0);
    return;
  }

  /* Preserve original pre-solo visibility if switching solo from one layer to another */
  if (self->solo_layer_idx < 0 ||
      !self->solo_saved_vis ||
      (int) self->solo_saved_vis->len != n) {
    g_array_set_size (self->solo_saved_vis, 0);
    for (int i = 0; i < n; i++) {
      gboolean v = paint_document_layer_get_visible (doc, i);
      g_array_append_val (self->solo_saved_vis, v);
    }
  }

  g_autofree gboolean *vis = g_new0 (gboolean, (gsize) n);
  vis[idx] = TRUE;
  self->solo_layer_idx = idx;
  paint_canvas_set_layers_visibility (self->canvas, vis, n);
}

typedef struct {
  PaintLayersPanel *panel;
  GtkWidget *row;
  GtkWidget *popover;
  int layer_index;
} LayerContextCtx;

static GtkWidget *
add_ctx_button (GtkWidget *box, const char *label, gboolean sensitive)
{
  GtkWidget *btn = gtk_button_new_with_label (label);
  gtk_widget_add_css_class (btn, "flat");
  GtkWidget *child = gtk_button_get_child (GTK_BUTTON (btn));
  if (GTK_IS_LABEL (child))
    gtk_label_set_xalign (GTK_LABEL (child), 0.0f);
  gtk_widget_set_sensitive (btn, sensitive);
  gtk_box_append (GTK_BOX (box), btn);
  return btn;
}

static void
on_ctx_rename (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  LayerContextCtx *ctx = user_data;
  PaintLayersPanel *self = ctx->panel;
  GtkWidget *row = ctx->row;
  int idx = ctx->layer_index;
  gtk_popover_popdown (GTK_POPOVER (ctx->popover));
  open_layer_rename_popover (self, row, idx);
}

static void
on_ctx_dup (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  LayerContextCtx *ctx = user_data;
  PaintLayersPanel *self = ctx->panel;
  int idx = ctx->layer_index;
  gtk_popover_popdown (GTK_POPOVER (ctx->popover));
  paint_canvas_duplicate_layer (self->canvas, idx);
}

static void
on_ctx_solo (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  LayerContextCtx *ctx = user_data;
  PaintLayersPanel *self = ctx->panel;
  int idx = ctx->layer_index;
  gtk_popover_popdown (GTK_POPOVER (ctx->popover));
  toggle_solo_layer (self, idx);
}

static void
on_ctx_alpha_lock (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  LayerContextCtx *ctx = user_data;
  PaintLayersPanel *self = ctx->panel;
  int idx = ctx->layer_index;
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  gboolean cur = doc ? paint_document_layer_get_alpha_lock (doc, idx) : FALSE;
  gtk_popover_popdown (GTK_POPOVER (ctx->popover));
  paint_canvas_set_layer_alpha_lock (self->canvas, idx, !cur);
}

static void
on_ctx_lock (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  LayerContextCtx *ctx = user_data;
  PaintLayersPanel *self = ctx->panel;
  int idx = ctx->layer_index;
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  gboolean cur = doc ? paint_document_layer_get_locked (doc, idx) : FALSE;
  gtk_popover_popdown (GTK_POPOVER (ctx->popover));
  paint_canvas_set_layer_locked (self->canvas, idx, !cur);
}

static void
on_ctx_merge (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  LayerContextCtx *ctx = user_data;
  PaintLayersPanel *self = ctx->panel;
  int idx = ctx->layer_index;
  gtk_popover_popdown (GTK_POPOVER (ctx->popover));
  paint_canvas_merge_layer_down (self->canvas, idx);
}

static void
on_ctx_delete (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  LayerContextCtx *ctx = user_data;
  PaintLayersPanel *self = ctx->panel;
  int idx = ctx->layer_index;
  gtk_popover_popdown (GTK_POPOVER (ctx->popover));
  paint_canvas_remove_layer (self->canvas, idx);
}

static void
on_layer_row_right_click (GtkGestureClick *gesture,
                          int n_press G_GNUC_UNUSED,
                          double x,
                          double y,
                          gpointer user_data)
{
  PaintLayersPanel *self = PAINT_LAYERS_PANEL (user_data);
  GtkWidget *row = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
  int idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "paint-layer-idx"));
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  if (!doc || !self->layers_list)
    return;

  int n = paint_document_get_n_layers (doc);
  GtkWidget *pop = gtk_popover_new ();
  gtk_popover_set_has_arrow (GTK_POPOVER (pop), FALSE);
  gtk_widget_set_parent (pop, self->layers_list);

  graphene_point_t pt;
  if (gtk_widget_compute_point (row, self->layers_list,
                                &GRAPHENE_POINT_INIT ((float) x, (float) y), &pt)) {
    GdkRectangle rect = { (int) lroundf (pt.x), (int) lroundf (pt.y), 1, 1 };
    gtk_popover_set_pointing_to (GTK_POPOVER (pop), &rect);
  }

  LayerContextCtx *ctx = g_new0 (LayerContextCtx, 1);
  ctx->panel = self;
  ctx->row = row;
  ctx->popover = pop;
  ctx->layer_index = idx;
  g_object_set_data_full (G_OBJECT (pop), "layer-ctx", ctx, g_free);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_margin_top (box, 4);
  gtk_widget_set_margin_bottom (box, 4);
  gtk_widget_set_margin_start (box, 4);
  gtk_widget_set_margin_end (box, 4);

  gboolean is_soloed = (self->solo_layer_idx == idx);
  gboolean alock = paint_document_layer_get_alpha_lock (doc, idx);
  gboolean locked = paint_document_layer_get_locked (doc, idx);

  g_signal_connect (add_ctx_button (box, _("Rename…"), TRUE),
                    "clicked", G_CALLBACK (on_ctx_rename), ctx);
  g_signal_connect (add_ctx_button (box, _("Duplicate"), TRUE),
                    "clicked", G_CALLBACK (on_ctx_dup), ctx);
  g_signal_connect (add_ctx_button (box, is_soloed ? _("Restore Layers Visibility") : _("Solo Layer"), n > 1),
                    "clicked", G_CALLBACK (on_ctx_solo), ctx);
  gtk_box_append (GTK_BOX (box), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
  g_signal_connect (add_ctx_button (box, alock ? _("Unlock Alpha") : _("Lock Alpha"), TRUE),
                    "clicked", G_CALLBACK (on_ctx_alpha_lock), ctx);
  g_signal_connect (add_ctx_button (box, locked ? _("Unlock Layer") : _("Lock Layer"), TRUE),
                    "clicked", G_CALLBACK (on_ctx_lock), ctx);
  gtk_box_append (GTK_BOX (box), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
  g_signal_connect (add_ctx_button (box, _("Merge Down"), idx > 0),
                    "clicked", G_CALLBACK (on_ctx_merge), ctx);
  g_signal_connect (add_ctx_button (box, _("Delete"), n > 1),
                    "clicked", G_CALLBACK (on_ctx_delete), ctx);

  g_signal_connect (pop, "closed", G_CALLBACK (on_layer_popover_closed), NULL);
  gtk_popover_set_child (GTK_POPOVER (pop), box);
  gtk_popover_popup (GTK_POPOVER (pop));
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

  PaintLayersPanel *self = PAINT_LAYERS_PANEL (user_data);
  GtkWidget *row = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
  int idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "paint-layer-idx"));
  open_layer_rename_popover (self, row, idx);
}

static void
on_layer_row_selected (GtkListBox *box G_GNUC_UNUSED,
                       GtkListBoxRow *row,
                       gpointer user_data)
{
  PaintLayersPanel *self = PAINT_LAYERS_PANEL (user_data);
  if (self->syncing || !row)
    return;
  int idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "paint-layer-idx"));
  paint_canvas_set_active_layer (self->canvas, idx);
}

static void
on_layer_vis_alt_pressed (GtkGestureClick *gesture,
                          int n_press G_GNUC_UNUSED,
                          double x G_GNUC_UNUSED,
                          double y G_GNUC_UNUSED,
                          gpointer user_data)
{
  PaintLayersPanel *self = PAINT_LAYERS_PANEL (user_data);
  if (self->syncing)
    return;
  GdkModifierType state = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (gesture));
  if (!(state & GDK_ALT_MASK))
    return;

  GtkWidget *btn = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
  int idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (btn), "paint-layer-idx"));
  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  toggle_solo_layer (self, idx);
}

static void
on_layer_vis_clicked (GtkButton *btn, gpointer user_data)
{
  PaintLayersPanel *self = PAINT_LAYERS_PANEL (user_data);
  if (self->syncing)
    return;
  int idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (btn), "paint-layer-idx"));
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  gboolean vis = paint_document_layer_get_visible (doc, idx);
  self->solo_layer_idx = -1;
  paint_canvas_set_layer_visible (self->canvas, idx, !vis);
}

static void
on_layer_alpha_lock_toggled (GtkToggleButton *btn, gpointer user_data)
{
  PaintLayersPanel *self = PAINT_LAYERS_PANEL (user_data);
  if (self->syncing || !self->canvas)
    return;
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  if (!doc)
    return;
  int active = paint_document_get_active_layer (doc);
  paint_canvas_set_layer_alpha_lock (self->canvas, active, gtk_toggle_button_get_active (btn));
}

static void
on_layer_lock_toggled (GtkToggleButton *btn, gpointer user_data)
{
  PaintLayersPanel *self = PAINT_LAYERS_PANEL (user_data);
  if (self->syncing || !self->canvas)
    return;
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  if (!doc)
    return;
  int active = paint_document_get_active_layer (doc);
  paint_canvas_set_layer_locked (self->canvas, active, gtk_toggle_button_get_active (btn));
}

static GdkContentProvider *
on_layer_drag_prepare (GtkDragSource *source,
                       double x G_GNUC_UNUSED,
                       double y G_GNUC_UNUSED,
                       gpointer user_data G_GNUC_UNUSED)
{
  GtkWidget *row = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (source));
  int idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "paint-layer-idx"));
  GValue val = G_VALUE_INIT;
  g_value_init (&val, G_TYPE_INT);
  g_value_set_int (&val, idx);
  GdkContentProvider *provider = gdk_content_provider_new_for_value (&val);
  g_value_unset (&val);
  return provider;
}

static GdkDragAction
on_layer_row_drop_enter (GtkDropTarget *target,
                         double x G_GNUC_UNUSED,
                         double y G_GNUC_UNUSED,
                         gpointer user_data G_GNUC_UNUSED)
{
  GtkWidget *row = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (target));
  gtk_widget_add_css_class (row, "paint-layer-drop-target");
  return GDK_ACTION_MOVE;
}

static void
on_layer_row_drop_leave (GtkDropTarget *target, gpointer user_data G_GNUC_UNUSED)
{
  GtkWidget *row = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (target));
  gtk_widget_remove_css_class (row, "paint-layer-drop-target");
}

static gboolean
on_layer_row_drop (GtkDropTarget *target,
                   const GValue *value,
                   double x G_GNUC_UNUSED,
                   double y G_GNUC_UNUSED,
                   gpointer user_data)
{
  PaintLayersPanel *self = PAINT_LAYERS_PANEL (user_data);
  GtkWidget *row = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (target));
  gtk_widget_remove_css_class (row, "paint-layer-drop-target");

  if (!G_VALUE_HOLDS_INT (value) || !self->canvas)
    return FALSE;

  int from_idx = g_value_get_int (value);
  int to_idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (row), "paint-layer-idx"));
  if (from_idx == to_idx)
    return FALSE;
  return paint_canvas_move_layer (self->canvas, from_idx, to_idx);
}

static gboolean
on_layers_files_dropped (GtkDropTarget *target G_GNUC_UNUSED,
                         const GValue *value,
                         double x G_GNUC_UNUSED,
                         double y G_GNUC_UNUSED,
                         gpointer user_data)
{
  PaintLayersPanel *self = PAINT_LAYERS_PANEL (user_data);
  if (!G_VALUE_HOLDS (value, GDK_TYPE_FILE_LIST) || !self->canvas)
    return FALSE;

  GdkFileList *fl = g_value_get_boxed (value);
  GSList *files = fl ? gdk_file_list_get_files (fl) : NULL;
  if (!files)
    return FALSE;

  gboolean imported_any = FALSE;
  for (GSList *l = files; l != NULL; l = l->next) {
    GFile *f = G_FILE (l->data);
    if (!G_IS_FILE (f))
      continue;
    g_autoptr (GdkTexture) tex = paint_io_load_file_as_texture (f, NULL);
    if (tex) {
      g_autofree char *name = paint_io_layer_name_from_file (f);
      if (paint_canvas_add_layer_from_texture (self->canvas, name, tex) >= 0)
        imported_any = TRUE;
    }
  }
  g_slist_free (files);
  return imported_any;
}

static void
on_layer_add_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  PaintLayersPanel *self = PAINT_LAYERS_PANEL (user_data);
  paint_canvas_add_layer (self->canvas, NULL);
}

static void
on_layer_dup_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  PaintLayersPanel *self = PAINT_LAYERS_PANEL (user_data);
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  paint_canvas_duplicate_layer (self->canvas, paint_document_get_active_layer (doc));
}

static void
on_layer_merge_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  PaintLayersPanel *self = PAINT_LAYERS_PANEL (user_data);
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  paint_canvas_merge_layer_down (self->canvas, paint_document_get_active_layer (doc));
}

static void
on_layer_del_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  PaintLayersPanel *self = PAINT_LAYERS_PANEL (user_data);
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  paint_canvas_remove_layer (self->canvas, paint_document_get_active_layer (doc));
}

static void
on_layer_up_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  PaintLayersPanel *self = PAINT_LAYERS_PANEL (user_data);
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  int active = paint_document_get_active_layer (doc);
  paint_canvas_move_layer (self->canvas, active, active + 1);
}

static void
on_layer_down_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  PaintLayersPanel *self = PAINT_LAYERS_PANEL (user_data);
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  int active = paint_document_get_active_layer (doc);
  paint_canvas_move_layer (self->canvas, active, active - 1);
}

static void
on_layer_flatten_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  PaintLayersPanel *self = PAINT_LAYERS_PANEL (user_data);
  paint_canvas_flatten_layers (self->canvas);
}

static void
on_layer_blend_changed (GObject *obj, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
  PaintLayersPanel *self = PAINT_LAYERS_PANEL (user_data);
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
  PaintLayersPanel *self = PAINT_LAYERS_PANEL (user_data);
  if (self->syncing)
    return;
  double val = gtk_range_get_value (range);
  g_autofree char *pct = g_strdup_printf ("%d%%", (int) lround (val));
  gtk_label_set_text (GTK_LABEL (self->layers_opacity_label), pct);
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  paint_canvas_set_layer_opacity (self->canvas, paint_document_get_active_layer (doc), val / 100.0);
}

void
paint_layers_panel_sync (PaintLayersPanel *self, gboolean panel_visible)
{
  if (!self || !self->layers_list || !self->canvas)
    return;
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  if (!doc)
    return;

  guint64 rev = paint_document_get_layers_revision (doc);
  if (self->last_layers_doc == doc && self->last_layers_revision == rev) {
    if (panel_visible) {
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
  if (self->solo_layer_idx >= n ||
      (self->solo_saved_vis && (int) self->solo_saved_vis->len != n)) {
    self->solo_layer_idx = -1;
    if (self->solo_saved_vis)
      g_array_set_size (self->solo_saved_vis, 0);
  }
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

    gboolean row_alock = paint_document_layer_get_alpha_lock (doc, idx);
    gboolean row_locked = paint_document_layer_get_locked (doc, idx);
    if (row_alock) {
      GtkWidget *a_icon = gtk_image_new_from_icon_name ("layer-alpha-lock-symbolic");
      gtk_image_set_pixel_size (GTK_IMAGE (a_icon), 12);
      gtk_widget_add_css_class (a_icon, "paint-layer-badge");
      gtk_widget_set_tooltip_text (a_icon, _("Alpha Locked"));
      gtk_box_append (GTK_BOX (hbox), a_icon);
    }
    if (row_locked) {
      GtkWidget *l_icon = gtk_image_new_from_icon_name ("changes-prevent-symbolic");
      gtk_image_set_pixel_size (GTK_IMAGE (l_icon), 12);
      gtk_widget_add_css_class (l_icon, "paint-layer-badge");
      gtk_widget_set_tooltip_text (l_icon, _("Layer Locked"));
      gtk_box_append (GTK_BOX (hbox), l_icon);
    }

    GtkWidget *vis_btn = gtk_button_new_from_icon_name (
        vis ? "view-reveal-symbolic" : "view-conceal-symbolic");
    gtk_widget_add_css_class (vis_btn, "flat");
    if (!vis)
      gtk_widget_add_css_class (vis_btn, "dim-label");
    gtk_widget_set_valign (vis_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text (vis_btn,
                                 vis ? _("Hide Layer (Alt+Click to Solo)")
                                     : _("Show Layer (Alt+Click to Solo)"));
    g_object_set_data (G_OBJECT (vis_btn), "paint-layer-idx", GINT_TO_POINTER (idx));
    GtkGesture *vis_alt = gtk_gesture_click_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (vis_alt), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (vis_alt), GTK_PHASE_CAPTURE);
    g_signal_connect (vis_alt, "pressed", G_CALLBACK (on_layer_vis_alt_pressed), self);
    gtk_widget_add_controller (vis_btn, GTK_EVENT_CONTROLLER (vis_alt));
    g_signal_connect (vis_btn, "clicked", G_CALLBACK (on_layer_vis_clicked), self);
    gtk_box_append (GTK_BOX (hbox), vis_btn);

    GtkGesture *dbl = gtk_gesture_click_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (dbl), GDK_BUTTON_PRIMARY);
    g_signal_connect (dbl, "pressed", G_CALLBACK (on_layer_row_double_click), self);
    gtk_widget_add_controller (row, GTK_EVENT_CONTROLLER (dbl));

    GtkGesture *rclick = gtk_gesture_click_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (rclick), GDK_BUTTON_SECONDARY);
    g_signal_connect (rclick, "pressed", G_CALLBACK (on_layer_row_right_click), self);
    gtk_widget_add_controller (row, GTK_EVENT_CONTROLLER (rclick));

    GtkDragSource *drag = gtk_drag_source_new ();
    gtk_drag_source_set_actions (drag, GDK_ACTION_MOVE);
    g_signal_connect (drag, "prepare", G_CALLBACK (on_layer_drag_prepare), self);
    gtk_widget_add_controller (row, GTK_EVENT_CONTROLLER (drag));

    GtkDropTarget *drop = gtk_drop_target_new (G_TYPE_INT, GDK_ACTION_MOVE);
    g_signal_connect (drop, "enter", G_CALLBACK (on_layer_row_drop_enter), self);
    g_signal_connect (drop, "leave", G_CALLBACK (on_layer_row_drop_leave), self);
    g_signal_connect (drop, "drop", G_CALLBACK (on_layer_row_drop), self);
    gtk_widget_add_controller (row, GTK_EVENT_CONTROLLER (drop));

    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), hbox);
    gtk_list_box_append (GTK_LIST_BOX (self->layers_list), row);
    if (idx == active)
      active_row = GTK_LIST_BOX_ROW (row);
  }

  if (active_row)
    gtk_list_box_select_row (GTK_LIST_BOX (self->layers_list), active_row);

  PaintBlendMode cur_blend = paint_document_layer_get_blend_mode (doc, active);
  double cur_opacity = paint_document_layer_get_opacity (doc, active);
  gboolean cur_alock = paint_document_layer_get_alpha_lock (doc, active);
  gboolean cur_locked = paint_document_layer_get_locked (doc, active);
  gtk_drop_down_set_selected (GTK_DROP_DOWN (self->layers_blend_dropdown), (guint) cur_blend);
  gtk_range_set_value (GTK_RANGE (self->layers_opacity_scale), cur_opacity * 100.0);
  g_autofree char *op_str = g_strdup_printf ("%d%%", (int) lround (cur_opacity * 100.0));
  gtk_label_set_text (GTK_LABEL (self->layers_opacity_label), op_str);

  if (self->layers_alpha_lock_btn)
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->layers_alpha_lock_btn), cur_alock);
  if (self->layers_lock_btn) {
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->layers_lock_btn), cur_locked);
    gtk_button_set_icon_name (GTK_BUTTON (self->layers_lock_btn),
                              cur_locked ? "changes-prevent-symbolic"
                                         : "changes-allow-symbolic");
  }

  gtk_widget_set_sensitive (self->layers_merge_btn, active > 0);
  gtk_widget_set_sensitive (self->layers_del_btn, n > 1);
  gtk_widget_set_sensitive (self->layers_up_btn, active < n - 1);
  gtk_widget_set_sensitive (self->layers_down_btn, active > 0);
  gtk_widget_set_sensitive (self->layers_flatten_btn, n > 1);

  self->syncing = FALSE;
}

static void
build_panel_ui (PaintLayersPanel *self)
{
  GtkWidget *side = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (side, "paint-layers-sidebar");

  GtkWidget *top = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_add_css_class (top, "paint-layers-toolbar");
  gtk_widget_set_margin_top (top, 8);
  gtk_widget_set_margin_bottom (top, 6);
  gtk_widget_set_margin_start (top, 12);
  gtk_widget_set_margin_end (top, 8);

  GtkWidget *heading = gtk_label_new (_("Layers"));
  gtk_widget_add_css_class (heading, "heading");
  gtk_widget_set_hexpand (heading, TRUE);
  gtk_widget_set_halign (heading, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (top), heading);

  self->layers_add_btn = gtk_button_new_from_icon_name ("list-add-symbolic");
  gtk_widget_add_css_class (self->layers_add_btn, "flat");
  gtk_widget_set_tooltip_text (self->layers_add_btn, _("New Layer (Ctrl+Shift+L)"));
  g_signal_connect (self->layers_add_btn, "clicked", G_CALLBACK (on_layer_add_clicked), self);
  gtk_box_append (GTK_BOX (top), self->layers_add_btn);

  self->layers_dup_btn = gtk_button_new_from_icon_name ("edit-copy-symbolic");
  gtk_widget_add_css_class (self->layers_dup_btn, "flat");
  gtk_widget_set_tooltip_text (self->layers_dup_btn, _("Duplicate Layer"));
  g_signal_connect (self->layers_dup_btn, "clicked", G_CALLBACK (on_layer_dup_clicked), self);
  gtk_box_append (GTK_BOX (top), self->layers_dup_btn);

  self->layers_merge_btn = gtk_button_new_from_icon_name ("layer-merge-down-symbolic");
  gtk_widget_add_css_class (self->layers_merge_btn, "flat");
  gtk_widget_set_tooltip_text (self->layers_merge_btn, _("Merge Layer Down"));
  g_signal_connect (self->layers_merge_btn, "clicked", G_CALLBACK (on_layer_merge_clicked), self);
  gtk_box_append (GTK_BOX (top), self->layers_merge_btn);

  self->layers_del_btn = gtk_button_new_from_icon_name ("list-remove-symbolic");
  gtk_widget_add_css_class (self->layers_del_btn, "flat");
  gtk_widget_set_tooltip_text (self->layers_del_btn, _("Delete Layer"));
  g_signal_connect (self->layers_del_btn, "clicked", G_CALLBACK (on_layer_del_clicked), self);
  gtk_box_append (GTK_BOX (top), self->layers_del_btn);
  gtk_box_append (GTK_BOX (side), top);

  GtkWidget *props = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_bottom (props, 8);
  gtk_widget_set_margin_start (props, 12);
  gtk_widget_set_margin_end (props, 12);

  GtkWidget *blend_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *blend_lbl = gtk_label_new (_("Blend"));
  gtk_widget_add_css_class (blend_lbl, "paint-dim-label");
  gtk_widget_set_halign (blend_lbl, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (blend_row), blend_lbl);

  self->layers_blend_dropdown = gtk_drop_down_new_from_strings (LAYER_BLEND_LABELS);
  gtk_widget_set_hexpand (self->layers_blend_dropdown, TRUE);
  g_signal_connect (self->layers_blend_dropdown, "notify::selected",
                    G_CALLBACK (on_layer_blend_changed), self);
  gtk_box_append (GTK_BOX (blend_row), self->layers_blend_dropdown);

  self->layers_alpha_lock_btn = gtk_toggle_button_new ();
  gtk_button_set_icon_name (GTK_BUTTON (self->layers_alpha_lock_btn), "layer-alpha-lock-symbolic");
  gtk_widget_add_css_class (self->layers_alpha_lock_btn, "flat");
  gtk_widget_set_tooltip_text (self->layers_alpha_lock_btn, _("Lock Transparency (Alpha Lock)"));
  g_signal_connect (self->layers_alpha_lock_btn, "toggled",
                    G_CALLBACK (on_layer_alpha_lock_toggled), self);
  gtk_box_append (GTK_BOX (blend_row), self->layers_alpha_lock_btn);

  self->layers_lock_btn = gtk_toggle_button_new ();
  gtk_button_set_icon_name (GTK_BUTTON (self->layers_lock_btn), "changes-allow-symbolic");
  gtk_widget_add_css_class (self->layers_lock_btn, "flat");
  gtk_widget_set_tooltip_text (self->layers_lock_btn, _("Lock Layer"));
  g_signal_connect (self->layers_lock_btn, "toggled",
                    G_CALLBACK (on_layer_lock_toggled), self);
  gtk_box_append (GTK_BOX (blend_row), self->layers_lock_btn);

  gtk_box_append (GTK_BOX (props), blend_row);

  GtkWidget *op_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *op_lbl = gtk_label_new (_("Opacity"));
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
  gtk_widget_set_tooltip_text (self->layers_up_btn, _("Move Layer Up"));
  g_signal_connect (self->layers_up_btn, "clicked", G_CALLBACK (on_layer_up_clicked), self);
  gtk_box_append (GTK_BOX (bot), self->layers_up_btn);

  self->layers_down_btn = gtk_button_new_from_icon_name ("go-down-symbolic");
  gtk_widget_add_css_class (self->layers_down_btn, "flat");
  gtk_widget_set_tooltip_text (self->layers_down_btn, _("Move Layer Down"));
  g_signal_connect (self->layers_down_btn, "clicked", G_CALLBACK (on_layer_down_clicked), self);
  gtk_box_append (GTK_BOX (bot), self->layers_down_btn);

  GtkWidget *spacer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand (spacer, TRUE);
  gtk_box_append (GTK_BOX (bot), spacer);

  self->layers_flatten_btn = gtk_button_new_with_label (_("Flatten"));
  gtk_widget_add_css_class (self->layers_flatten_btn, "flat");
  gtk_widget_set_tooltip_text (self->layers_flatten_btn, _("Flatten All Layers"));
  g_signal_connect (self->layers_flatten_btn, "clicked", G_CALLBACK (on_layer_flatten_clicked), self);
  gtk_box_append (GTK_BOX (bot), self->layers_flatten_btn);

  gtk_box_append (GTK_BOX (side), bot);

  GtkDropTarget *file_drop = gtk_drop_target_new (GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
  g_signal_connect (file_drop, "drop", G_CALLBACK (on_layers_files_dropped), self);
  gtk_widget_add_controller (side, GTK_EVENT_CONTROLLER (file_drop));

  adw_bin_set_child (ADW_BIN (self), side);
}

static void
paint_layers_panel_finalize (GObject *object)
{
  PaintLayersPanel *self = PAINT_LAYERS_PANEL (object);
  g_clear_pointer (&self->layer_thumbs, g_ptr_array_unref);
  g_clear_pointer (&self->solo_saved_vis, g_array_unref);
  G_OBJECT_CLASS (paint_layers_panel_parent_class)->finalize (object);
}

static void
paint_layers_panel_class_init (PaintLayersPanelClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = paint_layers_panel_finalize;
}

static void
paint_layers_panel_init (PaintLayersPanel *self)
{
  self->layer_thumbs = g_ptr_array_new ();
  self->solo_layer_idx = -1;
  self->solo_saved_vis = g_array_new (FALSE, FALSE, sizeof (gboolean));
}

PaintLayersPanel *
paint_layers_panel_new (PaintCanvas *canvas)
{
  PaintLayersPanel *self = g_object_new (PAINT_TYPE_LAYERS_PANEL, NULL);
  self->canvas = canvas;
  build_panel_ui (self);
  return self;
}
