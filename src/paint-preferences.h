#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

typedef enum {
  PAINT_COLOR_SCHEME_SYSTEM = 0,
  PAINT_COLOR_SCHEME_LIGHT = 1,
  PAINT_COLOR_SCHEME_DARK = 2,
} PaintColorSchemePref;

typedef struct {
  const char *id;
  const char *section;
  const char *label;
  const char *default_accel;
  const char *extra_accels[3];
} PaintKeybindSpec;

typedef struct {
  PaintColorSchemePref color_scheme;
  int default_width;
  int default_height;
  double default_brush_size;
  gboolean show_cursor_ring;
  gboolean show_pixel_grid;
  gboolean show_toolbar;
  gboolean restore_session;
  GHashTable *keybinds; /* char* action_id -> char* accel ("" = disabled) */
} PaintPreferences;

PaintPreferences *paint_preferences_load (void);
void paint_preferences_save (const PaintPreferences *prefs);
void paint_preferences_free (PaintPreferences *prefs);
void paint_preferences_apply_theme (const PaintPreferences *prefs);

const PaintKeybindSpec *paint_preferences_get_keybind_specs (gsize *out_count);
const PaintKeybindSpec *paint_preferences_find_keybind_spec (const char *id);
const char *paint_preferences_get_keybind (const PaintPreferences *prefs, const char *id);
gboolean paint_preferences_is_keybind_custom (const PaintPreferences *prefs, const char *id);
gboolean paint_preferences_has_custom_keybinds (const PaintPreferences *prefs);
void paint_preferences_set_keybind (PaintPreferences *prefs, const char *id, const char *accel);
void paint_preferences_reset_keybind (PaintPreferences *prefs, const char *id);
void paint_preferences_reset_all_keybinds (PaintPreferences *prefs);
const PaintKeybindSpec *paint_preferences_find_keybind_conflict (const PaintPreferences *prefs,
                                                                 const char *exclude_id,
                                                                 const char *accel);
char *paint_preferences_format_accel_label (const char *accel);
void paint_preferences_apply_accelerators (const PaintPreferences *prefs, GtkApplication *app);
const char *paint_preferences_match_local_keybind (const PaintPreferences *prefs,
                                                   guint keyval,
                                                   GdkModifierType state);

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
