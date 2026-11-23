#include "paint-filters.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

typedef struct {
  int x0;
  int y0;
  int cw;
  int ch;
  int mask_ox;
  int mask_oy;
  const unsigned char *mask_data;
  int mask_stride;
  int mask_w;
  int mask_h;
} FilterRegion;

static gboolean
prepare_filter_region (cairo_surface_t *surf,
                       const cairo_rectangle_int_t *rect,
                       cairo_surface_t *mask,
                       FilterRegion *out)
{
  memset (out, 0, sizeof (*out));
  if (!surf || cairo_surface_status (surf) != CAIRO_STATUS_SUCCESS ||
      cairo_image_surface_get_format (surf) != CAIRO_FORMAT_ARGB32) {
    return FALSE;
  }

  int w = cairo_image_surface_get_width (surf);
  int h = cairo_image_surface_get_height (surf);
  int rx = rect ? rect->x : 0;
  int ry = rect ? rect->y : 0;
  int rw = rect ? rect->width : w;
  int rh = rect ? rect->height : h;

  int x0 = CLAMP (rx, 0, w);
  int y0 = CLAMP (ry, 0, h);
  int x1 = CLAMP (rx + rw, 0, w);
  int y1 = CLAMP (ry + rh, 0, h);
  int cw = x1 - x0;
  int ch = y1 - y0;
  if (cw <= 0 || ch <= 0)
    return FALSE;

  out->x0 = x0;
  out->y0 = y0;
  out->cw = cw;
  out->ch = ch;
  out->mask_ox = rx;
  out->mask_oy = ry;

  if (mask && cairo_surface_status (mask) == CAIRO_STATUS_SUCCESS &&
      cairo_image_surface_get_format (mask) == CAIRO_FORMAT_A8) {
    cairo_surface_flush (mask);
    out->mask_data = cairo_image_surface_get_data (mask);
    out->mask_stride = cairo_image_surface_get_stride (mask);
    out->mask_w = cairo_image_surface_get_width (mask);
    out->mask_h = cairo_image_surface_get_height (mask);
  }
  return TRUE;
}

static inline gboolean
region_allows (const FilterRegion *reg, int x, int y)
{
  if (!reg->mask_data)
    return TRUE;
  int mx = x - reg->mask_ox;
  int my = y - reg->mask_oy;
  if (mx < 0 || mx >= reg->mask_w || my < 0 || my >= reg->mask_h)
    return FALSE;
  return reg->mask_data[(gsize) my * (gsize) reg->mask_stride + mx] >= 128;
}

static inline void
unpremultiply_pixel (uint32_t px, uint8_t *out_a, double *r, double *g, double *b)
{
  uint8_t a = (uint8_t) ((px >> 24) & 0xFFU);
  *out_a = a;
  if (a == 0) {
    *r = *g = *b = 0.0;
    return;
  }
  double inv = 1.0 / (double) a;
  *r = CLAMP ((double) ((px >> 16) & 0xFFU) * inv, 0.0, 1.0);
  *g = CLAMP ((double) ((px >> 8) & 0xFFU) * inv, 0.0, 1.0);
  *b = CLAMP ((double) (px & 0xFFU) * inv, 0.0, 1.0);
}

static inline uint32_t
premultiply_pixel (uint8_t a, double r, double g, double b)
{
  if (a == 0)
    return 0;
  double ad = (double) a;
  uint32_t pr = (uint32_t) lround (CLAMP (r, 0.0, 1.0) * ad);
  uint32_t pg = (uint32_t) lround (CLAMP (g, 0.0, 1.0) * ad);
  uint32_t pb = (uint32_t) lround (CLAMP (b, 0.0, 1.0) * ad);
  return ((uint32_t) a << 24) | (pr << 16) | (pg << 8) | pb;
}

typedef void (*PointPixelFunc) (double *r, double *g, double *b, const void *ctx);

static void
apply_point_filter (cairo_surface_t *surf,
                    const cairo_rectangle_int_t *rect,
                    cairo_surface_t *mask,
                    PointPixelFunc func,
                    const void *ctx)
{
  FilterRegion reg;
  if (!prepare_filter_region (surf, rect, mask, &reg))
    return;

  cairo_surface_flush (surf);
  int stride_px = cairo_image_surface_get_stride (surf) / 4;
  uint32_t *px = (uint32_t *) cairo_image_surface_get_data (surf);

  for (int y = reg.y0; y < reg.y0 + reg.ch; y++) {
    uint32_t *row = px + (gsize) y * (gsize) stride_px;
    for (int x = reg.x0; x < reg.x0 + reg.cw; x++) {
      if (!region_allows (&reg, x, y))
        continue;
      uint8_t a = 0;
      double r = 0.0, g = 0.0, b = 0.0;
      unpremultiply_pixel (row[x], &a, &r, &g, &b);
      if (a == 0)
        continue;
      func (&r, &g, &b, ctx);
      row[x] = premultiply_pixel (a, r, g, b);
    }
  }
  cairo_surface_mark_dirty (surf);
}

static void
pixel_invert (double *r, double *g, double *b, const void *ctx G_GNUC_UNUSED)
{
  *r = 1.0 - *r;
  *g = 1.0 - *g;
  *b = 1.0 - *b;
}

void
paint_filter_invert (cairo_surface_t *surf,
                     const cairo_rectangle_int_t *rect,
                     cairo_surface_t *mask,
                     gboolean alpha_lock G_GNUC_UNUSED)
{
  apply_point_filter (surf, rect, mask, pixel_invert, NULL);
}

static void
pixel_grayscale (double *r, double *g, double *b, const void *ctx G_GNUC_UNUSED)
{
  double lum = 0.2126 * (*r) + 0.7152 * (*g) + 0.0722 * (*b);
  *r = lum;
  *g = lum;
  *b = lum;
}

void
paint_filter_grayscale (cairo_surface_t *surf,
                        const cairo_rectangle_int_t *rect,
                        cairo_surface_t *mask,
                        gboolean alpha_lock G_GNUC_UNUSED)
{
  apply_point_filter (surf, rect, mask, pixel_grayscale, NULL);
}

typedef struct {
  double brightness;
  double contrast_factor;
} BrightnessContrastCtx;

static void
pixel_brightness_contrast (double *r, double *g, double *b, const void *ctx)
{
  const BrightnessContrastCtx *c = ctx;
  *r = CLAMP ((*r - 0.5) * c->contrast_factor + 0.5 + c->brightness, 0.0, 1.0);
  *g = CLAMP ((*g - 0.5) * c->contrast_factor + 0.5 + c->brightness, 0.0, 1.0);
  *b = CLAMP ((*b - 0.5) * c->contrast_factor + 0.5 + c->brightness, 0.0, 1.0);
}

void
paint_filter_brightness_contrast (cairo_surface_t *surf,
                                  const cairo_rectangle_int_t *rect,
                                  cairo_surface_t *mask,
                                  gboolean alpha_lock G_GNUC_UNUSED,
                                  double brightness,
                                  double contrast)
{
  BrightnessContrastCtx ctx;
  ctx.brightness = CLAMP (brightness, -1.0, 1.0);
  double c255 = CLAMP (contrast, -1.0, 1.0) * 255.0;
  ctx.contrast_factor = (259.0 * (c255 + 255.0)) / (255.0 * (259.0 - c255));
  apply_point_filter (surf, rect, mask, pixel_brightness_contrast, &ctx);
}

typedef struct {
  double hue_deg;
  double sat_delta;
  double light_delta;
} HslCtx;

static inline double
hue_to_rgb (double p, double q, double t)
{
  if (t < 0.0)
    t += 1.0;
  if (t > 1.0)
    t -= 1.0;
  if (t < 1.0 / 6.0)
    return p + (q - p) * 6.0 * t;
  if (t < 0.5)
    return q;
  if (t < 2.0 / 3.0)
    return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
  return p;
}

static void
pixel_hsl (double *r, double *g, double *b, const void *ctx)
{
  const HslCtx *p = ctx;
  double maxc = fmax (*r, fmax (*g, *b));
  double minc = fmin (*r, fmin (*g, *b));
  double l = 0.5 * (maxc + minc);
  double d = maxc - minc;

  if (d <= 1e-4) {
    double nl = CLAMP (l + p->light_delta, 0.0, 1.0);
    *r = *g = *b = nl;
    return;
  }

  double s = (l > 0.5) ? (d / (2.0 - maxc - minc)) : (d / (maxc + minc));
  double h = 0.0;
  if (maxc == *r)
    h = (*g - *b) / d + (*g < *b ? 6.0 : 0.0);
  else if (maxc == *g)
    h = (*b - *r) / d + 2.0;
  else
    h = (*r - *g) / d + 4.0;
  h *= 60.0;

  h = fmod (h + p->hue_deg, 360.0);
  if (h < 0.0)
    h += 360.0;
  s = CLAMP (s + p->sat_delta, 0.0, 1.0);
  l = CLAMP (l + p->light_delta, 0.0, 1.0);

  if (s <= 1e-4) {
    *r = *g = *b = l;
    return;
  }

  double q = (l < 0.5) ? (l * (1.0 + s)) : (l + s - l * s);
  double pv = 2.0 * l - q;
  double hk = h / 360.0;
  *r = hue_to_rgb (pv, q, hk + 1.0 / 3.0);
  *g = hue_to_rgb (pv, q, hk);
  *b = hue_to_rgb (pv, q, hk - 1.0 / 3.0);
}

void
paint_filter_hsl (cairo_surface_t *surf,
                  const cairo_rectangle_int_t *rect,
                  cairo_surface_t *mask,
                  gboolean alpha_lock G_GNUC_UNUSED,
                  double hue_deg,
                  double sat_delta,
                  double light_delta)
{
  HslCtx ctx = {
    .hue_deg = CLAMP (hue_deg, -180.0, 180.0),
    .sat_delta = CLAMP (sat_delta, -1.0, 1.0),
    .light_delta = CLAMP (light_delta, -1.0, 1.0),
  };
  apply_point_filter (surf, rect, mask, pixel_hsl, &ctx);
}

static void
box_blur_pass (const uint32_t *src, uint32_t *dst, uint32_t *tmp, int w, int h, int br)
{
  uint32_t win = (uint32_t) (2 * br + 1);
  uint32_t half = win / 2U;

  for (int y = 0; y < h; y++) {
    const uint32_t *srow = src + (gsize) y * (gsize) w;
    uint32_t *drow = tmp + (gsize) y * (gsize) w;
    uint32_t sa = 0, sr = 0, sg = 0, sb = 0;

    for (int k = -br; k <= br; k++) {
      uint32_t p = srow[CLAMP (k, 0, w - 1)];
      sa += (p >> 24) & 0xFFU;
      sr += (p >> 16) & 0xFFU;
      sg += (p >> 8) & 0xFFU;
      sb += p & 0xFFU;
    }
    drow[0] = (((sa + half) / win) << 24) |
              (((sr + half) / win) << 16) |
              (((sg + half) / win) << 8) |
              ((sb + half) / win);

    for (int x = 1; x < w; x++) {
      uint32_t prem = srow[CLAMP (x - 1 - br, 0, w - 1)];
      uint32_t padd = srow[CLAMP (x + br, 0, w - 1)];
      sa = sa - ((prem >> 24) & 0xFFU) + ((padd >> 24) & 0xFFU);
      sr = sr - ((prem >> 16) & 0xFFU) + ((padd >> 16) & 0xFFU);
      sg = sg - ((prem >> 8) & 0xFFU) + ((padd >> 8) & 0xFFU);
      sb = sb - (prem & 0xFFU) + (padd & 0xFFU);
      drow[x] = (((sa + half) / win) << 24) |
                (((sr + half) / win) << 16) |
                (((sg + half) / win) << 8) |
                ((sb + half) / win);
    }
  }

  for (int x = 0; x < w; x++) {
    uint32_t sa = 0, sr = 0, sg = 0, sb = 0;

    for (int k = -br; k <= br; k++) {
      uint32_t p = tmp[(gsize) CLAMP (k, 0, h - 1) * (gsize) w + x];
      sa += (p >> 24) & 0xFFU;
      sr += (p >> 16) & 0xFFU;
      sg += (p >> 8) & 0xFFU;
      sb += p & 0xFFU;
    }
    dst[x] = (((sa + half) / win) << 24) |
             (((sr + half) / win) << 16) |
             (((sg + half) / win) << 8) |
             ((sb + half) / win);

    for (int y = 1; y < h; y++) {
      uint32_t prem = tmp[(gsize) CLAMP (y - 1 - br, 0, h - 1) * (gsize) w + x];
      uint32_t padd = tmp[(gsize) CLAMP (y + br, 0, h - 1) * (gsize) w + x];
      sa = sa - ((prem >> 24) & 0xFFU) + ((padd >> 24) & 0xFFU);
      sr = sr - ((prem >> 16) & 0xFFU) + ((padd >> 16) & 0xFFU);
      sg = sg - ((prem >> 8) & 0xFFU) + ((padd >> 8) & 0xFFU);
      sb = sb - (prem & 0xFFU) + (padd & 0xFFU);
      dst[(gsize) y * (gsize) w + x] = (((sa + half) / win) << 24) |
                                       (((sr + half) / win) << 16) |
                                       (((sg + half) / win) << 8) |
                                       ((sb + half) / win);
    }
  }
}

void
paint_filter_gaussian_blur (cairo_surface_t *surf,
                            const cairo_rectangle_int_t *rect,
                            cairo_surface_t *mask,
                            gboolean alpha_lock,
                            double radius)
{
  double rad = CLAMP (radius, 0.0, 50.0);
  if (rad < 0.25)
    return;

  FilterRegion reg;
  if (!prepare_filter_region (surf, rect, mask, &reg))
    return;

  int br = MAX (1, (int) lround (rad * 0.5));
  gsize npix = (gsize) reg.cw * (gsize) reg.ch;
  uint32_t *buf_a = g_new (uint32_t, npix);
  uint32_t *buf_b = g_new (uint32_t, npix);

  cairo_surface_flush (surf);
  int stride_px = cairo_image_surface_get_stride (surf) / 4;
  uint32_t *px = (uint32_t *) cairo_image_surface_get_data (surf);

  for (int y = 0; y < reg.ch; y++) {
    const uint32_t *srow = px + (gsize) (reg.y0 + y) * (gsize) stride_px + reg.x0;
    memcpy (buf_a + (gsize) y * (gsize) reg.cw, srow, (gsize) reg.cw * sizeof (uint32_t));
  }

  /* 3 separable box blur passes approximate a Gaussian kernel */
  for (int pass = 0; pass < 3; pass++)
    box_blur_pass (buf_a, buf_a, buf_b, reg.cw, reg.ch, br);

  for (int y = 0; y < reg.ch; y++) {
    uint32_t *drow = px + (gsize) (reg.y0 + y) * (gsize) stride_px;
    const uint32_t *brow = buf_a + (gsize) y * (gsize) reg.cw;
    for (int x = 0; x < reg.cw; x++) {
      if (!region_allows (&reg, reg.x0 + x, reg.y0 + y))
        continue;
      uint32_t bp = brow[x];
      if (!alpha_lock) {
        drow[reg.x0 + x] = bp;
        continue;
      }
      uint32_t oa = (drow[reg.x0 + x] >> 24) & 0xFFU;
      if (oa == 0)
        continue;
      uint32_t ba = (bp >> 24) & 0xFFU;
      if (ba == 0)
        continue;
      uint32_t nr = MIN (oa, (((bp >> 16) & 0xFFU) * oa + ba / 2U) / ba);
      uint32_t ng = MIN (oa, (((bp >> 8) & 0xFFU) * oa + ba / 2U) / ba);
      uint32_t nb = MIN (oa, ((bp & 0xFFU) * oa + ba / 2U) / ba);
      drow[reg.x0 + x] = (oa << 24) | (nr << 16) | (ng << 8) | nb;
    }
  }

  g_free (buf_a);
  g_free (buf_b);
  cairo_surface_mark_dirty (surf);
}

static inline void
sample_unpremult_fallback (const uint32_t *buf,
                           int w,
                           int h,
                           int x,
                           int y,
                           double cr,
                           double cg,
                           double cb,
                           double *out_r,
                           double *out_g,
                           double *out_b)
{
  int sx = CLAMP (x, 0, w - 1);
  int sy = CLAMP (y, 0, h - 1);
  uint8_t a = 0;
  double r = 0.0, g = 0.0, b = 0.0;
  unpremultiply_pixel (buf[(gsize) sy * (gsize) w + sx], &a, &r, &g, &b);
  if (a == 0) {
    *out_r = cr;
    *out_g = cg;
    *out_b = cb;
  } else {
    *out_r = r;
    *out_g = g;
    *out_b = b;
  }
}

void
paint_filter_sharpen (cairo_surface_t *surf,
                      const cairo_rectangle_int_t *rect,
                      cairo_surface_t *mask,
                      gboolean alpha_lock G_GNUC_UNUSED,
                      double amount)
{
  double k = 0.5 * CLAMP (amount, 0.0, 2.0);
  if (k <= 0.0)
    return;

  FilterRegion reg;
  if (!prepare_filter_region (surf, rect, mask, &reg))
    return;

  gsize npix = (gsize) reg.cw * (gsize) reg.ch;
  uint32_t *orig = g_new (uint32_t, npix);

  cairo_surface_flush (surf);
  int stride_px = cairo_image_surface_get_stride (surf) / 4;
  uint32_t *px = (uint32_t *) cairo_image_surface_get_data (surf);

  for (int y = 0; y < reg.ch; y++) {
    const uint32_t *srow = px + (gsize) (reg.y0 + y) * (gsize) stride_px + reg.x0;
    memcpy (orig + (gsize) y * (gsize) reg.cw, srow, (gsize) reg.cw * sizeof (uint32_t));
  }

  for (int y = 0; y < reg.ch; y++) {
    uint32_t *drow = px + (gsize) (reg.y0 + y) * (gsize) stride_px;
    for (int x = 0; x < reg.cw; x++) {
      if (!region_allows (&reg, reg.x0 + x, reg.y0 + y))
        continue;
      uint8_t a = 0;
      double cr = 0.0, cg = 0.0, cb = 0.0;
      unpremultiply_pixel (orig[(gsize) y * (gsize) reg.cw + x], &a, &cr, &cg, &cb);
      if (a == 0)
        continue;

      double lr, lg, lb, rr, rg, rb, ur, ug, ub, dr, dg, db;
      sample_unpremult_fallback (orig, reg.cw, reg.ch, x - 1, y, cr, cg, cb, &lr, &lg, &lb);
      sample_unpremult_fallback (orig, reg.cw, reg.ch, x + 1, y, cr, cg, cb, &rr, &rg, &rb);
      sample_unpremult_fallback (orig, reg.cw, reg.ch, x, y - 1, cr, cg, cb, &ur, &ug, &ub);
      sample_unpremult_fallback (orig, reg.cw, reg.ch, x, y + 1, cr, cg, cb, &dr, &dg, &db);

      double nr = cr * (1.0 + 4.0 * k) - k * (lr + rr + ur + dr);
      double ng = cg * (1.0 + 4.0 * k) - k * (lg + rg + ug + dg);
      double nb = cb * (1.0 + 4.0 * k) - k * (lb + rb + ub + db);
      drow[reg.x0 + x] = premultiply_pixel (a, nr, ng, nb);
    }
  }

  g_free (orig);
  cairo_surface_mark_dirty (surf);
}
