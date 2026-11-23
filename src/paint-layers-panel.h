#pragma once

#include <adwaita.h>

#include "paint-canvas.h"

G_BEGIN_DECLS

#define PAINT_TYPE_LAYERS_PANEL (paint_layers_panel_get_type ())
G_DECLARE_FINAL_TYPE (PaintLayersPanel, paint_layers_panel, PAINT, LAYERS_PANEL, AdwBin)

PaintLayersPanel *paint_layers_panel_new (PaintCanvas *canvas);
void paint_layers_panel_sync (PaintLayersPanel *self, gboolean panel_visible);

G_END_DECLS
