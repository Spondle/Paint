#include "paint-tools.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FILL_TOLERANCE 72
#define FILL_FRINGE_MAX_DIST 185
#define FILL_COLLINEAR_ERR 18

PaintToolSettings *
paint_tool_settings_new (void)
{
  PaintToolSettings *settings = g_new0 (PaintToolSettings, 1);
  settings->color = (GdkRGBA) { 0.11f, 0.44f, 0.96f, 1.0f };
  settings->size = 6.0;
  settings->background_color = (GdkRGBA) { 1.0f, 1.0f, 1.0f, 1.0f };
  settings->active_tool_id = "brush";
  settings->brush_style = PAINT_BRUSH_SMOOTH;
  settings->text_font_family = g_strdup ("Sans");
  settings->text_font_size = 28.0;
  settings->history_points = g_array_new (FALSE, FALSE, sizeof (PaintPoint));
  return settings;
}

void
paint_tool_settings_free (PaintToolSettings *settings)
{
  if (!settings)
    return;
  if (settings->history_points)
    g_array_free (settings->history_points, TRUE);
  g_free (settings->text_font_family);
  g_free (settings);
}

void
paint_tool_settings_set_text_font_family (PaintToolSettings *settings, const char *family)
{
  if (!settings)
    return;
  g_free (settings->text_font_family);
  settings->text_font_family = g_strdup ((family && *family) ? family : "Sans");
}

static gboolean
rgba_nearly_equal (const GdkRGBA *a, const GdkRGBA *b)
{
  return fabsf (a->red - b->red) < 0.01f &&
         fabsf (a->green - b->green) < 0.01f &&
         fabsf (a->blue - b->blue) < 0.01f &&
         fabsf (a->alpha - b->alpha) < 0.01f;
}

void
paint_tool_settings_add_recent_color (PaintToolSettings *settings, const GdkRGBA *color)
{
  if (!settings || !color)
    return;

  int found = -1;
  for (int i = 0; i < settings->n_recent_colors; i++) {
    if (rgba_nearly_equal (&settings->recent_colors[i], color)) {
      found = i;
      break;
    }
  }

  if (found == 0) {
    settings->recent_colors[0] = *color;
    return;
  }

  if (found > 0) {
    for (int i = found; i > 0; i--)
      settings->recent_colors[i] = settings->recent_colors[i - 1];
    settings->recent_colors[0] = *color;
    return;
  }

  int limit = MIN (settings->n_recent_colors, PAINT_MAX_RECENT_COLORS - 1);
  for (int i = limit; i > 0; i--)
    settings->recent_colors[i] = settings->recent_colors[i - 1];
  settings->recent_colors[0] = *color;
  if (settings->n_recent_colors < PAINT_MAX_RECENT_COLORS)
    settings->n_recent_colors++;
}

static void
configure_brush_context (cairo_t *cr, const PaintToolSettings *settings)
{
  cairo_set_source_rgba (cr,
                         settings->color.red,
                         settings->color.green,
                         settings->color.blue,
                         settings->color.alpha);
  cairo_set_operator (cr, CAIRO_OPERATOR_OVER);
}

static void
configure_eraser_context (cairo_t *cr, const PaintToolSettings *settings)
{
  if (settings->background_color.alpha <= 0.0f) {
    cairo_set_operator (cr, CAIRO_OPERATOR_CLEAR);
    return;
  }
  cairo_set_source_rgba (cr,
                         settings->background_color.red,
                         settings->background_color.green,
                         settings->background_color.blue,
                         settings->background_color.alpha);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
}

static void
configure_transparent_context (cairo_t *cr)
{
  cairo_set_operator (cr, CAIRO_OPERATOR_CLEAR);
}

static void
configure_for_tool (const PaintTool *tool, cairo_t *cr, const PaintToolSettings *settings)
{
  if (g_strcmp0 (tool->id, "transparent") == 0)
    configure_transparent_context (cr);
  else if (g_strcmp0 (tool->id, "eraser") == 0)
    configure_eraser_context (cr, settings);
  else
    configure_brush_context (cr, settings);
}

static void
draw_pixel_stamp (cairo_t *cr, int ix, int iy, double size)
{
  int sz = MAX (1, (int) lround (size));
  int px = ix - sz / 2;
  int py = iy - sz / 2;
  cairo_rectangle (cr, (double) px, (double) py, (double) sz, (double) sz);
  cairo_fill (cr);
}

static void
draw_pixel_segment (const PaintTool *tool,
                    cairo_t *cr,
                    double x0,
                    double y0,
                    double x1,
                    double y1,
                    const PaintToolSettings *settings)
{
  configure_for_tool (tool, cr, settings);
  cairo_set_antialias (cr, CAIRO_ANTIALIAS_NONE);
  int ix0 = (int) floor (x0);
  int iy0 = (int) floor (y0);
  int ix1 = (int) floor (x1);
  int iy1 = (int) floor (y1);
  int steps = MAX (abs (ix1 - ix0), abs (iy1 - iy0));
  if (steps == 0)
    return;

  for (int i = 1; i <= steps; i++) {
    double t = (double) i / (double) steps;
    int sx = (int) lround ((double) ix0 + (double) (ix1 - ix0) * t);
    int sy = (int) lround ((double) iy0 + (double) (iy1 - iy0) * t);
    draw_pixel_stamp (cr, sx, sy, settings->size);
  }
}

static void
draw_soft_stamp (const PaintTool *tool,
                 cairo_t *cr,
                 double x,
                 double y,
                 const PaintToolSettings *settings)
{
  double r = MAX (1.5, settings->size * 0.5);
  cairo_pattern_t *pat = cairo_pattern_create_radial (x, y, 0.0, x, y, r);

  gboolean erase_alpha = (g_strcmp0 (tool->id, "transparent") == 0) ||
                         (g_strcmp0 (tool->id, "eraser") == 0 &&
                          settings->background_color.alpha <= 0.0f);
  if (erase_alpha) {
    cairo_set_operator (cr, CAIRO_OPERATOR_DEST_OUT);
    cairo_pattern_add_color_stop_rgba (pat, 0.0, 0.0, 0.0, 0.0, 0.35);
    cairo_pattern_add_color_stop_rgba (pat, 0.5, 0.0, 0.0, 0.0, 0.14);
    cairo_pattern_add_color_stop_rgba (pat, 1.0, 0.0, 0.0, 0.0, 0.0);
  } else {
    GdkRGBA c = (g_strcmp0 (tool->id, "eraser") == 0)
                  ? settings->background_color
                  : settings->color;
    double a = CLAMP ((double) c.alpha, 0.0, 1.0);
    cairo_set_operator (cr, CAIRO_OPERATOR_OVER);
    cairo_pattern_add_color_stop_rgba (pat, 0.0, c.red, c.green, c.blue, a * 0.35);
    cairo_pattern_add_color_stop_rgba (pat, 0.5, c.red, c.green, c.blue, a * 0.14);
    cairo_pattern_add_color_stop_rgba (pat, 1.0, c.red, c.green, c.blue, 0.0);
  }

  cairo_set_source (cr, pat);
  cairo_arc (cr, x, y, r, 0.0, 2.0 * G_PI);
  cairo_fill (cr);
  cairo_pattern_destroy (pat);
}

static void
draw_soft_segment (const PaintTool *tool,
                   cairo_t *cr,
                   double x0,
                   double y0,
                   double x1,
                   double y1,
                   const PaintToolSettings *settings)
{
  double r = MAX (1.5, settings->size * 0.5);
  double dist = hypot (x1 - x0, y1 - y0);
  if (dist < 0.5)
    return;
  double spacing = MAX (1.0, r * 0.25);
  int steps = MAX (1, (int) ceil (dist / spacing));
  for (int i = 1; i <= steps; i++) {
    double t = (double) i / (double) steps;
    draw_soft_stamp (tool, cr, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, settings);
  }
}

static void
freehand_begin_stroke (const PaintTool *tool,
                       cairo_t *cr,
                       double x,
                       double y,
                       PaintToolSettings *settings)
{
  PaintPoint pt = { x, y };
  g_array_set_size (settings->history_points, 0);
  g_array_append_val (settings->history_points, pt);

  if (settings->brush_style == PAINT_BRUSH_PIXEL) {
    configure_for_tool (tool, cr, settings);
    cairo_set_antialias (cr, CAIRO_ANTIALIAS_NONE);
    draw_pixel_stamp (cr, (int) floor (x), (int) floor (y), settings->size);
    return;
  }
  if (settings->brush_style == PAINT_BRUSH_SOFT) {
    draw_soft_stamp (tool, cr, x, y, settings);
    return;
  }

  configure_for_tool (tool, cr, settings);
  cairo_arc (cr, x, y, MAX (0.5, settings->size / 2.0), 0.0, 2.0 * G_PI);
  cairo_fill (cr);
}

static void
freehand_continue_stroke (const PaintTool *tool,
                          cairo_t *cr,
                          double x,
                          double y,
                          PaintToolSettings *settings)
{
  GArray *pts = settings->history_points;
  if (pts->len == 0) {
    freehand_begin_stroke (tool, cr, x, y, settings);
    return;
  }

  if (settings->brush_style == PAINT_BRUSH_PIXEL) {
    PaintPoint last = g_array_index (pts, PaintPoint, pts->len - 1);
    draw_pixel_segment (tool, cr, last.x, last.y, x, y, settings);
    PaintPoint next = { x, y };
    g_array_append_val (pts, next);
    return;
  }
  if (settings->brush_style == PAINT_BRUSH_SOFT) {
    PaintPoint last = g_array_index (pts, PaintPoint, pts->len - 1);
    draw_soft_segment (tool, cr, last.x, last.y, x, y, settings);
    PaintPoint next = { x, y };
    g_array_append_val (pts, next);
    return;
  }

  configure_for_tool (tool, cr, settings);
  cairo_set_line_width (cr, MAX (1.0, settings->size));
  cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);

  if (pts->len == 1) {
    PaintPoint p0 = g_array_index (pts, PaintPoint, 0);
    cairo_move_to (cr, p0.x, p0.y);
    cairo_line_to (cr, x, y);
    cairo_stroke (cr);
  } else {
    PaintPoint p0 = g_array_index (pts, PaintPoint, pts->len - 2);
    PaintPoint p1 = g_array_index (pts, PaintPoint, pts->len - 1);
    double mid1_x = (p0.x + p1.x) / 2.0;
    double mid1_y = (p0.y + p1.y) / 2.0;
    double mid2_x = (p1.x + x) / 2.0;
    double mid2_y = (p1.y + y) / 2.0;
    cairo_move_to (cr, mid1_x, mid1_y);
    cairo_curve_to (cr, p1.x, p1.y, p1.x, p1.y, mid2_x, mid2_y);
    cairo_stroke (cr);
  }

  PaintPoint next = { x, y };
  g_array_append_val (pts, next);
}

static void
freehand_end_stroke (const PaintTool *tool,
                     cairo_t *cr,
                     double x,
                     double y,
                     PaintToolSettings *settings)
{
  GArray *pts = settings->history_points;
  if (settings->brush_style == PAINT_BRUSH_PIXEL) {
    if (pts->len >= 1) {
      PaintPoint last = g_array_index (pts, PaintPoint, pts->len - 1);
      draw_pixel_segment (tool, cr, last.x, last.y, x, y, settings);
    }
    g_array_set_size (pts, 0);
    return;
  }
  if (settings->brush_style == PAINT_BRUSH_SOFT) {
    if (pts->len >= 1) {
      PaintPoint last = g_array_index (pts, PaintPoint, pts->len - 1);
      draw_soft_segment (tool, cr, last.x, last.y, x, y, settings);
    }
    g_array_set_size (pts, 0);
    return;
  }

  if (pts->len >= 2) {
    PaintPoint last = g_array_index (pts, PaintPoint, pts->len - 1);
    configure_for_tool (tool, cr, settings);
    cairo_set_line_width (cr, MAX (1.0, settings->size));
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to (cr, last.x, last.y);
    cairo_line_to (cr, x, y);
    cairo_stroke (cr);
  }
  g_array_set_size (pts, 0);
}

typedef struct {
  int x;
  int y;
} FillSeed;

static inline gboolean
pixel_matches (uint32_t px, uint32_t target, int tol)
{
  if (px == target)
    return TRUE;
  int a0 = (int) ((target >> 24) & 0xFF);
  int r0 = (int) ((target >> 16) & 0xFF);
  int g0 = (int) ((target >> 8) & 0xFF);
  int b0 = (int) (target & 0xFF);

  int a1 = (int) ((px >> 24) & 0xFF);
  int r1 = (int) ((px >> 16) & 0xFF);
  int g1 = (int) ((px >> 8) & 0xFF);
  int b1 = (int) (px & 0xFF);

  return abs (a1 - a0) <= tol &&
         abs (r1 - r0) <= tol &&
         abs (g1 - g0) <= tol &&
         abs (b1 - b0) <= tol;
}

static inline int
pixel_max_diff (uint32_t px, uint32_t target)
{
  int da = abs ((int) ((px >> 24) & 0xFF) - (int) ((target >> 24) & 0xFF));
  int dr = abs ((int) ((px >> 16) & 0xFF) - (int) ((target >> 16) & 0xFF));
  int dg = abs ((int) ((px >> 8) & 0xFF) - (int) ((target >> 8) & 0xFF));
  int db = abs ((int) (px & 0xFF) - (int) (target & 0xFF));
  return MAX (MAX (da, dr), MAX (dg, db));
}

static inline gboolean
pixel_is_blend_between (uint32_t px, uint32_t target, uint32_t fill)
{
  double ta = (double) ((target >> 24) & 0xFF);
  double tr = (double) ((target >> 16) & 0xFF);
  double tg = (double) ((target >> 8) & 0xFF);
  double tb = (double) (target & 0xFF);

  double da = (double) ((fill >> 24) & 0xFF) - ta;
  double dr = (double) ((fill >> 16) & 0xFF) - tr;
  double dg = (double) ((fill >> 8) & 0xFF) - tg;
  double db = (double) (fill & 0xFF) - tb;

  double len2 = da * da + dr * dr + dg * dg + db * db;
  if (len2 < 1.0)
    return FALSE;

  double pa = (double) ((px >> 24) & 0xFF) - ta;
  double pr = (double) ((px >> 16) & 0xFF) - tr;
  double pg = (double) ((px >> 8) & 0xFF) - tg;
  double pb = (double) (px & 0xFF) - tb;

  double t = (pa * da + pr * dr + pg * dg + pb * db) / len2;
  if (t <= 0.0 || t >= 0.995)
    return FALSE;
  if (t > 1.0)
    t = 1.0;

  double ea = fabs (pa - t * da);
  double er = fabs (pr - t * dr);
  double eg = fabs (pg - t * dg);
  double eb = fabs (pb - t * db);
  return MAX (MAX (ea, er), MAX (eg, eb)) <= (double) FILL_COLLINEAR_ERR;
}

static inline uint32_t
rgba_to_premultiplied_argb32 (const GdkRGBA *c, uint8_t *out_a)
{
  uint8_t a = (uint8_t) lround (CLAMP (c->alpha, 0.0f, 1.0f) * 255.0);
  uint8_t r = (uint8_t) lround (CLAMP (c->red, 0.0f, 1.0f) * a);
  uint8_t g = (uint8_t) lround (CLAMP (c->green, 0.0f, 1.0f) * a);
  uint8_t b = (uint8_t) lround (CLAMP (c->blue, 0.0f, 1.0f) * a);
  if (out_a)
    *out_a = a;
  return ((uint32_t) a << 24) | ((uint32_t) r << 16) | ((uint32_t) g << 8) | (uint32_t) b;
}

static inline uint32_t
blend_over_argb32 (uint32_t dst, uint32_t src_pm, uint8_t src_a)
{
  if (src_a == 255)
    return src_pm;
  if (src_a == 0)
    return dst;

  uint32_t inv_a = 255 - src_a;
  uint32_t dst_a = (dst >> 24) & 0xFF;
  uint32_t dst_r = (dst >> 16) & 0xFF;
  uint32_t dst_g = (dst >> 8) & 0xFF;
  uint32_t dst_b = dst & 0xFF;

  uint32_t src_r = (src_pm >> 16) & 0xFF;
  uint32_t src_g = (src_pm >> 8) & 0xFF;
  uint32_t src_b = src_pm & 0xFF;

  uint32_t out_a = src_a + (dst_a * inv_a + 127) / 255;
  uint32_t out_r = src_r + (dst_r * inv_a + 127) / 255;
  uint32_t out_g = src_g + (dst_g * inv_a + 127) / 255;
  uint32_t out_b = src_b + (dst_b * inv_a + 127) / 255;

  return (MIN (255u, out_a) << 24) |
         (MIN (255u, out_r) << 16) |
         (MIN (255u, out_g) << 8) |
         MIN (255u, out_b);
}

static void
flood_fill_surface (cairo_t *cr,
                    double x,
                    double y,
                    const PaintToolSettings *settings,
                    gboolean make_transparent)
{
  cairo_surface_t *surface = cairo_get_target (cr);
  if (!surface || cairo_surface_get_type (surface) != CAIRO_SURFACE_TYPE_IMAGE)
    return;

  cairo_surface_flush (surface);
  int w = cairo_image_surface_get_width (surface);
  int h = cairo_image_surface_get_height (surface);
  int stride = cairo_image_surface_get_stride (surface);
  unsigned char *data = cairo_image_surface_get_data (surface);

  int sx = (int) floor (x);
  int sy = (int) floor (y);
  if (sx < 0 || sx >= w || sy < 0 || sy >= h)
    return;

  uint8_t fill_a = 0;
  uint32_t fill_px = 0;
  if (!make_transparent) {
    fill_px = rgba_to_premultiplied_argb32 (&settings->color, &fill_a);
    if (fill_a == 0)
      return;
  }

  uint32_t *start_row = (uint32_t *) (data + (gsize) sy * (gsize) stride);
  uint32_t target_px = start_row[sx];
  if (make_transparent) {
    if (((target_px >> 24) & 0xFFU) == 0)
      return;
  } else if (fill_a == 255 && target_px == fill_px) {
    return;
  }

  guint8 *visited = g_malloc0 ((gsize) w * (gsize) h);
  GArray *stack = g_array_new (FALSE, FALSE, sizeof (FillSeed));
  FillSeed init = { sx, sy };
  g_array_append_val (stack, init);

  int min_x = sx;
  int max_x = sx;
  int min_y = sy;
  int max_y = sy;

  while (stack->len > 0) {
    FillSeed seed = g_array_index (stack, FillSeed, stack->len - 1);
    g_array_set_size (stack, stack->len - 1);

    int row_y = seed.y;
    if (visited[(gsize) row_y * (gsize) w + (gsize) seed.x])
      continue;

    uint32_t *row = (uint32_t *) (data + (gsize) row_y * (gsize) stride);
    guint8 *vis_row = visited + (gsize) row_y * (gsize) w;
    if (!pixel_matches (row[seed.x], target_px, FILL_TOLERANCE))
      continue;

    int left = seed.x;
    while (left > 0 && !vis_row[left - 1] &&
           pixel_matches (row[left - 1], target_px, FILL_TOLERANCE)) {
      left--;
    }

    int right = seed.x;
    while (right + 1 < w && !vis_row[right + 1] &&
           pixel_matches (row[right + 1], target_px, FILL_TOLERANCE)) {
      right++;
    }

    if (left < min_x) min_x = left;
    if (right > max_x) max_x = right;
    if (row_y < min_y) min_y = row_y;
    if (row_y > max_y) max_y = row_y;

    gboolean span_above = FALSE;
    gboolean span_below = FALSE;

    for (int col = left; col <= right; col++) {
      vis_row[col] = 1;
      if (make_transparent)
        row[col] = 0x00000000U;
      else
        row[col] = blend_over_argb32 (row[col], fill_px, fill_a);

      if (row_y > 0) {
        uint32_t *above_row = (uint32_t *) (data + (gsize) (row_y - 1) * (gsize) stride);
        guint8 *above_vis = visited + (gsize) (row_y - 1) * (gsize) w;
        gboolean match = !above_vis[col] && pixel_matches (above_row[col], target_px, FILL_TOLERANCE);
        if (!span_above && match) {
          FillSeed s = { col, row_y - 1 };
          g_array_append_val (stack, s);
          span_above = TRUE;
        } else if (span_above && !match) {
          span_above = FALSE;
        }
      }

      if (row_y + 1 < h) {
        uint32_t *below_row = (uint32_t *) (data + (gsize) (row_y + 1) * (gsize) stride);
        guint8 *below_vis = visited + (gsize) (row_y + 1) * (gsize) w;
        gboolean match = !below_vis[col] && pixel_matches (below_row[col], target_px, FILL_TOLERANCE);
        if (!span_below && match) {
          FillSeed s = { col, row_y + 1 };
          g_array_append_val (stack, s);
          span_below = TRUE;
        } else if (span_below && !match) {
          span_below = FALSE;
        }
      }
    }
  }

  /* Pass 2: smooth/fill 1px anti-aliased fringe adjacent to the filled mask */
  int y0 = MAX (0, min_y - 1);
  int y1 = MIN (h - 1, max_y + 1);
  int x0 = MAX (0, min_x - 1);
  int x1 = MIN (w - 1, max_x + 1);
  uint32_t target_a = (target_px >> 24) & 0xFFU;

  for (int row_y = y0; row_y <= y1; row_y++) {
    uint32_t *row = (uint32_t *) (data + (gsize) row_y * (gsize) stride);
    guint8 *vis_row = visited + (gsize) row_y * (gsize) w;

    for (int col = x0; col <= x1; col++) {
      if (vis_row[col])
        continue;

      gboolean has_filled_neighbor = FALSE;
      for (int dy = -1; dy <= 1 && !has_filled_neighbor; dy++) {
        int ny = row_y + dy;
        if (ny < 0 || ny >= h)
          continue;
        guint8 *nvis = visited + (gsize) ny * (gsize) w;
        for (int dx = -1; dx <= 1; dx++) {
          int nx = col + dx;
          if (nx >= 0 && nx < w && nvis[nx]) {
            has_filled_neighbor = TRUE;
            break;
          }
        }
      }
      if (!has_filled_neighbor)
        continue;

      uint32_t px = row[col];
      uint32_t px_a = (px >> 24) & 0xFFU;

      if (make_transparent) {
        if (px_a == 0)
          continue;
        if (pixel_is_blend_between (px, target_px, 0x00000000U)) {
          row[col] = 0x00000000U;
        } else {
          int diff = pixel_max_diff (px, target_px);
          if (diff <= FILL_FRINGE_MAX_DIST) {
            double clear_w = (double) (FILL_FRINGE_MAX_DIST + 10 - diff) /
                             (double) (FILL_FRINGE_MAX_DIST + 10);
            clear_w = CLAMP (clear_w, 0.0, 1.0);
            double keep = 1.0 - clear_w;
            uint32_t out_a = (uint32_t) lround ((double) px_a * keep);
            if (out_a == 0) {
              row[col] = 0x00000000U;
            } else {
              double tr = (double) ((target_px >> 16) & 0xFFU);
              double tg = (double) ((target_px >> 8) & 0xFFU);
              double tb = (double) (target_px & 0xFFU);
              double pr = (double) ((px >> 16) & 0xFFU);
              double pg = (double) ((px >> 8) & 0xFFU);
              double pb = (double) (px & 0xFFU);
              int r = CLAMP ((int) lround (pr - clear_w * tr), 0, (int) out_a);
              int g = CLAMP ((int) lround (pg - clear_w * tg), 0, (int) out_a);
              int b = CLAMP ((int) lround (pb - clear_w * tb), 0, (int) out_a);
              row[col] = (out_a << 24) |
                         ((uint32_t) r << 16) |
                         ((uint32_t) g << 8) |
                         (uint32_t) b;
            }
          }
        }
      } else {
        if (target_a > 0 && px_a == 0)
          continue;
        if (pixel_is_blend_between (px, target_px, fill_px)) {
          row[col] = blend_over_argb32 (px, fill_px, fill_a);
        } else {
          int diff = pixel_max_diff (px, target_px);
          if (diff <= FILL_FRINGE_MAX_DIST) {
            double weight = (double) (FILL_FRINGE_MAX_DIST + 10 - diff) /
                            (double) (FILL_FRINGE_MAX_DIST + 10);
            uint8_t blend_a = (uint8_t) lround (CLAMP (weight, 0.0, 1.0) * (double) fill_a);
            if (blend_a > 0) {
              uint8_t pm_r = (uint8_t) lround (CLAMP (settings->color.red, 0.0f, 1.0f) * blend_a);
              uint8_t pm_g = (uint8_t) lround (CLAMP (settings->color.green, 0.0f, 1.0f) * blend_a);
              uint8_t pm_b = (uint8_t) lround (CLAMP (settings->color.blue, 0.0f, 1.0f) * blend_a);
              uint32_t pm_px = ((uint32_t) blend_a << 24) |
                               ((uint32_t) pm_r << 16) |
                               ((uint32_t) pm_g << 8) |
                               (uint32_t) pm_b;
              row[col] = blend_over_argb32 (px, pm_px, blend_a);
            }
          }
        }
      }
    }
  }

  g_array_free (stack, TRUE);
  g_free (visited);
  cairo_surface_mark_dirty (surface);
}

static void
fill_begin_stroke (const PaintTool *tool G_GNUC_UNUSED,
                   cairo_t *cr,
                   double x,
                   double y,
                   PaintToolSettings *settings)
{
  flood_fill_surface (cr, x, y, settings, FALSE);
}

static void
transparent_fill_begin_stroke (const PaintTool *tool G_GNUC_UNUSED,
                               cairo_t *cr,
                               double x,
                               double y,
                               PaintToolSettings *settings)
{
  flood_fill_surface (cr, x, y, settings, TRUE);
}

static void
fill_noop (const PaintTool *tool G_GNUC_UNUSED,
           cairo_t *cr G_GNUC_UNUSED,
           double x G_GNUC_UNUSED,
           double y G_GNUC_UNUSED,
           PaintToolSettings *settings G_GNUC_UNUSED)
{
}

static void
draw_straight_line (cairo_t *cr,
                    double x0,
                    double y0,
                    double x1,
                    double y1,
                    const PaintToolSettings *settings)
{
  configure_brush_context (cr, settings);
  if (hypot (x1 - x0, y1 - y0) < 1e-6) {
    cairo_arc (cr, x0, y0, MAX (0.5, settings->size / 2.0), 0.0, 2.0 * G_PI);
    cairo_fill (cr);
    return;
  }

  cairo_set_line_width (cr, MAX (1.0, settings->size));
  cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
  cairo_move_to (cr, x0, y0);
  cairo_line_to (cr, x1, y1);
  cairo_stroke (cr);
}

static void
line_begin_stroke (const PaintTool *tool G_GNUC_UNUSED,
                   cairo_t *cr G_GNUC_UNUSED,
                   double x,
                   double y,
                   PaintToolSettings *settings)
{
  PaintPoint pt = { x, y };
  g_array_set_size (settings->history_points, 0);
  g_array_append_val (settings->history_points, pt);
}

static void
line_end_stroke (const PaintTool *tool,
                 cairo_t *cr,
                 double x,
                 double y,
                 PaintToolSettings *settings)
{
  GArray *pts = settings->history_points;
  double x0 = x;
  double y0 = y;
  if (pts->len > 0) {
    PaintPoint start = g_array_index (pts, PaintPoint, 0);
    x0 = start.x;
    y0 = start.y;
  }
  tool->draw_preview (tool, cr, x0, y0, x, y, settings);
  g_array_set_size (pts, 0);
}

static void
line_draw_preview (const PaintTool *tool G_GNUC_UNUSED,
                   cairo_t *cr,
                   double x0,
                   double y0,
                   double x1,
                   double y1,
                   const PaintToolSettings *settings)
{
  draw_straight_line (cr, x0, y0, x1, y1, settings);
}

static void
line_snap_point (double x0, double y0, double *x1, double *y1)
{
  double dx = *x1 - x0;
  double dy = *y1 - y0;
  if (hypot (dx, dy) < 1e-6)
    return;

  static const double dirs[8][2] = {
    {  1.0,            0.0           },
    {  1.0 / G_SQRT2,  1.0 / G_SQRT2 },
    {  0.0,            1.0           },
    { -1.0 / G_SQRT2,  1.0 / G_SQRT2 },
    { -1.0,            0.0           },
    { -1.0 / G_SQRT2, -1.0 / G_SQRT2 },
    {  0.0,           -1.0           },
    {  1.0 / G_SQRT2, -1.0 / G_SQRT2 },
  };
  double angle = atan2 (dy, dx);
  int octant = ((int) lround (angle / (G_PI / 4.0)) % 8 + 8) % 8;
  double ux = dirs[octant][0];
  double uy = dirs[octant][1];
  double proj = dx * ux + dy * uy;
  *x1 = x0 + proj * ux;
  *y1 = y0 + proj * uy;
}

static void
shape_snap_square (double x0, double y0, double *x1, double *y1)
{
  double dx = *x1 - x0;
  double dy = *y1 - y0;
  double side = MAX (fabs (dx), fabs (dy));
  *x1 = x0 + (dx < 0.0 ? -side : side);
  *y1 = y0 + (dy < 0.0 ? -side : side);
}

static void
build_polygon_path (cairo_t *cr,
                    double cx,
                    double cy,
                    double rx,
                    double ry,
                    int sides,
                    double inner_ratio)
{
  int total = (inner_ratio > 0.0) ? sides * 2 : sides;
  double step = (2.0 * G_PI) / (double) total;
  for (int i = 0; i < total; i++) {
    double a = -G_PI_2 + (double) i * step;
    double frac = (inner_ratio > 0.0 && (i % 2 == 1)) ? inner_ratio : 1.0;
    double px = cx + rx * frac * cos (a);
    double py = cy + ry * frac * sin (a);
    if (i == 0)
      cairo_move_to (cr, px, py);
    else
      cairo_line_to (cr, px, py);
  }
  cairo_close_path (cr);
}

static void
build_rounded_rect_path (cairo_t *cr, double x, double y, double w, double h, double r)
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
finish_shape_path (cairo_t *cr,
                   const PaintToolSettings *settings,
                   cairo_line_join_t join)
{
  configure_brush_context (cr, settings);
  if (settings->shape_filled) {
    cairo_fill (cr);
  } else {
    cairo_set_line_width (cr, MAX (1.0, settings->size));
    cairo_set_line_join (cr, join);
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
    cairo_stroke (cr);
  }
}

static void
arrow_compute_head (double len,
                    const PaintToolSettings *settings,
                    double *out_head_len,
                    double *out_wing_w)
{
  double head_len = CLAMP (len * 0.3, MAX (10.0, settings->size * 2.5), 48.0);
  if (head_len > len * 0.6)
    head_len = len * 0.6;
  *out_head_len = head_len;
  *out_wing_w = head_len * 0.6;
}

static void
build_block_arrow_path (cairo_t *cr,
                        double x0,
                        double y0,
                        double x1,
                        double y1,
                        double ux,
                        double uy,
                        double head_len,
                        double wing_w,
                        double shaft_w)
{
  double nx = -uy;
  double ny = ux;
  double hx = x1 - ux * head_len;
  double hy = y1 - uy * head_len;

  cairo_move_to (cr, x0 + nx * shaft_w, y0 + ny * shaft_w);
  cairo_line_to (cr, hx + nx * shaft_w, hy + ny * shaft_w);
  cairo_line_to (cr, hx + nx * wing_w, hy + ny * wing_w);
  cairo_line_to (cr, x1, y1);
  cairo_line_to (cr, hx - nx * wing_w, hy - ny * wing_w);
  cairo_line_to (cr, hx - nx * shaft_w, hy - ny * shaft_w);
  cairo_line_to (cr, x0 - nx * shaft_w, y0 - ny * shaft_w);
  cairo_close_path (cr);
}

static void
shape_draw_arrow (cairo_t *cr,
                  double x0,
                  double y0,
                  double x1,
                  double y1,
                  const PaintToolSettings *settings)
{
  double dx = x1 - x0;
  double dy = y1 - y0;
  double len = hypot (dx, dy);
  if (len < 1e-6) {
    draw_straight_line (cr, x0, y0, x1, y1, settings);
    return;
  }

  double ux = dx / len;
  double uy = dy / len;
  double nx = -uy;
  double ny = ux;

  double head_len, wing_w;
  arrow_compute_head (len, settings, &head_len, &wing_w);

  if (settings->shape_filled) {
    double shaft_w = MAX (2.0, MIN (wing_w * 0.42, settings->size * 0.75));
    build_block_arrow_path (cr, x0, y0, x1, y1, ux, uy, head_len, wing_w, shaft_w);
    finish_shape_path (cr, settings, CAIRO_LINE_JOIN_ROUND);
  } else {
    double hx = x1 - ux * head_len;
    double hy = y1 - uy * head_len;
    configure_brush_context (cr, settings);
    cairo_set_line_width (cr, MAX (1.0, settings->size));
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
    cairo_move_to (cr, x0, y0);
    cairo_line_to (cr, x1, y1);
    cairo_stroke (cr);

    cairo_move_to (cr, hx + nx * wing_w, hy + ny * wing_w);
    cairo_line_to (cr, x1, y1);
    cairo_line_to (cr, hx - nx * wing_w, hy - ny * wing_w);
    cairo_stroke (cr);
  }
}

static void
block_arrow_compute_dims (double len,
                          double *out_head_len,
                          double *out_wing_w,
                          double *out_shaft_w)
{
  *out_head_len = len * 0.38;
  *out_wing_w = len * 0.26;
  *out_shaft_w = len * 0.10;
}

static void
shape_draw_block_arrow (cairo_t *cr,
                        double x0,
                        double y0,
                        double x1,
                        double y1,
                        const PaintToolSettings *settings)
{
  double dx = x1 - x0;
  double dy = y1 - y0;
  double len = hypot (dx, dy);
  if (len < 1e-6) {
    draw_straight_line (cr, x0, y0, x1, y1, settings);
    return;
  }

  double head_len, wing_w, shaft_w;
  block_arrow_compute_dims (len, &head_len, &wing_w, &shaft_w);
  build_block_arrow_path (cr, x0, y0, x1, y1, dx / len, dy / len, head_len, wing_w, shaft_w);
  finish_shape_path (cr, settings, CAIRO_LINE_JOIN_ROUND);
}

static void
shape_draw_preview (const PaintTool *tool,
                    cairo_t *cr,
                    double x0,
                    double y0,
                    double x1,
                    double y1,
                    const PaintToolSettings *settings)
{
  if (g_strcmp0 (tool->id, "shape-arrow") == 0) {
    shape_draw_arrow (cr, x0, y0, x1, y1, settings);
    return;
  }
  if (g_strcmp0 (tool->id, "shape-block-arrow") == 0) {
    shape_draw_block_arrow (cr, x0, y0, x1, y1, settings);
    return;
  }

  double bx = MIN (x0, x1);
  double by = MIN (y0, y1);
  double bw = fabs (x1 - x0);
  double bh = fabs (y1 - y0);
  if (bw < 1e-3 && bh < 1e-3) {
    draw_straight_line (cr, x0, y0, x1, y1, settings);
    return;
  }
  if (bw < 1e-3 || bh < 1e-3) {
    draw_straight_line (cr, bx, by, bx + bw, by + bh, settings);
    return;
  }

  double cx = bx + bw * 0.5;
  double cy = by + bh * 0.5;
  double rx = bw * 0.5;
  double ry = bh * 0.5;
  double sry = (y1 < y0) ? -ry : ry;

  if (g_strcmp0 (tool->id, "shape-rectangle") == 0) {
    cairo_rectangle (cr, bx, by, bw, bh);
    finish_shape_path (cr, settings, CAIRO_LINE_JOIN_MITER);
  } else if (g_strcmp0 (tool->id, "shape-rounded-rect") == 0) {
    double r = MIN (MIN (bw, bh) * 0.2, MAX (8.0, settings->size * 1.5));
    build_rounded_rect_path (cr, bx, by, bw, bh, r);
    finish_shape_path (cr, settings, CAIRO_LINE_JOIN_ROUND);
  } else if (g_strcmp0 (tool->id, "shape-ellipse") == 0) {
    cairo_save (cr);
    cairo_translate (cr, cx, cy);
    cairo_scale (cr, rx, ry);
    cairo_arc (cr, 0.0, 0.0, 1.0, 0.0, 2.0 * G_PI);
    cairo_restore (cr);
    finish_shape_path (cr, settings, CAIRO_LINE_JOIN_ROUND);
  } else if (g_strcmp0 (tool->id, "shape-triangle") == 0) {
    cairo_move_to (cr, cx, y0);
    cairo_line_to (cr, bx + bw, y1);
    cairo_line_to (cr, bx, y1);
    cairo_close_path (cr);
    finish_shape_path (cr, settings, CAIRO_LINE_JOIN_ROUND);
  } else if (g_strcmp0 (tool->id, "shape-diamond") == 0) {
    cairo_move_to (cr, cx, by);
    cairo_line_to (cr, bx + bw, cy);
    cairo_line_to (cr, cx, by + bh);
    cairo_line_to (cr, bx, cy);
    cairo_close_path (cr);
    finish_shape_path (cr, settings, CAIRO_LINE_JOIN_ROUND);
  } else if (g_strcmp0 (tool->id, "shape-hexagon") == 0) {
    build_polygon_path (cr, cx, cy, rx, ry, 6, 0.0);
    finish_shape_path (cr, settings, CAIRO_LINE_JOIN_ROUND);
  } else if (g_strcmp0 (tool->id, "shape-star") == 0) {
    build_polygon_path (cr, cx, cy, rx, sry, 5, 0.38196601125);
    finish_shape_path (cr, settings, CAIRO_LINE_JOIN_ROUND);
  }
}

static void
polygon_get_bounds (const PaintTool *tool G_GNUC_UNUSED,
                    double x0,
                    double y0,
                    double x1,
                    double y1,
                    const PaintToolSettings *settings,
                    double *out_x0,
                    double *out_y0,
                    double *out_x1,
                    double *out_y1)
{
  double pad = settings->shape_filled ? 2.0 : MAX (2.0, settings->size * 2.0);
  *out_x0 = MIN (x0, x1) - pad;
  *out_y0 = MIN (y0, y1) - pad;
  *out_x1 = MAX (x0, x1) + pad;
  *out_y1 = MAX (y0, y1) + pad;
}

static void
arrow_get_bounds (const PaintTool *tool G_GNUC_UNUSED,
                  double x0,
                  double y0,
                  double x1,
                  double y1,
                  const PaintToolSettings *settings,
                  double *out_x0,
                  double *out_y0,
                  double *out_x1,
                  double *out_y1)
{
  double len = hypot (x1 - x0, y1 - y0);
  double head_len, wing_w;
  arrow_compute_head (len, settings, &head_len, &wing_w);
  double pad = wing_w + MAX (2.0, settings->size);
  *out_x0 = MIN (x0, x1) - pad;
  *out_y0 = MIN (y0, y1) - pad;
  *out_x1 = MAX (x0, x1) + pad;
  *out_y1 = MAX (y0, y1) + pad;
}

static void
block_arrow_get_bounds (const PaintTool *tool G_GNUC_UNUSED,
                        double x0,
                        double y0,
                        double x1,
                        double y1,
                        const PaintToolSettings *settings,
                        double *out_x0,
                        double *out_y0,
                        double *out_x1,
                        double *out_y1)
{
  double len = hypot (x1 - x0, y1 - y0);
  double head_len, wing_w, shaft_w;
  block_arrow_compute_dims (len, &head_len, &wing_w, &shaft_w);
  double pad = wing_w + MAX (2.0, settings->size);
  *out_x0 = MIN (x0, x1) - pad;
  *out_y0 = MIN (y0, y1) - pad;
  *out_x1 = MAX (x0, x1) + pad;
  *out_y1 = MAX (y0, y1) + pad;
}

static const PaintTool BUILTIN_TOOLS[] = {
  {
    .id = "select",
    .name = "Select",
    .icon_name = "tool-select-symbolic",
    .tooltip = "Select (M) · Drag to select, drag inside to move",
    .uses_brush_size = FALSE,
    .begin_stroke = fill_noop,
    .continue_stroke = fill_noop,
    .end_stroke = fill_noop,
  },
  {
    .id = "brush",
    .name = "Brush",
    .icon_name = "document-edit-symbolic",
    .tooltip = "Brush (B)",
    .uses_brush_size = TRUE,
    .begin_stroke = freehand_begin_stroke,
    .continue_stroke = freehand_continue_stroke,
    .end_stroke = freehand_end_stroke,
  },
  {
    .id = "line",
    .name = "Line",
    .icon_name = "tool-line-symbolic",
    .tooltip = "Line (L) · Hold Shift to snap angle",
    .uses_brush_size = TRUE,
    .begin_stroke = line_begin_stroke,
    .continue_stroke = fill_noop,
    .end_stroke = line_end_stroke,
    .draw_preview = line_draw_preview,
    .snap_point = line_snap_point,
  },
  {
    .id = "eraser",
    .name = "Eraser",
    .icon_name = "tool-eraser-symbolic",
    .tooltip = "Eraser (E)",
    .uses_brush_size = TRUE,
    .begin_stroke = freehand_begin_stroke,
    .continue_stroke = freehand_continue_stroke,
    .end_stroke = freehand_end_stroke,
  },
  {
    .id = "fill",
    .name = "Fill",
    .icon_name = "tool-fill-symbolic",
    .tooltip = "Fill (F)",
    .uses_brush_size = FALSE,
    .begin_stroke = fill_begin_stroke,
    .continue_stroke = fill_noop,
    .end_stroke = fill_noop,
  },
  {
    .id = "text",
    .name = "Text",
    .icon_name = "tool-text-symbolic",
    .tooltip = "Text (X) · Click canvas to add text",
    .uses_brush_size = FALSE,
    .begin_stroke = fill_noop,
    .continue_stroke = fill_noop,
    .end_stroke = fill_noop,
  },
  {
    .id = "eyedropper",
    .name = "Color Picker",
    .icon_name = "tool-eyedropper-symbolic",
    .tooltip = "Color Picker (I) · Or Alt+Click anytime",
    .uses_brush_size = FALSE,
    .begin_stroke = fill_noop,
    .continue_stroke = fill_noop,
    .end_stroke = fill_noop,
  },
  {
    .id = "transparent",
    .name = "Brush",
    .icon_name = "tool-transparent-symbolic",
    .tooltip = "Transparent Brush (T) · Paint areas transparent",
    .uses_brush_size = TRUE,
    .is_transparency = TRUE,
    .begin_stroke = freehand_begin_stroke,
    .continue_stroke = freehand_continue_stroke,
    .end_stroke = freehand_end_stroke,
  },
  {
    .id = "transparent-fill",
    .name = "Fill",
    .icon_name = "tool-transparent-fill-symbolic",
    .tooltip = "Transparent Fill · Click an area to make it transparent",
    .uses_brush_size = FALSE,
    .is_transparency = TRUE,
    .begin_stroke = transparent_fill_begin_stroke,
    .continue_stroke = fill_noop,
    .end_stroke = fill_noop,
  },
  {
    .id = "shape-rectangle",
    .name = "Rectangle",
    .icon_name = "tool-shape-rect-symbolic",
    .filled_icon_name = "tool-shape-rect-filled-symbolic",
    .tooltip = "Rectangle · Hold Shift for square",
    .uses_brush_size = TRUE,
    .is_shape = TRUE,
    .begin_stroke = line_begin_stroke,
    .continue_stroke = fill_noop,
    .end_stroke = line_end_stroke,
    .draw_preview = shape_draw_preview,
    .snap_point = shape_snap_square,
    .get_bounds = polygon_get_bounds,
  },
  {
    .id = "shape-rounded-rect",
    .name = "Rounded",
    .icon_name = "tool-shape-rounded-rect-symbolic",
    .filled_icon_name = "tool-shape-rounded-rect-filled-symbolic",
    .tooltip = "Rounded Rectangle · Hold Shift for square",
    .uses_brush_size = TRUE,
    .is_shape = TRUE,
    .begin_stroke = line_begin_stroke,
    .continue_stroke = fill_noop,
    .end_stroke = line_end_stroke,
    .draw_preview = shape_draw_preview,
    .snap_point = shape_snap_square,
    .get_bounds = polygon_get_bounds,
  },
  {
    .id = "shape-ellipse",
    .name = "Circle",
    .icon_name = "tool-shape-ellipse-symbolic",
    .filled_icon_name = "tool-shape-ellipse-filled-symbolic",
    .tooltip = "Ellipse / Circle · Hold Shift for circle",
    .uses_brush_size = TRUE,
    .is_shape = TRUE,
    .begin_stroke = line_begin_stroke,
    .continue_stroke = fill_noop,
    .end_stroke = line_end_stroke,
    .draw_preview = shape_draw_preview,
    .snap_point = shape_snap_square,
    .get_bounds = polygon_get_bounds,
  },
  {
    .id = "shape-triangle",
    .name = "Triangle",
    .icon_name = "tool-shape-triangle-symbolic",
    .filled_icon_name = "tool-shape-triangle-filled-symbolic",
    .tooltip = "Triangle · Hold Shift for 1:1 proportions",
    .uses_brush_size = TRUE,
    .is_shape = TRUE,
    .begin_stroke = line_begin_stroke,
    .continue_stroke = fill_noop,
    .end_stroke = line_end_stroke,
    .draw_preview = shape_draw_preview,
    .snap_point = shape_snap_square,
    .get_bounds = polygon_get_bounds,
  },
  {
    .id = "shape-diamond",
    .name = "Diamond",
    .icon_name = "tool-shape-diamond-symbolic",
    .filled_icon_name = "tool-shape-diamond-filled-symbolic",
    .tooltip = "Diamond · Hold Shift for 1:1 proportions",
    .uses_brush_size = TRUE,
    .is_shape = TRUE,
    .begin_stroke = line_begin_stroke,
    .continue_stroke = fill_noop,
    .end_stroke = line_end_stroke,
    .draw_preview = shape_draw_preview,
    .snap_point = shape_snap_square,
    .get_bounds = polygon_get_bounds,
  },
  {
    .id = "shape-hexagon",
    .name = "Hexagon",
    .icon_name = "tool-shape-hexagon-symbolic",
    .filled_icon_name = "tool-shape-hexagon-filled-symbolic",
    .tooltip = "Hexagon · Hold Shift for regular hexagon",
    .uses_brush_size = TRUE,
    .is_shape = TRUE,
    .begin_stroke = line_begin_stroke,
    .continue_stroke = fill_noop,
    .end_stroke = line_end_stroke,
    .draw_preview = shape_draw_preview,
    .snap_point = shape_snap_square,
    .get_bounds = polygon_get_bounds,
  },
  {
    .id = "shape-star",
    .name = "Star",
    .icon_name = "tool-shape-star-symbolic",
    .filled_icon_name = "tool-shape-star-filled-symbolic",
    .tooltip = "Star · Hold Shift for symmetric star",
    .uses_brush_size = TRUE,
    .is_shape = TRUE,
    .begin_stroke = line_begin_stroke,
    .continue_stroke = fill_noop,
    .end_stroke = line_end_stroke,
    .draw_preview = shape_draw_preview,
    .snap_point = shape_snap_square,
    .get_bounds = polygon_get_bounds,
  },
  {
    .id = "shape-arrow",
    .name = "Arrow",
    .icon_name = "tool-shape-arrow-symbolic",
    .filled_icon_name = "tool-shape-arrow-filled-symbolic",
    .tooltip = "Arrow · Hold Shift to snap angle",
    .uses_brush_size = TRUE,
    .is_shape = TRUE,
    .begin_stroke = line_begin_stroke,
    .continue_stroke = fill_noop,
    .end_stroke = line_end_stroke,
    .draw_preview = shape_draw_preview,
    .snap_point = line_snap_point,
    .get_bounds = arrow_get_bounds,
  },
  {
    .id = "shape-block-arrow",
    .name = "Block Arrow",
    .icon_name = "tool-shape-block-arrow-symbolic",
    .filled_icon_name = "tool-shape-block-arrow-filled-symbolic",
    .tooltip = "Block Arrow · Hold Shift to snap angle",
    .uses_brush_size = TRUE,
    .is_shape = TRUE,
    .begin_stroke = line_begin_stroke,
    .continue_stroke = fill_noop,
    .end_stroke = line_end_stroke,
    .draw_preview = shape_draw_preview,
    .snap_point = line_snap_point,
    .get_bounds = block_arrow_get_bounds,
  },
};

const PaintTool *
paint_tools_get_all (gsize *n_tools)
{
  if (n_tools)
    *n_tools = G_N_ELEMENTS (BUILTIN_TOOLS);
  return BUILTIN_TOOLS;
}

const PaintTool *
paint_tools_find (const char *tool_id)
{
  const PaintTool *brush_fallback = &BUILTIN_TOOLS[0];
  for (gsize i = 0; i < G_N_ELEMENTS (BUILTIN_TOOLS); i++) {
    if (g_strcmp0 (BUILTIN_TOOLS[i].id, "brush") == 0)
      brush_fallback = &BUILTIN_TOOLS[i];
    if (g_strcmp0 (BUILTIN_TOOLS[i].id, tool_id) == 0)
      return &BUILTIN_TOOLS[i];
  }
  return brush_fallback;
}

const char *
paint_tool_get_icon_name (const PaintTool *tool, const PaintToolSettings *settings)
{
  if (!tool)
    return NULL;
  if (tool->is_shape && settings && settings->shape_filled && tool->filled_icon_name)
    return tool->filled_icon_name;
  return tool->icon_name;
}
