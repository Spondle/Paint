#include "paint-preferences.h"

#include <math.h>
#include <string.h>

#include "paint-canvas.h"

static const PaintKeybindSpec KEYBIND_SPECS[] = {
  /* Tools */
  { "win.tool-select", "Tools", "Selection Tool", "m", { NULL } },
  { "win.tool-brush", "Tools", "Brush Tool", "b", { NULL } },
  { "win.tool-line", "Tools", "Line Tool", "l", { NULL } },
  { "win.tool-text", "Tools", "Text Tool", "x", { NULL } },
  { "win.tool-eraser", "Tools", "Eraser Tool", "e", { NULL } },
  { "win.tool-fill", "Tools", "Fill Tool", "f", { NULL } },
  { "win.tool-eyedropper", "Tools", "Color Picker", "i", { NULL } },
  { "win.tool-transparent", "Tools", "Transparency Tool", "t", { NULL } },
  { "win.tool-shape", "Tools", "Shapes Tool", "s", { NULL } },
  { "win.brush-smaller", "Tools", "Decrease Brush Size", "bracketleft", { NULL } },
  { "win.brush-larger", "Tools", "Increase Brush Size", "bracketright", { NULL } },

  /* Canvas & Layers */
  { "win.undo", "Canvas & Layers", "Undo", "<Primary>z", { NULL } },
  { "win.redo", "Canvas & Layers", "Redo", "<Primary><Shift>z", { "<Primary>y", NULL } },
  { "win.toggle-layers", "Canvas & Layers", "Toggle Layers Panel", "<Primary>l", { NULL } },
  { "win.new-layer", "Canvas & Layers", "New Layer", "<Primary><Shift>l", { NULL } },
  { "win.duplicate-layer", "Canvas & Layers", "Duplicate Layer", "", { NULL } },
  { "win.merge-layer-down", "Canvas & Layers", "Merge Layer Down", "", { NULL } },
  { "win.flatten-layers", "Canvas & Layers", "Flatten Layers", "", { NULL } },
  { "win.resize-canvas", "Canvas & Layers", "Resize & Scale", "", { NULL } },
  { "win.rotate-cw", "Canvas & Layers", "Rotate Right 90°", "", { NULL } },
  { "win.rotate-ccw", "Canvas & Layers", "Rotate Left 90°", "", { NULL } },
  { "win.flip-horizontal", "Canvas & Layers", "Flip Horizontal", "", { NULL } },
  { "win.flip-vertical", "Canvas & Layers", "Flip Vertical", "", { NULL } },

  /* Selection & Clipboard */
  { "win.select-all", "Selection & Clipboard", "Select All", "<Primary>a", { NULL } },
  { "win.copy", "Selection & Clipboard", "Copy", "<Primary>c", { NULL } },
  { "win.cut", "Selection & Clipboard", "Cut", "<Primary>x", { NULL } },
  { "win.paste", "Selection & Clipboard", "Paste", "<Primary>v", { NULL } },
  { "win.paste-new-tab", "Selection & Clipboard", "Paste as New Canvas", "<Primary><Shift>v", { NULL } },
  { "win.crop-selection", "Selection & Clipboard", "Crop to Selection", "<Primary><Shift>x", { NULL } },

  /* View */
  { "win.zoom-in", "View", "Zoom In", "<Primary>plus", { "<Primary>equal", "<Primary>KP_Add", NULL } },
  { "win.zoom-out", "View", "Zoom Out", "<Primary>minus", { "<Primary>KP_Subtract", NULL } },
  { "win.zoom-fit", "View", "Fit to Window", "<Primary>9", { "<Primary>KP_9", NULL } },
  { "win.zoom-reset", "View", "Reset Zoom", "<Primary>0", { "<Primary>KP_0", NULL } },
  { "win.toggle-toolbar", "View", "Toggle Floating Toolbar", "F9", { NULL } },
  { "win.toggle-fullscreen", "View", "Fullscreen", "F11", { NULL } },

  /* Files & Tabs */
  { "win.new-tab", "Files & Tabs", "New Canvas Tab", "<Primary>n", { "<Primary>t", NULL } },
  { "app.new-window", "Files & Tabs", "New Window", "<Primary><Shift>n", { NULL } },
  { "win.open", "Files & Tabs", "Open Image", "<Primary>o", { NULL } },
  { "win.save", "Files & Tabs", "Save", "<Primary>s", { NULL } },
  { "win.save-as", "Files & Tabs", "Save As", "<Primary><Shift>s", { NULL } },
  { "win.close-tab", "Files & Tabs", "Close Tab", "<Primary>w", { NULL } },
  { "win.tab-overview", "Files & Tabs", "Tab Overview", "<Primary><Shift>o", { NULL } },
  { "app.preferences", "Files & Tabs", "Preferences", "<Primary>comma", { NULL } },
  { "app.shortcuts", "Files & Tabs", "Keyboard Shortcuts", "<Primary>question", { NULL } },
  { "app.quit", "Files & Tabs", "Quit", "<Primary>q", { NULL } },
};

static char *
preferences_file_path (void)
{
  return g_build_filename (g_get_user_config_dir (), "paint", "settings.ini", NULL);
}

static gboolean
accels_equal (const char *a, const char *b)
{
  gboolean a_empty = (!a || !*a);
  gboolean b_empty = (!b || !*b);
  if (a_empty || b_empty)
    return a_empty && b_empty;

  guint ka = 0, kb = 0;
  GdkModifierType ma = 0, mb = 0;
  if (gtk_accelerator_parse (a, &ka, &ma) && gtk_accelerator_parse (b, &kb, &mb))
    return gdk_keyval_to_lower (ka) == gdk_keyval_to_lower (kb) && ma == mb;
  return g_strcmp0 (a, b) == 0;
}

static gboolean
is_global_accel (const char *accel)
{
  if (!accel || !*accel)
    return FALSE;
  guint kv = 0;
  GdkModifierType mods = 0;
  if (!gtk_accelerator_parse (accel, &kv, &mods))
    return FALSE;
  if ((mods & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK | GDK_META_MASK)) != 0)
    return TRUE;
  return kv >= GDK_KEY_F1 && kv <= GDK_KEY_F35;
}

const PaintKeybindSpec *
paint_preferences_get_keybind_specs (gsize *out_count)
{
  if (out_count)
    *out_count = G_N_ELEMENTS (KEYBIND_SPECS);
  return KEYBIND_SPECS;
}

const PaintKeybindSpec *
paint_preferences_find_keybind_spec (const char *id)
{
  if (!id)
    return NULL;
  for (gsize i = 0; i < G_N_ELEMENTS (KEYBIND_SPECS); i++) {
    if (g_strcmp0 (KEYBIND_SPECS[i].id, id) == 0)
      return &KEYBIND_SPECS[i];
  }
  return NULL;
}

const char *
paint_preferences_get_keybind (const PaintPreferences *prefs, const char *id)
{
  if (prefs && prefs->keybinds && id) {
    const char *override = g_hash_table_lookup (prefs->keybinds, id);
    if (override)
      return override;
  }
  const PaintKeybindSpec *spec = paint_preferences_find_keybind_spec (id);
  return spec ? spec->default_accel : "";
}

gboolean
paint_preferences_is_keybind_custom (const PaintPreferences *prefs, const char *id)
{
  return prefs && prefs->keybinds && id && g_hash_table_contains (prefs->keybinds, id);
}

gboolean
paint_preferences_has_custom_keybinds (const PaintPreferences *prefs)
{
  return prefs && prefs->keybinds && g_hash_table_size (prefs->keybinds) > 0;
}

void
paint_preferences_set_keybind (PaintPreferences *prefs, const char *id, const char *accel)
{
  if (!prefs || !prefs->keybinds || !id)
    return;
  const PaintKeybindSpec *spec = paint_preferences_find_keybind_spec (id);
  if (!spec)
    return;

  const char *val = accel ? accel : "";
  g_autofree char *norm = NULL;
  if (*val) {
    guint kv = 0;
    GdkModifierType mods = 0;
    if (!gtk_accelerator_parse (val, &kv, &mods))
      return;
    norm = gtk_accelerator_name (gdk_keyval_to_lower (kv), mods);
  } else {
    norm = g_strdup ("");
  }

  if (accels_equal (norm, spec->default_accel))
    g_hash_table_remove (prefs->keybinds, id);
  else
    g_hash_table_insert (prefs->keybinds, g_strdup (id), g_steal_pointer (&norm));
}

void
paint_preferences_reset_keybind (PaintPreferences *prefs, const char *id)
{
  if (!prefs || !prefs->keybinds || !id)
    return;
  g_hash_table_remove (prefs->keybinds, id);
}

void
paint_preferences_reset_all_keybinds (PaintPreferences *prefs)
{
  if (!prefs || !prefs->keybinds)
    return;
  g_hash_table_remove_all (prefs->keybinds);
}

const PaintKeybindSpec *
paint_preferences_find_keybind_conflict (const PaintPreferences *prefs,
                                         const char *exclude_id,
                                         const char *accel)
{
  if (!accel || !*accel)
    return NULL;
  for (gsize i = 0; i < G_N_ELEMENTS (KEYBIND_SPECS); i++) {
    const PaintKeybindSpec *spec = &KEYBIND_SPECS[i];
    if (exclude_id && g_strcmp0 (spec->id, exclude_id) == 0)
      continue;
    const char *cur = paint_preferences_get_keybind (prefs, spec->id);
    if (cur && *cur && accels_equal (cur, accel))
      return spec;
  }
  return NULL;
}

char *
paint_preferences_format_accel_label (const char *accel)
{
  if (!accel || !*accel)
    return g_strdup ("Disabled");
  guint kv = 0;
  GdkModifierType mods = 0;
  if (!gtk_accelerator_parse (accel, &kv, &mods))
    return g_strdup ("Disabled");
  return gtk_accelerator_get_label (kv, mods);
}

void
paint_preferences_apply_accelerators (const PaintPreferences *prefs, GtkApplication *app)
{
  if (!app)
    return;
  for (gsize i = 0; i < G_N_ELEMENTS (KEYBIND_SPECS); i++) {
    const PaintKeybindSpec *spec = &KEYBIND_SPECS[i];
    const char *accel = paint_preferences_get_keybind (prefs, spec->id);
    if (accel && *accel && is_global_accel (accel)) {
      const char *accels[5] = { accel, NULL, NULL, NULL, NULL };
      if (!paint_preferences_is_keybind_custom (prefs, spec->id)) {
        for (int j = 0; j < 3 && spec->extra_accels[j]; j++) {
          if (!paint_preferences_find_keybind_conflict (prefs, spec->id, spec->extra_accels[j]))
            accels[1 + j] = spec->extra_accels[j];
        }
      }
      gtk_application_set_accels_for_action (app, spec->id, accels);
    } else {
      const char *none[1] = { NULL };
      gtk_application_set_accels_for_action (app, spec->id, none);
    }
  }
}

const char *
paint_preferences_match_local_keybind (const PaintPreferences *prefs,
                                       guint keyval,
                                       GdkModifierType state)
{
  GdkModifierType mods = state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK | GDK_SHIFT_MASK);
  if ((mods & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) != 0)
    return NULL;

  for (gsize i = 0; i < G_N_ELEMENTS (KEYBIND_SPECS); i++) {
    const PaintKeybindSpec *spec = &KEYBIND_SPECS[i];
    const char *accel = paint_preferences_get_keybind (prefs, spec->id);
    if (!accel || !*accel || is_global_accel (accel))
      continue;

    guint b_key = 0;
    GdkModifierType b_mods = 0;
    if (!gtk_accelerator_parse (accel, &b_key, &b_mods))
      continue;

    if (gdk_keyval_to_lower (keyval) == gdk_keyval_to_lower (b_key) && mods == b_mods)
      return spec->id;

    if (gdk_keyval_to_lower (b_key) == gdk_keyval_to_upper (b_key) &&
        b_mods == 0 && mods == GDK_SHIFT_MASK && keyval == b_key) {
      return spec->id;
    }
  }
  return NULL;
}

PaintPreferences *
paint_preferences_load (void)
{
  PaintPreferences *prefs = g_new0 (PaintPreferences, 1);
  prefs->color_scheme = PAINT_COLOR_SCHEME_SYSTEM;
  prefs->default_width = PAINT_DEFAULT_CANVAS_WIDTH;
  prefs->default_height = PAINT_DEFAULT_CANVAS_HEIGHT;
  prefs->default_brush_size = 6.0;
  prefs->show_cursor_ring = TRUE;
  prefs->show_pixel_grid = TRUE;
  prefs->show_toolbar = TRUE;
  prefs->restore_session = TRUE;
  prefs->keybinds = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  g_autofree char *path = preferences_file_path ();
  g_autoptr (GKeyFile) kf = g_key_file_new ();
  if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, NULL))
    return prefs;

  if (g_key_file_has_key (kf, "Appearance", "ColorScheme", NULL)) {
    int scheme = g_key_file_get_integer (kf, "Appearance", "ColorScheme", NULL);
    prefs->color_scheme = CLAMP (scheme, PAINT_COLOR_SCHEME_SYSTEM, PAINT_COLOR_SCHEME_DARK);
  }
  if (g_key_file_has_key (kf, "Appearance", "ShowCursorRing", NULL))
    prefs->show_cursor_ring = g_key_file_get_boolean (kf, "Appearance", "ShowCursorRing", NULL);
  if (g_key_file_has_key (kf, "Appearance", "ShowPixelGrid", NULL))
    prefs->show_pixel_grid = g_key_file_get_boolean (kf, "Appearance", "ShowPixelGrid", NULL);
  if (g_key_file_has_key (kf, "Appearance", "ShowToolbar", NULL))
    prefs->show_toolbar = g_key_file_get_boolean (kf, "Appearance", "ShowToolbar", NULL);
  if (g_key_file_has_key (kf, "Behavior", "RestoreSession", NULL))
    prefs->restore_session = g_key_file_get_boolean (kf, "Behavior", "RestoreSession", NULL);

  if (g_key_file_has_key (kf, "Canvas", "DefaultWidth", NULL))
    prefs->default_width = CLAMP (g_key_file_get_integer (kf, "Canvas", "DefaultWidth", NULL), 64, 8192);
  if (g_key_file_has_key (kf, "Canvas", "DefaultHeight", NULL))
    prefs->default_height = CLAMP (g_key_file_get_integer (kf, "Canvas", "DefaultHeight", NULL), 64, 8192);
  if (g_key_file_has_key (kf, "Canvas", "DefaultBrushSize", NULL))
    prefs->default_brush_size = CLAMP (g_key_file_get_double (kf, "Canvas", "DefaultBrushSize", NULL), 1.0, 96.0);

  if (g_key_file_has_group (kf, "Shortcuts")) {
    gsize n_keys = 0;
    g_auto (GStrv) keys = g_key_file_get_keys (kf, "Shortcuts", &n_keys, NULL);
    for (gsize i = 0; keys && i < n_keys; i++) {
      if (!paint_preferences_find_keybind_spec (keys[i]))
        continue;
      g_autofree char *val = g_key_file_get_string (kf, "Shortcuts", keys[i], NULL);
      if (val) {
        guint kv = 0;
        GdkModifierType mods = 0;
        if (*val == '\0' || gtk_accelerator_parse (val, &kv, &mods))
          paint_preferences_set_keybind (prefs, keys[i], val);
      }
    }
  }

  return prefs;
}

void
paint_preferences_save (const PaintPreferences *prefs)
{
  if (!prefs)
    return;

  g_autofree char *path = preferences_file_path ();
  g_autofree char *dir = g_path_get_dirname (path);
  g_mkdir_with_parents (dir, 0755);

  g_autoptr (GKeyFile) kf = g_key_file_new ();
  g_key_file_set_integer (kf, "Appearance", "ColorScheme", prefs->color_scheme);
  g_key_file_set_boolean (kf, "Appearance", "ShowCursorRing", prefs->show_cursor_ring);
  g_key_file_set_boolean (kf, "Appearance", "ShowPixelGrid", prefs->show_pixel_grid);
  g_key_file_set_boolean (kf, "Appearance", "ShowToolbar", prefs->show_toolbar);
  g_key_file_set_boolean (kf, "Behavior", "RestoreSession", prefs->restore_session);
  g_key_file_set_integer (kf, "Canvas", "DefaultWidth", prefs->default_width);
  g_key_file_set_integer (kf, "Canvas", "DefaultHeight", prefs->default_height);
  g_key_file_set_double (kf, "Canvas", "DefaultBrushSize", prefs->default_brush_size);

  for (gsize i = 0; i < G_N_ELEMENTS (KEYBIND_SPECS); i++) {
    const char *id = KEYBIND_SPECS[i].id;
    if (paint_preferences_is_keybind_custom (prefs, id))
      g_key_file_set_string (kf, "Shortcuts", id, paint_preferences_get_keybind (prefs, id));
  }

  g_key_file_save_to_file (kf, path, NULL);
}

void
paint_preferences_free (PaintPreferences *prefs)
{
  if (!prefs)
    return;
  g_clear_pointer (&prefs->keybinds, g_hash_table_unref);
  g_free (prefs);
}

void
paint_preferences_apply_theme (const PaintPreferences *prefs)
{
  AdwStyleManager *style = adw_style_manager_get_default ();
  if (!style || !prefs)
    return;

  switch (prefs->color_scheme) {
  case PAINT_COLOR_SCHEME_LIGHT:
    adw_style_manager_set_color_scheme (style, ADW_COLOR_SCHEME_FORCE_LIGHT);
    break;
  case PAINT_COLOR_SCHEME_DARK:
    adw_style_manager_set_color_scheme (style, ADW_COLOR_SCHEME_FORCE_DARK);
    break;
  case PAINT_COLOR_SCHEME_SYSTEM:
  default:
    adw_style_manager_set_color_scheme (style, ADW_COLOR_SCHEME_DEFAULT);
    break;
  }
}

typedef struct {
  PaintPreferences *prefs;
  PaintPreferencesChangedCallback on_changed;
  gpointer user_data;
  GtkToggleButton *buttons[3];
  GtkWidget *badges[3];
  gboolean syncing;
} ThemeSelectorCtx;

static void
sync_theme_selector (ThemeSelectorCtx *ctx)
{
  if (!ctx || !ctx->prefs)
    return;
  ctx->syncing = TRUE;
  for (int i = 0; i < 3; i++) {
    gboolean active = ((int) ctx->prefs->color_scheme == i);
    gtk_toggle_button_set_active (ctx->buttons[i], active);
    gtk_widget_set_visible (ctx->badges[i], active);
  }
  ctx->syncing = FALSE;
}

static void
on_style_scheme_external_change (GtkWidget *box)
{
  ThemeSelectorCtx *ctx = g_object_get_data (G_OBJECT (box), "theme-selector-ctx");
  sync_theme_selector (ctx);
}

static void
on_theme_circle_toggled (GtkToggleButton *btn, gpointer user_data)
{
  ThemeSelectorCtx *ctx = user_data;
  if (ctx->syncing || !gtk_toggle_button_get_active (btn))
    return;

  int idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (btn), "paint-scheme-idx"));
  ctx->prefs->color_scheme = (PaintColorSchemePref) CLAMP (idx, 0, 2);
  paint_preferences_save (ctx->prefs);
  paint_preferences_apply_theme (ctx->prefs);
  sync_theme_selector (ctx);
  if (ctx->on_changed)
    ctx->on_changed (ctx->prefs, ctx->user_data);
}

GtkWidget *
paint_theme_selector_new (PaintPreferences *prefs,
                          PaintPreferencesChangedCallback on_changed,
                          gpointer user_data)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 14);
  gtk_widget_add_css_class (box, "paint-theme-selector");
  gtk_widget_set_halign (box, GTK_ALIGN_CENTER);

  ThemeSelectorCtx *ctx = g_new0 (ThemeSelectorCtx, 1);
  ctx->prefs = prefs;
  ctx->on_changed = on_changed;
  ctx->user_data = user_data;
  g_object_set_data_full (G_OBJECT (box), "theme-selector-ctx", ctx, g_free);

  static const struct {
    const char *css_class;
    const char *tooltip;
  } items[3] = {
    { "follow", "Follow System" },
    { "light", "Light Style" },
    { "dark", "Dark Style" },
  };

  GtkToggleButton *group_head = NULL;
  for (int i = 0; i < 3; i++) {
    GtkWidget *overlay = gtk_overlay_new ();

    GtkWidget *btn = gtk_toggle_button_new ();
    gtk_widget_add_css_class (btn, "paint-theme-circle");
    gtk_widget_add_css_class (btn, items[i].css_class);
    gtk_widget_set_tooltip_text (btn, items[i].tooltip);
    if (!group_head)
      group_head = GTK_TOGGLE_BUTTON (btn);
    else
      gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (btn), group_head);

    g_object_set_data (G_OBJECT (btn), "paint-scheme-idx", GINT_TO_POINTER (i));
    g_signal_connect (btn, "toggled", G_CALLBACK (on_theme_circle_toggled), ctx);
    gtk_overlay_set_child (GTK_OVERLAY (overlay), btn);

    GtkWidget *badge = gtk_image_new_from_icon_name ("object-select-symbolic");
    gtk_image_set_pixel_size (GTK_IMAGE (badge), 11);
    gtk_widget_add_css_class (badge, "paint-theme-check");
    gtk_widget_set_halign (badge, GTK_ALIGN_END);
    gtk_widget_set_valign (badge, GTK_ALIGN_END);
    gtk_widget_set_can_target (badge, FALSE);
    gtk_overlay_add_overlay (GTK_OVERLAY (overlay), badge);

    ctx->buttons[i] = GTK_TOGGLE_BUTTON (btn);
    ctx->badges[i] = badge;
    gtk_box_append (GTK_BOX (box), overlay);
  }

  sync_theme_selector (ctx);

  AdwStyleManager *style = adw_style_manager_get_default ();
  if (style) {
    g_signal_connect_object (style,
                             "notify::color-scheme",
                             G_CALLBACK (on_style_scheme_external_change),
                             box,
                             G_CONNECT_SWAPPED);
  }

  return box;
}

typedef struct {
  const PaintKeybindSpec *spec;
  GtkWidget *accel_label;
  GtkWidget *reset_btn;
} ShortcutRowRef;

typedef struct {
  PaintPreferences *prefs;
  PaintPreferencesChangedCallback on_changed;
  gpointer user_data;
  AdwDialog *prefs_dialog;
  GPtrArray *rows; /* element-type: ShortcutRowRef* */
  GtkWidget *reset_all_btn;

  AdwDialog *capture_dialog;
  const PaintKeybindSpec *capture_spec;
  GtkWidget *capture_preview_label;
  GtkWidget *capture_conflict_label;
  GtkWidget *capture_set_btn;
  char *pending_accel;
  const PaintKeybindSpec *pending_conflict;
  gboolean has_pending;
} PrefsDialogCtx;

static void
prefs_dialog_ctx_free (gpointer data)
{
  PrefsDialogCtx *ctx = data;
  if (!ctx)
    return;
  g_clear_pointer (&ctx->rows, g_ptr_array_unref);
  g_free (ctx->pending_accel);
  g_free (ctx);
}

static void
notify_changed (PrefsDialogCtx *ctx)
{
  paint_preferences_save (ctx->prefs);
  paint_preferences_apply_theme (ctx->prefs);
  if (ctx->on_changed)
    ctx->on_changed (ctx->prefs, ctx->user_data);
}

static void
refresh_shortcut_rows (PrefsDialogCtx *ctx)
{
  if (!ctx || !ctx->rows)
    return;
  for (guint i = 0; i < ctx->rows->len; i++) {
    ShortcutRowRef *ref = g_ptr_array_index (ctx->rows, i);
    const char *accel = paint_preferences_get_keybind (ctx->prefs, ref->spec->id);
    gboolean disabled = (!accel || !*accel);
    g_autofree char *lbl = paint_preferences_format_accel_label (accel);
    gtk_label_set_text (GTK_LABEL (ref->accel_label), lbl);
    if (disabled) {
      gtk_widget_remove_css_class (ref->accel_label, "paint-keycap-label");
      gtk_widget_add_css_class (ref->accel_label, "dim-label");
    } else {
      gtk_widget_remove_css_class (ref->accel_label, "dim-label");
      gtk_widget_add_css_class (ref->accel_label, "paint-keycap-label");
    }
    gtk_widget_set_visible (ref->reset_btn,
                            paint_preferences_is_keybind_custom (ctx->prefs, ref->spec->id));
  }
  if (ctx->reset_all_btn)
    gtk_widget_set_sensitive (ctx->reset_all_btn,
                              paint_preferences_has_custom_keybinds (ctx->prefs));
}

static void
on_shortcut_reset_one_clicked (GtkButton *btn, gpointer user_data)
{
  PrefsDialogCtx *ctx = user_data;
  const char *id = g_object_get_data (G_OBJECT (btn), "paint-keybind-id");
  paint_preferences_reset_keybind (ctx->prefs, id);
  notify_changed (ctx);
  refresh_shortcut_rows (ctx);
}

static void
on_shortcuts_reset_all_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  PrefsDialogCtx *ctx = user_data;
  paint_preferences_reset_all_keybinds (ctx->prefs);
  notify_changed (ctx);
  refresh_shortcut_rows (ctx);
}

static void
close_capture_dialog (PrefsDialogCtx *ctx)
{
  AdwDialog *dlg = ctx->capture_dialog;
  ctx->capture_dialog = NULL;
  if (dlg)
    adw_dialog_close (dlg);
}

static void
on_capture_dialog_closed (AdwDialog *dlg G_GNUC_UNUSED, gpointer user_data)
{
  PrefsDialogCtx *ctx = user_data;
  ctx->capture_dialog = NULL;
  ctx->capture_spec = NULL;
  g_clear_pointer (&ctx->pending_accel, g_free);
  ctx->pending_conflict = NULL;
  ctx->has_pending = FALSE;
}

static void
update_capture_preview (PrefsDialogCtx *ctx, const char *accel)
{
  g_free (ctx->pending_accel);
  ctx->pending_accel = g_strdup (accel ? accel : "");
  ctx->has_pending = TRUE;

  g_autofree char *lbl = paint_preferences_format_accel_label (ctx->pending_accel);
  gtk_widget_remove_css_class (ctx->capture_preview_label, "dim-label");
  gtk_label_set_text (GTK_LABEL (ctx->capture_preview_label), lbl);

  ctx->pending_conflict =
      (*ctx->pending_accel)
        ? paint_preferences_find_keybind_conflict (ctx->prefs,
                                                   ctx->capture_spec->id,
                                                   ctx->pending_accel)
        : NULL;
  if (ctx->pending_conflict) {
    g_autofree char *warn = g_strdup_printf (
        "Already used by “%s” — setting will reassign it",
        ctx->pending_conflict->label);
    gtk_label_set_text (GTK_LABEL (ctx->capture_conflict_label), warn);
    gtk_widget_set_visible (ctx->capture_conflict_label, TRUE);
  } else {
    gtk_widget_set_visible (ctx->capture_conflict_label, FALSE);
  }

  gtk_widget_set_sensitive (ctx->capture_set_btn, TRUE);
}

static void
on_capture_set_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  PrefsDialogCtx *ctx = user_data;
  if (!ctx->has_pending || !ctx->capture_spec)
    return;

  if (ctx->pending_accel && *ctx->pending_accel && ctx->pending_conflict)
    paint_preferences_set_keybind (ctx->prefs, ctx->pending_conflict->id, "");
  paint_preferences_set_keybind (ctx->prefs, ctx->capture_spec->id, ctx->pending_accel);
  close_capture_dialog (ctx);
  notify_changed (ctx);
  refresh_shortcut_rows (ctx);
}

static void
on_capture_clear_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  PrefsDialogCtx *ctx = user_data;
  if (!ctx->capture_spec)
    return;
  paint_preferences_set_keybind (ctx->prefs, ctx->capture_spec->id, "");
  close_capture_dialog (ctx);
  notify_changed (ctx);
  refresh_shortcut_rows (ctx);
}

static void
on_capture_reset_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
  PrefsDialogCtx *ctx = user_data;
  if (!ctx->capture_spec)
    return;
  paint_preferences_reset_keybind (ctx->prefs, ctx->capture_spec->id);
  close_capture_dialog (ctx);
  notify_changed (ctx);
  refresh_shortcut_rows (ctx);
}

static gboolean
on_capture_key_pressed (GtkEventControllerKey *ctrl,
                        guint keyval,
                        guint keycode G_GNUC_UNUSED,
                        GdkModifierType state,
                        gpointer user_data)
{
  PrefsDialogCtx *ctx = user_data;
  GdkEvent *ev = gtk_event_controller_get_current_event (GTK_EVENT_CONTROLLER (ctrl));
  if (ev && gdk_key_event_is_modifier (ev))
    return FALSE;

  GdkModifierType mods = state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK | GDK_SHIFT_MASK);

  if (mods == 0) {
    if (keyval == GDK_KEY_Escape) {
      close_capture_dialog (ctx);
      return TRUE;
    }
    if (keyval == GDK_KEY_BackSpace) {
      update_capture_preview (ctx, "");
      return TRUE;
    }
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
      if (ctx->has_pending)
        on_capture_set_clicked (NULL, ctx);
      return TRUE;
    }
    if (keyval == GDK_KEY_space)
      return TRUE;
  }

  guint lower = gdk_keyval_to_lower (keyval);
  guint upper = gdk_keyval_to_upper (keyval);
  gboolean is_letter = (lower != upper);
  guint norm_key = is_letter ? lower : keyval;
  if (!is_letter && mods == GDK_SHIFT_MASK)
    mods = 0;

  g_autofree char *accel = gtk_accelerator_name (norm_key, mods);
  if (accel && *accel)
    update_capture_preview (ctx, accel);
  return TRUE;
}

static void
open_shortcut_capture_dialog (PrefsDialogCtx *ctx, const PaintKeybindSpec *spec)
{
  if (ctx->capture_dialog || !spec)
    return;

  ctx->capture_spec = spec;
  ctx->has_pending = FALSE;
  ctx->pending_conflict = NULL;
  g_clear_pointer (&ctx->pending_accel, g_free);

  AdwDialog *dlg = adw_dialog_new ();
  adw_dialog_set_title (dlg, "Set Shortcut");
  adw_dialog_set_content_width (dlg, 360);
  ctx->capture_dialog = dlg;

  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_show_start_title_buttons (ADW_HEADER_BAR (header), FALSE);
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);

  GtkWidget *cancel_btn = gtk_button_new_with_label ("Cancel");
  g_signal_connect_swapped (cancel_btn, "clicked", G_CALLBACK (close_capture_dialog), ctx);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), cancel_btn);

  ctx->capture_set_btn = gtk_button_new_with_label ("Set");
  gtk_widget_add_css_class (ctx->capture_set_btn, "suggested-action");
  gtk_widget_set_sensitive (ctx->capture_set_btn, FALSE);
  g_signal_connect (ctx->capture_set_btn, "clicked", G_CALLBACK (on_capture_set_clicked), ctx);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), ctx->capture_set_btn);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top (content, 18);
  gtk_widget_set_margin_bottom (content, 18);
  gtk_widget_set_margin_start (content, 18);
  gtk_widget_set_margin_end (content, 18);

  g_autofree char *prompt = g_strdup_printf ("Press a new shortcut for “%s”", spec->label);
  GtkWidget *prompt_lbl = gtk_label_new (prompt);
  gtk_label_set_wrap (GTK_LABEL (prompt_lbl), TRUE);
  gtk_label_set_justify (GTK_LABEL (prompt_lbl), GTK_JUSTIFY_CENTER);
  gtk_box_append (GTK_BOX (content), prompt_lbl);

  ctx->capture_preview_label = gtk_label_new ("Press keys…");
  gtk_widget_add_css_class (ctx->capture_preview_label, "paint-keycap-preview");
  gtk_widget_add_css_class (ctx->capture_preview_label, "dim-label");
  gtk_widget_set_halign (ctx->capture_preview_label, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (content), ctx->capture_preview_label);

  ctx->capture_conflict_label = gtk_label_new ("");
  gtk_widget_add_css_class (ctx->capture_conflict_label, "warning");
  gtk_widget_add_css_class (ctx->capture_conflict_label, "caption");
  gtk_label_set_wrap (GTK_LABEL (ctx->capture_conflict_label), TRUE);
  gtk_label_set_justify (GTK_LABEL (ctx->capture_conflict_label), GTK_JUSTIFY_CENTER);
  gtk_widget_set_visible (ctx->capture_conflict_label, FALSE);
  gtk_box_append (GTK_BOX (content), ctx->capture_conflict_label);

  GtkWidget *hint_lbl = gtk_label_new ("Backspace to disable · Escape to cancel");
  gtk_widget_add_css_class (hint_lbl, "paint-dim-label");
  gtk_widget_add_css_class (hint_lbl, "caption");
  gtk_widget_set_halign (hint_lbl, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (content), hint_lbl);

  GtkWidget *btn_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (btn_row, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (btn_row, 4);

  GtkWidget *clear_btn = gtk_button_new_with_label ("Clear");
  g_signal_connect (clear_btn, "clicked", G_CALLBACK (on_capture_clear_clicked), ctx);
  gtk_box_append (GTK_BOX (btn_row), clear_btn);

  GtkWidget *reset_btn = gtk_button_new_with_label ("Reset Default");
  g_signal_connect (reset_btn, "clicked", G_CALLBACK (on_capture_reset_clicked), ctx);
  gtk_box_append (GTK_BOX (btn_row), reset_btn);
  gtk_box_append (GTK_BOX (content), btn_row);

  GtkEventController *key_ctrl = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (key_ctrl, GTK_PHASE_CAPTURE);
  g_signal_connect (key_ctrl, "key-pressed", G_CALLBACK (on_capture_key_pressed), ctx);
  gtk_widget_add_controller (GTK_WIDGET (dlg), key_ctrl);

  g_signal_connect (dlg, "closed", G_CALLBACK (on_capture_dialog_closed), ctx);

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), content);
  adw_dialog_set_child (dlg, toolbar);
  adw_dialog_present (dlg, GTK_WIDGET (ctx->prefs_dialog));
}

static void
on_shortcut_row_activated (AdwActionRow *row, gpointer user_data)
{
  PrefsDialogCtx *ctx = user_data;
  const PaintKeybindSpec *spec = g_object_get_data (G_OBJECT (row), "paint-keybind-spec");
  open_shortcut_capture_dialog (ctx, spec);
}

static GtkWidget *
build_shortcuts_page (PrefsDialogCtx *ctx)
{
  GtkWidget *page = adw_preferences_page_new ();
  adw_preferences_page_set_title (ADW_PREFERENCES_PAGE (page), "Shortcuts");
  adw_preferences_page_set_icon_name (ADW_PREFERENCES_PAGE (page), "input-keyboard-symbolic");

  gsize n_specs = 0;
  const PaintKeybindSpec *specs = paint_preferences_get_keybind_specs (&n_specs);
  GtkWidget *cur_grp = NULL;
  const char *cur_sec = NULL;

  for (gsize i = 0; i < n_specs; i++) {
    const PaintKeybindSpec *spec = &specs[i];
    if (!cur_grp || g_strcmp0 (cur_sec, spec->section) != 0) {
      cur_sec = spec->section;
      cur_grp = adw_preferences_group_new ();
      adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (cur_grp), cur_sec);
      if (i == 0) {
        ctx->reset_all_btn = gtk_button_new_with_label ("Reset All");
        gtk_widget_add_css_class (ctx->reset_all_btn, "flat");
        g_signal_connect (ctx->reset_all_btn, "clicked",
                          G_CALLBACK (on_shortcuts_reset_all_clicked), ctx);
        adw_preferences_group_set_header_suffix (ADW_PREFERENCES_GROUP (cur_grp),
                                                 ctx->reset_all_btn);
      }
      adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (cur_grp));
    }

    GtkWidget *row = adw_action_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), spec->label);
    gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
    g_object_set_data (G_OBJECT (row), "paint-keybind-spec", (gpointer) spec);
    g_signal_connect (row, "activated", G_CALLBACK (on_shortcut_row_activated), ctx);

    GtkWidget *accel_lbl = gtk_label_new ("");
    gtk_widget_set_valign (accel_lbl, GTK_ALIGN_CENTER);
    adw_action_row_add_suffix (ADW_ACTION_ROW (row), accel_lbl);

    GtkWidget *reset_btn = gtk_button_new_from_icon_name ("edit-undo-symbolic");
    gtk_widget_add_css_class (reset_btn, "flat");
    gtk_widget_set_valign (reset_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text (reset_btn, "Reset to Default");
    g_object_set_data (G_OBJECT (reset_btn), "paint-keybind-id", (gpointer) spec->id);
    g_signal_connect (reset_btn, "clicked", G_CALLBACK (on_shortcut_reset_one_clicked), ctx);
    adw_action_row_add_suffix (ADW_ACTION_ROW (row), reset_btn);

    adw_preferences_group_add (ADW_PREFERENCES_GROUP (cur_grp), row);

    ShortcutRowRef *ref = g_new0 (ShortcutRowRef, 1);
    ref->spec = spec;
    ref->accel_label = accel_lbl;
    ref->reset_btn = reset_btn;
    g_ptr_array_add (ctx->rows, ref);
  }

  refresh_shortcut_rows (ctx);
  return page;
}

static void
on_cursor_ring_toggled (GObject *obj, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
  PrefsDialogCtx *ctx = user_data;
  ctx->prefs->show_cursor_ring = adw_switch_row_get_active (ADW_SWITCH_ROW (obj));
  notify_changed (ctx);
}

static void
on_pixel_grid_toggled (GObject *obj, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
  PrefsDialogCtx *ctx = user_data;
  ctx->prefs->show_pixel_grid = adw_switch_row_get_active (ADW_SWITCH_ROW (obj));
  notify_changed (ctx);
}

static void
on_toolbar_toggled (GObject *obj, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
  PrefsDialogCtx *ctx = user_data;
  ctx->prefs->show_toolbar = adw_switch_row_get_active (ADW_SWITCH_ROW (obj));
  notify_changed (ctx);
}

static void
on_restore_session_toggled (GObject *obj, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
  PrefsDialogCtx *ctx = user_data;
  ctx->prefs->restore_session = adw_switch_row_get_active (ADW_SWITCH_ROW (obj));
  notify_changed (ctx);
}

static void
on_default_width_changed (GObject *obj, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
  PrefsDialogCtx *ctx = user_data;
  ctx->prefs->default_width = (int) lround (adw_spin_row_get_value (ADW_SPIN_ROW (obj)));
  notify_changed (ctx);
}

static void
on_default_height_changed (GObject *obj, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
  PrefsDialogCtx *ctx = user_data;
  ctx->prefs->default_height = (int) lround (adw_spin_row_get_value (ADW_SPIN_ROW (obj)));
  notify_changed (ctx);
}

static void
on_default_brush_changed (GObject *obj, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
  PrefsDialogCtx *ctx = user_data;
  ctx->prefs->default_brush_size = adw_spin_row_get_value (ADW_SPIN_ROW (obj));
  notify_changed (ctx);
}

void
paint_preferences_dialog_present (PaintPreferences *prefs,
                                  GtkWidget *parent,
                                  PaintPreferencesChangedCallback on_changed,
                                  gpointer user_data)
{
  AdwDialog *dlg = adw_preferences_dialog_new ();
  adw_dialog_set_title (dlg, "Preferences");
  adw_preferences_dialog_set_search_enabled (ADW_PREFERENCES_DIALOG (dlg), TRUE);

  PrefsDialogCtx *ctx = g_new0 (PrefsDialogCtx, 1);
  ctx->prefs = prefs;
  ctx->on_changed = on_changed;
  ctx->user_data = user_data;
  ctx->prefs_dialog = dlg;
  ctx->rows = g_ptr_array_new_with_free_func (g_free);
  g_object_set_data_full (G_OBJECT (dlg), "prefs-ctx", ctx, prefs_dialog_ctx_free);

  GtkWidget *page = adw_preferences_page_new ();
  adw_preferences_page_set_title (ADW_PREFERENCES_PAGE (page), "General");
  adw_preferences_page_set_icon_name (ADW_PREFERENCES_PAGE (page), "applications-graphics-symbolic");

  /* Appearance group */
  GtkWidget *appearance_grp = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (appearance_grp), "Appearance");

  GtkWidget *cursor_row = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (cursor_row), "Brush Cursor Ring");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (cursor_row),
                               "Show live brush size outline while hovering");
  adw_switch_row_set_active (ADW_SWITCH_ROW (cursor_row), prefs->show_cursor_ring);
  g_signal_connect (cursor_row, "notify::active", G_CALLBACK (on_cursor_ring_toggled), ctx);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (appearance_grp), cursor_row);

  GtkWidget *grid_row = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (grid_row), "High-Zoom Pixel Grid");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (grid_row),
                               "Show subtle pixel grid at 800% zoom or higher");
  adw_switch_row_set_active (ADW_SWITCH_ROW (grid_row), prefs->show_pixel_grid);
  g_signal_connect (grid_row, "notify::active", G_CALLBACK (on_pixel_grid_toggled), ctx);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (appearance_grp), grid_row);

  GtkWidget *toolbar_row = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (toolbar_row), "Floating Toolbar");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (toolbar_row),
                               "Show bottom floating tool controls on the canvas");
  adw_switch_row_set_active (ADW_SWITCH_ROW (toolbar_row), prefs->show_toolbar);
  g_signal_connect (toolbar_row, "notify::active", G_CALLBACK (on_toolbar_toggled), ctx);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (appearance_grp), toolbar_row);

  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (appearance_grp));

  /* Behavior group */
  GtkWidget *behavior_grp = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (behavior_grp), "Behavior");

  GtkWidget *restore_row = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (restore_row), "Restore Session");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (restore_row),
                               "Reopen open canvases and unsaved drafts on startup");
  adw_switch_row_set_active (ADW_SWITCH_ROW (restore_row), prefs->restore_session);
  g_signal_connect (restore_row, "notify::active", G_CALLBACK (on_restore_session_toggled), ctx);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (behavior_grp), restore_row);

  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (behavior_grp));

  /* New Canvas Defaults group */
  GtkWidget *canvas_grp = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (canvas_grp), "New Canvas Defaults");

  GtkWidget *w_row = adw_spin_row_new_with_range (64.0, 8192.0, 16.0);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (w_row), "Default Width (px)");
  adw_spin_row_set_value (ADW_SPIN_ROW (w_row), (double) prefs->default_width);
  g_signal_connect (w_row, "notify::value", G_CALLBACK (on_default_width_changed), ctx);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (canvas_grp), w_row);

  GtkWidget *h_row = adw_spin_row_new_with_range (64.0, 8192.0, 16.0);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (h_row), "Default Height (px)");
  adw_spin_row_set_value (ADW_SPIN_ROW (h_row), (double) prefs->default_height);
  g_signal_connect (h_row, "notify::value", G_CALLBACK (on_default_height_changed), ctx);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (canvas_grp), h_row);

  GtkWidget *brush_row = adw_spin_row_new_with_range (1.0, 96.0, 1.0);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (brush_row), "Default Brush Size (px)");
  adw_spin_row_set_value (ADW_SPIN_ROW (brush_row), prefs->default_brush_size);
  g_signal_connect (brush_row, "notify::value", G_CALLBACK (on_default_brush_changed), ctx);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (canvas_grp), brush_row);

  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (canvas_grp));

  adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (dlg), ADW_PREFERENCES_PAGE (page));
  adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (dlg),
                              ADW_PREFERENCES_PAGE (build_shortcuts_page (ctx)));
  adw_dialog_present (dlg, parent);
}
