#pragma once

#include <cairo.h>
#include <gtk/gtk.h>

#include "paint-tools.h"

G_BEGIN_DECLS

#define PAINT_DEFAULT_CANVAS_WIDTH 640
#define PAINT_DEFAULT_CANVAS_HEIGHT 380

typedef struct _PaintDocument PaintDocument;

PaintDocument *paint_document_new (int width, int height);
PaintDocument *paint_document_new_transparent (int width, int height);
PaintDocument *paint_document_new_from_file (GFile *file, GError **error);
PaintDocument *paint_document_new_from_draft (GFile *draft_file,
                                              GFile *original_file,
                                              gboolean modified,
                                              GError **error);
void paint_document_free (PaintDocument *doc);

int paint_document_get_width (const PaintDocument *doc);
int paint_document_get_height (const PaintDocument *doc);
cairo_surface_t *paint_document_get_surface (const PaintDocument *doc);
GFile *paint_document_get_file (const PaintDocument *doc);
gboolean paint_document_get_modified (const PaintDocument *doc);
void paint_document_set_modified (PaintDocument *doc, gboolean modified);
gboolean paint_document_has_transparent_background (const PaintDocument *doc);

gboolean paint_document_can_undo (const PaintDocument *doc);
gboolean paint_document_can_redo (const PaintDocument *doc);
gboolean paint_document_undo (PaintDocument *doc);
gboolean paint_document_redo (PaintDocument *doc);

void paint_document_begin_stroke (PaintDocument *doc,
                                  double x,
                                  double y,
                                  const PaintTool *tool,
                                  PaintToolSettings *settings);
void paint_document_continue_stroke (PaintDocument *doc,
                                     double x,
                                     double y,
                                     const PaintTool *tool,
                                     PaintToolSettings *settings);
void paint_document_end_stroke (PaintDocument *doc,
                                double x,
                                double y,
                                const PaintTool *tool,
                                PaintToolSettings *settings);
void paint_document_cancel_stroke (PaintDocument *doc);

void paint_document_clear (PaintDocument *doc);
void paint_document_clear_transparent (PaintDocument *doc);
void paint_document_resize (PaintDocument *doc, int new_width, int new_height);
void paint_document_scale (PaintDocument *doc, int new_width, int new_height);
void paint_document_crop (PaintDocument *doc, int x, int y, int w, int h);
void paint_document_rotate_90 (PaintDocument *doc, gboolean clockwise);
void paint_document_flip (PaintDocument *doc, gboolean horizontal);
gboolean paint_document_pick_color (const PaintDocument *doc,
                                    double x,
                                    double y,
                                    GdkRGBA *out_color);
void paint_document_shift_preview_start (PaintDocument *doc,
                                         PaintToolSettings *settings,
                                         double new_x0,
                                         double new_y0);
cairo_surface_t *paint_document_copy_rect (const PaintDocument *doc,
                                           int x,
                                           int y,
                                           int w,
                                           int h);
void paint_document_clear_rect (PaintDocument *doc, int x, int y, int w, int h);
void paint_document_blit_surface (PaintDocument *doc,
                                  cairo_surface_t *src,
                                  int x,
                                  int y);
void paint_document_commit_move (PaintDocument *doc,
                                 const cairo_rectangle_int_t *src_hole,
                                 cairo_surface_t *floating,
                                 int dst_x,
                                 int dst_y);
void paint_document_draw_text (PaintDocument *doc,
                               double x,
                               double y,
                               const char *text,
                               const PaintToolSettings *settings);
gboolean paint_document_save_to_file (PaintDocument *doc, GFile *file, GError **error);

cairo_surface_t *paint_surface_from_texture (GdkTexture *texture);
GdkTexture *paint_texture_from_surface (cairo_surface_t *surface);
PaintDocument *paint_document_new_from_texture (GdkTexture *texture);
cairo_surface_t *paint_surface_rotate_90 (cairo_surface_t *src, gboolean clockwise);
cairo_surface_t *paint_surface_flip (cairo_surface_t *src, gboolean horizontal);

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
void paint_canvas_rotate_90 (PaintCanvas *self, gboolean clockwise);
void paint_canvas_flip (PaintCanvas *self, gboolean horizontal);

gboolean paint_canvas_has_selection (PaintCanvas *self);
void paint_canvas_select_all (PaintCanvas *self);
void paint_canvas_commit_selection (PaintCanvas *self);
void paint_canvas_cancel_selection (PaintCanvas *self);
gboolean paint_canvas_delete_selection (PaintCanvas *self);
gboolean paint_canvas_crop_to_selection (PaintCanvas *self);
GdkTexture *paint_canvas_copy_selection (PaintCanvas *self);
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
void paint_canvas_check_viewport (PaintCanvas *self);

G_END_DECLS
