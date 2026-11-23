#pragma once

#include <cairo.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define PAINT_MAX_RECENT_COLORS 8
#define PAINT_MAX_CUSTOM_COLORS 16

typedef enum {
  PAINT_BRUSH_SMOOTH = 0,
  PAINT_BRUSH_SOFT = 1,
  PAINT_BRUSH_PIXEL = 2,
} PaintBrushStyle;

typedef struct {
  double x;
  double y;
} PaintPoint;

typedef struct {
  GdkRGBA color;
  GdkRGBA secondary_color;
  double size;
  GdkRGBA background_color;
  const char *active_tool_id;
  gboolean shape_filled;
  gboolean target_alpha_lock;
  PaintBrushStyle brush_style;
  int fill_tolerance;
  gboolean fill_sample_all_layers;
  cairo_surface_t *fill_sample_surface; /* unowned */
  gboolean gradient_radial;
  gboolean gradient_fade_transparent;
  gboolean stroke_has_clip;
  cairo_rectangle_int_t stroke_clip_rect;
  cairo_surface_t *stroke_clip_mask; /* unowned */
  double stroke_pressure;
  gboolean pressure_size;
  gboolean pressure_opacity;
  GdkRGBA recent_colors[PAINT_MAX_RECENT_COLORS];
  int n_recent_colors;
  GdkRGBA custom_colors[PAINT_MAX_CUSTOM_COLORS];
  int n_custom_colors;
  char *text_font_family;
  double text_font_size;
  gboolean text_bold;
  gboolean text_italic;
  PangoAlignment text_align;
  gboolean text_outline;
  gboolean text_bg_pill;
  GArray *history_points; /* element-type: PaintPoint */
} PaintToolSettings;

typedef struct _PaintTool PaintTool;

struct _PaintTool {
  const char *id;
  const char *name;
  const char *icon_name;
  const char *filled_icon_name;
  const char *tooltip;
  gboolean uses_brush_size;
  gboolean is_shape;
  gboolean is_transparency;
  gboolean is_select;
  gboolean is_retouch;

  void (*begin_stroke) (const PaintTool *tool,
                        cairo_t *cr,
                        double x,
                        double y,
                        PaintToolSettings *settings);
  void (*continue_stroke) (const PaintTool *tool,
                           cairo_t *cr,
                           double x,
                           double y,
                           PaintToolSettings *settings);
  void (*end_stroke) (const PaintTool *tool,
                      cairo_t *cr,
                      double x,
                      double y,
                      PaintToolSettings *settings);
  void (*draw_preview) (const PaintTool *tool,
                        cairo_t *cr,
                        double x0,
                        double y0,
                        double x1,
                        double y1,
                        const PaintToolSettings *settings);
  void (*snap_point) (double x0,
                      double y0,
                      double *x1,
                      double *y1);
  void (*get_bounds) (const PaintTool *tool,
                      double x0,
                      double y0,
                      double x1,
                      double y1,
                      const PaintToolSettings *settings,
                      double *out_x0,
                      double *out_y0,
                      double *out_x1,
                      double *out_y1);
};

PaintToolSettings *paint_tool_settings_new (void);
void paint_tool_settings_free (PaintToolSettings *settings);
void paint_tool_settings_add_recent_color (PaintToolSettings *settings,
                                           const GdkRGBA *color);
void paint_tool_settings_swap_colors (PaintToolSettings *settings);
void paint_tool_settings_reset_colors (PaintToolSettings *settings);
void paint_tool_settings_add_custom_color (PaintToolSettings *settings,
                                           const GdkRGBA *color);
void paint_tool_settings_remove_custom_color (PaintToolSettings *settings,
                                              int index);
void paint_tool_settings_set_text_font_family (PaintToolSettings *settings,
                                               const char *family);

cairo_surface_t *paint_tools_build_wand_mask (cairo_surface_t *src,
                                              int sx,
                                              int sy,
                                              int tolerance,
                                              cairo_rectangle_int_t *out_rect);

const PaintTool *paint_tools_get_all (gsize *n_tools);
const PaintTool *paint_tools_find (const char *tool_id);
const char *paint_tool_get_icon_name (const PaintTool *tool,
                                      const PaintToolSettings *settings);

G_END_DECLS
