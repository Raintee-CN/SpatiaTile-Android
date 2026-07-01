/*
 mvt_fast.h -- Public API for high-performance MVT tile generation
*/

#ifndef MVT_FAST_H
#define MVT_FAST_H

#include <spatialite/sqlite.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Register optimized SQL functions:
 *   AsMVTFast(geom, layer, extent, minx, miny, maxx, maxy, props, id, buffer)
 *   AsMVT2(geom, layer, extent, props, id, buffer)  -- drop-in for AsMVT
 *   AsMVTGeom2(geom, minx, miny, maxx, maxy [, extent, buffer, clip])
 */
void register_spatialite_mvt_fast_sql_functions (sqlite3 *db);

/*
 * Direct tile generation API (Plan B).
 * Bypasses SQL aggregate, queries + encodes in one C call.
 *
 * Parameters:
 *   db                  - Open sqlite3 handle (with SpatiaLite loaded)
 *   table_name          - Source table name
 *   layer_name          - MVT layer name (e.g. "features")
 *   z, x, y             - Tile coordinates (Web Mercator / TMS)
 *   properties_column   - SQL expression for properties (e.g. "f.properties")
 *                         or NULL for no properties
 *   properties_join_sql - JOIN clause for properties table, or NULL
 *   extent              - Tile extent (default 4096)
 *   buffer              - Tile buffer in tile units (default 64)
 *   has_spatial_index   - 1 if SpatialIndex exists for the table
 *   use_feature_id      - 1 to include f.id as MVT feature ID
 *   out_data            - Output: pointer to PBF data (caller must free())
 *   out_len             - Output: length of PBF data
 *
 * Returns 0 on success, non-zero on error.
 * If no features in tile, out_data=NULL and out_len=0 (still returns 0).
 */
int mvt_fast_generate_tile (sqlite3 *db, const char *table_name,
                            const char *layer_name, int z, int x, int y,
                            const char *properties_column,
                            const char *properties_join_sql,
                            int extent, int buffer,
                            int has_spatial_index, int use_feature_id,
                            unsigned char **out_data, int *out_len);

#ifdef __cplusplus
}
#endif

#endif /* MVT_FAST_H */