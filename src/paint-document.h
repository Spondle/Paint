#pragma once

#include <cairo.h>
#include <gtk/gtk.h>

#include "paint-tools.h"

G_BEGIN_DECLS

#define PAINT_DEFAULT_CANVAS_WIDTH 640
#define PAINT_DEFAULT_CANVAS_HEIGHT 380

#define PAINT_UNDO_BUDGET_BYTES ((gsize) (256 * 1024 * 1024))
#define PAINT_UNDO_MIN_STEPS 10
#define PAINT_UNDO_MAX_STEPS 250

typedef enum {
  PAINT_BLEND_NORMAL = 0,
  PAINT_BLEND_MULTIPLY,
  PAINT_BLEND_SCREEN,
  PAINT_BLEND_OVERLAY,
  PAINT_BLEND_DARKEN,
  PAINT_BLEND_LIGHTEN
} PaintBlendMode;

typedef struct _PaintDocument PaintDocument;

PaintDocument *paint_document_new (int width, int height);
PaintDocument *paint_document_new_transparent (int width, int height);
void paint_document_free (PaintDocument *doc);

int paint_document_get_width (const PaintDocument *doc);
int paint_document_get_height (const PaintDocument *doc);
cairo_surface_t *paint_document_get_surface (const PaintDocument *doc);
GFile *paint_document_get_file (const PaintDocument *doc);
gboolean paint_document_get_modified (const PaintDocument *doc);
void paint_document_set_modified (PaintDocument *doc, gboolean modified);
gboolean paint_document_has_transparent_background (const PaintDocument *doc);
gboolean paint_document_has_layer_structure (const PaintDocument *doc);

int paint_document_get_n_layers (const PaintDocument *doc);
int paint_document_get_active_layer (const PaintDocument *doc);
void paint_document_set_active_layer (PaintDocument *doc, int index);
guint64 paint_document_get_layers_revision (const PaintDocument *doc);

const char *paint_document_layer_get_name (const PaintDocument *doc, int index);
void paint_document_layer_set_name (PaintDocument *doc, int index, const char *name);
gboolean paint_document_layer_get_visible (const PaintDocument *doc, int index);
void paint_document_layer_set_visible (PaintDocument *doc, int index, gboolean visible);
double paint_document_layer_get_opacity (const PaintDocument *doc, int index);
void paint_document_layer_set_opacity (PaintDocument *doc, int index, double opacity);
PaintBlendMode paint_document_layer_get_blend_mode (const PaintDocument *doc, int index);
void paint_document_layer_set_blend_mode (PaintDocument *doc, int index, PaintBlendMode mode);
gboolean paint_document_layer_get_alpha_lock (const PaintDocument *doc, int index);
void paint_document_layer_set_alpha_lock (PaintDocument *doc, int index, gboolean alpha_lock);
gboolean paint_document_layer_get_locked (const PaintDocument *doc, int index);
void paint_document_layer_set_locked (PaintDocument *doc, int index, gboolean locked);
void paint_document_layer_set_locks_no_undo (PaintDocument *doc,
                                             int index,
                                             gboolean alpha_lock,
                                             gboolean locked);
cairo_surface_t *paint_document_layer_get_surface (const PaintDocument *doc, int index);

int paint_document_add_layer (PaintDocument *doc, const char *name);
int paint_document_add_layer_from_surface (PaintDocument *doc,
                                           const char *name,
                                           cairo_surface_t *src);
void paint_document_set_layers_visibility (PaintDocument *doc,
                                           const gboolean *visible,
                                           int count);
gboolean paint_document_duplicate_layer (PaintDocument *doc, int index);
gboolean paint_document_remove_layer (PaintDocument *doc, int index);
gboolean paint_document_move_layer (PaintDocument *doc, int from_index, int to_index);
gboolean paint_document_merge_layer_down (PaintDocument *doc, int index);
gboolean paint_document_flatten_layers (PaintDocument *doc);

gboolean paint_document_append_restored_layer (PaintDocument *doc,
                                               const char *name,
                                               GFile *png_file,
                                               gboolean visible,
                                               double opacity,
                                               PaintBlendMode blend_mode);
void paint_document_finish_session_restore (PaintDocument *doc,
                                            int active_layer,
                                            gboolean modified);

gboolean paint_document_can_undo (const PaintDocument *doc);
gboolean paint_document_can_redo (const PaintDocument *doc);
gboolean paint_document_undo (PaintDocument *doc);
gboolean paint_document_redo (PaintDocument *doc);
gsize paint_document_get_undo_memory_bytes (const PaintDocument *doc);
guint paint_document_get_undo_depth (const PaintDocument *doc);
void paint_document_set_undo_budget_bytes (PaintDocument *doc, gsize budget_bytes);

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
gboolean paint_document_trim_transparent (PaintDocument *doc);
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
cairo_surface_t *paint_document_copy_masked_rect (const PaintDocument *doc,
                                                  const cairo_rectangle_int_t *rect,
                                                  cairo_surface_t *mask);
cairo_surface_t *paint_document_copy_merged_rect (const PaintDocument *doc,
                                                  int x,
                                                  int y,
                                                  int w,
                                                  int h);
cairo_surface_t *paint_document_copy_merged_masked_rect (const PaintDocument *doc,
                                                         const cairo_rectangle_int_t *rect,
                                                         cairo_surface_t *mask);
void paint_document_clear_rect (PaintDocument *doc, int x, int y, int w, int h);
void paint_document_clear_masked_rect (PaintDocument *doc,
                                       const cairo_rectangle_int_t *rect,
                                       cairo_surface_t *mask);
void paint_document_blit_surface (PaintDocument *doc,
                                  cairo_surface_t *src,
                                  int x,
                                  int y);
void paint_document_commit_move (PaintDocument *doc,
                                 const cairo_rectangle_int_t *src_hole,
                                 cairo_surface_t *floating,
                                 int dst_x,
                                 int dst_y);
void paint_document_commit_move_masked (PaintDocument *doc,
                                        const cairo_rectangle_int_t *src_hole,
                                        cairo_surface_t *hole_mask,
                                        cairo_surface_t *floating,
                                        const cairo_rectangle_int_t *dst_rect);
void paint_document_draw_text (PaintDocument *doc,
                               double x,
                               double y,
                               const char *text,
                               const PaintToolSettings *settings);

cairo_surface_t *paint_document_dup_active_layer_surface (const PaintDocument *doc);
void paint_document_restore_active_layer_rect (PaintDocument *doc,
                                               cairo_surface_t *backup,
                                               const cairo_rectangle_int_t *rect);
void paint_document_notify_active_layer_rect_modified (PaintDocument *doc,
                                                       const cairo_rectangle_int_t *rect);
void paint_document_commit_active_layer_from_backup (PaintDocument *doc,
                                                   cairo_surface_t *backup,
                                                   const cairo_rectangle_int_t *rect);

cairo_surface_t *paint_surface_from_texture (GdkTexture *texture);
GdkTexture *paint_texture_from_surface (cairo_surface_t *surface);
PaintDocument *paint_document_new_from_texture (GdkTexture *texture);
cairo_surface_t *paint_surface_rotate_90 (cairo_surface_t *src, gboolean clockwise);
cairo_surface_t *paint_surface_flip (cairo_surface_t *src, gboolean horizontal);

G_END_DECLS
