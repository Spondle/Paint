#include "paint-application.h"

#include "paint-window.h"

struct _PaintApplication {
  AdwApplication parent_instance;

  PaintPreferences *preferences;
  PaintToolSettings *tool_settings;
};

G_DEFINE_FINAL_TYPE (PaintApplication, paint_application, ADW_TYPE_APPLICATION)

static void
on_preferences_changed (const PaintPreferences *prefs G_GNUC_UNUSED, gpointer user_data)
{
  GtkApplication *app = GTK_APPLICATION (user_data);
  for (GList *l = gtk_application_get_windows (app); l != NULL; l = l->next) {
    if (PAINT_IS_WINDOW (l->data))
      paint_window_apply_preferences (PAINT_WINDOW (l->data));
  }
}

static void
act_preferences (GSimpleAction *action G_GNUC_UNUSED,
                 GVariant *param G_GNUC_UNUSED,
                 gpointer user_data)
{
  PaintApplication *self = PAINT_APPLICATION (user_data);
  GtkWindow *active = gtk_application_get_active_window (GTK_APPLICATION (self));
  paint_preferences_dialog_present (self->preferences,
                                    GTK_WIDGET (active),
                                    on_preferences_changed,
                                    self);
}

static void
act_new_window (GSimpleAction *action G_GNUC_UNUSED,
                GVariant *param G_GNUC_UNUSED,
                gpointer user_data)
{
  PaintWindow *win = paint_window_new (ADW_APPLICATION (user_data));
  gtk_window_present (GTK_WINDOW (win));
}

static void
act_about (GSimpleAction *action G_GNUC_UNUSED,
           GVariant *param G_GNUC_UNUSED,
           gpointer user_data)
{
  GtkApplication *app = GTK_APPLICATION (user_data);
  AdwDialog *about = adw_about_dialog_new ();
  adw_about_dialog_set_application_name (ADW_ABOUT_DIALOG (about), PAINT_APP_NAME);
  adw_about_dialog_set_application_icon (ADW_ABOUT_DIALOG (about), PAINT_APP_ID);
  adw_about_dialog_set_version (ADW_ABOUT_DIALOG (about), PAINT_APP_VERSION);
  adw_about_dialog_set_developer_name (ADW_ABOUT_DIALOG (about), "Paint Contributors");
  adw_about_dialog_set_comments (
      ADW_ABOUT_DIALOG (about),
      "A fast, clean GNOME Paint application built with C, GTK 4, and Libadwaita.");
  adw_about_dialog_set_license_type (ADW_ABOUT_DIALOG (about), GTK_LICENSE_GPL_3_0);
  adw_dialog_present (about, GTK_WIDGET (gtk_application_get_active_window (app)));
}

static void
act_shortcuts (GSimpleAction *action G_GNUC_UNUSED,
               GVariant *param G_GNUC_UNUSED,
               gpointer user_data)
{
  static const struct {
    const char *section;
    const char *title;
    const char *accel;
  } items[] = {
    { "Files & Tabs", "New Canvas Tab", "<Control>n" },
    { "Files & Tabs", "New Window", "<Control><Shift>n" },
    { "Files & Tabs", "Open Image", "<Control>o" },
    { "Files & Tabs", "Save", "<Control>s" },
    { "Files & Tabs", "Save As", "<Control><Shift>s" },
    { "Files & Tabs", "Close Tab", "<Control>w" },
    { "Files & Tabs", "Tab Overview", "<Control><Shift>o" },
    { "Selection & Clipboard", "Select All", "<Control>a" },
    { "Selection & Clipboard", "Copy", "<Control>c" },
    { "Selection & Clipboard", "Cut", "<Control>x" },
    { "Selection & Clipboard", "Paste", "<Control>v" },
    { "Selection & Clipboard", "Paste as New Canvas", "<Control><Shift>v" },
    { "Selection & Clipboard", "Crop to Selection", "<Control><Shift>x" },
    { "Selection & Clipboard", "Delete Selection", "Delete" },
    { "Canvas & Tools", "Undo", "<Control>z" },
    { "Canvas & Tools", "Redo", "<Control><Shift>z" },
    { "Canvas & Tools", "Selection Tool", "m" },
    { "Canvas & Tools", "Brush Tool", "b" },
    { "Canvas & Tools", "Line Tool", "l" },
    { "Canvas & Tools", "Text Tool", "x" },
    { "Canvas & Tools", "Eraser Tool", "e" },
    { "Canvas & Tools", "Fill Tool", "f" },
    { "Canvas & Tools", "Color Picker (or Alt+Click)", "i" },
    { "Canvas & Tools", "Transparency Tool", "t" },
    { "Canvas & Tools", "Shapes Tool", "s" },
    { "Canvas & Tools", "Decrease / Increase Brush Size", "bracketleft bracketright" },
    { "Canvas & Tools", "Zoom In / Out / Fit / Reset", "<Control>plus <Control>minus <Control>9 <Control>0" },
    { "Canvas & Tools", "Toggle Layers Panel", "<Control>l" },
    { "Canvas & Tools", "New Layer", "<Control><Shift>l" },
    { "Canvas & Tools", "Fullscreen", "F11" },
    { "Canvas & Tools", "Toggle Floating Toolbar", "F9" },
    { "Canvas & Tools", "Preferences", "<Control>comma" },
  };

  GtkApplication *app = GTK_APPLICATION (user_data);
  GtkWidget *parent = GTK_WIDGET (gtk_application_get_active_window (app));

#if ADW_CHECK_VERSION (1, 8, 0)
  AdwDialog *dlg = adw_shortcuts_dialog_new ();
  AdwShortcutsSection *cur_sec = NULL;
  const char *cur_name = NULL;

  for (gsize i = 0; i < G_N_ELEMENTS (items); i++) {
    if (!cur_sec || g_strcmp0 (cur_name, items[i].section) != 0) {
      cur_name = items[i].section;
      cur_sec = adw_shortcuts_section_new (cur_name);
      adw_shortcuts_dialog_add (ADW_SHORTCUTS_DIALOG (dlg), cur_sec);
    }
    adw_shortcuts_section_add (cur_sec, adw_shortcuts_item_new (items[i].title, items[i].accel));
  }
  adw_dialog_present (dlg, parent);
#else
  AdwDialog *dlg = adw_preferences_dialog_new ();
  adw_dialog_set_title (dlg, "Keyboard Shortcuts");
  GtkWidget *page = adw_preferences_page_new ();
  GtkWidget *cur_grp = NULL;
  const char *cur_name = NULL;

  for (gsize i = 0; i < G_N_ELEMENTS (items); i++) {
    if (!cur_grp || g_strcmp0 (cur_name, items[i].section) != 0) {
      cur_name = items[i].section;
      cur_grp = adw_preferences_group_new ();
      adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (cur_grp), cur_name);
      adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (cur_grp));
    }
    GtkWidget *row = adw_action_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), items[i].title);
    GtkWidget *lbl = gtk_label_new (items[i].accel);
    gtk_widget_add_css_class (lbl, "dim-label");
    adw_action_row_add_suffix (ADW_ACTION_ROW (row), lbl);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (cur_grp), row);
  }
  adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (dlg), ADW_PREFERENCES_PAGE (page));
  adw_dialog_present (dlg, parent);
#endif
}

static void
act_quit (GSimpleAction *action G_GNUC_UNUSED,
          GVariant *param G_GNUC_UNUSED,
          gpointer user_data)
{
  PaintApplication *self = PAINT_APPLICATION (user_data);
  if (self->preferences && self->preferences->restore_session)
    paint_window_save_all_sessions (ADW_APPLICATION (self));
  else
    paint_window_clear_session ();
  g_application_quit (G_APPLICATION (self));
}

static void
paint_application_startup (GApplication *app)
{
  G_APPLICATION_CLASS (paint_application_parent_class)->startup (app);

  PaintApplication *self = PAINT_APPLICATION (app);
  self->preferences = paint_preferences_load ();
  paint_preferences_apply_theme (self->preferences);
  self->tool_settings = paint_tool_settings_new ();
  self->tool_settings->size = self->preferences->default_brush_size;

  GdkDisplay *display = gdk_display_get_default ();
  if (display) {
    GtkIconTheme *icon_theme = gtk_icon_theme_get_for_display (display);
    gtk_icon_theme_add_resource_path (icon_theme, "/io/github/spondle/Paint/icons");
    gtk_window_set_default_icon_name (PAINT_APP_ID);

    g_autoptr (GtkCssProvider) provider = gtk_css_provider_new ();
    gtk_css_provider_load_from_resource (provider, "/io/github/spondle/Paint/style.css");
    gtk_style_context_add_provider_for_display (display,
                                                GTK_STYLE_PROVIDER (provider),
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }

  static const GActionEntry app_actions[] = {
    { "new-window", act_new_window, NULL, NULL, NULL, { 0 } },
    { "preferences", act_preferences, NULL, NULL, NULL, { 0 } },
    { "about", act_about, NULL, NULL, NULL, { 0 } },
    { "shortcuts", act_shortcuts, NULL, NULL, NULL, { 0 } },
    { "quit", act_quit, NULL, NULL, NULL, { 0 } },
  };
  g_action_map_add_action_entries (G_ACTION_MAP (app),
                                   app_actions,
                                   G_N_ELEMENTS (app_actions),
                                   app);

  struct {
    const char *action;
    const char *accels[4];
  } accel_map[] = {
    { "app.new-window", { "<Primary><Shift>n", NULL } },
    { "app.preferences", { "<Primary>comma", NULL } },
    { "app.shortcuts", { "<Primary>question", NULL } },
    { "app.quit", { "<Primary>q", NULL } },
    { "win.new-tab", { "<Primary>n", "<Primary>t", NULL } },
    { "win.close-tab", { "<Primary>w", NULL } },
    { "win.open", { "<Primary>o", NULL } },
    { "win.save", { "<Primary>s", NULL } },
    { "win.save-as", { "<Primary><Shift>s", NULL } },
    { "win.undo", { "<Primary>z", NULL } },
    { "win.redo", { "<Primary><Shift>z", "<Primary>y", NULL } },
    { "win.copy", { "<Primary>c", NULL } },
    { "win.cut", { "<Primary>x", NULL } },
    { "win.paste", { "<Primary>v", NULL } },
    { "win.paste-new-tab", { "<Primary><Shift>v", NULL } },
    { "win.select-all", { "<Primary>a", NULL } },
    { "win.crop-selection", { "<Primary><Shift>x", NULL } },
    { "win.zoom-in", { "<Primary>plus", "<Primary>equal", "<Primary>KP_Add", NULL } },
    { "win.zoom-out", { "<Primary>minus", "<Primary>KP_Subtract", NULL } },
    { "win.zoom-fit", { "<Primary>9", "<Primary>KP_9", NULL } },
    { "win.zoom-reset", { "<Primary>0", "<Primary>KP_0", NULL } },
    { "win.tab-overview", { "<Primary><Shift>o", NULL } },
    { "win.toggle-layers", { "<Primary>l", NULL } },
    { "win.new-layer", { "<Primary><Shift>l", NULL } },
    { "win.toggle-toolbar", { "F9", NULL } },
    { "win.toggle-fullscreen", { "F11", NULL } },
  };

  for (gsize i = 0; i < G_N_ELEMENTS (accel_map); i++)
    gtk_application_set_accels_for_action (GTK_APPLICATION (app),
                                           accel_map[i].action,
                                           accel_map[i].accels);
}

static void
paint_application_activate (GApplication *app)
{
  PaintApplication *self = PAINT_APPLICATION (app);
  GtkWindow *win = gtk_application_get_active_window (GTK_APPLICATION (app));
  if (!win) {
    if (self->preferences && self->preferences->restore_session &&
        paint_window_restore_session (ADW_APPLICATION (app))) {
      return;
    }
    win = GTK_WINDOW (paint_window_new (ADW_APPLICATION (app)));
  }
  gtk_window_present (win);
}

static void
paint_application_open (GApplication *app,
                        GFile **files,
                        gint n_files,
                        const gchar *hint G_GNUC_UNUSED)
{
  GtkWindow *win = gtk_application_get_active_window (GTK_APPLICATION (app));
  if (!win)
    win = GTK_WINDOW (paint_window_new (ADW_APPLICATION (app)));

  for (gint i = 0; i < n_files; i++)
    paint_window_open_file (PAINT_WINDOW (win), files[i]);

  gtk_window_present (win);
}

static void
paint_application_finalize (GObject *object)
{
  PaintApplication *self = PAINT_APPLICATION (object);
  g_clear_pointer (&self->preferences, paint_preferences_free);
  g_clear_pointer (&self->tool_settings, paint_tool_settings_free);
  G_OBJECT_CLASS (paint_application_parent_class)->finalize (object);
}

static void
paint_application_class_init (PaintApplicationClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = paint_application_finalize;

  GApplicationClass *app_class = G_APPLICATION_CLASS (klass);
  app_class->startup = paint_application_startup;
  app_class->activate = paint_application_activate;
  app_class->open = paint_application_open;
}

static void
paint_application_init (PaintApplication *self G_GNUC_UNUSED)
{
}

PaintApplication *
paint_application_new (void)
{
  return g_object_new (PAINT_TYPE_APPLICATION,
                       "application-id", PAINT_APP_ID,
                       "flags", G_APPLICATION_HANDLES_OPEN,
                       NULL);
}

PaintPreferences *
paint_application_get_preferences (PaintApplication *self)
{
  return self->preferences;
}

PaintToolSettings *
paint_application_get_tool_settings (PaintApplication *self)
{
  return self->tool_settings;
}
