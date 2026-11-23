#pragma once

#include <adwaita.h>

#include "paint-canvas.h"

G_BEGIN_DECLS

typedef void (*PaintExportDoneCallback) (GFile *exported_file, gpointer user_data);

void paint_dialog_confirm_clear (GtkWidget *parent, PaintCanvas *canvas);
void paint_dialog_show_resize (GtkWidget *parent, PaintCanvas *canvas);
void paint_dialog_show_export (GtkWidget *parent,
                               PaintDocument *doc,
                               PaintExportDoneCallback on_exported,
                               gpointer user_data);

void paint_dialog_show_brightness_contrast (GtkWidget *parent, PaintCanvas *canvas);
void paint_dialog_show_hsl (GtkWidget *parent, PaintCanvas *canvas);
void paint_dialog_show_gaussian_blur (GtkWidget *parent, PaintCanvas *canvas);

G_END_DECLS
