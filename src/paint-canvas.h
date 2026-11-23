#pragma once

#include <cairo.h>
#include <gtk/gtk.h>

#include "paint-document.h"
#include "paint-filters.h"
#include "paint-io.h"
#include "paint-tools.h"

G_BEGIN_DECLS

#define PAINT_TYPE_CANVAS (paint_canvas_get_type ())
G_DECLARE_FINAL_TYPE (PaintCanvas, paint_canvas, PAINT, CANVAS, GtkWidget)

PaintCanvas *paint_canvas_new (PaintDocument *doc, PaintToolSettings *settings);

PaintDocument *paint_canvas_get_document (PaintCanvas *self);
void paint_canvas_set_document (PaintCanvas *self, PaintDocument *doc);
PaintToolSettings *paint_canvas_get_settings (PaintCanvas *self);

double paint_canvas_get_zoom (PaintCanvas *self);
double paint_canvas_get_min_zoom (void);
double paint_canvas_get_max_zoom (void);
void paint_canvas_set_zoom (PaintCanvas *self, double zoom);
void paint_canvas_zoom_in (PaintCanvas *self);
void paint_canvas_zoom_out (PaintCanvas *self);
void paint_canvas_zoom_reset (PaintCanvas *self);

gboolean paint_canvas_undo (PaintCanvas *self);
gboolean paint_canvas_redo (PaintCanvas *self);
void paint_canvas_clear (PaintCanvas *self);
void paint_canvas_clear_transparent (PaintCanvas *self);
void paint_canvas_resize (PaintCanvas *self, int width, int height);
void paint_canvas_scale (PaintCanvas *self, int width, int height);
gboolean paint_canvas_trim_transparent (PaintCanvas *self);
void paint_canvas_rotate_90 (PaintCanvas *self, gboolean clockwise);
void paint_canvas_flip (PaintCanvas *self, gboolean horizontal);

gboolean paint_canvas_has_selection (PaintCanvas *self);
void paint_canvas_select_all (PaintCanvas *self);
void paint_canvas_invert_selection (PaintCanvas *self);
gboolean paint_canvas_get_selection_mask_and_rect (PaintCanvas *self,
                                                  cairo_rectangle_int_t *out_rect,
                                                  cairo_surface_t **out_mask);
gboolean paint_canvas_is_drawing (PaintCanvas *self);
void paint_canvas_commit_selection (PaintCanvas *self);
void paint_canvas_cancel_selection (PaintCanvas *self);
gboolean paint_canvas_delete_selection (PaintCanvas *self);
gboolean paint_canvas_crop_to_selection (PaintCanvas *self);
GdkTexture *paint_canvas_copy_selection (PaintCanvas *self);
GdkTexture *paint_canvas_copy_merged_selection (PaintCanvas *self);
GdkTexture *paint_canvas_cut_selection (PaintCanvas *self);
void paint_canvas_paste_texture (PaintCanvas *self, GdkTexture *texture);
void paint_canvas_nudge_selection (PaintCanvas *self, int dx, int dy);

gboolean paint_canvas_has_active_text (PaintCanvas *self);
const char *paint_canvas_get_active_text (PaintCanvas *self);
void paint_canvas_set_active_text (PaintCanvas *self, const char *text);
void paint_canvas_commit_text (PaintCanvas *self);
void paint_canvas_cancel_text (PaintCanvas *self);
void paint_canvas_commit_active_overlays (PaintCanvas *self);

void paint_canvas_set_show_cursor_ring (PaintCanvas *self, gboolean show);
void paint_canvas_set_show_pixel_grid (PaintCanvas *self, gboolean show);
gboolean paint_canvas_get_show_pixel_grid (PaintCanvas *self);
void paint_canvas_set_alt_eyedropper (PaintCanvas *self, gboolean active);
void paint_canvas_set_pan_override (PaintCanvas *self, gboolean active, gboolean dragging);
void paint_canvas_check_viewport (PaintCanvas *self);

int paint_canvas_add_layer (PaintCanvas *self, const char *name);
int paint_canvas_add_layer_from_texture (PaintCanvas *self,
                                         const char *name,
                                         GdkTexture *texture);
gboolean paint_canvas_duplicate_layer (PaintCanvas *self, int index);
gboolean paint_canvas_remove_layer (PaintCanvas *self, int index);
gboolean paint_canvas_move_layer (PaintCanvas *self, int from_index, int to_index);
gboolean paint_canvas_merge_layer_down (PaintCanvas *self, int index);
gboolean paint_canvas_flatten_layers (PaintCanvas *self);
void paint_canvas_set_active_layer (PaintCanvas *self, int index);
void paint_canvas_set_layer_visible (PaintCanvas *self, int index, gboolean visible);
void paint_canvas_set_layers_visibility (PaintCanvas *self,
                                         const gboolean *visible,
                                         int count);
void paint_canvas_set_layer_opacity (PaintCanvas *self, int index, double opacity);
void paint_canvas_set_layer_blend_mode (PaintCanvas *self, int index, PaintBlendMode mode);
void paint_canvas_set_layer_alpha_lock (PaintCanvas *self, int index, gboolean alpha_lock);
void paint_canvas_set_layer_locked (PaintCanvas *self, int index, gboolean locked);
void paint_canvas_set_layer_name (PaintCanvas *self, int index, const char *name);

typedef void (*PaintCanvasFilterFunc) (cairo_surface_t *surface,
                                       const cairo_rectangle_int_t *rect,
                                       cairo_surface_t *mask,
                                       gboolean alpha_lock,
                                       gpointer user_data);

gboolean paint_canvas_begin_filter_preview (PaintCanvas *self);
void paint_canvas_update_filter_preview (PaintCanvas *self,
                                         PaintCanvasFilterFunc func,
                                         gpointer user_data);
void paint_canvas_commit_filter_preview (PaintCanvas *self);
void paint_canvas_cancel_filter_preview (PaintCanvas *self);
gboolean paint_canvas_apply_filter (PaintCanvas *self,
                                    PaintCanvasFilterFunc func,
                                    gpointer user_data);

G_END_DECLS
