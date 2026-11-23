#pragma once

#include <adwaita.h>

#include "paint-canvas.h"
#include "paint-tools.h"

G_BEGIN_DECLS

#define PAINT_TYPE_CANVAS_PAGE (paint_canvas_page_get_type ())
G_DECLARE_FINAL_TYPE (PaintCanvasPage, paint_canvas_page, PAINT, CANVAS_PAGE, AdwBin)

PaintCanvasPage *paint_canvas_page_new (PaintDocument *doc, PaintToolSettings *settings);

PaintCanvas *paint_canvas_page_get_canvas (PaintCanvasPage *self);
void paint_canvas_page_select_tool (PaintCanvasPage *self, const char *tool_id);
void paint_canvas_page_select_last_select (PaintCanvasPage *self);
void paint_canvas_page_select_last_shape (PaintCanvasPage *self);
void paint_canvas_page_select_last_transparency (PaintCanvasPage *self);
void paint_canvas_page_sync_color_picker (PaintCanvasPage *self);
void paint_canvas_page_set_brush_size (PaintCanvasPage *self, double size);
void paint_canvas_page_adjust_brush_size (PaintCanvasPage *self, double delta);
gboolean paint_canvas_page_zoom_fit (PaintCanvasPage *self);
void paint_canvas_page_toggle_osd (PaintCanvasPage *self);
void paint_canvas_page_set_osd_visible (PaintCanvasPage *self, gboolean visible);
void paint_canvas_page_set_space_pan (PaintCanvasPage *self, gboolean active);
void paint_canvas_page_set_layers_visible (PaintCanvasPage *self, gboolean visible);
gboolean paint_canvas_page_get_layers_visible (PaintCanvasPage *self);
void paint_canvas_page_sync_controls (PaintCanvasPage *self);

char *paint_canvas_page_dup_title (PaintCanvasPage *self);
char *paint_canvas_page_dup_subtitle (PaintCanvasPage *self);

G_END_DECLS
