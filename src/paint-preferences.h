#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

typedef enum {
  PAINT_COLOR_SCHEME_SYSTEM = 0,
  PAINT_COLOR_SCHEME_LIGHT = 1,
  PAINT_COLOR_SCHEME_DARK = 2,
} PaintColorSchemePref;

typedef struct {
  PaintColorSchemePref color_scheme;
  int default_width;
  int default_height;
  double default_brush_size;
  gboolean show_cursor_ring;
  gboolean show_pixel_grid;
  gboolean show_toolbar;
  gboolean restore_session;
} PaintPreferences;

PaintPreferences *paint_preferences_load (void);
void paint_preferences_save (const PaintPreferences *prefs);
void paint_preferences_free (PaintPreferences *prefs);
void paint_preferences_apply_theme (const PaintPreferences *prefs);

typedef void (*PaintPreferencesChangedCallback) (const PaintPreferences *prefs,
                                                 gpointer user_data);

GtkWidget *paint_theme_selector_new (PaintPreferences *prefs,
                                     PaintPreferencesChangedCallback on_changed,
                                     gpointer user_data);

void paint_preferences_dialog_present (PaintPreferences *prefs,
                                       GtkWidget *parent,
                                       PaintPreferencesChangedCallback on_changed,
                                       gpointer user_data);

G_END_DECLS
