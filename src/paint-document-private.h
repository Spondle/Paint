#pragma once

#include "paint-document.h"

G_BEGIN_DECLS

#define PAINT_UNDO_TILE_SIZE 256

typedef struct {
  char *name;
  cairo_surface_t *surface; /* ARGB32, doc->width x doc->height */
  gboolean visible;
  double opacity;           /* 0.0 .. 1.0 */
  PaintBlendMode blend_mode;
  gboolean alpha_lock;
  gboolean locked;
} PaintLayer;

typedef struct {
  char *name;
  gboolean visible;
  double opacity;
  PaintBlendMode blend_mode;
  gboolean alpha_lock;
  gboolean locked;
  int width;
  int height;
  int stride;
  GBytes *pixels;
} UndoLayerSnapshot;

typedef struct {
  int x;
  int y;
  int w;
  int h;
  GBytes *pixels; /* packed row-major w * 4 bytes */
} UndoTile;

typedef struct {
  int width;
  int height;
  gboolean is_full;
  GdkRGBA background_color;
  GPtrArray *layer_snaps; /* element-type: UndoLayerSnapshot* */
  int active_layer_index;
  int next_layer_num;
  gboolean is_opacity_step;
  int opacity_layer_idx;
  int layer_index;        /* for tiled steps */
  GPtrArray *tiles;       /* element-type: UndoTile* */
  GHashTable *saved_keys; /* key: uint32 packed (ty << 16) | tx */
  gsize byte_size;
} UndoStep;

struct _PaintDocument {
  int width;
  int height;
  GdkRGBA background_color;
  cairo_surface_t *surface;           /* non-owning alias to visible composite */
  cairo_surface_t *composite_surface; /* owned when multi-layer/opacity/blend/hole active */
  GPtrArray *layers;                  /* element-type: PaintLayer* (bottom -> top) */
  int active_layer_index;
  int next_layer_num;
  guint64 layers_revision;

  gboolean has_active_hole;
  cairo_rectangle_int_t active_hole;
  cairo_surface_t *active_hole_mask; /* optional A8 mask */

  GFile *file;
  gboolean modified;
  GPtrArray *undo_stack; /* element-type: UndoStep* */
  GPtrArray *redo_stack; /* element-type: UndoStep* */
  gsize undo_bytes;
  gsize redo_bytes;
  gsize undo_budget_bytes;

  UndoStep *active_stroke_undo;
  gboolean pre_stroke_modified;
  double stroke_start_x;
  double stroke_start_y;
  double prev_stroke_x;
  double prev_stroke_y;
  double prev2_stroke_x;
  double prev2_stroke_y;

  gboolean all_dirty;
  gboolean has_dirty_rect;
  cairo_rectangle_int_t dirty_rect;
};

PaintDocument *paint_document_new_with_background_internal (int width,
                                                            int height,
                                                            const GdkRGBA *bg);
cairo_surface_t *paint_document_create_blank_surface (int width,
                                                      int height,
                                                      const GdkRGBA *bg);
PaintLayer *paint_document_get_active_layer_internal (const PaintDocument *doc);
void paint_document_recomposite_all (PaintDocument *doc);
void paint_document_recomposite_rect (PaintDocument *doc, int rx, int ry, int rw, int rh);
void paint_document_mark_all_dirty (PaintDocument *doc);
void paint_document_mark_rect_dirty (PaintDocument *doc, int x, int y, int w, int h);
void paint_document_set_floating_hole (PaintDocument *doc,
                                       const cairo_rectangle_int_t *hole);
void paint_document_set_floating_hole_masked (PaintDocument *doc,
                                              const cairo_rectangle_int_t *hole,
                                              cairo_surface_t *hole_mask);
void paint_document_flush_active_stroke_undo (PaintDocument *doc);
void paint_document_push_full_undo (PaintDocument *doc);
void paint_document_set_file_internal (PaintDocument *doc, GFile *file);
void paint_document_set_background_color_internal (PaintDocument *doc, const GdkRGBA *bg);
void paint_document_append_layer_surface_internal (PaintDocument *doc,
                                                   const char *name,
                                                   cairo_surface_t *surface,
                                                   gboolean visible,
                                                   double opacity,
                                                   PaintBlendMode blend_mode);
void paint_document_clear_layers_internal (PaintDocument *doc);

void paint_document_render_text_layout (cairo_t *cr,
                                        double x,
                                        double y,
                                        const char *text,
                                        const PaintToolSettings *settings,
                                        gboolean draw,
                                        int *out_x,
                                        int *out_y,
                                        int *out_w,
                                        int *out_h);

void paint_document_compute_tool_stroke_bounds (const PaintTool *tool,
                                                const PaintToolSettings *settings,
                                                double x0,
                                                double y0,
                                                double x1,
                                                double y1,
                                                int *rx,
                                                int *ry,
                                                int *rw,
                                                int *rh);

G_END_DECLS
