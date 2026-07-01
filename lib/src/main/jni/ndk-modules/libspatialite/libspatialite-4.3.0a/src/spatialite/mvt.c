/*
 mvt.c -- lightweight Mapbox Vector Tile helpers for android-spatialite

 This module intentionally implements a small first version: AsMVTGeom()
 transforms SpatiaLite geometries into tile coordinates, and AsMVT()
 aggregates features into a MVT v2 PBF layer.
*/

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

#if defined(_WIN32) && !defined(__MINGW32__)
#include "config-msvc.h"
#else
#include "config.h"
#endif

#include <spatialite/sqlite.h>
#include <spatialite/gaiageo.h>
#include <spatialite/gaiaaux.h>

#define MVT_CMD_MOVE_TO 1
#define MVT_CMD_LINE_TO 2
#define MVT_CMD_CLOSE_PATH 7

#define MVT_GEOM_POINT 1
#define MVT_GEOM_LINE 2
#define MVT_GEOM_POLYGON 3

struct mvt_buf
{
    unsigned char *data;
    int len;
    int cap;
    int error;
};

struct mvt_key
{
    char *text;
};

struct mvt_value
{
    int type;
    char *text;
    double number;
    long long int_value;
    unsigned long long uint_value;
    int boolean;
    struct mvt_buf encoded;
};

#define MVT_VALUE_STRING 1
#define MVT_VALUE_DOUBLE 2
#define MVT_VALUE_BOOL 3
#define MVT_VALUE_INT 4
#define MVT_VALUE_UINT 5
#define MVT_VALUE_SINT 6

struct mvt_pt
{
    int x;
    int y;
};

struct mvt_ctx
{
    char *layer_name;
    int extent;
    struct mvt_buf features;
    struct mvt_key *keys;
    int key_count;
    int key_cap;
    struct mvt_value *values;
    int value_count;
    int value_cap;
    int feature_count;
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
mvt_buf_put_key (struct mvt_buf *buf, int field, int wire_type)
{
    mvt_buf_put_varint64 (buf, (unsigned long long) ((field << 3) | wire_type));
}

static void
mvt_buf_put_varint_field (struct mvt_buf *buf, int field, unsigned long long value)
{
    mvt_buf_put_key (buf, field, 0);
    mvt_buf_put_varint64 (buf, value);
}

static void
mvt_buf_put_fixed64_field (struct mvt_buf *buf, int field, double value)
{
    union
    {
        double d;
        unsigned long long u;
    } conv;
    int i;

    conv.d = value;
    mvt_buf_put_key (buf, field, 1);
    for (i = 0; i < 8; i++)
        mvt_buf_put_byte (buf, (unsigned char) ((conv.u >> (i * 8)) & 0xff));
}

static void
mvt_buf_put_bytes_field (struct mvt_buf *buf, int field, const unsigned char *data, int len)
{
    mvt_buf_put_key (buf, field, 2);
    mvt_buf_put_varint64 (buf, (unsigned long long) len);
    mvt_buf_put_data (buf, data, len);
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
mvt_command (int id, int count)
{
    return (unsigned int) ((id & 0x7) | (count << 3));
}

static unsigned int
mvt_zigzag (int value)
{
    return (unsigned int) ((value << 1) ^ (value >> 31));
}

static unsigned long long
mvt_zigzag64 (long long value)
{
    return (unsigned long long) ((value << 1) ^ (value >> 63));
}

static void
mvt_ctx_free (struct mvt_ctx *ctx)
{
    int i;

    mvt_buf_free (&ctx->features);
    if (ctx->layer_name)
        free (ctx->layer_name);
    ctx->layer_name = NULL;
    for (i = 0; i < ctx->key_count; i++)
      {
          if (ctx->keys[i].text)
              free (ctx->keys[i].text);
      }
    free (ctx->keys);
    ctx->keys = NULL;
    ctx->key_count = 0;
    ctx->key_cap = 0;
    for (i = 0; i < ctx->value_count; i++)
      {
          if (ctx->values[i].text)
              free (ctx->values[i].text);
          mvt_buf_free (&ctx->values[i].encoded);
      }
    free (ctx->values);
    ctx->values = NULL;
    ctx->value_count = 0;
    ctx->value_cap = 0;
}

static int
mvt_key_id (struct mvt_ctx *ctx, const char *key, int key_len)
{
    int i;
    int new_cap;
    struct mvt_key *new_keys;

    if (!key || key_len <= 0)
        return -1;
    for (i = 0; i < ctx->key_count; i++)
      {
          if ((int) strlen (ctx->keys[i].text) == key_len
              && memcmp (ctx->keys[i].text, key, (size_t) key_len) == 0)
              return i;
      }
    if (ctx->key_count == ctx->key_cap)
      {
          new_cap = ctx->key_cap ? ctx->key_cap * 2 : 16;
          new_keys = (struct mvt_key *) realloc (ctx->keys, sizeof (struct mvt_key) * new_cap);
          if (!new_keys)
            {
                ctx->error = 1;
                return -1;
            }
          ctx->keys = new_keys;
          ctx->key_cap = new_cap;
      }
    ctx->keys[ctx->key_count].text = mvt_strndup (key, key_len);
    if (!ctx->keys[ctx->key_count].text)
      {
          ctx->error = 1;
          return -1;
      }
    return ctx->key_count++;
}

static int
mvt_values_equal (struct mvt_value *value, int type, const char *text, int text_len,
                  double number, long long int_value, unsigned long long uint_value,
                  int boolean)
{
    if (value->type != type)
        return 0;
    if (type == MVT_VALUE_STRING)
        return value->text && (int) strlen (value->text) == text_len
            && memcmp (value->text, text, (size_t) text_len) == 0;
    if (type == MVT_VALUE_DOUBLE)
        return value->number == number;
    if (type == MVT_VALUE_INT || type == MVT_VALUE_SINT)
        return value->int_value == int_value;
    if (type == MVT_VALUE_UINT)
        return value->uint_value == uint_value;
    if (type == MVT_VALUE_BOOL)
        return value->boolean == boolean;
    return 0;
}

static int
mvt_value_id (struct mvt_ctx *ctx, int type, const char *text, int text_len,
              double number, long long int_value, unsigned long long uint_value,
              int boolean)
{
    int i;
    int new_cap;
    struct mvt_value *new_values;
    struct mvt_value *value;

    for (i = 0; i < ctx->value_count; i++)
      {
          if (mvt_values_equal (ctx->values + i, type, text, text_len, number,
                                int_value, uint_value, boolean))
              return i;
      }
    if (ctx->value_count == ctx->value_cap)
      {
          new_cap = ctx->value_cap ? ctx->value_cap * 2 : 32;
          new_values = (struct mvt_value *) realloc (ctx->values, sizeof (struct mvt_value) * new_cap);
          if (!new_values)
            {
                ctx->error = 1;
                return -1;
            }
          ctx->values = new_values;
          ctx->value_cap = new_cap;
      }
    value = ctx->values + ctx->value_count;
    memset (value, 0, sizeof (struct mvt_value));
    mvt_buf_init (&value->encoded);
    value->type = type;
    if (type == MVT_VALUE_STRING)
      {
          value->text = mvt_strndup (text, text_len);
          if (!value->text)
            {
                ctx->error = 1;
                return -1;
            }
          mvt_buf_put_bytes_field (&value->encoded, 1, (const unsigned char *) value->text,
                                   (int) strlen (value->text));
      }
    else if (type == MVT_VALUE_DOUBLE)
      {
          value->number = number;
          mvt_buf_put_fixed64_field (&value->encoded, 3, number);
      }
    else if (type == MVT_VALUE_INT)
      {
          value->int_value = int_value;
          mvt_buf_put_varint_field (&value->encoded, 4, (unsigned long long) int_value);
      }
    else if (type == MVT_VALUE_UINT)
      {
          value->uint_value = uint_value;
          mvt_buf_put_varint_field (&value->encoded, 5, uint_value);
      }
    else if (type == MVT_VALUE_SINT)
      {
          value->int_value = int_value;
          mvt_buf_put_varint_field (&value->encoded, 6, mvt_zigzag64 (int_value));
      }
    else if (type == MVT_VALUE_BOOL)
      {
          value->boolean = boolean ? 1 : 0;
          mvt_buf_put_varint_field (&value->encoded, 7, (unsigned long long) value->boolean);
      }
    if (value->encoded.error)
      {
          ctx->error = 1;
          return -1;
      }
    return ctx->value_count++;
}

static int
mvt_round_coord (double value)
{
    if (value >= 0.0)
        return (int) (value + 0.5);
    return (int) (value - 0.5);
}

static int
mvt_parse_json_integer (const char *start, const char *end,
                        long long *int_value, unsigned long long *uint_value,
                        int *is_unsigned)
{
    const char *p;
    char *copy;
    char *parse_end;

    *is_unsigned = 0;
    for (p = start; p < end; p++)
      {
          if (*p == '.' || *p == 'e' || *p == 'E')
              return 0;
      }
    copy = mvt_strndup (start, (int) (end - start));
    if (!copy)
        return 0;
    errno = 0;
    if (copy[0] == '-')
      {
          *int_value = strtoll (copy, &parse_end, 10);
          if (errno == 0 && parse_end && *parse_end == '\0')
            {
                free (copy);
                return 1;
            }
      }
    else
      {
          *uint_value = strtoull (copy, &parse_end, 10);
          if (errno == 0 && parse_end && *parse_end == '\0')
            {
                if (*uint_value <= (unsigned long long) LLONG_MAX)
                    *int_value = (long long) *uint_value;
                *is_unsigned = *uint_value > (unsigned long long) LLONG_MAX;
                free (copy);
                return 1;
            }
      }
    free (copy);
    return 0;
}

static int
mvt_bounds_intersects (gaiaGeomCollPtr geom, double minx, double miny,
                       double maxx, double maxy, double buffer_map)
{
    double qminx = minx - buffer_map;
    double qminy = miny - buffer_map;
    double qmaxx = maxx + buffer_map;
    double qmaxy = maxy + buffer_map;

    if (geom->MinX > qmaxx || geom->MaxX < qminx)
        return 0;
    if (geom->MinY > qmaxy || geom->MaxY < qminy)
        return 0;
    return 1;
}

static int
mvt_bounds_large_enough (gaiaGeomCollPtr geom, double fx, double fy)
{
    double w = fabs ((geom->MaxX - geom->MinX) * fx);
    double h = fabs ((geom->MaxY - geom->MinY) * fy);

    if (geom->FirstPoint)
        return 1;
    if (geom->FirstPolygon)
        return w >= 0.5 || h >= 0.5;
    if (geom->FirstLinestring)
        return w >= 0.5 || h >= 0.5;
    return 0;
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

static void
mvt_append_json_tags (struct mvt_ctx *ctx, struct mvt_buf *tags, const char *json)
{
    const char *p = json;
    char *key;
    char *text;
    int key_len;
    int text_len;
    int key_id;
    int value_id;
    char *endptr;
    const char *number_start;
    double number;
    long long int_value;
    unsigned long long uint_value;
    int is_unsigned;

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
          mvt_json_skip_ws (&p);
          key_id = mvt_key_id (ctx, key, key_len);
          free (key);
          if (key_id < 0)
              return;

          value_id = -1;
          if (*p == '"')
            {
                text = mvt_json_parse_string (&p, &text_len);
                if (text)
                  {
                      value_id = mvt_value_id (ctx, MVT_VALUE_STRING, text, text_len,
                                               0.0, 0, 0, 0);
                      free (text);
                  }
            }
          else if (strncmp (p, "true", 4) == 0)
            {
                value_id = mvt_value_id (ctx, MVT_VALUE_BOOL, NULL, 0, 0.0, 0, 0, 1);
                p += 4;
            }
          else if (strncmp (p, "false", 5) == 0)
            {
                value_id = mvt_value_id (ctx, MVT_VALUE_BOOL, NULL, 0, 0.0, 0, 0, 0);
                p += 5;
            }
          else if (strncmp (p, "null", 4) == 0)
            {
                p += 4;
            }
          else if (*p == '{' || *p == '[')
            {
                mvt_json_skip_value (&p);
            }
          else
            {
                number_start = p;
                number = strtod (p, &endptr);
                if (endptr != p)
                  {
                      if (mvt_parse_json_integer (number_start, endptr, &int_value,
                                                  &uint_value, &is_unsigned))
                        {
                            if (is_unsigned)
                                value_id = mvt_value_id (ctx, MVT_VALUE_UINT, NULL, 0,
                                                         0.0, 0, uint_value, 0);
                            else if (int_value < 0)
                                value_id = mvt_value_id (ctx, MVT_VALUE_SINT, NULL, 0,
                                                         0.0, int_value, 0, 0);
                            else
                                value_id = mvt_value_id (ctx, MVT_VALUE_INT, NULL, 0,
                                                         0.0, int_value, 0, 0);
                        }
                      else
                          value_id = mvt_value_id (ctx, MVT_VALUE_DOUBLE, NULL, 0,
                                                   number, 0, 0, 0);
                      p = endptr;
                  }
                else
                    mvt_json_skip_value (&p);
            }
          if (ctx->error)
              return;
          if (value_id >= 0)
            {
                mvt_buf_put_varint64 (tags, (unsigned long long) key_id);
                mvt_buf_put_varint64 (tags, (unsigned long long) value_id);
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

static void
mvt_transform_point (double *x, double *y, double minx, double maxy,
                     double fx, double fy)
{
    *x = (*x - minx) * fx;
    *y = (maxy - *y) * fy;
    *x = (double) mvt_round_coord (*x);
    *y = (double) mvt_round_coord (*y);
}

static void
mvt_transform_ring (gaiaRingPtr ring, double minx, double maxy, double fx, double fy)
{
    int i;
    double x;
    double y;
    double z;
    double m;

    for (i = 0; i < ring->Points; i++)
      {
          gaiaRingGetPoint (ring, i, &x, &y, &z, &m);
          mvt_transform_point (&x, &y, minx, maxy, fx, fy);
          gaiaRingSetPoint (ring, i, x, y, z, m);
      }
}

static void
mvt_transform_geom (gaiaGeomCollPtr geom, double minx, double maxy,
                    double fx, double fy)
{
    gaiaPointPtr point;
    gaiaLinestringPtr line;
    gaiaPolygonPtr poly;
    int i;
    double x;
    double y;
    double z;
    double m;

    for (point = geom->FirstPoint; point; point = point->Next)
      {
          x = point->X;
          y = point->Y;
          mvt_transform_point (&x, &y, minx, maxy, fx, fy);
          point->X = x;
          point->Y = y;
      }
    for (line = geom->FirstLinestring; line; line = line->Next)
      {
          for (i = 0; i < line->Points; i++)
            {
                gaiaLineGetPoint (line, i, &x, &y, &z, &m);
                mvt_transform_point (&x, &y, minx, maxy, fx, fy);
                gaiaLineSetPoint (line, i, x, y, z, m);
            }
      }
    for (poly = geom->FirstPolygon; poly; poly = poly->Next)
      {
          mvt_transform_ring (poly->Exterior, minx, maxy, fx, fy);
          for (i = 0; i < poly->NumInteriors; i++)
              mvt_transform_ring (poly->Interiors + i, minx, maxy, fx, fy);
      }
    gaiaMbrGeometry (geom);
}

static int
mvt_geom_type (gaiaGeomCollPtr geom)
{
    if (geom->FirstPolygon)
        return MVT_GEOM_POLYGON;
    if (geom->FirstLinestring)
        return MVT_GEOM_LINE;
    if (geom->FirstPoint)
        return MVT_GEOM_POINT;
    return 0;
}

static void
mvt_encode_point_xy (struct mvt_buf *buf, int x, int y, int *px, int *py)
{
    mvt_buf_put_varint64 (buf, mvt_zigzag (x - *px));
    mvt_buf_put_varint64 (buf, mvt_zigzag (y - *py));
    *px = x;
    *py = y;
}

static int
mvt_same_pt (struct mvt_pt a, struct mvt_pt b)
{
    return a.x == b.x && a.y == b.y;
}

static long long
mvt_cross (struct mvt_pt a, struct mvt_pt b, struct mvt_pt c)
{
    return (long long) (b.x - a.x) * (long long) (c.y - b.y)
        - (long long) (b.y - a.y) * (long long) (c.x - b.x);
}

static double
mvt_ring_area (struct mvt_pt *pts, int n)
{
    double area = 0.0;
    int i;
    int j;

    if (n < 3)
        return 0.0;
    for (i = 0, j = n - 1; i < n; j = i++)
        area += ((double) pts[j].x * (double) pts[i].y)
            - ((double) pts[i].x * (double) pts[j].y);
    return area / 2.0;
}

static void
mvt_reverse_pts (struct mvt_pt *pts, int n)
{
    int i;
    struct mvt_pt tmp;

    for (i = 0; i < n / 2; i++)
      {
          tmp = pts[i];
          pts[i] = pts[n - i - 1];
          pts[n - i - 1] = tmp;
      }
}

static int
mvt_clean_pts (struct mvt_pt *pts, int n, int closed)
{
    int i;
    int out = 0;
    int changed;
    int prev;
    int next;

    for (i = 0; i < n; i++)
      {
          if (out == 0 || !mvt_same_pt (pts[out - 1], pts[i]))
              pts[out++] = pts[i];
      }
    n = out;
    if (closed && n > 1 && mvt_same_pt (pts[0], pts[n - 1]))
        n--;
    do
      {
          changed = 0;
          out = 0;
          for (i = 0; i < n; i++)
            {
                if (!closed && (i == 0 || i == n - 1))
                  {
                      pts[out++] = pts[i];
                      continue;
                  }
                prev = i == 0 ? n - 1 : i - 1;
                next = i == n - 1 ? 0 : i + 1;
                if (n > 2 && mvt_cross (pts[prev], pts[i], pts[next]) == 0)
                  {
                      changed = 1;
                      continue;
                  }
                pts[out++] = pts[i];
            }
          n = out;
      }
    while (closed && changed && n > 3);
    return n;
}

static void
mvt_encode_pts (struct mvt_buf *buf, struct mvt_pt *pts, int n, int close_path,
                int *px, int *py)
{
    int i;

    if (n <= 0)
        return;
    mvt_buf_put_varint64 (buf, mvt_command (MVT_CMD_MOVE_TO, 1));
    mvt_encode_point_xy (buf, pts[0].x, pts[0].y, px, py);
    if (n > 1)
      {
          mvt_buf_put_varint64 (buf, mvt_command (MVT_CMD_LINE_TO, n - 1));
          for (i = 1; i < n; i++)
              mvt_encode_point_xy (buf, pts[i].x, pts[i].y, px, py);
      }
    if (close_path)
        mvt_buf_put_varint64 (buf, mvt_command (MVT_CMD_CLOSE_PATH, 1));
}

static int
mvt_clip_outcode (double x, double y, double min, double max)
{
    int code = 0;

    if (x < min)
        code |= 1;
    else if (x > max)
        code |= 2;
    if (y < min)
        code |= 4;
    else if (y > max)
        code |= 8;
    return code;
}

static int
mvt_clip_segment (double *x0, double *y0, double *x1, double *y1,
                  double min, double max)
{
    int out0 = mvt_clip_outcode (*x0, *y0, min, max);
    int out1 = mvt_clip_outcode (*x1, *y1, min, max);
    int out;
    double x;
    double y;

    while (1)
      {
          if (!(out0 | out1))
              return 1;
          if (out0 & out1)
              return 0;
          out = out0 ? out0 : out1;
          if (out & 8)
            {
                x = *x0 + (*x1 - *x0) * (max - *y0) / (*y1 - *y0);
                y = max;
            }
          else if (out & 4)
            {
                x = *x0 + (*x1 - *x0) * (min - *y0) / (*y1 - *y0);
                y = min;
            }
          else if (out & 2)
            {
                y = *y0 + (*y1 - *y0) * (max - *x0) / (*x1 - *x0);
                x = max;
            }
          else
            {
                y = *y0 + (*y1 - *y0) * (min - *x0) / (*x1 - *x0);
                x = min;
            }
          if (out == out0)
            {
                *x0 = x;
                *y0 = y;
                out0 = mvt_clip_outcode (*x0, *y0, min, max);
            }
          else
            {
                *x1 = x;
                *y1 = y;
                out1 = mvt_clip_outcode (*x1, *y1, min, max);
            }
      }
}

static int
mvt_clip_inside (struct mvt_pt p, int edge, int min, int max)
{
    if (edge == 0)
        return p.x >= min;
    if (edge == 1)
        return p.x <= max;
    if (edge == 2)
        return p.y >= min;
    return p.y <= max;
}

static struct mvt_pt
mvt_clip_intersection (struct mvt_pt a, struct mvt_pt b, int edge, int min, int max)
{
    struct mvt_pt out = a;
    double x1 = (double) a.x;
    double y1 = (double) a.y;
    double x2 = (double) b.x;
    double y2 = (double) b.y;
    double t;

    if (edge == 0 || edge == 1)
      {
          double x = edge == 0 ? (double) min : (double) max;
          if (x2 != x1)
              t = (x - x1) / (x2 - x1);
          else
              t = 0.0;
          out.x = mvt_round_coord (x);
          out.y = mvt_round_coord (y1 + t * (y2 - y1));
      }
    else
      {
          double y = edge == 2 ? (double) min : (double) max;
          if (y2 != y1)
              t = (y - y1) / (y2 - y1);
          else
              t = 0.0;
          out.x = mvt_round_coord (x1 + t * (x2 - x1));
          out.y = mvt_round_coord (y);
      }
    return out;
}

static int
mvt_clip_polygon_edge (struct mvt_pt *in, int in_count, struct mvt_pt *out,
                       int edge, int min, int max)
{
    int i;
    int out_count = 0;
    struct mvt_pt prev;
    struct mvt_pt cur;
    int prev_inside;
    int cur_inside;

    if (in_count <= 0)
        return 0;
    prev = in[in_count - 1];
    prev_inside = mvt_clip_inside (prev, edge, min, max);
    for (i = 0; i < in_count; i++)
      {
          cur = in[i];
          cur_inside = mvt_clip_inside (cur, edge, min, max);
          if (cur_inside)
            {
                if (!prev_inside)
                    out[out_count++] = mvt_clip_intersection (prev, cur, edge, min, max);
                out[out_count++] = cur;
            }
          else if (prev_inside)
              out[out_count++] = mvt_clip_intersection (prev, cur, edge, min, max);
          prev = cur;
          prev_inside = cur_inside;
      }
    return out_count;
}

static int
mvt_clip_polygon (struct mvt_pt **pts, int count, int min, int max)
{
    struct mvt_pt *tmp;
    struct mvt_pt *work;
    int cap;
    int edge;
    int out_count;

    if (count < 3)
        return 0;
    cap = count * 2 + 8;
    work = (struct mvt_pt *) malloc (sizeof (struct mvt_pt) * (size_t) cap);
    tmp = (struct mvt_pt *) malloc (sizeof (struct mvt_pt) * (size_t) cap);
    if (!work || !tmp)
      {
          free (work);
          free (tmp);
          return -1;
      }
    memcpy (work, *pts, sizeof (struct mvt_pt) * (size_t) count);
    for (edge = 0; edge < 4; edge++)
      {
          out_count = mvt_clip_polygon_edge (work, count, tmp, edge, min, max);
          if (out_count < 3)
            {
                free (work);
                free (tmp);
                return 0;
            }
          if (out_count > cap / 2)
            {
                struct mvt_pt *new_work;
                struct mvt_pt *new_tmp;
                cap = out_count * 2 + 8;
                new_work = (struct mvt_pt *) realloc (work, sizeof (struct mvt_pt) * (size_t) cap);
                new_tmp = (struct mvt_pt *) realloc (tmp, sizeof (struct mvt_pt) * (size_t) cap);
                if (!new_work || !new_tmp)
                  {
                      if (new_work)
                          work = new_work;
                      if (new_tmp)
                          tmp = new_tmp;
                      free (work);
                      free (tmp);
                      return -1;
                  }
                work = new_work;
                tmp = new_tmp;
            }
          memcpy (work, tmp, sizeof (struct mvt_pt) * (size_t) out_count);
          count = out_count;
      }
    free (*pts);
    free (tmp);
    *pts = work;
    return count;
}

static void
mvt_encode_line_points (struct mvt_buf *buf, gaiaLinestringPtr line, int extent,
                        int buffer, int *px, int *py)
{
    int i;
    int n = line->Points;
    int count = 0;
    double x;
    double y;
    double x0;
    double y0;
    double x1;
    double y1;
    double ox1;
    double oy1;
    double z;
    double m;
    double min = (double) -buffer;
    double max = (double) extent + (double) buffer;
    struct mvt_pt pts[2];

    if (n < 2)
        return;
    gaiaLineGetPoint (line, 0, &x0, &y0, &z, &m);
    for (i = 1; i < n; i++)
      {
          gaiaLineGetPoint (line, i, &x, &y, &z, &m);
          x1 = x;
          y1 = y;
          ox1 = x1;
          oy1 = y1;
          x = x0;
          y = y0;
          if (mvt_clip_segment (&x, &y, &x1, &y1, min, max))
            {
                pts[0].x = mvt_round_coord (x);
                pts[0].y = mvt_round_coord (y);
                pts[1].x = mvt_round_coord (x1);
                pts[1].y = mvt_round_coord (y1);
                if (!mvt_same_pt (pts[0], pts[1]))
                  {
                      mvt_encode_pts (buf, pts, 2, 0, px, py);
                      count++;
                  }
            }
          x0 = ox1;
          y0 = oy1;
      }
}

static void
mvt_encode_ring_points (struct mvt_buf *buf, gaiaRingPtr ring, int is_exterior,
                        int extent, int buffer, int *px, int *py)
{
    int i;
    int n = ring->Points;
    int limit = n > 0 ? n - 1 : n;
    int count;
    int clipped_count;
    double x;
    double y;
    double z;
    double m;
    double area;
    struct mvt_pt *pts;

    if (limit < 3)
        return;
    pts = (struct mvt_pt *) malloc (sizeof (struct mvt_pt) * (size_t) limit);
    if (!pts)
      {
          buf->error = 1;
          return;
      }
    for (i = 0; i < limit; i++)
      {
          gaiaRingGetPoint (ring, i, &x, &y, &z, &m);
          pts[i].x = mvt_round_coord (x);
          pts[i].y = mvt_round_coord (y);
      }
    count = mvt_clean_pts (pts, limit, 1);
    clipped_count = mvt_clip_polygon (&pts, count, -buffer, extent + buffer);
    if (clipped_count < 0)
      {
          buf->error = 1;
          free (pts);
          return;
      }
    count = clipped_count;
    count = mvt_clean_pts (pts, count, 1);
    if (count < 3)
      {
          free (pts);
          return;
      }
    area = mvt_ring_area (pts, count);
    if (area == 0.0)
      {
          free (pts);
          return;
      }
    if ((is_exterior && area > 0.0) || (!is_exterior && area < 0.0))
        mvt_reverse_pts (pts, count);
    mvt_encode_pts (buf, pts, count, 1, px, py);
    free (pts);
}

static int
mvt_encode_geometry (gaiaGeomCollPtr geom, struct mvt_buf *geometry, int extent, int buffer)
{
    gaiaPointPtr point;
    gaiaLinestringPtr line;
    gaiaPolygonPtr poly;
    int px = 0;
    int py = 0;
    int count = 0;
    int i;

    for (point = geom->FirstPoint; point; point = point->Next)
        count++;
    if (count > 0)
      {
          mvt_buf_put_varint64 (geometry, mvt_command (MVT_CMD_MOVE_TO, count));
          for (point = geom->FirstPoint; point; point = point->Next)
              mvt_encode_point_xy (geometry, mvt_round_coord (point->X),
                                   mvt_round_coord (point->Y), &px, &py);
      }
    for (line = geom->FirstLinestring; line; line = line->Next)
        mvt_encode_line_points (geometry, line, extent, buffer, &px, &py);
    for (poly = geom->FirstPolygon; poly; poly = poly->Next)
      {
          mvt_encode_ring_points (geometry, poly->Exterior, 1, extent, buffer, &px, &py);
          for (i = 0; i < poly->NumInteriors; i++)
              mvt_encode_ring_points (geometry, poly->Interiors + i, 0, extent, buffer, &px, &py);
      }
    return geometry->len > 0 && !geometry->error;
}

static void
fnct_MVTConcat_scalar (sqlite3_context * context, int argc, sqlite3_value ** argv)
{
    struct mvt_buf out;
    int i;
    const unsigned char *blob;
    int blob_size;

    mvt_buf_init (&out);
    for (i = 0; i < argc; i++)
      {
          if (sqlite3_value_type (argv[i]) != SQLITE_BLOB)
              continue;
          blob = sqlite3_value_blob (argv[i]);
          blob_size = sqlite3_value_bytes (argv[i]);
          if (blob && blob_size > 0)
              mvt_buf_put_data (&out, blob, blob_size);
      }
    if (out.error || out.len == 0)
        sqlite3_result_null (context);
    else
        sqlite3_result_blob (context, out.data, out.len, SQLITE_TRANSIENT);
    mvt_buf_free (&out);
}

static void
mvt_append_feature (struct mvt_ctx *ctx, gaiaGeomCollPtr geom, const char *properties_json,
                    int buffer,
                    unsigned long long feature_id, int has_feature_id)
{
    struct mvt_buf geometry;
    struct mvt_buf feature;
    struct mvt_buf tags;
    int type;

    type = mvt_geom_type (geom);
    if (!type)
        return;
    mvt_buf_init (&geometry);
    mvt_buf_init (&feature);
    mvt_buf_init (&tags);
    if (!mvt_encode_geometry (geom, &geometry, ctx->extent, buffer))
      {
          if (geometry.error)
              ctx->error = 1;
          mvt_buf_free (&geometry);
          mvt_buf_free (&feature);
          mvt_buf_free (&tags);
          return;
      }
    if (properties_json)
        mvt_append_json_tags (ctx, &tags, properties_json);
    if (ctx->error || tags.error)
      {
          ctx->error = 1;
          mvt_buf_free (&geometry);
          mvt_buf_free (&feature);
          mvt_buf_free (&tags);
          return;
      }
    if (tags.len > 0)
        mvt_buf_put_bytes_field (&feature, 2, tags.data, tags.len);
    if (has_feature_id)
        mvt_buf_put_varint_field (&feature, 1, feature_id);
    mvt_buf_put_varint_field (&feature, 3, (unsigned long long) type);
    mvt_buf_put_bytes_field (&feature, 4, geometry.data, geometry.len);
    if (feature.error)
        ctx->error = 1;
    else
      {
          mvt_buf_put_bytes_field (&ctx->features, 2, feature.data, feature.len);
          if (ctx->features.error)
              ctx->error = 1;
          else
              ctx->feature_count++;
      }
    mvt_buf_free (&geometry);
    mvt_buf_free (&feature);
    mvt_buf_free (&tags);
}

static void
fnct_AsMVTGeom (sqlite3_context * context, int argc, sqlite3_value ** argv)
{
    const unsigned char *blob;
    int blob_size;
    gaiaGeomCollPtr geom;
    double minx;
    double miny;
    double maxx;
    double maxy;
    int extent = 4096;
    int buffer = 256;
    int clip = 1;
    double width;
    double height;
    double fx;
    double fy;
    double buffer_map;
    unsigned char *out_blob;
    int out_size;

    if (argc < 5 || sqlite3_value_type (argv[0]) == SQLITE_NULL)
      {
          sqlite3_result_null (context);
          return;
      }
    if (sqlite3_value_type (argv[0]) != SQLITE_BLOB)
      {
          sqlite3_result_null (context);
          return;
      }
    minx = sqlite3_value_double (argv[1]);
    miny = sqlite3_value_double (argv[2]);
    maxx = sqlite3_value_double (argv[3]);
    maxy = sqlite3_value_double (argv[4]);
    if (argc > 5 && sqlite3_value_type (argv[5]) != SQLITE_NULL)
        extent = sqlite3_value_int (argv[5]);
    if (argc > 6 && sqlite3_value_type (argv[6]) != SQLITE_NULL)
        buffer = sqlite3_value_int (argv[6]);
    if (argc > 7 && sqlite3_value_type (argv[7]) != SQLITE_NULL)
        clip = sqlite3_value_int (argv[7]);
    width = maxx - minx;
    height = maxy - miny;
    if (width <= 0.0 || height <= 0.0 || extent <= 0 || buffer < 0)
      {
          sqlite3_result_null (context);
          return;
      }

    blob = sqlite3_value_blob (argv[0]);
    blob_size = sqlite3_value_bytes (argv[0]);
    geom = gaiaFromSpatiaLiteBlobWkb (blob, blob_size);
    if (!geom)
      {
          sqlite3_result_null (context);
          return;
      }
    gaiaMbrGeometry (geom);
    buffer_map = ((double) buffer / (double) extent) * width;
    if (clip && !mvt_bounds_intersects (geom, minx, miny, maxx, maxy, buffer_map))
      {
          gaiaFreeGeomColl (geom);
          sqlite3_result_null (context);
          return;
      }
    fx = (double) extent / width;
    fy = (double) extent / height;
    if (!mvt_bounds_large_enough (geom, fx, fy))
      {
          gaiaFreeGeomColl (geom);
          sqlite3_result_null (context);
          return;
      }
    mvt_transform_geom (geom, minx, maxy, fx, fy);
    gaiaToSpatiaLiteBlobWkb (geom, &out_blob, &out_size);
    gaiaFreeGeomColl (geom);
    if (!out_blob || out_size <= 0)
      {
          sqlite3_result_null (context);
          return;
      }
    sqlite3_result_blob (context, out_blob, out_size, free);
}

static void
fnct_AsMVT_step (sqlite3_context * context, int argc, sqlite3_value ** argv)
{
    struct mvt_ctx *ctx;
    const unsigned char *blob;
    int blob_size;
    gaiaGeomCollPtr geom;
    const char *name;
    const char *properties_json = NULL;
    unsigned long long feature_id = 0;
    int has_feature_id = 0;
    int buffer = 256;

    ctx = (struct mvt_ctx *) sqlite3_aggregate_context (context, sizeof (struct mvt_ctx));
    if (!ctx)
        return;
    if (!ctx->features.data && !ctx->layer_name)
      {
          mvt_buf_init (&ctx->features);
          ctx->extent = 4096;
          ctx->feature_count = 0;
          ctx->keys = NULL;
          ctx->key_count = 0;
          ctx->key_cap = 0;
          ctx->values = NULL;
          ctx->value_count = 0;
          ctx->value_cap = 0;
          ctx->error = 0;
          ctx->layer_name = (char *) malloc (8);
          if (ctx->layer_name)
              strcpy (ctx->layer_name, "default");
          else
              ctx->error = 1;
      }
    if (ctx->error || argc < 1 || sqlite3_value_type (argv[0]) == SQLITE_NULL)
        return;
    if (argc > 1 && sqlite3_value_type (argv[1]) == SQLITE_TEXT && ctx->feature_count == 0)
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
    if (argc > 2 && sqlite3_value_type (argv[2]) != SQLITE_NULL && ctx->feature_count == 0)
        ctx->extent = sqlite3_value_int (argv[2]);
    if (argc > 3 && sqlite3_value_type (argv[3]) == SQLITE_TEXT)
        properties_json = (const char *) sqlite3_value_text (argv[3]);
    if (argc > 4 && sqlite3_value_type (argv[4]) != SQLITE_NULL)
      {
          sqlite3_int64 raw_id = sqlite3_value_int64 (argv[4]);
          if (raw_id >= 0)
            {
                feature_id = (unsigned long long) raw_id;
                has_feature_id = 1;
            }
      }
    if (argc > 5 && sqlite3_value_type (argv[5]) != SQLITE_NULL)
        buffer = sqlite3_value_int (argv[5]);
    if (buffer < 0)
        buffer = 0;
    if (sqlite3_value_type (argv[0]) != SQLITE_BLOB)
        return;
    blob = sqlite3_value_blob (argv[0]);
    blob_size = sqlite3_value_bytes (argv[0]);
    geom = gaiaFromSpatiaLiteBlobWkb (blob, blob_size);
    if (!geom)
        return;
    mvt_append_feature (ctx, geom, properties_json, buffer, feature_id, has_feature_id);
    gaiaFreeGeomColl (geom);
}

static void
fnct_AsMVT_final (sqlite3_context * context)
{
    struct mvt_ctx *ctx;
    struct mvt_buf layer;
    struct mvt_buf tile;
    int i;

    ctx = (struct mvt_ctx *) sqlite3_aggregate_context (context, 0);
    if (!ctx || ctx->error)
      {
          if (ctx)
              mvt_ctx_free (ctx);
          sqlite3_result_null (context);
          return;
      }
    mvt_buf_init (&layer);
    mvt_buf_init (&tile);
    mvt_buf_put_bytes_field (&layer, 1, (const unsigned char *) ctx->layer_name,
                             (int) strlen (ctx->layer_name));
    mvt_buf_put_data (&layer, ctx->features.data, ctx->features.len);
    for (i = 0; i < ctx->key_count; i++)
        mvt_buf_put_bytes_field (&layer, 3, (const unsigned char *) ctx->keys[i].text,
                                 (int) strlen (ctx->keys[i].text));
    for (i = 0; i < ctx->value_count; i++)
        mvt_buf_put_bytes_field (&layer, 4, ctx->values[i].encoded.data,
                                 ctx->values[i].encoded.len);
    mvt_buf_put_varint_field (&layer, 5, (unsigned long long) ctx->extent);
    mvt_buf_put_varint_field (&layer, 15, 2);
    mvt_buf_put_bytes_field (&tile, 3, layer.data, layer.len);
    if (layer.error || tile.error)
        sqlite3_result_null (context);
    else
        sqlite3_result_blob (context, tile.data, tile.len, SQLITE_TRANSIENT);
    mvt_buf_free (&layer);
    mvt_buf_free (&tile);
    mvt_ctx_free (ctx);
}

void
register_spatialite_mvt_sql_functions (sqlite3 *db)
{
    sqlite3_create_function_v2 (db, "AsMVTGeom", 5,
                                SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0,
                                fnct_AsMVTGeom, 0, 0, 0);
    sqlite3_create_function_v2 (db, "AsMVTGeom", 6,
                                SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0,
                                fnct_AsMVTGeom, 0, 0, 0);
    sqlite3_create_function_v2 (db, "AsMVTGeom", 7,
                                SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0,
                                fnct_AsMVTGeom, 0, 0, 0);
    sqlite3_create_function_v2 (db, "AsMVTGeom", 8,
                                SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0,
                                fnct_AsMVTGeom, 0, 0, 0);
    sqlite3_create_function_v2 (db, "AsMVT", -1, SQLITE_UTF8, 0,
                                0, fnct_AsMVT_step, fnct_AsMVT_final, 0);
    sqlite3_create_function_v2 (db, "MVTConcat", -1, SQLITE_UTF8, 0,
                                fnct_MVTConcat_scalar, 0, 0, 0);
}
