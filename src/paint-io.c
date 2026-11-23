#include "paint-io.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_LIBWEBP
#include <webp/decode.h>
#include <webp/encode.h>
#endif

#include "paint-document-private.h"
#include "paint-i18n.h"

/* ============================================================================
 * CRC32 & Minimal ZIP Archive Reader/Writer for OpenRaster (.ora)
 * ============================================================================ */

static uint32_t
crc32_ieee (const guint8 *data, gsize len)
{
  static uint32_t table[256];
  static gsize table_init = 0;
  if (g_once_init_enter (&table_init)) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++)
        c = (c & 1U) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    g_once_init_leave (&table_init, 1);
  }

  uint32_t crc = 0xFFFFFFFFU;
  for (gsize i = 0; i < len; i++)
    crc = table[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFU;
}

static inline void
buf_put_le16 (GByteArray *arr, uint16_t v)
{
  guint8 b[2] = { (guint8) (v & 0xFFU), (guint8) ((v >> 8) & 0xFFU) };
  g_byte_array_append (arr, b, 2);
}

static inline void
buf_put_le32 (GByteArray *arr, uint32_t v)
{
  guint8 b[4] = {
    (guint8) (v & 0xFFU),
    (guint8) ((v >> 8) & 0xFFU),
    (guint8) ((v >> 16) & 0xFFU),
    (guint8) ((v >> 24) & 0xFFU),
  };
  g_byte_array_append (arr, b, 4);
}

static inline uint16_t
read_le16 (const guint8 *p)
{
  return (uint16_t) p[0] | ((uint16_t) p[1] << 8);
}

static inline uint32_t
read_le32 (const guint8 *p)
{
  return (uint32_t) p[0] |
         ((uint32_t) p[1] << 8) |
         ((uint32_t) p[2] << 16) |
         ((uint32_t) p[3] << 24);
}

typedef struct {
  char *name;
  GBytes *data;
  uint32_t crc;
  uint32_t local_offset;
  uint16_t flags;
} ZipWriteEntry;

static void
zip_write_entry_free (gpointer ptr)
{
  ZipWriteEntry *e = ptr;
  if (!e)
    return;
  g_free (e->name);
  g_clear_pointer (&e->data, g_bytes_unref);
  g_free (e);
}

static void
zip_entries_add (GPtrArray *entries, const char *name, GBytes *data, uint16_t flags)
{
  gsize len = 0;
  const guint8 *ptr = g_bytes_get_data (data, &len);
  ZipWriteEntry *e = g_new0 (ZipWriteEntry, 1);
  e->name = g_strdup (name);
  e->data = g_bytes_ref (data);
  e->crc = crc32_ieee (ptr, len);
  e->flags = flags;
  g_ptr_array_add (entries, e);
}

static GBytes *
zip_build_archive (GPtrArray *entries)
{
  GByteArray *out = g_byte_array_new ();

  for (guint i = 0; i < entries->len; i++) {
    ZipWriteEntry *e = g_ptr_array_index (entries, i);
    gsize dlen = 0;
    const guint8 *dptr = g_bytes_get_data (e->data, &dlen);
    uint16_t nlen = (uint16_t) strlen (e->name);

    e->local_offset = out->len;
    buf_put_le32 (out, 0x04034B50U);
    buf_put_le16 (out, 20);
    buf_put_le16 (out, e->flags);
    buf_put_le16 (out, 0); /* STORED (PNG entries are already zlib-compressed) */
    buf_put_le16 (out, 0);
    buf_put_le16 (out, 0x5421);
    buf_put_le32 (out, e->crc);
    buf_put_le32 (out, (uint32_t) dlen);
    buf_put_le32 (out, (uint32_t) dlen);
    buf_put_le16 (out, nlen);
    buf_put_le16 (out, 0);
    g_byte_array_append (out, (const guint8 *) e->name, nlen);
    if (dlen > 0)
      g_byte_array_append (out, dptr, (guint) dlen);
  }

  uint32_t cd_offset = out->len;
  for (guint i = 0; i < entries->len; i++) {
    ZipWriteEntry *e = g_ptr_array_index (entries, i);
    gsize dlen = 0;
    (void) g_bytes_get_data (e->data, &dlen);
    uint16_t nlen = (uint16_t) strlen (e->name);

    buf_put_le32 (out, 0x02014B50U);
    buf_put_le16 (out, 20);
    buf_put_le16 (out, 20);
    buf_put_le16 (out, e->flags);
    buf_put_le16 (out, 0);
    buf_put_le16 (out, 0);
    buf_put_le16 (out, 0x5421);
    buf_put_le32 (out, e->crc);
    buf_put_le32 (out, (uint32_t) dlen);
    buf_put_le32 (out, (uint32_t) dlen);
    buf_put_le16 (out, nlen);
    buf_put_le16 (out, 0);
    buf_put_le16 (out, 0);
    buf_put_le16 (out, 0);
    buf_put_le16 (out, 0);
    buf_put_le32 (out, 0);
    buf_put_le32 (out, e->local_offset);
    g_byte_array_append (out, (const guint8 *) e->name, nlen);
  }
  uint32_t cd_size = out->len - cd_offset;

  buf_put_le32 (out, 0x06054B50U);
  buf_put_le16 (out, 0);
  buf_put_le16 (out, 0);
  buf_put_le16 (out, (uint16_t) entries->len);
  buf_put_le16 (out, (uint16_t) entries->len);
  buf_put_le32 (out, cd_size);
  buf_put_le32 (out, cd_offset);
  buf_put_le16 (out, 0);

  return g_byte_array_free_to_bytes (out);
}

typedef struct {
  uint16_t method;
  uint32_t crc;
  uint32_t comp_size;
  uint32_t uncomp_size;
  uint32_t local_offset;
} ZipReadEntry;

static GHashTable *
zip_parse_directory (const guint8 *buf, gsize len, GError **error)
{
  if (!buf || len < 22) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 _("Invalid OpenRaster archive"));
    return NULL;
  }

  gsize min_off = (len > 65558) ? (len - 65558) : 0;
  gssize eocd_off = -1;
  for (gssize off = (gssize) len - 22; off >= (gssize) min_off; off--) {
    if (read_le32 (buf + off) == 0x06054B50U) {
      uint16_t comment_len = read_le16 (buf + off + 20);
      if ((gsize) off + 22 + comment_len == len) {
        eocd_off = off;
        break;
      }
    }
  }
  if (eocd_off < 0) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 _("OpenRaster central directory not found"));
    return NULL;
  }

  uint16_t total_entries = read_le16 (buf + eocd_off + 10);
  uint32_t cd_size = read_le32 (buf + eocd_off + 12);
  uint32_t cd_offset = read_le32 (buf + eocd_off + 16);
  if ((gsize) cd_offset > (gsize) eocd_off || (gsize) cd_offset + cd_size > (gsize) eocd_off) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 _("Corrupt OpenRaster central directory"));
    return NULL;
  }

  GHashTable *map = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  gsize cur = cd_offset;
  gsize cd_end = (gsize) cd_offset + cd_size;

  for (uint16_t i = 0; i < total_entries && cur + 46 <= cd_end; i++) {
    if (read_le32 (buf + cur) != 0x02014B50U) {
      g_hash_table_unref (map);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   _("Corrupt OpenRaster entry header"));
      return NULL;
    }

    uint16_t method = read_le16 (buf + cur + 10);
    uint32_t crc = read_le32 (buf + cur + 16);
    uint32_t comp_size = read_le32 (buf + cur + 20);
    uint32_t uncomp_size = read_le32 (buf + cur + 24);
    uint16_t name_len = read_le16 (buf + cur + 28);
    uint16_t extra_len = read_le16 (buf + cur + 30);
    uint16_t cmt_len = read_le16 (buf + cur + 32);
    uint32_t local_off = read_le32 (buf + cur + 42);

    if (cur + 46 + name_len + extra_len + cmt_len > cd_end) {
      g_hash_table_unref (map);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   _("Truncated OpenRaster entry"));
      return NULL;
    }

    char *name = g_strndup ((const char *) (buf + cur + 46), name_len);
    ZipReadEntry *e = g_new0 (ZipReadEntry, 1);
    e->method = method;
    e->crc = crc;
    e->comp_size = comp_size;
    e->uncomp_size = uncomp_size;
    e->local_offset = local_off;
    g_hash_table_insert (map, name, e);

    cur += 46 + name_len + extra_len + cmt_len;
  }

  return map;
}

static GBytes *
zip_extract_entry (const guint8 *buf,
                   gsize len,
                   GHashTable *dir,
                   const char *name,
                   GError **error)
{
  const ZipReadEntry *e = g_hash_table_lookup (dir, name);
  if (!e) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                 _("Missing “%s” in OpenRaster archive"), name);
    return NULL;
  }
  if ((gsize) e->local_offset + 30 > len || read_le32 (buf + e->local_offset) != 0x04034B50U) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 _("Corrupt local header for “%s”"), name);
    return NULL;
  }

  uint16_t l_name_len = read_le16 (buf + e->local_offset + 26);
  uint16_t l_extra_len = read_le16 (buf + e->local_offset + 28);
  gsize data_off = (gsize) e->local_offset + 30 + l_name_len + l_extra_len;
  if (data_off + e->comp_size > len) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 _("Truncated payload for “%s”"), name);
    return NULL;
  }

  const guint8 *src = buf + data_off;
  GBytes *result = NULL;

  if (e->method == 0) {
    if (e->comp_size != e->uncomp_size) {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   _("Invalid stored entry size for “%s”"), name);
      return NULL;
    }
    result = g_bytes_new (src, e->uncomp_size);
  } else if (e->method == 8) {
    g_autoptr (GZlibDecompressor) decomp =
        g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_RAW);
    GByteArray *out = g_byte_array_sized_new (MAX ((guint) 256, (guint) e->uncomp_size));
    guint8 tmp[16384];
    gsize in_pos = 0;

    while (TRUE) {
      gsize bytes_read = 0;
      gsize bytes_written = 0;
      GConverterResult conv_res = g_converter_convert (
          G_CONVERTER (decomp),
          src + in_pos,
          (gsize) e->comp_size - in_pos,
          tmp,
          sizeof (tmp),
          in_pos >= e->comp_size ? G_CONVERTER_INPUT_AT_END : G_CONVERTER_NO_FLAGS,
          &bytes_read,
          &bytes_written,
          error);
      if (conv_res == G_CONVERTER_ERROR) {
        g_byte_array_unref (out);
        return NULL;
      }
      if (bytes_written > 0)
        g_byte_array_append (out, tmp, (guint) bytes_written);
      in_pos += bytes_read;
      if (conv_res == G_CONVERTER_FINISHED)
        break;
      if (bytes_read == 0 && bytes_written == 0 && in_pos >= e->comp_size)
        break;
    }
    result = g_byte_array_free_to_bytes (out);
  } else {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                 _("Unsupported ZIP compression method %u in OpenRaster archive"),
                 (unsigned) e->method);
    return NULL;
  }

  gsize rlen = 0;
  const guint8 *rptr = g_bytes_get_data (result, &rlen);
  if (rlen != e->uncomp_size || crc32_ieee (rptr, rlen) != e->crc) {
    g_bytes_unref (result);
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 _("Checksum mismatch for “%s” in OpenRaster archive"), name);
    return NULL;
  }
  return result;
}

/* ============================================================================
 * PNG Stream & Pixel Format Helpers
 * ============================================================================ */

gboolean
paint_surface_has_transparency (cairo_surface_t *surface)
{
  if (!surface || cairo_surface_get_type (surface) != CAIRO_SURFACE_TYPE_IMAGE)
    return FALSE;
  if (cairo_image_surface_get_format (surface) != CAIRO_FORMAT_ARGB32)
    return FALSE;

  cairo_surface_flush (surface);
  int w = cairo_image_surface_get_width (surface);
  int h = cairo_image_surface_get_height (surface);
  int stride = cairo_image_surface_get_stride (surface);
  const unsigned char *data = cairo_image_surface_get_data (surface);

  for (int y = 0; y < h; y++) {
    const uint32_t *row = (const uint32_t *) (data + (gsize) y * (gsize) stride);
    for (int x = 0; x < w; x++) {
      if (((row[x] >> 24) & 0xFFU) < 255U)
        return TRUE;
    }
  }
  return FALSE;
}

typedef struct {
  const guint8 *data;
  gsize size;
  gsize offset;
} PngReadCursor;

static cairo_status_t
cairo_read_from_bytes (void *closure, unsigned char *out, unsigned int length)
{
  PngReadCursor *cursor = closure;
  if (cursor->offset + length > cursor->size)
    return CAIRO_STATUS_READ_ERROR;
  memcpy (out, cursor->data + cursor->offset, length);
  cursor->offset += length;
  return CAIRO_STATUS_SUCCESS;
}

static cairo_status_t
cairo_write_to_byte_array (void *closure, const unsigned char *data, unsigned int length)
{
  GByteArray *arr = closure;
  g_byte_array_append (arr, data, length);
  return CAIRO_STATUS_SUCCESS;
}

static cairo_surface_t *
decode_png_surface_from_bytes (GBytes *bytes, GError **error)
{
  gsize size = 0;
  const guint8 *ptr = g_bytes_get_data (bytes, &size);
  PngReadCursor cursor = { ptr, size, 0 };
  cairo_surface_t *surf =
      cairo_image_surface_create_from_png_stream (cairo_read_from_bytes, &cursor);
  if (cairo_surface_status (surf) != CAIRO_STATUS_SUCCESS) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 _("Failed to decode PNG image: %s"),
                 cairo_status_to_string (cairo_surface_status (surf)));
    cairo_surface_destroy (surf);
    return NULL;
  }
  return surf;
}

static GBytes *
encode_surface_to_png_bytes (cairo_surface_t *surface, GError **error)
{
  GByteArray *arr = g_byte_array_new ();
  cairo_status_t status =
      cairo_surface_write_to_png_stream (surface, cairo_write_to_byte_array, arr);
  if (status != CAIRO_STATUS_SUCCESS) {
    g_byte_array_unref (arr);
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 _("Failed to encode PNG: %s"), cairo_status_to_string (status));
    return NULL;
  }
  return g_byte_array_free_to_bytes (arr);
}

static GdkPixbuf *
pixbuf_from_argb32_surface (cairo_surface_t *surface, gboolean with_alpha)
{
  cairo_surface_flush (surface);
  int w = cairo_image_surface_get_width (surface);
  int h = cairo_image_surface_get_height (surface);
  int stride = cairo_image_surface_get_stride (surface);
  const unsigned char *src_data = cairo_image_surface_get_data (surface);

  GdkPixbuf *pb = gdk_pixbuf_new (GDK_COLORSPACE_RGB, with_alpha, 8, w, h);
  int pb_stride = gdk_pixbuf_get_rowstride (pb);
  guchar *dst_data = gdk_pixbuf_get_pixels (pb);

  for (int y = 0; y < h; y++) {
    const uint32_t *srow = (const uint32_t *) (src_data + (gsize) y * (gsize) stride);
    guchar *drow = dst_data + (gsize) y * (gsize) pb_stride;
    if (!with_alpha) {
      for (int x = 0; x < w; x++) {
        uint32_t px = srow[x];
        drow[x * 3 + 0] = (guchar) ((px >> 16) & 0xFFU);
        drow[x * 3 + 1] = (guchar) ((px >> 8) & 0xFFU);
        drow[x * 3 + 2] = (guchar) (px & 0xFFU);
      }
    } else {
      for (int x = 0; x < w; x++) {
        uint32_t px = srow[x];
        uint32_t a = (px >> 24) & 0xFFU;
        uint32_t r = (px >> 16) & 0xFFU;
        uint32_t g = (px >> 8) & 0xFFU;
        uint32_t b = px & 0xFFU;
        if (a > 0 && a < 255) {
          r = MIN (255U, (r * 255U + (a >> 1)) / a);
          g = MIN (255U, (g * 255U + (a >> 1)) / a);
          b = MIN (255U, (b * 255U + (a >> 1)) / a);
        }
        drow[x * 4 + 0] = (guchar) r;
        drow[x * 4 + 1] = (guchar) g;
        drow[x * 4 + 2] = (guchar) b;
        drow[x * 4 + 3] = (guchar) a;
      }
    }
  }
  return pb;
}

/* ============================================================================
 * OpenRaster (.ora) Encode & Decode
 * ============================================================================ */

static const char *
blend_mode_to_ora (PaintBlendMode mode)
{
  switch (mode) {
  case PAINT_BLEND_MULTIPLY:
    return "svg:multiply";
  case PAINT_BLEND_SCREEN:
    return "svg:screen";
  case PAINT_BLEND_OVERLAY:
    return "svg:overlay";
  case PAINT_BLEND_DARKEN:
    return "svg:darken";
  case PAINT_BLEND_LIGHTEN:
    return "svg:lighten";
  case PAINT_BLEND_NORMAL:
  default:
    return "svg:src-over";
  }
}

static PaintBlendMode
ora_to_blend_mode (const char *op)
{
  if (g_strcmp0 (op, "svg:multiply") == 0)
    return PAINT_BLEND_MULTIPLY;
  if (g_strcmp0 (op, "svg:screen") == 0)
    return PAINT_BLEND_SCREEN;
  if (g_strcmp0 (op, "svg:overlay") == 0)
    return PAINT_BLEND_OVERLAY;
  if (g_strcmp0 (op, "svg:darken") == 0)
    return PAINT_BLEND_DARKEN;
  if (g_strcmp0 (op, "svg:lighten") == 0)
    return PAINT_BLEND_LIGHTEN;
  return PAINT_BLEND_NORMAL;
}

GBytes *
paint_io_encode_ora (const PaintDocument *doc, GError **error)
{
  g_return_val_if_fail (doc != NULL, NULL);

  g_autoptr (GPtrArray) entries = g_ptr_array_new_with_free_func (zip_write_entry_free);
  g_autoptr (GBytes) mime_bytes = g_bytes_new_static ("image/openraster", 16);
  zip_entries_add (entries, "mimetype", mime_bytes, 0);

  GString *xml = g_string_new ("<?xml version='1.0' encoding='UTF-8'?>\n");
  char bg_r[G_ASCII_DTOSTR_BUF_SIZE], bg_g[G_ASCII_DTOSTR_BUF_SIZE];
  char bg_b[G_ASCII_DTOSTR_BUF_SIZE], bg_a[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_dtostr (bg_r, sizeof (bg_r), doc->background_color.red);
  g_ascii_dtostr (bg_g, sizeof (bg_g), doc->background_color.green);
  g_ascii_dtostr (bg_b, sizeof (bg_b), doc->background_color.blue);
  g_ascii_dtostr (bg_a, sizeof (bg_a), doc->background_color.alpha);
  g_string_append_printf (xml,
                          "<image version=\"0.0.6\" w=\"%d\" h=\"%d\" "
                          "paint-active-layer=\"%d\" paint-bg-rgba=\"%s,%s,%s,%s\">\n"
                          "  <stack>\n",
                          doc->width, doc->height, doc->active_layer_index,
                          bg_r, bg_g, bg_b, bg_a);

  g_autoptr (GPtrArray) layer_pngs = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
  int n_layers = paint_document_get_n_layers (doc);
  for (int i = n_layers - 1; i >= 0; i--) {
    const PaintLayer *layer = g_ptr_array_index (doc->layers, (guint) i);
    g_autofree char *src_path = g_strdup_printf ("data/layer%03d.png", i);
    g_autofree char *esc_name = g_markup_escape_text (layer->name ? layer->name : "Layer", -1);
    char op_buf[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_dtostr (op_buf, sizeof (op_buf), layer->opacity);

    g_string_append_printf (xml,
                            "    <layer name=\"%s\" src=\"%s\" x=\"0\" y=\"0\" "
                            "opacity=\"%s\" visibility=\"%s\" composite-op=\"%s\"",
                            esc_name,
                            src_path,
                            op_buf,
                            layer->visible ? "visible" : "hidden",
                            blend_mode_to_ora (layer->blend_mode));
    if (layer->alpha_lock)
      g_string_append (xml, " paint-alpha-lock=\"true\"");
    if (layer->locked)
      g_string_append (xml, " paint-locked=\"true\"");
    g_string_append (xml, "/>\n");

    GBytes *lpng = encode_surface_to_png_bytes (layer->surface, error);
    if (!lpng) {
      g_string_free (xml, TRUE);
      return NULL;
    }
    g_ptr_array_add (layer_pngs, lpng);
  }
  g_string_append (xml, "  </stack>\n</image>\n");

  gsize xml_len = xml->len;
  g_autoptr (GBytes) xml_bytes = g_bytes_new_take (g_string_free (xml, FALSE), xml_len);
  zip_entries_add (entries, "stack.xml", xml_bytes, 0x0800);

  for (int i = 0; i < n_layers; i++) {
    g_autofree char *src_path = g_strdup_printf ("data/layer%03d.png", i);
    GBytes *lpng = g_ptr_array_index (layer_pngs, (guint) (n_layers - 1 - i));
    zip_entries_add (entries, src_path, lpng, 0x0800);
  }

  g_autoptr (GBytes) merged_png = encode_surface_to_png_bytes (doc->surface, error);
  if (!merged_png)
    return NULL;
  zip_entries_add (entries, "mergedimage.png", merged_png, 0x0800);

  double scale = MIN (1.0, MIN (256.0 / MAX (1, doc->width), 256.0 / MAX (1, doc->height)));
  int tw = MAX (1, (int) lround (doc->width * scale));
  int th = MAX (1, (int) lround (doc->height * scale));
  cairo_surface_t *thumb_surf = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, tw, th);
  cairo_t *tcr = cairo_create (thumb_surf);
  cairo_set_operator (tcr, CAIRO_OPERATOR_SOURCE);
  cairo_scale (tcr, (double) tw / (double) doc->width, (double) th / (double) doc->height);
  cairo_set_source_surface (tcr, doc->surface, 0.0, 0.0);
  cairo_pattern_set_filter (cairo_get_source (tcr), CAIRO_FILTER_BILINEAR);
  cairo_paint (tcr);
  cairo_destroy (tcr);
  g_autoptr (GBytes) thumb_png = encode_surface_to_png_bytes (thumb_surf, error);
  cairo_surface_destroy (thumb_surf);
  if (!thumb_png)
    return NULL;
  zip_entries_add (entries, "Thumbnails/thumbnail.png", thumb_png, 0x0800);

  return zip_build_archive (entries);
}

typedef struct {
  char *name;
  char *src;
  int x;
  int y;
  double opacity;
  gboolean visible;
  PaintBlendMode blend_mode;
  gboolean alpha_lock;
  gboolean locked;
} OraLayerSpec;

typedef struct {
  int width;
  int height;
  int active_layer;
  gboolean has_bg;
  GdkRGBA bg;
  GPtrArray *specs; /* element-type: OraLayerSpec* (top -> bottom from XML) */
} OraStackParse;

static void
ora_layer_spec_free (gpointer ptr)
{
  OraLayerSpec *s = ptr;
  if (!s)
    return;
  g_free (s->name);
  g_free (s->src);
  g_free (s);
}

static void
ora_xml_start_element (GMarkupParseContext *ctx G_GNUC_UNUSED,
                       const gchar *element_name,
                       const gchar **attribute_names,
                       const gchar **attribute_values,
                       gpointer user_data,
                       GError **error G_GNUC_UNUSED)
{
  OraStackParse *st = user_data;
  if (g_strcmp0 (element_name, "image") == 0) {
    for (int i = 0; attribute_names && attribute_names[i]; i++) {
      const char *k = attribute_names[i];
      const char *v = attribute_values[i];
      if (g_strcmp0 (k, "w") == 0)
        st->width = CLAMP ((int) g_ascii_strtoll (v, NULL, 10), 1, 16384);
      else if (g_strcmp0 (k, "h") == 0)
        st->height = CLAMP ((int) g_ascii_strtoll (v, NULL, 10), 1, 16384);
      else if (g_strcmp0 (k, "paint-active-layer") == 0)
        st->active_layer = (int) g_ascii_strtoll (v, NULL, 10);
      else if (g_strcmp0 (k, "paint-bg-rgba") == 0) {
        g_auto (GStrv) parts = g_strsplit (v, ",", 4);
        if (g_strv_length (parts) == 4) {
          st->bg.red = (float) g_ascii_strtod (parts[0], NULL);
          st->bg.green = (float) g_ascii_strtod (parts[1], NULL);
          st->bg.blue = (float) g_ascii_strtod (parts[2], NULL);
          st->bg.alpha = (float) g_ascii_strtod (parts[3], NULL);
          st->has_bg = TRUE;
        }
      }
    }
  } else if (g_strcmp0 (element_name, "layer") == 0) {
    OraLayerSpec *spec = g_new0 (OraLayerSpec, 1);
    spec->opacity = 1.0;
    spec->visible = TRUE;
    spec->blend_mode = PAINT_BLEND_NORMAL;

    for (int i = 0; attribute_names && attribute_names[i]; i++) {
      const char *k = attribute_names[i];
      const char *v = attribute_values[i];
      if (g_strcmp0 (k, "name") == 0)
        spec->name = g_strdup (v);
      else if (g_strcmp0 (k, "src") == 0)
        spec->src = g_strdup (v);
      else if (g_strcmp0 (k, "x") == 0)
        spec->x = (int) g_ascii_strtoll (v, NULL, 10);
      else if (g_strcmp0 (k, "y") == 0)
        spec->y = (int) g_ascii_strtoll (v, NULL, 10);
      else if (g_strcmp0 (k, "opacity") == 0)
        spec->opacity = CLAMP (g_ascii_strtod (v, NULL), 0.0, 1.0);
      else if (g_strcmp0 (k, "visibility") == 0)
        spec->visible = (g_strcmp0 (v, "hidden") != 0);
      else if (g_strcmp0 (k, "composite-op") == 0)
        spec->blend_mode = ora_to_blend_mode (v);
      else if (g_strcmp0 (k, "paint-alpha-lock") == 0)
        spec->alpha_lock = (g_strcmp0 (v, "true") == 0 || g_strcmp0 (v, "1") == 0);
      else if (g_strcmp0 (k, "paint-locked") == 0)
        spec->locked = (g_strcmp0 (v, "true") == 0 || g_strcmp0 (v, "1") == 0);
    }

    if (spec->src && *spec->src)
      g_ptr_array_add (st->specs, spec);
    else
      ora_layer_spec_free (spec);
  }
}

PaintDocument *
paint_io_load_ora_bytes (GBytes *bytes, GFile *file, GError **error)
{
  gsize len = 0;
  const guint8 *raw = g_bytes_get_data (bytes, &len);
  g_autoptr (GHashTable) dir = zip_parse_directory (raw, len, error);
  if (!dir)
    return NULL;

  g_autoptr (GBytes) stack_bytes = zip_extract_entry (raw, len, dir, "stack.xml", error);
  if (!stack_bytes)
    return NULL;

  OraStackParse parsed = { 0 };
  parsed.active_layer = -1;
  parsed.specs = g_ptr_array_new_with_free_func (ora_layer_spec_free);

  static const GMarkupParser parser = {
    .start_element = ora_xml_start_element,
  };
  g_autoptr (GMarkupParseContext) mctx =
      g_markup_parse_context_new (&parser, 0, &parsed, NULL);
  gsize xml_len = 0;
  const char *xml_ptr = g_bytes_get_data (stack_bytes, &xml_len);
  if (!g_markup_parse_context_parse (mctx, xml_ptr, (gssize) xml_len, error) ||
      !g_markup_parse_context_end_parse (mctx, error)) {
    g_ptr_array_unref (parsed.specs);
    return NULL;
  }

  if (parsed.width <= 0 || parsed.height <= 0 || parsed.specs->len == 0) {
    g_ptr_array_unref (parsed.specs);
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 _("OpenRaster archive contains no valid layers"));
    return NULL;
  }

  GdkRGBA white = { 1.0f, 1.0f, 1.0f, 1.0f };
  PaintDocument *doc = paint_document_new_with_background_internal (
      parsed.width, parsed.height, parsed.has_bg ? &parsed.bg : &white);
  paint_document_clear_layers_internal (doc);

  /* OpenRaster stack.xml lists layers top-to-bottom; PaintDocument stores bottom-to-top */
  for (gssize idx = (gssize) parsed.specs->len - 1; idx >= 0; idx--) {
    const OraLayerSpec *spec = g_ptr_array_index (parsed.specs, (guint) idx);
    g_autoptr (GBytes) lbytes = zip_extract_entry (raw, len, dir, spec->src, error);
    if (!lbytes) {
      g_ptr_array_unref (parsed.specs);
      paint_document_free (doc);
      return NULL;
    }

    cairo_surface_t *decoded = decode_png_surface_from_bytes (lbytes, error);
    if (!decoded) {
      g_ptr_array_unref (parsed.specs);
      paint_document_free (doc);
      return NULL;
    }

    cairo_surface_t *surf = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
                                                        parsed.width,
                                                        parsed.height);
    cairo_t *cr = cairo_create (surf);
    cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface (cr, decoded, (double) spec->x, (double) spec->y);
    cairo_paint (cr);
    cairo_destroy (cr);
    cairo_surface_destroy (decoded);

    paint_document_append_layer_surface_internal (
        doc,
        (spec->name && *spec->name) ? spec->name : "Layer",
        surf,
        spec->visible,
        spec->opacity,
        spec->blend_mode);
    paint_document_layer_set_locks_no_undo (
        doc, paint_document_get_n_layers (doc) - 1, spec->alpha_lock, spec->locked);
  }

  int active = (parsed.active_layer >= 0)
                 ? CLAMP (parsed.active_layer, 0, (int) parsed.specs->len - 1)
                 : ((int) parsed.specs->len - 1);
  g_ptr_array_unref (parsed.specs);

  paint_document_recomposite_all (doc);
  if (!parsed.has_bg) {
    GdkRGBA bg = paint_surface_has_transparency (doc->surface)
                   ? (GdkRGBA) { 0.0f, 0.0f, 0.0f, 0.0f }
                   : (GdkRGBA) { 1.0f, 1.0f, 1.0f, 1.0f };
    paint_document_set_background_color_internal (doc, &bg);
  }

  paint_document_finish_session_restore (doc, active, FALSE);
  if (file)
    paint_document_set_file_internal (doc, file);
  return doc;
}

gboolean
paint_io_is_ora_file (GFile *file)
{
  if (!file)
    return FALSE;
  g_autofree char *base = g_file_get_basename (file);
  if (!base)
    return FALSE;
  g_autofree char *lower = g_ascii_strdown (base, -1);
  return g_str_has_suffix (lower, ".ora");
}

/* ============================================================================
 * Public Document Load / Save / Export
 * ============================================================================ */

#ifdef HAVE_LIBWEBP
static cairo_surface_t *
decode_webp_surface_from_bytes (const guint8 *data, gsize len, GError **error)
{
  int w = 0, h = 0;
  if (!data || len < 12 || !WebPGetInfo (data, (size_t) len, &w, &h) ||
      w <= 0 || h <= 0 || w > 16384 || h > 16384) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 _("Failed to decode WebP image"));
    return NULL;
  }

  uint8_t *rgba = WebPDecodeRGBA (data, (size_t) len, &w, &h);
  if (!rgba) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 _("Failed to decode WebP image"));
    return NULL;
  }

  cairo_surface_t *surf = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
  if (cairo_surface_status (surf) != CAIRO_STATUS_SUCCESS) {
    WebPFree (rgba);
    cairo_surface_destroy (surf);
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 _("Out of memory allocating image surface"));
    return NULL;
  }

  cairo_surface_flush (surf);
  unsigned char *dst = cairo_image_surface_get_data (surf);
  int dst_stride = cairo_image_surface_get_stride (surf);
  for (int y = 0; y < h; y++) {
    const uint8_t *src_row = rgba + (size_t) y * (size_t) w * 4;
    uint32_t *dst_row = (uint32_t *) (dst + (size_t) y * (size_t) dst_stride);
    for (int x = 0; x < w; x++) {
      uint8_t r = src_row[x * 4 + 0];
      uint8_t g = src_row[x * 4 + 1];
      uint8_t b = src_row[x * 4 + 2];
      uint8_t a = src_row[x * 4 + 3];
      uint32_t pr = (a == 255) ? r : (uint32_t) ((r * a + 127) / 255);
      uint32_t pg = (a == 255) ? g : (uint32_t) ((g * a + 127) / 255);
      uint32_t pb = (a == 255) ? b : (uint32_t) ((b * a + 127) / 255);
      dst_row[x] = ((uint32_t) a << 24) | (pr << 16) | (pg << 8) | pb;
    }
  }
  cairo_surface_mark_dirty (surf);
  WebPFree (rgba);
  return surf;
}
#endif

PaintDocument *
paint_document_new_from_file (GFile *file, GError **error)
{
  g_return_val_if_fail (G_IS_FILE (file), NULL);

  g_autoptr (GBytes) bytes = g_file_load_bytes (file, NULL, NULL, error);
  if (!bytes)
    return NULL;

  gsize len = 0;
  const guint8 *ptr = g_bytes_get_data (bytes, &len);

  gboolean is_zip = (len >= 4 && ptr[0] == 'P' && ptr[1] == 'K' && ptr[2] == 0x03 && ptr[3] == 0x04);
  if (is_zip || paint_io_is_ora_file (file))
    return paint_io_load_ora_bytes (bytes, file, error);

  cairo_surface_t *loaded = NULL;
  static const guint8 png_sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
  if (len >= 8 && memcmp (ptr, png_sig, 8) == 0) {
    loaded = decode_png_surface_from_bytes (bytes, error);
    if (!loaded)
      return NULL;
#ifdef HAVE_LIBWEBP
  } else if (len >= 12 && memcmp (ptr, "RIFF", 4) == 0 && memcmp (ptr + 8, "WEBP", 4) == 0) {
    loaded = decode_webp_surface_from_bytes (ptr, len, error);
    if (!loaded)
      return NULL;
#endif
  } else {
    g_autoptr (GdkTexture) texture = gdk_texture_new_from_bytes (bytes, error);
    if (!texture)
      return NULL;
    loaded = paint_surface_from_texture (texture);
  }

  int w = cairo_image_surface_get_width (loaded);
  int h = cairo_image_surface_get_height (loaded);
  gboolean has_alpha = paint_surface_has_transparency (loaded);
  PaintDocument *doc = has_alpha ? paint_document_new_transparent (w, h)
                                 : paint_document_new (w, h);
  PaintLayer *l0 = paint_document_get_active_layer_internal (doc);
  cairo_t *cr = cairo_create (l0->surface);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface (cr, loaded, 0, 0);
  cairo_paint (cr);
  cairo_destroy (cr);
  cairo_surface_destroy (loaded);

  paint_document_set_file_internal (doc, file);
  paint_document_set_modified (doc, FALSE);
  paint_document_recomposite_all (doc);
  paint_document_mark_all_dirty (doc);
  return doc;
}

PaintDocument *
paint_document_new_from_draft (GFile *draft_file,
                               GFile *original_file,
                               gboolean modified,
                               GError **error)
{
  PaintDocument *doc = paint_document_new_from_file (draft_file, error);
  if (!doc)
    return NULL;
  paint_document_set_file_internal (doc, original_file);
  paint_document_set_modified (doc, modified);
  return doc;
}

static const char *
infer_format_from_file (GFile *file)
{
  g_autofree char *base = file ? g_file_get_basename (file) : NULL;
  if (!base)
    return "png";
  g_autofree char *lower = g_ascii_strdown (base, -1);
  const char *ext = strrchr (lower, '.');
  if (!ext)
    return NULL;
  if (g_strcmp0 (ext, ".ora") == 0)
    return "ora";
  if (g_strcmp0 (ext, ".png") == 0)
    return "png";
  if (g_strcmp0 (ext, ".jpg") == 0 || g_strcmp0 (ext, ".jpeg") == 0)
    return "jpeg";
  if (g_strcmp0 (ext, ".webp") == 0)
    return "webp";
  if (g_strcmp0 (ext, ".bmp") == 0)
    return "bmp";
  return ext + 1;
}

static GBytes *
encode_document_to_bytes (PaintDocument *doc,
                          const char *fmt,
                          int quality,
                          const GdkRGBA *matte,
                          GError **error)
{
  if (g_strcmp0 (fmt, "ora") == 0)
    return paint_io_encode_ora (doc, error);

  if (g_strcmp0 (fmt, "png") == 0)
    return encode_surface_to_png_bytes (doc->surface, error);

  int q = CLAMP (quality > 0 ? quality : 92, 1, 100);
  gboolean is_jpeg = (g_strcmp0 (fmt, "jpeg") == 0);
  gboolean is_webp = (g_strcmp0 (fmt, "webp") == 0);

  if (is_webp) {
#ifdef HAVE_LIBWEBP
    g_autoptr (GdkPixbuf) pb = pixbuf_from_argb32_surface (doc->surface, TRUE);
    const uint8_t *rgba = gdk_pixbuf_read_pixels (pb);
    int w = gdk_pixbuf_get_width (pb);
    int h = gdk_pixbuf_get_height (pb);
    int stride = gdk_pixbuf_get_rowstride (pb);
    uint8_t *webp_out = NULL;
    size_t webp_size = (q >= 100)
                         ? WebPEncodeLosslessRGBA (rgba, w, h, stride, &webp_out)
                         : WebPEncodeRGBA (rgba, w, h, stride, (float) q, &webp_out);
    if (webp_size == 0 || !webp_out) {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Failed to encode WebP image"));
      return NULL;
    }
    return g_bytes_new_with_free_func (webp_out, webp_size, (GDestroyNotify) WebPFree, webp_out);
#endif
  }

  cairo_surface_t *export_surface = doc->surface;
  cairo_surface_t *flattened = NULL;
  if (is_jpeg) {
    GdkRGBA bg = (matte && matte->alpha > 0.0f)
                   ? (GdkRGBA) { matte->red, matte->green, matte->blue, 1.0f }
                   : (GdkRGBA) { 1.0f, 1.0f, 1.0f, 1.0f };
    flattened = paint_document_create_blank_surface (doc->width, doc->height, &bg);
    cairo_t *fcr = cairo_create (flattened);
    cairo_set_operator (fcr, CAIRO_OPERATOR_OVER);
    cairo_set_source_surface (fcr, doc->surface, 0, 0);
    cairo_paint (fcr);
    cairo_destroy (fcr);
    export_surface = flattened;
  }

  gboolean has_alpha = !is_jpeg && paint_surface_has_transparency (export_surface);
  g_autoptr (GdkPixbuf) pixbuf = pixbuf_from_argb32_surface (export_surface, has_alpha);
  g_clear_pointer (&flattened, cairo_surface_destroy);

  gchar *out_buf = NULL;
  gsize out_len = 0;
  gboolean ok = FALSE;
  if (is_jpeg || is_webp) {
    g_autofree char *q_str = g_strdup_printf ("%d", q);
    char *keys[] = { "quality", NULL };
    char *vals[] = { q_str, NULL };
    ok = gdk_pixbuf_save_to_bufferv (pixbuf, &out_buf, &out_len, fmt, keys, vals, error);
  } else {
    ok = gdk_pixbuf_save_to_bufferv (pixbuf, &out_buf, &out_len, fmt, NULL, NULL, error);
  }
  if (!ok)
    return NULL;
  return g_bytes_new_take (out_buf, out_len);
}

static gboolean
write_bytes_to_file (GFile *file, GBytes *bytes, GError **error)
{
  gsize len = 0;
  const char *data = g_bytes_get_data (bytes, &len);
  return g_file_replace_contents (file,
                                  data,
                                  len,
                                  NULL,
                                  FALSE,
                                  G_FILE_CREATE_NONE,
                                  NULL,
                                  NULL,
                                  error);
}

gboolean
paint_document_export_to_file (PaintDocument *doc,
                               GFile *file,
                               const PaintExportOptions *options,
                               GError **error)
{
  g_return_val_if_fail (doc != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  const char *fmt = (options && options->format && *options->format)
                      ? options->format
                      : infer_format_from_file (file);
  if (!fmt)
    fmt = "png";

  int quality = options ? options->quality : 92;
  const GdkRGBA *matte = options ? &options->matte_color : NULL;
  g_autoptr (GBytes) encoded = encode_document_to_bytes (doc, fmt, quality, matte, error);
  if (!encoded)
    return FALSE;

  return write_bytes_to_file (file, encoded, error);
}

gboolean
paint_document_save_to_file (PaintDocument *doc, GFile *file, GError **error)
{
  g_return_val_if_fail (doc != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  const char *fmt = infer_format_from_file (file);
  g_autoptr (GFile) target_file = NULL;
  if (!fmt) {
    fmt = "png";
    g_autofree char *uri = g_file_get_uri (file);
    g_autofree char *with_ext = g_strconcat (uri, ".png", NULL);
    target_file = g_file_new_for_uri (with_ext);
  } else {
    target_file = g_object_ref (file);
  }

  g_autoptr (GBytes) encoded = encode_document_to_bytes (doc, fmt, 92, NULL, error);
  if (!encoded)
    return FALSE;

  if (!write_bytes_to_file (target_file, encoded, error))
    return FALSE;

  paint_document_set_file_internal (doc, target_file);
  paint_document_set_modified (doc, FALSE);
  return TRUE;
}

GdkTexture *
paint_io_load_file_as_texture (GFile *file, GError **error)
{
  g_return_val_if_fail (G_IS_FILE (file), NULL);

  g_autoptr (GBytes) bytes = g_file_load_bytes (file, NULL, NULL, error);
  if (!bytes)
    return NULL;

  gsize len = 0;
  const guint8 *ptr = g_bytes_get_data (bytes, &len);
  gboolean is_zip = (len >= 4 && ptr[0] == 'P' && ptr[1] == 'K' && ptr[2] == 0x03 && ptr[3] == 0x04);
  if (!is_zip && !paint_io_is_ora_file (file)) {
    GdkTexture *tex = gdk_texture_new_from_bytes (bytes, NULL);
    if (tex)
      return tex;
  }

  PaintDocument *doc = paint_document_new_from_file (file, error);
  if (!doc)
    return NULL;
  GdkTexture *tex = paint_texture_from_surface (paint_document_get_surface (doc));
  paint_document_free (doc);
  return tex;
}

char *
paint_io_layer_name_from_file (GFile *file)
{
  if (!file)
    return g_strdup ("Layer");
  g_autofree char *base = g_file_get_basename (file);
  if (!base || !*base)
    return g_strdup ("Layer");
  char *dot = strrchr (base, '.');
  if (dot && dot != base)
    *dot = '\0';
  return g_strdup (base);
}
