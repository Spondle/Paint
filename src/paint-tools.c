#include "paint-tools.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "paint-i18n.h"

#define FILL_FRINGE_MAX_DIST 185
#define FILL_COLLINEAR_ERR 18

PaintToolSettings *
paint_tool_settings_new (void)
{
  PaintToolSettings *settings = g_new0 (PaintToolSettings, 1);
  settings->color = (GdkRGBA) { 0.11f, 0.44f, 0.96f, 1.0f };
  settings->secondary_color = (GdkRGBA) { 1.0f, 1.0f, 1.0f, 1.0f };
  settings->size = 6.0;
  settings->background_color = (GdkRGBA) { 1.0f, 1.0f, 1.0f, 1.0f };
  settings->active_tool_id = "brush";
  settings->brush_style = PAINT_BRUSH_SMOOTH;
  settings->fill_tolerance = 32;
  settings->stroke_pressure = 1.0;
  settings->pressure_size = TRUE;
  settings->pressure_opacity = FALSE;
  settings->text_font_family = g_strdup ("Sans");
  settings->text_font_size = 28.0;
  settings->text_align = PANGO_ALIGN_LEFT;
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

void
paint_tool_settings_swap_colors (PaintToolSettings *settings)
{
  if (!settings)
    return;
  GdkRGBA tmp = settings->color;
  settings->color = settings->secondary_color;
  settings->secondary_color = tmp;
}

void
paint_tool_settings_reset_colors (PaintToolSettings *settings)
{
  if (!settings)
    return;
  settings->color = (GdkRGBA) { 0.0f, 0.0f, 0.0f, 1.0f };
  settings->secondary_color = (GdkRGBA) { 1.0f, 1.0f, 1.0f, 1.0f };
}

void
paint_tool_settings_add_custom_color (PaintToolSettings *settings, const GdkRGBA *color)
{
  if (!settings || !color)
    return;
  for (int i = 0; i < settings->n_custom_colors; i++) {
    if (rgba_nearly_equal (&settings->custom_colors[i], color)) {
      settings->custom_colors[i] = *color;
      return;
    }
  }
  if (settings->n_custom_colors < PAINT_MAX_CUSTOM_COLORS) {
    settings->custom_colors[settings->n_custom_colors++] = *color;
  } else {
    for (int i = 1; i < PAINT_MAX_CUSTOM_COLORS; i++)
      settings->custom_colors[i - 1] = settings->custom_colors[i];
    settings->custom_colors[PAINT_MAX_CUSTOM_COLORS - 1] = *color;
  }
}

void
paint_tool_settings_remove_custom_color (PaintToolSettings *settings, int index)
{
  if (!settings || index < 0 || index >= settings->n_custom_colors)
    return;
  for (int i = index; i + 1 < settings->n_custom_colors; i++)
    settings->custom_colors[i] = settings->custom_colors[i + 1];
  settings->n_custom_colors--;
}

static inline double
effective_stroke_size (const PaintToolSettings *settings)
{
  double p = settings->stroke_pressure > 0.0 ? CLAMP (settings->stroke_pressure, 0.05, 1.0) : 1.0;
  if (!settings->pressure_size || p >= 0.999)
    return settings->size;
  return MAX (1.0, settings->size * (0.15 + 0.85 * p));
}

static inline float
effective_stroke_alpha (const PaintToolSettings *settings, float base_a)
{
  double p = settings->stroke_pressure > 0.0 ? CLAMP (settings->stroke_pressure, 0.05, 1.0) : 1.0;
  if (!settings->pressure_opacity || p >= 0.999)
    return base_a;
  return CLAMP (base_a * (float) p, 0.0f, 1.0f);
}

static void
configure_brush_context (cairo_t *cr, const PaintToolSettings *settings)
{
  float alpha = effective_stroke_alpha (settings, settings->color.alpha);
  cairo_set_source_rgba (cr,
                         settings->color.red,
                         settings->color.green,
                         settings->color.blue,
                         alpha);
  cairo_set_operator (cr, settings->target_alpha_lock ? CAIRO_OPERATOR_ATOP
                                                      : CAIRO_OPERATOR_OVER);
}

static void
configure_eraser_context (cairo_t *cr, const PaintToolSettings *settings)
{
  if (settings->background_color.alpha <= 0.0f) {
    if (settings->target_alpha_lock) {
      cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.0);
      cairo_set_operator (cr, CAIRO_OPERATOR_OVER);
    } else {
      cairo_set_operator (cr, CAIRO_OPERATOR_CLEAR);
    }
    return;
  }
  cairo_set_source_rgba (cr,
                         settings->background_color.red,
                         settings->background_color.green,
                         settings->background_color.blue,
                         settings->background_color.alpha);
  cairo_set_operator (cr, settings->target_alpha_lock ? CAIRO_OPERATOR_ATOP
                                                      : CAIRO_OPERATOR_SOURCE);
}

static void
configure_transparent_context (cairo_t *cr, const PaintToolSettings *settings)
{
  if (settings->target_alpha_lock) {
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.0);
    cairo_set_operator (cr, CAIRO_OPERATOR_OVER);
  } else {
    cairo_set_operator (cr, CAIRO_OPERATOR_CLEAR);
  }
}

static void
configure_for_tool (const PaintTool *tool, cairo_t *cr, const PaintToolSettings *settings)
{
  if (g_strcmp0 (tool->id, "transparent") == 0)
    configure_transparent_context (cr, settings);
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

  double eff_sz = effective_stroke_size (settings);
  for (int i = 1; i <= steps; i++) {
    double t = (double) i / (double) steps;
    int sx = (int) lround ((double) ix0 + (double) (ix1 - ix0) * t);
    int sy = (int) lround ((double) iy0 + (double) (iy1 - iy0) * t);
    draw_pixel_stamp (cr, sx, sy, eff_sz);
  }
}

static void
draw_soft_stamp (const PaintTool *tool,
                 cairo_t *cr,
                 double x,
                 double y,
                 const PaintToolSettings *settings)
{
  double eff_sz = effective_stroke_size (settings);
  double r = MAX (1.5, eff_sz * 0.5);
  cairo_pattern_t *pat = cairo_pattern_create_radial (x, y, 0.0, x, y, r);

  gboolean erase_alpha = (g_strcmp0 (tool->id, "transparent") == 0) ||
                         (g_strcmp0 (tool->id, "eraser") == 0 &&
                          settings->background_color.alpha <= 0.0f);
  if (erase_alpha) {
    if (settings->target_alpha_lock) {
      cairo_pattern_destroy (pat);
      return;
    }
    double pa = effective_stroke_alpha (settings, 1.0f);
    cairo_set_operator (cr, CAIRO_OPERATOR_DEST_OUT);
    cairo_pattern_add_color_stop_rgba (pat, 0.0, 0.0, 0.0, 0.0, 0.35 * pa);
    cairo_pattern_add_color_stop_rgba (pat, 0.5, 0.0, 0.0, 0.0, 0.14 * pa);
    cairo_pattern_add_color_stop_rgba (pat, 1.0, 0.0, 0.0, 0.0, 0.0);
  } else {
    GdkRGBA c = (g_strcmp0 (tool->id, "eraser") == 0)
                  ? settings->background_color
                  : settings->color;
    double a = CLAMP ((double) effective_stroke_alpha (settings, c.alpha), 0.0, 1.0);
    cairo_set_operator (cr, settings->target_alpha_lock ? CAIRO_OPERATOR_ATOP
                                                        : CAIRO_OPERATOR_OVER);
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
  double r = MAX (1.5, effective_stroke_size (settings) * 0.5);
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

  double eff_sz = effective_stroke_size (settings);
  if (settings->brush_style == PAINT_BRUSH_PIXEL) {
    configure_for_tool (tool, cr, settings);
    cairo_set_antialias (cr, CAIRO_ANTIALIAS_NONE);
    draw_pixel_stamp (cr, (int) floor (x), (int) floor (y), eff_sz);
    return;
  }
  if (settings->brush_style == PAINT_BRUSH_SOFT) {
    draw_soft_stamp (tool, cr, x, y, settings);
    return;
  }

  configure_for_tool (tool, cr, settings);
  cairo_arc (cr, x, y, MAX (0.5, eff_sz / 2.0), 0.0, 2.0 * G_PI);
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
  cairo_set_line_width (cr, MAX (1.0, effective_stroke_size (settings)));
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
    cairo_set_line_width (cr, MAX (1.0, effective_stroke_size (settings)));
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

static inline uint32_t
blend_atop_argb32 (uint32_t dst, uint32_t src_pm, uint8_t src_a)
{
  uint32_t dst_a = (dst >> 24) & 0xFFU;
  if (dst_a == 0 || src_a == 0)
    return dst;

  uint32_t inv_sa = 255U - (uint32_t) src_a;
  uint32_t src_r = (src_pm >> 16) & 0xFFU;
  uint32_t src_g = (src_pm >> 8) & 0xFFU;
  uint32_t src_b = src_pm & 0xFFU;
  uint32_t dst_r = (dst >> 16) & 0xFFU;
  uint32_t dst_g = (dst >> 8) & 0xFFU;
  uint32_t dst_b = dst & 0xFFU;

  uint32_t out_r = MIN (dst_a, (src_r * dst_a + dst_r * inv_sa + 127U) / 255U);
  uint32_t out_g = MIN (dst_a, (src_g * dst_a + dst_g * inv_sa + 127U) / 255U);
  uint32_t out_b = MIN (dst_a, (src_b * dst_a + dst_b * inv_sa + 127U) / 255U);
  return (dst_a << 24) | (out_r << 16) | (out_g << 8) | out_b;
}

static inline uint32_t
blend_pixel_for_lock (uint32_t dst, uint32_t src_pm, uint8_t src_a, gboolean alpha_lock)
{
  return alpha_lock ? blend_atop_argb32 (dst, src_pm, src_a)
                    : blend_over_argb32 (dst, src_pm, src_a);
}

static guint8 *
compute_flood_region_mask (cairo_surface_t *sample_surf,
                           int sx,
                           int sy,
                           int tol,
                           int *out_min_x,
                           int *out_max_x,
                           int *out_min_y,
                           int *out_max_y,
                           uint32_t *out_target_px)
{
  int w = cairo_image_surface_get_width (sample_surf);
  int h = cairo_image_surface_get_height (sample_surf);
  int stride = cairo_image_surface_get_stride (sample_surf);
  const unsigned char *data = cairo_image_surface_get_data (sample_surf);
  if (sx < 0 || sx >= w || sy < 0 || sy >= h || !data)
    return NULL;

  const uint32_t *start_row = (const uint32_t *) (data + (gsize) sy * (gsize) stride);
  uint32_t target_px = start_row[sx];
  if (out_target_px)
    *out_target_px = target_px;

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

    const uint32_t *row = (const uint32_t *) (data + (gsize) row_y * (gsize) stride);
    guint8 *vis_row = visited + (gsize) row_y * (gsize) w;
    if (!pixel_matches (row[seed.x], target_px, tol))
      continue;

    int left = seed.x;
    while (left > 0 && !vis_row[left - 1] &&
           pixel_matches (row[left - 1], target_px, tol)) {
      left--;
    }

    int right = seed.x;
    while (right + 1 < w && !vis_row[right + 1] &&
           pixel_matches (row[right + 1], target_px, tol)) {
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

      if (row_y > 0) {
        const uint32_t *above_row = (const uint32_t *) (data + (gsize) (row_y - 1) * (gsize) stride);
        guint8 *above_vis = visited + (gsize) (row_y - 1) * (gsize) w;
        gboolean match = !above_vis[col] && pixel_matches (above_row[col], target_px, tol);
        if (!span_above && match) {
          FillSeed s = { col, row_y - 1 };
          g_array_append_val (stack, s);
          span_above = TRUE;
        } else if (span_above && !match) {
          span_above = FALSE;
        }
      }

      if (row_y + 1 < h) {
        const uint32_t *below_row = (const uint32_t *) (data + (gsize) (row_y + 1) * (gsize) stride);
        guint8 *below_vis = visited + (gsize) (row_y + 1) * (gsize) w;
        gboolean match = !below_vis[col] && pixel_matches (below_row[col], target_px, tol);
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

  g_array_free (stack, TRUE);
  if (out_min_x) *out_min_x = min_x;
  if (out_max_x) *out_max_x = max_x;
  if (out_min_y) *out_min_y = min_y;
  if (out_max_y) *out_max_y = max_y;
  return visited;
}

cairo_surface_t *
paint_tools_build_wand_mask (cairo_surface_t *src,
                             int sx,
                             int sy,
                             int tolerance,
                             cairo_rectangle_int_t *out_rect)
{
  if (!src || cairo_surface_get_type (src) != CAIRO_SURFACE_TYPE_IMAGE)
    return NULL;
  cairo_surface_flush (src);
  int w = cairo_image_surface_get_width (src);
  int min_x = 0, max_x = 0, min_y = 0, max_y = 0;
  guint8 *visited = compute_flood_region_mask (src, sx, sy, CLAMP (tolerance, 0, 255),
                                               &min_x, &max_x, &min_y, &max_y, NULL);
  if (!visited)
    return NULL;

  int rw = max_x - min_x + 1;
  int rh = max_y - min_y + 1;
  cairo_surface_t *mask = cairo_image_surface_create (CAIRO_FORMAT_A8, rw, rh);
  unsigned char *mdata = cairo_image_surface_get_data (mask);
  int mstride = cairo_image_surface_get_stride (mask);
  for (int y = min_y; y <= max_y; y++) {
    unsigned char *mrow = mdata + (gsize) (y - min_y) * (gsize) mstride;
    const guint8 *vis_row = visited + (gsize) y * (gsize) w;
    for (int x = min_x; x <= max_x; x++)
      mrow[x - min_x] = vis_row[x] ? 255 : 0;
  }
  cairo_surface_mark_dirty (mask);
  g_free (visited);

  if (out_rect)
    *out_rect = (cairo_rectangle_int_t) { min_x, min_y, rw, rh };
  return mask;
}

static void
flood_fill_surface (cairo_t *cr,
                    double x,
                    double y,
                    const PaintToolSettings *settings,
                    gboolean make_transparent)
{
  if (make_transparent && settings->target_alpha_lock)
    return;
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

  cairo_surface_t *sample_surf = surface;
  if (settings->fill_sample_surface &&
      cairo_surface_get_type (settings->fill_sample_surface) == CAIRO_SURFACE_TYPE_IMAGE &&
      cairo_image_surface_get_width (settings->fill_sample_surface) == w &&
      cairo_image_surface_get_height (settings->fill_sample_surface) == h) {
    sample_surf = settings->fill_sample_surface;
    if (sample_surf != surface)
      cairo_surface_flush (sample_surf);
  }

  int sstride = cairo_image_surface_get_stride (sample_surf);
  const unsigned char *sdata = cairo_image_surface_get_data (sample_surf);
  const uint32_t *sample_start = (const uint32_t *) (sdata + (gsize) sy * (gsize) sstride);
  uint32_t start_px = sample_start[sx];
  if (make_transparent && sample_surf == surface && ((start_px >> 24) & 0xFFU) == 0)
    return;
  if (!make_transparent && sample_surf == surface && fill_a == 255 && start_px == fill_px)
    return;

  int tol = CLAMP (settings->fill_tolerance, 0, 255);
  int min_x = 0, max_x = 0, min_y = 0, max_y = 0;
  uint32_t target_px = start_px;
  guint8 *visited = compute_flood_region_mask (sample_surf, sx, sy, tol,
                                               &min_x, &max_x, &min_y, &max_y, &target_px);
  if (!visited)
    return;

  for (int row_y = min_y; row_y <= max_y; row_y++) {
    uint32_t *row = (uint32_t *) (data + (gsize) row_y * (gsize) stride);
    const guint8 *vis_row = visited + (gsize) row_y * (gsize) w;
    for (int col = min_x; col <= max_x; col++) {
      if (!vis_row[col])
        continue;
      if (make_transparent)
        row[col] = 0x00000000U;
      else
        row[col] = blend_pixel_for_lock (row[col], fill_px, fill_a, settings->target_alpha_lock);
    }
  }

  /* Pass 2: smooth/fill 1px anti-aliased fringe adjacent to the filled mask */
  if (tol > 0) {
    int y0 = MAX (0, min_y - 1);
    int y1 = MIN (h - 1, max_y + 1);
    int x0 = MAX (0, min_x - 1);
    int x1 = MIN (w - 1, max_x + 1);
    uint32_t target_a = (target_px >> 24) & 0xFFU;

    for (int row_y = y0; row_y <= y1; row_y++) {
      uint32_t *row = (uint32_t *) (data + (gsize) row_y * (gsize) stride);
      const uint32_t *srow = (const uint32_t *) (sdata + (gsize) row_y * (gsize) sstride);
      const guint8 *vis_row = visited + (gsize) row_y * (gsize) w;

      for (int col = x0; col <= x1; col++) {
        if (vis_row[col])
          continue;

        gboolean has_filled_neighbor = FALSE;
        for (int dy = -1; dy <= 1 && !has_filled_neighbor; dy++) {
          int ny = row_y + dy;
          if (ny < 0 || ny >= h)
            continue;
          const guint8 *nvis = visited + (gsize) ny * (gsize) w;
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

        uint32_t spx = srow[col];
        uint32_t px = row[col];
        uint32_t spx_a = (spx >> 24) & 0xFFU;

        if (make_transparent) {
          uint32_t px_a = (px >> 24) & 0xFFU;
          if (px_a == 0)
            continue;
          if (pixel_is_blend_between (spx, target_px, 0x00000000U)) {
            row[col] = 0x00000000U;
          } else {
            int diff = pixel_max_diff (spx, target_px);
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
          if (target_a > 0 && spx_a == 0)
            continue;
          if (pixel_is_blend_between (spx, target_px, fill_px)) {
            row[col] = blend_pixel_for_lock (px, fill_px, fill_a, settings->target_alpha_lock);
          } else {
            int diff = pixel_max_diff (spx, target_px);
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
                row[col] = blend_pixel_for_lock (px, pm_px, blend_a, settings->target_alpha_lock);
              }
            }
          }
        }
      }
    }
  }

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

static void
gradient_draw_preview (const PaintTool *tool G_GNUC_UNUSED,
                       cairo_t *cr,
                       double x0,
                       double y0,
                       double x1,
                       double y1,
                       const PaintToolSettings *settings)
{
  double dist = hypot (x1 - x0, y1 - y0);
  cairo_set_operator (cr, settings->target_alpha_lock ? CAIRO_OPERATOR_ATOP
                                                      : CAIRO_OPERATOR_OVER);
  if (dist < 0.5) {
    cairo_set_source_rgba (cr,
                           settings->color.red,
                           settings->color.green,
                           settings->color.blue,
                           settings->color.alpha);
    cairo_paint (cr);
    return;
  }

  cairo_pattern_t *pat = settings->gradient_radial
                           ? cairo_pattern_create_radial (x0, y0, 0.0, x0, y0, dist)
                           : cairo_pattern_create_linear (x0, y0, x1, y1);
  cairo_pattern_add_color_stop_rgba (pat, 0.0,
                                     settings->color.red,
                                     settings->color.green,
                                     settings->color.blue,
                                     settings->color.alpha);
  cairo_pattern_add_color_stop_rgba (pat, 1.0,
                                     settings->secondary_color.red,
                                     settings->secondary_color.green,
                                     settings->secondary_color.blue,
                                     settings->secondary_color.alpha);
  cairo_set_source (cr, pat);
  cairo_paint (cr);
  cairo_pattern_destroy (pat);
}

static void
gradient_get_bounds (const PaintTool *tool G_GNUC_UNUSED,
                     double x0 G_GNUC_UNUSED,
                     double y0 G_GNUC_UNUSED,
                     double x1 G_GNUC_UNUSED,
                     double y1 G_GNUC_UNUSED,
                     const PaintToolSettings *settings G_GNUC_UNUSED,
                     double *out_x0,
                     double *out_y0,
                     double *out_x1,
                     double *out_y1)
{
  *out_x0 = -65536.0;
  *out_y0 = -65536.0;
  *out_x1 = 65536.0;
  *out_y1 = 65536.0;
}

static inline uint32_t
retouch_pack_for_alpha_lock (uint32_t dst_px, uint32_t avg_a, uint32_t avg_r, uint32_t avg_g, uint32_t avg_b)
{
  uint32_t da = (dst_px >> 24) & 0xFFU;
  if (da == 0)
    return dst_px;
  if (avg_a == 0)
    return dst_px;
  uint32_t out_r = MIN (da, (avg_r * da + avg_a / 2) / avg_a);
  uint32_t out_g = MIN (da, (avg_g * da + avg_a / 2) / avg_a);
  uint32_t out_b = MIN (da, (avg_b * da + avg_a / 2) / avg_a);
  return (da << 24) | (out_r << 16) | (out_g << 8) | out_b;
}

static void
retouch_blur_stamp (cairo_t *cr, double cx, double cy, const PaintToolSettings *settings)
{
  cairo_surface_t *surface = cairo_get_target (cr);
  if (!surface || cairo_surface_get_type (surface) != CAIRO_SURFACE_TYPE_IMAGE)
    return;

  cairo_surface_flush (surface);
  int w = cairo_image_surface_get_width (surface);
  int h = cairo_image_surface_get_height (surface);
  int stride = cairo_image_surface_get_stride (surface);
  unsigned char *data = cairo_image_surface_get_data (surface);

  double r = MAX (2.0, effective_stroke_size (settings) * 0.5);
  double r2 = r * r;
  int kr = (r >= 8.0) ? 2 : 1;
  int bx0 = MAX (0, (int) floor (cx - r));
  int by0 = MAX (0, (int) floor (cy - r));
  int bx1 = MIN (w - 1, (int) ceil (cx + r));
  int by1 = MIN (h - 1, (int) ceil (cy + r));
  if (bx0 > bx1 || by0 > by1)
    return;

  int sx0 = MAX (0, bx0 - kr);
  int sy0 = MAX (0, by0 - kr);
  int sx1 = MIN (w - 1, bx1 + kr);
  int sy1 = MIN (h - 1, by1 + kr);
  int sw = sx1 - sx0 + 1;
  int sh = sy1 - sy0 + 1;
  g_autofree uint32_t *copy = g_new (uint32_t, (gsize) sw * (gsize) sh);
  for (int y = 0; y < sh; y++) {
    const uint32_t *src_row = (const uint32_t *) (data + (gsize) (sy0 + y) * (gsize) stride);
    memcpy (copy + (gsize) y * (gsize) sw, src_row + sx0, (gsize) sw * sizeof (uint32_t));
  }

  for (int y = by0; y <= by1; y++) {
    double dy = (double) y - cy;
    uint32_t *dst_row = (uint32_t *) (data + (gsize) y * (gsize) stride);
    for (int x = bx0; x <= bx1; x++) {
      double dx = (double) x - cx;
      if (dx * dx + dy * dy > r2)
        continue;

      uint32_t sa = 0, sr = 0, sg = 0, sb = 0, cnt = 0;
      for (int ky = y - kr; ky <= y + kr; ky++) {
        if (ky < sy0 || ky > sy1)
          continue;
        const uint32_t *crow = copy + (gsize) (ky - sy0) * (gsize) sw;
        for (int kx = x - kr; kx <= x + kr; kx++) {
          if (kx < sx0 || kx > sx1)
            continue;
          uint32_t p = crow[kx - sx0];
          sa += (p >> 24) & 0xFFU;
          sr += (p >> 16) & 0xFFU;
          sg += (p >> 8) & 0xFFU;
          sb += p & 0xFFU;
          cnt++;
        }
      }
      if (cnt == 0)
        continue;

      uint32_t avg_a = (sa + cnt / 2) / cnt;
      uint32_t avg_r = (sr + cnt / 2) / cnt;
      uint32_t avg_g = (sg + cnt / 2) / cnt;
      uint32_t avg_b = (sb + cnt / 2) / cnt;
      uint32_t dst_px = dst_row[x];
      uint32_t da = (dst_px >> 24) & 0xFFU;
      uint32_t dr = (dst_px >> 16) & 0xFFU;
      uint32_t dg = (dst_px >> 8) & 0xFFU;
      uint32_t db = dst_px & 0xFFU;

      uint32_t oa = (da * 40 + avg_a * 60 + 50) / 100;
      uint32_t or_ = (dr * 40 + avg_r * 60 + 50) / 100;
      uint32_t og_ = (dg * 40 + avg_g * 60 + 50) / 100;
      uint32_t ob_ = (db * 40 + avg_b * 60 + 50) / 100;

      if (settings->target_alpha_lock) {
        dst_row[x] = retouch_pack_for_alpha_lock (dst_px, oa, or_, og_, ob_);
      } else {
        dst_row[x] = (MIN (255u, oa) << 24) |
                     (MIN (oa, or_) << 16) |
                     (MIN (oa, og_) << 8) |
                     MIN (oa, ob_);
      }
    }
  }
  cairo_surface_mark_dirty (surface);
}

static void
retouch_pixelate_stamp (cairo_t *cr, double cx, double cy, const PaintToolSettings *settings)
{
  cairo_surface_t *surface = cairo_get_target (cr);
  if (!surface || cairo_surface_get_type (surface) != CAIRO_SURFACE_TYPE_IMAGE)
    return;

  cairo_surface_flush (surface);
  int w = cairo_image_surface_get_width (surface);
  int h = cairo_image_surface_get_height (surface);
  int stride = cairo_image_surface_get_stride (surface);
  unsigned char *data = cairo_image_surface_get_data (surface);

  double eff_sz = effective_stroke_size (settings);
  double r = MAX (2.0, eff_sz * 0.5);
  double r2 = r * r;
  int blk = CLAMP ((int) lround (MAX (3.0, eff_sz * 0.28)), 3, 24);
  int bx0 = MAX (0, (int) floor (cx - r));
  int by0 = MAX (0, (int) floor (cy - r));
  int bx1 = MIN (w - 1, (int) ceil (cx + r));
  int by1 = MIN (h - 1, (int) ceil (cy + r));
  if (bx0 > bx1 || by0 > by1)
    return;

  int gx0 = (bx0 / blk) * blk;
  int gy0 = (by0 / blk) * blk;
  for (int gy = gy0; gy <= by1; gy += blk) {
    int cy0 = MAX (0, gy);
    int cy1 = MIN (h - 1, gy + blk - 1);
    for (int gx = gx0; gx <= bx1; gx += blk) {
      int cx0 = MAX (0, gx);
      int cx1 = MIN (w - 1, gx + blk - 1);
      uint64_t sa = 0, sr = 0, sg = 0, sb = 0, cnt = 0;
      for (int y = cy0; y <= cy1; y++) {
        const uint32_t *row = (const uint32_t *) (data + (gsize) y * (gsize) stride);
        for (int x = cx0; x <= cx1; x++) {
          uint32_t p = row[x];
          sa += (p >> 24) & 0xFFU;
          sr += (p >> 16) & 0xFFU;
          sg += (p >> 8) & 0xFFU;
          sb += p & 0xFFU;
          cnt++;
        }
      }
      if (cnt == 0)
        continue;
      uint32_t avg_a = (uint32_t) ((sa + cnt / 2) / cnt);
      uint32_t avg_r = (uint32_t) ((sr + cnt / 2) / cnt);
      uint32_t avg_g = (uint32_t) ((sg + cnt / 2) / cnt);
      uint32_t avg_b = (uint32_t) ((sb + cnt / 2) / cnt);
      uint32_t avg_px = (MIN (255u, avg_a) << 24) |
                        (MIN (avg_a, avg_r) << 16) |
                        (MIN (avg_a, avg_g) << 8) |
                        MIN (avg_a, avg_b);

      for (int y = cy0; y <= cy1; y++) {
        double dy = (double) y - cy;
        uint32_t *row = (uint32_t *) (data + (gsize) y * (gsize) stride);
        for (int x = cx0; x <= cx1; x++) {
          double dx = (double) x - cx;
          if (dx * dx + dy * dy > r2)
            continue;
          if (settings->target_alpha_lock)
            row[x] = retouch_pack_for_alpha_lock (row[x], avg_a, avg_r, avg_g, avg_b);
          else
            row[x] = avg_px;
        }
      }
    }
  }
  cairo_surface_mark_dirty (surface);
}

static void
retouch_apply_stamp (const PaintTool *tool, cairo_t *cr, double x, double y, const PaintToolSettings *settings)
{
  if (g_strcmp0 (tool->id, "pixelate") == 0)
    retouch_pixelate_stamp (cr, x, y, settings);
  else
    retouch_blur_stamp (cr, x, y, settings);
}

static void
retouch_begin_stroke (const PaintTool *tool,
                      cairo_t *cr,
                      double x,
                      double y,
                      PaintToolSettings *settings)
{
  PaintPoint pt = { x, y };
  g_array_set_size (settings->history_points, 0);
  g_array_append_val (settings->history_points, pt);
  retouch_apply_stamp (tool, cr, x, y, settings);
}

static void
retouch_step_segment (const PaintTool *tool,
                      cairo_t *cr,
                      double x0,
                      double y0,
                      double x1,
                      double y1,
                      const PaintToolSettings *settings)
{
  double r = MAX (2.0, effective_stroke_size (settings) * 0.5);
  double dist = hypot (x1 - x0, y1 - y0);
  if (dist < 0.5)
    return;
  double spacing = MAX (1.5, r * 0.35);
  int steps = MAX (1, (int) ceil (dist / spacing));
  for (int i = 1; i <= steps; i++) {
    double t = (double) i / (double) steps;
    retouch_apply_stamp (tool, cr, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, settings);
  }
}

static void
retouch_continue_stroke (const PaintTool *tool,
                         cairo_t *cr,
                         double x,
                         double y,
                         PaintToolSettings *settings)
{
  GArray *pts = settings->history_points;
  if (pts->len == 0) {
    retouch_begin_stroke (tool, cr, x, y, settings);
    return;
  }
  PaintPoint last = g_array_index (pts, PaintPoint, pts->len - 1);
  retouch_step_segment (tool, cr, last.x, last.y, x, y, settings);
  PaintPoint next = { x, y };
  g_array_append_val (pts, next);
}

static void
retouch_end_stroke (const PaintTool *tool,
                    cairo_t *cr,
                    double x,
                    double y,
                    PaintToolSettings *settings)
{
  GArray *pts = settings->history_points;
  if (pts->len >= 1) {
    PaintPoint last = g_array_index (pts, PaintPoint, pts->len - 1);
    retouch_step_segment (tool, cr, last.x, last.y, x, y, settings);
  }
  g_array_set_size (pts, 0);
}

static const PaintTool BUILTIN_TOOLS[] = {
  {
    .id = "select",
    .name = N_("Rectangle Select"),
    .icon_name = "tool-select-symbolic",
    .tooltip = N_("Rectangle Select (M) · Drag to select, drag inside to move"),
    .uses_brush_size = FALSE,
    .is_select = TRUE,
    .begin_stroke = fill_noop,
    .continue_stroke = fill_noop,
    .end_stroke = fill_noop,
  },
  {
    .id = "select-ellipse",
    .name = N_("Ellipse Select"),
    .icon_name = "tool-select-ellipse-symbolic",
    .tooltip = N_("Ellipse Select · Hold Shift for circle"),
    .uses_brush_size = FALSE,
    .is_select = TRUE,
    .begin_stroke = fill_noop,
    .continue_stroke = fill_noop,
    .end_stroke = fill_noop,
  },
  {
    .id = "select-lasso",
    .name = N_("Freehand Lasso"),
    .icon_name = "tool-select-lasso-symbolic",
    .tooltip = N_("Freehand Lasso · Draw freehand selection region"),
    .uses_brush_size = FALSE,
    .is_select = TRUE,
    .begin_stroke = fill_noop,
    .continue_stroke = fill_noop,
    .end_stroke = fill_noop,
  },
  {
    .id = "select-wand",
    .name = N_("Magic Wand"),
    .icon_name = "tool-select-wand-symbolic",
    .tooltip = N_("Magic Wand · Click to select similar color region"),
    .uses_brush_size = FALSE,
    .is_select = TRUE,
    .begin_stroke = fill_noop,
    .continue_stroke = fill_noop,
    .end_stroke = fill_noop,
  },
  {
    .id = "brush",
    .name = N_("Brush"),
    .icon_name = "document-edit-symbolic",
    .tooltip = N_("Brush (B)"),
    .uses_brush_size = TRUE,
    .begin_stroke = freehand_begin_stroke,
    .continue_stroke = freehand_continue_stroke,
    .end_stroke = freehand_end_stroke,
  },
  {
    .id = "line",
    .name = N_("Line"),
    .icon_name = "tool-line-symbolic",
    .tooltip = N_("Line (L) · Hold Shift to snap angle"),
    .uses_brush_size = TRUE,
    .begin_stroke = line_begin_stroke,
    .continue_stroke = fill_noop,
    .end_stroke = line_end_stroke,
    .draw_preview = line_draw_preview,
    .snap_point = line_snap_point,
  },
  {
    .id = "eraser",
    .name = N_("Eraser"),
    .icon_name = "tool-eraser-symbolic",
    .tooltip = N_("Eraser (E)"),
    .uses_brush_size = TRUE,
    .begin_stroke = freehand_begin_stroke,
    .continue_stroke = freehand_continue_stroke,
    .end_stroke = freehand_end_stroke,
  },
  {
    .id = "fill",
    .name = N_("Fill"),
    .icon_name = "tool-fill-symbolic",
    .tooltip = N_("Fill (F)"),
    .uses_brush_size = FALSE,
    .begin_stroke = fill_begin_stroke,
    .continue_stroke = fill_noop,
    .end_stroke = fill_noop,
  },
  {
    .id = "gradient",
    .name = N_("Gradient"),
    .icon_name = "tool-gradient-symbolic",
    .tooltip = N_("Gradient (G) · Drag from Primary to Secondary color (Shift snaps)"),
    .uses_brush_size = FALSE,
    .begin_stroke = line_begin_stroke,
    .continue_stroke = fill_noop,
    .end_stroke = line_end_stroke,
    .draw_preview = gradient_draw_preview,
    .snap_point = line_snap_point,
    .get_bounds = gradient_get_bounds,
  },
  {
    .id = "blur",
    .name = N_("Blur Brush"),
    .icon_name = "tool-blur-symbolic",
    .tooltip = N_("Blur Brush · Soften painted details"),
    .uses_brush_size = TRUE,
    .is_retouch = TRUE,
    .begin_stroke = retouch_begin_stroke,
    .continue_stroke = retouch_continue_stroke,
    .end_stroke = retouch_end_stroke,
  },
  {
    .id = "pixelate",
    .name = N_("Pixelate Brush"),
    .icon_name = "tool-pixelate-symbolic",
    .tooltip = N_("Pixelate Brush · Paint mosaic blocks"),
    .uses_brush_size = TRUE,
    .is_retouch = TRUE,
    .begin_stroke = retouch_begin_stroke,
    .continue_stroke = retouch_continue_stroke,
    .end_stroke = retouch_end_stroke,
  },
  {
    .id = "text",
    .name = N_("Text"),
    .icon_name = "tool-text-symbolic",
    .tooltip = N_("Text (X) · Click canvas to add text"),
    .uses_brush_size = FALSE,
    .begin_stroke = fill_noop,
    .continue_stroke = fill_noop,
    .end_stroke = fill_noop,
  },
  {
    .id = "eyedropper",
    .name = N_("Color Picker"),
    .icon_name = "tool-eyedropper-symbolic",
    .tooltip = N_("Color Picker (I) · Or Alt+Click anytime"),
    .uses_brush_size = FALSE,
    .begin_stroke = fill_noop,
    .continue_stroke = fill_noop,
    .end_stroke = fill_noop,
  },
  {
    .id = "transparent",
    .name = N_("Brush"),
    .icon_name = "tool-transparent-symbolic",
    .tooltip = N_("Transparent Brush (T) · Paint areas transparent"),
    .uses_brush_size = TRUE,
    .is_transparency = TRUE,
    .begin_stroke = freehand_begin_stroke,
    .continue_stroke = freehand_continue_stroke,
    .end_stroke = freehand_end_stroke,
  },
  {
    .id = "transparent-fill",
    .name = N_("Fill"),
    .icon_name = "tool-transparent-fill-symbolic",
    .tooltip = N_("Transparent Fill · Click an area to make it transparent"),
    .uses_brush_size = FALSE,
    .is_transparency = TRUE,
    .begin_stroke = transparent_fill_begin_stroke,
    .continue_stroke = fill_noop,
    .end_stroke = fill_noop,
  },
  {
    .id = "shape-rectangle",
    .name = N_("Rectangle"),
    .icon_name = "tool-shape-rect-symbolic",
    .filled_icon_name = "tool-shape-rect-filled-symbolic",
    .tooltip = N_("Rectangle · Hold Shift for square"),
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
    .name = N_("Rounded"),
    .icon_name = "tool-shape-rounded-rect-symbolic",
    .filled_icon_name = "tool-shape-rounded-rect-filled-symbolic",
    .tooltip = N_("Rounded Rectangle · Hold Shift for square"),
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
    .name = N_("Circle"),
    .icon_name = "tool-shape-ellipse-symbolic",
    .filled_icon_name = "tool-shape-ellipse-filled-symbolic",
    .tooltip = N_("Ellipse / Circle · Hold Shift for circle"),
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
    .name = N_("Triangle"),
    .icon_name = "tool-shape-triangle-symbolic",
    .filled_icon_name = "tool-shape-triangle-filled-symbolic",
    .tooltip = N_("Triangle · Hold Shift for 1:1 proportions"),
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
    .name = N_("Diamond"),
    .icon_name = "tool-shape-diamond-symbolic",
    .filled_icon_name = "tool-shape-diamond-filled-symbolic",
    .tooltip = N_("Diamond · Hold Shift for 1:1 proportions"),
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
    .name = N_("Hexagon"),
    .icon_name = "tool-shape-hexagon-symbolic",
    .filled_icon_name = "tool-shape-hexagon-filled-symbolic",
    .tooltip = N_("Hexagon · Hold Shift for regular hexagon"),
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
    .name = N_("Star"),
    .icon_name = "tool-shape-star-symbolic",
    .filled_icon_name = "tool-shape-star-filled-symbolic",
    .tooltip = N_("Star · Hold Shift for symmetric star"),
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
    .name = N_("Arrow"),
    .icon_name = "tool-shape-arrow-symbolic",
    .filled_icon_name = "tool-shape-arrow-filled-symbolic",
    .tooltip = N_("Arrow · Hold Shift to snap angle"),
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
    .name = N_("Block Arrow"),
    .icon_name = "tool-shape-block-arrow-symbolic",
    .filled_icon_name = "tool-shape-block-arrow-filled-symbolic",
    .tooltip = N_("Block Arrow · Hold Shift to snap angle"),
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
  if (g_strcmp0 (tool->id, "gradient") == 0 && settings && settings->gradient_radial)
    return "tool-gradient-radial-symbolic";
  if (tool->is_shape && settings && settings->shape_filled && tool->filled_icon_name)
    return tool->filled_icon_name;
  return tool->icon_name;
}
