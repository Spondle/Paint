#pragma once

#include "paint-document.h"

G_BEGIN_DECLS

typedef struct {
  const char *format;   /* "png", "jpeg", "webp", "bmp", "ora", or NULL to infer from file */
  int quality;          /* 1..100 for JPEG/WebP (0 or negative defaults to 92) */
  GdkRGBA matte_color;  /* background matte for opaque formats like JPEG (alpha<=0 defaults white) */
} PaintExportOptions;

gboolean paint_surface_has_transparency (cairo_surface_t *surface);

PaintDocument *paint_document_new_from_file (GFile *file, GError **error);
PaintDocument *paint_document_new_from_draft (GFile *draft_file,
                                              GFile *original_file,
                                              gboolean modified,
                                              GError **error);

gboolean paint_document_save_to_file (PaintDocument *doc, GFile *file, GError **error);
gboolean paint_document_export_to_file (PaintDocument *doc,
                                        GFile *file,
                                        const PaintExportOptions *options,
                                        GError **error);

GBytes *paint_io_encode_ora (const PaintDocument *doc, GError **error);
PaintDocument *paint_io_load_ora_bytes (GBytes *bytes, GFile *file, GError **error);
gboolean paint_io_is_ora_file (GFile *file);
GdkTexture *paint_io_load_file_as_texture (GFile *file, GError **error);
char *paint_io_layer_name_from_file (GFile *file);

G_END_DECLS
