#include "paint-canvas.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "paint-document-private.h"

#define VIEWPORT_OVERSCAN 256
#define MIN_VP_CACHE_DIM 1024
#define MAX_OVERVIEW_DIM 2048

static const double ZOOM_STEPS[] = {
  0.05, 0.06, 0.08, 0.10, 0.125, 0.16, 0.20, 0.25, 0.33, 0.40, 0.50, 0.60,
  0.75, 0.90, 1.0, 1.25, 1.5, 2.0, 2.5, 3.0, 4.0, 6.0, 8.0, 12.0, 16.0, 24.0, 32.0
};

/* ============================================================================
 * PaintCanvas (2-Tier Single-Texture Cache: Overview < 50% + 1:1 Viewport >= 50%)
 * ============================================================================ */

enum {
  SIGNAL_CHANGED,
  SIGNAL_ZOOM_CHANGED,
  SIGNAL_COLOR_PICKED,
  SIGNAL_SELECTION_CHANGED,
  SIGNAL_TEXT_STATE_CHANGED,
  SIGNAL_HIDDEN_LAYER_BLOCKED,
  SIGNAL_LOCKED_LAYER_BLOCKED,
  SIGNAL_CURSOR_MOVED,
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
  gboolean pan_override_active;
  gboolean pan_override_dragging;
  gboolean pick_badge_active;
  GdkRGBA pick_badge_color;
  guint pick_badge_timeout_id;

  /* Selection & floating layer state */
  gboolean has_selection;
  cairo_rectangle_int_t sel_rect;
  cairo_surface_t *sel_mask;          /* optional A8 binary mask matching sel_rect */
  GArray *sel_ant_segs;               /* element-type: PaintSelSeg */
  GArray *lasso_points;               /* element-type: PaintPoint */
  gboolean sel_dragging_new;
  gboolean sel_moving;
  gboolean sel_resizing;
  int sel_resize_handle;              /* 0=none, 1..8 = NW,N,NE,E,SE,S,SW,W */
  cairo_rectangle_int_t sel_resize_orig_rect;
  cairo_surface_t *sel_orig_floating;
  cairo_surface_t *sel_orig_mask;
  double sel_move_anchor_x;
  double sel_move_anchor_y;
  int sel_move_orig_x;
  int sel_move_orig_y;
  cairo_surface_t *floating_surface;
  GdkTexture *floating_texture;
  gboolean floating_has_hole;
  cairo_rectangle_int_t floating_hole;
  cairo_surface_t *floating_hole_mask;

  const PaintTool *stylus_override_tool;

  /* Live filter preview state */
  cairo_surface_t *filter_backup;
  gboolean filter_has_rect;
  cairo_rectangle_int_t filter_rect;
  cairo_surface_t *filter_mask;
  gboolean filter_alpha_lock;

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

typedef struct {
  int x0;
  int y0;
  int x1;
  int y1;
} PaintSelSeg;

static void
rebuild_sel_ant_segments (PaintCanvas *self)
{
  if (!self->sel_ant_segs)
    return;
  g_array_set_size (self->sel_ant_segs, 0);
  if (!self->sel_mask || cairo_surface_status (self->sel_mask) != CAIRO_STATUS_SUCCESS)
    return;

  cairo_surface_flush (self->sel_mask);
  int mw = cairo_image_surface_get_width (self->sel_mask);
  int mh = cairo_image_surface_get_height (self->sel_mask);
  int stride = cairo_image_surface_get_stride (self->sel_mask);
  const unsigned char *data = cairo_image_surface_get_data (self->sel_mask);
  if (mw <= 0 || mh <= 0 || !data)
    return;

  for (int y = 0; y <= mh; y++) {
    const unsigned char *above = (y > 0) ? (data + (gsize) (y - 1) * (gsize) stride) : NULL;
    const unsigned char *below = (y < mh) ? (data + (gsize) y * (gsize) stride) : NULL;
    int run_start = -1;
    for (int x = 0; x < mw; x++) {
      gboolean in_a = above && above[x] >= 128;
      gboolean in_b = below && below[x] >= 128;
      if (in_a != in_b) {
        if (run_start < 0)
          run_start = x;
      } else if (run_start >= 0) {
        PaintSelSeg s = { run_start, y, x, y };
        g_array_append_val (self->sel_ant_segs, s);
        run_start = -1;
      }
    }
    if (run_start >= 0) {
      PaintSelSeg s = { run_start, y, mw, y };
      g_array_append_val (self->sel_ant_segs, s);
    }
  }

  for (int x = 0; x <= mw; x++) {
    int run_start = -1;
    for (int y = 0; y < mh; y++) {
      const unsigned char *row = data + (gsize) y * (gsize) stride;
      gboolean in_l = (x > 0) && row[x - 1] >= 128;
      gboolean in_r = (x < mw) && row[x] >= 128;
      if (in_l != in_r) {
        if (run_start < 0)
          run_start = y;
      } else if (run_start >= 0) {
        PaintSelSeg s = { x, run_start, x, y };
        g_array_append_val (self->sel_ant_segs, s);
        run_start = -1;
      }
    }
    if (run_start >= 0) {
      PaintSelSeg s = { x, run_start, x, mh };
      g_array_append_val (self->sel_ant_segs, s);
    }
  }
}

static void
threshold_a8_mask (cairo_surface_t *mask)
{
  if (!mask)
    return;
  cairo_surface_flush (mask);
  int w = cairo_image_surface_get_width (mask);
  int h = cairo_image_surface_get_height (mask);
  int stride = cairo_image_surface_get_stride (mask);
  unsigned char *data = cairo_image_surface_get_data (mask);
  for (int y = 0; y < h; y++) {
    unsigned char *row = data + (gsize) y * (gsize) stride;
    for (int x = 0; x < w; x++)
      row[x] = (row[x] >= 128) ? 255 : 0;
  }
  cairo_surface_mark_dirty (mask);
}

static cairo_surface_t *
create_ellipse_mask (int w, int h)
{
  if (w <= 0 || h <= 0)
    return NULL;
  cairo_surface_t *mask = cairo_image_surface_create (CAIRO_FORMAT_A8, w, h);
  cairo_t *cr = cairo_create (mask);
  cairo_set_operator (cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint (cr);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 1.0);
  cairo_translate (cr, (double) w * 0.5, (double) h * 0.5);
  cairo_scale (cr, MAX (0.5, (double) w * 0.5), MAX (0.5, (double) h * 0.5));
  cairo_arc (cr, 0.0, 0.0, 1.0, 0.0, 2.0 * G_PI);
  cairo_fill (cr);
  cairo_destroy (cr);
  threshold_a8_mask (mask);
  return mask;
}

static cairo_surface_t *
create_lasso_mask (const GArray *pts, int x0, int y0, int w, int h)
{
  if (!pts || pts->len < 3 || w <= 0 || h <= 0)
    return NULL;
  cairo_surface_t *mask = cairo_image_surface_create (CAIRO_FORMAT_A8, w, h);
  cairo_t *cr = cairo_create (mask);
  cairo_set_operator (cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint (cr);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 1.0);
  cairo_translate (cr, -(double) x0, -(double) y0);
  for (guint i = 0; i < pts->len; i++) {
    PaintPoint pt = g_array_index (pts, PaintPoint, i);
    if (i == 0)
      cairo_move_to (cr, pt.x, pt.y);
    else
      cairo_line_to (cr, pt.x, pt.y);
  }
  cairo_close_path (cr);
  cairo_fill (cr);
  cairo_destroy (cr);
  threshold_a8_mask (mask);
  return mask;
}

static cairo_surface_t *
scale_a8_mask (cairo_surface_t *src, int nw, int nh)
{
  if (!src || nw <= 0 || nh <= 0)
    return NULL;
  int ow = cairo_image_surface_get_width (src);
  int oh = cairo_image_surface_get_height (src);
  if (ow <= 0 || oh <= 0)
    return NULL;
  cairo_surface_t *dst = cairo_image_surface_create (CAIRO_FORMAT_A8, nw, nh);
  cairo_t *cr = cairo_create (dst);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_scale (cr, (double) nw / (double) ow, (double) nh / (double) oh);
  cairo_set_source_surface (cr, src, 0.0, 0.0);
  cairo_pattern_set_filter (cairo_get_source (cr), CAIRO_FILTER_BILINEAR);
  cairo_paint (cr);
  cairo_destroy (cr);
  threshold_a8_mask (dst);
  return dst;
}

static cairo_surface_t *
transform_a8_mask_rotate_90 (cairo_surface_t *src, gboolean clockwise)
{
  if (!src)
    return NULL;
  int w = cairo_image_surface_get_width (src);
  int h = cairo_image_surface_get_height (src);
  cairo_surface_t *dst = cairo_image_surface_create (CAIRO_FORMAT_A8, h, w);
  cairo_t *cr = cairo_create (dst);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  if (clockwise) {
    cairo_translate (cr, (double) h, 0.0);
    cairo_rotate (cr, G_PI_2);
  } else {
    cairo_translate (cr, 0.0, (double) w);
    cairo_rotate (cr, -G_PI_2);
  }
  cairo_set_source_surface (cr, src, 0.0, 0.0);
  cairo_pattern_set_filter (cairo_get_source (cr), CAIRO_FILTER_NEAREST);
  cairo_paint (cr);
  cairo_destroy (cr);
  return dst;
}

static cairo_surface_t *
transform_a8_mask_flip (cairo_surface_t *src, gboolean horizontal)
{
  if (!src)
    return NULL;
  int w = cairo_image_surface_get_width (src);
  int h = cairo_image_surface_get_height (src);
  cairo_surface_t *dst = cairo_image_surface_create (CAIRO_FORMAT_A8, w, h);
  cairo_t *cr = cairo_create (dst);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  if (horizontal) {
    cairo_translate (cr, (double) w, 0.0);
    cairo_scale (cr, -1.0, 1.0);
  } else {
    cairo_translate (cr, 0.0, (double) h);
    cairo_scale (cr, 1.0, -1.0);
  }
  cairo_set_source_surface (cr, src, 0.0, 0.0);
  cairo_pattern_set_filter (cairo_get_source (cr), CAIRO_FILTER_NEAREST);
  cairo_paint (cr);
  cairo_destroy (cr);
  return dst;
}

static cairo_surface_t *
scale_argb32_surface (cairo_surface_t *src, int nw, int nh)
{
  if (!src || nw <= 0 || nh <= 0)
    return NULL;
  int ow = cairo_image_surface_get_width (src);
  int oh = cairo_image_surface_get_height (src);
  if (ow <= 0 || oh <= 0)
    return NULL;
  cairo_surface_t *dst = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, nw, nh);
  cairo_t *cr = cairo_create (dst);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_scale (cr, (double) nw / (double) ow, (double) nh / (double) oh);
  cairo_set_source_surface (cr, src, 0.0, 0.0);
  cairo_pattern_set_filter (cairo_get_source (cr), CAIRO_FILTER_BILINEAR);
  cairo_paint (cr);
  cairo_destroy (cr);
  return dst;
}

static void
get_selection_handle_centers (const PaintCanvas *self, double hx[9], double hy[9])
{
  double x0 = self->sel_rect.x * self->zoom;
  double y0 = self->sel_rect.y * self->zoom;
  double x1 = (self->sel_rect.x + self->sel_rect.width) * self->zoom;
  double y1 = (self->sel_rect.y + self->sel_rect.height) * self->zoom;
  double xm = 0.5 * (x0 + x1);
  double ym = 0.5 * (y0 + y1);
  hx[0] = 0.0; hy[0] = 0.0;
  hx[1] = x0;  hy[1] = y0; /* NW */
  hx[2] = xm;  hy[2] = y0; /* N  */
  hx[3] = x1;  hy[3] = y0; /* NE */
  hx[4] = x1;  hy[4] = ym; /* E  */
  hx[5] = x1;  hy[5] = y1; /* SE */
  hx[6] = xm;  hy[6] = y1; /* S  */
  hx[7] = x0;  hy[7] = y1; /* SW */
  hx[8] = x0;  hy[8] = ym; /* W  */
}

static int
hit_test_selection_handle (const PaintCanvas *self, double doc_x, double doc_y)
{
  if (!self->has_selection || self->sel_rect.width <= 0 || self->sel_rect.height <= 0 ||
      self->zoom <= 0.0) {
    return 0;
  }
  double px = doc_x * self->zoom;
  double py = doc_y * self->zoom;
  double hx[9], hy[9];
  get_selection_handle_centers (self, hx, hy);

  static const int order[8] = { 1, 3, 5, 7, 2, 4, 6, 8 };
  for (int i = 0; i < 8; i++) {
    int h = order[i];
    if (fabs (px - hx[h]) <= 6.5 && fabs (py - hy[h]) <= 6.5)
      return h;
  }
  return 0;
}

static const char *
cursor_name_for_handle (int handle)
{
  switch (handle) {
    case 1: case 5: return "nwse-resize";
    case 2: case 6: return "ns-resize";
    case 3: case 7: return "nesw-resize";
    case 4: case 8: return "ew-resize";
    default: return "default";
  }
}

static gboolean
check_active_layer_editable (PaintCanvas *self)
{
  if (!self || !self->document)
    return FALSE;
  int idx = paint_document_get_active_layer (self->document);
  if (!paint_document_layer_get_visible (self->document, idx)) {
    g_signal_emit (self, signals[SIGNAL_HIDDEN_LAYER_BLOCKED], 0);
    return FALSE;
  }
  if (paint_document_layer_get_locked (self->document, idx)) {
    g_signal_emit (self, signals[SIGNAL_LOCKED_LAYER_BLOCKED], 0);
    return FALSE;
  }
  return TRUE;
}

static void
lift_selection_to_floating (PaintCanvas *self)
{
  if (!self->has_selection || self->floating_surface || !self->document)
    return;
  if (!check_active_layer_editable (self))
    return;
  self->floating_surface = paint_document_copy_masked_rect (self->document,
                                                             &self->sel_rect,
                                                             self->sel_mask);
  if (!self->floating_surface)
    return;
  self->floating_has_hole = TRUE;
  self->floating_hole = self->sel_rect;
  g_clear_pointer (&self->floating_hole_mask, cairo_surface_destroy);
  if (self->sel_mask)
    self->floating_hole_mask = cairo_surface_reference (self->sel_mask);
  paint_document_set_floating_hole_masked (self->document,
                                           &self->floating_hole,
                                           self->floating_hole_mask);
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
  self->sel_resizing = FALSE;
  self->sel_resize_handle = 0;
  self->floating_has_hole = FALSE;
  self->floating_hole = (cairo_rectangle_int_t) { 0, 0, 0, 0 };
  g_clear_pointer (&self->sel_mask, cairo_surface_destroy);
  g_clear_pointer (&self->floating_hole_mask, cairo_surface_destroy);
  g_clear_pointer (&self->sel_orig_floating, cairo_surface_destroy);
  g_clear_pointer (&self->sel_orig_mask, cairo_surface_destroy);
  g_clear_pointer (&self->floating_surface, cairo_surface_destroy);
  g_clear_object (&self->floating_texture);
  if (self->sel_ant_segs)
    g_array_set_size (self->sel_ant_segs, 0);
  if (self->lasso_points)
    g_array_set_size (self->lasso_points, 0);
  if (self->document)
    paint_document_set_floating_hole (self->document, NULL);
  if (had)
    g_signal_emit (self, signals[SIGNAL_SELECTION_CHANGED], 0);
}

static void
update_text_bounds (PaintCanvas *self)
{
  if (!self->text_active || !self->document || !self->settings)
    return;
  cairo_t *cr = cairo_create (self->document->surface);
  paint_document_render_text_layout (cr,
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
  if (self->pan_override_active) {
    want = self->pan_override_dragging ? "grabbing" : "grab";
  } else if (is_eyedropper_mode (self)) {
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
    } else if (tool->is_select) {
      if (self->has_selection && self->has_hover) {
        int h = hit_test_selection_handle (self, self->hover_x, self->hover_y);
        if (h > 0) {
          want = cursor_name_for_handle (h);
        } else if (self->hover_x >= self->sel_rect.x &&
                   self->hover_x < self->sel_rect.x + self->sel_rect.width &&
                   self->hover_y >= self->sel_rect.y &&
                   self->hover_y < self->sel_rect.y + self->sel_rect.height) {
          want = "move";
        } else {
          want = "crosshair";
        }
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
    paint_document_compute_tool_stroke_bounds (active_tool, self->settings,
                                               x0, y0, x1, y1,
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
    paint_document_render_text_layout (cr, self->text_x, self->text_y, self->text_content,
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

  if (self->sel_dragging_new && active_tool &&
      g_strcmp0 (active_tool->id, "select-lasso") == 0 &&
      self->lasso_points && self->lasso_points->len >= 2) {
    float vw = (float) gtk_widget_get_width (widget);
    float vh = (float) gtk_widget_get_height (widget);
    graphene_rect_t bounds = GRAPHENE_RECT_INIT (0.0f, 0.0f, MAX (1.0f, vw), MAX (1.0f, vh));
    cairo_t *cr = gtk_snapshot_append_cairo (snapshot, &bounds);
    static const double dash[] = { 5.0, 5.0 };
    cairo_set_line_width (cr, 1.0);
    for (int pass = 0; pass < 2; pass++) {
      cairo_set_dash (cr, dash, 2, pass ? 5.0 : 0.0);
      if (pass == 0)
        cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.95);
      else
        cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.9);
      for (guint i = 0; i < self->lasso_points->len; i++) {
        PaintPoint pt = g_array_index (self->lasso_points, PaintPoint, i);
        if (i == 0)
          cairo_move_to (cr, pt.x * self->zoom, pt.y * self->zoom);
        else
          cairo_line_to (cr, pt.x * self->zoom, pt.y * self->zoom);
      }
      cairo_stroke (cr);
    }
    cairo_destroy (cr);
  } else if ((self->has_selection || self->sel_dragging_new) &&
             self->sel_rect.width > 0 && self->sel_rect.height > 0) {
    double sx0 = floor (self->sel_rect.x * self->zoom) + 0.5;
    double sy0 = floor (self->sel_rect.y * self->zoom) + 0.5;
    double sw = MAX (1.0, round (self->sel_rect.width * self->zoom) - 1.0);
    double sh = MAX (1.0, round (self->sel_rect.height * self->zoom) - 1.0);
    graphene_rect_t bounds = GRAPHENE_RECT_INIT (
        (float) (sx0 - 10.0), (float) (sy0 - 10.0), (float) (sw + 20.0), (float) (sh + 20.0));
    cairo_t *cr = gtk_snapshot_append_cairo (snapshot, &bounds);
    static const double dash[] = { 5.0, 5.0 };
    cairo_set_line_width (cr, 1.0);

    gboolean is_ellipse_drag = self->sel_dragging_new && active_tool &&
                               g_strcmp0 (active_tool->id, "select-ellipse") == 0;
    gboolean use_ant_segs = !self->sel_dragging_new &&
                            self->sel_mask &&
                            self->sel_ant_segs &&
                            self->sel_ant_segs->len > 0;
    double mask_sx = 1.0;
    double mask_sy = 1.0;
    if (use_ant_segs) {
      int mw = MAX (1, cairo_image_surface_get_width (self->sel_mask));
      int mh = MAX (1, cairo_image_surface_get_height (self->sel_mask));
      mask_sx = (double) self->sel_rect.width / (double) mw;
      mask_sy = (double) self->sel_rect.height / (double) mh;
    }

    for (int pass = 0; pass < 2; pass++) {
      cairo_set_dash (cr, dash, 2, pass ? 5.0 : 0.0);
      if (pass == 0)
        cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.95);
      else
        cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.9);

      if (use_ant_segs) {
        for (guint i = 0; i < self->sel_ant_segs->len; i++) {
          PaintSelSeg s = g_array_index (self->sel_ant_segs, PaintSelSeg, i);
          cairo_move_to (cr,
                        ((double) self->sel_rect.x + s.x0 * mask_sx) * self->zoom,
                        ((double) self->sel_rect.y + s.y0 * mask_sy) * self->zoom);
          cairo_line_to (cr,
                        ((double) self->sel_rect.x + s.x1 * mask_sx) * self->zoom,
                        ((double) self->sel_rect.y + s.y1 * mask_sy) * self->zoom);
        }
        cairo_stroke (cr);
      } else if (is_ellipse_drag) {
        cairo_save (cr);
        cairo_translate (cr, sx0 + sw * 0.5, sy0 + sh * 0.5);
        cairo_scale (cr, MAX (0.5, sw * 0.5), MAX (0.5, sh * 0.5));
        cairo_arc (cr, 0.0, 0.0, 1.0, 0.0, 2.0 * G_PI);
        cairo_restore (cr);
        cairo_stroke (cr);
      } else {
        cairo_rectangle (cr, sx0, sy0, sw, sh);
        cairo_stroke (cr);
      }
    }

    if (self->has_selection && !self->sel_dragging_new) {
      if (self->sel_mask) {
        cairo_set_dash (cr, dash, 2, 0.0);
        cairo_set_source_rgba (cr, 0.11, 0.44, 0.96, 0.45);
        cairo_rectangle (cr, sx0, sy0, sw, sh);
        cairo_stroke (cr);
      }
      cairo_set_dash (cr, NULL, 0, 0.0);
      double hx[9], hy[9];
      get_selection_handle_centers (self, hx, hy);
      for (int h = 1; h <= 8; h++) {
        double rx = round (hx[h]) - 3.0;
        double ry = round (hy[h]) - 3.0;
        cairo_rectangle (cr, rx, ry, 6.0, 6.0);
        cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.98);
        cairo_fill_preserve (cr);
        cairo_set_source_rgba (cr, 0.11, 0.44, 0.96, 0.95);
        cairo_set_line_width (cr, 1.25);
        cairo_stroke (cr);
      }
    }
    cairo_destroy (cr);
  }

  gboolean eyedropper_active_mode = is_eyedropper_mode (self);
  gboolean show_ring = self->show_cursor_ring &&
                       self->has_hover &&
                       !self->pan_override_active &&
                       !eyedropper_active_mode &&
                       active_tool &&
                       !active_tool->is_select &&
                       g_strcmp0 (active_tool->id, "text") != 0 &&
                       g_strcmp0 (active_tool->id, "gradient") != 0;
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

static const PaintTool *
resolve_stroke_tool (PaintCanvas *self, GtkGesture *gesture, gboolean is_begin)
{
  const PaintTool *base = paint_tools_find (self->settings->active_tool_id);
  if (!self->settings || !gesture)
    return base;

  GdkEventSequence *seq = gtk_gesture_single_get_current_sequence (GTK_GESTURE_SINGLE (gesture));
  GdkEvent *ev = gtk_gesture_get_last_event (gesture, seq);
  double pressure = 1.0;
  if (ev) {
    double axis_val = 0.0;
    if (gdk_event_get_axis (ev, GDK_AXIS_PRESSURE, &axis_val))
      pressure = CLAMP (axis_val, 0.05, 1.0);
  }
  self->settings->stroke_pressure = pressure;

  if (is_begin) {
    self->stylus_override_tool = NULL;
    if (ev) {
      GdkDeviceTool *dtool = gdk_event_get_device_tool (ev);
      if (dtool &&
          gdk_device_tool_get_tool_type (dtool) == GDK_DEVICE_TOOL_TYPE_ERASER &&
          !base->is_select &&
          g_strcmp0 (base->id, "eyedropper") != 0 &&
          g_strcmp0 (base->id, "text") != 0) {
        self->stylus_override_tool = paint_tools_find ("eraser");
      }
    }
  }
  return self->stylus_override_tool ? self->stylus_override_tool : base;
}

static void
apply_selection_handle_resize (PaintCanvas *self,
                               double raw_cx,
                               double raw_cy,
                               GdkModifierType state)
{
  cairo_rectangle_int_t r = self->sel_resize_orig_rect;
  int x0 = r.x;
  int y0 = r.y;
  int x1 = r.x + r.width;
  int y1 = r.y + r.height;
  int dx = (int) lround (raw_cx - self->sel_move_anchor_x);
  int dy = (int) lround (raw_cy - self->sel_move_anchor_y);
  int h = self->sel_resize_handle;

  gboolean left = (h == 1 || h == 7 || h == 8);
  gboolean right = (h == 3 || h == 4 || h == 5);
  gboolean top = (h == 1 || h == 2 || h == 3);
  gboolean bottom = (h == 5 || h == 6 || h == 7);

  if (left)
    x0 = MIN (x0 + dx, x1 - 2);
  if (right)
    x1 = MAX (x1 + dx, x0 + 2);
  if (top)
    y0 = MIN (y0 + dy, y1 - 2);
  if (bottom)
    y1 = MAX (y1 + dy, y0 + 2);

  if ((left || right) && (top || bottom) && (state & GDK_SHIFT_MASK)) {
    double aspect = (double) MAX (1, r.width) / (double) MAX (1, r.height);
    int nw = x1 - x0;
    int nh = y1 - y0;
    if ((double) nw / aspect >= (double) nh)
      nh = MAX (2, (int) lround ((double) nw / aspect));
    else
      nw = MAX (2, (int) lround ((double) nh * aspect));
    if (left)
      x0 = x1 - nw;
    else
      x1 = x0 + nw;
    if (top)
      y0 = y1 - nh;
    else
      y1 = y0 + nh;
  }

  self->sel_rect = (cairo_rectangle_int_t) { x0, y0, x1 - x0, y1 - y0 };
}

static void
on_drag_begin (GtkGestureDrag *gesture,
               double start_x,
               double start_y,
               gpointer user_data)
{
  PaintCanvas *self = PAINT_CANVAS (user_data);
  gboolean is_touch = is_touch_gesture (GTK_GESTURE (gesture));

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

  const PaintTool *tool = resolve_stroke_tool (self, GTK_GESTURE (gesture), TRUE);
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

  if (tool->is_select) {
    self->touch_stroke_pending = FALSE;
    if (self->text_active)
      paint_canvas_commit_text (self);

    int handle = hit_test_selection_handle (self, cx, cy);
    if (self->has_selection && handle > 0) {
      lift_selection_to_floating (self);
      if (!self->floating_surface) {
        self->drawing_active = FALSE;
        return;
      }
      if (!self->sel_orig_floating)
        self->sel_orig_floating = cairo_surface_reference (self->floating_surface);
      if (!self->sel_orig_mask && self->sel_mask)
        self->sel_orig_mask = cairo_surface_reference (self->sel_mask);
      self->sel_resizing = TRUE;
      self->sel_resize_handle = handle;
      self->sel_resize_orig_rect = self->sel_rect;
      self->sel_move_anchor_x = cx;
      self->sel_move_anchor_y = cy;
      gtk_widget_queue_draw (GTK_WIDGET (self));
      return;
    }

    if (self->has_selection &&
        cx >= self->sel_rect.x && cx < self->sel_rect.x + self->sel_rect.width &&
        cy >= self->sel_rect.y && cy < self->sel_rect.y + self->sel_rect.height) {
      lift_selection_to_floating (self);
      if (!self->floating_surface) {
        self->drawing_active = FALSE;
        return;
      }
      self->sel_moving = TRUE;
      self->sel_move_anchor_x = cx;
      self->sel_move_anchor_y = cy;
      self->sel_move_orig_x = self->sel_rect.x;
      self->sel_move_orig_y = self->sel_rect.y;
      gtk_widget_queue_draw (GTK_WIDGET (self));
      return;
    }

    if (self->has_selection)
      paint_canvas_commit_selection (self);

    if (g_strcmp0 (tool->id, "select-wand") == 0) {
      self->drawing_active = FALSE;
      int sx = (int) floor (cx);
      int sy = (int) floor (cy);
      int active_idx = paint_document_get_active_layer (self->document);
      cairo_surface_t *src = self->settings->fill_sample_all_layers
                               ? self->document->surface
                               : paint_document_layer_get_surface (self->document, active_idx);
      cairo_rectangle_int_t r = { 0 };
      cairo_surface_t *mask = paint_tools_build_wand_mask (src, sx, sy,
                                                           self->settings->fill_tolerance, &r);
      if (mask && r.width > 0 && r.height > 0) {
        self->sel_mask = mask;
        self->sel_rect = r;
        self->has_selection = TRUE;
        rebuild_sel_ant_segments (self);
      } else if (mask) {
        cairo_surface_destroy (mask);
      }
      g_signal_emit (self, signals[SIGNAL_SELECTION_CHANGED], 0);
      gtk_widget_queue_draw (GTK_WIDGET (self));
      return;
    }

    self->sel_dragging_new = TRUE;
    self->has_selection = FALSE;
    g_clear_pointer (&self->sel_mask, cairo_surface_destroy);
    if (self->sel_ant_segs)
      g_array_set_size (self->sel_ant_segs, 0);
    if (g_strcmp0 (tool->id, "select-lasso") == 0) {
      g_array_set_size (self->lasso_points, 0);
      PaintPoint pt = {
        CLAMP (cx, 0.0, (double) self->document->width),
        CLAMP (cy, 0.0, (double) self->document->height)
      };
      g_array_append_val (self->lasso_points, pt);
      self->sel_rect = (cairo_rectangle_int_t) { (int) floor (pt.x), (int) floor (pt.y), 0, 0 };
    } else {
      self->sel_rect = (cairo_rectangle_int_t) {
        CLAMP ((int) floor (cx), 0, self->document->width),
        CLAMP ((int) floor (cy), 0, self->document->height),
        0, 0
      };
    }
    g_signal_emit (self, signals[SIGNAL_SELECTION_CHANGED], 0);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    return;
  }

  if (!check_active_layer_editable (self)) {
    self->drawing_active = FALSE;
    self->touch_stroke_pending = FALSE;
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
emit_cursor_coords (PaintCanvas *self, gboolean inside, double doc_x, double doc_y)
{
  if (!self->document || !inside) {
    g_signal_emit (self, signals[SIGNAL_CURSOR_MOVED], 0, FALSE, 0, 0);
    return;
  }
  int ix = CLAMP ((int) floor (doc_x), 0, MAX (0, self->document->width - 1));
  int iy = CLAMP ((int) floor (doc_y), 0, MAX (0, self->document->height - 1));
  g_signal_emit (self, signals[SIGNAL_CURSOR_MOVED], 0, TRUE, ix, iy);
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
  emit_cursor_coords (self, TRUE, raw_cx, raw_cy);

  if (self->eyedropper_active) {
    sample_color_at (self, raw_cx, raw_cy);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    return;
  }

  if (self->sel_resizing) {
    GdkModifierType state = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (gesture));
    apply_selection_handle_resize (self, raw_cx, raw_cy, state);
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
    const PaintTool *sel_tool = paint_tools_find (self->settings->active_tool_id);
    if (sel_tool && g_strcmp0 (sel_tool->id, "select-lasso") == 0) {
      double px = CLAMP (raw_cx, 0.0, (double) self->document->width);
      double py = CLAMP (raw_cy, 0.0, (double) self->document->height);
      if (self->lasso_points->len == 0) {
        PaintPoint pt = { px, py };
        g_array_append_val (self->lasso_points, pt);
      } else {
        PaintPoint last = g_array_index (self->lasso_points, PaintPoint, self->lasso_points->len - 1);
        if (hypot (px - last.x, py - last.y) >= 1.5) {
          PaintPoint pt = { px, py };
          g_array_append_val (self->lasso_points, pt);
        }
      }
      gtk_widget_queue_draw (GTK_WIDGET (self));
      return;
    }

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

  const PaintTool *tool = resolve_stroke_tool (self, GTK_GESTURE (gesture), FALSE);
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
    if (self->settings)
      self->settings->stroke_pressure = 1.0;
    self->stylus_override_tool = NULL;
    g_signal_emit (self, signals[SIGNAL_COLOR_PICKED], 0, TRUE);
    update_canvas_cursor (self);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    return;
  }

  if (self->sel_resizing) {
    GdkModifierType state = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (gesture));
    apply_selection_handle_resize (self, raw_cx, raw_cy, state);
    self->sel_resizing = FALSE;
    self->sel_resize_handle = 0;
    if (self->sel_orig_mask) {
      g_clear_pointer (&self->sel_mask, cairo_surface_destroy);
      self->sel_mask = scale_a8_mask (self->sel_orig_mask,
                                      self->sel_rect.width,
                                      self->sel_rect.height);
      rebuild_sel_ant_segments (self);
    }
    if (self->settings)
      self->settings->stroke_pressure = 1.0;
    self->stylus_override_tool = NULL;
    gtk_widget_queue_draw (GTK_WIDGET (self));
    g_signal_emit (self, signals[SIGNAL_SELECTION_CHANGED], 0);
    return;
  }

  if (self->sel_moving) {
    self->sel_moving = FALSE;
    if (self->settings)
      self->settings->stroke_pressure = 1.0;
    self->stylus_override_tool = NULL;
    g_signal_emit (self, signals[SIGNAL_SELECTION_CHANGED], 0);
    return;
  }

  if (self->sel_dragging_new) {
    const PaintTool *sel_tool = paint_tools_find (self->settings->active_tool_id);
    self->sel_dragging_new = FALSE;
    if (sel_tool && g_strcmp0 (sel_tool->id, "select-lasso") == 0) {
      if (self->lasso_points && self->lasso_points->len >= 3) {
        double minx = self->document->width, miny = self->document->height;
        double maxx = 0.0, maxy = 0.0;
        for (guint i = 0; i < self->lasso_points->len; i++) {
          PaintPoint pt = g_array_index (self->lasso_points, PaintPoint, i);
          if (pt.x < minx) minx = pt.x;
          if (pt.y < miny) miny = pt.y;
          if (pt.x > maxx) maxx = pt.x;
          if (pt.y > maxy) maxy = pt.y;
        }
        int x0 = CLAMP ((int) floor (minx), 0, self->document->width);
        int y0 = CLAMP ((int) floor (miny), 0, self->document->height);
        int x1 = CLAMP ((int) ceil (maxx), 0, self->document->width);
        int y1 = CLAMP ((int) ceil (maxy), 0, self->document->height);
        if (x1 - x0 >= 2 && y1 - y0 >= 2) {
          cairo_surface_t *mask = create_lasso_mask (self->lasso_points,
                                                     x0, y0, x1 - x0, y1 - y0);
          self->sel_mask = mask;
          self->sel_rect = (cairo_rectangle_int_t) { x0, y0, x1 - x0, y1 - y0 };
          rebuild_sel_ant_segments (self);
          if (self->sel_ant_segs->len > 0) {
            self->has_selection = TRUE;
          } else {
            g_clear_pointer (&self->sel_mask, cairo_surface_destroy);
            self->has_selection = FALSE;
            self->sel_rect = (cairo_rectangle_int_t) { 0, 0, 0, 0 };
          }
        }
      }
      if (self->lasso_points)
        g_array_set_size (self->lasso_points, 0);
    } else if (!self->has_selection) {
      self->sel_rect = (cairo_rectangle_int_t) { 0, 0, 0, 0 };
    } else if (sel_tool && g_strcmp0 (sel_tool->id, "select-ellipse") == 0) {
      self->sel_mask = create_ellipse_mask (self->sel_rect.width, self->sel_rect.height);
      rebuild_sel_ant_segments (self);
    }
    if (self->settings)
      self->settings->stroke_pressure = 1.0;
    self->stylus_override_tool = NULL;
    gtk_widget_queue_draw (GTK_WIDGET (self));
    g_signal_emit (self, signals[SIGNAL_SELECTION_CHANGED], 0);
    return;
  }

  if (self->text_dragging) {
    self->text_dragging = FALSE;
    if (self->settings)
      self->settings->stroke_pressure = 1.0;
    self->stylus_override_tool = NULL;
    return;
  }

  const PaintTool *tool = resolve_stroke_tool (self, GTK_GESTURE (gesture), FALSE);
  if (g_strcmp0 (tool->id, "text") == 0 || tool->is_select) {
    if (self->settings)
      self->settings->stroke_pressure = 1.0;
    self->stylus_override_tool = NULL;
    return;
  }

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
  if (self->settings)
    self->settings->stroke_pressure = 1.0;
  self->stylus_override_tool = NULL;
  if (g_strcmp0 (tool->id, "eraser") != 0 && !tool->is_transparency && !tool->is_retouch)
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
  if (self->settings)
    self->settings->stroke_pressure = 1.0;
  self->stylus_override_tool = NULL;

  if (self->eyedropper_active) {
    self->eyedropper_active = FALSE;
    update_canvas_cursor (self);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    return;
  }
  if (self->sel_resizing) {
    self->sel_rect = self->sel_resize_orig_rect;
    self->sel_resizing = FALSE;
    self->sel_resize_handle = 0;
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
    if (self->lasso_points)
      g_array_set_size (self->lasso_points, 0);
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
  emit_cursor_coords (self, TRUE, self->hover_x, self->hover_y);
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
  emit_cursor_coords (self, FALSE, 0.0, 0.0);
  if (self->show_cursor_ring || was_eyedropper || self->pick_badge_active)
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
clear_filter_state (PaintCanvas *self)
{
  g_clear_pointer (&self->filter_backup, cairo_surface_destroy);
  g_clear_pointer (&self->filter_mask, cairo_surface_destroy);
  self->filter_has_rect = FALSE;
  self->filter_rect = (cairo_rectangle_int_t) { 0, 0, 0, 0 };
  self->filter_alpha_lock = FALSE;
}

static void
paint_canvas_finalize (GObject *object)
{
  PaintCanvas *self = PAINT_CANVAS (object);
  g_clear_handle_id (&self->pick_badge_timeout_id, g_source_remove);
  clear_filter_state (self);
  invalidate_viewport_cache (self);
  g_clear_object (&self->checker_texture);
  g_clear_pointer (&self->vp_surface, cairo_surface_destroy);
  g_clear_pointer (&self->ov_surface, cairo_surface_destroy);
  g_clear_pointer (&self->sel_mask, cairo_surface_destroy);
  g_clear_pointer (&self->floating_hole_mask, cairo_surface_destroy);
  g_clear_pointer (&self->sel_orig_floating, cairo_surface_destroy);
  g_clear_pointer (&self->sel_orig_mask, cairo_surface_destroy);
  g_clear_pointer (&self->sel_ant_segs, g_array_unref);
  g_clear_pointer (&self->lasso_points, g_array_unref);
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
  signals[SIGNAL_HIDDEN_LAYER_BLOCKED] = g_signal_new ("hidden-layer-blocked",
                                                        G_TYPE_FROM_CLASS (klass),
                                                        G_SIGNAL_RUN_FIRST,
                                                        0, NULL, NULL, NULL,
                                                        G_TYPE_NONE, 0);
  signals[SIGNAL_LOCKED_LAYER_BLOCKED] = g_signal_new ("locked-layer-blocked",
                                                        G_TYPE_FROM_CLASS (klass),
                                                        G_SIGNAL_RUN_FIRST,
                                                        0, NULL, NULL, NULL,
                                                        G_TYPE_NONE, 0);
  signals[SIGNAL_CURSOR_MOVED] = g_signal_new ("cursor-moved",
                                                 G_TYPE_FROM_CLASS (klass),
                                                 G_SIGNAL_RUN_FIRST,
                                                 0, NULL, NULL, NULL,
                                                 G_TYPE_NONE, 3,
                                                 G_TYPE_BOOLEAN, G_TYPE_INT, G_TYPE_INT);
}

static void
paint_canvas_init (PaintCanvas *self)
{
  self->zoom = 1.0;
  self->show_cursor_ring = TRUE;
  self->show_pixel_grid = TRUE;
  self->sel_ant_segs = g_array_new (FALSE, FALSE, sizeof (PaintSelSeg));
  self->lasso_points = g_array_new (FALSE, FALSE, sizeof (PaintPoint));

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
  clear_filter_state (self);
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
  if (!check_active_layer_editable (self))
    return;
  paint_canvas_commit_active_overlays (self);
  paint_document_clear (self->document);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

void
paint_canvas_clear_transparent (PaintCanvas *self)
{
  if (!check_active_layer_editable (self))
    return;
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

static void
normalize_floating_for_transform (PaintCanvas *self)
{
  if (!self->floating_surface)
    return;
  int fw = cairo_image_surface_get_width (self->floating_surface);
  int fh = cairo_image_surface_get_height (self->floating_surface);
  if (fw != self->sel_rect.width || fh != self->sel_rect.height) {
    cairo_surface_t *scaled = scale_argb32_surface (self->floating_surface,
                                                    self->sel_rect.width,
                                                    self->sel_rect.height);
    if (scaled) {
      cairo_surface_destroy (self->floating_surface);
      self->floating_surface = scaled;
    }
  }
  g_clear_pointer (&self->sel_orig_floating, cairo_surface_destroy);
  g_clear_pointer (&self->sel_orig_mask, cairo_surface_destroy);
}

void
paint_canvas_rotate_90 (PaintCanvas *self, gboolean clockwise)
{
  if (self->has_selection) {
    lift_selection_to_floating (self);
    if (!self->floating_surface)
      return;
    normalize_floating_for_transform (self);
    cairo_surface_t *rot = paint_surface_rotate_90 (self->floating_surface, clockwise);
    cairo_surface_destroy (self->floating_surface);
    self->floating_surface = rot;
    if (self->sel_mask) {
      cairo_surface_t *rmask = transform_a8_mask_rotate_90 (self->sel_mask, clockwise);
      cairo_surface_destroy (self->sel_mask);
      self->sel_mask = rmask;
      rebuild_sel_ant_segments (self);
    }
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
    normalize_floating_for_transform (self);
    cairo_surface_t *flipped = paint_surface_flip (self->floating_surface, horizontal);
    cairo_surface_destroy (self->floating_surface);
    self->floating_surface = flipped;
    if (self->sel_mask) {
      cairo_surface_t *fmask = transform_a8_mask_flip (self->sel_mask, horizontal);
      cairo_surface_destroy (self->sel_mask);
      self->sel_mask = fmask;
      rebuild_sel_ant_segments (self);
    }
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

gboolean
paint_canvas_is_drawing (PaintCanvas *self)
{
  return self && self->drawing_active;
}

gboolean
paint_canvas_get_selection_mask_and_rect (PaintCanvas *self,
                                          cairo_rectangle_int_t *out_rect,
                                          cairo_surface_t **out_mask)
{
  if (!paint_canvas_has_selection (self))
    return FALSE;
  if (out_rect)
    *out_rect = self->sel_rect;
  if (out_mask)
    *out_mask = self->sel_mask;
  return TRUE;
}

void
paint_canvas_select_all (PaintCanvas *self)
{
  if (!self || !self->document)
    return;
  paint_canvas_commit_active_overlays (self);
  g_clear_pointer (&self->sel_mask, cairo_surface_destroy);
  if (self->sel_ant_segs)
    g_array_set_size (self->sel_ant_segs, 0);
  self->has_selection = TRUE;
  self->sel_rect = (cairo_rectangle_int_t) {
    0, 0, self->document->width, self->document->height
  };
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_SELECTION_CHANGED], 0);
}

void
paint_canvas_invert_selection (PaintCanvas *self)
{
  if (!self || !self->document)
    return;
  if (!paint_canvas_has_selection (self)) {
    paint_canvas_select_all (self);
    return;
  }

  cairo_rectangle_int_t old_r = self->sel_rect;
  cairo_surface_t *old_mask = self->sel_mask ? cairo_surface_reference (self->sel_mask) : NULL;
  paint_canvas_commit_active_overlays (self);

  int dw = self->document->width;
  int dh = self->document->height;
  const unsigned char *mdata = NULL;
  int mstride = 0;
  int mw = 0, mh = 0;
  if (old_mask) {
    cairo_surface_flush (old_mask);
    mdata = cairo_image_surface_get_data (old_mask);
    mstride = cairo_image_surface_get_stride (old_mask);
    mw = cairo_image_surface_get_width (old_mask);
    mh = cairo_image_surface_get_height (old_mask);
  }

  int min_x = dw, min_y = dh, max_x = -1, max_y = -1;
  guint8 *inv = g_malloc ((gsize) dw * (gsize) dh);
  for (int y = 0; y < dh; y++) {
    guint8 *irow = inv + (gsize) y * (gsize) dw;
    gboolean in_y = (y >= old_r.y && y < old_r.y + old_r.height);
    const unsigned char *mrow = NULL;
    if (mdata && in_y) {
      int my = y - old_r.y;
      if (my >= 0 && my < mh)
        mrow = mdata + (gsize) my * (gsize) mstride;
    }
    for (int x = 0; x < dw; x++) {
      gboolean inside = FALSE;
      if (in_y && x >= old_r.x && x < old_r.x + old_r.width) {
        if (!old_mask) {
          inside = TRUE;
        } else if (mrow) {
          int mx = x - old_r.x;
          if (mx >= 0 && mx < mw && mrow[mx] >= 128)
            inside = TRUE;
        }
      }
      if (!inside) {
        irow[x] = 255;
        if (x < min_x) min_x = x;
        if (x > max_x) max_x = x;
        if (y < min_y) min_y = y;
        if (y > max_y) max_y = y;
      } else {
        irow[x] = 0;
      }
    }
  }
  g_clear_pointer (&old_mask, cairo_surface_destroy);

  if (max_x < min_x || max_y < min_y) {
    g_free (inv);
    clear_selection_state (self);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    g_signal_emit (self, signals[SIGNAL_SELECTION_CHANGED], 0);
    return;
  }

  int cw = max_x - min_x + 1;
  int ch = max_y - min_y + 1;
  cairo_surface_t *cropped = cairo_image_surface_create (CAIRO_FORMAT_A8, cw, ch);
  unsigned char *cdata = cairo_image_surface_get_data (cropped);
  int cstride = cairo_image_surface_get_stride (cropped);
  for (int y = 0; y < ch; y++) {
    memcpy (cdata + (gsize) y * (gsize) cstride,
            inv + (gsize) (min_y + y) * (gsize) dw + (gsize) min_x,
            (gsize) cw);
  }
  cairo_surface_mark_dirty (cropped);
  g_free (inv);

  self->has_selection = TRUE;
  self->sel_rect = (cairo_rectangle_int_t) { min_x, min_y, cw, ch };
  self->sel_mask = cropped;
  rebuild_sel_ant_segments (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_SELECTION_CHANGED], 0);
}

void
paint_canvas_commit_selection (PaintCanvas *self)
{
  if (!self || !self->has_selection)
    return;

  if (self->floating_surface) {
    if (!check_active_layer_editable (self))
      return;
    paint_document_commit_move_masked (self->document,
                                       self->floating_has_hole ? &self->floating_hole : NULL,
                                       self->floating_hole_mask,
                                       self->floating_surface,
                                       &self->sel_rect);
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
  if (!check_active_layer_editable (self))
    return FALSE;

  if (self->floating_surface) {
    if (self->floating_has_hole) {
      paint_document_clear_masked_rect (self->document,
                                        &self->floating_hole,
                                        self->floating_hole_mask);
    }
    clear_selection_state (self);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
    return TRUE;
  }

  cairo_rectangle_int_t r = self->sel_rect;
  cairo_surface_t *mask = self->sel_mask ? cairo_surface_reference (self->sel_mask) : NULL;
  clear_selection_state (self);
  paint_document_clear_masked_rect (self->document, &r, mask);
  g_clear_pointer (&mask, cairo_surface_destroy);
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

gboolean
paint_canvas_trim_transparent (PaintCanvas *self)
{
  if (!self || !self->document)
    return FALSE;
  paint_canvas_cancel_filter_preview (self);
  paint_canvas_commit_active_overlays (self);
  if (!paint_document_trim_transparent (self->document))
    return FALSE;
  invalidate_viewport_cache (self);
  update_size_request (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
  return TRUE;
}

static cairo_surface_t *
dup_scaled_floating_surface (const PaintCanvas *self)
{
  if (!self->floating_surface)
    return NULL;
  int fw = cairo_image_surface_get_width (self->floating_surface);
  int fh = cairo_image_surface_get_height (self->floating_surface);
  if (fw == self->sel_rect.width && fh == self->sel_rect.height)
    return cairo_surface_reference (self->floating_surface);
  return scale_argb32_surface (self->floating_surface,
                               MAX (1, self->sel_rect.width),
                               MAX (1, self->sel_rect.height));
}

GdkTexture *
paint_canvas_copy_selection (PaintCanvas *self)
{
  if (!self || !self->document)
    return NULL;

  if (self->has_selection) {
    if (self->floating_surface) {
      cairo_surface_t *fs = dup_scaled_floating_surface (self);
      if (!fs)
        return NULL;
      GdkTexture *tex = paint_texture_from_surface (fs);
      cairo_surface_destroy (fs);
      return tex;
    }
    cairo_surface_t *copy = paint_document_copy_masked_rect (self->document,
                                                             &self->sel_rect,
                                                             self->sel_mask);
    if (!copy)
      return NULL;
    GdkTexture *tex = paint_texture_from_surface (copy);
    cairo_surface_destroy (copy);
    return tex;
  }

  return paint_texture_from_surface (self->document->surface);
}

GdkTexture *
paint_canvas_copy_merged_selection (PaintCanvas *self)
{
  if (!self || !self->document)
    return NULL;

  if (self->has_selection) {
    cairo_surface_t *copy = paint_document_copy_merged_masked_rect (
        self->document,
        &self->sel_rect,
        self->floating_surface ? NULL : self->sel_mask);
    if (!copy)
      return NULL;
    if (self->floating_surface) {
      cairo_surface_t *fs = dup_scaled_floating_surface (self);
      if (fs) {
        cairo_t *cr = cairo_create (copy);
        cairo_set_operator (cr, CAIRO_OPERATOR_OVER);
        cairo_set_source_surface (cr, fs, 0.0, 0.0);
        cairo_paint (cr);
        cairo_destroy (cr);
        cairo_surface_destroy (fs);
      }
    }
    GdkTexture *tex = paint_texture_from_surface (copy);
    cairo_surface_destroy (copy);
    return tex;
  }

  return paint_texture_from_surface (self->document->surface);
}

GdkTexture *
paint_canvas_cut_selection (PaintCanvas *self)
{
  if (!check_active_layer_editable (self))
    return NULL;
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
  if (!check_active_layer_editable (self))
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

  g_clear_pointer (&self->sel_mask, cairo_surface_destroy);
  g_clear_pointer (&self->sel_orig_floating, cairo_surface_destroy);
  g_clear_pointer (&self->sel_orig_mask, cairo_surface_destroy);
  g_clear_pointer (&self->floating_hole_mask, cairo_surface_destroy);
  if (self->sel_ant_segs)
    g_array_set_size (self->sel_ant_segs, 0);
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
  if (!self->floating_surface)
    return;
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
  if (drew && !check_active_layer_editable (self))
    return;
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
paint_canvas_set_pan_override (PaintCanvas *self, gboolean active, gboolean dragging)
{
  if (!self)
    return;
  if (self->pan_override_active == active && self->pan_override_dragging == dragging)
    return;
  self->pan_override_active = active;
  self->pan_override_dragging = active && dragging;
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

int
paint_canvas_add_layer (PaintCanvas *self, const char *name)
{
  if (!self || !self->document)
    return -1;
  paint_canvas_commit_active_overlays (self);
  int idx = paint_document_add_layer (self->document, name);
  if (idx >= 0) {
    gtk_widget_queue_draw (GTK_WIDGET (self));
    g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
  }
  return idx;
}

int
paint_canvas_add_layer_from_texture (PaintCanvas *self,
                                     const char *name,
                                     GdkTexture *texture)
{
  if (!self || !self->document || !texture)
    return -1;
  paint_canvas_commit_active_overlays (self);
  cairo_surface_t *raw = paint_surface_from_texture (texture);
  if (!raw)
    return -1;

  int pw = cairo_image_surface_get_width (raw);
  int ph = cairo_image_surface_get_height (raw);
  cairo_surface_t *placed = raw;
  if (pw < self->document->width || ph < self->document->height) {
    int ox = (pw < self->document->width) ? (self->document->width - pw) / 2 : 0;
    int oy = (ph < self->document->height) ? (self->document->height - ph) / 2 : 0;
    placed = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
                                         self->document->width,
                                         self->document->height);
    cairo_t *cr = cairo_create (placed);
    cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface (cr, raw, (double) ox, (double) oy);
    cairo_paint (cr);
    cairo_destroy (cr);
    cairo_surface_destroy (raw);
  }

  int idx = paint_document_add_layer_from_surface (self->document, name, placed);
  cairo_surface_destroy (placed);
  if (idx >= 0) {
    gtk_widget_queue_draw (GTK_WIDGET (self));
    g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
  }
  return idx;
}

gboolean
paint_canvas_duplicate_layer (PaintCanvas *self, int index)
{
  if (!self || !self->document)
    return FALSE;
  paint_canvas_commit_active_overlays (self);
  if (!paint_document_duplicate_layer (self->document, index))
    return FALSE;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
  return TRUE;
}

gboolean
paint_canvas_remove_layer (PaintCanvas *self, int index)
{
  if (!self || !self->document)
    return FALSE;
  paint_canvas_commit_active_overlays (self);
  if (!paint_document_remove_layer (self->document, index))
    return FALSE;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
  return TRUE;
}

gboolean
paint_canvas_move_layer (PaintCanvas *self, int from_index, int to_index)
{
  if (!self || !self->document)
    return FALSE;
  paint_canvas_commit_active_overlays (self);
  if (!paint_document_move_layer (self->document, from_index, to_index))
    return FALSE;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
  return TRUE;
}

gboolean
paint_canvas_merge_layer_down (PaintCanvas *self, int index)
{
  if (!self || !self->document)
    return FALSE;
  paint_canvas_commit_active_overlays (self);
  if (!paint_document_merge_layer_down (self->document, index))
    return FALSE;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
  return TRUE;
}

gboolean
paint_canvas_flatten_layers (PaintCanvas *self)
{
  if (!self || !self->document)
    return FALSE;
  paint_canvas_commit_active_overlays (self);
  if (!paint_document_flatten_layers (self->document))
    return FALSE;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
  return TRUE;
}

void
paint_canvas_set_active_layer (PaintCanvas *self, int index)
{
  if (!self || !self->document)
    return;
  paint_canvas_commit_active_overlays (self);
  paint_document_set_active_layer (self->document, index);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

void
paint_canvas_set_layer_visible (PaintCanvas *self, int index, gboolean visible)
{
  if (!self || !self->document)
    return;
  paint_canvas_commit_active_overlays (self);
  paint_document_layer_set_visible (self->document, index, visible);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

void
paint_canvas_set_layers_visibility (PaintCanvas *self,
                                    const gboolean *visible,
                                    int count)
{
  if (!self || !self->document || !visible || count <= 0)
    return;
  paint_canvas_commit_active_overlays (self);
  paint_document_set_layers_visibility (self->document, visible, count);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

void
paint_canvas_set_layer_opacity (PaintCanvas *self, int index, double opacity)
{
  if (!self || !self->document)
    return;
  paint_canvas_commit_active_overlays (self);
  paint_document_layer_set_opacity (self->document, index, opacity);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

void
paint_canvas_set_layer_blend_mode (PaintCanvas *self, int index, PaintBlendMode mode)
{
  if (!self || !self->document)
    return;
  paint_canvas_commit_active_overlays (self);
  paint_document_layer_set_blend_mode (self->document, index, mode);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

void
paint_canvas_set_layer_alpha_lock (PaintCanvas *self, int index, gboolean alpha_lock)
{
  if (!self || !self->document)
    return;
  paint_canvas_commit_active_overlays (self);
  paint_document_layer_set_alpha_lock (self->document, index, alpha_lock);
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

void
paint_canvas_set_layer_locked (PaintCanvas *self, int index, gboolean locked)
{
  if (!self || !self->document)
    return;
  paint_canvas_commit_active_overlays (self);
  paint_document_layer_set_locked (self->document, index, locked);
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

void
paint_canvas_set_layer_name (PaintCanvas *self, int index, const char *name)
{
  if (!self || !self->document)
    return;
  paint_canvas_commit_active_overlays (self);
  paint_document_layer_set_name (self->document, index, name);
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

gboolean
paint_canvas_begin_filter_preview (PaintCanvas *self)
{
  if (!self || !self->document)
    return FALSE;
  if (!check_active_layer_editable (self))
    return FALSE;

  if (self->filter_backup)
    paint_canvas_cancel_filter_preview (self);
  if (self->text_active)
    paint_canvas_commit_text (self);

  clear_filter_state (self);
  if (self->has_selection && self->sel_rect.width > 0 && self->sel_rect.height > 0) {
    self->filter_has_rect = TRUE;
    self->filter_rect = self->sel_rect;
    if (self->sel_mask &&
        cairo_image_surface_get_width (self->sel_mask) == self->sel_rect.width &&
        cairo_image_surface_get_height (self->sel_mask) == self->sel_rect.height) {
      self->filter_mask = cairo_surface_reference (self->sel_mask);
    }
    if (self->floating_surface)
      paint_canvas_commit_selection (self);
  }

  self->filter_backup = paint_document_dup_active_layer_surface (self->document);
  if (!self->filter_backup) {
    clear_filter_state (self);
    return FALSE;
  }
  self->filter_alpha_lock = paint_document_layer_get_alpha_lock (
      self->document,
      paint_document_get_active_layer (self->document));
  return TRUE;
}

void
paint_canvas_update_filter_preview (PaintCanvas *self,
                                    PaintCanvasFilterFunc func,
                                    gpointer user_data)
{
  if (!self || !self->document || !self->filter_backup || !func)
    return;

  const cairo_rectangle_int_t *r = self->filter_has_rect ? &self->filter_rect : NULL;
  paint_document_restore_active_layer_rect (self->document, self->filter_backup, r);

  int active = paint_document_get_active_layer (self->document);
  cairo_surface_t *layer_surf = paint_document_layer_get_surface (self->document, active);
  if (layer_surf)
    func (layer_surf, r, self->filter_mask, self->filter_alpha_lock, user_data);

  paint_document_notify_active_layer_rect_modified (self->document, r);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
paint_canvas_commit_filter_preview (PaintCanvas *self)
{
  if (!self || !self->document || !self->filter_backup)
    return;

  const cairo_rectangle_int_t *r = self->filter_has_rect ? &self->filter_rect : NULL;
  paint_document_commit_active_layer_from_backup (self->document, self->filter_backup, r);
  clear_filter_state (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

void
paint_canvas_cancel_filter_preview (PaintCanvas *self)
{
  if (!self || !self->document || !self->filter_backup)
    return;

  const cairo_rectangle_int_t *r = self->filter_has_rect ? &self->filter_rect : NULL;
  paint_document_restore_active_layer_rect (self->document, self->filter_backup, r);
  clear_filter_state (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

gboolean
paint_canvas_apply_filter (PaintCanvas *self,
                           PaintCanvasFilterFunc func,
                           gpointer user_data)
{
  if (!paint_canvas_begin_filter_preview (self))
    return FALSE;
  paint_canvas_update_filter_preview (self, func, user_data);
  paint_canvas_commit_filter_preview (self);
  return TRUE;
}
