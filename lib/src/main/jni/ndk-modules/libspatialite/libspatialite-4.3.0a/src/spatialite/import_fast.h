/*
 import_fast.h -- Public API for high-performance batch feature import
*/

#ifndef IMPORT_FAST_H
#define IMPORT_FAST_H

#include <spatialite/sqlite.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * import_fast_begin - Start a batch import session.
 *
 * Parameters:
 *   db               - Open sqlite3 handle (with SpatiaLite loaded)
 *   table_name       - Target feature table name
 *   has_properties   - 1 if table has INLINE properties column, 0 otherwise
 *   geometry_srid    - 3857 (already projected) or 4326 (needs Transform)
 *   transaction_batch - Rows per transaction commit (0 = default 5000)
 *
 * Returns opaque handle, or NULL on error.
 */
void *import_fast_begin(sqlite3 *db, const char *table_name,
                        int has_properties, int geometry_srid,
                        int transaction_batch);

/*
 * import_fast_insert - Insert one feature (INLINE or NONE properties mode).
 * Returns 0 on success.
 */
int import_fast_insert(void *handle,
                       const char *name, int name_len,
                       const char *properties, int properties_len,
                       double mbr_width, double mbr_height,
                       const unsigned char *wkb, int wkb_len);

/*
 * import_fast_insert_with_id - Insert one feature and return its rowid.
 * Used for SEPARATE properties mode.
 * Returns rowid on success, -1 on error.
 */
long long import_fast_insert_with_id(void *handle,
                                     const char *name, int name_len,
                                     double mbr_width, double mbr_height,
                                     const unsigned char *wkb, int wkb_len);

/*
 * import_fast_end - Finalize batch import, commit remaining rows.
 * Returns total rows inserted, or -1 on error.
 * Frees the handle.
 */
int import_fast_end(void *handle);

/*
 * import_fast_get_error - Get error message (before calling end).
 */
const char *import_fast_get_error(void *handle);

/*
 * import_fast_get_count - Get current inserted count.
 */
int import_fast_get_count(void *handle);

/*
 * Properties table helpers (for SEPARATE mode).
 */
void *import_fast_props_begin(sqlite3 *db, const char *props_table);
int import_fast_props_insert(void *handle, long long feature_id,
                             const char *properties, int properties_len);
void import_fast_props_end(void *handle);

#ifdef __cplusplus
}
#endif

#endif /* IMPORT_FAST_H */