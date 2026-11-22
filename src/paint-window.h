#pragma once

#include <adwaita.h>

#include "paint-canvas.h"

G_BEGIN_DECLS

#define PAINT_TYPE_WINDOW (paint_window_get_type ())
G_DECLARE_FINAL_TYPE (PaintWindow, paint_window, PAINT, WINDOW, AdwApplicationWindow)

PaintWindow *paint_window_new (AdwApplication *app);
PaintWindow *paint_window_new_empty (AdwApplication *app);
AdwTabView *paint_window_get_tab_view (PaintWindow *self);
AdwTabPage *paint_window_add_new_tab (PaintWindow *self, PaintDocument *doc);
void paint_window_open_file (PaintWindow *self, GFile *file);
void paint_window_apply_preferences (PaintWindow *self);
void paint_window_apply_startup_sizing (PaintWindow *self);
void paint_window_save_window_state (PaintWindow *self);
void paint_window_save_all_sessions (AdwApplication *app);
gboolean paint_window_restore_session (AdwApplication *app);
void paint_window_clear_session (void);

G_END_DECLS
