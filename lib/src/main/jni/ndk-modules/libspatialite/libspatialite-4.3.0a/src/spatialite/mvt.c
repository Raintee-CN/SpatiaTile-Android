/*
 mvt.c -- lightweight Mapbox Vector Tile helpers for android-spatialite

 This module intentionally implements a small first version: AsMVTGeom()
 transforms SpatiaLite geometries into tile coordinates, and AsMVT()
 aggregates geometry-only features into a MVT v2 PBF layer.
*/

#include <stdlib.h>
#include <string.h>
#include <math.h>

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

struct mvt_ctx
{
    char *layer_name;
    int extent;
    struct mvt_buf features;
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
mvt_buf_put_bytes_field (struct mvt_buf *buf, int field, const unsigned char *data, int len)
{
    mvt_buf_put_key (buf, field, 2);
    mvt_buf_put_varint64 (buf, (unsigned long long) len);
    mvt_buf_put_data (buf, data, len);
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

static int
mvt_round_coord (double value)
{
    if (value >= 0.0)
        return (int) (value + 0.5);
    return (int) (value - 0.5);
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

static void
mvt_encode_line_points (struct mvt_buf *buf, gaiaLinestringPtr line,
                        int close_path, int *px, int *py)
{
    int i;
    int n = line->Points;
    int limit = close_path && n > 0 ? n - 1 : n;
    double x;
    double y;
    double z;
    double m;

    if (limit <= 0)
        return;
    gaiaLineGetPoint (line, 0, &x, &y, &z, &m);
    mvt_buf_put_varint64 (buf, mvt_command (MVT_CMD_MOVE_TO, 1));
    mvt_encode_point_xy (buf, mvt_round_coord (x), mvt_round_coord (y), px, py);
    if (limit > 1)
      {
          mvt_buf_put_varint64 (buf, mvt_command (MVT_CMD_LINE_TO, limit - 1));
          for (i = 1; i < limit; i++)
            {
                gaiaLineGetPoint (line, i, &x, &y, &z, &m);
                mvt_encode_point_xy (buf, mvt_round_coord (x), mvt_round_coord (y), px, py);
            }
      }
    if (close_path)
        mvt_buf_put_varint64 (buf, mvt_command (MVT_CMD_CLOSE_PATH, 1));
}

static void
mvt_encode_ring_points (struct mvt_buf *buf, gaiaRingPtr ring, int *px, int *py)
{
    int i;
    int n = ring->Points;
    int limit = n > 0 ? n - 1 : n;
    double x;
    double y;
    double z;
    double m;

    if (limit <= 0)
        return;
    gaiaRingGetPoint (ring, 0, &x, &y, &z, &m);
    mvt_buf_put_varint64 (buf, mvt_command (MVT_CMD_MOVE_TO, 1));
    mvt_encode_point_xy (buf, mvt_round_coord (x), mvt_round_coord (y), px, py);
    if (limit > 1)
      {
          mvt_buf_put_varint64 (buf, mvt_command (MVT_CMD_LINE_TO, limit - 1));
          for (i = 1; i < limit; i++)
            {
                gaiaRingGetPoint (ring, i, &x, &y, &z, &m);
                mvt_encode_point_xy (buf, mvt_round_coord (x), mvt_round_coord (y), px, py);
            }
      }
    mvt_buf_put_varint64 (buf, mvt_command (MVT_CMD_CLOSE_PATH, 1));
}

static void
mvt_encode_geometry (gaiaGeomCollPtr geom, struct mvt_buf *geometry)
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
        mvt_encode_line_points (geometry, line, 0, &px, &py);
    for (poly = geom->FirstPolygon; poly; poly = poly->Next)
      {
          mvt_encode_ring_points (geometry, poly->Exterior, &px, &py);
          for (i = 0; i < poly->NumInteriors; i++)
              mvt_encode_ring_points (geometry, poly->Interiors + i, &px, &py);
      }
}

static void
mvt_append_feature (struct mvt_ctx *ctx, gaiaGeomCollPtr geom)
{
    struct mvt_buf geometry;
    struct mvt_buf feature;
    int type;

    type = mvt_geom_type (geom);
    if (!type)
        return;
    mvt_buf_init (&geometry);
    mvt_buf_init (&feature);
    mvt_encode_geometry (geom, &geometry);
    if (geometry.error || geometry.len == 0)
      {
          ctx->error = 1;
          mvt_buf_free (&geometry);
          mvt_buf_free (&feature);
          return;
      }
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

    ctx = (struct mvt_ctx *) sqlite3_aggregate_context (context, sizeof (struct mvt_ctx));
    if (!ctx)
        return;
    if (!ctx->features.data && !ctx->layer_name)
      {
          mvt_buf_init (&ctx->features);
          ctx->extent = 4096;
          ctx->feature_count = 0;
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
    if (sqlite3_value_type (argv[0]) != SQLITE_BLOB)
        return;
    blob = sqlite3_value_blob (argv[0]);
    blob_size = sqlite3_value_bytes (argv[0]);
    geom = gaiaFromSpatiaLiteBlobWkb (blob, blob_size);
    if (!geom)
        return;
    mvt_append_feature (ctx, geom);
    gaiaFreeGeomColl (geom);
}

static void
fnct_AsMVT_final (sqlite3_context * context)
{
    struct mvt_ctx *ctx;
    struct mvt_buf layer;
    struct mvt_buf tile;

    ctx = (struct mvt_ctx *) sqlite3_aggregate_context (context, 0);
    if (!ctx || ctx->error)
      {
          sqlite3_result_null (context);
          return;
      }
    mvt_buf_init (&layer);
    mvt_buf_init (&tile);
    mvt_buf_put_bytes_field (&layer, 1, (const unsigned char *) ctx->layer_name,
                             (int) strlen (ctx->layer_name));
    mvt_buf_put_data (&layer, ctx->features.data, ctx->features.len);
    mvt_buf_put_varint_field (&layer, 5, (unsigned long long) ctx->extent);
    mvt_buf_put_varint_field (&layer, 15, 2);
    mvt_buf_put_bytes_field (&tile, 3, layer.data, layer.len);
    if (layer.error || tile.error)
        sqlite3_result_null (context);
    else
        sqlite3_result_blob (context, tile.data, tile.len, SQLITE_TRANSIENT);
    mvt_buf_free (&layer);
    mvt_buf_free (&tile);
    mvt_buf_free (&ctx->features);
    if (ctx->layer_name)
        free (ctx->layer_name);
    ctx->layer_name = NULL;
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
}
