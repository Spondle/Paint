#include "paint-preferences.h"

#include <math.h>

#include "paint-canvas.h"

static char *
preferences_file_path (void)
{
  return g_build_filename (g_get_user_config_dir (), "paint", "settings.ini", NULL);
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
  g_key_file_save_to_file (kf, path, NULL);
}

void
paint_preferences_free (PaintPreferences *prefs)
{
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
  PaintPreferences *prefs;
  PaintPreferencesChangedCallback on_changed;
  gpointer user_data;
} PrefsDialogCtx;

static void
notify_changed (PrefsDialogCtx *ctx)
{
  paint_preferences_save (ctx->prefs);
  paint_preferences_apply_theme (ctx->prefs);
  if (ctx->on_changed)
    ctx->on_changed (ctx->prefs, ctx->user_data);
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

  PrefsDialogCtx *ctx = g_new0 (PrefsDialogCtx, 1);
  ctx->prefs = prefs;
  ctx->on_changed = on_changed;
  ctx->user_data = user_data;
  g_object_set_data_full (G_OBJECT (dlg), "prefs-ctx", ctx, g_free);

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
  adw_dialog_present (dlg, parent);
}
