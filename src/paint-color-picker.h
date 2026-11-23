#pragma once

#include <gtk/gtk.h>

#include "paint-tools.h"

G_BEGIN_DECLS

typedef struct _PaintColorPicker PaintColorPicker;

typedef void (*PaintColorChangedCallback) (gpointer user_data);

void paint_draw_checkerboard (cairo_t *cr, double x, double y, double w, double h, double cell);

PaintColorPicker *paint_color_picker_new (PaintToolSettings *settings,
                                          GtkWidget *focus_on_enter,
                                          PaintColorChangedCallback on_changed,
                                          gpointer user_data);
void paint_color_picker_free (PaintColorPicker *picker);

GtkWidget *paint_color_picker_get_button (PaintColorPicker *picker);
void paint_color_picker_sync (PaintColorPicker *picker);
void paint_color_picker_sync_recent (PaintColorPicker *picker);
void paint_color_picker_sync_custom (PaintColorPicker *picker);
void paint_color_picker_set_palette_callback (PaintColorPicker *picker,
                                              PaintColorChangedCallback on_palette,
                                              gpointer user_data);

G_END_DECLS
