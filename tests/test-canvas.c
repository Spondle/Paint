#include "paint-canvas.h"
#include "paint-tools.h"

#include <glib/gstdio.h>

static void
read_pixel_rgba (PaintDocument *doc, int x, int y, guint8 *r, guint8 *g, guint8 *b, guint8 *a)
{
  cairo_surface_t *surface = paint_document_get_surface (doc);
  cairo_surface_flush (surface);
  int stride = cairo_image_surface_get_stride (surface);
  const guint8 *data = cairo_image_surface_get_data (surface);
  const guint8 *px = data + y * stride + x * 4;
  *b = px[0];
  *g = px[1];
  *r = px[2];
  *a = px[3];
}

static void
test_blank_document (void)
{
  PaintDocument *doc = paint_document_new (64, 48);
  g_assert_cmpint (paint_document_get_width (doc), ==, 64);
  g_assert_cmpint (paint_document_get_height (doc), ==, 48);
  g_assert_false (paint_document_get_modified (doc));
  g_assert_false (paint_document_can_undo (doc));
  g_assert_false (paint_document_can_redo (doc));

  guint8 r, g, b, a;
  read_pixel_rgba (doc, 10, 10, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);
  g_assert_cmpuint (b, ==, 255);
  g_assert_cmpuint (a, ==, 255);
  paint_document_free (doc);
}

static void
test_brush_stroke_and_undo_redo (void)
{
  PaintDocument *doc = paint_document_new (80, 80);
  PaintToolSettings *settings = paint_tool_settings_new ();
  settings->color = (GdkRGBA) { 1.0f, 0.0f, 0.0f, 1.0f };
  settings->size = 8.0;
  const PaintTool *brush = paint_tools_find ("brush");

  paint_document_begin_stroke (doc, 20.0, 20.0, brush, settings);
  paint_document_continue_stroke (doc, 30.0, 20.0, brush, settings);
  paint_document_continue_stroke (doc, 40.0, 20.0, brush, settings);
  paint_document_end_stroke (doc, 50.0, 20.0, brush, settings);

  g_assert_true (paint_document_get_modified (doc));
  g_assert_true (paint_document_can_undo (doc));

  guint8 r, g, b, a;
  read_pixel_rgba (doc, 30, 20, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 0);
  g_assert_cmpuint (b, ==, 0);

  g_assert_true (paint_document_undo (doc));
  read_pixel_rgba (doc, 30, 20, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);
  g_assert_cmpuint (b, ==, 255);
  g_assert_true (paint_document_can_redo (doc));

  g_assert_true (paint_document_redo (doc));
  read_pixel_rgba (doc, 30, 20, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 0);
  g_assert_cmpuint (b, ==, 0);

  paint_tool_settings_free (settings);
  paint_document_free (doc);
}

static void
test_eraser_restores_background (void)
{
  PaintDocument *doc = paint_document_new (60, 60);
  PaintToolSettings *settings = paint_tool_settings_new ();
  settings->color = (GdkRGBA) { 0.0f, 0.0f, 1.0f, 1.0f };
  settings->size = 10.0;

  const PaintTool *brush = paint_tools_find ("brush");
  const PaintTool *eraser = paint_tools_find ("eraser");

  paint_document_begin_stroke (doc, 30.0, 30.0, brush, settings);
  paint_document_end_stroke (doc, 30.0, 30.0, brush, settings);

  guint8 r, g, b, a;
  read_pixel_rgba (doc, 30, 30, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 0);
  g_assert_cmpuint (g, ==, 0);
  g_assert_cmpuint (b, ==, 255);

  paint_document_begin_stroke (doc, 30.0, 30.0, eraser, settings);
  paint_document_end_stroke (doc, 30.0, 30.0, eraser, settings);
  read_pixel_rgba (doc, 30, 30, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);
  g_assert_cmpuint (b, ==, 255);

  paint_tool_settings_free (settings);
  paint_document_free (doc);
}

static void
test_resize_and_png_roundtrip (void)
{
  PaintDocument *doc = paint_document_new (40, 40);
  PaintToolSettings *settings = paint_tool_settings_new ();
  settings->color = (GdkRGBA) { 0.0f, 1.0f, 0.0f, 1.0f };
  settings->size = 6.0;
  const PaintTool *brush = paint_tools_find ("brush");

  paint_document_begin_stroke (doc, 10.0, 10.0, brush, settings);
  paint_document_end_stroke (doc, 10.0, 10.0, brush, settings);

  paint_document_resize (doc, 80, 60);
  g_assert_cmpint (paint_document_get_width (doc), ==, 80);
  g_assert_cmpint (paint_document_get_height (doc), ==, 60);

  guint8 r, g, b, a;
  read_pixel_rgba (doc, 10, 10, &r, &g, &b, &a);
  g_assert_cmpuint (g, ==, 255);
  g_assert_cmpuint (r, ==, 0);

  g_autofree char *tmp_dir = g_dir_make_tmp ("paint-test-XXXXXX", NULL);
  g_autofree char *png_path = g_build_filename (tmp_dir, "out.png", NULL);
  g_autoptr (GFile) out_file = g_file_new_for_path (png_path);

  g_autoptr (GError) err = NULL;
  g_assert_true (paint_document_save_to_file (doc, out_file, &err));
  g_assert_no_error (err);
  g_assert_false (paint_document_get_modified (doc));

  PaintDocument *loaded = paint_document_new_from_file (out_file, &err);
  g_assert_no_error (err);
  g_assert_nonnull (loaded);
  g_assert_cmpint (paint_document_get_width (loaded), ==, 80);
  g_assert_cmpint (paint_document_get_height (loaded), ==, 60);
  read_pixel_rgba (loaded, 10, 10, &r, &g, &b, &a);
  g_assert_cmpuint (g, ==, 255);

  paint_document_clear (loaded);
  read_pixel_rgba (loaded, 10, 10, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);
  g_assert_cmpuint (b, ==, 255);

  g_remove (png_path);
  g_rmdir (tmp_dir);
  paint_document_free (loaded);
  paint_tool_settings_free (settings);
  paint_document_free (doc);
}

static void
test_fill_tool_and_undo (void)
{
  PaintDocument *doc = paint_document_new (64, 64);
  PaintToolSettings *settings = paint_tool_settings_new ();

  /* Draw a closed black box from (16,16) to (48,48) */
  cairo_t *cr = cairo_create (paint_document_get_surface (doc));
  cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 1.0);
  cairo_set_line_width (cr, 4.0);
  cairo_rectangle (cr, 16.0, 16.0, 32.0, 32.0);
  cairo_stroke (cr);
  cairo_destroy (cr);

  /* Flood-fill inside the box with red */
  settings->color = (GdkRGBA) { 1.0f, 0.0f, 0.0f, 1.0f };
  const PaintTool *fill = paint_tools_find ("fill");
  g_assert_cmpstr (fill->id, ==, "fill");

  paint_document_begin_stroke (doc, 32.0, 32.0, fill, settings);
  paint_document_end_stroke (doc, 32.0, 32.0, fill, settings);

  guint8 r, g, b, a;
  /* Inside filled */
  read_pixel_rgba (doc, 32, 32, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 0);
  g_assert_cmpuint (b, ==, 0);

  /* Outside stays white */
  read_pixel_rgba (doc, 4, 4, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);
  g_assert_cmpuint (b, ==, 255);

  /* Undo restores inside to white */
  g_assert_true (paint_document_undo (doc));
  read_pixel_rgba (doc, 32, 32, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);
  g_assert_cmpuint (b, ==, 255);

  paint_tool_settings_free (settings);
  paint_document_free (doc);
}

static void
test_fill_antialiased_circle_no_halo (void)
{
  PaintDocument *doc = paint_document_new (100, 100);
  PaintToolSettings *settings = paint_tool_settings_new ();
  settings->color = (GdkRGBA) { 0.11f, 0.44f, 0.96f, 1.0f };

  /* Draw an anti-aliased blue circle */
  cairo_t *cr = cairo_create (paint_document_get_surface (doc));
  cairo_set_source_rgba (cr, 0.11, 0.44, 0.96, 1.0);
  cairo_set_line_width (cr, 8.0);
  cairo_arc (cr, 50.0, 50.0, 30.0, 0.0, 2.0 * G_PI);
  cairo_stroke (cr);
  cairo_destroy (cr);

  /* Fill inside with the same blue */
  const PaintTool *fill = paint_tools_find ("fill");
  paint_document_begin_stroke (doc, 50.0, 50.0, fill, settings);
  paint_document_end_stroke (doc, 50.0, 50.0, fill, settings);

  /* Every pixel inside radius 26 must be solid blue (no white AA fringe) */
  for (int y = 24; y <= 76; y++) {
    for (int x = 24; x <= 76; x++) {
      int dx = x - 50;
      int dy = y - 50;
      if (dx * dx + dy * dy <= 28 * 28) {
        guint8 r, g, b, a;
        read_pixel_rgba (doc, x, y, &r, &g, &b, &a);
        g_assert_cmpuint (r, <=, 35);
        g_assert_cmpuint (b, >=, 240);
      }
    }
  }

  /* Outside the circle must remain pure white */
  guint8 r, g, b, a;
  read_pixel_rgba (doc, 5, 5, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);
  g_assert_cmpuint (b, ==, 255);

  paint_tool_settings_free (settings);
  paint_document_free (doc);
}

static void
test_large_canvas_tiled_undo_redo (void)
{
  PaintDocument *doc = paint_document_new (4096, 4096);
  PaintToolSettings *settings = paint_tool_settings_new ();
  settings->color = (GdkRGBA) { 0.0f, 0.5f, 1.0f, 1.0f };
  settings->size = 12.0;
  const PaintTool *brush = paint_tools_find ("brush");

  /* Stroke across tile boundaries (x=240..540, y=250) */
  paint_document_begin_stroke (doc, 240.0, 250.0, brush, settings);
  for (int x = 250; x <= 540; x += 10)
    paint_document_continue_stroke (doc, (double) x, 250.0, brush, settings);
  paint_document_end_stroke (doc, 540.0, 250.0, brush, settings);

  guint8 r, g, b, a;
  read_pixel_rgba (doc, 260, 250, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 0);
  g_assert_cmpuint (b, ==, 255);

  read_pixel_rgba (doc, 520, 250, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 0);
  g_assert_cmpuint (b, ==, 255);

  g_assert_true (paint_document_undo (doc));
  read_pixel_rgba (doc, 260, 250, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  read_pixel_rgba (doc, 520, 250, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);

  g_assert_true (paint_document_redo (doc));
  read_pixel_rgba (doc, 520, 250, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 0);
  g_assert_cmpuint (b, ==, 255);

  paint_tool_settings_free (settings);
  paint_document_free (doc);
}

static void
test_cancel_stroke_restores_without_undo (void)
{
  PaintDocument *doc = paint_document_new (128, 128);
  PaintToolSettings *settings = paint_tool_settings_new ();
  settings->color = (GdkRGBA) { 1.0f, 0.0f, 0.0f, 1.0f };
  settings->size = 10.0;
  const PaintTool *brush = paint_tools_find ("brush");

  g_assert_false (paint_document_get_modified (doc));
  g_assert_false (paint_document_can_undo (doc));

  paint_document_begin_stroke (doc, 20.0, 20.0, brush, settings);
  paint_document_continue_stroke (doc, 40.0, 20.0, brush, settings);
  guint8 r, g, b, a;
  read_pixel_rgba (doc, 30, 20, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 0);

  paint_document_cancel_stroke (doc);
  read_pixel_rgba (doc, 30, 20, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);
  g_assert_cmpuint (b, ==, 255);
  g_assert_false (paint_document_get_modified (doc));
  g_assert_false (paint_document_can_undo (doc));

  paint_tool_settings_free (settings);
  paint_document_free (doc);
}

static void
test_line_tool_and_undo_redo (void)
{
  PaintDocument *doc = paint_document_new (100, 100);
  PaintToolSettings *settings = paint_tool_settings_new ();
  settings->color = (GdkRGBA) { 1.0f, 0.0f, 0.0f, 1.0f };
  settings->size = 6.0;
  const PaintTool *line = paint_tools_find ("line");
  g_assert_cmpstr (line->id, ==, "line");
  g_assert_nonnull (line->draw_preview);
  g_assert_nonnull (line->snap_point);

  paint_document_begin_stroke (doc, 10.0, 20.0, line, settings);
  /* Intermediate drag positions must not bake onto the document */
  paint_document_continue_stroke (doc, 40.0, 60.0, line, settings);
  paint_document_end_stroke (doc, 70.0, 20.0, line, settings);

  g_assert_true (paint_document_get_modified (doc));
  g_assert_true (paint_document_can_undo (doc));

  guint8 r, g, b, a;
  read_pixel_rgba (doc, 10, 20, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 0);
  read_pixel_rgba (doc, 40, 20, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 0);
  read_pixel_rgba (doc, 70, 20, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 0);

  /* Intermediate drag point stays white */
  read_pixel_rgba (doc, 40, 60, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);
  g_assert_cmpuint (b, ==, 255);

  g_assert_true (paint_document_undo (doc));
  read_pixel_rgba (doc, 40, 20, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);
  g_assert_cmpuint (b, ==, 255);

  g_assert_true (paint_document_redo (doc));
  read_pixel_rgba (doc, 40, 20, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 0);

  paint_tool_settings_free (settings);
  paint_document_free (doc);
}

static void
test_line_tool_tiled_undo_cancel_and_snap (void)
{
  PaintDocument *doc = paint_document_new (600, 600);
  PaintToolSettings *settings = paint_tool_settings_new ();
  settings->color = (GdkRGBA) { 0.0f, 0.0f, 1.0f, 1.0f };
  settings->size = 8.0;
  const PaintTool *line = paint_tools_find ("line");

  /* Cancel mid-drag leaves document clean */
  paint_document_begin_stroke (doc, 100.0, 100.0, line, settings);
  paint_document_continue_stroke (doc, 500.0, 500.0, line, settings);
  paint_document_cancel_stroke (doc);
  g_assert_false (paint_document_get_modified (doc));
  g_assert_false (paint_document_can_undo (doc));

  /* Draw across 256px tile boundaries */
  paint_document_begin_stroke (doc, 100.0, 100.0, line, settings);
  paint_document_continue_stroke (doc, 100.0, 400.0, line, settings);
  paint_document_end_stroke (doc, 500.0, 500.0, line, settings);

  guint8 r, g, b, a;
  read_pixel_rgba (doc, 100, 100, &r, &g, &b, &a);
  g_assert_cmpuint (b, ==, 255);
  g_assert_cmpuint (r, ==, 0);
  read_pixel_rgba (doc, 300, 300, &r, &g, &b, &a);
  g_assert_cmpuint (b, ==, 255);
  g_assert_cmpuint (r, ==, 0);
  read_pixel_rgba (doc, 500, 500, &r, &g, &b, &a);
  g_assert_cmpuint (b, ==, 255);
  g_assert_cmpuint (r, ==, 0);

  g_assert_true (paint_document_undo (doc));
  read_pixel_rgba (doc, 100, 100, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  read_pixel_rgba (doc, 300, 300, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  read_pixel_rgba (doc, 500, 500, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);

  /* Verify 45-degree Shift snapping */
  double sx = 200.0, sy = 112.0;
  line->snap_point (100.0, 100.0, &sx, &sy);
  g_assert_cmpfloat_with_epsilon (sx, 200.0, 1e-6);
  g_assert_cmpfloat_with_epsilon (sy, 100.0, 1e-6);

  sx = 195.0;
  sy = 205.0;
  line->snap_point (100.0, 100.0, &sx, &sy);
  g_assert_cmpfloat_with_epsilon (sx, 200.0, 1e-6);
  g_assert_cmpfloat_with_epsilon (sy, 200.0, 1e-6);

  paint_tool_settings_free (settings);
  paint_document_free (doc);
}

static void
test_shapes_outline_filled_snap_and_undo (void)
{
  PaintDocument *doc = paint_document_new (200, 200);
  PaintToolSettings *settings = paint_tool_settings_new ();
  settings->color = (GdkRGBA) { 1.0f, 0.0f, 0.0f, 1.0f };
  settings->size = 6.0;

  /* 1. Outline rectangle: border colored, center stays white */
  const PaintTool *rect = paint_tools_find ("shape-rectangle");
  g_assert_cmpstr (rect->id, ==, "shape-rectangle");
  g_assert_true (rect->is_shape);
  settings->shape_filled = FALSE;

  paint_document_begin_stroke (doc, 20.0, 20.0, rect, settings);
  paint_document_continue_stroke (doc, 180.0, 180.0, rect, settings);
  paint_document_end_stroke (doc, 80.0, 60.0, rect, settings);

  guint8 r, g, b, a;
  read_pixel_rgba (doc, 50, 20, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 0);
  read_pixel_rgba (doc, 50, 40, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);

  /* Undo restores rectangle border to white */
  g_assert_true (paint_document_undo (doc));
  read_pixel_rgba (doc, 50, 20, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);

  /* 2. Filled star: center is colored, undo restores */
  const PaintTool *star = paint_tools_find ("shape-star");
  g_assert_cmpstr (star->id, ==, "shape-star");
  settings->color = (GdkRGBA) { 0.0f, 0.0f, 1.0f, 1.0f };
  settings->shape_filled = TRUE;

  paint_document_begin_stroke (doc, 40.0, 40.0, star, settings);
  paint_document_end_stroke (doc, 140.0, 140.0, star, settings);
  read_pixel_rgba (doc, 90, 90, &r, &g, &b, &a);
  g_assert_cmpuint (b, ==, 255);
  g_assert_cmpuint (r, ==, 0);

  /* 3. Filled circle (ellipse + Shift square snap) */
  double sx = 180.0, sy = 140.0;
  const PaintTool *ellipse = paint_tools_find ("shape-ellipse");
  ellipse->snap_point (100.0, 100.0, &sx, &sy);
  g_assert_cmpfloat_with_epsilon (sx, 180.0, 1e-6);
  g_assert_cmpfloat_with_epsilon (sy, 180.0, 1e-6);

  /* 4. Horizontal arrow across y=100: wings extend above/below and must undo cleanly */
  const PaintTool *arrow = paint_tools_find ("shape-arrow");
  settings->shape_filled = FALSE;
  settings->size = 6.0;
  paint_document_begin_stroke (doc, 20.0, 100.0, arrow, settings);
  paint_document_end_stroke (doc, 160.0, 100.0, arrow, settings);
  read_pixel_rgba (doc, 150, 100, &r, &g, &b, &a);
  g_assert_cmpuint (b, ==, 255);
  g_assert_cmpuint (r, ==, 0);

  g_assert_true (paint_document_undo (doc));
  read_pixel_rgba (doc, 150, 100, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);

  /* 5. Clear canvas clears star and can be undone */
  paint_document_clear (doc);
  read_pixel_rgba (doc, 90, 90, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);
  g_assert_true (paint_document_undo (doc));
  read_pixel_rgba (doc, 90, 90, &r, &g, &b, &a);
  g_assert_cmpuint (b, ==, 255);
  g_assert_cmpuint (r, ==, 0);

  paint_tool_settings_free (settings);
  paint_document_free (doc);
}

static void
test_transparent_png_open_edit_resize_and_save (void)
{
  g_autofree char *tmp_dir = g_dir_make_tmp ("paint-alpha-test-XXXXXX", NULL);
  g_autofree char *src_path = g_build_filename (tmp_dir, "transparent.png", NULL);
  g_autofree char *out_path = g_build_filename (tmp_dir, "edited.png", NULL);

  /* Create an RGBA PNG with transparent background, opaque red rect, and semi-transparent pixel */
  cairo_surface_t *src = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 60, 60);
  cairo_t *cr = cairo_create (src);
  cairo_set_operator (cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint (cr);
  cairo_set_operator (cr, CAIRO_OPERATOR_OVER);
  cairo_set_source_rgba (cr, 1.0, 0.0, 0.0, 1.0);
  cairo_rectangle (cr, 20.0, 20.0, 20.0, 20.0);
  cairo_fill (cr);
  cairo_set_source_rgba (cr, 0.0, 0.0, 1.0, 0.5);
  cairo_rectangle (cr, 45.0, 45.0, 10.0, 10.0);
  cairo_fill (cr);
  cairo_destroy (cr);
  g_assert_cmpint (cairo_surface_write_to_png (src, src_path), ==, CAIRO_STATUS_SUCCESS);
  cairo_surface_destroy (src);

  /* Open transparent PNG */
  g_autoptr (GFile) src_file = g_file_new_for_path (src_path);
  g_autoptr (GError) err = NULL;
  PaintDocument *doc = paint_document_new_from_file (src_file, &err);
  g_assert_no_error (err);
  g_assert_nonnull (doc);
  g_assert_true (paint_document_has_transparent_background (doc));

  /* Background remains transparent (a == 0), opaque rect and semi-transparent rect preserved */
  guint8 r, g, b, a;
  read_pixel_rgba (doc, 5, 5, &r, &g, &b, &a);
  g_assert_cmpuint (a, ==, 0);
  read_pixel_rgba (doc, 30, 30, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 0);
  g_assert_cmpuint (b, ==, 0);
  g_assert_cmpuint (a, ==, 255);
  read_pixel_rgba (doc, 50, 50, &r, &g, &b, &a);
  g_assert_cmpuint (a, >, 100);
  g_assert_cmpuint (a, <, 160);

  /* Brush draws over transparent canvas while untouched area stays transparent */
  PaintToolSettings *settings = paint_tool_settings_new ();
  settings->color = (GdkRGBA) { 0.0f, 1.0f, 0.0f, 1.0f };
  settings->size = 8.0;
  const PaintTool *brush = paint_tools_find ("brush");
  paint_document_begin_stroke (doc, 10.0, 50.0, brush, settings);
  paint_document_end_stroke (doc, 10.0, 50.0, brush, settings);

  read_pixel_rgba (doc, 10, 50, &r, &g, &b, &a);
  g_assert_cmpuint (g, ==, 255);
  g_assert_cmpuint (a, ==, 255);
  read_pixel_rgba (doc, 5, 5, &r, &g, &b, &a);
  g_assert_cmpuint (a, ==, 0);

  /* Eraser on a transparent PNG erases back to transparent, not white */
  const PaintTool *eraser = paint_tools_find ("eraser");
  paint_document_begin_stroke (doc, 10.0, 50.0, eraser, settings);
  paint_document_end_stroke (doc, 10.0, 50.0, eraser, settings);
  read_pixel_rgba (doc, 10, 50, &r, &g, &b, &a);
  g_assert_cmpuint (a, ==, 0);

  /* Resizing preserves transparency both in existing and expanded areas */
  paint_document_resize (doc, 80, 80);
  read_pixel_rgba (doc, 5, 5, &r, &g, &b, &a);
  g_assert_cmpuint (a, ==, 0);
  read_pixel_rgba (doc, 70, 70, &r, &g, &b, &a);
  g_assert_cmpuint (a, ==, 0);
  read_pixel_rgba (doc, 30, 30, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (a, ==, 255);

  /* Save and reopen: transparency survives full roundtrip */
  g_autoptr (GFile) out_file = g_file_new_for_path (out_path);
  g_assert_true (paint_document_save_to_file (doc, out_file, &err));
  g_assert_no_error (err);

  PaintDocument *reloaded = paint_document_new_from_file (out_file, &err);
  g_assert_no_error (err);
  g_assert_nonnull (reloaded);
  g_assert_true (paint_document_has_transparent_background (reloaded));
  read_pixel_rgba (reloaded, 5, 5, &r, &g, &b, &a);
  g_assert_cmpuint (a, ==, 0);
  read_pixel_rgba (reloaded, 30, 30, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (a, ==, 255);

  /* Clear on transparent document clears to transparent */
  paint_document_clear (reloaded);
  read_pixel_rgba (reloaded, 30, 30, &r, &g, &b, &a);
  g_assert_cmpuint (a, ==, 0);

  g_remove (src_path);
  g_remove (out_path);
  g_rmdir (tmp_dir);
  paint_document_free (reloaded);
  paint_tool_settings_free (settings);
  paint_document_free (doc);
}

static void
test_transparent_brush_and_fill_tools (void)
{
  PaintDocument *doc = paint_document_new (100, 100);
  g_assert_false (paint_document_has_transparent_background (doc));

  PaintToolSettings *settings = paint_tool_settings_new ();
  settings->size = 12.0;

  /* 1. Transparent Brush punches transparent strokes into an opaque white canvas */
  const PaintTool *tbrush = paint_tools_find ("transparent");
  g_assert_cmpstr (tbrush->id, ==, "transparent");
  g_assert_true (tbrush->is_transparency);
  g_assert_true (tbrush->uses_brush_size);

  paint_document_begin_stroke (doc, 20.0, 20.0, tbrush, settings);
  paint_document_continue_stroke (doc, 40.0, 20.0, tbrush, settings);
  paint_document_end_stroke (doc, 50.0, 20.0, tbrush, settings);

  guint8 r, g, b, a;
  read_pixel_rgba (doc, 30, 20, &r, &g, &b, &a);
  g_assert_cmpuint (a, ==, 0);
  read_pixel_rgba (doc, 80, 80, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (a, ==, 255);

  /* Resizing an opaque canvas with transparent cutouts must preserve those cutouts */
  paint_document_resize (doc, 120, 120);
  read_pixel_rgba (doc, 30, 20, &r, &g, &b, &a);
  g_assert_cmpuint (a, ==, 0);
  read_pixel_rgba (doc, 110, 110, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (a, ==, 255);

  /* Undo resize + undo transparent stroke restores opaque white */
  g_assert_true (paint_document_undo (doc));
  g_assert_true (paint_document_undo (doc));
  read_pixel_rgba (doc, 30, 20, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (a, ==, 255);

  /* 2. Transparent Fill removes contiguous white background around a solid blue square */
  cairo_t *cr = cairo_create (paint_document_get_surface (doc));
  cairo_set_source_rgba (cr, 0.0, 0.0, 1.0, 1.0);
  cairo_rectangle (cr, 35.0, 35.0, 30.0, 30.0);
  cairo_fill (cr);
  cairo_destroy (cr);

  const PaintTool *tfill = paint_tools_find ("transparent-fill");
  g_assert_cmpstr (tfill->id, ==, "transparent-fill");
  g_assert_true (tfill->is_transparency);
  g_assert_false (tfill->uses_brush_size);

  paint_document_begin_stroke (doc, 5.0, 5.0, tfill, settings);
  paint_document_end_stroke (doc, 5.0, 5.0, tfill, settings);

  /* Outside becomes transparent, blue square stays opaque blue */
  read_pixel_rgba (doc, 5, 5, &r, &g, &b, &a);
  g_assert_cmpuint (a, ==, 0);
  read_pixel_rgba (doc, 50, 50, &r, &g, &b, &a);
  g_assert_cmpuint (b, ==, 255);
  g_assert_cmpuint (a, ==, 255);

  /* Undo restores white background */
  g_assert_true (paint_document_undo (doc));
  read_pixel_rgba (doc, 5, 5, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (a, ==, 255);

  /* 3. Clear to transparent sets transparent background and undo restores white background */
  paint_document_clear_transparent (doc);
  g_assert_true (paint_document_has_transparent_background (doc));
  read_pixel_rgba (doc, 50, 50, &r, &g, &b, &a);
  g_assert_cmpuint (a, ==, 0);

  g_assert_true (paint_document_undo (doc));
  g_assert_false (paint_document_has_transparent_background (doc));
  read_pixel_rgba (doc, 50, 50, &r, &g, &b, &a);
  g_assert_cmpuint (b, ==, 255);
  g_assert_cmpuint (a, ==, 255);

  paint_tool_settings_free (settings);
  paint_document_free (doc);
}

static void
test_shapes_vertical_flip_triangle_and_star (void)
{
  PaintDocument *doc = paint_document_new (200, 200);
  PaintToolSettings *settings = paint_tool_settings_new ();
  settings->color = (GdkRGBA) { 0.0f, 0.0f, 1.0f, 1.0f };
  settings->shape_filled = TRUE;

  const PaintTool *triangle = paint_tools_find ("shape-triangle");
  const PaintTool *star = paint_tools_find ("shape-star");
  guint8 r, g, b, a;

  /* 1. Upright triangle (drag downwards): apex at top, base at bottom */
  paint_document_begin_stroke (doc, 20.0, 20.0, triangle, settings);
  paint_document_end_stroke (doc, 120.0, 120.0, triangle, settings);
  read_pixel_rgba (doc, 30, 30, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (b, ==, 255);
  read_pixel_rgba (doc, 30, 110, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 0);
  g_assert_cmpuint (b, ==, 255);
  g_assert_true (paint_document_undo (doc));

  /* 2. Upside-down triangle (drag upwards): flat base at top, apex at bottom */
  paint_document_begin_stroke (doc, 20.0, 120.0, triangle, settings);
  paint_document_end_stroke (doc, 120.0, 20.0, triangle, settings);
  read_pixel_rgba (doc, 30, 30, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 0);
  g_assert_cmpuint (b, ==, 255);
  read_pixel_rgba (doc, 110, 30, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 0);
  g_assert_cmpuint (b, ==, 255);
  read_pixel_rgba (doc, 30, 110, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (b, ==, 255);
  read_pixel_rgba (doc, 70, 110, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 0);
  g_assert_cmpuint (b, ==, 255);
  g_assert_true (paint_document_undo (doc));

  /* 3. Upright star (drag downwards): single point at top (90, 50), notch at bottom (90, 130) */
  paint_document_begin_stroke (doc, 40.0, 40.0, star, settings);
  paint_document_end_stroke (doc, 140.0, 140.0, star, settings);
  read_pixel_rgba (doc, 90, 50, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 0);
  g_assert_cmpuint (b, ==, 255);
  read_pixel_rgba (doc, 90, 130, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (b, ==, 255);
  g_assert_true (paint_document_undo (doc));

  /* 4. Upside-down star (drag upwards & left): notch at top (90, 50), single point at bottom (90, 130) */
  paint_document_begin_stroke (doc, 140.0, 140.0, star, settings);
  paint_document_end_stroke (doc, 40.0, 40.0, star, settings);
  read_pixel_rgba (doc, 90, 50, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (b, ==, 255);
  read_pixel_rgba (doc, 90, 130, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 0);
  g_assert_cmpuint (b, ==, 255);
  g_assert_true (paint_document_undo (doc));

  /* 5. Squashed outline star/triangle/diamond: round join avoids miter spikes/bevel jumps and undoes cleanly */
  PaintDocument *wide_doc = paint_document_new (400, 100);
  const PaintTool *diamond = paint_tools_find ("shape-diamond");
  settings->shape_filled = FALSE;
  settings->size = 6.0;
  paint_document_begin_stroke (wide_doc, 50.0, 35.0, star, settings);
  paint_document_end_stroke (wide_doc, 350.0, 65.0, star, settings);
  read_pixel_rgba (wide_doc, 343, 45, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 0);
  g_assert_cmpuint (b, ==, 255);
  read_pixel_rgba (wide_doc, 348, 45, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (b, ==, 255);
  g_assert_true (paint_document_undo (wide_doc));
  read_pixel_rgba (wide_doc, 343, 45, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (b, ==, 255);

  paint_document_begin_stroke (wide_doc, 50.0, 40.0, triangle, settings);
  paint_document_end_stroke (wide_doc, 350.0, 60.0, triangle, settings);
  read_pixel_rgba (wide_doc, 350, 60, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 0);
  g_assert_cmpuint (b, ==, 255);
  read_pixel_rgba (wide_doc, 356, 60, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (b, ==, 255);
  g_assert_true (paint_document_undo (wide_doc));

  paint_document_begin_stroke (wide_doc, 50.0, 40.0, diamond, settings);
  paint_document_end_stroke (wide_doc, 350.0, 60.0, diamond, settings);
  read_pixel_rgba (wide_doc, 350, 50, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 0);
  g_assert_cmpuint (b, ==, 255);
  read_pixel_rgba (wide_doc, 356, 50, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (b, ==, 255);
  g_assert_true (paint_document_undo (wide_doc));
  paint_document_free (wide_doc);

  paint_tool_settings_free (settings);
  paint_document_free (doc);
}

static void
test_shape_tool_filled_icon_names (void)
{
  PaintToolSettings *settings = paint_tool_settings_new ();
  gsize n_tools = 0;
  const PaintTool *tools = paint_tools_get_all (&n_tools);

  for (gsize i = 0; i < n_tools; i++) {
    settings->shape_filled = FALSE;
    g_assert_cmpstr (paint_tool_get_icon_name (&tools[i], settings), ==, tools[i].icon_name);

    settings->shape_filled = TRUE;
    if (tools[i].is_shape) {
      g_assert_nonnull (tools[i].filled_icon_name);
      g_assert_cmpstr (paint_tool_get_icon_name (&tools[i], settings), ==, tools[i].filled_icon_name);
      g_assert_cmpstr (tools[i].filled_icon_name, !=, tools[i].icon_name);
    } else {
      g_assert_cmpstr (paint_tool_get_icon_name (&tools[i], settings), ==, tools[i].icon_name);
    }
  }

  paint_tool_settings_free (settings);
}

static void
test_block_arrow_outline_filled_and_undo (void)
{
  PaintDocument *doc = paint_document_new (200, 200);
  PaintToolSettings *settings = paint_tool_settings_new ();
  settings->color = (GdkRGBA) { 0.0f, 0.0f, 1.0f, 1.0f };
  settings->size = 4.0;

  const PaintTool *arrow = paint_tools_find ("shape-block-arrow");
  g_assert_cmpstr (arrow->id, ==, "shape-block-arrow");
  g_assert_true (arrow->is_shape);

  guint8 r, g, b, a;

  /* 1. Outline block arrow: hollow center, colored shaft edge & wing tip, undo restores */
  settings->shape_filled = FALSE;
  paint_document_begin_stroke (doc, 20.0, 100.0, arrow, settings);
  paint_document_end_stroke (doc, 120.0, 100.0, arrow, settings);
  read_pixel_rgba (doc, 50, 100, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (b, ==, 255);
  read_pixel_rgba (doc, 50, 90, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 0);
  g_assert_cmpuint (b, ==, 255);
  read_pixel_rgba (doc, 82, 74, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 0);
  g_assert_cmpuint (b, ==, 255);
  g_assert_true (paint_document_undo (doc));
  read_pixel_rgba (doc, 82, 74, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (b, ==, 255);

  /* 2. Filled block arrow: interior of shaft and head are colored, undo restores */
  settings->shape_filled = TRUE;
  paint_document_begin_stroke (doc, 20.0, 100.0, arrow, settings);
  paint_document_end_stroke (doc, 120.0, 100.0, arrow, settings);
  read_pixel_rgba (doc, 50, 100, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 0);
  g_assert_cmpuint (b, ==, 255);
  read_pixel_rgba (doc, 95, 100, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 0);
  g_assert_cmpuint (b, ==, 255);
  g_assert_true (paint_document_undo (doc));
  read_pixel_rgba (doc, 50, 100, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (b, ==, 255);

  paint_tool_settings_free (settings);
  paint_document_free (doc);
}

static void
test_eyedropper_pick_color (void)
{
  PaintDocument *doc = paint_document_new_transparent (40, 40);
  cairo_t *cr = cairo_create (paint_document_get_surface (doc));
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgba (cr, 1.0, 0.25, 0.5, 0.5);
  cairo_rectangle (cr, 10.0, 10.0, 10.0, 10.0);
  cairo_fill (cr);
  cairo_destroy (cr);

  g_assert_nonnull (paint_tools_find ("eyedropper"));

  GdkRGBA picked = { 0 };
  g_assert_true (paint_document_pick_color (doc, 15.0, 15.0, &picked));
  g_assert_cmpfloat_with_epsilon (picked.red, 1.0, 0.02);
  g_assert_cmpfloat_with_epsilon (picked.green, 0.25, 0.02);
  g_assert_cmpfloat_with_epsilon (picked.blue, 0.5, 0.02);
  g_assert_cmpfloat_with_epsilon (picked.alpha, 0.5, 0.02);

  g_assert_true (paint_document_pick_color (doc, 5.0, 5.0, &picked));
  g_assert_cmpfloat_with_epsilon (picked.alpha, 0.0, 0.01);

  g_assert_false (paint_document_pick_color (doc, -1.0, 5.0, &picked));
  g_assert_false (paint_document_pick_color (doc, 40.0, 5.0, &picked));

  paint_document_free (doc);
}

static void
test_brush_styles_pixel_and_soft (void)
{
  PaintDocument *doc = paint_document_new (100, 100);
  PaintToolSettings *settings = paint_tool_settings_new ();
  settings->color = (GdkRGBA) { 1.0f, 0.0f, 0.0f, 1.0f };
  const PaintTool *brush = paint_tools_find ("brush");

  /* Pixel brush produces crisp hard-edged pixels without anti-aliased fringe */
  settings->brush_style = PAINT_BRUSH_PIXEL;
  settings->size = 4.0;
  paint_document_begin_stroke (doc, 10.0, 10.0, brush, settings);
  paint_document_continue_stroke (doc, 20.0, 18.0, brush, settings);
  paint_document_end_stroke (doc, 30.0, 30.0, brush, settings);

  guint8 r, g, b, a;
  for (int y = 0; y < 40; y++)
    for (int x = 0; x < 40; x++)
      {
        read_pixel_rgba (doc, x, y, &r, &g, &b, &a);
        g_assert_true (g == 0 || g == 255);
      }
  read_pixel_rgba (doc, 20, 18, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 0);

  /* Soft brush creates a feathered radial gradient falloff around the stamp */
  settings->brush_style = PAINT_BRUSH_SOFT;
  settings->size = 20.0;
  paint_document_begin_stroke (doc, 70.0, 70.0, brush, settings);
  paint_document_end_stroke (doc, 70.0, 70.0, brush, settings);

  read_pixel_rgba (doc, 70, 70, &r, &g, &b, &a);
  guint8 g_center = g;
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g_center, <, 220);

  read_pixel_rgba (doc, 70, 76, &r, &g, &b, &a);
  g_assert_cmpuint (g, >, g_center);
  g_assert_cmpuint (g, <, 255);

  read_pixel_rgba (doc, 70, 82, &r, &g, &b, &a);
  g_assert_cmpuint (g, ==, 255);

  paint_tool_settings_free (settings);
  paint_document_free (doc);
}

static void
test_recent_colors_dedup_and_cap (void)
{
  PaintToolSettings *settings = paint_tool_settings_new ();

  for (int i = 0; i < 10; i++)
    {
      GdkRGBA c = { i / 10.0f, 0.2f, 0.8f, 1.0f };
      paint_tool_settings_add_recent_color (settings, &c);
    }

  g_assert_cmpint (settings->n_recent_colors, ==, PAINT_MAX_RECENT_COLORS);
  g_assert_cmpfloat_with_epsilon (settings->recent_colors[0].red, 0.9f, 1e-4);
  g_assert_cmpfloat_with_epsilon (settings->recent_colors[PAINT_MAX_RECENT_COLORS - 1].red, 0.2f, 1e-4);

  /* Re-adding an existing swatch moves it to front without duplicating */
  GdkRGBA dup = { 0.5f, 0.2f, 0.8f, 1.0f };
  paint_tool_settings_add_recent_color (settings, &dup);

  g_assert_cmpint (settings->n_recent_colors, ==, PAINT_MAX_RECENT_COLORS);
  g_assert_cmpfloat_with_epsilon (settings->recent_colors[0].red, 0.5f, 1e-4);

  int matches = 0;
  for (int i = 0; i < settings->n_recent_colors; i++)
    {
      if (settings->recent_colors[i].red > 0.45f && settings->recent_colors[i].red < 0.55f)
        matches++;
    }
  g_assert_cmpint (matches, ==, 1);

  paint_tool_settings_free (settings);
}

static void
test_selection_move_delete_crop_and_undo (void)
{
  PaintDocument *doc = paint_document_new (80, 80);
  cairo_t *cr = cairo_create (paint_document_get_surface (doc));
  cairo_set_operator (cr, CAIRO_OPERATOR_OVER);
  cairo_set_source_rgba (cr, 0.0, 0.0, 1.0, 1.0);
  cairo_rectangle (cr, 10.0, 10.0, 20.0, 20.0);
  cairo_fill (cr);
  cairo_destroy (cr);

  cairo_surface_t *copied = paint_document_copy_rect (doc, 10, 10, 20, 20);
  g_assert_nonnull (copied);
  g_assert_cmpint (cairo_image_surface_get_width (copied), ==, 20);
  g_assert_cmpint (cairo_image_surface_get_height (copied), ==, 20);

  cairo_rectangle_int_t src_hole = { 10, 10, 20, 20 };
  paint_document_commit_move (doc, &src_hole, copied, 40, 45);
  cairo_surface_destroy (copied);

  guint8 r, g, b, a;
  read_pixel_rgba (doc, 20, 20, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);
  g_assert_cmpuint (b, ==, 255);
  read_pixel_rgba (doc, 50, 55, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 0);
  g_assert_cmpuint (b, ==, 255);

  /* Single undo restores both source and destination */
  g_assert_true (paint_document_undo (doc));
  read_pixel_rgba (doc, 20, 20, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 0);
  g_assert_cmpuint (b, ==, 255);
  read_pixel_rgba (doc, 50, 55, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (b, ==, 255);

  g_assert_true (paint_document_redo (doc));

  /* Clear rect & undo */
  paint_document_clear_rect (doc, 40, 45, 20, 20);
  read_pixel_rgba (doc, 50, 55, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (b, ==, 255);
  g_assert_true (paint_document_undo (doc));
  read_pixel_rgba (doc, 50, 55, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 0);
  g_assert_cmpuint (b, ==, 255);

  /* Crop to selection & undo */
  paint_document_crop (doc, 40, 45, 20, 20);
  g_assert_cmpint (paint_document_get_width (doc), ==, 20);
  g_assert_cmpint (paint_document_get_height (doc), ==, 20);
  read_pixel_rgba (doc, 10, 10, &r, &g, &b, &a);
  g_assert_cmpuint (b, ==, 255);
  g_assert_true (paint_document_undo (doc));
  g_assert_cmpint (paint_document_get_width (doc), ==, 80);
  g_assert_cmpint (paint_document_get_height (doc), ==, 80);

  paint_document_free (doc);

  /* Clear rect on transparent document restores transparency */
  PaintDocument *tdoc = paint_document_new_transparent (40, 40);
  cairo_t *tcr = cairo_create (paint_document_get_surface (tdoc));
  cairo_set_source_rgba (tcr, 1.0, 0.0, 0.0, 1.0);
  cairo_rectangle (tcr, 5.0, 5.0, 20.0, 20.0);
  cairo_fill (tcr);
  cairo_destroy (tcr);

  paint_document_clear_rect (tdoc, 10, 10, 10, 10);
  read_pixel_rgba (tdoc, 15, 15, &r, &g, &b, &a);
  g_assert_cmpuint (a, ==, 0);
  paint_document_free (tdoc);
}

static void
test_scale_rotate_and_flip (void)
{
  PaintDocument *doc = paint_document_new (40, 20);
  cairo_t *cr = cairo_create (paint_document_get_surface (doc));
  cairo_set_operator (cr, CAIRO_OPERATOR_OVER);
  cairo_set_source_rgba (cr, 1.0, 0.0, 0.0, 1.0);
  cairo_rectangle (cr, 2.0, 2.0, 4.0, 4.0);
  cairo_fill (cr);
  cairo_destroy (cr);

  guint8 r, g, b, a;

  /* Scale 2x */
  paint_document_scale (doc, 80, 40);
  g_assert_cmpint (paint_document_get_width (doc), ==, 80);
  g_assert_cmpint (paint_document_get_height (doc), ==, 40);
  read_pixel_rgba (doc, 8, 8, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 0);
  g_assert_true (paint_document_undo (doc));
  g_assert_cmpint (paint_document_get_width (doc), ==, 40);
  g_assert_cmpint (paint_document_get_height (doc), ==, 20);

  /* Rotate 90 CW */
  paint_document_rotate_90 (doc, TRUE);
  g_assert_cmpint (paint_document_get_width (doc), ==, 20);
  g_assert_cmpint (paint_document_get_height (doc), ==, 40);
  read_pixel_rgba (doc, 16, 4, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 0);
  g_assert_true (paint_document_undo (doc));

  /* Rotate 90 CCW */
  paint_document_rotate_90 (doc, FALSE);
  g_assert_cmpint (paint_document_get_width (doc), ==, 20);
  g_assert_cmpint (paint_document_get_height (doc), ==, 40);
  read_pixel_rgba (doc, 4, 35, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 0);
  g_assert_true (paint_document_undo (doc));

  /* Flip horizontal */
  paint_document_flip (doc, TRUE);
  read_pixel_rgba (doc, 35, 4, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 0);
  read_pixel_rgba (doc, 4, 4, &r, &g, &b, &a);
  g_assert_cmpuint (g, ==, 255);
  g_assert_true (paint_document_undo (doc));

  /* Flip vertical */
  paint_document_flip (doc, FALSE);
  read_pixel_rgba (doc, 4, 15, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 0);
  read_pixel_rgba (doc, 4, 4, &r, &g, &b, &a);
  g_assert_cmpuint (g, ==, 255);

  paint_document_free (doc);
}

static int
count_dark_pixels (PaintDocument *doc, int x0, int y0, int x1, int y1)
{
  int dark = 0;
  for (int y = y0; y < y1; y++)
    for (int x = x0; x < x1; x++)
      {
        guint8 r, g, b, a;
        read_pixel_rgba (doc, x, y, &r, &g, &b, &a);
        if (r < 128 && g < 128 && b < 128)
          dark++;
      }
  return dark;
}

static void
test_text_rendering_and_undo (void)
{
  PaintDocument *doc = paint_document_new (200, 80);
  PaintToolSettings *settings = paint_tool_settings_new ();
  settings->color = (GdkRGBA) { 0.0f, 0.0f, 0.0f, 1.0f };
  paint_tool_settings_set_text_font_family (settings, "Sans");
  settings->text_font_size = 32;
  settings->text_bold = TRUE;
  settings->text_italic = FALSE;

  g_assert_nonnull (paint_tools_find ("text"));

  paint_document_draw_text (doc, 20.0, 15.0, "HELLO", settings);
  g_assert_true (paint_document_get_modified (doc));
  g_assert_cmpint (count_dark_pixels (doc, 20, 15, 180, 65), >, 20);

  guint8 r, g, b, a;
  read_pixel_rgba (doc, 5, 5, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);
  g_assert_cmpuint (b, ==, 255);

  g_assert_true (paint_document_undo (doc));
  g_assert_cmpint (count_dark_pixels (doc, 20, 15, 180, 65), ==, 0);

  g_assert_true (paint_document_redo (doc));
  g_assert_cmpint (count_dark_pixels (doc, 20, 15, 180, 65), >, 20);

  paint_tool_settings_free (settings);
  paint_document_free (doc);
}

static void
test_texture_roundtrip_and_preview_shift (void)
{
  PaintDocument *doc = paint_document_new (60, 60);
  cairo_t *cr = cairo_create (paint_document_get_surface (doc));
  cairo_set_operator (cr, CAIRO_OPERATOR_OVER);
  cairo_set_source_rgba (cr, 0.0, 0.0, 1.0, 1.0);
  cairo_rectangle (cr, 20.0, 20.0, 10.0, 10.0);
  cairo_fill (cr);
  cairo_destroy (cr);

  cairo_surface_t *copied = paint_document_copy_rect (doc, 20, 20, 10, 10);
  g_autoptr (GdkTexture) tex = paint_texture_from_surface (copied);
  cairo_surface_destroy (copied);
  g_assert_nonnull (tex);

  cairo_surface_t *surf2 = paint_surface_from_texture (tex);
  g_assert_nonnull (surf2);
  g_assert_cmpint (cairo_image_surface_get_width (surf2), ==, 10);
  g_assert_cmpint (cairo_image_surface_get_height (surf2), ==, 10);
  cairo_surface_destroy (surf2);

  PaintDocument *from_tex = paint_document_new_from_texture (tex);
  g_assert_nonnull (from_tex);
  g_assert_cmpint (paint_document_get_width (from_tex), ==, 10);
  g_assert_cmpint (paint_document_get_height (from_tex), ==, 10);
  guint8 r, g, b, a;
  read_pixel_rgba (from_tex, 5, 5, &r, &g, &b, &a);
  g_assert_cmpuint (b, ==, 255);
  paint_document_free (from_tex);

  /* Space-drag reposition shifts preview origin before committing */
  PaintToolSettings *settings = paint_tool_settings_new ();
  settings->color = (GdkRGBA) { 1.0f, 0.0f, 0.0f, 1.0f };
  settings->size = 6.0;
  const PaintTool *line = paint_tools_find ("line");

  paint_document_begin_stroke (doc, 10.0, 10.0, line, settings);
  paint_document_shift_preview_start (doc, settings, 10.0, 45.0);
  paint_document_end_stroke (doc, 40.0, 45.0, line, settings);

  read_pixel_rgba (doc, 25, 10, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);
  g_assert_cmpuint (b, ==, 255);

  read_pixel_rgba (doc, 25, 45, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 0);

  paint_tool_settings_free (settings);
  paint_document_free (doc);
}

static void
read_surface_pixel_rgba (cairo_surface_t *surface, int x, int y,
                         guint8 *r, guint8 *g, guint8 *b, guint8 *a)
{
  cairo_surface_flush (surface);
  int stride = cairo_image_surface_get_stride (surface);
  const guint8 *px = cairo_image_surface_get_data (surface) + y * stride + x * 4;
  *b = px[0];
  *g = px[1];
  *r = px[2];
  *a = px[3];
}

static void
test_layers_add_composite_visibility_and_opacity (void)
{
  PaintDocument *doc = paint_document_new (60, 60);
  g_assert_cmpint (paint_document_get_n_layers (doc), ==, 1);
  g_assert_cmpint (paint_document_get_active_layer (doc), ==, 0);
  g_assert_cmpstr (paint_document_layer_get_name (doc, 0), ==, "Background");
  g_assert_true (paint_document_layer_get_visible (doc, 0));
  g_assert_cmpfloat_with_epsilon (paint_document_layer_get_opacity (doc, 0), 1.0, 1e-5);

  int idx = paint_document_add_layer (doc, NULL);
  g_assert_cmpint (idx, ==, 1);
  g_assert_cmpint (paint_document_get_n_layers (doc), ==, 2);
  g_assert_cmpint (paint_document_get_active_layer (doc), ==, 1);
  g_assert_cmpstr (paint_document_layer_get_name (doc, 1), ==, "Layer 1");

  PaintToolSettings *settings = paint_tool_settings_new ();
  settings->color = (GdkRGBA) { 1.0f, 0.0f, 0.0f, 1.0f };
  settings->size = 14.0;
  const PaintTool *brush = paint_tools_find ("brush");

  paint_document_begin_stroke (doc, 30.0, 30.0, brush, settings);
  paint_document_end_stroke (doc, 30.0, 30.0, brush, settings);

  guint8 r, g, b, a;
  read_pixel_rgba (doc, 30, 30, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 0);
  g_assert_cmpuint (b, ==, 0);

  /* Background layer underneath stays white */
  read_surface_pixel_rgba (paint_document_layer_get_surface (doc, 0), 30, 30, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);
  g_assert_cmpuint (b, ==, 255);

  /* Hiding layer 1 reveals white background */
  paint_document_layer_set_visible (doc, 1, FALSE);
  g_assert_false (paint_document_layer_get_visible (doc, 1));
  read_pixel_rgba (doc, 30, 30, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);

  g_assert_true (paint_document_undo (doc));
  g_assert_true (paint_document_layer_get_visible (doc, 1));
  read_pixel_rgba (doc, 30, 30, &r, &g, &b, &a);
  g_assert_cmpuint (g, ==, 0);

  /* Sequential opacity drags coalesce into a single undo step */
  paint_document_layer_set_opacity (doc, 1, 0.7);
  paint_document_layer_set_opacity (doc, 1, 0.5);
  read_pixel_rgba (doc, 30, 30, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, >, 110);
  g_assert_cmpuint (g, <, 145);

  g_assert_true (paint_document_undo (doc));
  g_assert_cmpfloat_with_epsilon (paint_document_layer_get_opacity (doc, 1), 1.0, 1e-5);
  read_pixel_rgba (doc, 30, 30, &r, &g, &b, &a);
  g_assert_cmpuint (g, ==, 0);

  paint_tool_settings_free (settings);
  paint_document_free (doc);
}

static void
test_layers_blend_modes (void)
{
  PaintDocument *doc = paint_document_new (60, 60);
  cairo_t *cr0 = cairo_create (paint_document_layer_get_surface (doc, 0));
  cairo_set_source_rgba (cr0, 0.0, 1.0, 0.0, 1.0);
  cairo_rectangle (cr0, 10.0, 10.0, 40.0, 40.0);
  cairo_fill (cr0);
  cairo_destroy (cr0);

  paint_document_add_layer (doc, "Blend");
  cairo_t *cr1 = cairo_create (paint_document_layer_get_surface (doc, 1));
  cairo_set_source_rgba (cr1, 1.0, 0.0, 0.0, 1.0);
  cairo_rectangle (cr1, 10.0, 10.0, 40.0, 40.0);
  cairo_fill (cr1);
  cairo_destroy (cr1);

  guint8 r, g, b, a;

  paint_document_layer_set_blend_mode (doc, 1, PAINT_BLEND_MULTIPLY);
  read_pixel_rgba (doc, 30, 30, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 0);
  g_assert_cmpuint (g, ==, 0);
  g_assert_cmpuint (b, ==, 0);

  paint_document_layer_set_blend_mode (doc, 1, PAINT_BLEND_SCREEN);
  read_pixel_rgba (doc, 30, 30, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);
  g_assert_cmpuint (b, ==, 0);

  paint_document_layer_set_blend_mode (doc, 1, PAINT_BLEND_DARKEN);
  read_pixel_rgba (doc, 30, 30, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 0);
  g_assert_cmpuint (g, ==, 0);

  paint_document_layer_set_blend_mode (doc, 1, PAINT_BLEND_LIGHTEN);
  read_pixel_rgba (doc, 30, 30, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);

  paint_document_free (doc);
}

static void
test_layers_eraser_and_selection_preserve_lower_layers (void)
{
  PaintDocument *doc = paint_document_new (80, 80);
  cairo_t *cr0 = cairo_create (paint_document_layer_get_surface (doc, 0));
  cairo_set_source_rgba (cr0, 0.0, 1.0, 0.0, 1.0);
  cairo_rectangle (cr0, 10.0, 10.0, 60.0, 60.0);
  cairo_fill (cr0);
  cairo_destroy (cr0);

  paint_document_add_layer (doc, "Top");
  cairo_t *cr1 = cairo_create (paint_document_layer_get_surface (doc, 1));
  cairo_set_source_rgba (cr1, 0.0, 0.0, 1.0, 1.0);
  cairo_rectangle (cr1, 15.0, 15.0, 20.0, 20.0);
  cairo_fill (cr1);
  cairo_destroy (cr1);

  PaintToolSettings *settings = paint_tool_settings_new ();
  settings->size = 10.0;
  const PaintTool *eraser = paint_tools_find ("eraser");

  /* Erasing on upper layer clears to transparent, revealing green bottom layer */
  paint_document_begin_stroke (doc, 25.0, 25.0, eraser, settings);
  paint_document_end_stroke (doc, 25.0, 25.0, eraser, settings);

  guint8 r, g, b, a;
  read_pixel_rgba (doc, 25, 25, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 0);
  g_assert_cmpuint (g, ==, 255);
  g_assert_cmpuint (b, ==, 0);

  /* Moving selection hole on upper layer also reveals green bottom layer */
  paint_document_undo (doc);
  cairo_surface_t *clip = paint_document_copy_rect (doc, 16, 16, 10, 10);
  cairo_rectangle_int_t hole = { 16, 16, 10, 10 };
  paint_document_commit_move (doc, &hole, clip, 45, 45);
  cairo_surface_destroy (clip);

  read_pixel_rgba (doc, 20, 20, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 0);
  g_assert_cmpuint (g, ==, 255);
  g_assert_cmpuint (b, ==, 0);

  read_pixel_rgba (doc, 50, 50, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 0);
  g_assert_cmpuint (g, ==, 0);
  g_assert_cmpuint (b, ==, 255);

  paint_tool_settings_free (settings);
  paint_document_free (doc);
}

static void
test_layers_reorder_duplicate_remove_merge_and_flatten (void)
{
  PaintDocument *doc = paint_document_new (60, 60);
  PaintToolSettings *settings = paint_tool_settings_new ();
  settings->shape_filled = TRUE;
  const PaintTool *rect = paint_tools_find ("shape-rectangle");

  settings->color = (GdkRGBA) { 1.0f, 0.0f, 0.0f, 1.0f };
  paint_document_begin_stroke (doc, 10.0, 10.0, rect, settings);
  paint_document_end_stroke (doc, 50.0, 50.0, rect, settings);

  paint_document_add_layer (doc, "Green");
  settings->color = (GdkRGBA) { 0.0f, 1.0f, 0.0f, 1.0f };
  paint_document_begin_stroke (doc, 15.0, 15.0, rect, settings);
  paint_document_end_stroke (doc, 45.0, 45.0, rect, settings);

  paint_document_add_layer (doc, "Blue");
  settings->color = (GdkRGBA) { 0.0f, 0.0f, 1.0f, 1.0f };
  paint_document_begin_stroke (doc, 20.0, 20.0, rect, settings);
  paint_document_end_stroke (doc, 40.0, 40.0, rect, settings);

  guint8 r, g, b, a;
  read_pixel_rgba (doc, 30, 30, &r, &g, &b, &a);
  g_assert_cmpuint (b, ==, 255);

  /* Move Green (1) above Blue (2) -> active follows to 2 */
  paint_document_set_active_layer (doc, 1);
  g_assert_true (paint_document_move_layer (doc, 1, 2));
  g_assert_cmpint (paint_document_get_active_layer (doc), ==, 2);
  g_assert_cmpstr (paint_document_layer_get_name (doc, 2), ==, "Green");
  read_pixel_rgba (doc, 30, 30, &r, &g, &b, &a);
  g_assert_cmpuint (g, ==, 255);
  g_assert_cmpuint (b, ==, 0);

  /* Duplicate layer */
  g_assert_true (paint_document_duplicate_layer (doc, 2));
  g_assert_cmpint (paint_document_get_n_layers (doc), ==, 4);
  g_assert_cmpstr (paint_document_layer_get_name (doc, 3), ==, "Green Copy");

  /* Remove duplicated layer */
  g_assert_true (paint_document_remove_layer (doc, 3));
  g_assert_cmpint (paint_document_get_n_layers (doc), ==, 3);

  /* Merge top down onto Blue, cannot merge layer 0 */
  g_assert_false (paint_document_merge_layer_down (doc, 0));
  g_assert_true (paint_document_merge_layer_down (doc, 2));
  g_assert_cmpint (paint_document_get_n_layers (doc), ==, 2);
  read_pixel_rgba (doc, 30, 30, &r, &g, &b, &a);
  g_assert_cmpuint (g, ==, 255);

  /* Flatten collapses stack cleanly and supports undo */
  g_assert_true (paint_document_flatten_layers (doc));
  g_assert_cmpint (paint_document_get_n_layers (doc), ==, 1);
  g_assert_false (paint_document_remove_layer (doc, 0));
  read_pixel_rgba (doc, 30, 30, &r, &g, &b, &a);
  g_assert_cmpuint (g, ==, 255);

  g_assert_true (paint_document_undo (doc));
  g_assert_cmpint (paint_document_get_n_layers (doc), ==, 2);

  paint_tool_settings_free (settings);
  paint_document_free (doc);
}

static void
test_layers_transforms_affect_all_layers (void)
{
  PaintDocument *doc = paint_document_new (40, 20);
  cairo_t *cr0 = cairo_create (paint_document_layer_get_surface (doc, 0));
  cairo_set_source_rgba (cr0, 1.0, 0.0, 0.0, 1.0);
  cairo_rectangle (cr0, 2.0, 2.0, 4.0, 4.0);
  cairo_fill (cr0);
  cairo_destroy (cr0);

  paint_document_add_layer (doc, "BlueMark");
  cairo_t *cr1 = cairo_create (paint_document_layer_get_surface (doc, 1));
  cairo_set_source_rgba (cr1, 0.0, 0.0, 1.0, 1.0);
  cairo_rectangle (cr1, 34.0, 14.0, 4.0, 4.0);
  cairo_fill (cr1);
  cairo_destroy (cr1);

  guint8 r, g, b, a;

  paint_document_scale (doc, 80, 40);
  g_assert_cmpint (cairo_image_surface_get_width (paint_document_layer_get_surface (doc, 0)), ==, 80);
  g_assert_cmpint (cairo_image_surface_get_width (paint_document_layer_get_surface (doc, 1)), ==, 80);
  read_surface_pixel_rgba (paint_document_layer_get_surface (doc, 0), 8, 8, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 0);
  read_surface_pixel_rgba (paint_document_layer_get_surface (doc, 1), 71, 31, &r, &g, &b, &a);
  g_assert_cmpuint (b, ==, 255);
  g_assert_true (paint_document_undo (doc));

  paint_document_rotate_90 (doc, TRUE);
  g_assert_cmpint (cairo_image_surface_get_width (paint_document_layer_get_surface (doc, 1)), ==, 20);
  g_assert_cmpint (cairo_image_surface_get_height (paint_document_layer_get_surface (doc, 1)), ==, 40);
  read_surface_pixel_rgba (paint_document_layer_get_surface (doc, 0), 16, 4, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 0);
  read_surface_pixel_rgba (paint_document_layer_get_surface (doc, 1), 4, 35, &r, &g, &b, &a);
  g_assert_cmpuint (b, ==, 255);
  g_assert_true (paint_document_undo (doc));

  paint_document_flip (doc, TRUE);
  read_surface_pixel_rgba (paint_document_layer_get_surface (doc, 0), 35, 4, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 0);
  read_surface_pixel_rgba (paint_document_layer_get_surface (doc, 1), 4, 15, &r, &g, &b, &a);
  g_assert_cmpuint (b, ==, 255);

  paint_document_free (doc);
}

static void
test_layers_per_layer_tiled_undo_and_names (void)
{
  PaintDocument *doc = paint_document_new (400, 400);
  PaintToolSettings *settings = paint_tool_settings_new ();
  settings->size = 10.0;
  const PaintTool *brush = paint_tools_find ("brush");

  settings->color = (GdkRGBA) { 1.0f, 0.0f, 0.0f, 1.0f };
  paint_document_begin_stroke (doc, 50.0, 50.0, brush, settings);
  paint_document_end_stroke (doc, 50.0, 50.0, brush, settings);

  paint_document_add_layer (doc, NULL);
  paint_document_layer_set_name (doc, 1, "Ink");
  g_assert_cmpstr (paint_document_layer_get_name (doc, 1), ==, "Ink");

  settings->color = (GdkRGBA) { 0.0f, 0.0f, 1.0f, 1.0f };
  paint_document_begin_stroke (doc, 300.0, 300.0, brush, settings);
  paint_document_end_stroke (doc, 300.0, 300.0, brush, settings);

  guint8 r, g, b, a;
  read_pixel_rgba (doc, 300, 300, &r, &g, &b, &a);
  g_assert_cmpuint (b, ==, 255);

  /* Undo 1: reverts tiled stroke on Layer 1 */
  g_assert_true (paint_document_undo (doc));
  read_pixel_rgba (doc, 300, 300, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (b, ==, 255);

  /* Undo 2: reverts rename to "Layer 1" */
  g_assert_true (paint_document_undo (doc));
  g_assert_cmpstr (paint_document_layer_get_name (doc, 1), ==, "Layer 1");

  /* Undo 3: reverts adding Layer 1 */
  g_assert_true (paint_document_undo (doc));
  g_assert_cmpint (paint_document_get_n_layers (doc), ==, 1);

  /* Undo 4: reverts red stroke on Layer 0 */
  g_assert_true (paint_document_undo (doc));
  read_pixel_rgba (doc, 50, 50, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);
  g_assert_cmpuint (b, ==, 255);

  paint_tool_settings_free (settings);
  paint_document_free (doc);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/canvas/blank_document", test_blank_document);
  g_test_add_func ("/canvas/brush_stroke_and_undo_redo", test_brush_stroke_and_undo_redo);
  g_test_add_func ("/canvas/eraser_restores_background", test_eraser_restores_background);
  g_test_add_func ("/canvas/resize_and_png_roundtrip", test_resize_and_png_roundtrip);
  g_test_add_func ("/canvas/fill_tool_and_undo", test_fill_tool_and_undo);
  g_test_add_func ("/canvas/fill_antialiased_circle_no_halo", test_fill_antialiased_circle_no_halo);
  g_test_add_func ("/canvas/large_canvas_tiled_undo_redo", test_large_canvas_tiled_undo_redo);
  g_test_add_func ("/canvas/cancel_stroke_restores_without_undo", test_cancel_stroke_restores_without_undo);
  g_test_add_func ("/canvas/line_tool_and_undo_redo", test_line_tool_and_undo_redo);
  g_test_add_func ("/canvas/line_tool_tiled_undo_cancel_and_snap", test_line_tool_tiled_undo_cancel_and_snap);
  g_test_add_func ("/canvas/shapes_outline_filled_snap_and_undo", test_shapes_outline_filled_snap_and_undo);
  g_test_add_func ("/canvas/transparent_png_open_edit_resize_and_save", test_transparent_png_open_edit_resize_and_save);
  g_test_add_func ("/canvas/transparent_brush_and_fill_tools", test_transparent_brush_and_fill_tools);
  g_test_add_func ("/canvas/shapes_vertical_flip_triangle_and_star", test_shapes_vertical_flip_triangle_and_star);
  g_test_add_func ("/canvas/shape_tool_filled_icon_names", test_shape_tool_filled_icon_names);
  g_test_add_func ("/canvas/block_arrow_outline_filled_and_undo", test_block_arrow_outline_filled_and_undo);
  g_test_add_func ("/canvas/eyedropper_pick_color", test_eyedropper_pick_color);
  g_test_add_func ("/canvas/brush_styles_pixel_and_soft", test_brush_styles_pixel_and_soft);
  g_test_add_func ("/canvas/recent_colors_dedup_and_cap", test_recent_colors_dedup_and_cap);
  g_test_add_func ("/canvas/selection_move_delete_crop_and_undo", test_selection_move_delete_crop_and_undo);
  g_test_add_func ("/canvas/scale_rotate_and_flip", test_scale_rotate_and_flip);
  g_test_add_func ("/canvas/text_rendering_and_undo", test_text_rendering_and_undo);
  g_test_add_func ("/canvas/texture_roundtrip_and_preview_shift", test_texture_roundtrip_and_preview_shift);
  g_test_add_func ("/canvas/layers_add_composite_visibility_and_opacity", test_layers_add_composite_visibility_and_opacity);
  g_test_add_func ("/canvas/layers_blend_modes", test_layers_blend_modes);
  g_test_add_func ("/canvas/layers_eraser_and_selection_preserve_lower_layers", test_layers_eraser_and_selection_preserve_lower_layers);
  g_test_add_func ("/canvas/layers_reorder_duplicate_remove_merge_and_flatten", test_layers_reorder_duplicate_remove_merge_and_flatten);
  g_test_add_func ("/canvas/layers_transforms_affect_all_layers", test_layers_transforms_affect_all_layers);
  g_test_add_func ("/canvas/layers_per_layer_tiled_undo_and_names", test_layers_per_layer_tiled_undo_and_names);
  return g_test_run ();
}
