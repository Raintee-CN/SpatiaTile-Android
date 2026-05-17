/*
 mlt.c -- lightweight MapLibre Tile helpers for android-spatialite

 This module implements AsMLT()/ST_AsMLT() for MapLibre Tile output.
*/

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#if defined(_WIN32) && !defined(__MINGW32__)
#include "config-msvc.h"
#else
#include "config.h"
#endif

#include <spatialite/sqlite.h>
#include <spatialite/gaiageo.h>
#include <spatialite/gaiaaux.h>

struct mvt_buf
{
    unsigned char *data;
    int len;
    int cap;
    int error;
};

struct mlt_u32_vec
{
    unsigned int *data;
    int len;
    int cap;
};

struct mlt_i32_vec
{
    int *data;
    int len;
    int cap;
};

struct mlt_u64_vec
{
    unsigned long long *data;
    int len;
    int cap;
};

struct mlt_prop_col
{
    char *name;
    char **values;
    unsigned char *present;
    int len;
    int cap;
};

struct mlt_ctx
{
    char *layer_name;
    int extent;
    struct mlt_u32_vec geom_types;
    struct mlt_i32_vec vertices;
    struct mlt_u32_vec geometry_lengths;
    struct mlt_u32_vec part_lengths;
    struct mlt_u32_vec ring_lengths;
    struct mlt_u64_vec ids;
    struct mlt_prop_col *props;
    int prop_count;
    int prop_cap;
    int feature_count;
    int has_id;
    int has_multipoint;
    int has_multiline;
    int has_multipolygon;
    int has_polygon;
    int error;
};

static void
mvt_buf_init (struct mvt_buf *buf)
{
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
    buf->error = 0;
}

static void
mvt_buf_free (struct mvt_buf *buf)
{
    if (buf->data)
        free (buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
    buf->error = 0;
}

static int
mvt_buf_reserve (struct mvt_buf *buf, int extra)
{
    int needed;
    int new_cap;
    unsigned char *new_data;

    if (buf->error)
        return 0;
    needed = buf->len + extra;
    if (needed <= buf->cap)
        return 1;
    new_cap = buf->cap ? buf->cap * 2 : 256;
    while (new_cap < needed)
        new_cap *= 2;
    new_data = (unsigned char *) realloc (buf->data, new_cap);
    if (!new_data)
      {
          buf->error = 1;
          return 0;
      }
    buf->data = new_data;
    buf->cap = new_cap;
    return 1;
}

static void
mvt_buf_put_byte (struct mvt_buf *buf, unsigned char value)
{
    if (!mvt_buf_reserve (buf, 1))
        return;
    buf->data[buf->len++] = value;
}

static void
mvt_buf_put_data (struct mvt_buf *buf, const unsigned char *data, int len)
{
    if (len <= 0)
        return;
    if (!mvt_buf_reserve (buf, len))
        return;
    memcpy (buf->data + buf->len, data, len);
    buf->len += len;
}

static void
mvt_buf_put_varint64 (struct mvt_buf *buf, unsigned long long value)
{
    while (value >= 0x80)
      {
          mvt_buf_put_byte (buf, (unsigned char) ((value & 0x7f) | 0x80));
          value >>= 7;
      }
    mvt_buf_put_byte (buf, (unsigned char) value);
}

static void
mlt_buf_put_string (struct mvt_buf *buf, const char *text)
{
    int len = text ? (int) strlen (text) : 0;

    mvt_buf_put_varint64 (buf, (unsigned long long) len);
    if (len > 0)
        mvt_buf_put_data (buf, (const unsigned char *) text, len);
}

static void
mlt_buf_put_stream_meta (struct mvt_buf *buf, int physical_type, int logical_type,
                         int logical1, int logical2, int physical_encoding,
                         int num_values, int byte_len)
{
    mvt_buf_put_byte (buf, (unsigned char) ((physical_type << 4) | (logical_type & 0xf)));
    mvt_buf_put_byte (buf, (unsigned char) ((logical1 << 5) | ((logical2 & 0x7) << 2)
                                           | (physical_encoding & 0x3)));
    mvt_buf_put_varint64 (buf, (unsigned long long) num_values);
    mvt_buf_put_varint64 (buf, (unsigned long long) byte_len);
}

static char *
mvt_strndup (const char *text, int len)
{
    char *copy;

    if (len < 0 || (!text && len > 0))
        return NULL;
    copy = (char *) malloc ((size_t) len + 1);
    if (!copy)
        return NULL;
    if (len > 0)
        memcpy (copy, text, (size_t) len);
    copy[len] = '\0';
    return copy;
}

static unsigned int
mvt_zigzag (int value)
{
    return (unsigned int) ((value << 1) ^ (value >> 31));
}

static void
mlt_u32_vec_free (struct mlt_u32_vec *vec)
{
    free (vec->data);
    vec->data = NULL;
    vec->len = 0;
    vec->cap = 0;
}

static void
mlt_i32_vec_free (struct mlt_i32_vec *vec)
{
    free (vec->data);
    vec->data = NULL;
    vec->len = 0;
    vec->cap = 0;
}

static void
mlt_u64_vec_free (struct mlt_u64_vec *vec)
{
    free (vec->data);
    vec->data = NULL;
    vec->len = 0;
    vec->cap = 0;
}

static int
mlt_u32_vec_push (struct mlt_u32_vec *vec, unsigned int value)
{
    unsigned int *new_data;
    int new_cap;

    if (vec->len == vec->cap)
      {
          new_cap = vec->cap ? vec->cap * 2 : 64;
          new_data = (unsigned int *) realloc (vec->data, sizeof (unsigned int) * (size_t) new_cap);
          if (!new_data)
              return 0;
          vec->data = new_data;
          vec->cap = new_cap;
      }
    vec->data[vec->len++] = value;
    return 1;
}

static int
mlt_i32_vec_push (struct mlt_i32_vec *vec, int value)
{
    int *new_data;
    int new_cap;

    if (vec->len == vec->cap)
      {
          new_cap = vec->cap ? vec->cap * 2 : 128;
          new_data = (int *) realloc (vec->data, sizeof (int) * (size_t) new_cap);
          if (!new_data)
              return 0;
          vec->data = new_data;
          vec->cap = new_cap;
      }
    vec->data[vec->len++] = value;
    return 1;
}

static int
mlt_u64_vec_push (struct mlt_u64_vec *vec, unsigned long long value)
{
    unsigned long long *new_data;
    int new_cap;

    if (vec->len == vec->cap)
      {
          new_cap = vec->cap ? vec->cap * 2 : 64;
          new_data = (unsigned long long *) realloc (vec->data,
                                                     sizeof (unsigned long long) * (size_t) new_cap);
          if (!new_data)
              return 0;
          vec->data = new_data;
          vec->cap = new_cap;
      }
    vec->data[vec->len++] = value;
    return 1;
}

static void
mlt_ctx_free (struct mlt_ctx *ctx)
{
    int i;
    int j;

    if (ctx->layer_name)
        free (ctx->layer_name);
    ctx->layer_name = NULL;
    mlt_u32_vec_free (&ctx->geom_types);
    mlt_i32_vec_free (&ctx->vertices);
    mlt_u32_vec_free (&ctx->geometry_lengths);
    mlt_u32_vec_free (&ctx->part_lengths);
    mlt_u32_vec_free (&ctx->ring_lengths);
    mlt_u64_vec_free (&ctx->ids);
    for (i = 0; i < ctx->prop_count; i++)
      {
          if (ctx->props[i].name)
              free (ctx->props[i].name);
          for (j = 0; j < ctx->props[i].len; j++)
            {
                if (ctx->props[i].values[j])
                    free (ctx->props[i].values[j]);
            }
          free (ctx->props[i].values);
          free (ctx->props[i].present);
      }
    free (ctx->props);
    ctx->props = NULL;
    ctx->prop_count = 0;
    ctx->prop_cap = 0;
}

static int
mlt_prop_reserve (struct mlt_prop_col *col, int count)
{
    int new_cap;
    char **new_values;
    unsigned char *new_present;

    if (count <= col->cap)
        return 1;
    new_cap = col->cap ? col->cap * 2 : 64;
    while (new_cap < count)
        new_cap *= 2;
    new_values = (char **) realloc (col->values, sizeof (char *) * (size_t) new_cap);
    if (!new_values)
        return 0;
    col->values = new_values;
    new_present = (unsigned char *) realloc (col->present, (size_t) new_cap);
    if (!new_present)
        return 0;
    col->present = new_present;
    while (col->cap < new_cap)
      {
          col->values[col->cap] = NULL;
          col->present[col->cap] = 0;
          col->cap++;
      }
    return 1;
}

static int
mlt_prop_ensure_len (struct mlt_prop_col *col, int len)
{
    if (!mlt_prop_reserve (col, len))
        return 0;
    while (col->len < len)
      {
          col->values[col->len] = NULL;
          col->present[col->len] = 0;
          col->len++;
      }
    return 1;
}

static int
mlt_prop_col_id (struct mlt_ctx *ctx, const char *name, int name_len)
{
    int i;
    int new_cap;
    struct mlt_prop_col *new_props;

    if (!name || name_len <= 0)
        return -1;
    for (i = 0; i < ctx->prop_count; i++)
      {
          if ((int) strlen (ctx->props[i].name) == name_len
              && memcmp (ctx->props[i].name, name, (size_t) name_len) == 0)
              return i;
      }
    if (ctx->prop_count == ctx->prop_cap)
      {
          new_cap = ctx->prop_cap ? ctx->prop_cap * 2 : 16;
          new_props = (struct mlt_prop_col *) realloc (ctx->props,
                                                       sizeof (struct mlt_prop_col) * (size_t) new_cap);
          if (!new_props)
              return -1;
          ctx->props = new_props;
          ctx->prop_cap = new_cap;
      }
    memset (ctx->props + ctx->prop_count, 0, sizeof (struct mlt_prop_col));
    ctx->props[ctx->prop_count].name = mvt_strndup (name, name_len);
    if (!ctx->props[ctx->prop_count].name)
        return -1;
    if (!mlt_prop_ensure_len (ctx->props + ctx->prop_count, ctx->feature_count))
        return -1;
    return ctx->prop_count++;
}

static int
mlt_prop_set (struct mlt_ctx *ctx, int col_id, int feature_index, char *value)
{
    struct mlt_prop_col *col;

    if (col_id < 0 || col_id >= ctx->prop_count)
        return 0;
    col = ctx->props + col_id;
    if (!mlt_prop_ensure_len (col, feature_index + 1))
        return 0;
    if (col->values[feature_index])
        free (col->values[feature_index]);
    col->values[feature_index] = value;
    col->present[feature_index] = value ? 1 : 0;
    return 1;
}

static int
mvt_round_coord (double value)
{
    if (value >= 0.0)
        return (int) (value + 0.5);
    return (int) (value - 0.5);
}

static void
mvt_json_skip_ws (const char **ptr)
{
    while (**ptr && isspace ((unsigned char) **ptr))
        (*ptr)++;
}

static int
mvt_json_hex (char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static void
mvt_json_append_utf8 (struct mvt_buf *buf, unsigned int code)
{
    if (code <= 0x7f)
        mvt_buf_put_byte (buf, (unsigned char) code);
    else if (code <= 0x7ff)
      {
          mvt_buf_put_byte (buf, (unsigned char) (0xc0 | (code >> 6)));
          mvt_buf_put_byte (buf, (unsigned char) (0x80 | (code & 0x3f)));
      }
    else
      {
          mvt_buf_put_byte (buf, (unsigned char) (0xe0 | (code >> 12)));
          mvt_buf_put_byte (buf, (unsigned char) (0x80 | ((code >> 6) & 0x3f)));
          mvt_buf_put_byte (buf, (unsigned char) (0x80 | (code & 0x3f)));
      }
}

static char *
mvt_json_parse_string (const char **ptr, int *out_len)
{
    struct mvt_buf buf;
    const char *p;
    int i;
    int hex;
    unsigned int code;
    char *text;

    *out_len = 0;
    mvt_json_skip_ws (ptr);
    if (**ptr != '"')
        return NULL;
    (*ptr)++;
    mvt_buf_init (&buf);
    p = *ptr;
    while (*p)
      {
          if (*p == '"')
            {
                p++;
                text = mvt_strndup ((const char *) buf.data, buf.len);
                *out_len = buf.len;
                mvt_buf_free (&buf);
                *ptr = p;
                return text;
            }
          if (*p == '\\')
            {
                p++;
                if (!*p)
                    break;
                switch (*p)
                  {
                  case '"':
                  case '\\':
                  case '/':
                      mvt_buf_put_byte (&buf, (unsigned char) *p);
                      p++;
                      break;
                  case 'b':
                      mvt_buf_put_byte (&buf, '\b');
                      p++;
                      break;
                  case 'f':
                      mvt_buf_put_byte (&buf, '\f');
                      p++;
                      break;
                  case 'n':
                      mvt_buf_put_byte (&buf, '\n');
                      p++;
                      break;
                  case 'r':
                      mvt_buf_put_byte (&buf, '\r');
                      p++;
                      break;
                  case 't':
                      mvt_buf_put_byte (&buf, '\t');
                      p++;
                      break;
                  case 'u':
                      p++;
                      code = 0;
                      for (i = 0; i < 4; i++)
                        {
                            hex = mvt_json_hex (p[i]);
                            if (hex < 0)
                              {
                                  mvt_buf_free (&buf);
                                  return NULL;
                              }
                            code = (code << 4) | (unsigned int) hex;
                        }
                      mvt_json_append_utf8 (&buf, code);
                      p += 4;
                      break;
                  default:
                      mvt_buf_free (&buf);
                      return NULL;
                  }
            }
          else
            {
                mvt_buf_put_byte (&buf, (unsigned char) *p);
                p++;
            }
          if (buf.error)
            {
                mvt_buf_free (&buf);
                return NULL;
            }
      }
    mvt_buf_free (&buf);
    return NULL;
}

static void
mvt_json_skip_string (const char **ptr)
{
    int len;
    char *text = mvt_json_parse_string (ptr, &len);
    if (text)
        free (text);
}

static void
mvt_json_skip_value (const char **ptr)
{
    int depth = 0;
    int in_string = 0;
    int escape = 0;

    mvt_json_skip_ws (ptr);
    if (**ptr == '"')
      {
          mvt_json_skip_string (ptr);
          return;
      }
    if (**ptr == '{' || **ptr == '[')
      {
          do
            {
                if (in_string)
                  {
                      if (escape)
                          escape = 0;
                      else if (**ptr == '\\')
                          escape = 1;
                      else if (**ptr == '"')
                          in_string = 0;
                  }
                else
                  {
                      if (**ptr == '"')
                          in_string = 1;
                      else if (**ptr == '{' || **ptr == '[')
                          depth++;
                      else if (**ptr == '}' || **ptr == ']')
                          depth--;
                  }
                (*ptr)++;
            }
          while (**ptr && depth > 0);
          return;
      }
    while (**ptr && **ptr != ',' && **ptr != '}')
        (*ptr)++;
}

static char *
mlt_json_scalar_to_string (const char **ptr)
{
    const char *start;
    char *text;
    int text_len;

    mvt_json_skip_ws (ptr);
    if (**ptr == '"')
        return mvt_json_parse_string (ptr, &text_len);
    if (strncmp (*ptr, "true", 4) == 0)
      {
          *ptr += 4;
          return mvt_strndup ("true", 4);
      }
    if (strncmp (*ptr, "false", 5) == 0)
      {
          *ptr += 5;
          return mvt_strndup ("false", 5);
      }
    if (strncmp (*ptr, "null", 4) == 0)
      {
          *ptr += 4;
          return NULL;
      }
    if (**ptr == '{' || **ptr == '[')
      {
          mvt_json_skip_value (ptr);
          return NULL;
      }
    start = *ptr;
    while (**ptr && **ptr != ',' && **ptr != '}' && !isspace ((unsigned char) **ptr))
        (*ptr)++;
    if (*ptr == start)
      {
          mvt_json_skip_value (ptr);
          return NULL;
      }
    text = mvt_strndup (start, (int) (*ptr - start));
    return text;
}

static void
mlt_append_json_properties (struct mlt_ctx *ctx, const char *json, int feature_index)
{
    const char *p = json;
    char *key;
    char *value;
    int key_len;
    int col_id;

    if (!json)
        return;
    mvt_json_skip_ws (&p);
    if (*p != '{')
        return;
    p++;
    while (*p)
      {
          mvt_json_skip_ws (&p);
          if (*p == '}')
              return;
          key = mvt_json_parse_string (&p, &key_len);
          if (!key)
              return;
          mvt_json_skip_ws (&p);
          if (*p != ':')
            {
                free (key);
                return;
            }
          p++;
          col_id = mlt_prop_col_id (ctx, key, key_len);
          free (key);
          if (col_id < 0)
            {
                ctx->error = 1;
                return;
            }
          value = mlt_json_scalar_to_string (&p);
          if (value && !mlt_prop_set (ctx, col_id, feature_index, value))
            {
                free (value);
                ctx->error = 1;
                return;
            }
          mvt_json_skip_ws (&p);
          if (*p == ',')
            {
                p++;
                continue;
            }
          if (*p == '}')
              return;
      }
}

static int
mlt_append_point_geom (struct mlt_ctx *ctx, gaiaPointPtr point)
{
    if (!mlt_u32_vec_push (&ctx->geom_types, 0))
        return 0;
    return mlt_i32_vec_push (&ctx->vertices, mvt_round_coord (point->X))
        && mlt_i32_vec_push (&ctx->vertices, mvt_round_coord (point->Y));
}

static int
mlt_append_multipoint_geom (struct mlt_ctx *ctx, gaiaPointPtr first)
{
    int old_vertices = ctx->vertices.len;
    unsigned int count = 0;
    gaiaPointPtr point;

    for (point = first; point; point = point->Next)
      {
          if (!mlt_i32_vec_push (&ctx->vertices, mvt_round_coord (point->X))
              || !mlt_i32_vec_push (&ctx->vertices, mvt_round_coord (point->Y)))
            {
                ctx->vertices.len = old_vertices;
                return 0;
            }
          count++;
      }
    if (count == 0)
        return 1;
    if (!mlt_u32_vec_push (&ctx->geometry_lengths, count)
        || !mlt_u32_vec_push (&ctx->geom_types, 3))
      {
          ctx->vertices.len = old_vertices;
          return 0;
      }
    ctx->has_multipoint = 1;
    return 1;
}

static int
mlt_append_line_vertices (struct mlt_ctx *ctx, gaiaLinestringPtr line)
{
    int i;
    int count = 0;
    int prev_x = 0;
    int prev_y = 0;
    int x;
    int y;
    double dx;
    double dy;
    double z;
    double m;

    for (i = 0; i < line->Points; i++)
      {
          gaiaLineGetPoint (line, i, &dx, &dy, &z, &m);
          x = mvt_round_coord (dx);
          y = mvt_round_coord (dy);
          if (count > 0 && x == prev_x && y == prev_y)
              continue;
          if (!mlt_i32_vec_push (&ctx->vertices, x) || !mlt_i32_vec_push (&ctx->vertices, y))
              return 0;
          prev_x = x;
          prev_y = y;
          count++;
      }
    if (count < 2)
        return 0;
    return mlt_u32_vec_push (&ctx->ring_lengths, (unsigned int) count);
}

static int
mlt_append_line_geom (struct mlt_ctx *ctx, gaiaLinestringPtr line)
{
    int old_vertices = ctx->vertices.len;
    int old_rings = ctx->ring_lengths.len;

    if (!mlt_append_line_vertices (ctx, line))
      {
          ctx->vertices.len = old_vertices;
          ctx->ring_lengths.len = old_rings;
          return 1;
      }
    return mlt_u32_vec_push (&ctx->geom_types, 1);
}

static int
mlt_append_multiline_geom (struct mlt_ctx *ctx, gaiaLinestringPtr first)
{
    int old_vertices = ctx->vertices.len;
    int old_geometry = ctx->geometry_lengths.len;
    int old_rings = ctx->ring_lengths.len;
    unsigned int line_count = 0;
    gaiaLinestringPtr line;

    for (line = first; line; line = line->Next)
      {
          if (mlt_append_line_vertices (ctx, line))
              line_count++;
          else
            {
                ctx->vertices.len = old_vertices;
                ctx->geometry_lengths.len = old_geometry;
                ctx->ring_lengths.len = old_rings;
                return 0;
            }
      }
    if (line_count == 0)
      {
          ctx->vertices.len = old_vertices;
          ctx->ring_lengths.len = old_rings;
          return 1;
      }
    if (!mlt_u32_vec_push (&ctx->geometry_lengths, line_count)
        || !mlt_u32_vec_push (&ctx->geom_types, 4))
      {
          ctx->vertices.len = old_vertices;
          ctx->geometry_lengths.len = old_geometry;
          ctx->ring_lengths.len = old_rings;
          return 0;
      }
    ctx->has_multiline = 1;
    return 1;
}

static int
mlt_append_ring_vertices (struct mlt_ctx *ctx, gaiaRingPtr ring)
{
    int i;
    int limit = ring->Points > 0 ? ring->Points - 1 : 0;
    int count = 0;
    int prev_x = 0;
    int prev_y = 0;
    int x;
    int y;
    double dx;
    double dy;
    double z;
    double m;

    if (limit < 3)
        return 0;
    for (i = 0; i < limit; i++)
      {
          gaiaRingGetPoint (ring, i, &dx, &dy, &z, &m);
          x = mvt_round_coord (dx);
          y = mvt_round_coord (dy);
          if (count > 0 && x == prev_x && y == prev_y)
              continue;
          if (!mlt_i32_vec_push (&ctx->vertices, x) || !mlt_i32_vec_push (&ctx->vertices, y))
              return 0;
          prev_x = x;
          prev_y = y;
          count++;
      }
    if (count < 3)
        return 0;
    return mlt_u32_vec_push (&ctx->ring_lengths, (unsigned int) count);
}

static int
mlt_append_polygon_geom (struct mlt_ctx *ctx, gaiaPolygonPtr poly)
{
    int i;
    int old_vertices = ctx->vertices.len;
    int old_parts = ctx->part_lengths.len;
    int old_rings = ctx->ring_lengths.len;
    unsigned int ring_count = 0;

    if (mlt_append_ring_vertices (ctx, poly->Exterior))
        ring_count++;
    for (i = 0; i < poly->NumInteriors; i++)
      {
          if (mlt_append_ring_vertices (ctx, poly->Interiors + i))
              ring_count++;
      }
    if (ring_count == 0)
      {
          ctx->vertices.len = old_vertices;
          ctx->part_lengths.len = old_parts;
          ctx->ring_lengths.len = old_rings;
          return 1;
    }
    if (!mlt_u32_vec_push (&ctx->part_lengths, ring_count))
        return 0;
    ctx->has_polygon = 1;
    return mlt_u32_vec_push (&ctx->geom_types, 2);
}

static int
mlt_append_multipolygon_geom (struct mlt_ctx *ctx, gaiaPolygonPtr first)
{
    int i;
    int old_vertices = ctx->vertices.len;
    int old_geometry = ctx->geometry_lengths.len;
    int old_parts = ctx->part_lengths.len;
    int old_rings = ctx->ring_lengths.len;
    unsigned int polygon_count = 0;
    unsigned int ring_count;
    gaiaPolygonPtr poly;

    for (poly = first; poly; poly = poly->Next)
      {
          ring_count = 0;
          if (mlt_append_ring_vertices (ctx, poly->Exterior))
              ring_count++;
          for (i = 0; i < poly->NumInteriors; i++)
            {
                if (mlt_append_ring_vertices (ctx, poly->Interiors + i))
                    ring_count++;
            }
          if (ring_count > 0)
            {
                if (!mlt_u32_vec_push (&ctx->part_lengths, ring_count))
                  {
                      ctx->vertices.len = old_vertices;
                      ctx->geometry_lengths.len = old_geometry;
                      ctx->part_lengths.len = old_parts;
                      ctx->ring_lengths.len = old_rings;
                      return 0;
                  }
                polygon_count++;
            }
      }
    if (polygon_count == 0)
      {
          ctx->vertices.len = old_vertices;
          ctx->part_lengths.len = old_parts;
          ctx->ring_lengths.len = old_rings;
          return 1;
      }
    if (!mlt_u32_vec_push (&ctx->geometry_lengths, polygon_count)
        || !mlt_u32_vec_push (&ctx->geom_types, 5))
      {
          ctx->vertices.len = old_vertices;
          ctx->geometry_lengths.len = old_geometry;
          ctx->part_lengths.len = old_parts;
          ctx->ring_lengths.len = old_rings;
          return 0;
      }
    ctx->has_polygon = 1;
    ctx->has_multipolygon = 1;
    return 1;
}

static void
mlt_append_geom (struct mlt_ctx *ctx, gaiaGeomCollPtr geom, unsigned long long feature_id,
                 int has_feature_id)
{
    int old_count = ctx->geom_types.len;
    gaiaPointPtr point;
    gaiaLinestringPtr line;
    gaiaPolygonPtr poly;

    if (geom->DeclaredType == GAIA_MULTIPOLYGON || geom->DeclaredType == GAIA_MULTIPOLYGONZ
        || geom->DeclaredType == GAIA_MULTIPOLYGONM || geom->DeclaredType == GAIA_MULTIPOLYGONZM)
      {
          if (!mlt_append_multipolygon_geom (ctx, geom->FirstPolygon))
              ctx->error = 1;
      }
    else if (geom->DeclaredType == GAIA_MULTILINESTRING || geom->DeclaredType == GAIA_MULTILINESTRINGZ
             || geom->DeclaredType == GAIA_MULTILINESTRINGM || geom->DeclaredType == GAIA_MULTILINESTRINGZM)
      {
          if (!mlt_append_multiline_geom (ctx, geom->FirstLinestring))
              ctx->error = 1;
      }
    else if (geom->DeclaredType == GAIA_MULTIPOINT || geom->DeclaredType == GAIA_MULTIPOINTZ
             || geom->DeclaredType == GAIA_MULTIPOINTM || geom->DeclaredType == GAIA_MULTIPOINTZM)
      {
          if (!mlt_append_multipoint_geom (ctx, geom->FirstPoint))
              ctx->error = 1;
      }
    else
      {
          for (poly = geom->FirstPolygon; poly; poly = poly->Next)
            {
                if (!mlt_append_polygon_geom (ctx, poly))
                    ctx->error = 1;
            }
          for (line = geom->FirstLinestring; line; line = line->Next)
            {
                if (!mlt_append_line_geom (ctx, line))
                    ctx->error = 1;
            }
          for (point = geom->FirstPoint; point; point = point->Next)
            {
                if (!mlt_append_point_geom (ctx, point))
                    ctx->error = 1;
            }
      }
    while (ctx->ids.len < ctx->geom_types.len)
      {
          if (!mlt_u64_vec_push (&ctx->ids, has_feature_id ? feature_id : 0))
            {
                ctx->error = 1;
                return;
            }
      }
    if (has_feature_id && ctx->geom_types.len > old_count)
        ctx->has_id = 1;
}

static void
mlt_write_u32_varint_stream (struct mvt_buf *out, int physical_type, int logical_type,
                             struct mlt_u32_vec *values)
{
    struct mvt_buf data;
    int i;

    mvt_buf_init (&data);
    for (i = 0; i < values->len; i++)
        mvt_buf_put_varint64 (&data, (unsigned long long) values->data[i]);
    mlt_buf_put_stream_meta (out, physical_type, logical_type, 0, 0, 2, values->len, data.len);
    mvt_buf_put_data (out, data.data, data.len);
    if (data.error)
        out->error = 1;
    mvt_buf_free (&data);
}

static void
mlt_write_i32_varint_stream (struct mvt_buf *out, int physical_type, int logical_type,
                             struct mlt_i32_vec *values)
{
    struct mvt_buf data;
    int i;

    mvt_buf_init (&data);
    for (i = 0; i < values->len; i++)
        mvt_buf_put_varint64 (&data, (unsigned long long) mvt_zigzag (values->data[i]));
    mlt_buf_put_stream_meta (out, physical_type, logical_type, 0, 0, 2, values->len, data.len);
    mvt_buf_put_data (out, data.data, data.len);
    if (data.error)
        out->error = 1;
    mvt_buf_free (&data);
}

static void
mlt_write_u64_varint_stream (struct mvt_buf *out, int physical_type, int logical_type,
                             struct mlt_u64_vec *values)
{
    struct mvt_buf data;
    int i;

    mvt_buf_init (&data);
    for (i = 0; i < values->len; i++)
        mvt_buf_put_varint64 (&data, values->data[i]);
    mlt_buf_put_stream_meta (out, physical_type, logical_type, 0, 0, 2, values->len, data.len);
    mvt_buf_put_data (out, data.data, data.len);
    if (data.error)
        out->error = 1;
    mvt_buf_free (&data);
}

static void
mlt_copy_line_lengths_to_part_lengths (struct mlt_u32_vec *parts, struct mlt_u32_vec *rings)
{
    int i;

    parts->len = 0;
    for (i = 0; i < rings->len; i++)
      {
          if (!mlt_u32_vec_push (parts, rings->data[i]))
              return;
      }
}

static void
mlt_write_bool_present_stream (struct mvt_buf *out, unsigned char *present, int count)
{
    struct mvt_buf data;
    int i;
    int byte_count = (count + 7) / 8;

    mvt_buf_init (&data);
    mvt_buf_put_byte (&data, (unsigned char) (256 - byte_count));
    for (i = 0; i < byte_count; i++)
        mvt_buf_put_byte (&data, 0);
    for (i = 0; i < count; i++)
      {
          if (present[i])
              data.data[1 + (i / 8)] |= (unsigned char) (1 << (i % 8));
      }
    mlt_buf_put_stream_meta (out, 0, 0, 0, 0, 0, count, data.len);
    mvt_buf_put_data (out, data.data, data.len);
    if (data.error)
        out->error = 1;
    mvt_buf_free (&data);
}

static void
mlt_write_string_property (struct mvt_buf *out, struct mlt_prop_col *col, int feature_count)
{
    struct mlt_u32_vec lengths;
    struct mvt_buf text;
    int i;
    int len;

    memset (&lengths, 0, sizeof (lengths));
    mvt_buf_init (&text);
    if (!mlt_prop_ensure_len (col, feature_count))
      {
          out->error = 1;
          return;
      }
    mlt_write_bool_present_stream (out, col->present, feature_count);
    for (i = 0; i < feature_count; i++)
      {
          if (!col->present[i])
              continue;
          len = (int) strlen (col->values[i]);
          if (!mlt_u32_vec_push (&lengths, (unsigned int) len))
            {
                out->error = 1;
                break;
            }
          mvt_buf_put_data (&text, (const unsigned char *) col->values[i], len);
      }
    if (!out->error)
      {
          mlt_write_u32_varint_stream (out, 3, 0, &lengths);
          mlt_buf_put_stream_meta (out, 1, 0, 0, 0, 0, text.len, text.len);
          mvt_buf_put_data (out, text.data, text.len);
      }
    if (text.error)
        out->error = 1;
    mlt_u32_vec_free (&lengths);
    mvt_buf_free (&text);
}

static void
mlt_build_tile (struct mlt_ctx *ctx, struct mvt_buf *tile)
{
    struct mvt_buf body;
    struct mlt_u32_vec line_part_lengths;
    struct mlt_u32_vec *part_lengths;
    struct mlt_u32_vec *ring_lengths;
    int column_count;
    int geom_streams = 2;
    int i;

    memset (&line_part_lengths, 0, sizeof (line_part_lengths));
    part_lengths = &ctx->part_lengths;
    ring_lengths = &ctx->ring_lengths;
    if (!ctx->has_polygon && ctx->ring_lengths.len > 0)
      {
          mlt_copy_line_lengths_to_part_lengths (&line_part_lengths, &ctx->ring_lengths);
          if (line_part_lengths.len != ctx->ring_lengths.len)
            {
                tile->error = 1;
                mlt_u32_vec_free (&line_part_lengths);
                return;
            }
          part_lengths = &line_part_lengths;
          ring_lengths = NULL;
      }
    mvt_buf_init (&body);
    column_count = (ctx->has_id ? 2 : 1) + ctx->prop_count;
    mlt_buf_put_string (&body, ctx->layer_name ? ctx->layer_name : "default");
    mvt_buf_put_varint64 (&body, (unsigned long long) ctx->extent);
    mvt_buf_put_varint64 (&body, (unsigned long long) column_count);
    if (ctx->has_id)
        mvt_buf_put_varint64 (&body, 2); /* UINT64 logical id, non-nullable */
    mvt_buf_put_varint64 (&body, 4); /* geometry */
    for (i = 0; i < ctx->prop_count; i++)
      {
          mvt_buf_put_varint64 (&body, 29); /* nullable string property */
          mlt_buf_put_string (&body, ctx->props[i].name);
      }

    if (ctx->has_id)
        mlt_write_u64_varint_stream (&body, 1, 0, &ctx->ids);
    if (ctx->geometry_lengths.len > 0)
        geom_streams++;
    if (part_lengths && part_lengths->len > 0)
        geom_streams++;
    if (ring_lengths && ring_lengths->len > 0)
        geom_streams++;
    mvt_buf_put_varint64 (&body, (unsigned long long) geom_streams);
    mlt_write_u32_varint_stream (&body, 1, 0, &ctx->geom_types);
    if (ctx->geometry_lengths.len > 0)
        mlt_write_u32_varint_stream (&body, 3, 1, &ctx->geometry_lengths);
    if (part_lengths && part_lengths->len > 0)
        mlt_write_u32_varint_stream (&body, 3, 2, part_lengths);
    if (ring_lengths && ring_lengths->len > 0)
        mlt_write_u32_varint_stream (&body, 3, 3, ring_lengths);
    mlt_write_i32_varint_stream (&body, 1, 3, &ctx->vertices);
    for (i = 0; i < ctx->prop_count; i++)
      {
          mvt_buf_put_varint64 (&body, 3); /* present, length, data */
          mlt_write_string_property (&body, ctx->props + i, ctx->feature_count);
      }

    mvt_buf_put_varint64 (tile, (unsigned long long) body.len + 1);
    mvt_buf_put_byte (tile, 1);
    mvt_buf_put_data (tile, body.data, body.len);
    if (body.error)
        tile->error = 1;
    mlt_u32_vec_free (&line_part_lengths);
    mvt_buf_free (&body);
}

static void
fnct_AsMLT_step (sqlite3_context * context, int argc, sqlite3_value ** argv)
{
    struct mlt_ctx *ctx;
    const unsigned char *blob;
    int blob_size;
    gaiaGeomCollPtr geom;
    const char *name;
    sqlite3_int64 raw_id;
    unsigned long long feature_id = 0;
    int has_feature_id = 0;
    const char *properties_json = NULL;
    int old_count;
    int new_count;
    int feature_index;
    int i;

    ctx = (struct mlt_ctx *) sqlite3_aggregate_context (context, sizeof (struct mlt_ctx));
    if (!ctx)
        return;
    if (!ctx->layer_name && ctx->geom_types.cap == 0)
      {
          memset (ctx, 0, sizeof (struct mlt_ctx));
          ctx->extent = 4096;
          ctx->layer_name = (char *) malloc (8);
          if (ctx->layer_name)
              strcpy (ctx->layer_name, "default");
          else
              ctx->error = 1;
      }
    if (ctx->error || argc < 1 || sqlite3_value_type (argv[0]) == SQLITE_NULL)
        return;
    if (argc > 1 && sqlite3_value_type (argv[1]) == SQLITE_TEXT && ctx->geom_types.len == 0)
      {
          name = (const char *) sqlite3_value_text (argv[1]);
          if (name)
            {
                free (ctx->layer_name);
                ctx->layer_name = (char *) malloc (strlen (name) + 1);
                if (ctx->layer_name)
                    strcpy (ctx->layer_name, name);
                else
                    ctx->error = 1;
            }
      }
    if (argc > 2 && sqlite3_value_type (argv[2]) != SQLITE_NULL && ctx->geom_types.len == 0)
        ctx->extent = sqlite3_value_int (argv[2]);
    if (argc > 3 && sqlite3_value_type (argv[3]) != SQLITE_NULL)
      {
          if (sqlite3_value_type (argv[3]) == SQLITE_TEXT)
              properties_json = (const char *) sqlite3_value_text (argv[3]);
          else
            {
                raw_id = sqlite3_value_int64 (argv[3]);
                if (raw_id >= 0)
                  {
                      feature_id = (unsigned long long) raw_id;
                      has_feature_id = 1;
                  }
            }
      }
    if (argc > 4 && sqlite3_value_type (argv[4]) != SQLITE_NULL)
      {
          raw_id = sqlite3_value_int64 (argv[4]);
          if (raw_id >= 0)
            {
                feature_id = (unsigned long long) raw_id;
                has_feature_id = 1;
            }
      }
    if (sqlite3_value_type (argv[0]) != SQLITE_BLOB)
        return;
    blob = sqlite3_value_blob (argv[0]);
    blob_size = sqlite3_value_bytes (argv[0]);
    geom = gaiaFromSpatiaLiteBlobWkb (blob, blob_size);
    if (!geom)
        return;
    old_count = ctx->geom_types.len;
    mlt_append_geom (ctx, geom, feature_id, has_feature_id);
    gaiaFreeGeomColl (geom);
    new_count = ctx->geom_types.len;
    for (feature_index = old_count; feature_index < new_count; feature_index++)
      {
          for (i = 0; i < ctx->prop_count; i++)
            {
                if (!mlt_prop_ensure_len (ctx->props + i, feature_index + 1))
                  {
                      ctx->error = 1;
                      return;
                  }
            }
          if (properties_json)
              mlt_append_json_properties (ctx, properties_json, feature_index);
          if (ctx->error)
              return;
          ctx->feature_count++;
      }
}

static void
fnct_AsMLT_final (sqlite3_context * context)
{
    struct mlt_ctx *ctx;
    struct mvt_buf tile;

    ctx = (struct mlt_ctx *) sqlite3_aggregate_context (context, 0);
    if (!ctx || ctx->error || ctx->geom_types.len == 0)
      {
          if (ctx)
              mlt_ctx_free (ctx);
          sqlite3_result_null (context);
          return;
      }
    mvt_buf_init (&tile);
    mlt_build_tile (ctx, &tile);
    if (tile.error)
        sqlite3_result_null (context);
    else
        sqlite3_result_blob (context, tile.data, tile.len, SQLITE_TRANSIENT);
    mvt_buf_free (&tile);
    mlt_ctx_free (ctx);
}

void
register_spatialite_mlt_sql_functions (sqlite3 *db)
{
    sqlite3_create_function_v2 (db, "AsMLT", -1, SQLITE_UTF8, 0,
                                0, fnct_AsMLT_step, fnct_AsMLT_final, 0);
    sqlite3_create_function_v2 (db, "ST_AsMLT", -1, SQLITE_UTF8, 0,
                                0, fnct_AsMLT_step, fnct_AsMLT_final, 0);
}
