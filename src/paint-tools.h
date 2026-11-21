#pragma once

#include <cairo.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define PAINT_MAX_RECENT_COLORS 8

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
  double size;
  GdkRGBA background_color;
  const char *active_tool_id;
  gboolean shape_filled;
  PaintBrushStyle brush_style;
  GdkRGBA recent_colors[PAINT_MAX_RECENT_COLORS];
  int n_recent_colors;
  char *text_font_family;
  double text_font_size;
  gboolean text_bold;
  gboolean text_italic;
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
void paint_tool_settings_set_text_font_family (PaintToolSettings *settings,
                                               const char *family);

const PaintTool *paint_tools_get_all (gsize *n_tools);
const PaintTool *paint_tools_find (const char *tool_id);
const char *paint_tool_get_icon_name (const PaintTool *tool,
                                      const PaintToolSettings *settings);

G_END_DECLS
