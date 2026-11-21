#include "paint-canvas.h"

#include <math.h>
#include <pango/pangocairo.h>
#include <stdint.h>
#include <string.h>

#define MAX_UNDO_STEPS 40
#define UNDO_TILE_SIZE 256
#define VIEWPORT_OVERSCAN 256
#define MIN_VP_CACHE_DIM 1024
#define MAX_OVERVIEW_DIM 2048

static const double ZOOM_STEPS[] = {
  0.05, 0.06, 0.08, 0.10, 0.125, 0.16, 0.20, 0.25, 0.33, 0.40, 0.50, 0.60,
  0.75, 0.90, 1.0, 1.25, 1.5, 2.0, 2.5, 3.0, 4.0, 6.0, 8.0, 12.0, 16.0, 24.0, 32.0
};

/* ============================================================================
 * Tiled / Full-Surface Undo Snapshots
 * ============================================================================ */

typedef struct {
  int x;
  int y;
  int w;
  int h;
  GBytes *pixels; /* packed row-major w * 4 bytes */
} UndoTile;

static void
undo_tile_free (gpointer ptr)
{
  UndoTile *t = ptr;
  if (!t)
    return;
  g_clear_pointer (&t->pixels, g_bytes_unref);
  g_free (t);
}

typedef struct {
  int width;
  int height;
  gboolean is_full;
  GdkRGBA background_color;
  int full_stride;
  GBytes *full_pixels;
  GPtrArray *tiles;       /* element-type: UndoTile* */
  GHashTable *saved_keys; /* key: uint32 packed (ty << 16) | tx */
} UndoStep;

static void
undo_step_free (gpointer ptr)
{
  UndoStep *step = ptr;
  if (!step)
    return;
  g_clear_pointer (&step->full_pixels, g_bytes_unref);
  g_clear_pointer (&step->tiles, g_ptr_array_unref);
  g_clear_pointer (&step->saved_keys, g_hash_table_unref);
  g_free (step);
}

static UndoTile *
capture_rect_tile (cairo_surface_t *surface, int x, int y, int w, int h)
{
  cairo_surface_flush (surface);
  int stride = cairo_image_surface_get_stride (surface);
  const unsigned char *src = cairo_image_surface_get_data (surface);
  gsize row_bytes = (gsize) w * 4;
  unsigned char *buf = g_malloc (row_bytes * (gsize) h);

  for (int row = 0; row < h; row++) {
    const unsigned char *src_row = src + (gsize) (y + row) * (gsize) stride + (gsize) x * 4;
    memcpy (buf + (gsize) row * row_bytes, src_row, row_bytes);
  }

  UndoTile *t = g_new0 (UndoTile, 1);
  t->x = x;
  t->y = y;
  t->w = w;
  t->h = h;
  t->pixels = g_bytes_new_take (buf, row_bytes * (gsize) h);
  return t;
}

static void
restore_rect_tile (cairo_surface_t *surface, const UndoTile *t)
{
  int stride = cairo_image_surface_get_stride (surface);
  unsigned char *dst = cairo_image_surface_get_data (surface);
  gsize len = 0;
  const unsigned char *src = g_bytes_get_data (t->pixels, &len);
  gsize row_bytes = (gsize) t->w * 4;

  for (int row = 0; row < t->h; row++) {
    unsigned char *dst_row = dst + (gsize) (t->y + row) * (gsize) stride + (gsize) t->x * 4;
    memcpy (dst_row, src + (gsize) row * row_bytes, row_bytes);
  }
}

static UndoStep *
undo_step_new_full (cairo_surface_t *surface, const GdkRGBA *background_color)
{
  cairo_surface_flush (surface);
  int w = cairo_image_surface_get_width (surface);
  int h = cairo_image_surface_get_height (surface);
  int stride = cairo_image_surface_get_stride (surface);
  const unsigned char *data = cairo_image_surface_get_data (surface);

  UndoStep *step = g_new0 (UndoStep, 1);
  step->width = w;
  step->height = h;
  step->is_full = TRUE;
  step->background_color = background_color
                             ? *background_color
                             : (GdkRGBA) { 1.0f, 1.0f, 1.0f, 1.0f };
  step->full_stride = stride;
  step->full_pixels = g_bytes_new (data, (gsize) stride * (gsize) h);
  return step;
}

static UndoStep *
undo_step_new_tiled (int width, int height)
{
  UndoStep *step = g_new0 (UndoStep, 1);
  step->width = width;
  step->height = height;
  step->is_full = FALSE;
  step->tiles = g_ptr_array_new_with_free_func (undo_tile_free);
  step->saved_keys = g_hash_table_new (g_direct_hash, g_direct_equal);
  return step;
}

static void
undo_step_capture_rect (UndoStep *step,
                        cairo_surface_t *surface,
                        int rx,
                        int ry,
                        int rw,
                        int rh)
{
  int x0 = MAX (0, rx);
  int y0 = MAX (0, ry);
  int x1 = MIN (step->width, rx + rw);
  int y1 = MIN (step->height, ry + rh);
  if (x0 >= x1 || y0 >= y1)
    return;

  int tx0 = x0 / UNDO_TILE_SIZE;
  int ty0 = y0 / UNDO_TILE_SIZE;
  int tx1 = (x1 - 1) / UNDO_TILE_SIZE;
  int ty1 = (y1 - 1) / UNDO_TILE_SIZE;

  for (int ty = ty0; ty <= ty1; ty++) {
    for (int tx = tx0; tx <= tx1; tx++) {
      guint key = (((guint) ty) << 16) | (guint) tx;
      if (g_hash_table_contains (step->saved_keys, GUINT_TO_POINTER (key)))
        continue;
      g_hash_table_add (step->saved_keys, GUINT_TO_POINTER (key));

      int tile_x = tx * UNDO_TILE_SIZE;
      int tile_y = ty * UNDO_TILE_SIZE;
      int tile_w = MIN (UNDO_TILE_SIZE, step->width - tile_x);
      int tile_h = MIN (UNDO_TILE_SIZE, step->height - tile_y);
      g_ptr_array_add (step->tiles, capture_rect_tile (surface, tile_x, tile_y, tile_w, tile_h));
    }
  }
}

static cairo_surface_t *
restore_full_surface (const UndoStep *step)
{
  cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
                                                         step->width,
                                                         step->height);
  gsize len = 0;
  const unsigned char *src = g_bytes_get_data (step->full_pixels, &len);
  unsigned char *dst = cairo_image_surface_get_data (surface);
  int dst_stride = cairo_image_surface_get_stride (surface);

  if (dst_stride == step->full_stride) {
    memcpy (dst, src, len);
  } else {
    int copy_bytes = MIN (dst_stride, step->full_stride);
    for (int y = 0; y < step->height; y++)
      memcpy (dst + y * dst_stride, src + y * step->full_stride, (gsize) copy_bytes);
  }
  cairo_surface_mark_dirty (surface);
  return surface;
}

/* ============================================================================
 * PaintDocument
 * ============================================================================ */

struct _PaintDocument {
  int width;
  int height;
  GdkRGBA background_color;
  cairo_surface_t *surface;
  GFile *file;
  gboolean modified;
  GPtrArray *undo_stack; /* element-type: UndoStep* */
  GPtrArray *redo_stack; /* element-type: UndoStep* */

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

static void
mark_all_dirty (PaintDocument *doc)
{
  doc->all_dirty = TRUE;
  doc->has_dirty_rect = FALSE;
}

static void
mark_rect_dirty (PaintDocument *doc, int x, int y, int w, int h)
{
  if (doc->all_dirty)
    return;
  int x0 = MAX (0, x);
  int y0 = MAX (0, y);
  int x1 = MIN (doc->width, x + w);
  int y1 = MIN (doc->height, y + h);
  if (x0 >= x1 || y0 >= y1)
    return;

  if (!doc->has_dirty_rect) {
    doc->dirty_rect = (cairo_rectangle_int_t) { x0, y0, x1 - x0, y1 - y0 };
    doc->has_dirty_rect = TRUE;
  } else {
    int ux0 = MIN (doc->dirty_rect.x, x0);
    int uy0 = MIN (doc->dirty_rect.y, y0);
    int ux1 = MAX (doc->dirty_rect.x + doc->dirty_rect.width, x1);
    int uy1 = MAX (doc->dirty_rect.y + doc->dirty_rect.height, y1);
    doc->dirty_rect = (cairo_rectangle_int_t) { ux0, uy0, ux1 - ux0, uy1 - uy0 };
  }
}

static void
push_undo_step (PaintDocument *doc, UndoStep *step)
{
  g_ptr_array_add (doc->undo_stack, step);
  if (doc->undo_stack->len > MAX_UNDO_STEPS)
    g_ptr_array_remove_index (doc->undo_stack, 0);
  g_ptr_array_set_size (doc->redo_stack, 0);
}

static cairo_surface_t *
create_blank_surface (int width, int height, const GdkRGBA *bg)
{
  cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create (surface);
  cairo_set_source_rgba (cr, bg->red, bg->green, bg->blue, bg->alpha);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_paint (cr);
  cairo_destroy (cr);
  return surface;
}

static PaintDocument *
paint_document_new_with_background (int width, int height, const GdkRGBA *bg)
{
  PaintDocument *doc = g_new0 (PaintDocument, 1);
  doc->width = MAX (1, width);
  doc->height = MAX (1, height);
  doc->background_color = *bg;
  doc->surface = create_blank_surface (doc->width, doc->height, &doc->background_color);
  doc->undo_stack = g_ptr_array_new_with_free_func (undo_step_free);
  doc->redo_stack = g_ptr_array_new_with_free_func (undo_step_free);
  doc->all_dirty = TRUE;
  return doc;
}

PaintDocument *
paint_document_new (int width, int height)
{
  GdkRGBA white = { 1.0f, 1.0f, 1.0f, 1.0f };
  return paint_document_new_with_background (width, height, &white);
}

PaintDocument *
paint_document_new_transparent (int width, int height)
{
  GdkRGBA transparent = { 0.0f, 0.0f, 0.0f, 0.0f };
  return paint_document_new_with_background (width, height, &transparent);
}

static gboolean
surface_has_transparency (cairo_surface_t *surface)
{
  if (!surface || cairo_surface_get_type (surface) != CAIRO_SURFACE_TYPE_IMAGE)
    return FALSE;
  if (cairo_image_surface_get_format (surface) != CAIRO_FORMAT_ARGB32)
    return FALSE;

  cairo_surface_flush (surface);
  int w = cairo_image_surface_get_width (surface);
  int h = cairo_image_surface_get_height (surface);
  int stride = cairo_image_surface_get_stride (surface);
  const unsigned char *data = cairo_image_surface_get_data (surface);

  for (int y = 0; y < h; y++) {
    const uint32_t *row = (const uint32_t *) (data + (gsize) y * (gsize) stride);
    for (int x = 0; x < w; x++) {
      if (((row[x] >> 24) & 0xFFU) < 255U)
        return TRUE;
    }
  }
  return FALSE;
}

typedef struct {
  const guint8 *data;
  gsize size;
  gsize offset;
} PngReadCursor;

static cairo_status_t
cairo_read_from_bytes (void *closure, unsigned char *out, unsigned int length)
{
  PngReadCursor *cursor = closure;
  if (cursor->offset + length > cursor->size)
    return CAIRO_STATUS_READ_ERROR;
  memcpy (out, cursor->data + cursor->offset, length);
  cursor->offset += length;
  return CAIRO_STATUS_SUCCESS;
}

static cairo_status_t
cairo_write_to_byte_array (void *closure, const unsigned char *data, unsigned int length)
{
  GByteArray *arr = closure;
  g_byte_array_append (arr, data, length);
  return CAIRO_STATUS_SUCCESS;
}

PaintDocument *
paint_document_new_from_file (GFile *file, GError **error)
{
  g_return_val_if_fail (G_IS_FILE (file), NULL);

  g_autofree char *path = g_file_get_path (file);
  g_autofree char *lower = path ? g_ascii_strdown (path, -1) : NULL;
  cairo_surface_t *loaded = NULL;

  if (lower && g_str_has_suffix (lower, ".png")) {
    loaded = cairo_image_surface_create_from_png (path);
    if (cairo_surface_status (loaded) != CAIRO_STATUS_SUCCESS) {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to read PNG image: %s",
                   cairo_status_to_string (cairo_surface_status (loaded)));
      cairo_surface_destroy (loaded);
      return NULL;
    }
  } else {
    g_autoptr (GdkTexture) texture = gdk_texture_new_from_file (file, error);
    if (!texture)
      return NULL;
    g_autoptr (GBytes) png_bytes = gdk_texture_save_to_png_bytes (texture);
    gsize size = 0;
    const guint8 *ptr = g_bytes_get_data (png_bytes, &size);
    PngReadCursor cursor = { ptr, size, 0 };
    loaded = cairo_image_surface_create_from_png_stream (cairo_read_from_bytes, &cursor);
    if (cairo_surface_status (loaded) != CAIRO_STATUS_SUCCESS) {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to decode image surface");
      cairo_surface_destroy (loaded);
      return NULL;
    }
  }

  int w = cairo_image_surface_get_width (loaded);
  int h = cairo_image_surface_get_height (loaded);
  gboolean has_alpha = surface_has_transparency (loaded);
  PaintDocument *doc = has_alpha ? paint_document_new_transparent (w, h)
                                 : paint_document_new (w, h);
  cairo_t *cr = cairo_create (doc->surface);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface (cr, loaded, 0, 0);
  cairo_paint (cr);
  cairo_destroy (cr);
  cairo_surface_destroy (loaded);

  doc->file = g_object_ref (file);
  doc->modified = FALSE;
  mark_all_dirty (doc);
  return doc;
}

PaintDocument *
paint_document_new_from_draft (GFile *draft_file,
                               GFile *original_file,
                               gboolean modified,
                               GError **error)
{
  PaintDocument *doc = paint_document_new_from_file (draft_file, error);
  if (!doc)
    return NULL;
  g_set_object (&doc->file, original_file);
  doc->modified = modified;
  return doc;
}

void
paint_document_free (PaintDocument *doc)
{
  if (!doc)
    return;
  g_clear_pointer (&doc->active_stroke_undo, undo_step_free);
  g_clear_pointer (&doc->surface, cairo_surface_destroy);
  g_clear_object (&doc->file);
  g_clear_pointer (&doc->undo_stack, g_ptr_array_unref);
  g_clear_pointer (&doc->redo_stack, g_ptr_array_unref);
  g_free (doc);
}

int
paint_document_get_width (const PaintDocument *doc)
{
  return doc->width;
}

int
paint_document_get_height (const PaintDocument *doc)
{
  return doc->height;
}

cairo_surface_t *
paint_document_get_surface (const PaintDocument *doc)
{
  return doc->surface;
}

GFile *
paint_document_get_file (const PaintDocument *doc)
{
  return doc->file;
}

gboolean
paint_document_get_modified (const PaintDocument *doc)
{
  return doc->modified;
}

void
paint_document_set_modified (PaintDocument *doc, gboolean modified)
{
  doc->modified = modified;
}

gboolean
paint_document_has_transparent_background (const PaintDocument *doc)
{
  return doc && doc->background_color.alpha < 0.5f;
}

gboolean
paint_document_can_undo (const PaintDocument *doc)
{
  return doc->undo_stack->len > 0 || doc->active_stroke_undo != NULL;
}

gboolean
paint_document_can_redo (const PaintDocument *doc)
{
  return doc->redo_stack->len > 0;
}

static UndoStep *
apply_undo_step (PaintDocument *doc, const UndoStep *step)
{
  if (step->is_full) {
    UndoStep *reverse = undo_step_new_full (doc->surface, &doc->background_color);
    cairo_surface_destroy (doc->surface);
    doc->surface = restore_full_surface (step);
    doc->width = cairo_image_surface_get_width (doc->surface);
    doc->height = cairo_image_surface_get_height (doc->surface);
    doc->background_color = step->background_color;
    mark_all_dirty (doc);
    return reverse;
  }

  UndoStep *reverse = undo_step_new_tiled (doc->width, doc->height);
  for (guint i = 0; i < step->tiles->len; i++) {
    const UndoTile *t = g_ptr_array_index (step->tiles, i);
    g_ptr_array_add (reverse->tiles, capture_rect_tile (doc->surface, t->x, t->y, t->w, t->h));
    restore_rect_tile (doc->surface, t);
    mark_rect_dirty (doc, t->x, t->y, t->w, t->h);
  }
  cairo_surface_mark_dirty (doc->surface);
  return reverse;
}

gboolean
paint_document_undo (PaintDocument *doc)
{
  if (doc->active_stroke_undo) {
    push_undo_step (doc, doc->active_stroke_undo);
    doc->active_stroke_undo = NULL;
  }
  if (doc->undo_stack->len == 0)
    return FALSE;

  guint last = doc->undo_stack->len - 1;
  const UndoStep *step = g_ptr_array_index (doc->undo_stack, last);
  g_ptr_array_add (doc->redo_stack, apply_undo_step (doc, step));
  g_ptr_array_remove_index (doc->undo_stack, last);
  doc->modified = TRUE;
  return TRUE;
}

gboolean
paint_document_redo (PaintDocument *doc)
{
  if (doc->redo_stack->len == 0)
    return FALSE;

  guint last = doc->redo_stack->len - 1;
  const UndoStep *step = g_ptr_array_index (doc->redo_stack, last);
  g_ptr_array_add (doc->undo_stack, apply_undo_step (doc, step));
  g_ptr_array_remove_index (doc->redo_stack, last);
  doc->modified = TRUE;
  return TRUE;
}

static void
compute_stroke_bounds (double x0,
                       double y0,
                       double x1,
                       double y1,
                       double stroke_size,
                       int *rx,
                       int *ry,
                       int *rw,
                       int *rh)
{
  int pad = (int) ceil (stroke_size / 2.0) + 4;
  double min_x = MIN (x0, x1);
  double max_x = MAX (x0, x1);
  double min_y = MIN (y0, y1);
  double max_y = MAX (y0, y1);

  *rx = (int) floor (min_x) - pad;
  *ry = (int) floor (min_y) - pad;
  *rw = (int) ceil (max_x - min_x) + pad * 2 + 2;
  *rh = (int) ceil (max_y - min_y) + pad * 2 + 2;
}

static void
compute_tool_stroke_bounds (const PaintTool *tool,
                            const PaintToolSettings *settings,
                            double x0,
                            double y0,
                            double x1,
                            double y1,
                            int *rx,
                            int *ry,
                            int *rw,
                            int *rh)
{
  if (tool && tool->get_bounds) {
    double bx0, by0, bx1, by1;
    tool->get_bounds (tool, x0, y0, x1, y1, settings, &bx0, &by0, &bx1, &by1);
    *rx = (int) floor (bx0) - 4;
    *ry = (int) floor (by0) - 4;
    *rw = (int) ceil (bx1 - bx0) + 10;
    *rh = (int) ceil (by1 - by0) + 10;
    return;
  }

  double stroke_size = (tool && tool->uses_brush_size) ? settings->size : 1.0;
  compute_stroke_bounds (x0, y0, x1, y1, stroke_size, rx, ry, rw, rh);
}

void
paint_document_begin_stroke (PaintDocument *doc,
                             double x,
                             double y,
                             const PaintTool *tool,
                             PaintToolSettings *settings)
{
  if (doc->active_stroke_undo) {
    push_undo_step (doc, doc->active_stroke_undo);
    doc->active_stroke_undo = NULL;
  }

  doc->pre_stroke_modified = doc->modified;
  doc->stroke_start_x = x;
  doc->stroke_start_y = y;
  doc->prev_stroke_x = x;
  doc->prev_stroke_y = y;
  doc->prev2_stroke_x = x;
  doc->prev2_stroke_y = y;

  GdkRGBA prev_bg = settings->background_color;
  settings->background_color = doc->background_color;

  if (tool->draw_preview) {
    cairo_t *cr = cairo_create (doc->surface);
    tool->begin_stroke (tool, cr, x, y, settings);
    cairo_destroy (cr);
    settings->background_color = prev_bg;
    return;
  }

  if (tool->uses_brush_size) {
    int rx, ry, rw, rh;
    compute_stroke_bounds (x, y, x, y, settings->size, &rx, &ry, &rw, &rh);

    doc->active_stroke_undo = undo_step_new_tiled (doc->width, doc->height);
    undo_step_capture_rect (doc->active_stroke_undo, doc->surface, rx, ry, rw, rh);

    cairo_t *cr = cairo_create (doc->surface);
    tool->begin_stroke (tool, cr, x, y, settings);
    cairo_destroy (cr);
    mark_rect_dirty (doc, rx, ry, rw, rh);
  } else {
    push_undo_step (doc, undo_step_new_full (doc->surface, &doc->background_color));
    cairo_t *cr = cairo_create (doc->surface);
    tool->begin_stroke (tool, cr, x, y, settings);
    cairo_destroy (cr);
    mark_all_dirty (doc);
  }

  settings->background_color = prev_bg;
  doc->modified = TRUE;
}

void
paint_document_continue_stroke (PaintDocument *doc,
                                double x,
                                double y,
                                const PaintTool *tool,
                                PaintToolSettings *settings)
{
  GdkRGBA prev_bg = settings->background_color;
  settings->background_color = doc->background_color;

  if (tool->draw_preview) {
    cairo_t *cr = cairo_create (doc->surface);
    tool->continue_stroke (tool, cr, x, y, settings);
    cairo_destroy (cr);
    settings->background_color = prev_bg;
    return;
  }

  if (tool->uses_brush_size) {
    if (!doc->active_stroke_undo)
      doc->active_stroke_undo = undo_step_new_tiled (doc->width, doc->height);

    int pad = (int) ceil (settings->size / 2.0) + 4;
    double min_x = MIN (MIN (doc->prev2_stroke_x, doc->prev_stroke_x), x);
    double max_x = MAX (MAX (doc->prev2_stroke_x, doc->prev_stroke_x), x);
    double min_y = MIN (MIN (doc->prev2_stroke_y, doc->prev_stroke_y), y);
    double max_y = MAX (MAX (doc->prev2_stroke_y, doc->prev_stroke_y), y);

    int rx = (int) floor (min_x) - pad;
    int ry = (int) floor (min_y) - pad;
    int rw = (int) ceil (max_x - min_x) + pad * 2 + 2;
    int rh = (int) ceil (max_y - min_y) + pad * 2 + 2;

    undo_step_capture_rect (doc->active_stroke_undo, doc->surface, rx, ry, rw, rh);

    cairo_t *cr = cairo_create (doc->surface);
    tool->continue_stroke (tool, cr, x, y, settings);
    cairo_destroy (cr);
    mark_rect_dirty (doc, rx, ry, rw, rh);

    doc->prev2_stroke_x = doc->prev_stroke_x;
    doc->prev2_stroke_y = doc->prev_stroke_y;
    doc->prev_stroke_x = x;
    doc->prev_stroke_y = y;
  } else {
    cairo_t *cr = cairo_create (doc->surface);
    tool->continue_stroke (tool, cr, x, y, settings);
    cairo_destroy (cr);
  }

  settings->background_color = prev_bg;
  doc->modified = TRUE;
}

void
paint_document_end_stroke (PaintDocument *doc,
                           double x,
                           double y,
                           const PaintTool *tool,
                           PaintToolSettings *settings)
{
  GdkRGBA prev_bg = settings->background_color;
  settings->background_color = doc->background_color;

  if (tool->draw_preview) {
    int rx, ry, rw, rh;
    compute_tool_stroke_bounds (tool, settings, doc->stroke_start_x, doc->stroke_start_y,
                                x, y, &rx, &ry, &rw, &rh);

    UndoStep *step = undo_step_new_tiled (doc->width, doc->height);
    undo_step_capture_rect (step, doc->surface, rx, ry, rw, rh);

    cairo_t *cr = cairo_create (doc->surface);
    tool->end_stroke (tool, cr, x, y, settings);
    cairo_destroy (cr);
    mark_rect_dirty (doc, rx, ry, rw, rh);

    push_undo_step (doc, step);
    settings->background_color = prev_bg;
    doc->modified = TRUE;
    return;
  }

  if (tool->uses_brush_size) {
    int rx, ry, rw, rh;
    compute_stroke_bounds (doc->prev_stroke_x, doc->prev_stroke_y, x, y,
                           settings->size, &rx, &ry, &rw, &rh);

    if (doc->active_stroke_undo)
      undo_step_capture_rect (doc->active_stroke_undo, doc->surface, rx, ry, rw, rh);

    cairo_t *cr = cairo_create (doc->surface);
    tool->end_stroke (tool, cr, x, y, settings);
    cairo_destroy (cr);
    mark_rect_dirty (doc, rx, ry, rw, rh);

    if (doc->active_stroke_undo) {
      push_undo_step (doc, doc->active_stroke_undo);
      doc->active_stroke_undo = NULL;
    }
  } else {
    cairo_t *cr = cairo_create (doc->surface);
    tool->end_stroke (tool, cr, x, y, settings);
    cairo_destroy (cr);
  }

  settings->background_color = prev_bg;
  doc->modified = TRUE;
}

void
paint_document_cancel_stroke (PaintDocument *doc)
{
  if (!doc || !doc->active_stroke_undo)
    return;

  UndoStep *step = doc->active_stroke_undo;
  doc->active_stroke_undo = NULL;

  for (guint i = 0; i < step->tiles->len; i++) {
    const UndoTile *t = g_ptr_array_index (step->tiles, i);
    restore_rect_tile (doc->surface, t);
    mark_rect_dirty (doc, t->x, t->y, t->w, t->h);
  }
  cairo_surface_mark_dirty (doc->surface);
  undo_step_free (step);
  doc->modified = doc->pre_stroke_modified;
}

void
paint_document_clear (PaintDocument *doc)
{
  if (doc->active_stroke_undo) {
    push_undo_step (doc, doc->active_stroke_undo);
    doc->active_stroke_undo = NULL;
  }
  push_undo_step (doc, undo_step_new_full (doc->surface, &doc->background_color));
  cairo_t *cr = cairo_create (doc->surface);
  cairo_set_source_rgba (cr,
                         doc->background_color.red,
                         doc->background_color.green,
                         doc->background_color.blue,
                         doc->background_color.alpha);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_paint (cr);
  cairo_destroy (cr);
  doc->modified = TRUE;
  mark_all_dirty (doc);
}

void
paint_document_clear_transparent (PaintDocument *doc)
{
  if (doc->active_stroke_undo) {
    push_undo_step (doc, doc->active_stroke_undo);
    doc->active_stroke_undo = NULL;
  }
  push_undo_step (doc, undo_step_new_full (doc->surface, &doc->background_color));
  doc->background_color = (GdkRGBA) { 0.0f, 0.0f, 0.0f, 0.0f };
  cairo_t *cr = cairo_create (doc->surface);
  cairo_set_operator (cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint (cr);
  cairo_destroy (cr);
  doc->modified = TRUE;
  mark_all_dirty (doc);
}

void
paint_document_resize (PaintDocument *doc, int new_width, int new_height)
{
  int w = MAX (1, new_width);
  int h = MAX (1, new_height);
  if (w == doc->width && h == doc->height)
    return;

  if (doc->active_stroke_undo) {
    push_undo_step (doc, doc->active_stroke_undo);
    doc->active_stroke_undo = NULL;
  }
  push_undo_step (doc, undo_step_new_full (doc->surface, &doc->background_color));

  cairo_surface_t *new_surface = create_blank_surface (w, h, &doc->background_color);
  cairo_t *cr = cairo_create (new_surface);
  cairo_rectangle (cr, 0.0, 0.0, (double) MIN (w, doc->width), (double) MIN (h, doc->height));
  cairo_clip (cr);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface (cr, doc->surface, 0, 0);
  cairo_paint (cr);
  cairo_destroy (cr);

  cairo_surface_destroy (doc->surface);
  doc->surface = new_surface;
  doc->width = w;
  doc->height = h;
  doc->modified = TRUE;
  mark_all_dirty (doc);
}

static void
flush_active_stroke_undo (PaintDocument *doc)
{
  if (doc->active_stroke_undo) {
    push_undo_step (doc, doc->active_stroke_undo);
    doc->active_stroke_undo = NULL;
  }
}

void
paint_document_scale (PaintDocument *doc, int new_width, int new_height)
{
  int w = MAX (1, new_width);
  int h = MAX (1, new_height);
  if (w == doc->width && h == doc->height)
    return;

  flush_active_stroke_undo (doc);
  push_undo_step (doc, undo_step_new_full (doc->surface, &doc->background_color));

  cairo_surface_t *new_surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *cr = cairo_create (new_surface);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_scale (cr, (double) w / (double) doc->width, (double) h / (double) doc->height);
  cairo_set_source_surface (cr, doc->surface, 0.0, 0.0);
  cairo_pattern_set_filter (cairo_get_source (cr), CAIRO_FILTER_BEST);
  cairo_paint (cr);
  cairo_destroy (cr);

  cairo_surface_destroy (doc->surface);
  doc->surface = new_surface;
  doc->width = w;
  doc->height = h;
  doc->modified = TRUE;
  mark_all_dirty (doc);
}

void
paint_document_crop (PaintDocument *doc, int x, int y, int w, int h)
{
  int x0 = CLAMP (x, 0, doc->width);
  int y0 = CLAMP (y, 0, doc->height);
  int x1 = CLAMP (x + w, 0, doc->width);
  int y1 = CLAMP (y + h, 0, doc->height);
  int cw = x1 - x0;
  int ch = y1 - y0;
  if (cw <= 0 || ch <= 0)
    return;
  if (x0 == 0 && y0 == 0 && cw == doc->width && ch == doc->height)
    return;

  flush_active_stroke_undo (doc);
  push_undo_step (doc, undo_step_new_full (doc->surface, &doc->background_color));

  cairo_surface_t *new_surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, cw, ch);
  cairo_t *cr = cairo_create (new_surface);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface (cr, doc->surface, (double) -x0, (double) -y0);
  cairo_paint (cr);
  cairo_destroy (cr);

  cairo_surface_destroy (doc->surface);
  doc->surface = new_surface;
  doc->width = cw;
  doc->height = ch;
  doc->modified = TRUE;
  mark_all_dirty (doc);
}

cairo_surface_t *
paint_surface_rotate_90 (cairo_surface_t *src, gboolean clockwise)
{
  cairo_surface_flush (src);
  int w = cairo_image_surface_get_width (src);
  int h = cairo_image_surface_get_height (src);
  int s_stride = cairo_image_surface_get_stride (src);
  const uint32_t *s_px = (const uint32_t *) cairo_image_surface_get_data (src);
  int s_stride_px = s_stride / 4;

  cairo_surface_t *dst = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, h, w);
  int d_stride = cairo_image_surface_get_stride (dst);
  uint32_t *d_px = (uint32_t *) cairo_image_surface_get_data (dst);
  int d_stride_px = d_stride / 4;

  for (int y = 0; y < h; y++) {
    const uint32_t *srow = s_px + (gsize) y * (gsize) s_stride_px;
    for (int x = 0; x < w; x++) {
      int dx = clockwise ? (h - 1 - y) : y;
      int dy = clockwise ? x : (w - 1 - x);
      d_px[(gsize) dy * (gsize) d_stride_px + dx] = srow[x];
    }
  }
  cairo_surface_mark_dirty (dst);
  return dst;
}

cairo_surface_t *
paint_surface_flip (cairo_surface_t *src, gboolean horizontal)
{
  cairo_surface_flush (src);
  int w = cairo_image_surface_get_width (src);
  int h = cairo_image_surface_get_height (src);
  int s_stride = cairo_image_surface_get_stride (src);
  const uint32_t *s_px = (const uint32_t *) cairo_image_surface_get_data (src);
  int s_stride_px = s_stride / 4;

  cairo_surface_t *dst = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
  int d_stride = cairo_image_surface_get_stride (dst);
  uint32_t *d_px = (uint32_t *) cairo_image_surface_get_data (dst);
  int d_stride_px = d_stride / 4;

  for (int y = 0; y < h; y++) {
    int dy = horizontal ? y : (h - 1 - y);
    const uint32_t *srow = s_px + (gsize) y * (gsize) s_stride_px;
    uint32_t *drow = d_px + (gsize) dy * (gsize) d_stride_px;
    if (horizontal) {
      for (int x = 0; x < w; x++)
        drow[w - 1 - x] = srow[x];
    } else {
      memcpy (drow, srow, (gsize) w * 4);
    }
  }
  cairo_surface_mark_dirty (dst);
  return dst;
}

void
paint_document_rotate_90 (PaintDocument *doc, gboolean clockwise)
{
  flush_active_stroke_undo (doc);
  push_undo_step (doc, undo_step_new_full (doc->surface, &doc->background_color));

  cairo_surface_t *rot = paint_surface_rotate_90 (doc->surface, clockwise);
  cairo_surface_destroy (doc->surface);
  doc->surface = rot;
  doc->width = cairo_image_surface_get_width (rot);
  doc->height = cairo_image_surface_get_height (rot);
  doc->modified = TRUE;
  mark_all_dirty (doc);
}

void
paint_document_flip (PaintDocument *doc, gboolean horizontal)
{
  flush_active_stroke_undo (doc);
  push_undo_step (doc, undo_step_new_full (doc->surface, &doc->background_color));

  cairo_surface_t *flipped = paint_surface_flip (doc->surface, horizontal);
  cairo_surface_destroy (doc->surface);
  doc->surface = flipped;
  doc->modified = TRUE;
  mark_all_dirty (doc);
}

gboolean
paint_document_pick_color (const PaintDocument *doc,
                           double x,
                           double y,
                           GdkRGBA *out_color)
{
  if (!doc || !doc->surface || !out_color)
    return FALSE;

  int px = (int) floor (x);
  int py = (int) floor (y);
  if (px < 0 || px >= doc->width || py < 0 || py >= doc->height)
    return FALSE;

  cairo_surface_flush (doc->surface);
  int stride = cairo_image_surface_get_stride (doc->surface);
  const unsigned char *data = cairo_image_surface_get_data (doc->surface);
  uint32_t argb = ((const uint32_t *) (data + (gsize) py * (gsize) stride))[px];

  uint8_t a = (uint8_t) ((argb >> 24) & 0xFFU);
  uint8_t r = (uint8_t) ((argb >> 16) & 0xFFU);
  uint8_t g = (uint8_t) ((argb >> 8) & 0xFFU);
  uint8_t b = (uint8_t) (argb & 0xFFU);

  if (a == 0) {
    *out_color = (GdkRGBA) { 0.0f, 0.0f, 0.0f, 0.0f };
  } else {
    *out_color = (GdkRGBA) {
      CLAMP ((float) r / (float) a, 0.0f, 1.0f),
      CLAMP ((float) g / (float) a, 0.0f, 1.0f),
      CLAMP ((float) b / (float) a, 0.0f, 1.0f),
      (float) a / 255.0f
    };
  }
  return TRUE;
}

void
paint_document_shift_preview_start (PaintDocument *doc,
                                    PaintToolSettings *settings,
                                    double new_x0,
                                    double new_y0)
{
  if (!doc)
    return;
  doc->stroke_start_x = new_x0;
  doc->stroke_start_y = new_y0;
  if (settings && settings->history_points && settings->history_points->len > 0) {
    PaintPoint *pt = &g_array_index (settings->history_points, PaintPoint, 0);
    pt->x = new_x0;
    pt->y = new_y0;
  }
}

cairo_surface_t *
paint_document_copy_rect (const PaintDocument *doc, int x, int y, int w, int h)
{
  if (!doc || !doc->surface)
    return NULL;
  int x0 = CLAMP (x, 0, doc->width);
  int y0 = CLAMP (y, 0, doc->height);
  int x1 = CLAMP (x + w, 0, doc->width);
  int y1 = CLAMP (y + h, 0, doc->height);
  int cw = x1 - x0;
  int ch = y1 - y0;
  if (cw <= 0 || ch <= 0)
    return NULL;

  cairo_surface_t *copy = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, cw, ch);
  cairo_t *cr = cairo_create (copy);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface (cr, doc->surface, (double) -x0, (double) -y0);
  cairo_paint (cr);
  cairo_destroy (cr);
  return copy;
}

static void
fill_doc_rect_with_bg (PaintDocument *doc, int x0, int y0, int w, int h)
{
  cairo_t *cr = cairo_create (doc->surface);
  cairo_rectangle (cr, (double) x0, (double) y0, (double) w, (double) h);
  cairo_clip (cr);
  cairo_set_source_rgba (cr,
                         doc->background_color.red,
                         doc->background_color.green,
                         doc->background_color.blue,
                         doc->background_color.alpha);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_paint (cr);
  cairo_destroy (cr);
}

void
paint_document_clear_rect (PaintDocument *doc, int x, int y, int w, int h)
{
  int x0 = CLAMP (x, 0, doc->width);
  int y0 = CLAMP (y, 0, doc->height);
  int x1 = CLAMP (x + w, 0, doc->width);
  int y1 = CLAMP (y + h, 0, doc->height);
  if (x1 <= x0 || y1 <= y0)
    return;

  flush_active_stroke_undo (doc);
  UndoStep *step = undo_step_new_tiled (doc->width, doc->height);
  undo_step_capture_rect (step, doc->surface, x0, y0, x1 - x0, y1 - y0);
  fill_doc_rect_with_bg (doc, x0, y0, x1 - x0, y1 - y0);
  mark_rect_dirty (doc, x0, y0, x1 - x0, y1 - y0);
  push_undo_step (doc, step);
  doc->modified = TRUE;
}

void
paint_document_blit_surface (PaintDocument *doc, cairo_surface_t *src, int x, int y)
{
  paint_document_commit_move (doc, NULL, src, x, y);
}

void
paint_document_commit_move (PaintDocument *doc,
                            const cairo_rectangle_int_t *src_hole,
                            cairo_surface_t *floating,
                            int dst_x,
                            int dst_y)
{
  if (!doc || (!src_hole && !floating))
    return;

  flush_active_stroke_undo (doc);
  UndoStep *step = undo_step_new_tiled (doc->width, doc->height);

  int hx0 = 0, hy0 = 0, hw = 0, hh = 0;
  if (src_hole && src_hole->width > 0 && src_hole->height > 0) {
    hx0 = CLAMP (src_hole->x, 0, doc->width);
    hy0 = CLAMP (src_hole->y, 0, doc->height);
    int hx1 = CLAMP (src_hole->x + src_hole->width, 0, doc->width);
    int hy1 = CLAMP (src_hole->y + src_hole->height, 0, doc->height);
    hw = hx1 - hx0;
    hh = hy1 - hy0;
    if (hw > 0 && hh > 0)
      undo_step_capture_rect (step, doc->surface, hx0, hy0, hw, hh);
  }

  int dx0 = 0, dy0 = 0, dw = 0, dh = 0;
  if (floating) {
    int fw = cairo_image_surface_get_width (floating);
    int fh = cairo_image_surface_get_height (floating);
    dx0 = CLAMP (dst_x, 0, doc->width);
    dy0 = CLAMP (dst_y, 0, doc->height);
    int dx1 = CLAMP (dst_x + fw, 0, doc->width);
    int dy1 = CLAMP (dst_y + fh, 0, doc->height);
    dw = dx1 - dx0;
    dh = dy1 - dy0;
    if (dw > 0 && dh > 0)
      undo_step_capture_rect (step, doc->surface, dx0, dy0, dw, dh);
  }

  if (hw > 0 && hh > 0) {
    fill_doc_rect_with_bg (doc, hx0, hy0, hw, hh);
    mark_rect_dirty (doc, hx0, hy0, hw, hh);
  }
  if (floating && dw > 0 && dh > 0) {
    cairo_t *cr = cairo_create (doc->surface);
    cairo_rectangle (cr, (double) dx0, (double) dy0, (double) dw, (double) dh);
    cairo_clip (cr);
    cairo_set_operator (cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_surface (cr, floating, (double) dst_x, (double) dst_y);
    cairo_paint (cr);
    cairo_destroy (cr);
    mark_rect_dirty (doc, dx0, dy0, dw, dh);
  }

  push_undo_step (doc, step);
  doc->modified = TRUE;
}

static void
paint_render_text_layout (cairo_t *cr,
                          double x,
                          double y,
                          const char *text,
                          const PaintToolSettings *settings,
                          gboolean draw,
                          int *out_x,
                          int *out_y,
                          int *out_w,
                          int *out_h)
{
  PangoLayout *layout = pango_cairo_create_layout (cr);
  pango_layout_set_text (layout, text ? text : "", -1);

  PangoFontDescription *desc = pango_font_description_new ();
  const char *family = (settings->text_font_family && *settings->text_font_family)
                         ? settings->text_font_family
                         : "Sans";
  pango_font_description_set_family (desc, family);
  pango_font_description_set_weight (desc,
                                     settings->text_bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
  pango_font_description_set_style (desc,
                                    settings->text_italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
  double px_size = MAX (6.0, settings->text_font_size);
  pango_font_description_set_absolute_size (desc, px_size * PANGO_SCALE);
  pango_layout_set_font_description (layout, desc);
  pango_font_description_free (desc);

  PangoRectangle ink = { 0 };
  PangoRectangle logical = { 0 };
  pango_layout_get_pixel_extents (layout, &ink, &logical);
  int bx = MIN (ink.x, logical.x);
  int by = MIN (ink.y, logical.y);
  int bx1 = MAX (ink.x + ink.width, logical.x + logical.width);
  int by1 = MAX (ink.y + ink.height, logical.y + logical.height);

  if (out_x)
    *out_x = (int) floor (x) + bx;
  if (out_y)
    *out_y = (int) floor (y) + by;
  if (out_w)
    *out_w = MAX (24, bx1 - bx);
  if (out_h)
    *out_h = MAX ((int) ceil (px_size * 1.2), by1 - by);

  if (draw && text && *text) {
    cairo_save (cr);
    cairo_set_source_rgba (cr,
                           settings->color.red,
                           settings->color.green,
                           settings->color.blue,
                           settings->color.alpha);
    cairo_set_operator (cr, CAIRO_OPERATOR_OVER);
    cairo_move_to (cr, x, y);
    pango_cairo_show_layout (cr, layout);
    cairo_restore (cr);
  }
  g_object_unref (layout);
}

void
paint_document_draw_text (PaintDocument *doc,
                          double x,
                          double y,
                          const char *text,
                          const PaintToolSettings *settings)
{
  if (!doc || !settings || !text || !*text)
    return;

  flush_active_stroke_undo (doc);
  int bx, by, bw, bh;
  cairo_t *cr = cairo_create (doc->surface);
  paint_render_text_layout (cr, x, y, text, settings, FALSE, &bx, &by, &bw, &bh);
  cairo_destroy (cr);

  int rx = bx - 6;
  int ry = by - 6;
  int rw = bw + 12;
  int rh = bh + 12;
  UndoStep *step = undo_step_new_tiled (doc->width, doc->height);
  undo_step_capture_rect (step, doc->surface, rx, ry, rw, rh);

  cr = cairo_create (doc->surface);
  paint_render_text_layout (cr, x, y, text, settings, TRUE, NULL, NULL, NULL, NULL);
  cairo_destroy (cr);

  mark_rect_dirty (doc, rx, ry, rw, rh);
  push_undo_step (doc, step);
  doc->modified = TRUE;
}

cairo_surface_t *
paint_surface_from_texture (GdkTexture *texture)
{
  if (!texture)
    return NULL;
  int w = MAX (1, gdk_texture_get_width (texture));
  int h = MAX (1, gdk_texture_get_height (texture));
  cairo_surface_t *surf = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
  int stride = cairo_image_surface_get_stride (surf);
  unsigned char *data = cairo_image_surface_get_data (surf);
  gdk_texture_download (texture, data, (gsize) stride);
  cairo_surface_mark_dirty (surf);
  return surf;
}

GdkTexture *
paint_texture_from_surface (cairo_surface_t *surface)
{
  if (!surface)
    return NULL;
  cairo_surface_flush (surface);
  int w = cairo_image_surface_get_width (surface);
  int h = cairo_image_surface_get_height (surface);
  int stride = cairo_image_surface_get_stride (surface);
  const unsigned char *data = cairo_image_surface_get_data (surface);
  g_autoptr (GBytes) bytes = g_bytes_new (data, (gsize) stride * (gsize) h);
  return gdk_memory_texture_new (w, h, GDK_MEMORY_DEFAULT, bytes, (gsize) stride);
}

PaintDocument *
paint_document_new_from_texture (GdkTexture *texture)
{
  if (!texture)
    return NULL;
  cairo_surface_t *loaded = paint_surface_from_texture (texture);
  int w = cairo_image_surface_get_width (loaded);
  int h = cairo_image_surface_get_height (loaded);
  gboolean has_alpha = surface_has_transparency (loaded);
  PaintDocument *doc = has_alpha ? paint_document_new_transparent (w, h)
                                 : paint_document_new (w, h);
  cairo_t *cr = cairo_create (doc->surface);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface (cr, loaded, 0.0, 0.0);
  cairo_paint (cr);
  cairo_destroy (cr);
  cairo_surface_destroy (loaded);

  doc->modified = TRUE;
  mark_all_dirty (doc);
  return doc;
}

gboolean
paint_document_save_to_file (PaintDocument *doc, GFile *file, GError **error)
{
  g_return_val_if_fail (doc != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  g_autofree char *path = g_file_get_path (file);
  if (!path) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                 "Saving to non-local URIs is not supported yet.");
    return FALSE;
  }

  g_autofree char *lower = g_ascii_strdown (path, -1);
  g_autoptr (GFile) target_file = NULL;
  g_autofree char *target_path = NULL;

  const char *ext = strrchr (lower, '.');
  if (!ext || g_strcmp0 (ext, ".png") == 0) {
    if (!ext) {
      target_path = g_strconcat (path, ".png", NULL);
      target_file = g_file_new_for_path (target_path);
    } else {
      target_path = g_strdup (path);
      target_file = g_object_ref (file);
    }
    cairo_status_t status = cairo_surface_write_to_png (doc->surface, target_path);
    if (status != CAIRO_STATUS_SUCCESS) {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to save PNG: %s", cairo_status_to_string (status));
      return FALSE;
    }
  } else {
    target_file = g_object_ref (file);
    gboolean is_jpeg = (g_strcmp0 (ext, ".jpg") == 0 || g_strcmp0 (ext, ".jpeg") == 0);
    cairo_surface_t *export_surface = doc->surface;
    cairo_surface_t *flattened = NULL;
    if (is_jpeg) {
      GdkRGBA white = { 1.0f, 1.0f, 1.0f, 1.0f };
      flattened = create_blank_surface (doc->width, doc->height, &white);
      cairo_t *fcr = cairo_create (flattened);
      cairo_set_source_surface (fcr, doc->surface, 0, 0);
      cairo_paint (fcr);
      cairo_destroy (fcr);
      export_surface = flattened;
    }

    GByteArray *arr = g_byte_array_new ();
    cairo_status_t status = cairo_surface_write_to_png_stream (export_surface,
                                                               cairo_write_to_byte_array,
                                                               arr);
    g_clear_pointer (&flattened, cairo_surface_destroy);
    if (status != CAIRO_STATUS_SUCCESS) {
      g_byte_array_unref (arr);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to encode image");
      return FALSE;
    }

    g_autoptr (GBytes) bytes = g_byte_array_free_to_bytes (arr);
    g_autoptr (GInputStream) stream = g_memory_input_stream_new_from_bytes (bytes);
    g_autoptr (GdkPixbuf) pixbuf = gdk_pixbuf_new_from_stream (stream, NULL, error);
    if (!pixbuf)
      return FALSE;

    const char *fmt = is_jpeg ? "jpeg" : ext + 1;
    if (!gdk_pixbuf_save (pixbuf, path, fmt, error, NULL))
      return FALSE;
  }

  g_set_object (&doc->file, target_file);
  doc->modified = FALSE;
  return TRUE;
}

/* ============================================================================
 * PaintCanvas (2-Tier Single-Texture Cache: Overview < 50% + 1:1 Viewport >= 50%)
 * ============================================================================ */

enum {
  SIGNAL_CHANGED,
  SIGNAL_ZOOM_CHANGED,
  SIGNAL_COLOR_PICKED,
  SIGNAL_SELECTION_CHANGED,
  SIGNAL_TEXT_STATE_CHANGED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _PaintCanvas {
  GtkWidget parent_instance;

  PaintDocument *document;
  PaintToolSettings *settings; /* unowned shared settings */

  /* Tier 1 (>= 50% zoom, or >= 30% half-res on >4096 canvases): overscanned viewport texture */
  cairo_surface_t *vp_surface;
  GdkTexture *vp_texture;
  cairo_surface_t *vp_doc_surface;
  gboolean vp_valid;
  int vp_div;
  int vp_x;
  int vp_y;
  int vp_w;
  int vp_h;
  int vp_surf_w;
  int vp_surf_h;
  gboolean vp_all_dirty;
  gboolean vp_has_dirty_rect;
  cairo_rectangle_int_t vp_dirty_rect;

  /* Tier 2 (overview): box-downsampled <= 2048x2048 overview texture */
  cairo_surface_t *ov_surface;
  GdkTexture *ov_texture;
  cairo_surface_t *ov_doc_surface;
  int ov_div;
  int ov_w;
  int ov_h;
  gboolean ov_all_dirty;
  gboolean ov_has_dirty_rect;
  cairo_rectangle_int_t ov_dirty_rect;

  /* Viewport-sized cached checkerboard texture for transparent regions */
  GdkTexture *checker_texture;
  int checker_w;
  int checker_h;

  double zoom;
  gboolean has_hover;
  double hover_x;
  double hover_y;
  gboolean drawing_active;
  gboolean touch_stroke_pending;
  double drag_start_x;
  double drag_start_y;
  double last_drag_off_x;
  double last_drag_off_y;
  double preview_end_x;
  double preview_end_y;
  gboolean show_cursor_ring;
  gboolean show_pixel_grid;

  /* Space-reposition state for previewed shapes/lines */
  gboolean space_held;
  double shape_shift_x;
  double shape_shift_y;
  double prev_raw_cx;
  double prev_raw_cy;

  /* Eyedropper state */
  gboolean eyedropper_active;
  gboolean alt_held;
  gboolean pick_badge_active;
  GdkRGBA pick_badge_color;
  guint pick_badge_timeout_id;

  /* Selection & floating layer state */
  gboolean has_selection;
  cairo_rectangle_int_t sel_rect;
  gboolean sel_dragging_new;
  gboolean sel_moving;
  double sel_move_anchor_x;
  double sel_move_anchor_y;
  int sel_move_orig_x;
  int sel_move_orig_y;
  cairo_surface_t *floating_surface;
  GdkTexture *floating_texture;
  gboolean floating_has_hole;
  cairo_rectangle_int_t floating_hole;

  /* Interactive text placement state */
  gboolean text_active;
  double text_x;
  double text_y;
  char *text_content;
  int text_box_x;
  int text_box_y;
  int text_box_w;
  int text_box_h;
  gboolean text_dragging;
  double text_drag_anchor_x;
  double text_drag_anchor_y;
  double text_drag_orig_x;
  double text_drag_orig_y;

  const char *cursor_name;
};

G_DEFINE_FINAL_TYPE (PaintCanvas, paint_canvas, GTK_TYPE_WIDGET)

static void
invalidate_viewport_cache (PaintCanvas *self)
{
  self->vp_valid = FALSE;
  self->vp_doc_surface = NULL;
  self->vp_div = 0;
  self->vp_surf_w = 0;
  self->vp_surf_h = 0;
  self->vp_all_dirty = FALSE;
  self->vp_has_dirty_rect = FALSE;
  g_clear_object (&self->vp_texture);

  self->ov_doc_surface = NULL;
  self->ov_all_dirty = FALSE;
  self->ov_has_dirty_rect = FALSE;
  g_clear_object (&self->ov_texture);
}

static void
expand_dirty_rect (gboolean *all_dirty,
                   gboolean *has_rect,
                   cairo_rectangle_int_t *accum,
                   const cairo_rectangle_int_t *r)
{
  if (*all_dirty)
    return;
  if (!*has_rect) {
    *accum = *r;
    *has_rect = TRUE;
  } else {
    int x0 = MIN (accum->x, r->x);
    int y0 = MIN (accum->y, r->y);
    int x1 = MAX (accum->x + accum->width, r->x + r->width);
    int y1 = MAX (accum->y + accum->height, r->y + r->height);
    *accum = (cairo_rectangle_int_t) { x0, y0, x1 - x0, y1 - y0 };
  }
}

static void
propagate_document_dirty (PaintCanvas *self)
{
  PaintDocument *doc = self->document;
  if (!doc)
    return;

  if (doc->all_dirty) {
    self->vp_all_dirty = TRUE;
    self->vp_has_dirty_rect = FALSE;
    self->ov_all_dirty = TRUE;
    self->ov_has_dirty_rect = FALSE;
    doc->all_dirty = FALSE;
    doc->has_dirty_rect = FALSE;
  } else if (doc->has_dirty_rect) {
    expand_dirty_rect (&self->vp_all_dirty, &self->vp_has_dirty_rect,
                       &self->vp_dirty_rect, &doc->dirty_rect);
    expand_dirty_rect (&self->ov_all_dirty, &self->ov_has_dirty_rect,
                       &self->ov_dirty_rect, &doc->dirty_rect);
    doc->has_dirty_rect = FALSE;
  }
}

static GdkTexture *
upload_surface_texture (cairo_surface_t *surface,
                        GdkTexture *old_texture,
                        int w,
                        int h,
                        const cairo_rectangle_int_t *update_rect)
{
  cairo_surface_flush (surface);
  int stride = cairo_image_surface_get_stride (surface);
  const unsigned char *data = cairo_image_surface_get_data (surface);
  g_autoptr (GBytes) bytes = g_bytes_new (data, (gsize) stride * (gsize) h);

#if GTK_CHECK_VERSION (4, 16, 0)
  if (update_rect && old_texture &&
      update_rect->width > 0 && update_rect->height > 0) {
    cairo_region_t *region = cairo_region_create_rectangle (update_rect);
    g_autoptr (GdkMemoryTextureBuilder) builder = gdk_memory_texture_builder_new ();
    gdk_memory_texture_builder_set_width (builder, w);
    gdk_memory_texture_builder_set_height (builder, h);
    gdk_memory_texture_builder_set_format (builder, GDK_MEMORY_DEFAULT);
    gdk_memory_texture_builder_set_stride (builder, (gsize) stride);
    gdk_memory_texture_builder_set_bytes (builder, bytes);
    gdk_memory_texture_builder_set_update_texture (builder, old_texture);
    gdk_memory_texture_builder_set_update_region (builder, region);

    GdkTexture *updated = gdk_memory_texture_builder_build (builder);
    cairo_region_destroy (region);
    return updated;
  }
#else
  (void) update_rect;
  (void) old_texture;
#endif

  return gdk_memory_texture_new (w, h, GDK_MEMORY_DEFAULT, bytes, (gsize) stride);
}

static graphene_rect_t
compute_visible_widget_rect (PaintCanvas *self)
{
  float draw_w = (float) gtk_widget_get_width (GTK_WIDGET (self));
  float draw_h = (float) gtk_widget_get_height (GTK_WIDGET (self));
  graphene_rect_t vis = GRAPHENE_RECT_INIT (0.0f, 0.0f, draw_w, draw_h);
  if (draw_w <= 0.0f || draw_h <= 0.0f)
    return vis;

  GtkWidget *scrolled = gtk_widget_get_ancestor (GTK_WIDGET (self), GTK_TYPE_SCROLLED_WINDOW);
  if (scrolled) {
    graphene_rect_t computed;
    if (gtk_widget_compute_bounds (scrolled, GTK_WIDGET (self), &computed))
      graphene_rect_intersection (&vis, &computed, &vis);
  }
  return vis;
}

static void box_downsample_rect (cairo_surface_t *src_surf,
                                 cairo_surface_t *dst_surf,
                                 int div,
                                 int src_ox,
                                 int src_oy,
                                 int dx0,
                                 int dy0,
                                 int dw,
                                 int dh);

static int
target_vp_div (const PaintCanvas *self)
{
  if (!self->document)
    return 0;
  if (self->zoom >= 0.50)
    return 1;
  int max_dim = MAX (self->document->width, self->document->height);
  if (self->zoom >= 0.30 && max_dim > MAX_OVERVIEW_DIM * 2)
    return 2;
  return 0;
}

static gboolean
is_visible_covered_by_vp_cache (PaintCanvas *self, const graphene_rect_t *vis)
{
  int want_div = target_vp_div (self);
  if (want_div <= 0 || !self->vp_valid || !self->vp_texture || !self->vp_surface ||
      !self->document || self->vp_doc_surface != self->document->surface ||
      self->vp_div != want_div) {
    return FALSE;
  }

  int req_x0 = MAX (0, (int) floor (vis->origin.x / self->zoom));
  int req_y0 = MAX (0, (int) floor (vis->origin.y / self->zoom));
  int req_x1 = MIN (self->document->width,
                    (int) ceil ((vis->origin.x + vis->size.width) / self->zoom));
  int req_y1 = MIN (self->document->height,
                    (int) ceil ((vis->origin.y + vis->size.height) / self->zoom));

  return req_x0 >= self->vp_x &&
         req_y0 >= self->vp_y &&
         req_x1 <= self->vp_x + self->vp_w &&
         req_y1 <= self->vp_y + self->vp_h;
}

static void
rebuild_vp_cache (PaintCanvas *self, const graphene_rect_t *vis)
{
  PaintDocument *doc = self->document;
  int div = target_vp_div (self);
  if (div <= 0)
    return;

  int pad = CLAMP ((int) ceil ((double) VIEWPORT_OVERSCAN / self->zoom), 64, 256);
  int req_x0 = MAX (0, (int) floor (vis->origin.x / self->zoom) - pad);
  int req_y0 = MAX (0, (int) floor (vis->origin.y / self->zoom) - pad);
  int req_x1 = MIN (doc->width, (int) ceil ((vis->origin.x + vis->size.width) / self->zoom) + pad);
  int req_y1 = MIN (doc->height, (int) ceil ((vis->origin.y + vis->size.height) / self->zoom) + pad);

  req_x0 = (req_x0 / div) * div;
  req_y0 = (req_y0 / div) * div;

  /* Ensure a minimum cache span so zooming 100% <-> 3200% doesn't re-copy */
  int min_w = MIN (doc->width, MIN_VP_CACHE_DIM * div);
  int min_h = MIN (doc->height, MIN_VP_CACHE_DIM * div);
  if (req_x1 - req_x0 < min_w) {
    int cx = (req_x0 + req_x1) / 2;
    int max_x0 = MAX (0, ((doc->width - min_w) / div) * div);
    req_x0 = CLAMP (((cx - min_w / 2) / div) * div, 0, max_x0);
    req_x1 = MIN (doc->width, req_x0 + min_w);
  }
  if (req_y1 - req_y0 < min_h) {
    int cy = (req_y0 + req_y1) / 2;
    int max_y0 = MAX (0, ((doc->height - min_h) / div) * div);
    req_y0 = CLAMP (((cy - min_h / 2) / div) * div, 0, max_y0);
    req_y1 = MIN (doc->height, req_y0 + min_h);
  }

  int cw = MAX (1, req_x1 - req_x0);
  int ch = MAX (1, req_y1 - req_y0);
  int sw = MAX (1, (cw + div - 1) / div);
  int sh = MAX (1, (ch + div - 1) / div);

  if (!self->vp_surface ||
      cairo_image_surface_get_width (self->vp_surface) != sw ||
      cairo_image_surface_get_height (self->vp_surface) != sh) {
    g_clear_pointer (&self->vp_surface, cairo_surface_destroy);
    self->vp_surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, sw, sh);
    g_clear_object (&self->vp_texture);
  }

  self->vp_div = div;
  self->vp_x = req_x0;
  self->vp_y = req_y0;
  self->vp_w = cw;
  self->vp_h = ch;
  self->vp_surf_w = sw;
  self->vp_surf_h = sh;
  self->vp_doc_surface = doc->surface;
  self->vp_valid = TRUE;
  self->vp_all_dirty = FALSE;
  self->vp_has_dirty_rect = FALSE;

  box_downsample_rect (doc->surface, self->vp_surface, div, req_x0, req_y0, 0, 0, sw, sh);
  GdkTexture *tex = upload_surface_texture (self->vp_surface, NULL, sw, sh, NULL);
  g_clear_object (&self->vp_texture);
  self->vp_texture = tex;
}

static GdkTexture *
ensure_vp_texture (PaintCanvas *self, const graphene_rect_t *vis)
{
  if (!is_visible_covered_by_vp_cache (self, vis)) {
    rebuild_vp_cache (self, vis);
    return self->vp_texture;
  }

  int div = self->vp_div;
  if (self->vp_all_dirty) {
    self->vp_all_dirty = FALSE;
    self->vp_has_dirty_rect = FALSE;
    box_downsample_rect (self->document->surface, self->vp_surface, div,
                         self->vp_x, self->vp_y, 0, 0, self->vp_surf_w, self->vp_surf_h);
    GdkTexture *tex = upload_surface_texture (
        self->vp_surface, NULL, self->vp_surf_w, self->vp_surf_h, NULL);
    g_clear_object (&self->vp_texture);
    self->vp_texture = tex;
    return self->vp_texture;
  }

  if (self->vp_has_dirty_rect) {
    cairo_rectangle_int_t d = self->vp_dirty_rect;
    self->vp_has_dirty_rect = FALSE;

    int cx0 = MAX (self->vp_x, d.x);
    int cy0 = MAX (self->vp_y, d.y);
    int cx1 = MIN (self->vp_x + self->vp_w, d.x + d.width);
    int cy1 = MIN (self->vp_y + self->vp_h, d.y + d.height);

    if (cx0 < cx1 && cy0 < cy1) {
      int vx0 = (cx0 - self->vp_x) / div;
      int vy0 = (cy0 - self->vp_y) / div;
      int vx1 = MIN (self->vp_surf_w, (cx1 - self->vp_x + div - 1) / div);
      int vy1 = MIN (self->vp_surf_h, (cy1 - self->vp_y + div - 1) / div);

      if (vx0 < vx1 && vy0 < vy1) {
        cairo_rectangle_int_t upd = { vx0, vy0, vx1 - vx0, vy1 - vy0 };
        box_downsample_rect (self->document->surface, self->vp_surface, div,
                             self->vp_x, self->vp_y, upd.x, upd.y, upd.width, upd.height);
        GdkTexture *next = upload_surface_texture (
            self->vp_surface, self->vp_texture, self->vp_surf_w, self->vp_surf_h, &upd);
        g_set_object (&self->vp_texture, next);
        g_object_unref (next);
      }
    }
  }

  return self->vp_texture;
}

static uint32_t
downsample_clamped_pixel (const uint32_t *src_px,
                          int src_stride_px,
                          int sx0,
                          int sx1,
                          int sy0,
                          int sy1)
{
  uint32_t sum_a = 0, sum_r = 0, sum_g = 0, sum_b = 0;
  uint32_t count = 0;
  for (int sy = sy0; sy < sy1; sy++) {
    const uint32_t *srow = src_px + (gsize) sy * (gsize) src_stride_px;
    for (int sx = sx0; sx < sx1; sx++) {
      uint32_t px = srow[sx];
      sum_a += (px >> 24) & 0xFFU;
      sum_r += (px >> 16) & 0xFFU;
      sum_g += (px >> 8) & 0xFFU;
      sum_b += px & 0xFFU;
      count++;
    }
  }
  if (count == 0)
    return 0;
  uint32_t half = count >> 1;
  return (((sum_a + half) / count) << 24) |
         (((sum_r + half) / count) << 16) |
         (((sum_g + half) / count) << 8) |
         ((sum_b + half) / count);
}

static void
box_downsample_rect (cairo_surface_t *src_surf,
                     cairo_surface_t *dst_surf,
                     int div,
                     int src_ox,
                     int src_oy,
                     int dx0,
                     int dy0,
                     int dw,
                     int dh)
{
  cairo_surface_flush (src_surf);
  int src_w = cairo_image_surface_get_width (src_surf);
  int src_h = cairo_image_surface_get_height (src_surf);
  int src_stride = cairo_image_surface_get_stride (src_surf);
  const unsigned char *src_data = cairo_image_surface_get_data (src_surf);

  int dst_stride = cairo_image_surface_get_stride (dst_surf);
  unsigned char *dst_data = cairo_image_surface_get_data (dst_surf);

  if (div == 1) {
    gsize row_bytes = (gsize) dw * 4;
    int sx = src_ox + dx0;
    for (int row = 0; row < dh; row++) {
      int sy = src_oy + dy0 + row;
      const unsigned char *srow = src_data + (gsize) sy * (gsize) src_stride + (gsize) sx * 4;
      unsigned char *drow = dst_data + (gsize) (dy0 + row) * (gsize) dst_stride + (gsize) dx0 * 4;
      memcpy (drow, srow, row_bytes);
    }
    cairo_surface_mark_dirty_rectangle (dst_surf, dx0, dy0, dw, dh);
    return;
  }

  const uint32_t *src_px = (const uint32_t *) src_data;
  uint32_t *dst_px = (uint32_t *) dst_data;
  int src_stride_px = src_stride / 4;
  int dst_stride_px = dst_stride / 4;

  /* Safe for 16-bit lane SWAR accumulation up to div=8 (max 64 * 255 = 16320 < 65535) */
  int shift = (div == 2) ? 2 : (div == 4) ? 4 : (div == 8) ? 6 : 0;
  int full_x_end = MAX (0, (src_w - src_ox) / div);
  int full_y_end = MAX (0, (src_h - src_oy) / div);

  for (int dy = dy0; dy < dy0 + dh; dy++) {
    int sy0 = src_oy + dy * div;
    int sy1 = MIN (src_h, sy0 + div);
    uint32_t *drow = dst_px + (gsize) dy * (gsize) dst_stride_px;
    gboolean full_y = (shift > 0) && (dy < full_y_end);
    int dx = dx0;
    int fast_end = full_y ? MIN (dx0 + dw, full_x_end) : dx0;

    if (div == 4 && full_y) {
      const uint32_t *r0 = src_px + (gsize) sy0 * (gsize) src_stride_px;
      const uint32_t *r1 = r0 + src_stride_px;
      const uint32_t *r2 = r1 + src_stride_px;
      const uint32_t *r3 = r2 + src_stride_px;
      for (; dx < fast_end; dx++) {
        int sx0 = src_ox + dx * 4;
        uint32_t rb = 0x00080008U;
        uint32_t ag = 0x00080008U;
        for (int k = 0; k < 4; k++) {
          uint32_t p0 = r0[sx0 + k], p1 = r1[sx0 + k], p2 = r2[sx0 + k], p3 = r3[sx0 + k];
          rb += (p0 & 0x00FF00FFU) + (p1 & 0x00FF00FFU) + (p2 & 0x00FF00FFU) + (p3 & 0x00FF00FFU);
          ag += ((p0 >> 8) & 0x00FF00FFU) + ((p1 >> 8) & 0x00FF00FFU) +
                ((p2 >> 8) & 0x00FF00FFU) + ((p3 >> 8) & 0x00FF00FFU);
        }
        drow[dx] = ((rb >> 4) & 0x00FF00FFU) | (((ag >> 4) & 0x00FF00FFU) << 8);
      }
    } else if (shift > 0 && full_y) {
      uint32_t bias = 0x00010001U * (1U << (shift - 1));
      for (; dx < fast_end; dx++) {
        int sx0 = src_ox + dx * div;
        uint32_t rb = bias;
        uint32_t ag = bias;
        const uint32_t *row = src_px + (gsize) sy0 * (gsize) src_stride_px + sx0;
        for (int ky = 0; ky < div; ky++, row += src_stride_px) {
          for (int kx = 0; kx < div; kx++) {
            uint32_t p = row[kx];
            rb += p & 0x00FF00FFU;
            ag += (p >> 8) & 0x00FF00FFU;
          }
        }
        drow[dx] = ((rb >> shift) & 0x00FF00FFU) | (((ag >> shift) & 0x00FF00FFU) << 8);
      }
    }

    for (; dx < dx0 + dw; dx++) {
      int sx0 = src_ox + dx * div;
      int sx1 = MIN (src_w, sx0 + div);
      drow[dx] = downsample_clamped_pixel (src_px, src_stride_px, sx0, sx1, sy0, sy1);
    }
  }
  cairo_surface_mark_dirty_rectangle (dst_surf, dx0, dy0, dw, dh);
}

static double
get_widget_device_scale (GtkWidget *widget)
{
  GtkNative *native = gtk_widget_get_native (widget);
  if (native) {
    GdkSurface *surface = gtk_native_get_surface (native);
    if (surface) {
      double s = gdk_surface_get_scale (surface);
      if (s > 0.0)
        return s;
    }
  }
  int sf = gtk_widget_get_scale_factor (widget);
  return sf > 0 ? (double) sf : 1.0;
}

static GdkTexture *
ensure_overview_texture (PaintCanvas *self, double device_scale)
{
  PaintDocument *doc = self->document;
  if (!doc || !doc->surface)
    return NULL;

  int max_ov_dim = (device_scale > 1.25) ? MAX_OVERVIEW_DIM * 2 : MAX_OVERVIEW_DIM;
  int max_dim = MAX (doc->width, doc->height);
  int div = 1;
  while (max_dim / div > max_ov_dim)
    div *= 2;
  int ow = MAX (1, (doc->width + div - 1) / div);
  int oh = MAX (1, (doc->height + div - 1) / div);

  if (!self->ov_texture || !self->ov_surface ||
      self->ov_doc_surface != doc->surface ||
      self->ov_div != div || self->ov_w != ow || self->ov_h != oh) {
    if (!self->ov_surface ||
        cairo_image_surface_get_width (self->ov_surface) != ow ||
        cairo_image_surface_get_height (self->ov_surface) != oh) {
      g_clear_pointer (&self->ov_surface, cairo_surface_destroy);
      self->ov_surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, ow, oh);
    }
    self->ov_div = div;
    self->ov_w = ow;
    self->ov_h = oh;
    self->ov_doc_surface = doc->surface;
    self->ov_all_dirty = FALSE;
    self->ov_has_dirty_rect = FALSE;

    box_downsample_rect (doc->surface, self->ov_surface, div, 0, 0, 0, 0, ow, oh);
    GdkTexture *tex = upload_surface_texture (self->ov_surface, NULL, ow, oh, NULL);
    g_clear_object (&self->ov_texture);
    self->ov_texture = tex;
    return self->ov_texture;
  }

  if (self->ov_all_dirty) {
    self->ov_all_dirty = FALSE;
    self->ov_has_dirty_rect = FALSE;
    box_downsample_rect (doc->surface, self->ov_surface, div, 0, 0, 0, 0, ow, oh);
    GdkTexture *tex = upload_surface_texture (self->ov_surface, NULL, ow, oh, NULL);
    g_clear_object (&self->ov_texture);
    self->ov_texture = tex;
    return self->ov_texture;
  }

  if (self->ov_has_dirty_rect) {
    cairo_rectangle_int_t d = self->ov_dirty_rect;
    self->ov_has_dirty_rect = FALSE;

    int dx0 = CLAMP (d.x / div, 0, ow - 1);
    int dy0 = CLAMP (d.y / div, 0, oh - 1);
    int dx1 = CLAMP ((d.x + d.width + div - 1) / div, dx0 + 1, ow);
    int dy1 = CLAMP ((d.y + d.height + div - 1) / div, dy0 + 1, oh);

    cairo_rectangle_int_t upd = { dx0, dy0, dx1 - dx0, dy1 - dy0 };
    box_downsample_rect (doc->surface, self->ov_surface, div, 0, 0,
                         upd.x, upd.y, upd.width, upd.height);
    GdkTexture *next = upload_surface_texture (
        self->ov_surface, self->ov_texture, ow, oh, &upd);
    g_set_object (&self->ov_texture, next);
    g_object_unref (next);
  }

  return self->ov_texture;
}

static void
update_size_request (PaintCanvas *self)
{
  if (!self->document)
    return;
  int w = MAX (1, (int) lround (self->document->width * self->zoom));
  int h = MAX (1, (int) lround (self->document->height * self->zoom));
  gtk_widget_set_size_request (GTK_WIDGET (self), w, h);

  if (w > 4096 || h > 4096)
    gtk_widget_remove_css_class (GTK_WIDGET (self), "paint-canvas-shadow");
  else
    gtk_widget_add_css_class (GTK_WIDGET (self), "paint-canvas-shadow");
}

static void
paint_canvas_measure (GtkWidget *widget,
                      GtkOrientation orientation,
                      int for_size G_GNUC_UNUSED,
                      int *minimum,
                      int *natural,
                      int *minimum_baseline,
                      int *natural_baseline)
{
  PaintCanvas *self = PAINT_CANVAS (widget);
  int size = 1;
  if (self->document) {
    double dim = (orientation == GTK_ORIENTATION_HORIZONTAL)
                   ? self->document->width
                   : self->document->height;
    size = MAX (1, (int) lround (dim * self->zoom));
  }
  *minimum = size;
  *natural = size;
  if (minimum_baseline)
    *minimum_baseline = -1;
  if (natural_baseline)
    *natural_baseline = -1;
}

static GdkTexture *
ensure_checker_texture (PaintCanvas *self, int req_w, int req_h)
{
  if (self->checker_texture &&
      self->checker_w >= req_w &&
      self->checker_h >= req_h) {
    return self->checker_texture;
  }

  int cw = MAX (self->checker_w, ((MAX (16, req_w) + 63) / 64) * 64);
  int ch = MAX (self->checker_h, ((MAX (16, req_h) + 63) / 64) * 64);

  const int cell = 8;
  const uint32_t light = 0xFFF0F0F2U;
  const uint32_t dark = 0xFFD2D2D8U;

  gsize row_bytes = (gsize) cw * 4;
  uint32_t *row_even = g_malloc (row_bytes);
  uint32_t *row_odd = g_malloc (row_bytes);
  for (int x = 0; x < cw; x++) {
    gboolean dark_col = ((x / cell) & 1) != 0;
    row_even[x] = dark_col ? dark : light;
    row_odd[x] = dark_col ? light : dark;
  }

  unsigned char *pixels = g_malloc (row_bytes * (gsize) ch);
  for (int y = 0; y < ch; y++) {
    const uint32_t *src = (((y / cell) & 1) != 0) ? row_odd : row_even;
    memcpy (pixels + (gsize) y * row_bytes, src, row_bytes);
  }
  g_free (row_even);
  g_free (row_odd);

  g_autoptr (GBytes) bytes = g_bytes_new_take (pixels, row_bytes * (gsize) ch);
  g_clear_object (&self->checker_texture);
  self->checker_texture = gdk_memory_texture_new (cw, ch, GDK_MEMORY_DEFAULT, bytes, row_bytes);
  self->checker_w = cw;
  self->checker_h = ch;
  return self->checker_texture;
}

static void
sync_floating_texture (PaintCanvas *self)
{
  g_clear_object (&self->floating_texture);
  if (self->floating_surface)
    self->floating_texture = paint_texture_from_surface (self->floating_surface);
}

static void
lift_selection_to_floating (PaintCanvas *self)
{
  if (!self->has_selection || self->floating_surface || !self->document)
    return;
  self->floating_surface = paint_document_copy_rect (self->document,
                                                     self->sel_rect.x,
                                                     self->sel_rect.y,
                                                     self->sel_rect.width,
                                                     self->sel_rect.height);
  if (!self->floating_surface)
    return;
  self->floating_has_hole = TRUE;
  self->floating_hole = self->sel_rect;
  sync_floating_texture (self);
}

static void
clear_selection_state (PaintCanvas *self)
{
  gboolean had = self->has_selection || self->floating_surface != NULL;
  self->has_selection = FALSE;
  self->sel_rect = (cairo_rectangle_int_t) { 0, 0, 0, 0 };
  self->sel_dragging_new = FALSE;
  self->sel_moving = FALSE;
  self->floating_has_hole = FALSE;
  self->floating_hole = (cairo_rectangle_int_t) { 0, 0, 0, 0 };
  g_clear_pointer (&self->floating_surface, cairo_surface_destroy);
  g_clear_object (&self->floating_texture);
  if (had)
    g_signal_emit (self, signals[SIGNAL_SELECTION_CHANGED], 0);
}

static void
update_text_bounds (PaintCanvas *self)
{
  if (!self->text_active || !self->document || !self->settings)
    return;
  cairo_t *cr = cairo_create (self->document->surface);
  paint_render_text_layout (cr,
                            self->text_x,
                            self->text_y,
                            self->text_content,
                            self->settings,
                            FALSE,
                            &self->text_box_x,
                            &self->text_box_y,
                            &self->text_box_w,
                            &self->text_box_h);
  cairo_destroy (cr);
}

static gboolean
is_eyedropper_mode (PaintCanvas *self)
{
  return self &&
         (self->alt_held ||
          self->eyedropper_active ||
          (self->settings && g_strcmp0 (self->settings->active_tool_id, "eyedropper") == 0));
}

static void
clear_pick_badge (PaintCanvas *self)
{
  if (!self)
    return;
  g_clear_handle_id (&self->pick_badge_timeout_id, g_source_remove);
  self->pick_badge_active = FALSE;
}

static gboolean
on_pick_badge_timeout (gpointer user_data)
{
  PaintCanvas *self = PAINT_CANVAS (user_data);
  self->pick_badge_timeout_id = 0;
  self->pick_badge_active = FALSE;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  return G_SOURCE_REMOVE;
}

static void
trigger_pick_badge (PaintCanvas *self, const GdkRGBA *color)
{
  if (!self || !color)
    return;
  clear_pick_badge (self);
  self->pick_badge_color = *color;
  self->pick_badge_active = TRUE;
  self->pick_badge_timeout_id = g_timeout_add (750, on_pick_badge_timeout, self);
}

static void
trace_rounded_rect_path (cairo_t *cr, double x, double y, double w, double h, double r)
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
draw_cursor_color_badge (GtkSnapshot *snapshot,
                         PaintCanvas *self,
                         double px,
                         double py,
                         const GdkRGBA *color)
{
  if (!color)
    return;

  int w = gtk_widget_get_width (GTK_WIDGET (self));
  int h = gtk_widget_get_height (GTK_WIDGET (self));
  const double box_size = 24.0;
  const double radius = 5.0;

  double bx = px + 14.0;
  double by = py - 36.0;
  if (w > 0 && bx + box_size > (double) w - 4.0)
    bx = px - box_size - 14.0;
  if (by < 4.0)
    by = py + 16.0;
  if (w > 0)
    bx = CLAMP (bx, 4.0, MAX (4.0, (double) w - box_size - 4.0));
  if (h > 0)
    by = CLAMP (by, 4.0, MAX (4.0, (double) h - box_size - 4.0));

  graphene_rect_t bounds = GRAPHENE_RECT_INIT (
      (float) (bx - 4.0), (float) (by - 4.0),
      (float) (box_size + 8.0), (float) (box_size + 8.0));
  cairo_t *cr = gtk_snapshot_append_cairo (snapshot, &bounds);

  cairo_save (cr);
  trace_rounded_rect_path (cr, bx, by, box_size, box_size, radius);
  cairo_clip (cr);
  if (color->alpha < 0.999f) {
    const double cell = 5.0;
    cairo_set_source_rgb (cr, 0.86, 0.86, 0.88);
    cairo_rectangle (cr, bx, by, box_size, box_size);
    cairo_fill (cr);

    cairo_set_source_rgb (cr, 0.64, 0.64, 0.68);
    int cols = (int) ceil (box_size / cell);
    int rows = (int) ceil (box_size / cell);
    for (int r = 0; r < rows; r++) {
      for (int c = (r & 1); c < cols; c += 2) {
        cairo_rectangle (cr, bx + c * cell, by + r * cell, cell, cell);
      }
    }
    cairo_fill (cr);
  }
  cairo_set_source_rgba (cr, color->red, color->green, color->blue, color->alpha);
  cairo_rectangle (cr, bx, by, box_size, box_size);
  cairo_fill (cr);
  cairo_restore (cr);

  trace_rounded_rect_path (cr, bx, by, box_size, box_size, radius);
  cairo_set_line_width (cr, 3.0);
  cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.45);
  cairo_stroke_preserve (cr);

  cairo_set_line_width (cr, 1.5);
  cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.95);
  cairo_stroke_preserve (cr);

  cairo_set_line_width (cr, 0.75);
  cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.35);
  cairo_stroke (cr);

  cairo_destroy (cr);
}

static void
update_canvas_cursor (PaintCanvas *self)
{
  const PaintTool *tool = self->settings
                            ? paint_tools_find (self->settings->active_tool_id)
                            : NULL;
  const char *want = NULL;
  if (is_eyedropper_mode (self)) {
    want = "crosshair";
  } else if (tool) {
    if (g_strcmp0 (tool->id, "text") == 0) {
      if (self->text_active && self->has_hover &&
          self->hover_x >= self->text_box_x - 8 &&
          self->hover_x <= self->text_box_x + self->text_box_w + 8 &&
          self->hover_y >= self->text_box_y - 8 &&
          self->hover_y <= self->text_box_y + self->text_box_h + 8) {
        want = "move";
      } else {
        want = "text";
      }
    } else if (g_strcmp0 (tool->id, "select") == 0) {
      if (self->has_selection && self->has_hover &&
          self->hover_x >= self->sel_rect.x &&
          self->hover_x < self->sel_rect.x + self->sel_rect.width &&
          self->hover_y >= self->sel_rect.y &&
          self->hover_y < self->sel_rect.y + self->sel_rect.height) {
        want = "move";
      } else {
        want = "crosshair";
      }
    }
  }

  if (g_strcmp0 (self->cursor_name, want) != 0) {
    self->cursor_name = want;
    gtk_widget_set_cursor_from_name (GTK_WIDGET (self), want);
  }
}

static void
paint_canvas_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  PaintCanvas *self = PAINT_CANVAS (widget);
  if (!self->document)
    return;

  propagate_document_dirty (self);
  update_canvas_cursor (self);

  double device_scale = get_widget_device_scale (widget);
  double eff_zoom = self->zoom * device_scale;
  const PaintTool *active_tool = self->settings
                                   ? paint_tools_find (self->settings->active_tool_id)
                                   : NULL;
  gboolean has_preview = self->drawing_active &&
                         !self->touch_stroke_pending &&
                         active_tool &&
                         active_tool->draw_preview != NULL;

  int active_div = 1;
  GskScalingFilter canvas_filter = GSK_SCALING_FILTER_LINEAR;
  graphene_rect_t vis_rect = compute_visible_widget_rect (self);

  GdkTexture *checker = NULL;
  int checker_tx = 0;
  int checker_ty = 0;
  if (vis_rect.size.width > 0.0f && vis_rect.size.height > 0.0f) {
    checker_tx = (int) floor ((double) vis_rect.origin.x / 16.0) * 16;
    checker_ty = (int) floor ((double) vis_rect.origin.y / 16.0) * 16;
    int req_w = MAX (16, (int) ceil ((double) (vis_rect.origin.x + vis_rect.size.width)) - checker_tx);
    int req_h = MAX (16, (int) ceil ((double) (vis_rect.origin.y + vis_rect.size.height)) - checker_ty);
    checker = ensure_checker_texture (self, req_w, req_h);
    gtk_snapshot_push_clip (snapshot, &vis_rect);
    gtk_snapshot_append_scaled_texture (snapshot,
                                        checker,
                                        GSK_SCALING_FILTER_NEAREST,
                                        &GRAPHENE_RECT_INIT ((float) checker_tx,
                                                             (float) checker_ty,
                                                             (float) self->checker_w,
                                                             (float) self->checker_h));
    gtk_snapshot_pop (snapshot);
  }

  if (target_vp_div (self) == 0) {
    GdkTexture *tex = ensure_overview_texture (self, device_scale);
    if (!tex)
      return;
    active_div = self->ov_div;
    float bw = (float) (self->ov_w * self->ov_div * self->zoom);
    float bh = (float) (self->ov_h * self->ov_div * self->zoom);
    if (bw <= 0.0f || bh <= 0.0f)
      return;
    gboolean trilinear = (!self->drawing_active || has_preview) &&
                         (eff_zoom * self->ov_div < 0.5);
    canvas_filter = trilinear ? GSK_SCALING_FILTER_TRILINEAR : GSK_SCALING_FILTER_LINEAR;
    gtk_snapshot_append_scaled_texture (snapshot,
                                        tex,
                                        canvas_filter,
                                        &GRAPHENE_RECT_INIT (0.0f, 0.0f, bw, bh));
  } else {
    if (vis_rect.size.width <= 0.0f || vis_rect.size.height <= 0.0f)
      return;
    GdkTexture *tex = ensure_vp_texture (self, &vis_rect);
    if (!tex)
      return;
    active_div = self->vp_div;

    float x0 = (float) (self->vp_x * self->zoom);
    float y0 = (float) (self->vp_y * self->zoom);
    float x1 = (float) ((self->vp_x + self->vp_surf_w * self->vp_div) * self->zoom);
    float y1 = (float) ((self->vp_y + self->vp_surf_h * self->vp_div) * self->zoom);
    double cache_eff_zoom = eff_zoom / (double) self->vp_div;
    canvas_filter = (fabs (cache_eff_zoom - 1.0) < 1e-4 || cache_eff_zoom >= 1.5)
                      ? GSK_SCALING_FILTER_NEAREST
                      : GSK_SCALING_FILTER_LINEAR;
    gtk_snapshot_append_scaled_texture (snapshot,
                                        tex,
                                        canvas_filter,
                                        &GRAPHENE_RECT_INIT (x0, y0, x1 - x0, y1 - y0));
  }

  if (self->floating_surface && self->floating_texture) {
    if (self->floating_has_hole &&
        self->floating_hole.width > 0 &&
        self->floating_hole.height > 0) {
      graphene_rect_t hole_rect = GRAPHENE_RECT_INIT (
          (float) (self->floating_hole.x * self->zoom),
          (float) (self->floating_hole.y * self->zoom),
          (float) (self->floating_hole.width * self->zoom),
          (float) (self->floating_hole.height * self->zoom));
      if (self->document->background_color.alpha <= 0.0f && checker) {
        gtk_snapshot_push_clip (snapshot, &hole_rect);
        gtk_snapshot_append_scaled_texture (snapshot,
                                            checker,
                                            GSK_SCALING_FILTER_NEAREST,
                                            &GRAPHENE_RECT_INIT ((float) checker_tx,
                                                                 (float) checker_ty,
                                                                 (float) self->checker_w,
                                                                 (float) self->checker_h));
        gtk_snapshot_pop (snapshot);
      } else {
        gtk_snapshot_append_color (snapshot, &self->document->background_color, &hole_rect);
      }
    }

    graphene_rect_t float_rect = GRAPHENE_RECT_INIT (
        (float) (self->sel_rect.x * self->zoom),
        (float) (self->sel_rect.y * self->zoom),
        (float) (self->sel_rect.width * self->zoom),
        (float) (self->sel_rect.height * self->zoom));
    gtk_snapshot_append_scaled_texture (snapshot,
                                        self->floating_texture,
                                        canvas_filter,
                                        &float_rect);
  }

  if (self->show_pixel_grid && self->zoom >= 8.0 &&
      vis_rect.size.width > 0.0f && vis_rect.size.height > 0.0f) {
    int gx0 = MAX (0, (int) floor (vis_rect.origin.x / self->zoom));
    int gy0 = MAX (0, (int) floor (vis_rect.origin.y / self->zoom));
    int gx1 = MIN (self->document->width,
                   (int) ceil ((vis_rect.origin.x + vis_rect.size.width) / self->zoom));
    int gy1 = MIN (self->document->height,
                   (int) ceil ((vis_rect.origin.y + vis_rect.size.height) / self->zoom));

    cairo_t *cr = gtk_snapshot_append_cairo (snapshot, &vis_rect);
    cairo_set_line_width (cr, 1.0);
    cairo_set_source_rgba (cr, 0.5, 0.5, 0.5, 0.28);
    for (int gx = gx0; gx <= gx1; gx++) {
      double sx = floor ((double) gx * self->zoom) + 0.5;
      cairo_move_to (cr, sx, vis_rect.origin.y);
      cairo_line_to (cr, sx, vis_rect.origin.y + vis_rect.size.height);
    }
    for (int gy = gy0; gy <= gy1; gy++) {
      double sy = floor ((double) gy * self->zoom) + 0.5;
      cairo_move_to (cr, vis_rect.origin.x, sy);
      cairo_line_to (cr, vis_rect.origin.x + vis_rect.size.width, sy);
    }
    cairo_stroke (cr);
    cairo_destroy (cr);
  }

  if (has_preview) {
    double x0 = self->drag_start_x / self->zoom + self->shape_shift_x;
    double y0 = self->drag_start_y / self->zoom + self->shape_shift_y;
    double x1 = self->preview_end_x;
    double y1 = self->preview_end_y;

    int rx, ry, rw, rh;
    compute_tool_stroke_bounds (active_tool, self->settings, x0, y0, x1, y1,
                                &rx, &ry, &rw, &rh);

    int vis_x0 = MAX (0, (int) floor (vis_rect.origin.x / self->zoom) - 4);
    int vis_y0 = MAX (0, (int) floor (vis_rect.origin.y / self->zoom) - 4);
    int vis_x1 = MIN (self->document->width,
                      (int) ceil ((vis_rect.origin.x + vis_rect.size.width) / self->zoom) + 4);
    int vis_y1 = MIN (self->document->height,
                      (int) ceil ((vis_rect.origin.y + vis_rect.size.height) / self->zoom) + 4);

    int cx0 = MAX (vis_x0, rx);
    int cy0 = MAX (vis_y0, ry);
    int cx1 = MIN (vis_x1, rx + rw);
    int cy1 = MIN (vis_y1, ry + rh);

    if (cx0 < cx1 && cy0 < cy1) {
      int div = MAX (1, active_div);
      int ax0 = (cx0 / div) * div;
      int ay0 = (cy0 / div) * div;
      int ax1 = MIN (self->document->width, ((cx1 + div - 1) / div) * div);
      int ay1 = MIN (self->document->height, ((cy1 + div - 1) / div) * div);
      int sw = MAX (1, (ax1 - ax0 + div - 1) / div);
      int sh = MAX (1, (ay1 - ay0 + div - 1) / div);

      cairo_surface_t *psurf = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, sw, sh);
      cairo_t *cr = cairo_create (psurf);
      if (div > 1)
        cairo_scale (cr, 1.0 / (double) div, 1.0 / (double) div);
      cairo_translate (cr, -(double) ax0, -(double) ay0);
      active_tool->draw_preview (active_tool, cr, x0, y0, x1, y1, self->settings);
      cairo_destroy (cr);

      cairo_surface_flush (psurf);
      int stride = cairo_image_surface_get_stride (psurf);
      unsigned char *data = cairo_image_surface_get_data (psurf);
      g_autoptr (GBytes) bytes = g_bytes_new (data, (gsize) stride * (gsize) sh);
      g_autoptr (GdkTexture) ptex = gdk_memory_texture_new (sw, sh, GDK_MEMORY_DEFAULT, bytes, (gsize) stride);
      cairo_surface_destroy (psurf);

      float px0 = (float) (ax0 * self->zoom);
      float py0 = (float) (ay0 * self->zoom);
      float px1 = (float) ((ax0 + sw * div) * self->zoom);
      float py1 = (float) ((ay0 + sh * div) * self->zoom);
      if (div > 1) {
        float cw = (float) (self->document->width * self->zoom);
        float ch = (float) (self->document->height * self->zoom);
        gtk_snapshot_push_clip (snapshot, &GRAPHENE_RECT_INIT (0.0f, 0.0f, cw, ch));
      }
      gtk_snapshot_append_scaled_texture (snapshot,
                                          ptex,
                                          canvas_filter,
                                          &GRAPHENE_RECT_INIT (px0, py0, px1 - px0, py1 - py0));
      if (div > 1)
        gtk_snapshot_pop (snapshot);
    }
  }

  if (self->text_active && self->settings) {
    update_text_bounds (self);
    float pad = 8.0f;
    float bx0 = (float) (self->text_box_x * self->zoom) - pad;
    float by0 = (float) (self->text_box_y * self->zoom) - pad;
    float bw = (float) (self->text_box_w * self->zoom) + pad * 2.0f;
    float bh = (float) (self->text_box_h * self->zoom) + pad * 2.0f;
    graphene_rect_t bounds = GRAPHENE_RECT_INIT (bx0, by0, bw, bh);

    cairo_t *cr = gtk_snapshot_append_cairo (snapshot, &bounds);
    cairo_save (cr);
    cairo_scale (cr, self->zoom, self->zoom);
    paint_render_text_layout (cr, self->text_x, self->text_y, self->text_content,
                              self->settings, TRUE, NULL, NULL, NULL, NULL);
    cairo_restore (cr);

    double gx = floor (self->text_box_x * self->zoom) - 2.5;
    double gy = floor (self->text_box_y * self->zoom) - 2.5;
    double gw = ceil (self->text_box_w * self->zoom) + 5.0;
    double gh = ceil (self->text_box_h * self->zoom) + 5.0;
    static const double dash[] = { 4.0, 4.0 };
    cairo_set_line_width (cr, 1.0);
    cairo_set_dash (cr, dash, 2, 0.0);
    cairo_set_source_rgba (cr, 0.11, 0.44, 0.96, 0.85);
    cairo_rectangle (cr, gx, gy, gw, gh);
    cairo_stroke (cr);
    cairo_destroy (cr);
  }

  if ((self->has_selection || self->sel_dragging_new) &&
      self->sel_rect.width > 0 && self->sel_rect.height > 0) {
    double sx0 = floor (self->sel_rect.x * self->zoom) + 0.5;
    double sy0 = floor (self->sel_rect.y * self->zoom) + 0.5;
    double sw = MAX (1.0, round (self->sel_rect.width * self->zoom) - 1.0);
    double sh = MAX (1.0, round (self->sel_rect.height * self->zoom) - 1.0);
    graphene_rect_t bounds = GRAPHENE_RECT_INIT (
        (float) (sx0 - 3.0), (float) (sy0 - 3.0), (float) (sw + 6.0), (float) (sh + 6.0));
    cairo_t *cr = gtk_snapshot_append_cairo (snapshot, &bounds);
    static const double dash[] = { 5.0, 5.0 };
    cairo_set_line_width (cr, 1.0);
    cairo_set_dash (cr, dash, 2, 0.0);
    cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.95);
    cairo_rectangle (cr, sx0, sy0, sw, sh);
    cairo_stroke (cr);

    cairo_set_dash (cr, dash, 2, 5.0);
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.9);
    cairo_rectangle (cr, sx0, sy0, sw, sh);
    cairo_stroke (cr);
    cairo_destroy (cr);
  }

  gboolean eyedropper_active_mode = is_eyedropper_mode (self);
  gboolean show_ring = self->show_cursor_ring &&
                       self->has_hover &&
                       !eyedropper_active_mode &&
                       active_tool &&
                       g_strcmp0 (active_tool->id, "select") != 0 &&
                       g_strcmp0 (active_tool->id, "text") != 0;
  if (show_ring) {
    double px = self->hover_x * self->zoom;
    double py = self->hover_y * self->zoom;
    double radius = active_tool->uses_brush_size
                      ? MAX (2.0, (self->settings->size * self->zoom) / 2.0)
                      : 3.5;

    float pad = (float) (radius + 4.0);
    graphene_rect_t bounds = GRAPHENE_RECT_INIT ((float) px - pad,
                                                 (float) py - pad,
                                                 pad * 2.0f,
                                                 pad * 2.0f);
    cairo_t *cr = gtk_snapshot_append_cairo (snapshot, &bounds);
    cairo_set_line_width (cr, 1.5);
    cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.85);
    cairo_arc (cr, px, py, radius + 0.75, 0.0, 2.0 * G_PI);
    cairo_stroke (cr);

    cairo_set_line_width (cr, 1.0);
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.75);
    cairo_arc (cr, px, py, radius, 0.0, 2.0 * G_PI);
    cairo_stroke (cr);
    cairo_destroy (cr);
  }

  if (self->has_hover) {
    double px = self->hover_x * self->zoom;
    double py = self->hover_y * self->zoom;
    if (eyedropper_active_mode) {
      GdkRGBA hovered;
      if (paint_document_pick_color (self->document, self->hover_x, self->hover_y, &hovered))
        draw_cursor_color_badge (snapshot, self, px, py, &hovered);
    } else if (self->pick_badge_active) {
      draw_cursor_color_badge (snapshot, self, px, py, &self->pick_badge_color);
    }
  }
}

static gboolean
is_touch_gesture (GtkGesture *gesture)
{
  GdkEventSequence *seq = gtk_gesture_single_get_current_sequence (GTK_GESTURE_SINGLE (gesture));
  GdkEvent *ev = gtk_gesture_get_last_event (gesture, seq);
  if (ev) {
    GdkEventType et = gdk_event_get_event_type (ev);
    if (et == GDK_TOUCH_BEGIN || et == GDK_TOUCH_UPDATE ||
        et == GDK_TOUCH_END || et == GDK_TOUCH_CANCEL) {
      return TRUE;
    }
  }
  GdkDevice *dev = gtk_gesture_get_device (gesture);
  return dev && gdk_device_get_source (dev) == GDK_SOURCE_TOUCHSCREEN;
}

static void
resolve_drag_point_from (double x0,
                         double y0,
                         GtkGestureDrag *gesture,
                         const PaintTool *tool,
                         double raw_cx,
                         double raw_cy,
                         double *out_cx,
                         double *out_cy)
{
  double cx = raw_cx;
  double cy = raw_cy;
  if (tool && tool->snap_point) {
    GdkModifierType state = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (gesture));
    if (state & GDK_SHIFT_MASK)
      tool->snap_point (x0, y0, &cx, &cy);
  }
  *out_cx = cx;
  *out_cy = cy;
}

static void
snap_square_coords (double x0, double y0, double *x1, double *y1)
{
  double dx = *x1 - x0;
  double dy = *y1 - y0;
  double side = MAX (fabs (dx), fabs (dy));
  *x1 = x0 + (dx < 0.0 ? -side : side);
  *y1 = y0 + (dy < 0.0 ? -side : side);
}

static gboolean
on_canvas_key_pressed (GtkEventControllerKey *ctrl G_GNUC_UNUSED,
                       guint keyval,
                       guint keycode G_GNUC_UNUSED,
                       GdkModifierType state G_GNUC_UNUSED,
                       gpointer user_data)
{
  PaintCanvas *self = PAINT_CANVAS (user_data);
  if (keyval == GDK_KEY_space && self->drawing_active) {
    const PaintTool *tool = self->settings
                              ? paint_tools_find (self->settings->active_tool_id)
                              : NULL;
    if (tool && tool->draw_preview) {
      self->space_held = TRUE;
      return TRUE;
    }
  }
  return FALSE;
}

static void
on_canvas_key_released (GtkEventControllerKey *ctrl G_GNUC_UNUSED,
                        guint keyval,
                        guint keycode G_GNUC_UNUSED,
                        GdkModifierType state G_GNUC_UNUSED,
                        gpointer user_data)
{
  PaintCanvas *self = PAINT_CANVAS (user_data);
  if (keyval == GDK_KEY_space)
    self->space_held = FALSE;
}

static gboolean
sample_color_at (PaintCanvas *self, double cx, double cy)
{
  GdkRGBA picked;
  if (paint_document_pick_color (self->document, cx, cy, &picked)) {
    self->settings->color = picked;
    g_signal_emit (self, signals[SIGNAL_COLOR_PICKED], 0, FALSE);
    return TRUE;
  }
  return FALSE;
}

static void
on_drag_begin (GtkGestureDrag *gesture,
               double start_x,
               double start_y,
               gpointer user_data)
{
  PaintCanvas *self = PAINT_CANVAS (user_data);
  gboolean is_touch = is_touch_gesture (GTK_GESTURE (gesture));

  /* Leave touch sequences unclaimed so parent CAPTURE zoom gesture can see a 2nd finger */
  if (!is_touch)
    gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);

  gtk_widget_grab_focus (GTK_WIDGET (self));
  self->drawing_active = TRUE;
  self->touch_stroke_pending = is_touch;
  self->drag_start_x = start_x;
  self->drag_start_y = start_y;
  self->last_drag_off_x = 0.0;
  self->last_drag_off_y = 0.0;
  self->space_held = FALSE;
  self->shape_shift_x = 0.0;
  self->shape_shift_y = 0.0;

  double cx = start_x / self->zoom;
  double cy = start_y / self->zoom;
  self->prev_raw_cx = cx;
  self->prev_raw_cy = cy;
  self->has_hover = !is_touch;
  self->hover_x = cx;
  self->hover_y = cy;
  self->preview_end_x = cx;
  self->preview_end_y = cy;

  const PaintTool *tool = paint_tools_find (self->settings->active_tool_id);
  GdkModifierType state = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (gesture));
  if (g_strcmp0 (tool->id, "eyedropper") == 0 || (state & GDK_ALT_MASK) || self->alt_held) {
    self->eyedropper_active = TRUE;
    self->touch_stroke_pending = FALSE;
    sample_color_at (self, cx, cy);
    update_canvas_cursor (self);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    return;
  }

  clear_pick_badge (self);

  if (g_strcmp0 (tool->id, "select") == 0) {
    self->touch_stroke_pending = FALSE;
    if (self->text_active)
      paint_canvas_commit_text (self);
    if (self->has_selection &&
        cx >= self->sel_rect.x && cx < self->sel_rect.x + self->sel_rect.width &&
        cy >= self->sel_rect.y && cy < self->sel_rect.y + self->sel_rect.height) {
      lift_selection_to_floating (self);
      self->sel_moving = TRUE;
      self->sel_move_anchor_x = cx;
      self->sel_move_anchor_y = cy;
      self->sel_move_orig_x = self->sel_rect.x;
      self->sel_move_orig_y = self->sel_rect.y;
    } else {
      if (self->has_selection)
        paint_canvas_commit_selection (self);
      self->sel_dragging_new = TRUE;
      self->has_selection = FALSE;
      self->sel_rect = (cairo_rectangle_int_t) {
        CLAMP ((int) floor (cx), 0, self->document->width),
        CLAMP ((int) floor (cy), 0, self->document->height),
        0, 0
      };
      g_signal_emit (self, signals[SIGNAL_SELECTION_CHANGED], 0);
    }
    gtk_widget_queue_draw (GTK_WIDGET (self));
    return;
  }

  if (g_strcmp0 (tool->id, "text") == 0) {
    self->touch_stroke_pending = FALSE;
    if (self->has_selection)
      paint_canvas_commit_selection (self);
    if (self->text_active &&
        cx >= self->text_box_x - 10 && cx <= self->text_box_x + self->text_box_w + 10 &&
        cy >= self->text_box_y - 10 && cy <= self->text_box_y + self->text_box_h + 10) {
      self->text_dragging = TRUE;
      self->text_drag_anchor_x = cx;
      self->text_drag_anchor_y = cy;
      self->text_drag_orig_x = self->text_x;
      self->text_drag_orig_y = self->text_y;
    } else {
      if (self->text_active)
        paint_canvas_commit_text (self);
      self->text_active = TRUE;
      self->text_x = CLAMP (cx, 0.0, (double) self->document->width);
      self->text_y = CLAMP (cy, 0.0, (double) self->document->height);
      g_free (self->text_content);
      self->text_content = g_strdup ("");
      update_text_bounds (self);
      g_signal_emit (self, signals[SIGNAL_TEXT_STATE_CHANGED], 0);
      gtk_widget_queue_draw (GTK_WIDGET (self));
    }
    return;
  }

  if (!is_touch) {
    paint_canvas_commit_active_overlays (self);
    paint_document_begin_stroke (self->document, cx, cy, tool, self->settings);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    if (!tool->draw_preview)
      g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
  }
}

static void
on_drag_update (GtkGestureDrag *gesture,
                double offset_x,
                double offset_y,
                gpointer user_data)
{
  PaintCanvas *self = PAINT_CANVAS (user_data);
  if (!self->drawing_active)
    return;

  double raw_cx = (self->drag_start_x + offset_x) / self->zoom;
  double raw_cy = (self->drag_start_y + offset_y) / self->zoom;
  self->hover_x = raw_cx;
  self->hover_y = raw_cy;

  if (self->eyedropper_active) {
    sample_color_at (self, raw_cx, raw_cy);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    return;
  }

  if (self->sel_moving) {
    int dx = (int) lround (raw_cx - self->sel_move_anchor_x);
    int dy = (int) lround (raw_cy - self->sel_move_anchor_y);
    self->sel_rect.x = self->sel_move_orig_x + dx;
    self->sel_rect.y = self->sel_move_orig_y + dy;
    gtk_widget_queue_draw (GTK_WIDGET (self));
    return;
  }

  if (self->sel_dragging_new) {
    double sx0 = CLAMP (self->drag_start_x / self->zoom, 0.0, (double) self->document->width);
    double sy0 = CLAMP (self->drag_start_y / self->zoom, 0.0, (double) self->document->height);
    double ex = raw_cx;
    double ey = raw_cy;
    GdkModifierType state = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (gesture));
    if (state & GDK_SHIFT_MASK)
      snap_square_coords (sx0, sy0, &ex, &ey);
    ex = CLAMP (ex, 0.0, (double) self->document->width);
    ey = CLAMP (ey, 0.0, (double) self->document->height);
    int x0 = (int) floor (MIN (sx0, ex));
    int y0 = (int) floor (MIN (sy0, ey));
    int x1 = (int) ceil (MAX (sx0, ex));
    int y1 = (int) ceil (MAX (sy0, ey));
    self->sel_rect = (cairo_rectangle_int_t) { x0, y0, x1 - x0, y1 - y0 };
    self->has_selection = (self->sel_rect.width >= 2 && self->sel_rect.height >= 2);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    return;
  }

  if (self->text_dragging) {
    self->text_x = self->text_drag_orig_x + (raw_cx - self->text_drag_anchor_x);
    self->text_y = self->text_drag_orig_y + (raw_cy - self->text_drag_anchor_y);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    return;
  }

  const PaintTool *tool = paint_tools_find (self->settings->active_tool_id);
  if (g_strcmp0 (tool->id, "text") == 0)
    return;

  if (self->touch_stroke_pending) {
    if (!tool->uses_brush_size)
      return;
    if (hypot (offset_x, offset_y) < 6.0)
      return;
    self->touch_stroke_pending = FALSE;
    paint_canvas_commit_active_overlays (self);
    paint_document_begin_stroke (self->document,
                                 self->drag_start_x / self->zoom,
                                 self->drag_start_y / self->zoom,
                                 tool,
                                 self->settings);
    if (!tool->draw_preview)
      g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
  } else if (hypot (offset_x - self->last_drag_off_x, offset_y - self->last_drag_off_y) < 0.75) {
    return;
  }

  self->last_drag_off_x = offset_x;
  self->last_drag_off_y = offset_y;

  if (tool->draw_preview) {
    double ddx = raw_cx - self->prev_raw_cx;
    double ddy = raw_cy - self->prev_raw_cy;
    self->prev_raw_cx = raw_cx;
    self->prev_raw_cy = raw_cy;

    if (self->space_held) {
      self->shape_shift_x += ddx;
      self->shape_shift_y += ddy;
      self->preview_end_x += ddx;
      self->preview_end_y += ddy;
      paint_document_shift_preview_start (
          self->document,
          self->settings,
          self->drag_start_x / self->zoom + self->shape_shift_x,
          self->drag_start_y / self->zoom + self->shape_shift_y);
      paint_document_continue_stroke (self->document,
                                      self->preview_end_x,
                                      self->preview_end_y,
                                      tool,
                                      self->settings);
      gtk_widget_queue_draw (GTK_WIDGET (self));
      return;
    }

    double sx0 = self->drag_start_x / self->zoom + self->shape_shift_x;
    double sy0 = self->drag_start_y / self->zoom + self->shape_shift_y;
    double cx, cy;
    resolve_drag_point_from (sx0, sy0, gesture, tool, raw_cx, raw_cy, &cx, &cy);
    self->preview_end_x = cx;
    self->preview_end_y = cy;
    paint_document_continue_stroke (self->document, cx, cy, tool, self->settings);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    return;
  }

  self->prev_raw_cx = raw_cx;
  self->prev_raw_cy = raw_cy;
  self->preview_end_x = raw_cx;
  self->preview_end_y = raw_cy;
  paint_document_continue_stroke (self->document, raw_cx, raw_cy, tool, self->settings);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_drag_end (GtkGestureDrag *gesture,
             double offset_x,
             double offset_y,
             gpointer user_data)
{
  PaintCanvas *self = PAINT_CANVAS (user_data);
  if (!self->drawing_active)
    return;

  self->drawing_active = FALSE;
  double raw_cx = (self->drag_start_x + offset_x) / self->zoom;
  double raw_cy = (self->drag_start_y + offset_y) / self->zoom;

  if (self->eyedropper_active) {
    self->eyedropper_active = FALSE;
    self->hover_x = raw_cx;
    self->hover_y = raw_cy;
    if (sample_color_at (self, raw_cx, raw_cy)) {
      paint_tool_settings_add_recent_color (self->settings, &self->settings->color);
      trigger_pick_badge (self, &self->settings->color);
    }
    g_signal_emit (self, signals[SIGNAL_COLOR_PICKED], 0, TRUE);
    update_canvas_cursor (self);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    return;
  }

  if (self->sel_moving) {
    self->sel_moving = FALSE;
    g_signal_emit (self, signals[SIGNAL_SELECTION_CHANGED], 0);
    return;
  }

  if (self->sel_dragging_new) {
    self->sel_dragging_new = FALSE;
    if (!self->has_selection)
      self->sel_rect = (cairo_rectangle_int_t) { 0, 0, 0, 0 };
    gtk_widget_queue_draw (GTK_WIDGET (self));
    g_signal_emit (self, signals[SIGNAL_SELECTION_CHANGED], 0);
    return;
  }

  if (self->text_dragging) {
    self->text_dragging = FALSE;
    return;
  }

  const PaintTool *tool = paint_tools_find (self->settings->active_tool_id);
  if (g_strcmp0 (tool->id, "text") == 0 || g_strcmp0 (tool->id, "select") == 0)
    return;

  if (self->touch_stroke_pending) {
    self->touch_stroke_pending = FALSE;
    paint_canvas_commit_active_overlays (self);
    paint_document_begin_stroke (self->document,
                                 self->drag_start_x / self->zoom,
                                 self->drag_start_y / self->zoom,
                                 tool,
                                 self->settings);
  }

  double cx = raw_cx;
  double cy = raw_cy;
  if (tool->draw_preview) {
    if (self->space_held) {
      double ddx = raw_cx - self->prev_raw_cx;
      double ddy = raw_cy - self->prev_raw_cy;
      self->shape_shift_x += ddx;
      self->shape_shift_y += ddy;
      self->preview_end_x += ddx;
      self->preview_end_y += ddy;
      paint_document_shift_preview_start (
          self->document,
          self->settings,
          self->drag_start_x / self->zoom + self->shape_shift_x,
          self->drag_start_y / self->zoom + self->shape_shift_y);
      cx = self->preview_end_x;
      cy = self->preview_end_y;
    } else {
      double sx0 = self->drag_start_x / self->zoom + self->shape_shift_x;
      double sy0 = self->drag_start_y / self->zoom + self->shape_shift_y;
      resolve_drag_point_from (sx0, sy0, gesture, tool, raw_cx, raw_cy, &cx, &cy);
    }
  }

  self->space_held = FALSE;
  self->shape_shift_x = 0.0;
  self->shape_shift_y = 0.0;

  paint_document_end_stroke (self->document, cx, cy, tool, self->settings);
  if (g_strcmp0 (tool->id, "eraser") != 0 && !tool->is_transparency)
    paint_tool_settings_add_recent_color (self->settings, &self->settings->color);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

static void
on_drag_cancel (GtkGesture *gesture G_GNUC_UNUSED,
                GdkEventSequence *sequence G_GNUC_UNUSED,
                gpointer user_data)
{
  PaintCanvas *self = PAINT_CANVAS (user_data);
  if (!self->drawing_active)
    return;

  self->drawing_active = FALSE;
  self->space_held = FALSE;
  self->shape_shift_x = 0.0;
  self->shape_shift_y = 0.0;

  if (self->eyedropper_active) {
    self->eyedropper_active = FALSE;
    update_canvas_cursor (self);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    return;
  }
  if (self->sel_moving) {
    self->sel_rect.x = self->sel_move_orig_x;
    self->sel_rect.y = self->sel_move_orig_y;
    self->sel_moving = FALSE;
    gtk_widget_queue_draw (GTK_WIDGET (self));
    return;
  }
  if (self->sel_dragging_new) {
    self->sel_dragging_new = FALSE;
    self->has_selection = FALSE;
    self->sel_rect = (cairo_rectangle_int_t) { 0, 0, 0, 0 };
    gtk_widget_queue_draw (GTK_WIDGET (self));
    g_signal_emit (self, signals[SIGNAL_SELECTION_CHANGED], 0);
    return;
  }
  if (self->text_dragging) {
    self->text_x = self->text_drag_orig_x;
    self->text_y = self->text_drag_orig_y;
    self->text_dragging = FALSE;
    gtk_widget_queue_draw (GTK_WIDGET (self));
    return;
  }

  if (self->touch_stroke_pending) {
    self->touch_stroke_pending = FALSE;
  } else {
    paint_document_cancel_stroke (self->document);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
  }
}

static void
on_motion (GtkEventControllerMotion *ctrl,
           double x,
           double y,
           gpointer user_data)
{
  PaintCanvas *self = PAINT_CANVAS (user_data);
  self->has_hover = TRUE;
  self->hover_x = x / self->zoom;
  self->hover_y = y / self->zoom;
  if (!self->drawing_active) {
    GdkModifierType state = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (ctrl));
    self->alt_held = (state & GDK_ALT_MASK) != 0;
  }
  update_canvas_cursor (self);
  if (self->show_cursor_ring || is_eyedropper_mode (self) || self->pick_badge_active)
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_leave (GtkEventControllerMotion *ctrl G_GNUC_UNUSED, gpointer user_data)
{
  PaintCanvas *self = PAINT_CANVAS (user_data);
  gboolean was_eyedropper = is_eyedropper_mode (self);
  self->has_hover = FALSE;
  update_canvas_cursor (self);
  if (self->show_cursor_ring || was_eyedropper || self->pick_badge_active)
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
paint_canvas_finalize (GObject *object)
{
  PaintCanvas *self = PAINT_CANVAS (object);
  g_clear_handle_id (&self->pick_badge_timeout_id, g_source_remove);
  invalidate_viewport_cache (self);
  g_clear_object (&self->checker_texture);
  g_clear_pointer (&self->vp_surface, cairo_surface_destroy);
  g_clear_pointer (&self->ov_surface, cairo_surface_destroy);
  g_clear_pointer (&self->floating_surface, cairo_surface_destroy);
  g_clear_object (&self->floating_texture);
  g_clear_pointer (&self->text_content, g_free);
  g_clear_pointer (&self->document, paint_document_free);
  G_OBJECT_CLASS (paint_canvas_parent_class)->finalize (object);
}

static void
paint_canvas_class_init (PaintCanvasClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = paint_canvas_finalize;

  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  widget_class->measure = paint_canvas_measure;
  widget_class->snapshot = paint_canvas_snapshot;

  signals[SIGNAL_CHANGED] = g_signal_new ("changed",
                                          G_TYPE_FROM_CLASS (klass),
                                          G_SIGNAL_RUN_FIRST,
                                          0, NULL, NULL, NULL,
                                          G_TYPE_NONE, 0);
  signals[SIGNAL_ZOOM_CHANGED] = g_signal_new ("zoom-changed",
                                               G_TYPE_FROM_CLASS (klass),
                                               G_SIGNAL_RUN_FIRST,
                                               0, NULL, NULL, NULL,
                                               G_TYPE_NONE, 1, G_TYPE_DOUBLE);
  signals[SIGNAL_COLOR_PICKED] = g_signal_new ("color-picked",
                                                G_TYPE_FROM_CLASS (klass),
                                                G_SIGNAL_RUN_FIRST,
                                                0, NULL, NULL, NULL,
                                                G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
  signals[SIGNAL_SELECTION_CHANGED] = g_signal_new ("selection-changed",
                                                     G_TYPE_FROM_CLASS (klass),
                                                     G_SIGNAL_RUN_FIRST,
                                                     0, NULL, NULL, NULL,
                                                     G_TYPE_NONE, 0);
  signals[SIGNAL_TEXT_STATE_CHANGED] = g_signal_new ("text-state-changed",
                                                      G_TYPE_FROM_CLASS (klass),
                                                      G_SIGNAL_RUN_FIRST,
                                                      0, NULL, NULL, NULL,
                                                      G_TYPE_NONE, 0);
}

static void
paint_canvas_init (PaintCanvas *self)
{
  self->zoom = 1.0;
  self->show_cursor_ring = TRUE;
  self->show_pixel_grid = TRUE;

  gtk_widget_add_css_class (GTK_WIDGET (self), "paint-canvas-frame");
  gtk_widget_add_css_class (GTK_WIDGET (self), "paint-canvas-shadow");
  gtk_widget_set_halign (GTK_WIDGET (self), GTK_ALIGN_CENTER);
  gtk_widget_set_valign (GTK_WIDGET (self), GTK_ALIGN_CENTER);
  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
  gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_HIDDEN);

  GtkGesture *drag = gtk_gesture_drag_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (drag), GDK_BUTTON_PRIMARY);
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_drag_end), self);
  g_signal_connect (drag, "cancel", G_CALLBACK (on_drag_cancel), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));

  GtkEventController *motion = gtk_event_controller_motion_new ();
  g_signal_connect (motion, "motion", G_CALLBACK (on_motion), self);
  g_signal_connect (motion, "leave", G_CALLBACK (on_leave), self);
  gtk_widget_add_controller (GTK_WIDGET (self), motion);

  GtkEventController *key_ctrl = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (key_ctrl, GTK_PHASE_CAPTURE);
  g_signal_connect (key_ctrl, "key-pressed", G_CALLBACK (on_canvas_key_pressed), self);
  g_signal_connect (key_ctrl, "key-released", G_CALLBACK (on_canvas_key_released), self);
  gtk_widget_add_controller (GTK_WIDGET (self), key_ctrl);
}

PaintCanvas *
paint_canvas_new (PaintDocument *doc, PaintToolSettings *settings)
{
  PaintCanvas *self = g_object_new (PAINT_TYPE_CANVAS, NULL);
  self->document = doc ? doc : paint_document_new (PAINT_DEFAULT_CANVAS_WIDTH,
                                                   PAINT_DEFAULT_CANVAS_HEIGHT);
  self->settings = settings;
  invalidate_viewport_cache (self);
  update_size_request (self);
  return self;
}

PaintDocument *
paint_canvas_get_document (PaintCanvas *self)
{
  return self->document;
}

void
paint_canvas_set_document (PaintCanvas *self, PaintDocument *doc)
{
  paint_canvas_cancel_text (self);
  clear_selection_state (self);
  invalidate_viewport_cache (self);
  g_clear_pointer (&self->document, paint_document_free);
  self->document = doc;
  update_size_request (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

PaintToolSettings *
paint_canvas_get_settings (PaintCanvas *self)
{
  return self->settings;
}

double
paint_canvas_get_zoom (PaintCanvas *self)
{
  return self->zoom;
}

double
paint_canvas_get_min_zoom (void)
{
  return ZOOM_STEPS[0];
}

double
paint_canvas_get_max_zoom (void)
{
  return ZOOM_STEPS[G_N_ELEMENTS (ZOOM_STEPS) - 1];
}

void
paint_canvas_set_zoom (PaintCanvas *self, double zoom)
{
  double min_z = ZOOM_STEPS[0];
  double max_z = ZOOM_STEPS[G_N_ELEMENTS (ZOOM_STEPS) - 1];
  double clamped = CLAMP (zoom, min_z, max_z);
  if (fabs (clamped - self->zoom) < 1e-4)
    return;
  self->zoom = clamped;
  update_size_request (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_ZOOM_CHANGED], 0, self->zoom);
}

void
paint_canvas_zoom_in (PaintCanvas *self)
{
  for (gsize i = 0; i < G_N_ELEMENTS (ZOOM_STEPS); i++) {
    if (ZOOM_STEPS[i] > self->zoom + 1e-4) {
      paint_canvas_set_zoom (self, ZOOM_STEPS[i]);
      return;
    }
  }
}

void
paint_canvas_zoom_out (PaintCanvas *self)
{
  for (gssize i = (gssize) G_N_ELEMENTS (ZOOM_STEPS) - 1; i >= 0; i--) {
    if (ZOOM_STEPS[i] < self->zoom - 1e-4) {
      paint_canvas_set_zoom (self, ZOOM_STEPS[i]);
      return;
    }
  }
}

void
paint_canvas_zoom_reset (PaintCanvas *self)
{
  paint_canvas_set_zoom (self, 1.0);
}

gboolean
paint_canvas_undo (PaintCanvas *self)
{
  if (self->text_active) {
    paint_canvas_cancel_text (self);
    return TRUE;
  }
  if (self->floating_surface) {
    paint_canvas_cancel_selection (self);
    return TRUE;
  }
  clear_selection_state (self);
  if (paint_document_undo (self->document)) {
    update_size_request (self);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
    return TRUE;
  }
  return FALSE;
}

gboolean
paint_canvas_redo (PaintCanvas *self)
{
  paint_canvas_commit_active_overlays (self);
  if (paint_document_redo (self->document)) {
    update_size_request (self);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
    return TRUE;
  }
  return FALSE;
}

void
paint_canvas_clear (PaintCanvas *self)
{
  paint_canvas_commit_active_overlays (self);
  paint_document_clear (self->document);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

void
paint_canvas_clear_transparent (PaintCanvas *self)
{
  paint_canvas_commit_active_overlays (self);
  paint_document_clear_transparent (self->document);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

void
paint_canvas_resize (PaintCanvas *self, int width, int height)
{
  paint_canvas_commit_active_overlays (self);
  paint_document_resize (self->document, width, height);
  update_size_request (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

void
paint_canvas_scale (PaintCanvas *self, int width, int height)
{
  paint_canvas_commit_active_overlays (self);
  paint_document_scale (self->document, width, height);
  update_size_request (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

void
paint_canvas_rotate_90 (PaintCanvas *self, gboolean clockwise)
{
  if (self->has_selection) {
    lift_selection_to_floating (self);
    if (!self->floating_surface)
      return;
    cairo_surface_t *rot = paint_surface_rotate_90 (self->floating_surface, clockwise);
    cairo_surface_destroy (self->floating_surface);
    self->floating_surface = rot;
    int nw = cairo_image_surface_get_width (rot);
    int nh = cairo_image_surface_get_height (rot);
    int cx = self->sel_rect.x + self->sel_rect.width / 2;
    int cy = self->sel_rect.y + self->sel_rect.height / 2;
    self->sel_rect.x = cx - nw / 2;
    self->sel_rect.y = cy - nh / 2;
    self->sel_rect.width = nw;
    self->sel_rect.height = nh;
    sync_floating_texture (self);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    g_signal_emit (self, signals[SIGNAL_SELECTION_CHANGED], 0);
    return;
  }

  paint_canvas_commit_text (self);
  paint_document_rotate_90 (self->document, clockwise);
  update_size_request (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

void
paint_canvas_flip (PaintCanvas *self, gboolean horizontal)
{
  if (self->has_selection) {
    lift_selection_to_floating (self);
    if (!self->floating_surface)
      return;
    cairo_surface_t *flipped = paint_surface_flip (self->floating_surface, horizontal);
    cairo_surface_destroy (self->floating_surface);
    self->floating_surface = flipped;
    sync_floating_texture (self);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    return;
  }

  paint_canvas_commit_text (self);
  paint_document_flip (self->document, horizontal);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

gboolean
paint_canvas_has_selection (PaintCanvas *self)
{
  return self && self->has_selection && self->sel_rect.width > 0 && self->sel_rect.height > 0;
}

void
paint_canvas_select_all (PaintCanvas *self)
{
  if (!self || !self->document)
    return;
  paint_canvas_commit_active_overlays (self);
  self->has_selection = TRUE;
  self->sel_rect = (cairo_rectangle_int_t) {
    0, 0, self->document->width, self->document->height
  };
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_SELECTION_CHANGED], 0);
}

void
paint_canvas_commit_selection (PaintCanvas *self)
{
  if (!self || !self->has_selection)
    return;

  if (self->floating_surface) {
    paint_document_commit_move (self->document,
                                self->floating_has_hole ? &self->floating_hole : NULL,
                                self->floating_surface,
                                self->sel_rect.x,
                                self->sel_rect.y);
    clear_selection_state (self);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
  } else {
    clear_selection_state (self);
    gtk_widget_queue_draw (GTK_WIDGET (self));
  }
}

void
paint_canvas_cancel_selection (PaintCanvas *self)
{
  if (!self)
    return;
  clear_selection_state (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

gboolean
paint_canvas_delete_selection (PaintCanvas *self)
{
  if (!self || !self->has_selection)
    return FALSE;

  if (self->floating_surface) {
    if (self->floating_has_hole) {
      paint_document_clear_rect (self->document,
                                 self->floating_hole.x,
                                 self->floating_hole.y,
                                 self->floating_hole.width,
                                 self->floating_hole.height);
    }
    clear_selection_state (self);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
    return TRUE;
  }

  cairo_rectangle_int_t r = self->sel_rect;
  clear_selection_state (self);
  paint_document_clear_rect (self->document, r.x, r.y, r.width, r.height);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
  return TRUE;
}

gboolean
paint_canvas_crop_to_selection (PaintCanvas *self)
{
  if (!self || !self->has_selection || self->sel_rect.width <= 0 || self->sel_rect.height <= 0)
    return FALSE;

  cairo_rectangle_int_t r = self->sel_rect;
  if (self->floating_surface)
    paint_canvas_commit_selection (self);
  else
    clear_selection_state (self);

  paint_document_crop (self->document, r.x, r.y, r.width, r.height);
  update_size_request (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
  return TRUE;
}

GdkTexture *
paint_canvas_copy_selection (PaintCanvas *self)
{
  if (!self || !self->document)
    return NULL;

  if (self->has_selection) {
    if (self->floating_surface)
      return paint_texture_from_surface (self->floating_surface);
    cairo_surface_t *copy = paint_document_copy_rect (self->document,
                                                      self->sel_rect.x,
                                                      self->sel_rect.y,
                                                      self->sel_rect.width,
                                                      self->sel_rect.height);
    if (!copy)
      return NULL;
    GdkTexture *tex = paint_texture_from_surface (copy);
    cairo_surface_destroy (copy);
    return tex;
  }

  return paint_texture_from_surface (self->document->surface);
}

GdkTexture *
paint_canvas_cut_selection (PaintCanvas *self)
{
  GdkTexture *tex = paint_canvas_copy_selection (self);
  if (!tex)
    return NULL;

  if (self->has_selection)
    paint_canvas_delete_selection (self);
  else
    paint_canvas_clear (self);
  return tex;
}

void
paint_canvas_paste_texture (PaintCanvas *self, GdkTexture *texture)
{
  if (!self || !self->document || !texture)
    return;

  paint_canvas_commit_active_overlays (self);
  cairo_surface_t *surf = paint_surface_from_texture (texture);
  if (!surf)
    return;

  int pw = cairo_image_surface_get_width (surf);
  int ph = cairo_image_surface_get_height (surf);
  double cx = (double) self->document->width * 0.5;
  double cy = (double) self->document->height * 0.5;

  graphene_rect_t vis = compute_visible_widget_rect (self);
  if (vis.size.width > 0.0f && vis.size.height > 0.0f && self->zoom > 0.0) {
    cx = (vis.origin.x + vis.size.width * 0.5) / self->zoom;
    cy = (vis.origin.y + vis.size.height * 0.5) / self->zoom;
  }

  int dst_x = (pw >= self->document->width)
                ? 0
                : CLAMP ((int) lround (cx) - pw / 2, 0, self->document->width - pw);
  int dst_y = (ph >= self->document->height)
                ? 0
                : CLAMP ((int) lround (cy) - ph / 2, 0, self->document->height - ph);

  self->floating_surface = surf;
  self->floating_has_hole = FALSE;
  self->floating_hole = (cairo_rectangle_int_t) { 0, 0, 0, 0 };
  self->has_selection = TRUE;
  self->sel_rect = (cairo_rectangle_int_t) { dst_x, dst_y, pw, ph };
  sync_floating_texture (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_SELECTION_CHANGED], 0);
}

void
paint_canvas_nudge_selection (PaintCanvas *self, int dx, int dy)
{
  if (!self || !self->has_selection)
    return;
  lift_selection_to_floating (self);
  self->sel_rect.x += dx;
  self->sel_rect.y += dy;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

gboolean
paint_canvas_has_active_text (PaintCanvas *self)
{
  return self && self->text_active;
}

const char *
paint_canvas_get_active_text (PaintCanvas *self)
{
  return (self && self->text_content) ? self->text_content : "";
}

void
paint_canvas_set_active_text (PaintCanvas *self, const char *text)
{
  if (!self || !self->text_active)
    return;
  g_free (self->text_content);
  self->text_content = g_strdup (text ? text : "");
  update_text_bounds (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
paint_canvas_commit_text (PaintCanvas *self)
{
  if (!self || !self->text_active)
    return;

  gboolean drew = (self->text_content && *self->text_content);
  if (drew) {
    paint_document_draw_text (self->document,
                              self->text_x,
                              self->text_y,
                              self->text_content,
                              self->settings);
    paint_tool_settings_add_recent_color (self->settings, &self->settings->color);
  }

  self->text_active = FALSE;
  self->text_dragging = FALSE;
  g_clear_pointer (&self->text_content, g_free);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_TEXT_STATE_CHANGED], 0);
  if (drew)
    g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

void
paint_canvas_cancel_text (PaintCanvas *self)
{
  if (!self || !self->text_active)
    return;
  self->text_active = FALSE;
  self->text_dragging = FALSE;
  g_clear_pointer (&self->text_content, g_free);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_TEXT_STATE_CHANGED], 0);
}

void
paint_canvas_commit_active_overlays (PaintCanvas *self)
{
  if (!self)
    return;
  if (self->text_active)
    paint_canvas_commit_text (self);
  if (self->has_selection)
    paint_canvas_commit_selection (self);
}

void
paint_canvas_set_show_cursor_ring (PaintCanvas *self, gboolean show)
{
  self->show_cursor_ring = show;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
paint_canvas_set_show_pixel_grid (PaintCanvas *self, gboolean show)
{
  if (!self)
    return;
  self->show_pixel_grid = show;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

gboolean
paint_canvas_get_show_pixel_grid (PaintCanvas *self)
{
  return self ? self->show_pixel_grid : TRUE;
}

void
paint_canvas_set_alt_eyedropper (PaintCanvas *self, gboolean active)
{
  if (!self || self->alt_held == active)
    return;
  self->alt_held = active;
  update_canvas_cursor (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
paint_canvas_check_viewport (PaintCanvas *self)
{
  if (!self->document || gtk_widget_get_width (GTK_WIDGET (self)) <= 0)
    return;
  if (target_vp_div (self) == 0 && self->ov_texture &&
      self->ov_doc_surface == self->document->surface) {
    return;
  }
  graphene_rect_t vis = compute_visible_widget_rect (self);
  if (vis.size.width > 0.0f && vis.size.height > 0.0f &&
      !is_visible_covered_by_vp_cache (self, &vis)) {
    gtk_widget_queue_draw (GTK_WIDGET (self));
  }
}
