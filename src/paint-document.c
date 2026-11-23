#include "paint-document-private.h"

#include <math.h>
#include <pango/pangocairo.h>
#include <stdint.h>
#include <string.h>

/* ============================================================================
 * Tiled / Full-Surface Undo Snapshots
 * ============================================================================ */

static PaintLayer *
paint_layer_new (const char *name,
                 cairo_surface_t *surface,
                 gboolean visible,
                 double opacity,
                 PaintBlendMode blend_mode)
{
  PaintLayer *layer = g_new0 (PaintLayer, 1);
  layer->name = g_strdup (name ? name : "Layer");
  layer->surface = surface;
  layer->visible = visible;
  layer->opacity = CLAMP (opacity, 0.0, 1.0);
  layer->blend_mode = blend_mode;
  return layer;
}

static void
paint_layer_free (gpointer ptr)
{
  PaintLayer *layer = ptr;
  if (!layer)
    return;
  g_free (layer->name);
  g_clear_pointer (&layer->surface, cairo_surface_destroy);
  g_free (layer);
}

static UndoLayerSnapshot *
undo_layer_snapshot_new (const PaintLayer *layer)
{
  cairo_surface_flush (layer->surface);
  int w = cairo_image_surface_get_width (layer->surface);
  int h = cairo_image_surface_get_height (layer->surface);
  int stride = cairo_image_surface_get_stride (layer->surface);
  const unsigned char *data = cairo_image_surface_get_data (layer->surface);

  UndoLayerSnapshot *snap = g_new0 (UndoLayerSnapshot, 1);
  snap->name = g_strdup (layer->name);
  snap->visible = layer->visible;
  snap->opacity = layer->opacity;
  snap->blend_mode = layer->blend_mode;
  snap->alpha_lock = layer->alpha_lock;
  snap->locked = layer->locked;
  snap->width = w;
  snap->height = h;
  snap->stride = stride;
  snap->pixels = g_bytes_new (data, (gsize) stride * (gsize) h);
  return snap;
}

static void
undo_layer_snapshot_free (gpointer ptr)
{
  UndoLayerSnapshot *snap = ptr;
  if (!snap)
    return;
  g_free (snap->name);
  g_clear_pointer (&snap->pixels, g_bytes_unref);
  g_free (snap);
}

static PaintLayer *
undo_layer_snapshot_restore (const UndoLayerSnapshot *snap)
{
  cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
                                                         snap->width,
                                                         snap->height);
  gsize len = 0;
  const unsigned char *src = g_bytes_get_data (snap->pixels, &len);
  unsigned char *dst = cairo_image_surface_get_data (surface);
  int dst_stride = cairo_image_surface_get_stride (surface);

  if (dst_stride == snap->stride) {
    memcpy (dst, src, len);
  } else {
    int copy_bytes = MIN (dst_stride, snap->stride);
    for (int y = 0; y < snap->height; y++)
      memcpy (dst + y * dst_stride, src + y * snap->stride, (gsize) copy_bytes);
  }
  cairo_surface_mark_dirty (surface);
  PaintLayer *layer = paint_layer_new (snap->name, surface, snap->visible, snap->opacity, snap->blend_mode);
  layer->alpha_lock = snap->alpha_lock;
  layer->locked = snap->locked;
  return layer;
}

static void
undo_tile_free (gpointer ptr)
{
  UndoTile *t = ptr;
  if (!t)
    return;
  g_clear_pointer (&t->pixels, g_bytes_unref);
  g_free (t);
}

static void
undo_step_free (gpointer ptr)
{
  UndoStep *step = ptr;
  if (!step)
    return;
  g_clear_pointer (&step->layer_snaps, g_ptr_array_unref);
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
undo_step_new_tiled (int width, int height, int layer_index)
{
  UndoStep *step = g_new0 (UndoStep, 1);
  step->width = width;
  step->height = height;
  step->is_full = FALSE;
  step->layer_index = layer_index;
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

  int tx0 = x0 / PAINT_UNDO_TILE_SIZE;
  int ty0 = y0 / PAINT_UNDO_TILE_SIZE;
  int tx1 = (x1 - 1) / PAINT_UNDO_TILE_SIZE;
  int ty1 = (y1 - 1) / PAINT_UNDO_TILE_SIZE;

  for (int ty = ty0; ty <= ty1; ty++) {
    for (int tx = tx0; tx <= tx1; tx++) {
      guint key = (((guint) ty) << 16) | (guint) tx;
      if (g_hash_table_contains (step->saved_keys, GUINT_TO_POINTER (key)))
        continue;
      g_hash_table_add (step->saved_keys, GUINT_TO_POINTER (key));

      int tile_x = tx * PAINT_UNDO_TILE_SIZE;
      int tile_y = ty * PAINT_UNDO_TILE_SIZE;
      int tile_w = MIN (PAINT_UNDO_TILE_SIZE, step->width - tile_x);
      int tile_h = MIN (PAINT_UNDO_TILE_SIZE, step->height - tile_y);
      UndoTile *tile = capture_rect_tile (surface, tile_x, tile_y, tile_w, tile_h);
      step->byte_size += g_bytes_get_size (tile->pixels);
      g_ptr_array_add (step->tiles, tile);
    }
  }
}

static UndoStep *
undo_step_new_full_doc (const PaintDocument *doc)
{
  UndoStep *step = g_new0 (UndoStep, 1);
  step->width = doc->width;
  step->height = doc->height;
  step->is_full = TRUE;
  step->background_color = doc->background_color;
  step->active_layer_index = doc->active_layer_index;
  step->next_layer_num = doc->next_layer_num;
  step->layer_snaps = g_ptr_array_new_with_free_func (undo_layer_snapshot_free);
  for (guint i = 0; i < doc->layers->len; i++) {
    const PaintLayer *layer = g_ptr_array_index (doc->layers, i);
    UndoLayerSnapshot *snap = undo_layer_snapshot_new (layer);
    step->byte_size += g_bytes_get_size (snap->pixels);
    g_ptr_array_add (step->layer_snaps, snap);
  }
  return step;
}

/* ============================================================================
 * Compositing & Dirty Tracking
 * ============================================================================ */

PaintLayer *
paint_document_get_active_layer_internal (const PaintDocument *doc)
{
  if (!doc || !doc->layers || doc->layers->len == 0)
    return NULL;
  int idx = CLAMP (doc->active_layer_index, 0, (int) doc->layers->len - 1);
  return g_ptr_array_index (doc->layers, (guint) idx);
}

static GdkRGBA
get_active_layer_clear_color (const PaintDocument *doc)
{
  if (doc && doc->active_layer_index == 0)
    return doc->background_color;
  return (GdkRGBA) { 0.0f, 0.0f, 0.0f, 0.0f };
}

static cairo_operator_t
blend_mode_to_cairo (PaintBlendMode mode)
{
  switch (mode) {
  case PAINT_BLEND_MULTIPLY:
    return CAIRO_OPERATOR_MULTIPLY;
  case PAINT_BLEND_SCREEN:
    return CAIRO_OPERATOR_SCREEN;
  case PAINT_BLEND_OVERLAY:
    return CAIRO_OPERATOR_OVERLAY;
  case PAINT_BLEND_DARKEN:
    return CAIRO_OPERATOR_DARKEN;
  case PAINT_BLEND_LIGHTEN:
    return CAIRO_OPERATOR_LIGHTEN;
  case PAINT_BLEND_NORMAL:
  default:
    return CAIRO_OPERATOR_OVER;
  }
}

static gboolean
needs_composite_surface (const PaintDocument *doc)
{
  if (!doc || !doc->layers || doc->layers->len != 1 || doc->has_active_hole)
    return TRUE;
  const PaintLayer *l0 = g_ptr_array_index (doc->layers, 0);
  return !l0->visible || l0->opacity < 0.999 || l0->blend_mode != PAINT_BLEND_NORMAL;
}

static void
composite_region_into (const PaintDocument *doc,
                       cairo_surface_t *dst,
                       int rx,
                       int ry,
                       int rw,
                       int rh)
{
  int x0 = CLAMP (rx, 0, doc->width);
  int y0 = CLAMP (ry, 0, doc->height);
  int x1 = CLAMP (rx + rw, 0, doc->width);
  int y1 = CLAMP (ry + rh, 0, doc->height);
  int cw = x1 - x0;
  int ch = y1 - y0;
  if (cw <= 0 || ch <= 0)
    return;

  cairo_t *cr = cairo_create (dst);
  cairo_rectangle (cr, (double) x0, (double) y0, (double) cw, (double) ch);
  cairo_clip (cr);
  cairo_set_operator (cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint (cr);

  for (guint i = 0; i < doc->layers->len; i++) {
    const PaintLayer *layer = g_ptr_array_index (doc->layers, i);
    if (!layer->visible || layer->opacity <= 0.001)
      continue;

    cairo_save (cr);
    if (doc->has_active_hole && (int) i == doc->active_layer_index) {
      int hx0 = MAX (x0, doc->active_hole.x);
      int hy0 = MAX (y0, doc->active_hole.y);
      int hx1 = MIN (x1, doc->active_hole.x + doc->active_hole.width);
      int hy1 = MIN (y1, doc->active_hole.y + doc->active_hole.height);
      if (hx1 > hx0 && hy1 > hy0) {
        if (doc->active_hole_mask) {
          cairo_push_group (cr);
          cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
          cairo_set_source_surface (cr, layer->surface, 0.0, 0.0);
          cairo_paint (cr);
          if (i == 0 && doc->background_color.alpha > 0.0f) {
            cairo_set_source_rgba (cr,
                                   doc->background_color.red,
                                   doc->background_color.green,
                                   doc->background_color.blue,
                                   doc->background_color.alpha);
            cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
          } else {
            cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 1.0);
            cairo_set_operator (cr, CAIRO_OPERATOR_DEST_OUT);
          }
          cairo_mask_surface (cr, doc->active_hole_mask,
                              (double) doc->active_hole.x,
                              (double) doc->active_hole.y);
          cairo_pop_group_to_source (cr);
          cairo_set_operator (cr, blend_mode_to_cairo (layer->blend_mode));
          if (layer->opacity >= 0.999)
            cairo_paint (cr);
          else
            cairo_paint_with_alpha (cr, layer->opacity);
          cairo_restore (cr);
          continue;
        }

        if (i == 0 && doc->background_color.alpha > 0.0f) {
          cairo_save (cr);
          cairo_rectangle (cr, (double) hx0, (double) hy0,
                           (double) (hx1 - hx0), (double) (hy1 - hy0));
          cairo_clip (cr);
          cairo_set_source_rgba (cr,
                                 doc->background_color.red,
                                 doc->background_color.green,
                                 doc->background_color.blue,
                                 doc->background_color.alpha);
          cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
          cairo_paint (cr);
          cairo_restore (cr);
        }

        cairo_set_fill_rule (cr, CAIRO_FILL_RULE_EVEN_ODD);
        cairo_rectangle (cr, (double) x0, (double) y0, (double) cw, (double) ch);
        cairo_rectangle (cr, (double) hx0, (double) hy0,
                         (double) (hx1 - hx0), (double) (hy1 - hy0));
        cairo_clip (cr);
      }
    }

    cairo_set_operator (cr, blend_mode_to_cairo (layer->blend_mode));
    cairo_set_source_surface (cr, layer->surface, 0.0, 0.0);
    if (layer->opacity >= 0.999)
      cairo_paint (cr);
    else
      cairo_paint_with_alpha (cr, layer->opacity);
    cairo_restore (cr);
  }
  cairo_destroy (cr);
}

void
paint_document_recomposite_all (PaintDocument *doc)
{
  if (!doc || !doc->layers || doc->layers->len == 0)
    return;

  if (!needs_composite_surface (doc)) {
    g_clear_pointer (&doc->composite_surface, cairo_surface_destroy);
    PaintLayer *l0 = g_ptr_array_index (doc->layers, 0);
    doc->surface = l0->surface;
    return;
  }

  if (!doc->composite_surface ||
      cairo_image_surface_get_width (doc->composite_surface) != doc->width ||
      cairo_image_surface_get_height (doc->composite_surface) != doc->height) {
    g_clear_pointer (&doc->composite_surface, cairo_surface_destroy);
    doc->composite_surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
                                                         doc->width,
                                                         doc->height);
  }
  doc->surface = doc->composite_surface;
  composite_region_into (doc, doc->composite_surface, 0, 0, doc->width, doc->height);
}

void
paint_document_recomposite_rect (PaintDocument *doc, int rx, int ry, int rw, int rh)
{
  if (!doc || !doc->layers || doc->layers->len == 0)
    return;

  if (needs_composite_surface (doc)) {
    if (!doc->composite_surface ||
        cairo_image_surface_get_width (doc->composite_surface) != doc->width ||
        cairo_image_surface_get_height (doc->composite_surface) != doc->height) {
      paint_document_recomposite_all (doc);
      paint_document_mark_all_dirty (doc);
      return;
    }
    doc->surface = doc->composite_surface;
    composite_region_into (doc, doc->composite_surface, rx, ry, rw, rh);
  } else if (doc->composite_surface) {
    paint_document_recomposite_all (doc);
    paint_document_mark_all_dirty (doc);
  } else {
    PaintLayer *l0 = g_ptr_array_index (doc->layers, 0);
    doc->surface = l0->surface;
  }
}

void
paint_document_mark_all_dirty (PaintDocument *doc)
{
  doc->all_dirty = TRUE;
  doc->has_dirty_rect = FALSE;
}

void
paint_document_mark_rect_dirty (PaintDocument *doc, int x, int y, int w, int h)
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
enforce_undo_budget (PaintDocument *doc)
{
  gsize budget = doc->undo_budget_bytes > 0 ? doc->undo_budget_bytes : PAINT_UNDO_BUDGET_BYTES;
  while (doc->undo_stack->len > PAINT_UNDO_MAX_STEPS ||
         (doc->undo_bytes > budget && doc->undo_stack->len > PAINT_UNDO_MIN_STEPS)) {
    const UndoStep *oldest = g_ptr_array_index (doc->undo_stack, 0);
    doc->undo_bytes = (doc->undo_bytes >= oldest->byte_size)
                        ? (doc->undo_bytes - oldest->byte_size)
                        : 0;
    g_ptr_array_remove_index (doc->undo_stack, 0);
  }
}

static void
push_undo_step (PaintDocument *doc, UndoStep *step)
{
  doc->redo_bytes = 0;
  g_ptr_array_set_size (doc->redo_stack, 0);
  g_ptr_array_add (doc->undo_stack, step);
  doc->undo_bytes += step->byte_size;
  enforce_undo_budget (doc);
}

void
paint_document_flush_active_stroke_undo (PaintDocument *doc)
{
  if (doc->active_stroke_undo) {
    push_undo_step (doc, doc->active_stroke_undo);
    doc->active_stroke_undo = NULL;
  }
}

void
paint_document_push_full_undo (PaintDocument *doc)
{
  paint_document_flush_active_stroke_undo (doc);
  push_undo_step (doc, undo_step_new_full_doc (doc));
}

void
paint_document_set_file_internal (PaintDocument *doc, GFile *file)
{
  if (!doc)
    return;
  g_set_object (&doc->file, file);
}

void
paint_document_set_background_color_internal (PaintDocument *doc, const GdkRGBA *bg)
{
  if (!doc || !bg)
    return;
  doc->background_color = *bg;
}

void
paint_document_append_layer_surface_internal (PaintDocument *doc,
                                              const char *name,
                                              cairo_surface_t *surface,
                                              gboolean visible,
                                              double opacity,
                                              PaintBlendMode blend_mode)
{
  if (!doc || !doc->layers || !surface)
    return;
  g_ptr_array_add (doc->layers,
                   paint_layer_new (name, surface, visible, opacity, blend_mode));
}

void
paint_document_clear_layers_internal (PaintDocument *doc)
{
  if (!doc || !doc->layers)
    return;
  g_ptr_array_set_size (doc->layers, 0);
}

cairo_surface_t *
paint_document_create_blank_surface (int width, int height, const GdkRGBA *bg)
{
  cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create (surface);
  cairo_set_source_rgba (cr, bg->red, bg->green, bg->blue, bg->alpha);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_paint (cr);
  cairo_destroy (cr);
  return surface;
}

PaintDocument *
paint_document_new_with_background_internal (int width, int height, const GdkRGBA *bg)
{
  PaintDocument *doc = g_new0 (PaintDocument, 1);
  doc->width = MAX (1, width);
  doc->height = MAX (1, height);
  doc->background_color = *bg;
  doc->layers = g_ptr_array_new_with_free_func (paint_layer_free);
  cairo_surface_t *bg_surf = paint_document_create_blank_surface (doc->width,
                                                                  doc->height,
                                                                  &doc->background_color);
  g_ptr_array_add (doc->layers,
                   paint_layer_new ("Background", bg_surf, TRUE, 1.0, PAINT_BLEND_NORMAL));
  doc->active_layer_index = 0;
  doc->next_layer_num = 1;
  doc->layers_revision = 1;
  doc->surface = bg_surf;
  doc->undo_stack = g_ptr_array_new_with_free_func (undo_step_free);
  doc->redo_stack = g_ptr_array_new_with_free_func (undo_step_free);
  doc->undo_budget_bytes = PAINT_UNDO_BUDGET_BYTES;
  doc->all_dirty = TRUE;
  return doc;
}

PaintDocument *
paint_document_new (int width, int height)
{
  GdkRGBA white = { 1.0f, 1.0f, 1.0f, 1.0f };
  return paint_document_new_with_background_internal (width, height, &white);
}

PaintDocument *
paint_document_new_transparent (int width, int height)
{
  GdkRGBA transparent = { 0.0f, 0.0f, 0.0f, 0.0f };
  return paint_document_new_with_background_internal (width, height, &transparent);
}

void
paint_document_free (PaintDocument *doc)
{
  if (!doc)
    return;
  g_clear_pointer (&doc->active_stroke_undo, undo_step_free);
  g_clear_pointer (&doc->active_hole_mask, cairo_surface_destroy);
  g_clear_pointer (&doc->composite_surface, cairo_surface_destroy);
  doc->surface = NULL;
  g_clear_pointer (&doc->layers, g_ptr_array_unref);
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
paint_document_has_layer_structure (const PaintDocument *doc)
{
  if (!doc || !doc->layers || doc->layers->len == 0)
    return FALSE;
  if (doc->layers->len > 1)
    return TRUE;
  const PaintLayer *l0 = g_ptr_array_index (doc->layers, 0);
  if (!l0->visible || l0->opacity < 0.999 || l0->blend_mode != PAINT_BLEND_NORMAL ||
      l0->alpha_lock || l0->locked) {
    return TRUE;
  }
  if (g_strcmp0 (l0->name, "Background") != 0)
    return TRUE;
  return FALSE;
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

gsize
paint_document_get_undo_memory_bytes (const PaintDocument *doc)
{
  if (!doc)
    return 0;
  gsize total = doc->undo_bytes;
  if (doc->active_stroke_undo)
    total += doc->active_stroke_undo->byte_size;
  return total;
}

guint
paint_document_get_undo_depth (const PaintDocument *doc)
{
  if (!doc)
    return 0;
  return doc->undo_stack->len + (doc->active_stroke_undo ? 1U : 0U);
}

void
paint_document_set_undo_budget_bytes (PaintDocument *doc, gsize budget_bytes)
{
  if (!doc)
    return;
  doc->undo_budget_bytes = budget_bytes > 0 ? budget_bytes : PAINT_UNDO_BUDGET_BYTES;
  enforce_undo_budget (doc);
}

static UndoStep *
apply_undo_step (PaintDocument *doc, const UndoStep *step)
{
  if (step->is_full) {
    UndoStep *reverse = undo_step_new_full_doc (doc);
    doc->has_active_hole = FALSE;
    doc->active_hole = (cairo_rectangle_int_t) { 0, 0, 0, 0 };
    g_clear_pointer (&doc->active_hole_mask, cairo_surface_destroy);
    g_ptr_array_set_size (doc->layers, 0);
    for (guint i = 0; i < step->layer_snaps->len; i++) {
      const UndoLayerSnapshot *snap = g_ptr_array_index (step->layer_snaps, i);
      g_ptr_array_add (doc->layers, undo_layer_snapshot_restore (snap));
    }
    doc->width = step->width;
    doc->height = step->height;
    doc->background_color = step->background_color;
    doc->active_layer_index = CLAMP (step->active_layer_index, 0, MAX (0, (int) doc->layers->len - 1));
    doc->next_layer_num = step->next_layer_num;
    g_clear_pointer (&doc->composite_surface, cairo_surface_destroy);
    paint_document_recomposite_all (doc);
    doc->layers_revision++;
    paint_document_mark_all_dirty (doc);
    return reverse;
  }

  int idx = CLAMP (step->layer_index, 0, MAX (0, (int) doc->layers->len - 1));
  PaintLayer *layer = g_ptr_array_index (doc->layers, (guint) idx);
  UndoStep *reverse = undo_step_new_tiled (doc->width, doc->height, idx);
  for (guint i = 0; i < step->tiles->len; i++) {
    const UndoTile *t = g_ptr_array_index (step->tiles, i);
    UndoTile *rev_tile = capture_rect_tile (layer->surface, t->x, t->y, t->w, t->h);
    reverse->byte_size += g_bytes_get_size (rev_tile->pixels);
    g_ptr_array_add (reverse->tiles, rev_tile);
    restore_rect_tile (layer->surface, t);
    paint_document_recomposite_rect (doc, t->x, t->y, t->w, t->h);
    paint_document_mark_rect_dirty (doc, t->x, t->y, t->w, t->h);
  }
  cairo_surface_mark_dirty (layer->surface);
  if (doc->active_layer_index != idx) {
    doc->active_layer_index = idx;
    doc->layers_revision++;
  }
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
  doc->undo_bytes = (doc->undo_bytes >= step->byte_size)
                      ? (doc->undo_bytes - step->byte_size)
                      : 0;
  UndoStep *reverse = apply_undo_step (doc, step);
  doc->redo_bytes += reverse->byte_size;
  g_ptr_array_add (doc->redo_stack, reverse);
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
  doc->redo_bytes = (doc->redo_bytes >= step->byte_size)
                      ? (doc->redo_bytes - step->byte_size)
                      : 0;
  UndoStep *reverse = apply_undo_step (doc, step);
  doc->undo_bytes += reverse->byte_size;
  g_ptr_array_add (doc->undo_stack, reverse);
  g_ptr_array_remove_index (doc->redo_stack, last);
  enforce_undo_budget (doc);
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

void
paint_document_compute_tool_stroke_bounds (const PaintTool *tool,
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
  paint_document_flush_active_stroke_undo (doc);

  PaintLayer *active = paint_document_get_active_layer_internal (doc);
  if (!active)
    return;

  doc->pre_stroke_modified = doc->modified;
  doc->stroke_start_x = x;
  doc->stroke_start_y = y;
  doc->prev_stroke_x = x;
  doc->prev_stroke_y = y;
  doc->prev2_stroke_x = x;
  doc->prev2_stroke_y = y;

  GdkRGBA prev_bg = settings->background_color;
  gboolean prev_alpha_lock = settings->target_alpha_lock;
  cairo_surface_t *prev_sample = settings->fill_sample_surface;
  settings->background_color = get_active_layer_clear_color (doc);
  settings->target_alpha_lock = active->alpha_lock;
  settings->fill_sample_surface = settings->fill_sample_all_layers ? doc->surface : NULL;

  if (tool->draw_preview) {
    cairo_t *cr = cairo_create (active->surface);
    tool->begin_stroke (tool, cr, x, y, settings);
    cairo_destroy (cr);
    settings->background_color = prev_bg;
    settings->target_alpha_lock = prev_alpha_lock;
    settings->fill_sample_surface = prev_sample;
    return;
  }

  if (tool->uses_brush_size) {
    int rx, ry, rw, rh;
    compute_stroke_bounds (x, y, x, y, settings->size, &rx, &ry, &rw, &rh);

    doc->active_stroke_undo = undo_step_new_tiled (doc->width, doc->height, doc->active_layer_index);
    undo_step_capture_rect (doc->active_stroke_undo, active->surface, rx, ry, rw, rh);

    cairo_t *cr = cairo_create (active->surface);
    tool->begin_stroke (tool, cr, x, y, settings);
    cairo_destroy (cr);
    paint_document_recomposite_rect (doc, rx, ry, rw, rh);
    paint_document_mark_rect_dirty (doc, rx, ry, rw, rh);
  } else {
    push_undo_step (doc, undo_step_new_full_doc (doc));
    cairo_t *cr = cairo_create (active->surface);
    tool->begin_stroke (tool, cr, x, y, settings);
    cairo_destroy (cr);
    paint_document_recomposite_all (doc);
    paint_document_mark_all_dirty (doc);
  }

  settings->background_color = prev_bg;
  settings->target_alpha_lock = prev_alpha_lock;
  settings->fill_sample_surface = prev_sample;
  doc->modified = TRUE;
}

void
paint_document_continue_stroke (PaintDocument *doc,
                                double x,
                                double y,
                                const PaintTool *tool,
                                PaintToolSettings *settings)
{
  PaintLayer *active = paint_document_get_active_layer_internal (doc);
  if (!active)
    return;

  GdkRGBA prev_bg = settings->background_color;
  gboolean prev_alpha_lock = settings->target_alpha_lock;
  cairo_surface_t *prev_sample = settings->fill_sample_surface;
  settings->background_color = get_active_layer_clear_color (doc);
  settings->target_alpha_lock = active->alpha_lock;
  settings->fill_sample_surface = settings->fill_sample_all_layers ? doc->surface : NULL;

  if (tool->draw_preview) {
    cairo_t *cr = cairo_create (active->surface);
    tool->continue_stroke (tool, cr, x, y, settings);
    cairo_destroy (cr);
    settings->background_color = prev_bg;
    settings->target_alpha_lock = prev_alpha_lock;
    settings->fill_sample_surface = prev_sample;
    return;
  }

  if (tool->uses_brush_size) {
    if (!doc->active_stroke_undo)
      doc->active_stroke_undo = undo_step_new_tiled (doc->width, doc->height, doc->active_layer_index);

    int pad = (int) ceil (settings->size / 2.0) + 4;
    double min_x = MIN (MIN (doc->prev2_stroke_x, doc->prev_stroke_x), x);
    double max_x = MAX (MAX (doc->prev2_stroke_x, doc->prev_stroke_x), x);
    double min_y = MIN (MIN (doc->prev2_stroke_y, doc->prev_stroke_y), y);
    double max_y = MAX (MAX (doc->prev2_stroke_y, doc->prev_stroke_y), y);

    int rx = (int) floor (min_x) - pad;
    int ry = (int) floor (min_y) - pad;
    int rw = (int) ceil (max_x - min_x) + pad * 2 + 2;
    int rh = (int) ceil (max_y - min_y) + pad * 2 + 2;

    undo_step_capture_rect (doc->active_stroke_undo, active->surface, rx, ry, rw, rh);

    cairo_t *cr = cairo_create (active->surface);
    tool->continue_stroke (tool, cr, x, y, settings);
    cairo_destroy (cr);
    paint_document_recomposite_rect (doc, rx, ry, rw, rh);
    paint_document_mark_rect_dirty (doc, rx, ry, rw, rh);

    doc->prev2_stroke_x = doc->prev_stroke_x;
    doc->prev2_stroke_y = doc->prev_stroke_y;
    doc->prev_stroke_x = x;
    doc->prev_stroke_y = y;
  } else {
    cairo_t *cr = cairo_create (active->surface);
    tool->continue_stroke (tool, cr, x, y, settings);
    cairo_destroy (cr);
    paint_document_recomposite_all (doc);
    paint_document_mark_all_dirty (doc);
  }

  settings->background_color = prev_bg;
  settings->target_alpha_lock = prev_alpha_lock;
  settings->fill_sample_surface = prev_sample;
  doc->modified = TRUE;
}

void
paint_document_end_stroke (PaintDocument *doc,
                           double x,
                           double y,
                           const PaintTool *tool,
                           PaintToolSettings *settings)
{
  PaintLayer *active = paint_document_get_active_layer_internal (doc);
  if (!active)
    return;

  GdkRGBA prev_bg = settings->background_color;
  gboolean prev_alpha_lock = settings->target_alpha_lock;
  cairo_surface_t *prev_sample = settings->fill_sample_surface;
  settings->background_color = get_active_layer_clear_color (doc);
  settings->target_alpha_lock = active->alpha_lock;
  settings->fill_sample_surface = settings->fill_sample_all_layers ? doc->surface : NULL;

  if (tool->draw_preview) {
    int rx, ry, rw, rh;
    paint_document_compute_tool_stroke_bounds (tool, settings,
                                               doc->stroke_start_x, doc->stroke_start_y,
                                               x, y, &rx, &ry, &rw, &rh);
    int cx0 = CLAMP (rx, 0, doc->width);
    int cy0 = CLAMP (ry, 0, doc->height);
    int cx1 = CLAMP (rx + rw, 0, doc->width);
    int cy1 = CLAMP (ry + rh, 0, doc->height);
    rx = cx0;
    ry = cy0;
    rw = cx1 - cx0;
    rh = cy1 - cy0;

    UndoStep *step = undo_step_new_tiled (doc->width, doc->height, doc->active_layer_index);
    undo_step_capture_rect (step, active->surface, rx, ry, rw, rh);

    cairo_t *cr = cairo_create (active->surface);
    tool->end_stroke (tool, cr, x, y, settings);
    cairo_destroy (cr);
    paint_document_recomposite_rect (doc, rx, ry, rw, rh);
    paint_document_mark_rect_dirty (doc, rx, ry, rw, rh);

    push_undo_step (doc, step);
    settings->background_color = prev_bg;
    settings->target_alpha_lock = prev_alpha_lock;
    settings->fill_sample_surface = prev_sample;
    doc->modified = TRUE;
    return;
  }

  if (tool->uses_brush_size) {
    int rx, ry, rw, rh;
    compute_stroke_bounds (doc->prev_stroke_x, doc->prev_stroke_y, x, y,
                           settings->size, &rx, &ry, &rw, &rh);

    if (doc->active_stroke_undo)
      undo_step_capture_rect (doc->active_stroke_undo, active->surface, rx, ry, rw, rh);

    cairo_t *cr = cairo_create (active->surface);
    tool->end_stroke (tool, cr, x, y, settings);
    cairo_destroy (cr);
    paint_document_recomposite_rect (doc, rx, ry, rw, rh);
    paint_document_mark_rect_dirty (doc, rx, ry, rw, rh);

    if (doc->active_stroke_undo) {
      push_undo_step (doc, doc->active_stroke_undo);
      doc->active_stroke_undo = NULL;
    }
  } else {
    cairo_t *cr = cairo_create (active->surface);
    tool->end_stroke (tool, cr, x, y, settings);
    cairo_destroy (cr);
    paint_document_recomposite_all (doc);
    paint_document_mark_all_dirty (doc);
  }

  settings->background_color = prev_bg;
  settings->target_alpha_lock = prev_alpha_lock;
  settings->fill_sample_surface = prev_sample;
  doc->modified = TRUE;
}

void
paint_document_cancel_stroke (PaintDocument *doc)
{
  if (!doc || !doc->active_stroke_undo)
    return;

  UndoStep *step = doc->active_stroke_undo;
  doc->active_stroke_undo = NULL;

  int idx = CLAMP (step->layer_index, 0, MAX (0, (int) doc->layers->len - 1));
  PaintLayer *layer = g_ptr_array_index (doc->layers, (guint) idx);

  for (guint i = 0; i < step->tiles->len; i++) {
    const UndoTile *t = g_ptr_array_index (step->tiles, i);
    restore_rect_tile (layer->surface, t);
    paint_document_recomposite_rect (doc, t->x, t->y, t->w, t->h);
    paint_document_mark_rect_dirty (doc, t->x, t->y, t->w, t->h);
  }
  cairo_surface_mark_dirty (layer->surface);
  undo_step_free (step);
  doc->modified = doc->pre_stroke_modified;
}

void
paint_document_clear (PaintDocument *doc)
{
  PaintLayer *active = paint_document_get_active_layer_internal (doc);
  if (!active)
    return;
  paint_document_flush_active_stroke_undo (doc);
  push_undo_step (doc, undo_step_new_full_doc (doc));

  GdkRGBA clr = get_active_layer_clear_color (doc);
  cairo_t *cr = cairo_create (active->surface);
  cairo_set_source_rgba (cr, clr.red, clr.green, clr.blue, clr.alpha);
  cairo_set_operator (cr, active->alpha_lock ? CAIRO_OPERATOR_ATOP : CAIRO_OPERATOR_SOURCE);
  cairo_paint (cr);
  cairo_destroy (cr);

  paint_document_recomposite_all (doc);
  doc->modified = TRUE;
  paint_document_mark_all_dirty (doc);
}

void
paint_document_clear_transparent (PaintDocument *doc)
{
  PaintLayer *active = paint_document_get_active_layer_internal (doc);
  if (!active || active->alpha_lock)
    return;
  paint_document_flush_active_stroke_undo (doc);
  push_undo_step (doc, undo_step_new_full_doc (doc));
  if (doc->active_layer_index == 0)
    doc->background_color = (GdkRGBA) { 0.0f, 0.0f, 0.0f, 0.0f };

  cairo_t *cr = cairo_create (active->surface);
  cairo_set_operator (cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint (cr);
  cairo_destroy (cr);

  paint_document_recomposite_all (doc);
  doc->modified = TRUE;
  paint_document_mark_all_dirty (doc);
}

void
paint_document_resize (PaintDocument *doc, int new_width, int new_height)
{
  int w = MAX (1, new_width);
  int h = MAX (1, new_height);
  if (w == doc->width && h == doc->height)
    return;

  paint_document_flush_active_stroke_undo (doc);
  push_undo_step (doc, undo_step_new_full_doc (doc));

  GdkRGBA transparent = { 0.0f, 0.0f, 0.0f, 0.0f };
  for (guint i = 0; i < doc->layers->len; i++) {
    PaintLayer *layer = g_ptr_array_index (doc->layers, i);
    const GdkRGBA *pad_bg = (i == 0) ? &doc->background_color : &transparent;
    cairo_surface_t *new_surface = paint_document_create_blank_surface (w, h, pad_bg);
    cairo_t *cr = cairo_create (new_surface);
    cairo_rectangle (cr, 0.0, 0.0, (double) MIN (w, doc->width), (double) MIN (h, doc->height));
    cairo_clip (cr);
    cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface (cr, layer->surface, 0, 0);
    cairo_paint (cr);
    cairo_destroy (cr);
    cairo_surface_destroy (layer->surface);
    layer->surface = new_surface;
  }

  g_clear_pointer (&doc->composite_surface, cairo_surface_destroy);
  doc->width = w;
  doc->height = h;
  doc->layers_revision++;
  paint_document_recomposite_all (doc);
  doc->modified = TRUE;
  paint_document_mark_all_dirty (doc);
}

void
paint_document_scale (PaintDocument *doc, int new_width, int new_height)
{
  int w = MAX (1, new_width);
  int h = MAX (1, new_height);
  if (w == doc->width && h == doc->height)
    return;

  paint_document_flush_active_stroke_undo (doc);
  push_undo_step (doc, undo_step_new_full_doc (doc));

  double sx = (double) w / (double) doc->width;
  double sy = (double) h / (double) doc->height;
  for (guint i = 0; i < doc->layers->len; i++) {
    PaintLayer *layer = g_ptr_array_index (doc->layers, i);
    cairo_surface_t *new_surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create (new_surface);
    cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
    cairo_scale (cr, sx, sy);
    cairo_set_source_surface (cr, layer->surface, 0.0, 0.0);
    cairo_pattern_set_filter (cairo_get_source (cr), CAIRO_FILTER_BEST);
    cairo_paint (cr);
    cairo_destroy (cr);
    cairo_surface_destroy (layer->surface);
    layer->surface = new_surface;
  }

  g_clear_pointer (&doc->composite_surface, cairo_surface_destroy);
  doc->width = w;
  doc->height = h;
  doc->layers_revision++;
  paint_document_recomposite_all (doc);
  doc->modified = TRUE;
  paint_document_mark_all_dirty (doc);
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

  paint_document_flush_active_stroke_undo (doc);
  push_undo_step (doc, undo_step_new_full_doc (doc));

  for (guint i = 0; i < doc->layers->len; i++) {
    PaintLayer *layer = g_ptr_array_index (doc->layers, i);
    cairo_surface_t *new_surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, cw, ch);
    cairo_t *cr = cairo_create (new_surface);
    cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface (cr, layer->surface, (double) -x0, (double) -y0);
    cairo_paint (cr);
    cairo_destroy (cr);
    cairo_surface_destroy (layer->surface);
    layer->surface = new_surface;
  }

  g_clear_pointer (&doc->composite_surface, cairo_surface_destroy);
  doc->width = cw;
  doc->height = ch;
  doc->layers_revision++;
  paint_document_recomposite_all (doc);
  doc->modified = TRUE;
  paint_document_mark_all_dirty (doc);
}

gboolean
paint_document_trim_transparent (PaintDocument *doc)
{
  if (!doc)
    return FALSE;
  cairo_surface_t *comp = paint_document_get_surface (doc);
  cairo_surface_flush (comp);
  int w = doc->width;
  int h = doc->height;
  int stride_px = cairo_image_surface_get_stride (comp) / 4;
  const uint32_t *px = (const uint32_t *) cairo_image_surface_get_data (comp);

  int min_x = w, min_y = h, max_x = -1, max_y = -1;
  for (int y = 0; y < h; y++) {
    const uint32_t *row = px + (gsize) y * (gsize) stride_px;
    for (int x = 0; x < w; x++) {
      if (((row[x] >> 24) & 0xFFU) > 0U) {
        if (x < min_x) min_x = x;
        if (x > max_x) max_x = x;
        if (y < min_y) min_y = y;
        if (y > max_y) max_y = y;
      }
    }
  }

  if (max_x < min_x || max_y < min_y)
    return FALSE;
  int cw = max_x - min_x + 1;
  int ch = max_y - min_y + 1;
  if (min_x == 0 && min_y == 0 && cw == w && ch == h)
    return FALSE;

  paint_document_crop (doc, min_x, min_y, cw, ch);
  return TRUE;
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
  paint_document_flush_active_stroke_undo (doc);
  push_undo_step (doc, undo_step_new_full_doc (doc));

  for (guint i = 0; i < doc->layers->len; i++) {
    PaintLayer *layer = g_ptr_array_index (doc->layers, i);
    cairo_surface_t *rot = paint_surface_rotate_90 (layer->surface, clockwise);
    cairo_surface_destroy (layer->surface);
    layer->surface = rot;
  }

  g_clear_pointer (&doc->composite_surface, cairo_surface_destroy);
  PaintLayer *l0 = g_ptr_array_index (doc->layers, 0);
  doc->width = cairo_image_surface_get_width (l0->surface);
  doc->height = cairo_image_surface_get_height (l0->surface);
  doc->layers_revision++;
  paint_document_recomposite_all (doc);
  doc->modified = TRUE;
  paint_document_mark_all_dirty (doc);
}

void
paint_document_flip (PaintDocument *doc, gboolean horizontal)
{
  paint_document_flush_active_stroke_undo (doc);
  push_undo_step (doc, undo_step_new_full_doc (doc));

  for (guint i = 0; i < doc->layers->len; i++) {
    PaintLayer *layer = g_ptr_array_index (doc->layers, i);
    cairo_surface_t *flipped = paint_surface_flip (layer->surface, horizontal);
    cairo_surface_destroy (layer->surface);
    layer->surface = flipped;
  }

  paint_document_recomposite_all (doc);
  doc->modified = TRUE;
  paint_document_mark_all_dirty (doc);
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

static cairo_surface_t *
copy_surface_masked_rect (cairo_surface_t *src_surf,
                          int doc_w,
                          int doc_h,
                          const cairo_rectangle_int_t *rect,
                          cairo_surface_t *mask)
{
  if (!src_surf || !rect)
    return NULL;
  int x0 = CLAMP (rect->x, 0, doc_w);
  int y0 = CLAMP (rect->y, 0, doc_h);
  int x1 = CLAMP (rect->x + rect->width, 0, doc_w);
  int y1 = CLAMP (rect->y + rect->height, 0, doc_h);
  int cw = x1 - x0;
  int ch = y1 - y0;
  if (cw <= 0 || ch <= 0)
    return NULL;

  cairo_surface_t *copy = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, cw, ch);
  cairo_t *cr = cairo_create (copy);
  cairo_set_source_surface (cr, src_surf, (double) -x0, (double) -y0);
  if (mask) {
    cairo_set_operator (cr, CAIRO_OPERATOR_OVER);
    cairo_mask_surface (cr, mask, (double) (rect->x - x0), (double) (rect->y - y0));
  } else {
    cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint (cr);
  }
  cairo_destroy (cr);
  return copy;
}

cairo_surface_t *
paint_document_copy_rect (const PaintDocument *doc, int x, int y, int w, int h)
{
  cairo_rectangle_int_t r = { x, y, w, h };
  return paint_document_copy_masked_rect (doc, &r, NULL);
}

cairo_surface_t *
paint_document_copy_masked_rect (const PaintDocument *doc,
                                 const cairo_rectangle_int_t *rect,
                                 cairo_surface_t *mask)
{
  PaintLayer *active = paint_document_get_active_layer_internal (doc);
  if (!active || !active->surface)
    return NULL;
  return copy_surface_masked_rect (active->surface, doc->width, doc->height, rect, mask);
}

cairo_surface_t *
paint_document_copy_merged_rect (const PaintDocument *doc, int x, int y, int w, int h)
{
  cairo_rectangle_int_t r = { x, y, w, h };
  return paint_document_copy_merged_masked_rect (doc, &r, NULL);
}

cairo_surface_t *
paint_document_copy_merged_masked_rect (const PaintDocument *doc,
                                        const cairo_rectangle_int_t *rect,
                                        cairo_surface_t *mask)
{
  if (!doc || !doc->surface)
    return NULL;
  return copy_surface_masked_rect (doc->surface, doc->width, doc->height, rect, mask);
}

static void
fill_layer_masked_rect_with_clear_color (const PaintDocument *doc,
                                          PaintLayer *layer,
                                          int x0,
                                          int y0,
                                          int w,
                                          int h,
                                          const cairo_rectangle_int_t *mask_origin,
                                          cairo_surface_t *mask)
{
  GdkRGBA clr = get_active_layer_clear_color (doc);
  cairo_t *cr = cairo_create (layer->surface);
  cairo_rectangle (cr, (double) x0, (double) y0, (double) w, (double) h);
  cairo_clip (cr);
  cairo_set_source_rgba (cr, clr.red, clr.green, clr.blue, clr.alpha);
  cairo_set_operator (cr, layer->alpha_lock ? CAIRO_OPERATOR_ATOP : CAIRO_OPERATOR_SOURCE);
  if (mask && mask_origin)
    cairo_mask_surface (cr, mask, (double) mask_origin->x, (double) mask_origin->y);
  else
    cairo_paint (cr);
  cairo_destroy (cr);
}

void
paint_document_clear_rect (PaintDocument *doc, int x, int y, int w, int h)
{
  cairo_rectangle_int_t r = { x, y, w, h };
  paint_document_clear_masked_rect (doc, &r, NULL);
}

void
paint_document_clear_masked_rect (PaintDocument *doc,
                                  const cairo_rectangle_int_t *rect,
                                  cairo_surface_t *mask)
{
  PaintLayer *active = paint_document_get_active_layer_internal (doc);
  if (!active || !rect)
    return;
  int x0 = CLAMP (rect->x, 0, doc->width);
  int y0 = CLAMP (rect->y, 0, doc->height);
  int x1 = CLAMP (rect->x + rect->width, 0, doc->width);
  int y1 = CLAMP (rect->y + rect->height, 0, doc->height);
  if (x1 <= x0 || y1 <= y0)
    return;

  paint_document_flush_active_stroke_undo (doc);
  UndoStep *step = undo_step_new_tiled (doc->width, doc->height, doc->active_layer_index);
  undo_step_capture_rect (step, active->surface, x0, y0, x1 - x0, y1 - y0);
  fill_layer_masked_rect_with_clear_color (doc, active, x0, y0, x1 - x0, y1 - y0, rect, mask);
  paint_document_recomposite_rect (doc, x0, y0, x1 - x0, y1 - y0);
  paint_document_mark_rect_dirty (doc, x0, y0, x1 - x0, y1 - y0);
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
  cairo_rectangle_int_t dst_rect = {
    dst_x,
    dst_y,
    floating ? cairo_image_surface_get_width (floating) : 0,
    floating ? cairo_image_surface_get_height (floating) : 0
  };
  paint_document_commit_move_masked (doc, src_hole, NULL, floating, floating ? &dst_rect : NULL);
}

void
paint_document_commit_move_masked (PaintDocument *doc,
                                   const cairo_rectangle_int_t *src_hole,
                                   cairo_surface_t *hole_mask,
                                   cairo_surface_t *floating,
                                   const cairo_rectangle_int_t *dst_rect)
{
  PaintLayer *active = paint_document_get_active_layer_internal (doc);
  if (!active || (!src_hole && !floating))
    return;

  paint_document_flush_active_stroke_undo (doc);
  gboolean had_hole = doc->has_active_hole;
  cairo_rectangle_int_t prev_hole = doc->active_hole;
  doc->has_active_hole = FALSE;
  doc->active_hole = (cairo_rectangle_int_t) { 0, 0, 0, 0 };
  g_clear_pointer (&doc->active_hole_mask, cairo_surface_destroy);

  UndoStep *step = undo_step_new_tiled (doc->width, doc->height, doc->active_layer_index);

  int hx0 = 0, hy0 = 0, hw = 0, hh = 0;
  if (src_hole && src_hole->width > 0 && src_hole->height > 0) {
    hx0 = CLAMP (src_hole->x, 0, doc->width);
    hy0 = CLAMP (src_hole->y, 0, doc->height);
    int hx1 = CLAMP (src_hole->x + src_hole->width, 0, doc->width);
    int hy1 = CLAMP (src_hole->y + src_hole->height, 0, doc->height);
    hw = hx1 - hx0;
    hh = hy1 - hy0;
    if (hw > 0 && hh > 0)
      undo_step_capture_rect (step, active->surface, hx0, hy0, hw, hh);
  }

  int dx0 = 0, dy0 = 0, dw = 0, dh = 0;
  int target_x = 0, target_y = 0, target_w = 0, target_h = 0;
  if (floating) {
    int fw = cairo_image_surface_get_width (floating);
    int fh = cairo_image_surface_get_height (floating);
    target_x = dst_rect ? dst_rect->x : 0;
    target_y = dst_rect ? dst_rect->y : 0;
    target_w = (dst_rect && dst_rect->width > 0) ? dst_rect->width : fw;
    target_h = (dst_rect && dst_rect->height > 0) ? dst_rect->height : fh;
    dx0 = CLAMP (target_x, 0, doc->width);
    dy0 = CLAMP (target_y, 0, doc->height);
    int dx1 = CLAMP (target_x + target_w, 0, doc->width);
    int dy1 = CLAMP (target_y + target_h, 0, doc->height);
    dw = dx1 - dx0;
    dh = dy1 - dy0;
    if (dw > 0 && dh > 0)
      undo_step_capture_rect (step, active->surface, dx0, dy0, dw, dh);
  }

  if (hw > 0 && hh > 0) {
    fill_layer_masked_rect_with_clear_color (doc, active, hx0, hy0, hw, hh, src_hole, hole_mask);
    paint_document_recomposite_rect (doc, hx0, hy0, hw, hh);
    paint_document_mark_rect_dirty (doc, hx0, hy0, hw, hh);
  } else if (had_hole && prev_hole.width > 0 && prev_hole.height > 0) {
    paint_document_recomposite_rect (doc, prev_hole.x, prev_hole.y,
                                     prev_hole.width, prev_hole.height);
    paint_document_mark_rect_dirty (doc, prev_hole.x, prev_hole.y,
                                    prev_hole.width, prev_hole.height);
  }
  if (floating && dw > 0 && dh > 0) {
    int fw = MAX (1, cairo_image_surface_get_width (floating));
    int fh = MAX (1, cairo_image_surface_get_height (floating));
    cairo_t *cr = cairo_create (active->surface);
    cairo_rectangle (cr, (double) dx0, (double) dy0, (double) dw, (double) dh);
    cairo_clip (cr);
    cairo_set_operator (cr, active->alpha_lock ? CAIRO_OPERATOR_ATOP : CAIRO_OPERATOR_OVER);
    cairo_translate (cr, (double) target_x, (double) target_y);
    if (target_w != fw || target_h != fh)
      cairo_scale (cr, (double) target_w / (double) fw, (double) target_h / (double) fh);
    cairo_set_source_surface (cr, floating, 0.0, 0.0);
    if (target_w != fw || target_h != fh)
      cairo_pattern_set_filter (cairo_get_source (cr), CAIRO_FILTER_BILINEAR);
    cairo_paint (cr);
    cairo_destroy (cr);
    paint_document_recomposite_rect (doc, dx0, dy0, dw, dh);
    paint_document_mark_rect_dirty (doc, dx0, dy0, dw, dh);
  }

  push_undo_step (doc, step);
  doc->modified = TRUE;
}

static void
trace_doc_rounded_rect_path (cairo_t *cr, double x, double y, double w, double h, double r)
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
paint_document_render_text_layout (cairo_t *cr,
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
  PangoContext *pctx = pango_layout_get_context (layout);
  pango_cairo_context_set_resolution (pctx, 96.0);
  cairo_font_options_t *fopt = cairo_font_options_create ();
  cairo_font_options_set_hint_metrics (fopt, CAIRO_HINT_METRICS_OFF);
  pango_cairo_context_set_font_options (pctx, fopt);
  cairo_font_options_destroy (fopt);
  pango_layout_set_text (layout, text ? text : "", -1);
  pango_layout_set_alignment (layout, settings ? settings->text_align : PANGO_ALIGN_LEFT);

  PangoFontDescription *desc = pango_font_description_new ();
  const char *family = (settings && settings->text_font_family && *settings->text_font_family)
                         ? settings->text_font_family
                         : "Sans";
  pango_font_description_set_family (desc, family);
  pango_font_description_set_weight (desc,
                                     (settings && settings->text_bold)
                                       ? PANGO_WEIGHT_BOLD
                                       : PANGO_WEIGHT_NORMAL);
  pango_font_description_set_style (desc,
                                    (settings && settings->text_italic)
                                      ? PANGO_STYLE_ITALIC
                                      : PANGO_STYLE_NORMAL);
  double px_size = settings ? MAX (6.0, settings->text_font_size) : 28.0;
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
  int tw = MAX (24, bx1 - bx) + 4;
  int th = MAX ((int) ceil (px_size * 1.2), by1 - by);

  double pad = 0.0;
  if (settings && settings->text_bg_pill)
    pad = MAX (6.0, px_size * 0.25);
  else if (settings && settings->text_outline)
    pad = MAX (2.0, ceil (px_size * 0.10));
  int ipad = (int) ceil (pad);

  if (out_x)
    *out_x = (int) floor (x) + bx - ipad;
  if (out_y)
    *out_y = (int) floor (y) + by - ipad;
  if (out_w)
    *out_w = tw + ipad * 2;
  if (out_h)
    *out_h = th + ipad * 2;

  if (draw && settings && text && *text) {
    cairo_save (cr);
    cairo_operator_t op = settings->target_alpha_lock ? CAIRO_OPERATOR_ATOP : CAIRO_OPERATOR_OVER;
    cairo_set_operator (cr, op);

    if (settings->text_bg_pill) {
      double rx = x + (double) bx - pad;
      double ry = y + (double) by - pad;
      double rw = (double) tw + pad * 2.0;
      double rh = (double) th + pad * 2.0;
      trace_doc_rounded_rect_path (cr, rx, ry, rw, rh, MIN (12.0, pad * 1.35));
      cairo_set_source_rgba (cr,
                             settings->secondary_color.red,
                             settings->secondary_color.green,
                             settings->secondary_color.blue,
                             settings->secondary_color.alpha);
      cairo_fill (cr);
    }

    if (settings->text_outline) {
      cairo_new_path (cr);
      cairo_move_to (cr, x, y);
      pango_cairo_layout_path (cr, layout);
      cairo_set_source_rgba (cr,
                             settings->secondary_color.red,
                             settings->secondary_color.green,
                             settings->secondary_color.blue,
                             settings->secondary_color.alpha);
      cairo_set_line_width (cr, MAX (2.0, px_size * 0.14));
      cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
      cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
      cairo_stroke (cr);
    }

    cairo_set_source_rgba (cr,
                           settings->color.red,
                           settings->color.green,
                           settings->color.blue,
                           settings->color.alpha);
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
  PaintLayer *active = paint_document_get_active_layer_internal (doc);
  if (!active || !settings || !text || !*text)
    return;

  PaintToolSettings eff = *settings;
  eff.target_alpha_lock = active->alpha_lock;

  paint_document_flush_active_stroke_undo (doc);
  int bx, by, bw, bh;
  cairo_t *cr = cairo_create (active->surface);
  paint_document_render_text_layout (cr, x, y, text, &eff, FALSE, &bx, &by, &bw, &bh);
  cairo_destroy (cr);

  int rx = bx - 6;
  int ry = by - 6;
  int rw = bw + 12;
  int rh = bh + 12;
  UndoStep *step = undo_step_new_tiled (doc->width, doc->height, doc->active_layer_index);
  undo_step_capture_rect (step, active->surface, rx, ry, rw, rh);

  cr = cairo_create (active->surface);
  paint_document_render_text_layout (cr, x, y, text, &eff, TRUE, NULL, NULL, NULL, NULL);
  cairo_destroy (cr);

  paint_document_recomposite_rect (doc, rx, ry, rw, rh);
  paint_document_mark_rect_dirty (doc, rx, ry, rw, rh);
  push_undo_step (doc, step);
  doc->modified = TRUE;
}

void
paint_document_set_floating_hole (PaintDocument *doc,
                                  const cairo_rectangle_int_t *hole)
{
  paint_document_set_floating_hole_masked (doc, hole, NULL);
}

void
paint_document_set_floating_hole_masked (PaintDocument *doc,
                                         const cairo_rectangle_int_t *hole,
                                         cairo_surface_t *hole_mask)
{
  if (!doc)
    return;
  gboolean had_hole = doc->has_active_hole;
  cairo_rectangle_int_t old_hole = doc->active_hole;
  gboolean had_comp = (doc->composite_surface != NULL);

  g_clear_pointer (&doc->active_hole_mask, cairo_surface_destroy);
  if (hole && hole->width > 0 && hole->height > 0) {
    doc->has_active_hole = TRUE;
    doc->active_hole = *hole;
    if (hole_mask)
      doc->active_hole_mask = cairo_surface_reference (hole_mask);
  } else {
    doc->has_active_hole = FALSE;
    doc->active_hole = (cairo_rectangle_int_t) { 0, 0, 0, 0 };
  }

  gboolean want_comp = needs_composite_surface (doc);
  if (want_comp != had_comp) {
    paint_document_recomposite_all (doc);
    paint_document_mark_all_dirty (doc);
    return;
  }

  if (had_hole && old_hole.width > 0 && old_hole.height > 0) {
    paint_document_recomposite_rect (doc, old_hole.x, old_hole.y, old_hole.width, old_hole.height);
    paint_document_mark_rect_dirty (doc, old_hole.x, old_hole.y, old_hole.width, old_hole.height);
  }
  if (doc->has_active_hole) {
    paint_document_recomposite_rect (doc, doc->active_hole.x, doc->active_hole.y,
                                     doc->active_hole.width, doc->active_hole.height);
    paint_document_mark_rect_dirty (doc, doc->active_hole.x, doc->active_hole.y,
                                    doc->active_hole.width, doc->active_hole.height);
  }
}

/* ============================================================================
 * Layers Management
 * ============================================================================ */

int
paint_document_get_n_layers (const PaintDocument *doc)
{
  return (doc && doc->layers) ? (int) doc->layers->len : 0;
}

int
paint_document_get_active_layer (const PaintDocument *doc)
{
  return doc ? doc->active_layer_index : 0;
}

void
paint_document_set_active_layer (PaintDocument *doc, int index)
{
  if (!doc || !doc->layers || doc->layers->len == 0)
    return;
  int clamped = CLAMP (index, 0, (int) doc->layers->len - 1);
  if (doc->active_layer_index == clamped)
    return;
  paint_document_flush_active_stroke_undo (doc);
  doc->active_layer_index = clamped;
  doc->layers_revision++;
}

guint64
paint_document_get_layers_revision (const PaintDocument *doc)
{
  return doc ? doc->layers_revision : 0;
}

const char *
paint_document_layer_get_name (const PaintDocument *doc, int index)
{
  if (!doc || !doc->layers || index < 0 || index >= (int) doc->layers->len)
    return "";
  const PaintLayer *layer = g_ptr_array_index (doc->layers, (guint) index);
  return layer->name;
}

void
paint_document_layer_set_name (PaintDocument *doc, int index, const char *name)
{
  if (!doc || !doc->layers || index < 0 || index >= (int) doc->layers->len)
    return;
  const char *trimmed = (name && *name) ? name : "Layer";
  PaintLayer *layer = g_ptr_array_index (doc->layers, (guint) index);
  if (g_strcmp0 (layer->name, trimmed) == 0)
    return;
  paint_document_flush_active_stroke_undo (doc);
  push_undo_step (doc, undo_step_new_full_doc (doc));
  g_free (layer->name);
  layer->name = g_strdup (trimmed);
  doc->layers_revision++;
  doc->modified = TRUE;
}

gboolean
paint_document_layer_get_visible (const PaintDocument *doc, int index)
{
  if (!doc || !doc->layers || index < 0 || index >= (int) doc->layers->len)
    return FALSE;
  const PaintLayer *layer = g_ptr_array_index (doc->layers, (guint) index);
  return layer->visible;
}

void
paint_document_layer_set_visible (PaintDocument *doc, int index, gboolean visible)
{
  if (!doc || !doc->layers || index < 0 || index >= (int) doc->layers->len)
    return;
  PaintLayer *layer = g_ptr_array_index (doc->layers, (guint) index);
  if (layer->visible == visible)
    return;
  paint_document_flush_active_stroke_undo (doc);
  push_undo_step (doc, undo_step_new_full_doc (doc));
  layer->visible = visible;
  doc->layers_revision++;
  paint_document_recomposite_all (doc);
  paint_document_mark_all_dirty (doc);
  doc->modified = TRUE;
}

double
paint_document_layer_get_opacity (const PaintDocument *doc, int index)
{
  if (!doc || !doc->layers || index < 0 || index >= (int) doc->layers->len)
    return 1.0;
  const PaintLayer *layer = g_ptr_array_index (doc->layers, (guint) index);
  return layer->opacity;
}

void
paint_document_layer_set_opacity (PaintDocument *doc, int index, double opacity)
{
  if (!doc || !doc->layers || index < 0 || index >= (int) doc->layers->len)
    return;
  double clamped = CLAMP (opacity, 0.0, 1.0);
  PaintLayer *layer = g_ptr_array_index (doc->layers, (guint) index);
  if (fabs (layer->opacity - clamped) < 1e-4)
    return;

  paint_document_flush_active_stroke_undo (doc);
  gboolean can_coalesce = FALSE;
  if (doc->undo_stack->len > 0 && doc->redo_stack->len == 0) {
    const UndoStep *top = g_ptr_array_index (doc->undo_stack, doc->undo_stack->len - 1);
    if (top->is_full && top->is_opacity_step && top->opacity_layer_idx == index)
      can_coalesce = TRUE;
  }
  if (!can_coalesce) {
    UndoStep *step = undo_step_new_full_doc (doc);
    step->is_opacity_step = TRUE;
    step->opacity_layer_idx = index;
    push_undo_step (doc, step);
  }

  layer->opacity = clamped;
  doc->layers_revision++;
  paint_document_recomposite_all (doc);
  paint_document_mark_all_dirty (doc);
  doc->modified = TRUE;
}

PaintBlendMode
paint_document_layer_get_blend_mode (const PaintDocument *doc, int index)
{
  if (!doc || !doc->layers || index < 0 || index >= (int) doc->layers->len)
    return PAINT_BLEND_NORMAL;
  const PaintLayer *layer = g_ptr_array_index (doc->layers, (guint) index);
  return layer->blend_mode;
}

void
paint_document_layer_set_blend_mode (PaintDocument *doc, int index, PaintBlendMode mode)
{
  if (!doc || !doc->layers || index < 0 || index >= (int) doc->layers->len)
    return;
  PaintLayer *layer = g_ptr_array_index (doc->layers, (guint) index);
  if (layer->blend_mode == mode)
    return;
  paint_document_flush_active_stroke_undo (doc);
  push_undo_step (doc, undo_step_new_full_doc (doc));
  layer->blend_mode = mode;
  doc->layers_revision++;
  paint_document_recomposite_all (doc);
  paint_document_mark_all_dirty (doc);
  doc->modified = TRUE;
}

cairo_surface_t *
paint_document_layer_get_surface (const PaintDocument *doc, int index)
{
  if (!doc || !doc->layers || index < 0 || index >= (int) doc->layers->len)
    return NULL;
  const PaintLayer *layer = g_ptr_array_index (doc->layers, (guint) index);
  return layer->surface;
}

gboolean
paint_document_layer_get_alpha_lock (const PaintDocument *doc, int index)
{
  if (!doc || !doc->layers || index < 0 || index >= (int) doc->layers->len)
    return FALSE;
  const PaintLayer *layer = g_ptr_array_index (doc->layers, (guint) index);
  return layer->alpha_lock;
}

void
paint_document_layer_set_alpha_lock (PaintDocument *doc, int index, gboolean alpha_lock)
{
  if (!doc || !doc->layers || index < 0 || index >= (int) doc->layers->len)
    return;
  PaintLayer *layer = g_ptr_array_index (doc->layers, (guint) index);
  gboolean val = !!alpha_lock;
  if (layer->alpha_lock == val)
    return;
  paint_document_flush_active_stroke_undo (doc);
  push_undo_step (doc, undo_step_new_full_doc (doc));
  layer->alpha_lock = val;
  doc->layers_revision++;
  doc->modified = TRUE;
}

gboolean
paint_document_layer_get_locked (const PaintDocument *doc, int index)
{
  if (!doc || !doc->layers || index < 0 || index >= (int) doc->layers->len)
    return FALSE;
  const PaintLayer *layer = g_ptr_array_index (doc->layers, (guint) index);
  return layer->locked;
}

void
paint_document_layer_set_locked (PaintDocument *doc, int index, gboolean locked)
{
  if (!doc || !doc->layers || index < 0 || index >= (int) doc->layers->len)
    return;
  PaintLayer *layer = g_ptr_array_index (doc->layers, (guint) index);
  gboolean val = !!locked;
  if (layer->locked == val)
    return;
  paint_document_flush_active_stroke_undo (doc);
  push_undo_step (doc, undo_step_new_full_doc (doc));
  layer->locked = val;
  doc->layers_revision++;
  doc->modified = TRUE;
}

void
paint_document_layer_set_locks_no_undo (PaintDocument *doc,
                                        int index,
                                        gboolean alpha_lock,
                                        gboolean locked)
{
  if (!doc || !doc->layers || index < 0 || index >= (int) doc->layers->len)
    return;
  PaintLayer *layer = g_ptr_array_index (doc->layers, (guint) index);
  layer->alpha_lock = !!alpha_lock;
  layer->locked = !!locked;
  doc->layers_revision++;
}

void
paint_document_set_layers_visibility (PaintDocument *doc,
                                      const gboolean *visible,
                                      int count)
{
  if (!doc || !doc->layers || !visible || count <= 0)
    return;
  int n = MIN (count, (int) doc->layers->len);
  gboolean any_changed = FALSE;
  for (int i = 0; i < n; i++) {
    const PaintLayer *layer = g_ptr_array_index (doc->layers, (guint) i);
    if (layer->visible != !!visible[i]) {
      any_changed = TRUE;
      break;
    }
  }
  if (!any_changed)
    return;

  paint_document_flush_active_stroke_undo (doc);
  push_undo_step (doc, undo_step_new_full_doc (doc));
  for (int i = 0; i < n; i++) {
    PaintLayer *layer = g_ptr_array_index (doc->layers, (guint) i);
    layer->visible = !!visible[i];
  }
  doc->layers_revision++;
  paint_document_recomposite_all (doc);
  paint_document_mark_all_dirty (doc);
  doc->modified = TRUE;
}

int
paint_document_add_layer (PaintDocument *doc, const char *name)
{
  if (!doc || !doc->layers)
    return -1;
  paint_document_flush_active_stroke_undo (doc);
  push_undo_step (doc, undo_step_new_full_doc (doc));

  cairo_surface_t *surf = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
                                                      doc->width,
                                                      doc->height);
  g_autofree char *auto_name = name ? g_strdup (name)
                                    : g_strdup_printf ("Layer %d", doc->next_layer_num++);
  int insert_idx = CLAMP (doc->active_layer_index + 1, 0, (int) doc->layers->len);
  g_ptr_array_insert (doc->layers,
                      (guint) insert_idx,
                      paint_layer_new (auto_name, surf, TRUE, 1.0, PAINT_BLEND_NORMAL));
  doc->active_layer_index = insert_idx;
  doc->layers_revision++;
  paint_document_recomposite_all (doc);
  paint_document_mark_all_dirty (doc);
  doc->modified = TRUE;
  return insert_idx;
}

int
paint_document_add_layer_from_surface (PaintDocument *doc,
                                       const char *name,
                                       cairo_surface_t *src)
{
  if (!doc || !doc->layers || !src || cairo_surface_status (src) != CAIRO_STATUS_SUCCESS)
    return -1;

  paint_document_flush_active_stroke_undo (doc);
  push_undo_step (doc, undo_step_new_full_doc (doc));

  cairo_surface_t *surf = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
                                                      doc->width,
                                                      doc->height);
  cairo_t *cr = cairo_create (surf);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface (cr, src, 0.0, 0.0);
  cairo_paint (cr);
  cairo_destroy (cr);

  g_autofree char *auto_name = (name && *name) ? g_strdup (name)
                                               : g_strdup_printf ("Layer %d", doc->next_layer_num++);
  int insert_idx = CLAMP (doc->active_layer_index + 1, 0, (int) doc->layers->len);
  g_ptr_array_insert (doc->layers,
                      (guint) insert_idx,
                      paint_layer_new (auto_name, surf, TRUE, 1.0, PAINT_BLEND_NORMAL));
  doc->active_layer_index = insert_idx;
  doc->layers_revision++;
  paint_document_recomposite_all (doc);
  paint_document_mark_all_dirty (doc);
  doc->modified = TRUE;
  return insert_idx;
}

gboolean
paint_document_duplicate_layer (PaintDocument *doc, int index)
{
  if (!doc || !doc->layers || index < 0 || index >= (int) doc->layers->len)
    return FALSE;
  paint_document_flush_active_stroke_undo (doc);
  push_undo_step (doc, undo_step_new_full_doc (doc));

  const PaintLayer *src = g_ptr_array_index (doc->layers, (guint) index);
  cairo_surface_t *copy = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
                                                      doc->width,
                                                      doc->height);
  cairo_t *cr = cairo_create (copy);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface (cr, src->surface, 0.0, 0.0);
  cairo_paint (cr);
  cairo_destroy (cr);

  g_autofree char *dup_name = g_strdup_printf ("%s Copy", src->name);
  PaintLayer *dup = paint_layer_new (dup_name, copy, src->visible, src->opacity, src->blend_mode);
  dup->alpha_lock = src->alpha_lock;
  dup->locked = src->locked;
  int insert_idx = index + 1;
  g_ptr_array_insert (doc->layers, (guint) insert_idx, dup);
  doc->active_layer_index = insert_idx;
  doc->layers_revision++;
  paint_document_recomposite_all (doc);
  paint_document_mark_all_dirty (doc);
  doc->modified = TRUE;
  return TRUE;
}

gboolean
paint_document_remove_layer (PaintDocument *doc, int index)
{
  if (!doc || !doc->layers || doc->layers->len <= 1 ||
      index < 0 || index >= (int) doc->layers->len) {
    return FALSE;
  }
  paint_document_flush_active_stroke_undo (doc);
  push_undo_step (doc, undo_step_new_full_doc (doc));

  g_ptr_array_remove_index (doc->layers, (guint) index);
  if (doc->active_layer_index > index)
    doc->active_layer_index--;
  doc->active_layer_index = CLAMP (doc->active_layer_index, 0, (int) doc->layers->len - 1);
  doc->layers_revision++;
  paint_document_recomposite_all (doc);
  paint_document_mark_all_dirty (doc);
  doc->modified = TRUE;
  return TRUE;
}

gboolean
paint_document_move_layer (PaintDocument *doc, int from_index, int to_index)
{
  if (!doc || !doc->layers ||
      from_index < 0 || from_index >= (int) doc->layers->len ||
      to_index < 0 || to_index >= (int) doc->layers->len ||
      from_index == to_index) {
    return FALSE;
  }
  paint_document_flush_active_stroke_undo (doc);
  push_undo_step (doc, undo_step_new_full_doc (doc));

  PaintLayer **arr = (PaintLayer **) doc->layers->pdata;
  PaintLayer *moved = arr[from_index];
  if (from_index < to_index) {
    for (int i = from_index; i < to_index; i++)
      arr[i] = arr[i + 1];
  } else {
    for (int i = from_index; i > to_index; i--)
      arr[i] = arr[i - 1];
  }
  arr[to_index] = moved;

  if (doc->active_layer_index == from_index)
    doc->active_layer_index = to_index;
  else if (from_index < doc->active_layer_index && doc->active_layer_index <= to_index)
    doc->active_layer_index--;
  else if (to_index <= doc->active_layer_index && doc->active_layer_index < from_index)
    doc->active_layer_index++;

  doc->layers_revision++;
  paint_document_recomposite_all (doc);
  paint_document_mark_all_dirty (doc);
  doc->modified = TRUE;
  return TRUE;
}

gboolean
paint_document_merge_layer_down (PaintDocument *doc, int index)
{
  if (!doc || !doc->layers || index <= 0 || index >= (int) doc->layers->len)
    return FALSE;
  paint_document_flush_active_stroke_undo (doc);
  push_undo_step (doc, undo_step_new_full_doc (doc));

  PaintLayer *top = g_ptr_array_index (doc->layers, (guint) index);
  PaintLayer *bot = g_ptr_array_index (doc->layers, (guint) (index - 1));
  if (top->visible && top->opacity > 0.001) {
    cairo_t *cr = cairo_create (bot->surface);
    cairo_set_operator (cr, blend_mode_to_cairo (top->blend_mode));
    cairo_set_source_surface (cr, top->surface, 0.0, 0.0);
    if (top->opacity >= 0.999)
      cairo_paint (cr);
    else
      cairo_paint_with_alpha (cr, top->opacity);
    cairo_destroy (cr);
  }

  g_ptr_array_remove_index (doc->layers, (guint) index);
  if (doc->active_layer_index >= index)
    doc->active_layer_index--;
  doc->active_layer_index = CLAMP (doc->active_layer_index, 0, (int) doc->layers->len - 1);

  doc->layers_revision++;
  paint_document_recomposite_all (doc);
  paint_document_mark_all_dirty (doc);
  doc->modified = TRUE;
  return TRUE;
}

gboolean
paint_document_flatten_layers (PaintDocument *doc)
{
  if (!doc || !doc->layers || doc->layers->len == 0)
    return FALSE;
  const PaintLayer *l0 = g_ptr_array_index (doc->layers, 0);
  if (doc->layers->len <= 1 && l0->visible && l0->opacity >= 0.999 &&
      l0->blend_mode == PAINT_BLEND_NORMAL) {
    return FALSE;
  }

  paint_document_flush_active_stroke_undo (doc);
  push_undo_step (doc, undo_step_new_full_doc (doc));
  paint_document_recomposite_all (doc);

  cairo_surface_t *flat_surf = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
                                                           doc->width,
                                                           doc->height);
  cairo_t *cr = cairo_create (flat_surf);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface (cr, doc->surface, 0.0, 0.0);
  cairo_paint (cr);
  cairo_destroy (cr);

  /* Check if flattened surface has transparency */
  cairo_surface_flush (flat_surf);
  int stride = cairo_image_surface_get_stride (flat_surf);
  const unsigned char *data = cairo_image_surface_get_data (flat_surf);
  gboolean has_alpha = FALSE;
  for (int y = 0; y < doc->height && !has_alpha; y++) {
    const uint32_t *row = (const uint32_t *) (data + (gsize) y * (gsize) stride);
    for (int x = 0; x < doc->width; x++) {
      if (((row[x] >> 24) & 0xFFU) < 255U) {
        has_alpha = TRUE;
        break;
      }
    }
  }
  if (has_alpha)
    doc->background_color = (GdkRGBA) { 0.0f, 0.0f, 0.0f, 0.0f };

  g_ptr_array_set_size (doc->layers, 0);
  g_ptr_array_add (doc->layers,
                   paint_layer_new ("Background", flat_surf, TRUE, 1.0, PAINT_BLEND_NORMAL));
  doc->active_layer_index = 0;
  doc->layers_revision++;
  paint_document_recomposite_all (doc);
  paint_document_mark_all_dirty (doc);
  doc->modified = TRUE;
  return TRUE;
}

gboolean
paint_document_append_restored_layer (PaintDocument *doc,
                                      const char *name,
                                      GFile *png_file,
                                      gboolean visible,
                                      double opacity,
                                      PaintBlendMode blend_mode)
{
  if (!doc || !doc->layers || !png_file)
    return FALSE;
  g_autofree char *path = g_file_get_path (png_file);
  if (!path)
    return FALSE;

  cairo_surface_t *loaded = cairo_image_surface_create_from_png (path);
  if (cairo_surface_status (loaded) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy (loaded);
    return FALSE;
  }

  cairo_surface_t *surf = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
                                                      doc->width,
                                                      doc->height);
  cairo_t *cr = cairo_create (surf);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface (cr, loaded, 0.0, 0.0);
  cairo_paint (cr);
  cairo_destroy (cr);
  cairo_surface_destroy (loaded);

  g_ptr_array_add (doc->layers,
                   paint_layer_new (name, surf, visible, opacity, blend_mode));
  return TRUE;
}

void
paint_document_finish_session_restore (PaintDocument *doc,
                                       int active_layer,
                                       gboolean modified)
{
  if (!doc || !doc->layers || doc->layers->len == 0)
    return;
  g_ptr_array_set_size (doc->undo_stack, 0);
  g_ptr_array_set_size (doc->redo_stack, 0);
  doc->undo_bytes = 0;
  doc->redo_bytes = 0;
  doc->active_layer_index = CLAMP (active_layer, 0, (int) doc->layers->len - 1);
  doc->next_layer_num = MAX (1, (int) doc->layers->len);
  doc->modified = modified;
  doc->layers_revision++;
  paint_document_recomposite_all (doc);
  paint_document_mark_all_dirty (doc);
}

/* ============================================================================
 * Texture / Surface Conversions
 * ============================================================================ */

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

  cairo_surface_flush (loaded);
  int stride = cairo_image_surface_get_stride (loaded);
  const unsigned char *data = cairo_image_surface_get_data (loaded);
  gboolean has_alpha = FALSE;
  for (int y = 0; y < h && !has_alpha; y++) {
    const uint32_t *row = (const uint32_t *) (data + (gsize) y * (gsize) stride);
    for (int x = 0; x < w; x++) {
      if (((row[x] >> 24) & 0xFFU) < 255U) {
        has_alpha = TRUE;
        break;
      }
    }
  }

  PaintDocument *doc = has_alpha ? paint_document_new_transparent (w, h)
                                 : paint_document_new (w, h);
  PaintLayer *l0 = paint_document_get_active_layer_internal (doc);
  cairo_t *cr = cairo_create (l0->surface);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface (cr, loaded, 0.0, 0.0);
  cairo_paint (cr);
  cairo_destroy (cr);
  cairo_surface_destroy (loaded);

  doc->modified = TRUE;
  paint_document_recomposite_all (doc);
  paint_document_mark_all_dirty (doc);
  return doc;
}

static void
clamp_doc_rect (const PaintDocument *doc,
                const cairo_rectangle_int_t *rect,
                cairo_rectangle_int_t *out)
{
  int rx = rect ? rect->x : 0;
  int ry = rect ? rect->y : 0;
  int rw = rect ? rect->width : doc->width;
  int rh = rect ? rect->height : doc->height;
  int x0 = CLAMP (rx, 0, doc->width);
  int y0 = CLAMP (ry, 0, doc->height);
  int x1 = CLAMP (rx + rw, 0, doc->width);
  int y1 = CLAMP (ry + rh, 0, doc->height);
  *out = (cairo_rectangle_int_t) { x0, y0, MAX (0, x1 - x0), MAX (0, y1 - y0) };
}

cairo_surface_t *
paint_document_dup_active_layer_surface (const PaintDocument *doc)
{
  PaintLayer *layer = paint_document_get_active_layer_internal (doc);
  if (!layer || !layer->surface)
    return NULL;
  int w = cairo_image_surface_get_width (layer->surface);
  int h = cairo_image_surface_get_height (layer->surface);
  cairo_surface_t *dup = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *cr = cairo_create (dup);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface (cr, layer->surface, 0.0, 0.0);
  cairo_paint (cr);
  cairo_destroy (cr);
  return dup;
}

void
paint_document_notify_active_layer_rect_modified (PaintDocument *doc,
                                                  const cairo_rectangle_int_t *rect)
{
  if (!doc)
    return;
  cairo_rectangle_int_t r;
  clamp_doc_rect (doc, rect, &r);
  if (r.width <= 0 || r.height <= 0)
    return;
  paint_document_recomposite_rect (doc, r.x, r.y, r.width, r.height);
  paint_document_mark_rect_dirty (doc, r.x, r.y, r.width, r.height);
}

void
paint_document_restore_active_layer_rect (PaintDocument *doc,
                                          cairo_surface_t *backup,
                                          const cairo_rectangle_int_t *rect)
{
  PaintLayer *layer = paint_document_get_active_layer_internal (doc);
  if (!layer || !layer->surface || !backup)
    return;
  cairo_rectangle_int_t r;
  clamp_doc_rect (doc, rect, &r);
  if (r.width <= 0 || r.height <= 0)
    return;

  cairo_t *cr = cairo_create (layer->surface);
  cairo_rectangle (cr, (double) r.x, (double) r.y, (double) r.width, (double) r.height);
  cairo_clip (cr);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface (cr, backup, 0.0, 0.0);
  cairo_paint (cr);
  cairo_destroy (cr);
  paint_document_notify_active_layer_rect_modified (doc, &r);
}

void
paint_document_commit_active_layer_from_backup (PaintDocument *doc,
                                                cairo_surface_t *backup,
                                                const cairo_rectangle_int_t *rect)
{
  if (!doc || !backup)
    return;
  cairo_rectangle_int_t r;
  clamp_doc_rect (doc, rect, &r);
  if (r.width <= 0 || r.height <= 0)
    return;

  paint_document_flush_active_stroke_undo (doc);
  UndoStep *step = undo_step_new_tiled (doc->width, doc->height, doc->active_layer_index);
  undo_step_capture_rect (step, backup, r.x, r.y, r.width, r.height);
  push_undo_step (doc, step);

  doc->modified = TRUE;
  doc->layers_revision++;
  paint_document_recomposite_rect (doc, r.x, r.y, r.width, r.height);
  paint_document_mark_rect_dirty (doc, r.x, r.y, r.width, r.height);
}
