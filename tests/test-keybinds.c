#include "paint-preferences.h"

#include <glib/gstdio.h>

static void
test_keybinds_defaults_and_format (void)
{
  gsize count = 0;
  const PaintKeybindSpec *specs = paint_preferences_get_keybind_specs (&count);
  g_assert_nonnull (specs);
  g_assert_cmpuint (count, >, 30);

  const PaintKeybindSpec *brush = paint_preferences_find_keybind_spec ("win.tool-brush");
  g_assert_nonnull (brush);
  g_assert_cmpstr (brush->default_accel, ==, "b");

  g_autofree char *disabled_lbl = paint_preferences_format_accel_label ("");
  g_assert_cmpstr (disabled_lbl, ==, "Disabled");

  g_autofree char *brush_lbl = paint_preferences_format_accel_label ("b");
  g_assert_nonnull (brush_lbl);
  g_assert_true (*brush_lbl != '\0');
  g_assert_cmpstr (brush_lbl, !=, "Disabled");

  g_autofree char *undo_lbl = paint_preferences_format_accel_label ("<Primary>z");
  g_assert_nonnull (undo_lbl);
  g_assert_true (*undo_lbl != '\0');
}

static void
test_keybinds_override_disable_and_reset (void)
{
  PaintPreferences *prefs = paint_preferences_load ();

  g_assert_false (paint_preferences_is_keybind_custom (prefs, "win.tool-brush"));
  g_assert_cmpstr (paint_preferences_get_keybind (prefs, "win.tool-brush"), ==, "b");

  /* Setting equivalent canonical string does not count as custom override */
  paint_preferences_set_keybind (prefs, "win.undo", "<Control>z");
  g_assert_false (paint_preferences_is_keybind_custom (prefs, "win.undo"));

  /* Override to custom key and disable another */
  paint_preferences_set_keybind (prefs, "win.tool-brush", "p");
  g_assert_true (paint_preferences_is_keybind_custom (prefs, "win.tool-brush"));
  g_assert_cmpstr (paint_preferences_get_keybind (prefs, "win.tool-brush"), ==, "p");

  paint_preferences_set_keybind (prefs, "win.tool-line", "");
  g_assert_true (paint_preferences_is_keybind_custom (prefs, "win.tool-line"));
  g_assert_cmpstr (paint_preferences_get_keybind (prefs, "win.tool-line"), ==, "");
  g_assert_true (paint_preferences_has_custom_keybinds (prefs));

  paint_preferences_reset_keybind (prefs, "win.tool-brush");
  g_assert_false (paint_preferences_is_keybind_custom (prefs, "win.tool-brush"));
  g_assert_cmpstr (paint_preferences_get_keybind (prefs, "win.tool-brush"), ==, "b");
  g_assert_true (paint_preferences_has_custom_keybinds (prefs));

  paint_preferences_reset_all_keybinds (prefs);
  g_assert_false (paint_preferences_has_custom_keybinds (prefs));
  g_assert_cmpstr (paint_preferences_get_keybind (prefs, "win.tool-line"), ==, "l");

  paint_preferences_free (prefs);
}

static void
test_keybinds_conflict_detection (void)
{
  PaintPreferences *prefs = paint_preferences_load ();

  /* Disabled shortcuts never conflict with each other */
  g_assert_null (paint_preferences_find_keybind_conflict (prefs, "win.duplicate-layer", ""));

  /* Rebinding Brush to "e" conflicts with Eraser, unless Eraser itself is checked */
  const PaintKeybindSpec *conflict =
      paint_preferences_find_keybind_conflict (prefs, "win.tool-brush", "e");
  g_assert_nonnull (conflict);
  g_assert_cmpstr (conflict->id, ==, "win.tool-eraser");

  g_assert_null (paint_preferences_find_keybind_conflict (prefs, "win.tool-eraser", "e"));

  /* Modifier canonical equality (<Control>s matches <Primary>s) */
  const PaintKeybindSpec *save_conflict =
      paint_preferences_find_keybind_conflict (prefs, "win.copy", "<Control>s");
  g_assert_nonnull (save_conflict);
  g_assert_cmpstr (save_conflict->id, ==, "win.save");

  paint_preferences_free (prefs);
}

static void
test_keybinds_local_matching (void)
{
  PaintPreferences *prefs = paint_preferences_load ();

  /* Bare letter & CapsLock uppercase both match */
  g_assert_cmpstr (paint_preferences_match_local_keybind (prefs, GDK_KEY_b, 0),
                   ==, "win.tool-brush");
  g_assert_cmpstr (paint_preferences_match_local_keybind (prefs, GDK_KEY_B, 0),
                   ==, "win.tool-brush");

  /* Modifier chords do not match local single-key dispatcher */
  g_assert_null (paint_preferences_match_local_keybind (prefs, GDK_KEY_b, GDK_CONTROL_MASK));

  /* Brackets work */
  g_assert_cmpstr (paint_preferences_match_local_keybind (prefs, GDK_KEY_bracketleft, 0),
                   ==, "win.brush-smaller");

  /* Rebind brush to Shift+B and verify bare b stops matching while Shift+B matches */
  paint_preferences_set_keybind (prefs, "win.tool-brush", "<Shift>b");
  g_assert_null (paint_preferences_match_local_keybind (prefs, GDK_KEY_b, 0));
  g_assert_cmpstr (paint_preferences_match_local_keybind (prefs, GDK_KEY_B, GDK_SHIFT_MASK),
                   ==, "win.tool-brush");

  /* Disabled shortcut stops matching */
  paint_preferences_set_keybind (prefs, "win.tool-eraser", "");
  g_assert_null (paint_preferences_match_local_keybind (prefs, GDK_KEY_e, 0));

  paint_preferences_free (prefs);
}

static void
test_keybinds_save_and_reload_roundtrip (void)
{
  PaintPreferences *prefs = paint_preferences_load ();
  paint_preferences_set_keybind (prefs, "win.tool-brush", "<Shift>p");
  paint_preferences_set_keybind (prefs, "win.duplicate-layer", "<Primary>d");
  paint_preferences_set_keybind (prefs, "win.tool-line", "");
  paint_preferences_save (prefs);
  paint_preferences_free (prefs);

  PaintPreferences *reloaded = paint_preferences_load ();
  g_assert_cmpstr (paint_preferences_get_keybind (reloaded, "win.tool-brush"), ==, "<Shift>p");
  g_assert_cmpstr (paint_preferences_get_keybind (reloaded, "win.duplicate-layer"), ==, "<Control>d");
  g_assert_cmpstr (paint_preferences_get_keybind (reloaded, "win.tool-line"), ==, "");
  g_assert_true (paint_preferences_is_keybind_custom (reloaded, "win.tool-line"));
  g_assert_false (paint_preferences_is_keybind_custom (reloaded, "win.tool-eraser"));
  paint_preferences_free (reloaded);
}

static void
test_startup_behavior_defaults_and_roundtrip (void)
{
  g_autofree char *ini = g_build_filename (g_get_user_config_dir (), "paint", "settings.ini", NULL);
  g_unlink (ini);

  PaintPreferences *prefs = paint_preferences_load ();
  g_assert_cmpint (prefs->startup_window_mode, ==, PAINT_WINDOW_SIZE_REMEMBER);
  g_assert_false (prefs->fit_canvas_on_startup);
  g_assert_cmpint (prefs->last_window_width, ==, PAINT_DEFAULT_WINDOW_WIDTH);
  g_assert_cmpint (prefs->last_window_height, ==, PAINT_DEFAULT_WINDOW_HEIGHT);
  g_assert_false (prefs->last_window_maximized);

  prefs->startup_window_mode = PAINT_WINDOW_SIZE_FIT_CANVAS;
  prefs->fit_canvas_on_startup = TRUE;
  prefs->last_window_width = 1120;
  prefs->last_window_height = 840;
  prefs->last_window_maximized = TRUE;
  paint_preferences_save (prefs);
  paint_preferences_free (prefs);

  PaintPreferences *reloaded = paint_preferences_load ();
  g_assert_cmpint (reloaded->startup_window_mode, ==, PAINT_WINDOW_SIZE_FIT_CANVAS);
  g_assert_true (reloaded->fit_canvas_on_startup);
  g_assert_cmpint (reloaded->last_window_width, ==, 1120);
  g_assert_cmpint (reloaded->last_window_height, ==, 840);
  g_assert_true (reloaded->last_window_maximized);

  reloaded->startup_window_mode = PAINT_WINDOW_SIZE_DEFAULT;
  reloaded->fit_canvas_on_startup = FALSE;
  reloaded->last_window_maximized = FALSE;
  paint_preferences_save (reloaded);
  paint_preferences_free (reloaded);

  PaintPreferences *reloaded2 = paint_preferences_load ();
  g_assert_cmpint (reloaded2->startup_window_mode, ==, PAINT_WINDOW_SIZE_DEFAULT);
  g_assert_false (reloaded2->fit_canvas_on_startup);
  g_assert_cmpint (reloaded2->last_window_width, ==, 1120);
  g_assert_cmpint (reloaded2->last_window_height, ==, 840);
  g_assert_false (reloaded2->last_window_maximized);
  paint_preferences_free (reloaded2);
}

int
main (int argc, char **argv)
{
  g_autofree char *tmp_cfg = g_dir_make_tmp ("paint-keybinds-XXXXXX", NULL);
  g_assert_nonnull (tmp_cfg);
  g_setenv ("XDG_CONFIG_HOME", tmp_cfg, TRUE);

  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/keybinds/defaults_and_format", test_keybinds_defaults_and_format);
  g_test_add_func ("/keybinds/override_disable_and_reset", test_keybinds_override_disable_and_reset);
  g_test_add_func ("/keybinds/conflict_detection", test_keybinds_conflict_detection);
  g_test_add_func ("/keybinds/local_matching", test_keybinds_local_matching);
  g_test_add_func ("/keybinds/save_and_reload_roundtrip", test_keybinds_save_and_reload_roundtrip);
  g_test_add_func ("/preferences/startup_behavior_roundtrip",
                   test_startup_behavior_defaults_and_roundtrip);
  int res = g_test_run ();

  g_autofree char *ini = g_build_filename (tmp_cfg, "paint", "settings.ini", NULL);
  g_autofree char *sub = g_build_filename (tmp_cfg, "paint", NULL);
  g_unlink (ini);
  g_rmdir (sub);
  g_rmdir (tmp_cfg);
  return res;
}
