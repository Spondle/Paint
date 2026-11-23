#pragma once

#include <cairo.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

void paint_filter_invert (cairo_surface_t *surf,
                          const cairo_rectangle_int_t *rect,
                          cairo_surface_t *mask,
                          gboolean alpha_lock);

void paint_filter_grayscale (cairo_surface_t *surf,
                             const cairo_rectangle_int_t *rect,
                             cairo_surface_t *mask,
                             gboolean alpha_lock);

void paint_filter_brightness_contrast (cairo_surface_t *surf,
                                       const cairo_rectangle_int_t *rect,
                                       cairo_surface_t *mask,
                                       gboolean alpha_lock,
                                       double brightness,
                                       double contrast);

void paint_filter_hsl (cairo_surface_t *surf,
                       const cairo_rectangle_int_t *rect,
                       cairo_surface_t *mask,
                       gboolean alpha_lock,
                       double hue_deg,
                       double sat_delta,
                       double light_delta);

void paint_filter_gaussian_blur (cairo_surface_t *surf,
                                 const cairo_rectangle_int_t *rect,
                                 cairo_surface_t *mask,
                                 gboolean alpha_lock,
                                 double radius);

void paint_filter_sharpen (cairo_surface_t *surf,
                           const cairo_rectangle_int_t *rect,
                           cairo_surface_t *mask,
                           gboolean alpha_lock,
                           double amount);

G_END_DECLS
